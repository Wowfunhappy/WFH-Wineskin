/*
 * WINESKIN: reuse of retired OpenGL buffer objects.
 *
 * Without ARB_buffer_storage, wined3d gives each buffer object an OpenGL
 * buffer of its own, and a DISCARD write to a buffer the GPU may still read
 * renames it: a new OpenGL buffer replaces the old one, which is deleted.
 * Constant and dynamic vertex buffers are discarded many times a frame, so
 * the old buffers go to a pool instead, and a buffer of the same size and
 * usage is taken from it once the GPU has finished the commands that read it
 * (its command fence has completed). A reused buffer keeps its name, so the
 * OpenCL memory object made for it (cl.c) stays valid.
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
#include "wined3d_gl.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d);

#define BO_POOL_BUCKETS 64
#define BO_POOL_MAX_BO_SIZE (8u << 20)
/* The pool holds at most an eighth of the video memory, and no more than
 * 256 MiB: a Mac with Intel graphics shares 1.5 GiB of system memory. */
#define BO_POOL_MAX_BYTES (256u << 20)
#define BO_POOL_MIN_BYTES (32u << 20)
/* How many of a bucket's oldest buffers a request looks at. */
#define BO_POOL_SCAN 8

struct wined3d_bo_pool_entry
{
    GLuint id;
    GLenum usage;
    GLsizeiptr size;
    bool coherent;
    uint64_t fence_id;
};

struct wined3d_bo_pool_bucket
{
    struct wined3d_bo_pool_entry *entries;
    SIZE_T start, count, capacity;
};

struct wined3d_bo_pool_gl
{
    struct wined3d_bo_pool_bucket buckets[BO_POOL_BUCKETS];
    size_t bytes;
    size_t max_bytes;
};

static struct wined3d_bo_pool_bucket *bo_pool_bucket(struct wined3d_bo_pool_gl *pool, GLsizeiptr size)
{
    uint64_t h = (uint64_t)size * 0x9e3779b97f4a7c15ull;

    return &pool->buckets[h >> 58];
}

/* A pooled buffer for a new buffer object, or 0. */
GLuint wined3d_device_gl_take_pooled_bo(struct wined3d_device_gl *device_gl,
        GLsizeiptr size, GLenum usage, bool coherent)
{
    struct wined3d_bo_pool_gl *pool = device_gl->bo_pool;
    struct wined3d_bo_pool_bucket *bucket;
    struct wined3d_bo_pool_entry *e;
    SIZE_T i, end;
    GLuint id;

    if (!pool)
        return 0;
    bucket = bo_pool_bucket(pool, size);
    end = bucket->start + min(bucket->count, BO_POOL_SCAN);
    for (i = bucket->start; i < end; ++i)
    {
        e = &bucket->entries[i];
        if (e->size != size || e->usage != usage || e->coherent != coherent
                || e->fence_id > device_gl->completed_fence_id)
            continue;

        id = e->id;
        *e = bucket->entries[bucket->start];
        ++bucket->start;
        if (!--bucket->count)
            bucket->start = 0;
        pool->bytes -= size;
        TRACE("Reusing buffer %u, size %Iu.\n", id, size);
        return id;
    }
    return 0;
}

/* Takes a retired buffer object's OpenGL buffer into the pool; false if the
 * caller is to delete it. The buffer is unmapped. */
bool wined3d_context_gl_pool_bo(struct wined3d_context_gl *context_gl, const struct wined3d_bo_gl *bo)
{
    struct wined3d_device_gl *device_gl = wined3d_device_gl(context_gl->c.device);
    struct wined3d_bo_pool_gl *pool = device_gl->bo_pool;
    struct wined3d_bo_pool_bucket *bucket;
    struct wined3d_bo_pool_entry *e;

    if (bo->memory || context_gl->gl_info->supported[ARB_BUFFER_STORAGE]
            || bo->usage != GL_STREAM_DRAW || bo->size > BO_POOL_MAX_BO_SIZE)
        return false;

    if (!pool && !(pool = device_gl->bo_pool = calloc(1, sizeof(*pool))))
        return false;
    if (!pool->max_bytes)
    {
        UINT64 vram = context_gl->c.device->adapter->driver_info.vram_bytes;

        pool->max_bytes = vram ? min(max(vram / 8, BO_POOL_MIN_BYTES), BO_POOL_MAX_BYTES) : BO_POOL_MAX_BYTES;
        TRACE("Pooling up to %Iu MiB of buffers.\n", pool->max_bytes >> 20);
    }
    if (pool->bytes + bo->size > pool->max_bytes)
        return false;

    bucket = bo_pool_bucket(pool, bo->size);
    if (bucket->start && bucket->start + bucket->count == bucket->capacity)
    {
        memmove(bucket->entries, &bucket->entries[bucket->start], bucket->count * sizeof(*bucket->entries));
        bucket->start = 0;
    }
    if (!wined3d_array_reserve((void **)&bucket->entries, &bucket->capacity,
            bucket->start + bucket->count + 1, sizeof(*bucket->entries)))
        return false;

    e = &bucket->entries[bucket->start + bucket->count++];
    e->id = bo->id;
    e->usage = bo->usage;
    e->size = bo->size;
    e->coherent = bo->b.coherent;
    e->fence_id = bo->command_fence_id;
    pool->bytes += bo->size;
    return true;
}

/* Deletes the pooled buffers; the device's OpenGL work is finished. */
void wined3d_device_gl_destroy_bo_pool(struct wined3d_device_gl *device_gl, struct wined3d_context_gl *context_gl)
{
    struct wined3d_bo_pool_gl *pool = device_gl->bo_pool;
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_bo_pool_bucket *bucket;
    unsigned int b;
    SIZE_T i;

    if (!pool)
        return;

    wined3d_context_gl_unbind_buffer_textures(context_gl);
    for (b = 0; b < BO_POOL_BUCKETS; ++b)
    {
        bucket = &pool->buckets[b];
        for (i = bucket->start; i < bucket->start + bucket->count; ++i)
        {
            wined3d_cl_forget_gl_object(false, bucket->entries[i].id);
            GL_EXTCALL(glDeleteBuffers(1, &bucket->entries[i].id));
        }
        free(bucket->entries);
    }
    checkGLcall("delete pooled buffers");
    free(pool);
    device_gl->bo_pool = NULL;
}
