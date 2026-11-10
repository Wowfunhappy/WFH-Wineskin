/*
 * WINESKIN: buffer views of formats that are not buffer texture formats.
 *
 * OpenGL 4.1 attaches a buffer to a buffer texture with a short list of
 * internal formats: no SNORM formats, no BGRA, no 10/10/10/2. Direct3D 11
 * has buffer views of any format (Unreal Engine fetches vertex tangents from
 * R8G8B8A8_SNORM buffer views). Such a view reads a buffer of its own that
 * holds the viewed elements as RGBA32F. The conversion runs on the GPU: the
 * source buffer is fetched as a vertex attribute of the view's format, which
 * OpenGL converts to floats, and captured by transform feedback. It is done
 * again when the view is used after the source buffer has been written
 * (struct wined3d_bo.content_serial).
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

struct wined3d_gl_buffer_convert_state
{
    GLuint program, feedback;
    /* A draw needs a complete framebuffer even when nothing is rasterised. */
    GLuint renderbuffer;
};

/* Whether views of a format can be converted: OpenGL has a float or
 * normalised vertex attribute type for it. */
bool wined3d_gl_buffer_view_format_convertible(const struct wined3d_gl_info *gl_info,
        const struct wined3d_format_gl *format_gl)
{
    return gl_info->supported[ARB_TRANSFORM_FEEDBACK2] && gl_info->glsl_version >= MAKEDWORD_VERSION(3, 30)
            && !gl_info->supported[WINED3D_GL_LEGACY_CONTEXT]
            && (format_gl->f.caps[WINED3D_GL_RES_TYPE_BUFFER] & WINED3D_FORMAT_CAP_VERTEX_ATTRIBUTE)
            && format_gl->vtx_type && !(format_gl->f.attrs & WINED3D_FORMAT_ATTR_INTEGER);
}

static GLuint convert_compile_shader(const struct wined3d_gl_info *gl_info, GLenum type, const char *source)
{
    GLuint id = GL_EXTCALL(glCreateShader(type));

    GL_EXTCALL(glShaderSource(id, 1, &source, NULL));
    GL_EXTCALL(glCompileShader(id));
    return id;
}

static struct wined3d_gl_buffer_convert_state *convert_get_state(struct wined3d_context_gl *context_gl)
{
    static const char vs_source[] =
        "#version 330\n"
        "in vec4 element;\n"
        "out vec4 converted;\n"
        "void main()\n"
        "{\n"
        "    converted = element;\n"
        "    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "}\n";
    /* macOS's core profile fails draws of a program without a fragment shader. */
    static const char fs_source[] =
        "#version 330\n"
        "void main()\n"
        "{\n"
        "}\n";
    static const char *varying = "converted";
    struct wined3d_device_gl *device_gl = wined3d_device_gl(context_gl->c.device);
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_gl_buffer_convert_state *state;
    GLint status = 0;
    GLuint vs, fs;

    if (device_gl->buffer_convert_state)
        return device_gl->buffer_convert_state;
    if (!(state = calloc(1, sizeof(*state))))
        return NULL;

    vs = convert_compile_shader(gl_info, GL_VERTEX_SHADER, vs_source);
    fs = convert_compile_shader(gl_info, GL_FRAGMENT_SHADER, fs_source);
    state->program = GL_EXTCALL(glCreateProgram());
    GL_EXTCALL(glAttachShader(state->program, vs));
    GL_EXTCALL(glAttachShader(state->program, fs));
    GL_EXTCALL(glBindAttribLocation(state->program, 0, "element"));
    GL_EXTCALL(glTransformFeedbackVaryings(state->program, 1, &varying, GL_INTERLEAVED_ATTRIBS));
    GL_EXTCALL(glLinkProgram(state->program));
    GL_EXTCALL(glDeleteShader(vs));
    GL_EXTCALL(glDeleteShader(fs));
    GL_EXTCALL(glGetProgramiv(state->program, GL_LINK_STATUS, &status));
    if (!status)
    {
        ERR("Failed to link the buffer conversion program.\n");
        print_glsl_info_log(gl_info, state->program, TRUE);
        GL_EXTCALL(glDeleteProgram(state->program));
        free(state);
        return NULL;
    }
    GL_EXTCALL(glGenTransformFeedbacks(1, &state->feedback));
    gl_info->fbo_ops.glGenRenderbuffers(1, &state->renderbuffer);
    gl_info->fbo_ops.glBindRenderbuffer(GL_RENDERBUFFER, state->renderbuffer);
    gl_info->fbo_ops.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 1, 1);
    gl_info->fbo_ops.glBindRenderbuffer(GL_RENDERBUFFER, 0);
    checkGLcall("create buffer conversion state");

    device_gl->buffer_convert_state = state;
    return state;
}

void wined3d_device_gl_destroy_buffer_convert_state(struct wined3d_device_gl *device_gl,
        struct wined3d_context_gl *context_gl)
{
    struct wined3d_gl_buffer_convert_state *state = device_gl->buffer_convert_state;
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;

    if (!state)
        return;
    GL_EXTCALL(glDeleteProgram(state->program));
    GL_EXTCALL(glDeleteTransformFeedbacks(1, &state->feedback));
    gl_info->fbo_ops.glDeleteRenderbuffers(1, &state->renderbuffer);
    checkGLcall("destroy buffer conversion state");
    free(state);
    device_gl->buffer_convert_state = NULL;
}

/* Writes the "count" elements of "format_gl" at "offset" of buffer "src" to
 * buffer "dst" as RGBA32F. This draws: it runs where wined3d loads
 * resources, never while a draw's state is being applied. Context
 * activation is done by the caller. */
void wined3d_context_gl_convert_buffer_elements(struct wined3d_context_gl *context_gl,
        const struct wined3d_format_gl *format_gl, GLuint src, unsigned int offset, unsigned int count, GLuint dst)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_gl_buffer_convert_state *state;
    GLuint fbo;

    if (!count || !(state = convert_get_state(context_gl)))
        return;

    /* A transform feedback of the application's is paused, and stays the
     * default transform feedback object's. */
    wined3d_context_gl_prepare_generated_draw(context_gl);

    gl_info->fbo_ops.glGenFramebuffers(1, &fbo);
    gl_info->fbo_ops.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    gl_info->fbo_ops.glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_RENDERBUFFER, state->renderbuffer);

    GL_EXTCALL(glUseProgram(state->program));
    GL_EXTCALL(glBindBuffer(GL_ARRAY_BUFFER, src));
    GL_EXTCALL(glEnableVertexAttribArray(0));
    GL_EXTCALL(glVertexAttribPointer(0, format_gl->vtx_format, format_gl->vtx_type,
            !!(format_gl->f.attrs & WINED3D_FORMAT_ATTR_NORMALISED), format_gl->f.byte_count,
            (const void *)(ULONG_PTR)offset));
    GL_EXTCALL(glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, state->feedback));
    GL_EXTCALL(glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, dst));
    gl_info->gl_ops.gl.p_glEnable(GL_RASTERIZER_DISCARD);
    GL_EXTCALL(glBeginTransformFeedback(GL_POINTS));
    gl_info->gl_ops.gl.p_glDrawArrays(GL_POINTS, 0, count);
    GL_EXTCALL(glEndTransformFeedback());
    gl_info->gl_ops.gl.p_glDisable(GL_RASTERIZER_DISCARD);
    GL_EXTCALL(glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0));
    GL_EXTCALL(glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0));
    GL_EXTCALL(glDisableVertexAttribArray(0));
    GL_EXTCALL(glBindBuffer(GL_ARRAY_BUFFER, 0));
    GL_EXTCALL(glUseProgram(0));
    gl_info->fbo_ops.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, context_gl->fbo_draw_binding);
    gl_info->fbo_ops.glDeleteFramebuffers(1, &fbo);
    checkGLcall("convert buffer elements");
}
