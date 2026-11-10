/*
 * WINESKIN: stream output without transform feedback.
 *
 * On macOS 10.9, glEndTransformFeedback() and glPauseTransformFeedback()
 * wait until the GPU has finished every command submitted before them
 * (~/WineskinTests/perf/tools/xfbstall.c), in any context of the share
 * group. A game that streams out vertices once a frame (Unity skins meshes
 * that way) then waits each frame for the GPU to finish the frame before.
 *
 * A draw whose vertex shader outputs are streamed out with no geometry shader
 * of the game's and no rasterization is run here instead with a geometry
 * shader that writes each captured component, as its bits, to a pixel of a
 * 4096-wide R32UI target, in the order the components take in the buffer;
 * glReadPixels() then copies the rows into the stream output buffer bound as
 * the pixel pack buffer. Both are queued like any other command, and nothing
 * waits. Draws this can't do (geometry shaders of the game's, several output
 * buffers, gaps in the layout, instancing, indirect draws, strips) use
 * transform feedback.
 *
 * WINE_D3D_CONFIG=StreamOutputEmulation=n (or HKCU\Software\Wine\Direct3D)
 * turns it off.
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

#define SO_EMU_WIDTH 4096
#define SO_EMU_MAX_HEIGHT 4096
/* A geometry shader may emit 1024 components; each point here takes five. */
#define SO_EMU_MAX_EMITS 200

static unsigned int so_emu_vertices_per_primitive(enum wined3d_primitive_type type)
{
    switch (type)
    {
        case WINED3D_PT_POINTLIST: return 1;
        case WINED3D_PT_LINELIST: return 2;
        case WINED3D_PT_TRIANGLELIST: return 3;
        default: return 0;
    }
}

static GLuint so_emu_compile(const struct wined3d_gl_info *gl_info, GLenum type, const char *source)
{
    GLint status;
    GLuint id;

    id = GL_EXTCALL(glCreateShader(type));
    GL_EXTCALL(glShaderSource(id, 1, &source, NULL));
    GL_EXTCALL(glCompileShader(id));
    GL_EXTCALL(glGetShaderiv(id, GL_COMPILE_STATUS, &status));
    if (!status)
    {
        char log[1024];

        GL_EXTCALL(glGetShaderInfoLog(id, sizeof(log), NULL, log));
        ERR("Failed to compile a stream output shader: %s\n", debugstr_a(log));
        GL_EXTCALL(glDeleteShader(id));
        return 0;
    }
    return id;
}

/* The program that runs the vertex shader vs_id of vshader with the stream
 * output layout of gshader, a geometry shader without code of its own, for
 * draws of the given primitive type. Returns 0 if the layout can't be done
 * here. */
GLuint wined3d_so_emu_build_program(const struct wined3d_context_gl *context_gl, GLuint vs_id,
        const struct wined3d_shader *vshader, const struct wined3d_shader *gshader,
        enum wined3d_primitive_type primitive_type, unsigned int *components, GLint *scale_location)
{
    static const char swizzle[] = "xyzw";
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    const struct wined3d_stream_output_desc *so_desc;
    struct wined3d_string_buffer buffer;
    unsigned int verts, count, i, j, v, idx;
    unsigned int register_idx, component_idx;
    GLuint program, gs_id, fs_id;
    GLint status;

    if (!gshader || gshader->function || !(so_desc = gshader->u.gs.so_desc)
            || so_desc->rasterizer_stream_idx != WINED3D_NO_RASTERIZER_STREAM)
        return 0;
    if (!(verts = so_emu_vertices_per_primitive(primitive_type)))
        return 0;

    count = 0;
    for (i = 0; i < so_desc->element_count; ++i)
    {
        const struct wined3d_stream_output_element *e = &so_desc->elements[i];

        if (e->stream_idx || e->output_slot || !e->semantic_name
                || !shader_get_stream_output_register_info(gshader, e, &register_idx, &component_idx)
                || component_idx + e->component_count > 4)
            return 0;
        count += e->component_count;
    }
    if (!count || (so_desc->buffer_stride_count && so_desc->buffer_strides[0]
            && so_desc->buffer_strides[0] != count * 4) || verts * count > SO_EMU_MAX_EMITS)
        return 0;

    if (!string_buffer_init(&buffer))
        return 0;
    shader_addline(&buffer, "#version 330\n");
    shader_addline(&buffer, "layout(%s) in;\n", verts == 1 ? "points" : verts == 2 ? "lines" : "triangles");
    shader_addline(&buffer, "layout(points, max_vertices = %u) out;\n", verts * count);
    shader_addline(&buffer, "in shader_in_out { vec4 reg[%u]; } shader_in[];\n", gshader->limits->packed_input);
    shader_addline(&buffer, "uniform vec2 so_emu_scale;\n");
    shader_addline(&buffer, "flat out uint so_emu_value;\n");
    shader_addline(&buffer, "void so_emu_emit(int i, float v)\n{\n");
    shader_addline(&buffer, "    gl_Position = vec4((vec2(i %% %u, i / %u) + 0.5) * so_emu_scale - 1.0, 0.0, 1.0);\n",
            SO_EMU_WIDTH, SO_EMU_WIDTH);
    shader_addline(&buffer, "    gl_PointSize = 1.0;\n");
    shader_addline(&buffer, "    so_emu_value = floatBitsToUint(v);\n");
    shader_addline(&buffer, "    EmitVertex();\n    EndPrimitive();\n}\n");
    shader_addline(&buffer, "void main()\n{\n    int b = gl_PrimitiveIDIn * %u;\n", verts * count);
    idx = 0;
    for (v = 0; v < verts; ++v)
    {
        for (i = 0; i < so_desc->element_count; ++i)
        {
            const struct wined3d_stream_output_element *e = &so_desc->elements[i];

            shader_get_stream_output_register_info(gshader, e, &register_idx, &component_idx);
            for (j = 0; j < e->component_count; ++j)
                shader_addline(&buffer, "    so_emu_emit(b + %u, shader_in[%u].reg[%u].%c);\n",
                        idx++, v, register_idx, swizzle[component_idx + j]);
        }
    }
    shader_addline(&buffer, "}\n");
    gs_id = so_emu_compile(gl_info, GL_GEOMETRY_SHADER, buffer.buffer);
    string_buffer_free(&buffer);
    if (!gs_id)
        return 0;
    if (!(fs_id = so_emu_compile(gl_info, GL_FRAGMENT_SHADER, "#version 330\n"
            "flat in uint so_emu_value;\nlayout(location = 0) out uint so_emu_out;\n"
            "void main()\n{\n    so_emu_out = so_emu_value;\n}\n")))
    {
        GL_EXTCALL(glDeleteShader(gs_id));
        return 0;
    }

    program = GL_EXTCALL(glCreateProgram());
    GL_EXTCALL(glAttachShader(program, vs_id));
    GL_EXTCALL(glAttachShader(program, gs_id));
    GL_EXTCALL(glAttachShader(program, fs_id));
    if (vshader)
    {
        /* As shader_glsl_link_program() binds them when the shader does not
         * place its inputs itself. */
        char name[32];
        uint32_t map = vshader->reg_maps.input_registers;
        unsigned int location;

        while (map)
        {
            i = wined3d_bit_scan(&map);
            location = wined3d_vs_attrib_location(vshader, gl_info->limits.vertex_attribs, i);
            if (location >= gl_info->limits.vertex_attribs)
                continue;
            sprintf(name, "vs_in%u", i);
            GL_EXTCALL(glBindAttribLocation(program, location, name));
            sprintf(name, "vs_in_uint%u", i);
            GL_EXTCALL(glBindAttribLocation(program, location, name));
            sprintf(name, "vs_in_int%u", i);
            GL_EXTCALL(glBindAttribLocation(program, location, name));
        }
    }
    GL_EXTCALL(glLinkProgram(program));
    GL_EXTCALL(glDeleteShader(gs_id));
    GL_EXTCALL(glDeleteShader(fs_id));
    GL_EXTCALL(glGetProgramiv(program, GL_LINK_STATUS, &status));
    if (!status)
    {
        char log[1024];

        GL_EXTCALL(glGetProgramInfoLog(program, sizeof(log), NULL, log));
        ERR("Failed to link a stream output program: %s\n", debugstr_a(log));
        GL_EXTCALL(glDeleteProgram(program));
        return 0;
    }
    checkGLcall("build stream output program");

    *components = count * verts;
    *scale_location = GL_EXTCALL(glGetUniformLocation(program, "so_emu_scale"));
    TRACE("Program %u streams out %u components a primitive.\n", program, count * verts);
    return program;
}


static bool so_emu_prepare_target(struct wined3d_context_gl *context_gl, unsigned int rows)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    unsigned int height;

    if (context_gl->so_emu_height >= rows)
        return true;

    for (height = max(context_gl->so_emu_height, 16); height < rows; height *= 2) ;
    if (height > SO_EMU_MAX_HEIGHT)
        return false;

    /* A renderbuffer: binding a texture would change the draw's own bindings. */
    if (!context_gl->so_emu_renderbuffer)
        gl_info->fbo_ops.glGenRenderbuffers(1, &context_gl->so_emu_renderbuffer);
    if (!context_gl->so_emu_fbo)
        gl_info->fbo_ops.glGenFramebuffers(1, &context_gl->so_emu_fbo);
    gl_info->fbo_ops.glBindRenderbuffer(GL_RENDERBUFFER, context_gl->so_emu_renderbuffer);
    gl_info->fbo_ops.glRenderbufferStorage(GL_RENDERBUFFER, GL_R32UI, SO_EMU_WIDTH, height);
    gl_info->fbo_ops.glBindRenderbuffer(GL_RENDERBUFFER, 0);
    gl_info->fbo_ops.glBindFramebuffer(GL_FRAMEBUFFER, context_gl->so_emu_fbo);
    context_gl->fbo_read_binding = context_gl->fbo_draw_binding = context_gl->so_emu_fbo;
    context_invalidate_state(&context_gl->c, STATE_FRAMEBUFFER);
    gl_info->fbo_ops.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
            context_gl->so_emu_renderbuffer);
    gl_info->gl_ops.gl.p_glDrawBuffer(GL_COLOR_ATTACHMENT0);
    gl_info->gl_ops.gl.p_glReadBuffer(GL_COLOR_ATTACHMENT0);
    if (gl_info->fbo_ops.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        ERR("Stream output target is incomplete.\n");
        context_gl->so_emu_height = 0;
        context_invalidate_state(&context_gl->c, STATE_FRAMEBUFFER);
        return false;
    }
    checkGLcall("stream output target");
    context_gl->so_emu_height = height;
    return true;
}

/* Called for a draw with stream output, after its state is applied and in
 * place of beginning transform feedback. Returns false if the draw is to use
 * transform feedback; otherwise the draw renders into this file's target and
 * wined3d_context_gl_so_emu_end() copies it to the buffer. */
bool wined3d_context_gl_so_emu_begin(struct wined3d_context_gl *context_gl, const struct wined3d_state *state,
        const struct wined3d_draw_parameters *parameters, struct wined3d_so_emu_draw *draw)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    const struct wined3d_stream_output *so = &state->stream_output[0];
    unsigned int verts, components, primitives, capacity, offset, total, i;
    GLint scale_location;
    GLuint program;

    draw->active = false;
    if (!wined3d_settings.stream_output_emulation || parameters->indirect || parameters->u.direct.instance_count > 1
            || !so->buffer || context_gl->c.transform_feedback_active)
        return false;
    for (i = 1; i < ARRAY_SIZE(state->stream_output); ++i)
    {
        if (state->stream_output[i].buffer)
            return false;
    }
    if (!(verts = so_emu_vertices_per_primitive(state->primitive_type)))
        return false;
    if (!(program = wined3d_glsl_so_emu_program(context_gl, state, &components, &scale_location)))
        return false;

    offset = (so->offset == ~0u ? 0 : so->offset) + context_gl->so_emu_written;
    capacity = so->buffer->resource.size > offset ? (so->buffer->resource.size - offset) / (components * 4) : 0;
    primitives = min(parameters->u.direct.index_count / verts, capacity);
    total = primitives * components;
    if ((total + SO_EMU_WIDTH - 1) / SO_EMU_WIDTH > SO_EMU_MAX_HEIGHT)
        return false;

    draw->active = true;
    draw->buffer = so->buffer;
    draw->offset = offset;
    draw->total = total;
    draw->program = program;
    if (!total)
    {
        draw->skip = true;
        return true;
    }
    draw->skip = false;
    if (!so_emu_prepare_target(context_gl, (total + SO_EMU_WIDTH - 1) / SO_EMU_WIDTH))
    {
        draw->active = false;
        return false;
    }

    gl_info->fbo_ops.glBindFramebuffer(GL_FRAMEBUFFER, context_gl->so_emu_fbo);
    context_gl->fbo_read_binding = context_gl->fbo_draw_binding = context_gl->so_emu_fbo;
    context_invalidate_state(&context_gl->c, STATE_FRAMEBUFFER);
    gl_info->gl_ops.gl.p_glViewport(0, 0, SO_EMU_WIDTH, context_gl->so_emu_height);
    context_invalidate_state(&context_gl->c, STATE_VIEWPORT);
    gl_info->gl_ops.gl.p_glDisable(GL_SCISSOR_TEST);
    context_invalidate_state(&context_gl->c, STATE_RASTERIZER);
    GL_EXTCALL(glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
    context_invalidate_state(&context_gl->c, STATE_BLEND);
    draw->clip_distance_mask = context_gl->c.clip_distance_mask;
    wined3d_context_gl_enable_clip_distances(context_gl, 0);

    GL_EXTCALL(glUseProgram(program));
    GL_EXTCALL(glUniform2f(scale_location, 2.0f / SO_EMU_WIDTH, 2.0f / context_gl->so_emu_height));
    checkGLcall("stream output setup");
    return true;
}

void wined3d_context_gl_so_emu_end(struct wined3d_context_gl *context_gl, const struct wined3d_state *state,
        const struct wined3d_so_emu_draw *draw)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_bo_gl *bo_gl = wined3d_bo_gl(draw->buffer->buffer_object);
    unsigned int rows = draw->total / SO_EMU_WIDTH, rest = draw->total % SO_EMU_WIDTH;
    GLintptr base = bo_gl->b.buffer_offset + draw->offset;

    context_gl->so_emu_written += draw->total * 4;
    if (draw->skip)
        return;

    GL_EXTCALL(glBindBuffer(GL_PIXEL_PACK_BUFFER, bo_gl->id));
    gl_info->gl_ops.gl.p_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl_info->gl_ops.gl.p_glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (rows)
        gl_info->gl_ops.gl.p_glReadPixels(0, 0, SO_EMU_WIDTH, rows, GL_RED_INTEGER, GL_UNSIGNED_INT, (void *)base);
    if (rest)
        gl_info->gl_ops.gl.p_glReadPixels(0, rows, rest, 1, GL_RED_INTEGER, GL_UNSIGNED_INT,
                (void *)(base + rows * SO_EMU_WIDTH * 4));
    GL_EXTCALL(glBindBuffer(GL_PIXEL_PACK_BUFFER, 0));
    wined3d_context_gl_reference_bo(context_gl, bo_gl);
    checkGLcall("stream output copy");

    /* The draw's own program, which wined3d believes is current. */
    GL_EXTCALL(glUseProgram(wined3d_glsl_so_emu_restore_program(context_gl)));
    wined3d_context_gl_enable_clip_distances(context_gl, draw->clip_distance_mask);
    if (state->rasterizer_state && state->rasterizer_state->desc.scissor)
        gl_info->gl_ops.gl.p_glEnable(GL_SCISSOR_TEST);
}

/* The stream output targets were set: the next draw writes at their offsets. */
void wined3d_context_gl_so_emu_reset(struct wined3d_context_gl *context_gl)
{
    context_gl->so_emu_written = 0;
}

void wined3d_context_gl_so_emu_cleanup(struct wined3d_context_gl *context_gl)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;

    if (context_gl->so_emu_fbo)
        gl_info->fbo_ops.glDeleteFramebuffers(1, &context_gl->so_emu_fbo);
    if (context_gl->so_emu_renderbuffer)
        gl_info->fbo_ops.glDeleteRenderbuffers(1, &context_gl->so_emu_renderbuffer);
    context_gl->so_emu_fbo = context_gl->so_emu_renderbuffer = 0;
    context_gl->so_emu_height = 0;
}
