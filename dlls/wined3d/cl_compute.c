/*
 * WINESKIN: compute shader dispatches on OpenCL (macOS).
 *
 * macOS's OpenGL 4.1 has no compute shaders. Where the OpenGL implementation
 * lacks ARB_compute_shader, a dispatch runs the compute shader as an OpenCL
 * kernel (glsl_cl.h translates it) on the OpenCL context that shares the
 * OpenGL context's objects (cl.c, the Unix side). This file loads the bound
 * resources into their OpenGL locations, describes each kernel argument for
 * the Unix side, and runs the dispatch.
 *
 * HKCU\Software\Wine\Direct3D\OpenCLCompute = "n" (or WINE_D3D_CONFIG
 * "OpenCLCompute=n") turns it off; dispatches then do nothing.
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

WINE_DEFAULT_DEBUG_CHANNEL(d3d_cl);
WINE_DECLARE_DEBUG_CHANNEL(d3d);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

static LONG cl_state; /* 0: not initialised, 1: available, -1: unavailable */
static struct wined3d_cl_init_params cl_caps;
static bool cl_used;

bool wined3d_cl_available(struct wined3d_context_gl *context_gl)
{
    NTSTATUS status;

    if (cl_state)
        return cl_state > 0;

    if (!wined3d_settings.opencl_compute)
    {
        TRACE("OpenCL compute is disabled.\n");
        cl_state = -1;
        return false;
    }

    if ((status = __wine_init_unix_call()) || (status = WINE_UNIX_CALL(unix_cl_init, &cl_caps)))
    {
        ERR_(winediag)("Compute shaders need OpenCL with OpenGL sharing (status %#lx); dispatches do nothing.\n",
                status);
        cl_state = -1;
        return false;
    }
    TRACE("OpenCL compute: work group size %u, local memory %u, caps %#x.\n",
            cl_caps.max_work_group_size, cl_caps.local_mem_size, cl_caps.caps);
    cl_state = 1;
    return true;
}

bool wined3d_cl_build_kernel(const char *source, const unsigned int *group_size, uint64_t *handle)
{
    struct wined3d_cl_build_params params;
    NTSTATUS status;

    memset(&params, 0, sizeof(params));
    params.source = (ULONG_PTR)source;
    params.group_size[0] = group_size[0];
    params.group_size[1] = group_size[1];
    params.group_size[2] = group_size[2];
    if ((status = WINE_UNIX_CALL(unix_cl_build, &params)))
    {
        ERR("Failed to build an OpenCL kernel, status %#lx.\n", status);
        return false;
    }
    if (params.work_group_size < group_size[0] * group_size[1] * group_size[2])
    {
        FIXME("Thread group %ux%ux%u exceeds the kernel's work group size %u.\n",
                group_size[0], group_size[1], group_size[2], params.work_group_size);
        wined3d_cl_release_kernel(params.kernel);
        return false;
    }
    *handle = params.kernel;
    return true;
}

void wined3d_cl_release_kernel(uint64_t handle)
{
    struct wined3d_cl_release_kernel_params params = {.kernel = handle};

    if (handle)
        WINE_UNIX_CALL(unix_cl_release_kernel, &params);
}

void wined3d_cl_finish(void)
{
    if (cl_used)
        WINE_UNIX_CALL(unix_cl_finish, NULL);
}

/* The element format of a typed buffer view (WINED3D_CL_FORMAT_*). */
static uint32_t cl_buffer_format(const struct wined3d_format *format)
{
    uint32_t code;

    if (format->red_size == 10 && format->green_size == 10 && format->blue_size == 10 && format->alpha_size == 2)
        return WINED3D_CL_FORMAT_PACKED | 0
                | ((format->attrs & WINED3D_FORMAT_ATTR_INTEGER) ? WINED3D_CL_FORMAT_UINT : WINED3D_CL_FORMAT_UNORM);
    if (format->red_size == 11 && format->green_size == 11 && format->blue_size == 10)
        return WINED3D_CL_FORMAT_PACKED | 1 | WINED3D_CL_FORMAT_FLOAT;

    code = format->component_count ? format->component_count - 1 : 0;
    switch (format->byte_count / max(format->component_count, 1))
    {
        case 1: code |= WINED3D_CL_FORMAT_8; break;
        case 2: code |= WINED3D_CL_FORMAT_16; break;
        case 4: code |= WINED3D_CL_FORMAT_32; break;
        default:
            FIXME("Unhandled typed buffer format %s.\n", debug_d3dformat(format->id));
            code |= WINED3D_CL_FORMAT_32;
            break;
    }
    if (format->attrs & WINED3D_FORMAT_ATTR_FLOAT)
        code |= WINED3D_CL_FORMAT_FLOAT;
    else if (format->attrs & WINED3D_FORMAT_ATTR_INTEGER)
        code |= (format->attrs & WINED3D_FORMAT_ATTR_UNSIGNED) ? WINED3D_CL_FORMAT_UINT : WINED3D_CL_FORMAT_SINT;
    else if (format->attrs & WINED3D_FORMAT_ATTR_NORMALISED)
        code |= (format->attrs & WINED3D_FORMAT_ATTR_UNSIGNED) ? WINED3D_CL_FORMAT_UNORM : WINED3D_CL_FORMAT_SNORM;
    else
        code |= WINED3D_CL_FORMAT_UINT;
    if (format->component_count >= 3 && format->blue_offset < format->red_offset)
        code |= WINED3D_CL_FORMAT_BGRA;
    return code;
}

/* The information argument of a buffer view (glsl_cl.h). */
static void cl_buffer_view_info(const struct wined3d_buffer *buffer, const struct wined3d_view_desc *desc,
        const struct wined3d_format *format, uint32_t *data)
{
    unsigned int stride, offset;

    stride = desc->format_id == WINED3DFMT_UNKNOWN ? buffer->structure_byte_stride : format->byte_count;
    offset = min(desc->u.buffer.start_idx * stride, buffer->resource.size);
    data[0] = buffer->buffer_object->buffer_offset + offset;
    data[1] = min(desc->u.buffer.count * stride, buffer->resource.size - offset);
    data[2] = stride ? data[1] / stride : 0;
    data[3] = cl_buffer_format(format);
}

static enum wined3d_cl_channel cl_texture_channel(const struct wined3d_texture *texture)
{
    const struct wined3d_format_gl *format_gl = wined3d_format_gl(texture->resource.format);

    if (texture->resource.format->depth_size)
        return WINED3D_CL_CHANNEL_DEPTH;
    switch (format_gl->format)
    {
        case GL_RED_INTEGER:
        case GL_RG_INTEGER:
        case GL_RGB_INTEGER:
        case GL_RGBA_INTEGER:
        case GL_BGRA_INTEGER:
            if (format_gl->type == GL_BYTE || format_gl->type == GL_SHORT || format_gl->type == GL_INT)
                return WINED3D_CL_CHANNEL_INT;
            return WINED3D_CL_CHANNEL_UINT;
        default:
            return WINED3D_CL_CHANNEL_FLOAT;
    }
}

/* Whether OpenCL makes images of OpenGL textures of an internal format. */
static bool cl_gl_format_shareable(GLenum internal)
{
    switch (internal)
    {
        case GL_R8: case GL_RG8: case GL_RGB8: case GL_RGBA8:
        case GL_R16: case GL_RG16: case GL_RGBA16:
        case GL_R16F: case GL_RG16F: case GL_RGB16F: case GL_RGBA16F:
        case GL_R32F: case GL_RG32F: case GL_RGB32F: case GL_RGBA32F:
        case GL_R8UI: case GL_RG8UI: case GL_RGBA8UI: case GL_R8I: case GL_RG8I: case GL_RGBA8I:
        case GL_R16UI: case GL_RG16UI: case GL_RGBA16UI: case GL_R16I: case GL_RG16I: case GL_RGBA16I:
        case GL_R32UI: case GL_RG32UI: case GL_RGBA32UI: case GL_R32I: case GL_RG32I: case GL_RGBA32I:
        case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT32F:
        case GL_DEPTH24_STENCIL8: case GL_DEPTH32F_STENCIL8:
            return true;
        default:
            return false;
    }
}

/* A texture level of a format OpenCL can't share has a shadow texture of one
 * it can: the level is copied to it (through a pixel buffer) before a
 * dispatch, and back after one that writes it. */
struct cl_shadow
{
    struct list entry;
    GLuint source, name, pbo;
    GLenum source_target, image_target, target;
    unsigned int level, width, height, depth;
    GLenum internal, format, type, upload_format;
    unsigned int texel_size;
    bool blit; /* copied with glBlitFramebuffer, on the GPU */
    struct wined3d_device *device; /* whose share group "source" and "name" are in */
};

/* Each device's CS thread reaches its own shadows only; the lock keeps the
 * list whole. It is recursive: destroying a shadow forgets its texture. */
static struct list cl_shadows = LIST_INIT(cl_shadows);
static CRITICAL_SECTION cl_shadows_cs;
static CRITICAL_SECTION_DEBUG cl_shadows_cs_debug =
{
    0, 0, &cl_shadows_cs,
    {&cl_shadows_cs_debug.ProcessLocksList, &cl_shadows_cs_debug.ProcessLocksList},
    0, 0, {(DWORD_PTR)(__FILE__ ": cl_shadows_cs")}
};
static CRITICAL_SECTION cl_shadows_cs = {&cl_shadows_cs_debug, -1, 0, 0, 0, 0};

/* A device's kernels that clear unordered access views, by image kind. A
 * kernel belongs to the OpenCL context of the device that built it. */
struct cl_clear_kernels
{
    struct list entry;
    struct wined3d_device *device;
    uint64_t kernels[256];
    bool failed[256];
};

static struct list cl_clear_kernel_list = LIST_INIT(cl_clear_kernel_list);

static struct cl_clear_kernels *cl_get_clear_kernels(struct wined3d_device *device, bool create)
{
    struct cl_clear_kernels *entry;

    EnterCriticalSection(&cl_shadows_cs);
    LIST_FOR_EACH_ENTRY(entry, &cl_clear_kernel_list, struct cl_clear_kernels, entry)
    {
        if (entry->device == device)
        {
            LeaveCriticalSection(&cl_shadows_cs);
            return entry;
        }
    }
    if (create && (entry = calloc(1, sizeof(*entry))))
    {
        entry->device = device;
        list_add_head(&cl_clear_kernel_list, &entry->entry);
    }
    else
    {
        entry = NULL;
    }
    LeaveCriticalSection(&cl_shadows_cs);
    return entry;
}

static void cl_shadow_destroy(struct wined3d_context_gl *context_gl, struct cl_shadow *shadow)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;

    wined3d_cl_forget_gl_object(true, shadow->name);
    gl_info->gl_ops.gl.p_glDeleteTextures(1, &shadow->name);
    GL_EXTCALL(glDeleteBuffers(1, &shadow->pbo));
    EnterCriticalSection(&cl_shadows_cs);
    list_remove(&shadow->entry);
    LeaveCriticalSection(&cl_shadows_cs);
    free(shadow);
}


/* The shadow of a level of "texture_gl"; "image_target" is a cube face's target. */
static struct cl_shadow *cl_get_shadow(struct wined3d_context_gl *context_gl, struct wined3d_texture_gl *texture_gl,
        GLuint source, GLenum image_target, unsigned int level)
{
    const struct wined3d_format *format = texture_gl->t.resource.format;
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    unsigned int width, height, depth;
    struct cl_shadow *shadow;
    GLenum target;

    width = wined3d_texture_get_level_width(&texture_gl->t, level);
    height = wined3d_texture_get_level_height(&texture_gl->t, level);
    if (texture_gl->target == GL_TEXTURE_3D)
        depth = wined3d_texture_get_level_depth(&texture_gl->t, level);
    else if (texture_gl->target == GL_TEXTURE_CUBE_MAP)
        depth = 1;
    else
        depth = texture_gl->t.layer_count;

    EnterCriticalSection(&cl_shadows_cs);
    LIST_FOR_EACH_ENTRY(shadow, &cl_shadows, struct cl_shadow, entry)
    {
        if (shadow->device == context_gl->c.device && shadow->source == source
                && shadow->image_target == image_target && shadow->level == level)
        {
            if (shadow->width == width && shadow->height == height && shadow->depth == depth)
            {
                LeaveCriticalSection(&cl_shadows_cs);
                return shadow;
            }
            cl_shadow_destroy(context_gl, shadow);
            break;
        }
    }
    LeaveCriticalSection(&cl_shadows_cs);

    if (!(shadow = calloc(1, sizeof(*shadow))))
        return NULL;
    shadow->device = context_gl->c.device;
    shadow->source = source;
    shadow->source_target = texture_gl->target;
    shadow->image_target = image_target;
    shadow->level = level;
    shadow->width = width;
    shadow->height = height;
    shadow->depth = depth;
    target = texture_gl->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_2D : texture_gl->target;
    shadow->target = target;

    if (format->depth_size)
    {
        shadow->internal = GL_R32F;
        shadow->format = GL_DEPTH_COMPONENT;
        shadow->upload_format = GL_RED;
        shadow->type = GL_FLOAT;
        shadow->texel_size = 4;
    }
    else if (format->attrs & WINED3D_FORMAT_ATTR_INTEGER)
    {
        bool sint = !(format->attrs & WINED3D_FORMAT_ATTR_UNSIGNED);

        shadow->internal = sint ? GL_RGBA32I : GL_RGBA32UI;
        shadow->format = shadow->upload_format = GL_RGBA_INTEGER;
        shadow->type = sint ? GL_INT : GL_UNSIGNED_INT;
        shadow->texel_size = 16;
    }
    else if (format->red_size > 16 || format->green_size > 16 || format->blue_size > 16 || format->alpha_size > 16)
    {
        shadow->internal = GL_RGBA32F;
        shadow->format = shadow->upload_format = GL_RGBA;
        shadow->type = GL_FLOAT;
        shadow->texel_size = 16;
    }
    else
    {
        shadow->internal = GL_RGBA16F;
        shadow->format = shadow->upload_format = GL_RGBA;
        shadow->type = GL_HALF_FLOAT;
        shadow->texel_size = 8;
    }

    /* Renderable formats are blitted; others go through a pixel buffer,
     * which macOS converts on the CPU. */
    switch (wined3d_gl_get_internal_format(&texture_gl->t.resource, wined3d_format_gl(format), false))
    {
        case GL_R11F_G11F_B10F:
        case GL_RGB10_A2:
        case GL_SRGB8_ALPHA8:
        case GL_RGB565:
        case GL_RGB5_A1:
        case GL_RGBA4:
            shadow->blit = texture_gl->target != GL_TEXTURE_1D && texture_gl->target != GL_TEXTURE_1D_ARRAY;
            break;
        default:
            break;
    }

    if (!shadow->blit)
        WARN("The shadow of level %u of texture %u (%s) is copied through a pixel buffer, "
                "which waits for the GPU at every dispatch.\n", level, source, debug_d3dformat(format->id));
    TRACE("Creating a %ux%ux%u shadow (internal format %#x) of level %u of texture %u (%s).\n",
            width, height, depth, shadow->internal, level, source, debug_d3dformat(format->id));

    gl_info->gl_ops.gl.p_glGenTextures(1, &shadow->name);
    wined3d_context_gl_bind_texture(context_gl, target, shadow->name);
    gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
    gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    switch (target)
    {
        case GL_TEXTURE_1D:
            gl_info->gl_ops.gl.p_glTexImage1D(target, 0, shadow->internal, width, 0,
                    shadow->upload_format, shadow->type, NULL);
            break;
        case GL_TEXTURE_2D_ARRAY:
        case GL_TEXTURE_3D:
            GL_EXTCALL(glTexImage3D(target, 0, shadow->internal, width, height, depth, 0,
                    shadow->upload_format, shadow->type, NULL));
            break;
        case GL_TEXTURE_1D_ARRAY:
            gl_info->gl_ops.gl.p_glTexImage2D(target, 0, shadow->internal, width, depth, 0,
                    shadow->upload_format, shadow->type, NULL);
            break;
        default:
            gl_info->gl_ops.gl.p_glTexImage2D(target, 0, shadow->internal, width, height, 0,
                    shadow->upload_format, shadow->type, NULL);
            break;
    }
    GL_EXTCALL(glGenBuffers(1, &shadow->pbo));
    wined3d_context_gl_bind_bo(context_gl, GL_PIXEL_PACK_BUFFER, shadow->pbo);
    GL_EXTCALL(glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)width * height * depth * shadow->texel_size,
            NULL, GL_STREAM_COPY));
    wined3d_context_gl_bind_bo(context_gl, GL_PIXEL_PACK_BUFFER, 0);
    context_invalidate_compute_state(&context_gl->c, STATE_COMPUTE_SHADER_RESOURCE_BINDING);
    context_invalidate_state(&context_gl->c, STATE_GRAPHICS_SHADER_RESOURCE_BINDING);
    checkGLcall("create shadow texture");

    EnterCriticalSection(&cl_shadows_cs);
    list_add_head(&cl_shadows, &shadow->entry);
    LeaveCriticalSection(&cl_shadows_cs);
    return shadow;
}

/* Copies the source level to the shadow, or the shadow back to the source. */
static void cl_shadow_copy(struct wined3d_context_gl *context_gl, struct cl_shadow *shadow, bool to_shadow)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    GLenum dst_target, dst_image_target, src_target, src_image_target, dst_format, src_format;
    GLuint src, dst;
    unsigned int dst_level, src_level;

    if (to_shadow)
    {
        src = shadow->source;
        src_target = shadow->source_target;
        src_image_target = shadow->image_target;
        src_level = shadow->level;
        src_format = shadow->format;
        dst = shadow->name;
        dst_target = dst_image_target = shadow->target;
        dst_level = 0;
        dst_format = shadow->upload_format;
    }
    else
    {
        src = shadow->name;
        src_target = src_image_target = shadow->target;
        src_level = 0;
        src_format = shadow->upload_format;
        dst = shadow->source;
        dst_target = shadow->source_target;
        dst_image_target = shadow->image_target;
        dst_level = shadow->level;
        dst_format = shadow->format;
        if (dst_format == GL_DEPTH_COMPONENT)
            return;
    }

    if (shadow->blit)
    {
        unsigned int layer, layers = shadow->target == GL_TEXTURE_2D ? 1 : shadow->depth;
        GLuint fbo[2];

        gl_info->fbo_ops.glGenFramebuffers(2, fbo);
        gl_info->fbo_ops.glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]);
        gl_info->fbo_ops.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]);
        gl_info->gl_ops.gl.p_glDisable(GL_SCISSOR_TEST);
        context_invalidate_state(&context_gl->c, STATE_RASTERIZER);
        gl_info->gl_ops.gl.p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        context_invalidate_state(&context_gl->c, STATE_BLEND);
        if (gl_info->supported[ARB_FRAMEBUFFER_SRGB])
        {
            gl_info->gl_ops.gl.p_glDisable(GL_FRAMEBUFFER_SRGB);
            context_invalidate_state(&context_gl->c, STATE_SHADER(WINED3D_SHADER_TYPE_PIXEL));
        }
        for (layer = 0; layer < layers; ++layer)
        {
            if (layers == 1 && src_target != GL_TEXTURE_2D_ARRAY && src_target != GL_TEXTURE_3D)
                gl_info->fbo_ops.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                        src_image_target, src, src_level);
            else
                gl_info->fbo_ops.glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                        src, src_level, layer);
            if (layers == 1 && dst_target != GL_TEXTURE_2D_ARRAY && dst_target != GL_TEXTURE_3D)
                gl_info->fbo_ops.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                        dst_image_target, dst, dst_level);
            else
                gl_info->fbo_ops.glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                        dst, dst_level, layer);
            gl_info->fbo_ops.glBlitFramebuffer(0, 0, shadow->width, shadow->height,
                    0, 0, shadow->width, shadow->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        gl_info->fbo_ops.glBindFramebuffer(GL_READ_FRAMEBUFFER, context_gl->fbo_read_binding);
        gl_info->fbo_ops.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, context_gl->fbo_draw_binding);
        gl_info->fbo_ops.glDeleteFramebuffers(2, fbo);
        context_invalidate_state(&context_gl->c, STATE_FRAMEBUFFER);
        checkGLcall("shadow texture blit");
        return;
    }

    wined3d_context_gl_bind_texture(context_gl, src_target, src);
    wined3d_context_gl_bind_bo(context_gl, GL_PIXEL_PACK_BUFFER, shadow->pbo);
    gl_info->gl_ops.gl.p_glGetTexImage(src_image_target, src_level, src_format, shadow->type, NULL);
    wined3d_context_gl_bind_bo(context_gl, GL_PIXEL_PACK_BUFFER, 0);

    wined3d_context_gl_bind_texture(context_gl, dst_target, dst);
    wined3d_context_gl_bind_bo(context_gl, GL_PIXEL_UNPACK_BUFFER, shadow->pbo);
    switch (dst_target)
    {
        case GL_TEXTURE_1D:
            gl_info->gl_ops.gl.p_glTexSubImage1D(dst_image_target, dst_level, 0, shadow->width,
                    dst_format, shadow->type, NULL);
            break;
        case GL_TEXTURE_2D_ARRAY:
        case GL_TEXTURE_3D:
            GL_EXTCALL(glTexSubImage3D(dst_image_target, dst_level, 0, 0, 0, shadow->width, shadow->height,
                    shadow->depth, dst_format, shadow->type, NULL));
            break;
        case GL_TEXTURE_1D_ARRAY:
            gl_info->gl_ops.gl.p_glTexSubImage2D(dst_image_target, dst_level, 0, 0, shadow->width, shadow->depth,
                    dst_format, shadow->type, NULL);
            break;
        default:
            gl_info->gl_ops.gl.p_glTexSubImage2D(dst_image_target, dst_level, 0, 0, shadow->width, shadow->height,
                    dst_format, shadow->type, NULL);
            break;
    }
    wined3d_context_gl_bind_bo(context_gl, GL_PIXEL_UNPACK_BUFFER, 0);
    context_invalidate_compute_state(&context_gl->c, STATE_COMPUTE_SHADER_RESOURCE_BINDING);
    context_invalidate_state(&context_gl->c, STATE_GRAPHICS_SHADER_RESOURCE_BINDING);
    checkGLcall("shadow texture copy");
}

void wined3d_cl_forget_gl_object(bool texture, GLuint name)
{
    struct wined3d_cl_forget_params params;

    if (!cl_used || !name)
        return;
    /* The caller's context is current; "name" is in its share group. */
    if (texture)
    {
        struct wined3d_context_gl *context_gl;
        struct cl_shadow *shadow, *next;

        if ((context_gl = wined3d_context_gl_get_current()))
        {
            EnterCriticalSection(&cl_shadows_cs);
            LIST_FOR_EACH_ENTRY_SAFE(shadow, next, &cl_shadows, struct cl_shadow, entry)
            {
                if (shadow->device == context_gl->c.device && shadow->source == name)
                    cl_shadow_destroy(context_gl, shadow);
            }
            LeaveCriticalSection(&cl_shadows_cs);
        }
    }
    params.type = texture ? WINED3D_CL_OBJECT_TEXTURE : WINED3D_CL_OBJECT_BUFFER;
    params.name = name;
    WINE_UNIX_CALL(unix_cl_forget, &params);
}

/* Releases the shadows and the OpenCL context of the device whose context is
 * current, before its OpenGL contexts, and with them its share group, are
 * destroyed. A share group made later may reuse the address. */
void wined3d_cl_release_device(struct wined3d_context_gl *context_gl)
{
    struct cl_shadow *shadow, *next;
    struct cl_clear_kernels *clear;
    unsigned int i;

    if (!cl_used)
        return;
    EnterCriticalSection(&cl_shadows_cs);
    LIST_FOR_EACH_ENTRY_SAFE(shadow, next, &cl_shadows, struct cl_shadow, entry)
    {
        if (shadow->device == context_gl->c.device)
            cl_shadow_destroy(context_gl, shadow);
    }
    if ((clear = cl_get_clear_kernels(context_gl->c.device, false)))
    {
        for (i = 0; i < ARRAY_SIZE(clear->kernels); ++i)
            wined3d_cl_release_kernel(clear->kernels[i]);
        list_remove(&clear->entry);
        free(clear);
    }
    LeaveCriticalSection(&cl_shadows_cs);
    WINE_UNIX_CALL(unix_cl_release_context, NULL);
}


/* The OpenCL image of a texture view: the OpenGL target and level it is made
 * from (a shadow's, for a format OpenCL can't share), its kind, and its
 * information argument. */
struct cl_texture_view
{
    GLuint name;
    GLenum target;
    unsigned int level;
    uint8_t kind;
    uint32_t info[8];
    struct cl_shadow *shadow;
};

static bool cl_get_texture_view(struct wined3d_context_gl *context_gl, struct wined3d_resource *resource,
        const struct wined3d_view_desc *desc, bool uav, struct cl_texture_view *view)
{
    struct wined3d_texture_gl *texture_gl = wined3d_texture_gl(texture_from_resource(resource));
    struct wined3d_texture *texture = &texture_gl->t;
    unsigned int level = desc->u.texture.level_idx, layers;
    enum wined3d_cl_image_dim dim;

    memset(view, 0, sizeof(*view));
    view->name = wined3d_texture_gl_get_texture_name(texture_gl, &context_gl->c, FALSE);
    view->target = texture_gl->target;
    view->level = level;
    layers = desc->u.texture.layer_count;

    switch (texture_gl->target)
    {
        case GL_TEXTURE_1D: dim = WINED3D_CL_IMAGE_1D; break;
        case GL_TEXTURE_1D_ARRAY: dim = WINED3D_CL_IMAGE_1D_ARRAY; break;
        case GL_TEXTURE_2D:
        case GL_TEXTURE_RECTANGLE_ARB: dim = WINED3D_CL_IMAGE_2D; break;
        case GL_TEXTURE_2D_ARRAY: dim = WINED3D_CL_IMAGE_2D_ARRAY; break;
        case GL_TEXTURE_3D: dim = WINED3D_CL_IMAGE_3D; break;
        case GL_TEXTURE_2D_MULTISAMPLE: dim = WINED3D_CL_IMAGE_2D_MSAA; break;
        case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: dim = WINED3D_CL_IMAGE_2D_ARRAY_MSAA; break;
        case GL_TEXTURE_CUBE_MAP:
            /* OpenCL images of cube textures are single faces. */
            if (layers > 1)
                FIXME("Using face %u of a %u-face cube view.\n", desc->u.texture.layer_idx, layers);
            view->target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + desc->u.texture.layer_idx % 6;
            dim = WINED3D_CL_IMAGE_2D;
            layers = 1;
            break;
        default:
            FIXME("Unhandled texture target %#x.\n", texture_gl->target);
            return false;
    }

    view->kind = WINED3D_CL_IMAGE_KIND(dim, cl_texture_channel(texture));
    if (!cl_gl_format_shareable(wined3d_gl_get_internal_format(&texture->resource,
            wined3d_format_gl(texture->resource.format), false)))
    {
        if (dim == WINED3D_CL_IMAGE_2D_MSAA || dim == WINED3D_CL_IMAGE_2D_ARRAY_MSAA)
        {
            FIXME("Unhandled multisample format %s.\n", debug_d3dformat(texture->resource.format->id));
            return false;
        }
        if (!(view->shadow = cl_get_shadow(context_gl, texture_gl, view->name, view->target, level)))
            return false;
        view->name = view->shadow->name;
        view->target = view->shadow->target;
        view->level = 0;
        if (view->shadow->internal == GL_RGBA32UI)
            view->kind = WINED3D_CL_IMAGE_KIND(dim, WINED3D_CL_CHANNEL_UINT);
        else if (view->shadow->internal == GL_RGBA32I)
            view->kind = WINED3D_CL_IMAGE_KIND(dim, WINED3D_CL_CHANNEL_INT);
        else
            view->kind = WINED3D_CL_IMAGE_KIND(dim, WINED3D_CL_CHANNEL_FLOAT);
    }
    view->info[0] = wined3d_texture_get_level_width(texture, level);
    view->info[1] = wined3d_texture_get_level_height(texture, level);
    if (dim == WINED3D_CL_IMAGE_3D)
    {
        unsigned int depth = wined3d_texture_get_level_depth(texture, level);

        if (uav)
        {
            view->info[4] = min(desc->u.texture.layer_idx, depth);
            view->info[2] = min(layers, depth - view->info[4]);
        }
        else
        {
            view->info[2] = depth;
        }
    }
    else
    {
        view->info[2] = layers;
        if (dim == WINED3D_CL_IMAGE_1D_ARRAY || dim == WINED3D_CL_IMAGE_2D_ARRAY
                || dim == WINED3D_CL_IMAGE_2D_ARRAY_MSAA)
            view->info[4] = desc->u.texture.layer_idx;
    }
    view->info[3] = uav ? 1 : desc->u.texture.level_count;
    view->info[5] = max(texture->resource.multisample_type, 1);
    return true;
}

static void cl_arg_gl_buffer(struct wined3d_cl_arg *arg, struct wined3d_bo_gl *bo_gl)
{
    arg->type = WINED3D_CL_ARG_GL_BUFFER;
    arg->gl_name = bo_gl->id;
}

static void cl_arg_value(struct wined3d_cl_arg *arg, const void *data, unsigned int size)
{
    arg->type = WINED3D_CL_ARG_VALUE;
    arg->size = size;
    memcpy(arg->data, data, size);
}

static bool cl_read_indirect_args(struct wined3d_context_gl *context_gl, const struct wined3d_state *state,
        const struct wined3d_indirect_dispatch_parameters *indirect, uint32_t *counts)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_buffer *buffer = indirect->buffer;
    struct wined3d_bo_gl *bo_gl;

    wined3d_cl_finish();
    wined3d_buffer_load_location(buffer, &context_gl->c, WINED3D_LOCATION_BUFFER);
    if (!buffer->buffer_object)
    {
        ERR("Indirect argument buffer %p has no buffer object.\n", buffer);
        return false;
    }
    bo_gl = wined3d_bo_gl(buffer->buffer_object);
    wined3d_context_gl_bind_bo(context_gl, bo_gl->binding, bo_gl->id);
    GL_EXTCALL(glGetBufferSubData(bo_gl->binding, bo_gl->b.buffer_offset + indirect->offset,
            3 * sizeof(*counts), counts));
    wined3d_context_gl_bind_bo(context_gl, bo_gl->binding, 0);
    checkGLcall("read indirect dispatch arguments");
    return true;
}

bool wined3d_cl_dispatch_compute(struct wined3d_context_gl *context_gl, const struct wined3d_state *state,
        const struct wined3d_dispatch_parameters *parameters)
{
    struct wined3d_shader *shader = state->shader[WINED3D_SHADER_TYPE_COMPUTE];
    struct wined3d_unordered_access_view * const *uavs = state->unordered_access_view[WINED3D_PIPELINE_COMPUTE];
    struct cl_texture_view srv_images[MAX_SHADER_RESOURCE_VIEWS], uav_images[MAX_UNORDERED_ACCESS_VIEWS];
    const struct wined3d_shader_thread_group_size *group_size;
    const struct wined3d_shader_reg_maps *reg_maps;
    struct wined3d_cl_dispatch_params params;
    const struct wined3d_cl_kernel *kernel;
    struct wined3d_cl_kernel_key key;
    struct wined3d_cl_arg *args;
    struct wined3d_buffer *buffer;
    uint32_t counts[3];
    unsigned int i;
    NTSTATUS status;

    if (!wined3d_cl_available(context_gl))
        return false;
    cl_used = true;

    reg_maps = &shader->reg_maps;
    group_size = &shader->u.cs.thread_group_size;

    if (parameters->indirect)
    {
        if (!cl_read_indirect_args(context_gl, state, &parameters->u.indirect, counts))
            return true;
    }
    else
    {
        counts[0] = parameters->u.direct.group_count_x;
        counts[1] = parameters->u.direct.group_count_y;
        counts[2] = parameters->u.direct.group_count_z;
    }
    TRACE("Dispatching %ux%ux%u groups of %ux%ux%u threads of compute shader %p.\n",
            counts[0], counts[1], counts[2], group_size->x, group_size->y, group_size->z, shader);
    if (!counts[0] || !counts[1] || !counts[2])
        return true;

    /* Load the resources into their OpenGL locations, and choose the kernel
     * for the image kinds bound. */
    memset(&key, 0, sizeof(key));
    memset(srv_images, 0, sizeof(srv_images));
    memset(uav_images, 0, sizeof(uav_images));
    for (i = 0; i < WINED3D_MAX_CBS; ++i)
    {
        if (reg_maps->cb_sizes[i] && (buffer = state->cb[WINED3D_SHADER_TYPE_COMPUTE][i].buffer))
        {
            wined3d_buffer_load_location(buffer, &context_gl->c, WINED3D_LOCATION_BUFFER);
            wined3d_context_gl_reference_buffer(context_gl, buffer);
        }
    }
    for (i = 0; i < ARRAY_SIZE(reg_maps->resource_info); ++i)
    {
        struct wined3d_shader_resource_view *view;

        if (!reg_maps->resource_info[i].type || !(view = state->shader_resource_view[WINED3D_SHADER_TYPE_COMPUTE][i]))
            continue;
        if (view->resource->type == WINED3D_RTYPE_BUFFER)
        {
            buffer = buffer_from_resource(view->resource);
            wined3d_buffer_load_location(buffer, &context_gl->c, WINED3D_LOCATION_BUFFER);
            wined3d_context_gl_reference_buffer(context_gl, buffer);
        }
        else
        {
            wined3d_texture_load(texture_from_resource(view->resource), &context_gl->c, FALSE);
            if (cl_get_texture_view(context_gl, view->resource, &view->desc, false, &srv_images[i]))
            {
                key.srv[i] = srv_images[i].kind;
                if (srv_images[i].shadow)
                    cl_shadow_copy(context_gl, srv_images[i].shadow, true);
            }
        }
    }
    for (i = 0; i < MAX_UNORDERED_ACCESS_VIEWS; ++i)
    {
        struct wined3d_unordered_access_view *view;

        if (!reg_maps->uav_resource_info[i].type || !(view = uavs[i]))
            continue;
        if (view->resource->type == WINED3D_RTYPE_BUFFER)
        {
            buffer = buffer_from_resource(view->resource);
            wined3d_buffer_acquire_bo_for_write(buffer, &context_gl->c);
            wined3d_buffer_load_location(buffer, &context_gl->c, WINED3D_LOCATION_BUFFER);
            wined3d_unordered_access_view_invalidate_location(view, ~WINED3D_LOCATION_BUFFER);
            wined3d_context_gl_reference_buffer(context_gl, buffer);
        }
        else
        {
            wined3d_texture_load(texture_from_resource(view->resource), &context_gl->c, FALSE);
            wined3d_unordered_access_view_invalidate_location(view, ~WINED3D_LOCATION_TEXTURE_RGB);
            if (cl_get_texture_view(context_gl, view->resource, &view->desc, true, &uav_images[i]))
            {
                key.uav[i] = uav_images[i].kind;
                if (uav_images[i].shadow)
                    cl_shadow_copy(context_gl, uav_images[i].shadow, true);
            }
        }
    }
    /* Unbound texture slots take a null image of the kind the shader declares. */
    for (i = 0; i < ARRAY_SIZE(reg_maps->resource_info); ++i)
    {
        if (reg_maps->resource_info[i].type > WINED3D_SHADER_RESOURCE_BUFFER && !key.srv[i])
            key.srv[i] = WINED3D_CL_IMAGE_KIND(WINED3D_CL_IMAGE_2D, WINED3D_CL_CHANNEL_FLOAT);
    }
    for (i = 0; i < MAX_UNORDERED_ACCESS_VIEWS; ++i)
    {
        if (reg_maps->uav_resource_info[i].type > WINED3D_SHADER_RESOURCE_BUFFER && !key.uav[i])
            key.uav[i] = WINED3D_CL_IMAGE_KIND(WINED3D_CL_IMAGE_2D, WINED3D_CL_CHANNEL_FLOAT);
    }

    if (!(kernel = wined3d_glsl_get_cl_kernel(context_gl, shader, &key)))
        return true;

    if (!(args = calloc(kernel->binding_count, sizeof(*args))))
        return true;

    for (i = 0; i < kernel->binding_count; ++i)
    {
        const struct wined3d_cl_binding *binding = &kernel->bindings[i];
        struct wined3d_cl_arg *arg = &args[i];
        unsigned int slot = binding->slot;

        switch (binding->type)
        {
            case WINED3D_CL_BINDING_CB:
            {
                const struct wined3d_constant_buffer_state *cb = &state->cb[WINED3D_SHADER_TYPE_COMPUTE][slot];

                if (cb->buffer && cb->buffer->buffer_object)
                    cl_arg_gl_buffer(arg, wined3d_bo_gl(cb->buffer->buffer_object));
                else
                    arg->type = WINED3D_CL_ARG_NULL_BUFFER;
                break;
            }

            case WINED3D_CL_BINDING_CB_OFFSET:
            {
                const struct wined3d_constant_buffer_state *cb = &state->cb[WINED3D_SHADER_TYPE_COMPUTE][slot];
                uint32_t offset = 0;

                if (cb->buffer && cb->buffer->buffer_object)
                    offset = cb->buffer->buffer_object->buffer_offset + cb->offset;
                cl_arg_value(arg, &offset, sizeof(offset));
                break;
            }

            case WINED3D_CL_BINDING_SRV:
            case WINED3D_CL_BINDING_SRV_INFO:
            {
                struct wined3d_shader_resource_view *view = state->shader_resource_view[WINED3D_SHADER_TYPE_COMPUTE][slot];
                bool info = binding->type == WINED3D_CL_BINDING_SRV_INFO;
                uint32_t data[8] = {0};

                if (reg_maps->resource_info[slot].type == WINED3D_SHADER_RESOURCE_BUFFER)
                {
                    if (view && view->resource->type == WINED3D_RTYPE_BUFFER
                            && (buffer = buffer_from_resource(view->resource))->buffer_object)
                    {
                        cl_buffer_view_info(buffer, &view->desc, view->format, data);
                        if (info)
                            cl_arg_value(arg, data, sizeof(data));
                        else
                            cl_arg_gl_buffer(arg, wined3d_bo_gl(buffer->buffer_object));
                    }
                    else if (info)
                        cl_arg_value(arg, data, sizeof(data));
                    else
                        arg->type = WINED3D_CL_ARG_NULL_BUFFER;
                }
                else if (view && srv_images[slot].name && srv_images[slot].kind == key.srv[slot])
                {
                    if (info)
                    {
                        cl_arg_value(arg, srv_images[slot].info, sizeof(srv_images[slot].info));
                    }
                    else
                    {
                        arg->type = WINED3D_CL_ARG_GL_TEXTURE;
                        arg->gl_name = srv_images[slot].name;
                        arg->gl_target = srv_images[slot].target;
                        arg->level = srv_images[slot].level;
                    }
                }
                else if (info)
                {
                    cl_arg_value(arg, data, sizeof(data));
                }
                else
                {
                    arg->type = WINED3D_CL_ARG_NULL_IMAGE;
                    arg->image_type = WINED3D_CL_IMAGE_KIND_DIM(key.srv[slot]);
                    arg->channel = WINED3D_CL_IMAGE_KIND_CHANNEL(key.srv[slot]);
                }
                break;
            }

            case WINED3D_CL_BINDING_SAMPLER:
            {
                const struct wined3d_sampler *sampler = state->sampler[WINED3D_SHADER_TYPE_COMPUTE][slot];
                struct wined3d_cl_sampler_desc desc = {WINED3D_TADDRESS_CLAMP, 0};

                if (sampler)
                {
                    desc.address_mode = sampler->desc.address_u;
                    desc.linear = sampler->desc.min_filter == WINED3D_TEXF_LINEAR
                            || sampler->desc.mag_filter == WINED3D_TEXF_LINEAR
                            || sampler->desc.min_filter == WINED3D_TEXF_ANISOTROPIC;
                }
                arg->type = WINED3D_CL_ARG_SAMPLER;
                memcpy(arg->data, &desc, sizeof(desc));
                break;
            }

            case WINED3D_CL_BINDING_SAMPLER_CMP:
            {
                const struct wined3d_sampler *sampler = state->sampler[WINED3D_SHADER_TYPE_COMPUTE][slot];
                uint32_t func = sampler ? sampler->desc.comparison_func : WINED3D_CMP_NEVER;

                cl_arg_value(arg, &func, sizeof(func));
                break;
            }

            case WINED3D_CL_BINDING_UAV:
            case WINED3D_CL_BINDING_UAV_READ:
            case WINED3D_CL_BINDING_UAV_INFO:
            case WINED3D_CL_BINDING_UAV_COUNTER:
            {
                struct wined3d_unordered_access_view *view = uavs[slot];
                struct wined3d_unordered_access_view_gl *view_gl = view ? wined3d_unordered_access_view_gl(view) : NULL;
                bool info = binding->type == WINED3D_CL_BINDING_UAV_INFO;
                uint32_t data[8] = {0};

                if (binding->type == WINED3D_CL_BINDING_UAV_COUNTER)
                {
                    if (view_gl && view_gl->counter_bo.id)
                        cl_arg_gl_buffer(arg, &view_gl->counter_bo);
                    else
                        arg->type = WINED3D_CL_ARG_NULL_BUFFER;
                    break;
                }

                if (reg_maps->uav_resource_info[slot].type == WINED3D_SHADER_RESOURCE_BUFFER)
                {
                    if (view && view->resource->type == WINED3D_RTYPE_BUFFER
                            && (buffer = buffer_from_resource(view->resource))->buffer_object)
                    {
                        cl_buffer_view_info(buffer, &view->desc, view->format, data);
                        if (view_gl->counter_bo.id)
                            data[5] = view_gl->counter_bo.b.buffer_offset;
                        if (info)
                            cl_arg_value(arg, data, sizeof(data));
                        else
                            cl_arg_gl_buffer(arg, wined3d_bo_gl(buffer->buffer_object));
                    }
                    else if (info)
                        cl_arg_value(arg, data, sizeof(data));
                    else
                        arg->type = WINED3D_CL_ARG_NULL_BUFFER;
                }
                else if (view && uav_images[slot].name && uav_images[slot].kind == key.uav[slot])
                {
                    if (info)
                    {
                        cl_arg_value(arg, uav_images[slot].info, sizeof(uav_images[slot].info));
                    }
                    else
                    {
                        arg->type = WINED3D_CL_ARG_GL_TEXTURE;
                        arg->gl_name = uav_images[slot].name;
                        arg->gl_target = uav_images[slot].target;
                        arg->level = uav_images[slot].level;
                    }
                }
                else if (info)
                {
                    cl_arg_value(arg, data, sizeof(data));
                }
                else
                {
                    arg->type = WINED3D_CL_ARG_NULL_IMAGE;
                    arg->image_type = WINED3D_CL_IMAGE_KIND_DIM(key.uav[slot]);
                    arg->channel = WINED3D_CL_IMAGE_KIND_CHANNEL(key.uav[slot]);
                }
                break;
            }

            default:
                ERR("Unhandled binding type %u.\n", binding->type);
                break;
        }
    }

    memset(&params, 0, sizeof(params));
    params.kernel = kernel->handle;
    params.args = (ULONG_PTR)args;
    params.arg_count = kernel->binding_count;
    params.group_count[0] = counts[0];
    params.group_count[1] = counts[1];
    params.group_count[2] = counts[2];
    params.group_size[0] = group_size->x;
    params.group_size[1] = group_size->y;
    params.group_size[2] = group_size->z;
    params.sync = 0;
    if ((status = WINE_UNIX_CALL(unix_cl_dispatch, &params)))
    {
        for (i = 0; i < kernel->binding_count; ++i)
        {
            if (args[i].status)
                FIXME("Compute shader %p: argument %u (binding %u, slot %u, OpenGL object %u, target %#x)"
                        " failed with OpenCL error %d.\n", shader, i, kernel->bindings[i].type,
                        kernel->bindings[i].slot, args[i].gl_name, args[i].gl_target, (int)args[i].status);
        }
        ERR("Compute shader %p: dispatch failed, status %#lx.\n", shader, status);
    }
    free(args);

    for (i = 0; i < MAX_UNORDERED_ACCESS_VIEWS; ++i)
    {
        if (uavs[i] && reg_maps->uav_resource_info[i].type > WINED3D_SHADER_RESOURCE_BUFFER
                && uav_images[i].shadow && uav_images[i].kind == key.uav[i])
            cl_shadow_copy(context_gl, uav_images[i].shadow, false);
    }

    /* OpenGL state the dispatch's resource loads changed. */
    context_invalidate_state(&context_gl->c, STATE_FRAMEBUFFER);
    context_gl->c.last_was_blit = FALSE;
    context_gl->c.last_was_ffp_blit = FALSE;
    return true;
}

/* The float a channel of "bits" holds for the raw bits "v" (a uint clear writes
 * a view's channels' bits). */
static uint32_t cl_channel_bits_to_float(uint32_t v, unsigned int bits, unsigned int attrs)
{
    uint32_t mask = bits >= 32 ? ~0u : (1u << bits) - 1;
    union {float f; uint32_t u;} r;

    v &= mask;
    if ((attrs & WINED3D_FORMAT_ATTR_INTEGER) || bits >= 32)
        return v;
    if (attrs & WINED3D_FORMAT_ATTR_FLOAT)
    {
        /* 16-bit floats have a sign, 11 and 10-bit ones none; all have 5 exponent bits. */
        unsigned int mantissa_bits = bits == 16 ? 10 : bits - 5, e, m;
        bool negative = bits == 16 && (v & 0x8000);

        if (bits != 16 && bits != 11 && bits != 10)
            return v;
        e = (v >> mantissa_bits) & 0x1f;
        m = v & ((1u << mantissa_bits) - 1);
        if (e == 0x1f)
            r.u = 0x7f800000 | (m << (23 - mantissa_bits));
        else if (!e)
            r.f = ldexpf((float)m, -14 - (int)mantissa_bits);
        else
            r.u = ((e + 112) << 23) | (m << (23 - mantissa_bits));
        if (negative)
            r.u |= 0x80000000;
        return r.u;
    }
    if (attrs & WINED3D_FORMAT_ATTR_UNSIGNED)
    {
        r.f = (float)v / (float)mask;
    }
    else
    {
        int s = (int)(v << (32 - bits)) >> (32 - bits);

        r.f = max((float)s / (float)(mask >> 1), -1.0f);
    }
    return r.u;
}

/* The float bits "v" rounded to the nearest value a normalised channel of
 * "bits" holds, so that the image write rounds no further. */
static uint32_t cl_round_normalised(uint32_t v, unsigned int bits, unsigned int attrs)
{
    union {float f; uint32_t u;} r;
    float max_value;

    r.u = v;
    if (attrs & WINED3D_FORMAT_ATTR_UNSIGNED)
    {
        max_value = (float)((1u << bits) - 1);
        r.f = floorf(min(max(r.f, 0.0f), 1.0f) * max_value + 0.5f) / max_value;
    }
    else
    {
        max_value = (float)((1u << (bits - 1)) - 1);
        r.f = min(max(r.f, -1.0f), 1.0f) * max_value;
        r.f = (r.f < 0.0f ? -floorf(-r.f + 0.5f) : floorf(r.f + 0.5f)) / max_value;
    }
    return r.u;
}

bool wined3d_cl_clear_uav(struct wined3d_unordered_access_view_gl *view_gl, const struct wined3d_uvec4 *clear_value,
        struct wined3d_context_gl *context_gl, bool fp)
{
    struct cl_clear_kernels *clear;
    struct wined3d_unordered_access_view *view = &view_gl->v;
    struct wined3d_resource *resource = view->resource;
    unsigned int group_size[3] = {64, 1, 1}, extent[3] = {1, 1, 1}, i;
    uint32_t info[8] = {0}, value[4];
    struct wined3d_cl_dispatch_params params;
    struct cl_shadow *shadow = NULL;
    struct wined3d_cl_arg args[3];
    NTSTATUS status;
    uint8_t kind = 0;

    if (!wined3d_cl_available(context_gl))
        return false;
    cl_used = true;

    TRACE("view %p, value %#x %#x %#x %#x, fp %d.\n", view, clear_value->x, clear_value->y,
            clear_value->z, clear_value->w, fp);
    memset(args, 0, sizeof(args));
    value[0] = clear_value->x;
    value[1] = clear_value->y;
    value[2] = clear_value->z;
    value[3] = clear_value->w;

    if (resource->type == WINED3D_RTYPE_BUFFER)
    {
        struct wined3d_buffer *buffer = buffer_from_resource(resource);

        wined3d_buffer_acquire_bo_for_write(buffer, &context_gl->c);
        wined3d_buffer_load_location(buffer, &context_gl->c, WINED3D_LOCATION_BUFFER);
        wined3d_unordered_access_view_invalidate_location(view, ~WINED3D_LOCATION_BUFFER);
        if (!buffer->buffer_object)
            return false;
        wined3d_context_gl_reference_buffer(context_gl, buffer);
        cl_buffer_view_info(buffer, &view->desc, view->format, info);
        if ((view->desc.flags & WINED3D_VIEW_BUFFER_RAW) || view->desc.format_id == WINED3DFMT_UNKNOWN)
        {
            info[3] = ~0u;
            extent[0] = info[1] / 4;
        }
        else
        {
            /* A uint clear writes each channel's bits. */
            if (!fp)
                info[3] = (info[3] & ~(7u << 4)) | WINED3D_CL_FORMAT_UINT;
            extent[0] = info[2];
        }
        cl_arg_gl_buffer(&args[0], wined3d_bo_gl(buffer->buffer_object));
    }
    else
    {
        const struct wined3d_format *format = view->format;
        struct cl_texture_view image;
        unsigned int dim;

        wined3d_texture_load(texture_from_resource(resource), &context_gl->c, FALSE);
        wined3d_unordered_access_view_invalidate_location(view, ~WINED3D_LOCATION_TEXTURE_RGB);
        if (!cl_get_texture_view(context_gl, resource, &view->desc, true, &image))
            return false;
        if ((shadow = image.shadow))
            cl_shadow_copy(context_gl, shadow, true);
        kind = image.kind;
        dim = WINED3D_CL_IMAGE_KIND_DIM(kind);
        memcpy(info, image.info, sizeof(info));
        extent[0] = info[0];
        if (dim == WINED3D_CL_IMAGE_1D_ARRAY)
            extent[1] = info[2];
        else if (dim != WINED3D_CL_IMAGE_1D)
            extent[1] = info[1];
        if (dim == WINED3D_CL_IMAGE_2D_ARRAY || dim == WINED3D_CL_IMAGE_3D)
            extent[2] = info[2];
        if (dim != WINED3D_CL_IMAGE_1D && dim != WINED3D_CL_IMAGE_1D_ARRAY)
        {
            group_size[0] = 8;
            group_size[1] = 8;
        }
        if (WINED3D_CL_IMAGE_KIND_CHANNEL(kind) == WINED3D_CL_CHANNEL_FLOAT)
        {
            const unsigned int sizes[4] = {format->red_size, format->green_size, format->blue_size, format->alpha_size};

            for (i = 0; i < 4; ++i)
            {
                if (!fp)
                    value[i] = sizes[i] ? cl_channel_bits_to_float(value[i], sizes[i], format->attrs) : 0;
                else if (sizes[i] && sizes[i] < 32 && (format->attrs & WINED3D_FORMAT_ATTR_NORMALISED))
                    value[i] = cl_round_normalised(value[i], sizes[i], format->attrs);
            }
        }
        else if (!fp)
        {
            const unsigned int sizes[4] = {format->red_size, format->green_size, format->blue_size, format->alpha_size};

            /* Each channel takes the low bits of its value. */
            for (i = 0; i < 4; ++i)
            {
                if (sizes[i] && sizes[i] < 32)
                    value[i] &= (1u << sizes[i]) - 1;
            }
        }
        args[0].type = WINED3D_CL_ARG_GL_TEXTURE;
        args[0].gl_name = image.name;
        args[0].gl_target = image.target;
        args[0].level = image.level;
    }
    cl_arg_value(&args[1], info, sizeof(info));
    cl_arg_value(&args[2], value, sizeof(value));

    if (!(clear = cl_get_clear_kernels(context_gl->c.device, true)))
        return false;
    if (!clear->kernels[kind] && !clear->failed[kind]
            && !wined3d_glsl_build_cl_clear_kernel(kind, group_size, &clear->kernels[kind]))
        clear->failed[kind] = true;
    if (!clear->kernels[kind])
        return false;

    memset(&params, 0, sizeof(params));
    params.kernel = clear->kernels[kind];
    params.args = (ULONG_PTR)args;
    params.arg_count = ARRAY_SIZE(args);
    for (i = 0; i < 3; ++i)
    {
        params.group_size[i] = group_size[i];
        params.group_count[i] = (extent[i] + group_size[i] - 1) / group_size[i];
        if (!params.group_count[i])
            return true;
    }
    params.sync = 0;
    if ((status = WINE_UNIX_CALL(unix_cl_dispatch, &params)))
        ERR("Failed to clear unordered access view %p, status %#lx.\n", view, status);
    if (shadow)
        cl_shadow_copy(context_gl, shadow, false);
    return true;
}
