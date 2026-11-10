/*
 * WINESKIN: a frame capture for debugging rendering on the OpenGL backend.
 *
 * With WINESKIN_D3D_FRAME_DUMP set to a directory (a Windows path, such as
 * Z:\tmp\dump), creating a file named "go" in it captures the next frame (a
 * file named "go-indirect" the frame after the next one that dispatches a
 * compute shader with indirect arguments):
 * - frame.txt lists the frame's draws and compute dispatches in order, with
 *   their shaders, the render target, and the textures the pixel or compute
 *   shader reads; draws to one render target in a row are a pass. After each
 *   draw come the samples it passed, the OpenGL draw buffers, color masks,
 *   depth state and program it ran with, and its OpenGL errors;
 * - pNNNN.ppm is the first render target (or the depth buffer) as each pass
 *   left it (pNNNN-K.ppm the other render targets), cNNNN-cbK.bin and
 *   cNNNN-tK.bin the constant buffers and buffer views a dispatch reads (at
 *   most 1 MB of each), cNNNN-tK.ppm its small textures, and cNNNN-uK.ppm an unordered access view after a dispatch, at
 *   most 1920 pixels wide; float targets are tone-mapped (x / (1 + x)) and
 *   gamma-encoded. frame.txt has each image's range, mean and count of
 *   values that are not finite.
 * Reading the targets back waits for the GPU; nothing runs when the variable
 * is not set.
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

#include <stdio.h>
#include <math.h>
#include "wined3d_private.h"
#include "wined3d_gl.h"

struct dump_target
{
    GLuint name;
    GLenum target;
    unsigned int level, layer, width, height;
    bool depth, integer;
    enum wined3d_format_id format;
};

static struct
{
    int enabled; /* 0: not looked up, 1: on, -1: off */
    char dir[MAX_PATH];
    bool active;
    /* Waiting for a frame with an indirect dispatch, and having seen one. */
    bool armed, triggered;
    FILE *log;
    unsigned int pass, draw, dispatch;
    struct dump_target target;
    /* The pass's other render targets. */
    struct dump_target targets[WINED3D_MAX_RENDER_TARGETS];
    const struct wined3d_shader *last_ps;
    GLuint query;
    bool query_active;
} dump;

bool wined3d_gl_frame_dump_active;

/* A name for a shader that is the same in every run (FNV-1a of its byte code). */
unsigned int wined3d_shader_byte_code_hash(const struct wined3d_shader *shader)
{
    const uint8_t *data = (const uint8_t *)shader->byte_code;
    unsigned int hash = 0x811c9dc5, i;

    if (!data)
        return 0;
    for (i = 0; i < shader->byte_code_size; ++i)
        hash = (hash ^ data[i]) * 0x01000193;
    return hash;
}

static bool dump_enabled(void)
{
    const char *env;

    if (!dump.enabled)
    {
        if ((env = getenv("WINESKIN_D3D_FRAME_DUMP")) && *env && strlen(env) < MAX_PATH - 32)
        {
            strcpy(dump.dir, env);
            dump.enabled = 1;
        }
        else
        {
            dump.enabled = -1;
        }
    }
    return dump.enabled > 0;
}

static void dump_get_target(struct dump_target *target, struct wined3d_resource *resource,
        unsigned int level, unsigned int layer, const struct wined3d_format *format)
{
    struct wined3d_texture_gl *texture_gl;

    memset(target, 0, sizeof(*target));
    if (!resource || resource->type == WINED3D_RTYPE_BUFFER)
        return;
    texture_gl = wined3d_texture_gl(texture_from_resource(resource));
    target->name = texture_gl->texture_rgb.name ? texture_gl->texture_rgb.name : texture_gl->texture_srgb.name;
    target->target = texture_gl->target;
    target->level = level;
    target->layer = layer;
    target->width = wined3d_texture_get_level_width(&texture_gl->t, level);
    target->height = wined3d_texture_get_level_height(&texture_gl->t, level);
    target->depth = format->depth_size || format->stencil_size;
    target->integer = !!(format->attrs & WINED3D_FORMAT_ATTR_INTEGER);
    target->format = format->id;
}

static void dump_view_target(struct dump_target *target, const struct wined3d_rendertarget_view *view)
{
    struct wined3d_texture *texture;

    memset(target, 0, sizeof(*target));
    if (!view || view->resource->type == WINED3D_RTYPE_BUFFER)
        return;
    texture = texture_from_resource(view->resource);
    dump_get_target(target, view->resource, view->sub_resource_idx % texture->level_count,
            view->resource->type == WINED3D_RTYPE_TEXTURE_3D ? 0
            : view->sub_resource_idx / texture->level_count, view->format);
}

/* Reads a texture level's layer back and writes it as "name".ppm. */
static void dump_write(struct wined3d_context_gl *context_gl, const struct dump_target *target, const char *name)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    unsigned int x, y, c, step, w, h, channels, bad = 0;
    float min[4], max[4], *data, *p;
    char path[MAX_PATH + 64];
    GLenum attachment, status;
    double sum[4] = {0};
    uint8_t *row;
    GLuint fbo;
    FILE *f;

    if (!target->name || !target->width || !target->height)
        return;
    if (target->target == GL_TEXTURE_2D_MULTISAMPLE || target->target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY)
    {
        fprintf(dump.log, "  %s: multisample texture %u, not read\n", name, target->name);
        return;
    }
    channels = target->depth ? 1 : 4;
    if (!(data = malloc((size_t)target->width * target->height * channels * sizeof(*data))))
        return;

    attachment = target->depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;
    gl_info->fbo_ops.glGenFramebuffers(1, &fbo);
    gl_info->fbo_ops.glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    switch (target->target)
    {
        case GL_TEXTURE_2D_ARRAY:
        case GL_TEXTURE_3D:
        case GL_TEXTURE_1D_ARRAY:
        case GL_TEXTURE_CUBE_MAP_ARRAY:
            gl_info->fbo_ops.glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, attachment,
                    target->name, target->level, target->layer);
            break;
        case GL_TEXTURE_CUBE_MAP:
            gl_info->fbo_ops.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, attachment,
                    GL_TEXTURE_CUBE_MAP_POSITIVE_X + target->layer % 6, target->name, target->level);
            break;
        default:
            gl_info->fbo_ops.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, attachment,
                    target->target, target->name, target->level);
            break;
    }
    gl_info->gl_ops.gl.p_glReadBuffer(target->depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
    status = gl_info->fbo_ops.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        fprintf(dump.log, "  %s: texture %u (target %#x, level %u, layer %u) is not readable, status %#x\n",
                name, target->name, target->target, target->level, target->layer, status);
    }
    else
    {
        wined3d_context_gl_bind_bo(context_gl, GL_PIXEL_PACK_BUFFER, 0);
        gl_info->gl_ops.gl.p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
        gl_info->gl_ops.gl.p_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        if (target->depth)
        {
            gl_info->gl_ops.gl.p_glReadPixels(0, 0, target->width, target->height,
                    GL_DEPTH_COMPONENT, GL_FLOAT, data);
        }
        else if (target->integer)
        {
            /* Integer texels are read as integers and shown as value / 255. */
            int32_t *ints = (int32_t *)data;
            size_t i, count = (size_t)target->width * target->height * 4;

            gl_info->gl_ops.gl.p_glReadPixels(0, 0, target->width, target->height,
                    GL_RGBA_INTEGER, GL_INT, data);
            for (i = 0; i < count; ++i)
                data[i] = ints[i] / 255.0f;
        }
        else
        {
            gl_info->gl_ops.gl.p_glReadPixels(0, 0, target->width, target->height, GL_RGBA, GL_FLOAT, data);
        }
        while (gl_info->gl_ops.gl.p_glGetError());

        for (c = 0; c < channels; ++c)
        {
            min[c] = INFINITY;
            max[c] = -INFINITY;
        }
        for (p = data, y = 0; y < target->height; ++y)
        {
            for (x = 0; x < target->width; ++x)
            {
                for (c = 0; c < channels; ++c, ++p)
                {
                    if (!isfinite(*p))
                    {
                        ++bad;
                        continue;
                    }
                    if (*p < min[c]) min[c] = *p;
                    if (*p > max[c]) max[c] = *p;
                    sum[c] += *p;
                }
            }
        }
        fprintf(dump.log, "  %s: texture %u %s %ux%u level %u layer %u, not finite %u;", name, target->name,
                debug_d3dformat(target->format), target->width, target->height, target->level, target->layer, bad);
        for (c = 0; c < channels; ++c)
            fprintf(dump.log, " [%g %g mean %g]", min[c], max[c],
                    sum[c] / ((double)target->width * target->height));
        fprintf(dump.log, "\n");

        step = (target->width + 1919) / 1920;
        w = target->width / step;
        h = target->height / step;
        sprintf(path, "%s\\%s.ppm", dump.dir, name);
        if (w && h && (row = malloc(w * 3)) && (f = fopen(path, "wb")))
        {
            bool tonemap = !target->depth && !target->integer && max[0] > 1.001f;

            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (y = 0; y < h; ++y)
            {
                for (x = 0; x < w; ++x)
                {
                    p = &data[((size_t)y * step * target->width + x * step) * channels];
                    for (c = 0; c < 3; ++c)
                    {
                        float v = p[channels == 1 ? 0 : c];

                        if (!isfinite(v))
                            v = c ? 0.0f : 1.0f;
                        else if (v < 0.0f)
                            v = 0.0f;
                        if (tonemap)
                            v = powf(v / (1.0f + v), 1.0f / 2.2f);
                        else if (v > 1.0f)
                            v = 1.0f;
                        row[x * 3 + c] = (uint8_t)(v * 255.0f + 0.5f);
                    }
                }
                fwrite(row, 1, w * 3, f);
            }
            fclose(f);
            free(row);
        }
    }

    gl_info->fbo_ops.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl_info->fbo_ops.glDeleteFramebuffers(1, &fbo);
    while (gl_info->gl_ops.gl.p_glGetError());
    context_invalidate_state(&context_gl->c, STATE_FRAMEBUFFER);
    free(data);
}

/* Prints the first words of a buffer's OpenGL buffer object. */
static void dump_buffer_words(struct wined3d_context_gl *context_gl, struct wined3d_buffer *buffer,
        unsigned int offset, unsigned int count)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_bo_gl *bo_gl;
    uint32_t words[32] = {0};
    unsigned int i;

    if (!buffer->buffer_object)
    {
        fprintf(dump.log, " (no buffer object)\n");
        return;
    }
    bo_gl = wined3d_bo_gl(buffer->buffer_object);
    count = min(count, ARRAY_SIZE(words));
    if (offset >= buffer->resource.size)
        count = 0;
    count = min(count, (buffer->resource.size - offset) / 4);
    wined3d_context_gl_bind_bo(context_gl, bo_gl->binding, bo_gl->id);
    GL_EXTCALL(glGetBufferSubData(bo_gl->binding, bo_gl->b.buffer_offset + offset, count * 4, words));
    wined3d_context_gl_bind_bo(context_gl, bo_gl->binding, 0);
    while (gl_info->gl_ops.gl.p_glGetError());
    fprintf(dump.log, " bo %u +%u:", bo_gl->id, offset);
    for (i = 0; i < count; ++i)
        fprintf(dump.log, " %#x", words[i]);
    fprintf(dump.log, "\n");
}

/* Writes up to 1 MB of a buffer's OpenGL buffer object to "name".bin. */
static void dump_buffer_file(struct wined3d_context_gl *context_gl, struct wined3d_buffer *buffer,
        unsigned int offset, const char *name)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_bo_gl *bo_gl;
    char path[MAX_PATH + 64];
    unsigned int size;
    void *data;
    FILE *f;

    if (!buffer->buffer_object || offset >= buffer->resource.size)
        return;
    size = min(buffer->resource.size - offset, 0x100000);
    if (!(data = malloc(size)))
        return;
    bo_gl = wined3d_bo_gl(buffer->buffer_object);
    wined3d_context_gl_bind_bo(context_gl, bo_gl->binding, bo_gl->id);
    GL_EXTCALL(glGetBufferSubData(bo_gl->binding, bo_gl->b.buffer_offset + offset, size, data));
    wined3d_context_gl_bind_bo(context_gl, bo_gl->binding, 0);
    while (gl_info->gl_ops.gl.p_glGetError());
    sprintf(path, "%s\\%s.bin", dump.dir, name);
    if ((f = fopen(path, "wb")))
    {
        fwrite(data, 1, size, f);
        fclose(f);
    }
    free(data);
}

static void dump_end_pass(struct wined3d_context_gl *context_gl)
{
    unsigned int i;
    char name[32];

    if (!dump.target.name)
        return;
    sprintf(name, "p%04u", dump.pass);
    dump_write(context_gl, &dump.target, name);
    for (i = 1; i < ARRAY_SIZE(dump.targets); ++i)
    {
        sprintf(name, "p%04u-%u", dump.pass, i);
        dump_write(context_gl, &dump.targets[i], name);
    }
    memset(&dump.target, 0, sizeof(dump.target));
    memset(dump.targets, 0, sizeof(dump.targets));
    ++dump.pass;
}

static void dump_shader_resources(const struct wined3d_state *state, enum wined3d_shader_type type)
{
    const struct wined3d_shader *shader = state->shader[type];
    unsigned int i, count = 0;

    if (!shader)
        return;
    for (i = 0; i < ARRAY_SIZE(shader->reg_maps.resource_info); ++i)
    {
        const struct wined3d_shader_resource_view *view = state->shader_resource_view[type][i];
        struct wined3d_texture_gl *texture_gl;

        if (!shader->reg_maps.resource_info[i].type)
            continue;
        ++count;
        if (!view)
        {
            fprintf(dump.log, "      t%u: none\n", i);
        }
        else if (view->resource->type == WINED3D_RTYPE_BUFFER)
        {
            fprintf(dump.log, "      t%u: buffer %p %s, %u bytes\n", i, view->resource,
                    debug_d3dformat(view->format->id), view->resource->size);
        }
        else
        {
            texture_gl = wined3d_texture_gl(texture_from_resource(view->resource));
            fprintf(dump.log, "      t%u: texture %u (view %u) %s as %s, %ux%ux%u, target %#x, levels %u+%u,"
                    " layers %u+%u\n", i,
                    texture_gl->texture_rgb.name ? texture_gl->texture_rgb.name : texture_gl->texture_srgb.name,
                    wined3d_shader_resource_view_gl((struct wined3d_shader_resource_view *)view)->gl_view.name,
                    debug_d3dformat(view->resource->format->id), debug_d3dformat(view->format->id),
                    texture_gl->t.resource.width, texture_gl->t.resource.height, texture_gl->t.resource.depth,
                    texture_gl->target, view->desc.u.texture.level_idx, view->desc.u.texture.level_count,
                    view->desc.u.texture.layer_idx, view->desc.u.texture.layer_count);
        }
    }
    fprintf(dump.log, "      (%u resources declared)\n", count);
}

void wined3d_gl_frame_dump_draw(struct wined3d_context_gl *context_gl, const struct wined3d_state *state,
        const struct wined3d_draw_parameters *parameters)
{
    const struct wined3d_rendertarget_view *view;
    struct dump_target target;
    unsigned int i;

    if (!dump.active)
        return;

    if (!(view = state->fb.render_targets[0]))
        view = state->fb.depth_stencil;
    dump_view_target(&target, view);
    if (target.name != dump.target.name || target.level != dump.target.level || target.layer != dump.target.layer)
    {
        dump_end_pass(context_gl);
        dump.target = target;
        dump.last_ps = (void *)~(ULONG_PTR)0;
        fprintf(dump.log, "pass %u: target texture %u %s %ux%u level %u layer %u; targets", dump.pass,
                target.name, debug_d3dformat(target.format), target.width, target.height, target.level,
                target.layer);
        for (i = 0; i < ARRAY_SIZE(state->fb.render_targets); ++i)
        {
            if (i)
                dump_view_target(&dump.targets[i], state->fb.render_targets[i]);
            if ((view = state->fb.render_targets[i]) && view->resource->type != WINED3D_RTYPE_BUFFER)
                fprintf(dump.log, " %u:%u(%s)", i,
                        wined3d_texture_gl(texture_from_resource(view->resource))->texture_rgb.name,
                        debug_d3dformat(view->format->id));
        }
        if ((view = state->fb.depth_stencil) && view->resource->type != WINED3D_RTYPE_BUFFER)
            fprintf(dump.log, " depth:%u(%s)",
                    wined3d_texture_gl(texture_from_resource(view->resource))->texture_rgb.name,
                    debug_d3dformat(view->format->id));
        fprintf(dump.log, "\n");
    }

    fprintf(dump.log, "  draw %u: vs %p hs %p gs %p ps %p", dump.draw++, state->shader[WINED3D_SHADER_TYPE_VERTEX],
            state->shader[WINED3D_SHADER_TYPE_HULL], state->shader[WINED3D_SHADER_TYPE_GEOMETRY],
            state->shader[WINED3D_SHADER_TYPE_PIXEL]);
    if (parameters->indirect)
        fprintf(dump.log, " indirect\n");
    else
        fprintf(dump.log, " %s%u x%u\n", parameters->indexed ? "indexed " : "",
                parameters->u.direct.index_count, parameters->u.direct.instance_count);
    if (state->shader[WINED3D_SHADER_TYPE_PIXEL] != dump.last_ps)
    {
        dump.last_ps = state->shader[WINED3D_SHADER_TYPE_PIXEL];
        dump_shader_resources(state, WINED3D_SHADER_TYPE_PIXEL);
    }

    /* Count the samples the draw passes, unless the application is counting. */
    {
        const struct wined3d_gl_info *gl_info = context_gl->gl_info;
        GLint current[2] = {0};

        GL_EXTCALL(glGetQueryiv(GL_SAMPLES_PASSED, GL_CURRENT_QUERY, &current[0]));
        GL_EXTCALL(glGetQueryiv(GL_ANY_SAMPLES_PASSED, GL_CURRENT_QUERY, &current[1]));
        while (gl_info->gl_ops.gl.p_glGetError());
        if (dump.query_active)
        {
            /* The previous draw was skipped. */
            GL_EXTCALL(glEndQuery(GL_SAMPLES_PASSED));
            dump.query_active = false;
            fprintf(dump.log, "    -> skipped\n");
        }
        else if (!current[0] && !current[1])
        {
            if (!dump.query)
                GL_EXTCALL(glGenQueries(1, &dump.query));
            GL_EXTCALL(glBeginQuery(GL_SAMPLES_PASSED, dump.query));
            dump.query_active = true;
        }
    }
}

void wined3d_gl_frame_dump_draw_done(struct wined3d_context_gl *context_gl, const struct wined3d_state *state)
{
    const struct wined3d_shader *ps = state->shader[WINED3D_SHADER_TYPE_PIXEL];
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    GLint value, program = 0, func = 0;
    GLboolean mask[4], depth_mask = 0;
    unsigned int i;
    GLenum error;

    if (!dump.active)
        return;

    fprintf(dump.log, "    ->");
    if (dump.query_active)
    {
        GLuint samples = 0;

        GL_EXTCALL(glEndQuery(GL_SAMPLES_PASSED));
        GL_EXTCALL(glGetQueryObjectuiv(dump.query, GL_QUERY_RESULT, &samples));
        dump.query_active = false;
        fprintf(dump.log, " samples %u;", samples);
    }
    fprintf(dump.log, " ps rt_mask %#x; buffers", ps ? (unsigned int)ps->reg_maps.rt_mask : 0);
    for (i = 0; i < 8; ++i)
    {
        value = 0;
        gl_info->gl_ops.gl.p_glGetIntegerv(GL_DRAW_BUFFER0 + i, &value);
        memset(mask, 0, sizeof(mask));
        GL_EXTCALL(glGetBooleani_v(GL_COLOR_WRITEMASK, i, mask));
        if (value)
            fprintf(dump.log, " %u:%#x/%u%u%u%u", i, value, mask[0], mask[1], mask[2], mask[3]);
    }
    gl_info->gl_ops.gl.p_glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl_info->gl_ops.gl.p_glGetIntegerv(GL_DEPTH_FUNC, &func);
    gl_info->gl_ops.gl.p_glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    fprintf(dump.log, "; program %d, depth %s func %#x write %u, stencil %u, blend %u, discard %u, srgb %u",
            program, gl_info->gl_ops.gl.p_glIsEnabled(GL_DEPTH_TEST) ? "on" : "off", func, depth_mask,
            gl_info->gl_ops.gl.p_glIsEnabled(GL_STENCIL_TEST), gl_info->gl_ops.gl.p_glIsEnabled(GL_BLEND),
            gl_info->gl_ops.gl.p_glIsEnabled(GL_RASTERIZER_DISCARD),
            gl_info->gl_ops.gl.p_glIsEnabled(GL_FRAMEBUFFER_SRGB));
    i = 0;
    while ((error = gl_info->gl_ops.gl.p_glGetError()))
    {
        fprintf(dump.log, ", GL error %#x", error);
        ++i;
    }
    fprintf(dump.log, "\n");
    if (i && program)
    {
        GLint link = 0, valid = 0;
        char info[1024];

        info[0] = 0;
        GL_EXTCALL(glValidateProgram(program));
        GL_EXTCALL(glGetProgramiv(program, GL_LINK_STATUS, &link));
        GL_EXTCALL(glGetProgramiv(program, GL_VALIDATE_STATUS, &valid));
        GL_EXTCALL(glGetProgramInfoLog(program, sizeof(info), NULL, info));
        fprintf(dump.log, "    program %d: link status %d, validate status %d: %s\n", program, link, valid, info);
        while (gl_info->gl_ops.gl.p_glGetError());
    }
}

void wined3d_gl_frame_dump_dispatch(struct wined3d_context_gl *context_gl, const struct wined3d_state *state,
        const struct wined3d_dispatch_parameters *parameters, bool done)
{
    const struct wined3d_shader *shader = state->shader[WINED3D_SHADER_TYPE_COMPUTE];
    struct dump_target target;
    char name[32];
    unsigned int i;

    if (dump.armed && parameters->indirect)
        dump.triggered = true;
    if (!dump.active || !shader)
        return;

    if (!done)
    {
        dump_end_pass(context_gl);
        fprintf(dump.log, "dispatch %u: cs %p (%08x)", dump.dispatch, shader, wined3d_shader_byte_code_hash(shader));
        if (parameters->indirect)
        {
            fprintf(dump.log, " indirect, buffer %p", parameters->u.indirect.buffer);
            wined3d_cl_finish();
            dump_buffer_words(context_gl, parameters->u.indirect.buffer, parameters->u.indirect.offset, 3);
        }
        else
            fprintf(dump.log, " %ux%ux%u groups\n", parameters->u.direct.group_count_x,
                    parameters->u.direct.group_count_y, parameters->u.direct.group_count_z);
        dump_shader_resources(state, WINED3D_SHADER_TYPE_COMPUTE);
        for (i = 0; i < WINED3D_MAX_CBS; ++i)
        {
            const struct wined3d_constant_buffer_state *cb = &state->cb[WINED3D_SHADER_TYPE_COMPUTE][i];

            if (!shader->reg_maps.cb_sizes[i] || !cb->buffer)
                continue;
            fprintf(dump.log, "      cb%u: buffer %p, %u bytes, offset %u, locations %#x;", i, cb->buffer,
                    cb->buffer->resource.size, cb->offset, (unsigned int)cb->buffer->locations);
            dump_buffer_words(context_gl, cb->buffer, cb->offset, 16);
            fprintf(dump.log, "      cb%u words 112..:", i);
            dump_buffer_words(context_gl, cb->buffer, cb->offset + 448, 16);
        }
        return;
    }

    wined3d_cl_finish();
    /* Small textures the shader reads, and the shadow textures OpenCL reads in their place. */
    for (i = 0; i < ARRAY_SIZE(shader->reg_maps.resource_info); ++i)
    {
        const struct wined3d_shader_resource_view *view = state->shader_resource_view[WINED3D_SHADER_TYPE_COMPUTE][i];
        struct dump_target shadow;

        if (shader->reg_maps.resource_info[i].type <= WINED3D_SHADER_RESOURCE_BUFFER || !view
                || view->resource->type == WINED3D_RTYPE_BUFFER)
            continue;
        dump_get_target(&target, view->resource, view->desc.u.texture.level_idx, view->desc.u.texture.layer_idx,
                view->format);
        shadow = target;
        if ((shadow.name = wined3d_cl_get_shadow_name(context_gl->c.device, target.name, target.level,
                &shadow.target)))
        {
            shadow.level = 0;
            shadow.depth = false;
            sprintf(name, "c%04u-t%u-shadow", dump.dispatch, i);
            dump_write(context_gl, &shadow, name);
        }
        if (target.width * target.height <= 4096)
        {
            sprintf(name, "c%04u-t%u", dump.dispatch, i);
            dump_write(context_gl, &target, name);
        }
    }
    for (i = 0; i < WINED3D_MAX_CBS; ++i)
    {
        const struct wined3d_constant_buffer_state *cb = &state->cb[WINED3D_SHADER_TYPE_COMPUTE][i];

        if (!shader->reg_maps.cb_sizes[i] || !cb->buffer)
            continue;
        fprintf(dump.log, "      cb%u after:", i);
        dump_buffer_words(context_gl, cb->buffer, cb->offset, 16);
        sprintf(name, "c%04u-cb%u", dump.dispatch, i);
        dump_buffer_file(context_gl, cb->buffer, cb->offset, name);
    }
    for (i = 0; i < ARRAY_SIZE(shader->reg_maps.resource_info); ++i)
    {
        const struct wined3d_shader_resource_view *view = state->shader_resource_view[WINED3D_SHADER_TYPE_COMPUTE][i];

        if (!shader->reg_maps.resource_info[i].type || !view || view->resource->type != WINED3D_RTYPE_BUFFER)
            continue;
        sprintf(name, "c%04u-t%u", dump.dispatch, i);
        dump_buffer_file(context_gl, buffer_from_resource(view->resource), 0, name);
    }
    for (i = 0; i < 16; ++i)
    {
        const struct wined3d_sampler *sampler = state->sampler[WINED3D_SHADER_TYPE_COMPUTE][i];

        if (sampler)
            fprintf(dump.log, "      s%u: address %u %u %u, filter min %u mag %u mip %u, compare %u\n", i,
                    sampler->desc.address_u, sampler->desc.address_v, sampler->desc.address_w,
                    sampler->desc.min_filter, sampler->desc.mag_filter, sampler->desc.mip_filter,
                    (unsigned int)sampler->desc.comparison_func);
    }
    for (i = 0; i < MAX_UNORDERED_ACCESS_VIEWS; ++i)
    {
        const struct wined3d_unordered_access_view *view = state->unordered_access_view[WINED3D_PIPELINE_COMPUTE][i];

        if (!shader->reg_maps.uav_resource_info[i].type || !view)
            continue;
        if (view->resource->type == WINED3D_RTYPE_BUFFER)
        {
            fprintf(dump.log, "      u%u: buffer %p %s, %u bytes;", i, view->resource,
                    debug_d3dformat(view->format->id), view->resource->size);
            dump_buffer_words(context_gl, buffer_from_resource(view->resource), 0, 32);
            continue;
        }
        dump_get_target(&target, view->resource, view->desc.u.texture.level_idx,
                view->resource->type == WINED3D_RTYPE_TEXTURE_3D
                ? wined3d_texture_get_level_depth(texture_from_resource(view->resource),
                view->desc.u.texture.level_idx) / 2 : view->desc.u.texture.layer_idx, view->format);
        target.depth = false;
        sprintf(name, "c%04u-u%u", dump.dispatch, i);
        dump_write(context_gl, &target, name);
    }
    ++dump.dispatch;
}

void wined3d_gl_frame_dump_present(struct wined3d_context_gl *context_gl, const struct wined3d_swapchain *swapchain,
        const RECT *src_rect, const RECT *dst_rect)
{
    const struct wined3d_swapchain_desc *desc = &swapchain->state.desc;
    const struct wined3d_texture *back_buffer = swapchain->back_buffers[0];
    char path[MAX_PATH + 64];
    RECT client, context_client;
    FILE *f;

    if (!dump_enabled())
        return;

    if (dump.active)
    {
        dump_end_pass(context_gl);
        /* What the present blits from and to, and the windows' sizes it goes by. */
        GetClientRect(swapchain->win_handle, &client);
        GetClientRect(context_gl->window, &context_client);
        fprintf(dump.log, "present src %s dst %s, swap chain window %p client %s, context window %p client %s, "
                "back buffer %ux%u, desc %ux%u, windowed %d, swap effect %#x\n",
                wine_dbgstr_rect(src_rect), wine_dbgstr_rect(dst_rect), swapchain->win_handle,
                wine_dbgstr_rect(&client), context_gl->window, wine_dbgstr_rect(&context_client),
                back_buffer->resource.width, back_buffer->resource.height, desc->backbuffer_width,
                desc->backbuffer_height, desc->windowed, desc->swap_effect);
        fprintf(dump.log, "present\n");
        fclose(dump.log);
        dump.log = NULL;
        dump.active = wined3d_gl_frame_dump_active = false;
        return;
    }

    if (dump.armed)
    {
        if (!dump.triggered)
            return;
        dump.armed = dump.triggered = false;
    }
    else
    {
        sprintf(path, "%s\\go", dump.dir);
        if (!(f = fopen(path, "rb")))
        {
            sprintf(path, "%s\\go-indirect", dump.dir);
            if (!(f = fopen(path, "rb")))
                return;
            fclose(f);
            remove(path);
            dump.armed = wined3d_gl_frame_dump_active = true;
            return;
        }
        fclose(f);
        remove(path);
    }
    sprintf(path, "%s\\frame.txt", dump.dir);
    if (!(dump.log = fopen(path, "w")))
        return;
    dump.pass = dump.draw = dump.dispatch = 0;
    memset(&dump.target, 0, sizeof(dump.target));
    dump.active = wined3d_gl_frame_dump_active = true;
}
