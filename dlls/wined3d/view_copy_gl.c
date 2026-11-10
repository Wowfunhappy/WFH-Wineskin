/*
 * WINESKIN: views that are textures of their own.
 *
 * Without ARB_texture_view (macOS's OpenGL 4.1) a view is bound as its
 * texture, which is right as long as the view reads the texture the way it
 * is stored. A view that doesn't is a texture of its own in the view's
 * format, holding the levels and layers the view selects. It is brought up to
 * date on the GPU (texture_copy_gl.c) where a draw loads its resources, and
 * only after the texture has been written since the last time
 * (wined3d_texture.content_serial).
 *
 * Such views are:
 * - views that read a typeless texture's bits as another kind of number than
 *   it is stored as: an R32_UINT view of an R32_TYPELESS texture, which is
 *   stored as R32F (see init_typeless_formats());
 * - views of some of the texture's layers, and views of another type than the
 *   texture (a cube view of six layers of an array, a 2D view of one layer);
 * - unordered access views of a level other than the first, when a graphics
 *   shader reads them as a texture (shader_glsl_declare_image_emulation). An
 *   unordered access view gets its texture the first time a draw uses it:
 *   OpenCL kernels use the texture itself.
 *
 * A depth view's texture has the texture's depth format; it holds no stencil.
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

struct wined3d_gl_view_copy
{
    struct wined3d_texture_gl *texture_gl;
    unsigned int level_idx, level_count, layer_idx, layer_count;
    struct wined3d_gl_texel_format texture_format, view_format;
    bool reinterpret;
    /* The texture's content_serial the view's texture was copied at. */
    uint32_t serial;
    bool loaded;
};

/* Whether a view reads the texture's bits as another kind of number than the
 * texture stores. */
static bool view_reinterprets(const struct wined3d_texture_gl *texture_gl, const struct wined3d_format *view_format)
{
    const struct wined3d_format *format = texture_gl->t.resource.format;
    struct wined3d_gl_texel_format texture_texel, view_texel;

    if (format->id == view_format->id || !wined3d_format_is_typeless(format)
            || format->typeless_id != view_format->typeless_id || format->byte_count != view_format->byte_count)
        return false;
    wined3d_gl_texel_format_from_format(&texture_texel, format);
    wined3d_gl_texel_format_from_format(&view_texel, view_format);
    if (texture_texel.type == WINED3D_GL_TEXEL_DEPTH || view_texel.type == WINED3D_GL_TEXEL_DEPTH
            || texture_texel.type == WINED3D_GL_TEXEL_FLOAT || view_texel.type == WINED3D_GL_TEXEL_FLOAT)
        return false;
    return texture_texel.type != view_texel.type;
}

static bool view_is_depth(const struct wined3d_texture_gl *texture_gl)
{
    const struct wined3d_format *format = texture_gl->t.resource.format;

    return format->depth_size || format->stencil_size;
}

bool wined3d_gl_view_needs_copy(const struct wined3d_gl_info *gl_info, const struct wined3d_texture_gl *texture_gl,
        const struct wined3d_view_desc *desc, const struct wined3d_format *view_format, GLenum view_target)
{
    if (gl_info->supported[ARB_TEXTURE_VIEW] || gl_info->glsl_version < MAKEDWORD_VERSION(3, 30))
        return false;
    if (texture_gl->t.swapchain)
        return false;
    switch (texture_gl->target)
    {
        case GL_TEXTURE_1D:
        case GL_TEXTURE_1D_ARRAY:
        case GL_TEXTURE_2D:
        case GL_TEXTURE_2D_ARRAY:
        case GL_TEXTURE_CUBE_MAP:
            break;
        case GL_TEXTURE_3D:
            return view_target == GL_TEXTURE_3D && view_reinterprets(texture_gl, view_format);
        default:
            return false;
    }
    switch (view_target)
    {
        case GL_TEXTURE_1D:
        case GL_TEXTURE_1D_ARRAY:
        case GL_TEXTURE_2D:
        case GL_TEXTURE_2D_ARRAY:
        case GL_TEXTURE_CUBE_MAP:
        case GL_TEXTURE_CUBE_MAP_ARRAY:
            break;
        default:
            return false;
    }
    if (view_reinterprets(texture_gl, view_format))
        return true;
    return view_target != texture_gl->target || desc->u.texture.layer_idx
            || desc->u.texture.layer_count != texture_gl->t.layer_count;
}

static unsigned int view_copy_level_layers(const struct wined3d_gl_view_copy *copy, GLenum target, unsigned int level)
{
    if (target == GL_TEXTURE_3D)
        return wined3d_texture_get_level_depth(&copy->texture_gl->t, copy->level_idx + level);
    return copy->layer_count;
}

/* Creates the view's texture. Called from the command stream thread. */
void wined3d_gl_view_create_copy(struct wined3d_gl_view *view, struct wined3d_texture_gl *texture_gl,
        const struct wined3d_view_desc *desc, const struct wined3d_format *view_format, GLenum view_target)
{
    const struct wined3d_format_gl *format_gl = wined3d_format_gl(view_format);
    unsigned int i, face, level, width, height, layers;
    GLenum internal, format, type;
    const struct wined3d_gl_info *gl_info;
    struct wined3d_context_gl *context_gl;
    struct wined3d_gl_view_copy *copy;

    if (!(copy = calloc(1, sizeof(*copy))))
        return;
    copy->texture_gl = texture_gl;
    copy->level_idx = desc->u.texture.level_idx;
    copy->level_count = desc->u.texture.level_count;
    if (texture_gl->target == GL_TEXTURE_3D)
    {
        copy->layer_idx = 0;
        copy->layer_count = 1;
    }
    else
    {
        copy->layer_idx = desc->u.texture.layer_idx;
        copy->layer_count = desc->u.texture.layer_count;
    }
    wined3d_gl_texel_format_from_format(&copy->texture_format, texture_gl->t.resource.format);
    if (view_is_depth(texture_gl))
    {
        format_gl = wined3d_format_gl(texture_gl->t.resource.format);
        copy->view_format = copy->texture_format;
    }
    else
    {
        wined3d_gl_texel_format_from_format(&copy->view_format, view_format);
        copy->reinterpret = view_reinterprets(texture_gl, view_format);
    }
    internal = format_gl->internal;
    format = format_gl->format;
    type = format_gl->type;
    if ((view_format->attrs & WINED3D_FORMAT_ATTR_COMPRESSED)
            || (texture_gl->t.resource.format_caps & WINED3D_FORMAT_CAP_DECOMPRESS))
    {
        /* OpenGL can't draw to a compressed texture: the view's texture
         * holds decoded texels, sRGB ones as they are stored. */
        format = GL_RGBA;
        switch (view_format->id)
        {
            case WINED3DFMT_ATI1N:
            case WINED3DFMT_ATI2N:
            case WINED3DFMT_BC4_UNORM:
            case WINED3DFMT_BC4_SNORM:
            case WINED3DFMT_BC5_UNORM:
            case WINED3DFMT_BC5_SNORM:
            case WINED3DFMT_BC6H_UF16:
            case WINED3DFMT_BC6H_SF16:
                internal = GL_RGBA16F;
                type = GL_HALF_FLOAT;
                break;
            default:
                internal = (view_format->caps[texture_gl->t.resource.gl_type] & WINED3D_FORMAT_CAP_SRGB_READ)
                        ? GL_SRGB8_ALPHA8 : GL_RGBA8;
                type = GL_UNSIGNED_BYTE;
                break;
        }
    }

    context_gl = wined3d_context_gl(context_acquire(texture_gl->t.resource.device, NULL, 0));
    gl_info = context_gl->gl_info;

    view->target = view_target;
    gl_info->gl_ops.gl.p_glGenTextures(1, &view->name);
    wined3d_context_gl_bind_texture(context_gl, view_target, view->name);
    for (i = 0; i < copy->level_count; ++i)
    {
        level = copy->level_idx + i;
        width = wined3d_texture_get_level_width(&texture_gl->t, level);
        height = wined3d_texture_get_level_height(&texture_gl->t, level);
        layers = view_copy_level_layers(copy, view_target, i);
        switch (view_target)
        {
            case GL_TEXTURE_1D:
                gl_info->gl_ops.gl.p_glTexImage1D(view_target, i, internal, width, 0,
                        format, type, NULL);
                break;
            case GL_TEXTURE_1D_ARRAY:
                gl_info->gl_ops.gl.p_glTexImage2D(view_target, i, internal, width, layers, 0,
                        format, type, NULL);
                break;
            case GL_TEXTURE_2D_ARRAY:
            case GL_TEXTURE_3D:
            case GL_TEXTURE_CUBE_MAP_ARRAY:
                GL_EXTCALL(glTexImage3D(view_target, i, internal, width, height, layers, 0,
                        format, type, NULL));
                break;
            case GL_TEXTURE_CUBE_MAP:
                for (face = 0; face < 6; ++face)
                    gl_info->gl_ops.gl.p_glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, i,
                            internal, width, height, 0, format, type, NULL);
                break;
            default:
                gl_info->gl_ops.gl.p_glTexImage2D(view_target, i, internal, width, height, 0,
                        format, type, NULL);
                break;
        }
    }
    gl_info->gl_ops.gl.p_glTexParameteri(view_target, GL_TEXTURE_MAX_LEVEL, copy->level_count - 1);
    checkGLcall("create view texture");
    TRACE("View texture %u (target %#x, %s) of levels %u-%u, layers %u-%u of texture %p (%s).\n",
            view->name, view_target, debug_d3dformat(view_format->id), copy->level_idx,
            copy->level_idx + copy->level_count - 1, copy->layer_idx, copy->layer_idx + copy->layer_count - 1,
            texture_gl, debug_d3dformat(texture_gl->t.resource.format->id));

    view->copy = copy;

    context_invalidate_compute_state(&context_gl->c, STATE_COMPUTE_SHADER_RESOURCE_BINDING);
    context_invalidate_state(&context_gl->c, STATE_GRAPHICS_SHADER_RESOURCE_BINDING);
    context_release(&context_gl->c);
}

void wined3d_gl_view_destroy_copy(const struct wined3d_gl_view *view)
{
    free(view->copy);
}

/* Brings the view's texture up to date with the texture, which the caller
 * has loaded. This draws: see wined3d_context_gl_copy_texels(). */
void wined3d_gl_view_load_copy(const struct wined3d_gl_view *view, struct wined3d_context_gl *context_gl)
{
    struct wined3d_gl_view_copy *copy = view->copy;
    struct wined3d_texture_gl *texture_gl = copy->texture_gl;
    struct wined3d_gl_copy_image src, dst;
    unsigned int i, j, layers;

    if (copy->loaded && copy->serial == texture_gl->t.content_serial)
        return;

    src.name = wined3d_texture_gl_get_gl_texture(texture_gl, FALSE)->name;
    src.target = texture_gl->target;
    src.texture_gl = texture_gl;
    dst.name = view->name;
    dst.target = view->target;
    dst.texture_gl = NULL;
    for (i = 0; i < copy->level_count; ++i)
    {
        src.level = copy->level_idx + i;
        dst.level = i;
        layers = view_copy_level_layers(copy, view->target, i);
        for (j = 0; j < layers; ++j)
        {
            src.layer = copy->layer_idx + j;
            dst.layer = j;
            if (!wined3d_context_gl_copy_texels(context_gl, &src, &copy->texture_format, &dst, &copy->view_format,
                    wined3d_texture_get_level_width(&texture_gl->t, src.level),
                    view->target == GL_TEXTURE_1D || view->target == GL_TEXTURE_1D_ARRAY
                    ? 1 : wined3d_texture_get_level_height(&texture_gl->t, src.level), copy->reinterpret)
                    && !copy->loaded)
                FIXME("View texture %u is not updated.\n", view->name);
        }
    }
    copy->serial = texture_gl->t.content_serial;
    copy->loaded = true;
}

/* Brings up to date the texture a graphics shader reads an unordered access
 * view of a texture through, creating it when the view needs one. */
void wined3d_unordered_access_view_gl_load_copy(struct wined3d_unordered_access_view_gl *view_gl,
        struct wined3d_context_gl *context_gl)
{
    struct wined3d_texture_gl *texture_gl = wined3d_texture_gl(texture_from_resource(view_gl->v.resource));
    const struct wined3d_view_desc *desc = &view_gl->v.desc;
    GLenum view_target = texture_gl->target;

    if (!view_gl->gl_view.copy)
    {
        if (view_gl->gl_view.name)
            return;
        if ((desc->flags & WINED3D_VIEW_TEXTURE_ARRAY) && view_target == GL_TEXTURE_2D)
            view_target = GL_TEXTURE_2D_ARRAY;
        else if ((desc->flags & WINED3D_VIEW_TEXTURE_ARRAY) && view_target == GL_TEXTURE_1D)
            view_target = GL_TEXTURE_1D_ARRAY;
        else if (!(desc->flags & WINED3D_VIEW_TEXTURE_ARRAY) && view_target == GL_TEXTURE_2D_ARRAY)
            view_target = GL_TEXTURE_2D;
        else if (!(desc->flags & WINED3D_VIEW_TEXTURE_ARRAY) && view_target == GL_TEXTURE_1D_ARRAY)
            view_target = GL_TEXTURE_1D;
        if (view_is_depth(texture_gl) || (!desc->u.texture.level_idx
                && !wined3d_gl_view_needs_copy(context_gl->gl_info, texture_gl, desc, view_gl->v.format, view_target)))
            return;
        if (texture_gl->target == GL_TEXTURE_3D && !view_reinterprets(texture_gl, view_gl->v.format))
        {
            WARN("Reading level 0 for a view of level %u of a 3D texture.\n", desc->u.texture.level_idx);
            return;
        }
        wined3d_gl_view_create_copy(&view_gl->gl_view, texture_gl, desc, view_gl->v.format, view_target);
        if (!view_gl->gl_view.copy)
            return;
    }
    wined3d_gl_view_load_copy(&view_gl->gl_view, context_gl);
}

/* Generates the mipmaps of a view that has a texture of its own: in that
 * texture, from the view's first level, and copies them to the texture's
 * levels and layers the view selects. */
void wined3d_gl_view_copy_generate_mipmap(const struct wined3d_gl_view *view, struct wined3d_context_gl *context_gl)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_gl_view_copy *copy = view->copy;
    struct wined3d_texture_gl *texture_gl = copy->texture_gl;
    struct wined3d_gl_copy_image src, dst;
    unsigned int i, j, layers;

    wined3d_gl_view_load_copy(view, context_gl);

    wined3d_context_gl_bind_texture(context_gl, view->target, view->name);
    gl_info->fbo_ops.glGenerateMipmap(view->target);
    checkGLcall("generate view mipmaps");
    context_invalidate_compute_state(&context_gl->c, STATE_COMPUTE_SHADER_RESOURCE_BINDING);
    context_invalidate_state(&context_gl->c, STATE_GRAPHICS_SHADER_RESOURCE_BINDING);

    src.name = view->name;
    src.target = view->target;
    src.texture_gl = NULL;
    dst.name = wined3d_texture_gl_get_gl_texture(texture_gl, FALSE)->name;
    dst.target = texture_gl->target;
    dst.texture_gl = texture_gl;
    for (i = 1; i < copy->level_count; ++i)
    {
        src.level = i;
        dst.level = copy->level_idx + i;
        layers = view_copy_level_layers(copy, view->target, i);
        for (j = 0; j < layers; ++j)
        {
            src.layer = j;
            dst.layer = copy->layer_idx + j;
            if (!wined3d_context_gl_copy_texels(context_gl, &src, &copy->view_format, &dst, &copy->texture_format,
                    wined3d_texture_get_level_width(&texture_gl->t, dst.level),
                    view->target == GL_TEXTURE_1D || view->target == GL_TEXTURE_1D_ARRAY
                    ? 1 : wined3d_texture_get_level_height(&texture_gl->t, dst.level), copy->reinterpret))
            {
                FIXME("The mipmaps of view texture %u are not copied to the texture.\n", view->name);
                return;
            }
        }
    }
}

/* The caller has brought the view's texture and the texture in step. */
void wined3d_gl_view_copy_set_current(const struct wined3d_gl_view *view)
{
    view->copy->serial = view->copy->texture_gl->t.content_serial;
    view->copy->loaded = true;
}
