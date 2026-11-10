/*
 * WINESKIN: client-side map shadows for dynamic buffers.
 *
 * Without persistent buffer mapping (ARB_buffer_storage, which macOS's OpenGL
 * lacks), the client thread can't map an OpenGL buffer, so a DISCARD or
 * NOOVERWRITE map of a dynamic buffer would have to wait for the command
 * stream to reach the buffer's last use and map it there. Instead, such a map
 * returns system memory, the buffer's shadow, and the unmap queues the
 * shadow's contents for the command stream thread to write into the buffer in
 * order with the draws around it:
 *
 *  - A DISCARD map gives the buffer a new shadow, and the command stream
 *    writes the whole buffer from it.
 *  - A NOOVERWRITE map returns the buffer's current shadow, which holds every
 *    byte written since the last DISCARD, and the command stream writes the
 *    mapped range. The application promises not to touch bytes the GPU may
 *    still read, so the bytes rewritten under the GPU are the ones it already
 *    holds.
 *
 * Direct3D 10 and later map a whole buffer, so the bytes a map wrote are not
 * known. The shadow of a buffer of at most 64 KiB (a constant buffer,
 * typically) is ordinary memory and is written whole. The shadow of a larger
 * buffer (an engine's vertex or index scratch buffer of megabytes, to which
 * each map appends a few kilobytes) is write-watched memory (VirtualAlloc()
 * with MEM_WRITE_WATCH): the unmap asks which pages were written since the
 * previous one and queues those, whatever the map's kind. A page's first write
 * after an unmap costs a fault, about 7 microseconds; a map through the
 * command stream costs the wait for the draws queued before it.
 *
 * A shadow is reference counted: the buffer holds one, and each queued upload
 * holds one until the command stream has copied it. Anything else that writes
 * the buffer (an update, a copy, a map of another kind) drops the buffer's
 * shadow, and the next NOOVERWRITE map then goes through the command stream.
 * Freed shadows return to per-size pools.
 *
 * HKCU\Software\Wine\Direct3D\MapShadows = "n" (or WINE_D3D_CONFIG
 * "MapShadows=n") turns shadows off, "small" the write-watched ones.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "wined3d_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d);

/* Shadows come from pools of power-of-two sizes, 256 bytes and up. Those of
 * more than 64 KiB (constant buffers are at most that) are write-watched; a
 * buffer larger than the largest shadow maps through the command stream. */
#define SHADOW_MIN_ORDER 8
#define SHADOW_MAX_PLAIN_ORDER 16
#define SHADOW_MAX_ORDER 24
#define SHADOW_POOL_BYTES (4u << 20)
#define SHADOW_PAGE_SIZE 0x1000u

struct wined3d_map_shadow
{
    SLIST_ENTRY entry;
    LONG refcount;
    unsigned int order;
    /* The size of a write-watched shadow's data, which starts on a page of its
     * own; 0 for a shadow in ordinary memory. */
    size_t watched_size;
    DECLSPEC_ALIGN(64) BYTE data[1];
};

static SLIST_HEADER shadow_pools[SHADOW_MAX_ORDER + 1];
static LONG shadow_pool_counts[SHADOW_MAX_ORDER + 1];

static unsigned int shadow_order(size_t size)
{
    unsigned int order = SHADOW_MIN_ORDER;

    while (order <= SHADOW_MAX_ORDER && ((size_t)1 << order) < size)
        ++order;
    return order;
}

static struct wined3d_map_shadow *wined3d_map_shadow_create(size_t size)
{
    unsigned int order = shadow_order(size);
    struct wined3d_map_shadow *shadow;
    SLIST_ENTRY *entry;

    if (order <= SHADOW_MAX_ORDER)
    {
        if ((entry = InterlockedPopEntrySList(&shadow_pools[order])))
        {
            InterlockedDecrement(&shadow_pool_counts[order]);
            shadow = CONTAINING_RECORD(entry, struct wined3d_map_shadow, entry);
            shadow->refcount = 1;
            if (shadow->watched_size)
                ResetWriteWatch(shadow->data, shadow->watched_size);
            return shadow;
        }
        size = (size_t)1 << order;
    }

    if (order > SHADOW_MAX_PLAIN_ORDER)
    {
        /* The header ends the first page, the data starts the second. */
        BYTE *base = VirtualAlloc(NULL, SHADOW_PAGE_SIZE + size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                PAGE_READWRITE);

        if (!base)
            return NULL;
        shadow = (struct wined3d_map_shadow *)(base + SHADOW_PAGE_SIZE - offsetof(struct wined3d_map_shadow, data));
        shadow->refcount = 1;
        shadow->order = order;
        shadow->watched_size = size;
        return shadow;
    }

    if (!(shadow = _aligned_malloc(offsetof(struct wined3d_map_shadow, data[size]), 64)))
        return NULL;
    shadow->refcount = 1;
    shadow->order = order;
    shadow->watched_size = 0;
    return shadow;
}

static void wined3d_map_shadow_release(struct wined3d_map_shadow *shadow)
{
    unsigned int order = shadow->order;

    if (InterlockedDecrement(&shadow->refcount))
        return;

    if (order <= SHADOW_MAX_ORDER
            && InterlockedIncrement(&shadow_pool_counts[order]) <= max(2, SHADOW_POOL_BYTES >> order))
    {
        InterlockedPushEntrySList(&shadow_pools[order], &shadow->entry);
        return;
    }
    if (order <= SHADOW_MAX_ORDER)
        InterlockedDecrement(&shadow_pool_counts[order]);
    if (shadow->watched_size)
        VirtualFree(shadow->data - SHADOW_PAGE_SIZE, 0, MEM_RELEASE);
    else
        _aligned_free(shadow);
}

static struct wined3d_map_shadow *wined3d_map_shadow_from_data(const void *data)
{
    return CONTAINING_RECORD(data, struct wined3d_map_shadow, data);
}

/* Whether DISCARD and NOOVERWRITE maps of the resource use a shadow: dynamic
 * buffers the GPU only reads, on an adapter that can't map buffers from the
 * client thread. */
bool wined3d_resource_uses_map_shadow(const struct wined3d_resource *resource)
{
    return resource->type == WINED3D_RTYPE_BUFFER
            && resource->size <= ((size_t)1 << (wined3d_settings.map_shadow_watch
            ? SHADOW_MAX_ORDER : SHADOW_MAX_PLAIN_ORDER))
            && wined3d_settings.map_shadows
            && !resource->device->adapter->d3d_info.persistent_map
            && (resource->usage & WINED3DUSAGE_DYNAMIC)
            && !(resource->bind_flags & (WINED3D_BIND_UNORDERED_ACCESS | WINED3D_BIND_STREAM_OUTPUT));
}

/* The client thread's DISCARD or NOOVERWRITE map. Fails for a NOOVERWRITE map
 * of a buffer without a shadow, which then maps through the command stream. */
bool wined3d_client_map_shadow(struct wined3d_resource *resource, struct wined3d_map_desc *map_desc,
        const struct wined3d_box *box, uint32_t flags)
{
    struct wined3d_client_resource *client = &resource->client;
    struct wined3d_map_shadow *shadow;

    if (flags & WINED3D_MAP_DISCARD)
    {
        if (!(shadow = wined3d_map_shadow_create(resource->size)))
            return false;
        wined3d_client_drop_map_shadow(resource);
        client->shadow = shadow;
        /* The buffer has contents from here on: its NOOVERWRITE maps stay NOOVERWRITE
         * (see CLIENT_BO_DISCARDED in wined3d_cs_map_upload_bo()). */
        memset(&client->addr, 0, sizeof(client->addr));
    }
    else if (!(shadow = client->shadow))
    {
        TRACE("NOOVERWRITE map of buffer %p without a shadow.\n", resource);
        return false;
    }

    /* The reference the upload at unmap takes over. */
    InterlockedIncrement(&shadow->refcount);

    map_desc->row_pitch = map_desc->slice_pitch = resource->size;
    map_desc->data = shadow->data + box->left;
    client->mapped_upload.addr.buffer_object = NULL;
    client->mapped_upload.addr.addr = shadow->data;
    client->mapped_upload.flags = UPLOAD_BO_UPLOAD_ON_UNMAP | UPLOAD_BO_SHADOW;
    if (flags & WINED3D_MAP_DISCARD)
        client->mapped_upload.flags |= UPLOAD_BO_SHADOW_DISCARD;
    client->mapped_box = *box;
    return true;
}

/* The buffer's contents change other than through its shadow. */
void wined3d_client_drop_map_shadow(struct wined3d_resource *resource)
{
    struct wined3d_client_resource *client = &resource->client;

    if (client->shadow)
    {
        wined3d_map_shadow_release(client->shadow);
        client->shadow = NULL;
    }
}

/* A draw reads the mapped buffer before its unmap (d3d9): the upload queued
 * now takes its own reference, and the later one writes without discarding. */
void wined3d_client_flush_map_shadow(struct upload_bo *upload_bo)
{
    InterlockedIncrement(&wined3d_map_shadow_from_data(upload_bo->addr.addr)->refcount);
}

/* Whether a mapped shadow is write-watched: its unmap then queues the ranges
 * wined3d_client_shadow_dirty_ranges() returns in place of the mapped box. */
bool wined3d_upload_bo_shadow_is_watched(const struct upload_bo *upload_bo)
{
    return !!wined3d_map_shadow_from_data(upload_bo->addr.addr)->watched_size;
}

/* The ranges of the buffer whose pages in a write-watched shadow were written
 * since they were last returned here, at most "max_count" of them; "more" is
 * set when another call has more. The pages count as unwritten from here on,
 * and each range holds a reference for its upload to release. */
unsigned int wined3d_client_shadow_dirty_ranges(const struct wined3d_resource *resource,
        const struct upload_bo *upload_bo, struct wined3d_range *ranges, unsigned int max_count, bool *more)
{
    struct wined3d_map_shadow *shadow = wined3d_map_shadow_from_data(upload_bo->addr.addr);
    ULONG_PTR count, first, next;
    unsigned int range_count = 0;
    void *pages[64];
    DWORD page_size;
    size_t offset;

    *more = false;
    count = ARRAY_SIZE(pages);
    if (GetWriteWatch(0, shadow->data, shadow->watched_size, pages, &count, &page_size))
    {
        ERR("Failed to get the written pages of shadow %p, error %lu.\n", shadow, GetLastError());
        return 0;
    }
    if (count == ARRAY_SIZE(pages))
        *more = true;

    for (first = 0; first < count; first = next)
    {
        for (next = first + 1; next < count; ++next)
        {
            if ((BYTE *)pages[next] != (BYTE *)pages[next - 1] + page_size)
                break;
        }
        if (range_count == max_count)
        {
            *more = true;
            break;
        }
        ResetWriteWatch(pages[first], (next - first) * page_size);
        if ((offset = (BYTE *)pages[first] - shadow->data) >= resource->size)
            continue;
        ranges[range_count].offset = offset;
        ranges[range_count].size = min((next - first) * page_size, resource->size - offset);
        ++range_count;
        InterlockedIncrement(&shadow->refcount);
    }
    return range_count;
}

/* Releases the reference a map took for its upload; the uploads of a
 * write-watched shadow hold their own. */
void wined3d_client_end_watched_map(const struct upload_bo *upload_bo)
{
    wined3d_map_shadow_release(wined3d_map_shadow_from_data(upload_bo->addr.addr));
}

/* The command stream's half: write a mapped range from the shadow into the
 * buffer, then release the upload's reference. A DISCARD map of a shadow in
 * ordinary memory writes the whole buffer, whatever range it named, as
 * wined3d's other DISCARD maps do, and an empty range is the whole buffer too;
 * a write-watched shadow writes the range it is given.
 *
 * The write is a plain buffer update (glBufferSubData() on OpenGL) of the
 * buffer object in place: macOS's OpenGL copies such an update into its command
 * stream, which costs a fraction of mapping the buffer, and far less than
 * replacing a buffer object the GPU still reads with a new one. */
void wined3d_buffer_upload_map_shadow(struct wined3d_buffer *buffer,
        const struct wined3d_box *box, const struct upload_bo *upload_bo)
{
    bool discard = upload_bo->flags & UPLOAD_BO_SHADOW_DISCARD;
    struct wined3d_resource *resource = &buffer->resource;
    const BYTE *data = upload_bo->addr.addr;
    struct wined3d_bo_address dst, src;
    struct wined3d_context *context;
    struct wined3d_range range;
    unsigned int offset, size;
    uint32_t location;

    offset = box->left;
    size = box->right - box->left;
    if (!wined3d_map_shadow_from_data(data)->watched_size && (discard || !size))
    {
        offset = 0;
        size = resource->size;
    }
    wined3d_cs_stat_upload_bytes += size;
    TRACE("Buffer %p, size %u, bind flags %#x: writing %u bytes at %u%s.\n", buffer,
            resource->size, resource->bind_flags, size, offset, discard ? ", discard" : "");

    context = context_acquire(resource->device, NULL, 0);

    /* Fails for a buffer that has no buffer object; the write then goes to its system memory. */
    if (discard && wined3d_buffer_prepare_location(buffer, context, WINED3D_LOCATION_BUFFER))
    {
        wined3d_buffer_validate_location(buffer, WINED3D_LOCATION_BUFFER);
        wined3d_buffer_invalidate_location(buffer, ~WINED3D_LOCATION_BUFFER);
    }
    wined3d_buffer_acquire_bo_for_write(buffer, context);

    location = wined3d_buffer_get_memory(buffer, context, &dst);
    dst.addr += offset;
    src.buffer_object = NULL;
    src.addr = (BYTE *)data + offset;
    range.offset = 0;
    range.size = size;
    wined3d_context_copy_bo_address(context, &dst, &src, 1, &range, WINED3D_MAP_WRITE);
    wined3d_buffer_invalidate_location(buffer, ~location);

    context_release(context);

    wined3d_map_shadow_release(wined3d_map_shadow_from_data(data));
}
