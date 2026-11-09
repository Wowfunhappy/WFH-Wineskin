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

bool wined3d_cl_build_kernel(const char *source, const struct wined3d_shader *shader, uint64_t *handle)
{
    const struct wined3d_shader_thread_group_size *group_size = &shader->u.cs.thread_group_size;
    struct wined3d_cl_build_params params;
    NTSTATUS status;

    memset(&params, 0, sizeof(params));
    params.source = (ULONG_PTR)source;
    params.group_size[0] = group_size->x;
    params.group_size[1] = group_size->y;
    params.group_size[2] = group_size->z;
    if ((status = WINE_UNIX_CALL(unix_cl_build, &params)))
    {
        ERR("Failed to build the OpenCL kernel for compute shader %p, status %#lx.\n", shader, status);
        return false;
    }
    if (params.work_group_size < group_size->x * group_size->y * group_size->z)
    {
        FIXME("Compute shader %p: thread group %ux%ux%u exceeds the kernel's work group size %u.\n",
                shader, group_size->x, group_size->y, group_size->z, params.work_group_size);
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

void wined3d_cl_forget_gl_object(bool texture, GLuint name)
{
    struct wined3d_cl_forget_params params;

    if (!cl_used || !name)
        return;
    params.type = texture ? WINED3D_CL_OBJECT_TEXTURE : WINED3D_CL_OBJECT_BUFFER;
    params.name = name;
    WINE_UNIX_CALL(unix_cl_forget, &params);
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

/* The OpenCL image of a texture view: the OpenGL target and level it is made
 * from, its kind, and its information argument. */
struct cl_texture_view
{
    GLuint name;
    GLenum target;
    unsigned int level;
    uint8_t kind;
    uint32_t info[8];
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
                key.srv[i] = srv_images[i].kind;
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
                key.uav[i] = uav_images[i].kind;
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
    params.sync = 1;
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

    /* OpenGL state the dispatch's resource loads changed. */
    context_invalidate_state(&context_gl->c, STATE_FRAMEBUFFER);
    context_gl->c.last_was_blit = FALSE;
    context_gl->c.last_was_ffp_blit = FALSE;
    return true;
}
