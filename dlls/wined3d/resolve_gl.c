/*
 * WINESKIN: multisample resolves of sRGB formats as a draw.
 *
 * Direct3D resolves an sRGB format in linear space: each sample is decoded,
 * the samples averaged, the average encoded. macOS 10.9's NVIDIA driver
 * averages the encoded bytes in glBlitFramebuffer, whatever the formats and
 * GL_FRAMEBUFFER_SRGB (~/WineskinTests/d3d11tools/probe/msblit2.c), which
 * darkens every edge an antialiased sRGB target resolves. A resolve whose
 * format is sRGB, from a multisample texture to a texture, therefore draws:
 * a fragment shader fetches the samples, decodes them unless the texture's
 * sRGB internal format did (multisample textures take no sRGB decode
 * parameter), averages, encodes, and writes the bytes with
 * GL_FRAMEBUFFER_SRGB off.
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

WINE_DECLARE_DEBUG_CHANNEL(d3d);

struct wined3d_gl_resolve_state
{
    GLuint programs[2]; /* GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_2D_MULTISAMPLE_ARRAY */
    GLint locations[2], decode_locations[2];
    bool failed[2];
};

static const char resolve_vs[] =
    "#version 330\n"
    "void main()\n"
    "{\n"
    "    gl_Position = vec4(vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* "args": xy the source texel of the destination's origin, z the layer, w the
 * sample count; "fetch_decodes": whether texel fetches return decoded values
 * (an sRGB internal format). */
static const char resolve_fs[] =
    "uniform ivec4 args;\n"
    "uniform int fetch_decodes;\n"
    "layout(location = 0) out vec4 dst;\n"
    "vec3 decode(vec3 c)\n"
    "{\n"
    "    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));\n"
    "}\n"
    "vec3 encode(vec3 c)\n"
    "{\n"
    "    c = clamp(c, 0.0, 1.0);\n"
    "    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, greaterThan(c, vec3(0.0031308)));\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy) + args.xy;\n"
    "    vec4 sum = vec4(0.0);\n"
    "    for (int i = 0; i < args.w; ++i)\n"
    "    {\n"
    "        vec4 t = FETCH;\n"
    "        sum += fetch_decodes != 0 ? t : vec4(decode(t.rgb), t.a);\n"
    "    }\n"
    "    sum /= float(args.w);\n"
    "    dst = vec4(encode(sum.rgb), sum.a);\n"
    "}\n";

static GLuint resolve_compile(const struct wined3d_gl_info *gl_info, GLenum type, const char *const *sources,
        unsigned int count)
{
    GLuint shader;
    GLint status;

    shader = GL_EXTCALL(glCreateShader(type));
    GL_EXTCALL(glShaderSource(shader, count, sources, NULL));
    GL_EXTCALL(glCompileShader(shader));
    GL_EXTCALL(glGetShaderiv(shader, GL_COMPILE_STATUS, &status));
    if (!status)
    {
        ERR_(d3d)("Failed to compile a resolve shader.\n");
        print_glsl_info_log(gl_info, shader, FALSE);
        GL_EXTCALL(glDeleteShader(shader));
        return 0;
    }
    return shader;
}

static GLuint resolve_get_program(struct wined3d_context_gl *context_gl, bool array, GLint *location)
{
    struct wined3d_device_gl *device_gl = wined3d_device_gl(context_gl->c.device);
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_gl_resolve_state *state;
    const char *fs_sources[3];
    GLuint vs, fs, program;
    GLint status;

    if (!(state = device_gl->resolve_state))
    {
        if (!(state = calloc(1, sizeof(*state))))
            return 0;
        device_gl->resolve_state = state;
    }
    if (state->programs[array] || state->failed[array])
    {
        *location = state->locations[array];
        return state->programs[array];
    }
    state->failed[array] = true;

    fs_sources[0] = array ? "#version 330\nuniform sampler2DMSArray src;\n"
            "#define FETCH texelFetch(src, ivec3(p, args.z), i)\n"
            : "#version 330\nuniform sampler2DMS src;\n#define FETCH texelFetch(src, p, i)\n";
    fs_sources[1] = resolve_fs;
    fs_sources[2] = resolve_vs;
    if (!(vs = resolve_compile(gl_info, GL_VERTEX_SHADER, &fs_sources[2], 1)))
        return 0;
    if (!(fs = resolve_compile(gl_info, GL_FRAGMENT_SHADER, fs_sources, 2)))
    {
        GL_EXTCALL(glDeleteShader(vs));
        return 0;
    }
    program = GL_EXTCALL(glCreateProgram());
    GL_EXTCALL(glAttachShader(program, vs));
    GL_EXTCALL(glAttachShader(program, fs));
    GL_EXTCALL(glLinkProgram(program));
    GL_EXTCALL(glDeleteShader(vs));
    GL_EXTCALL(glDeleteShader(fs));
    GL_EXTCALL(glGetProgramiv(program, GL_LINK_STATUS, &status));
    if (!status)
    {
        ERR_(d3d)("Failed to link a resolve program.\n");
        print_glsl_info_log(gl_info, program, TRUE);
        GL_EXTCALL(glDeleteProgram(program));
        return 0;
    }
    GL_EXTCALL(glUseProgram(program));
    *location = GL_EXTCALL(glGetUniformLocation(program, "src"));
    GL_EXTCALL(glUniform1i(*location, 0));
    state->locations[array] = GL_EXTCALL(glGetUniformLocation(program, "args"));
    state->decode_locations[array] = GL_EXTCALL(glGetUniformLocation(program, "fetch_decodes"));
    GL_EXTCALL(glUseProgram(0));
    checkGLcall("create resolve program");
    state->programs[array] = program;
    state->failed[array] = false;
    *location = state->locations[array];
    return program;
}

/* Resolves "src_rect" of layer "layer" of the multisample texture
 * "texture_gl" (its sRGB texture with "srgb") into "dst_rect" of the draw
 * framebuffer bound, in linear space, the stored bytes of both being
 * sRGB-encoded. Returns false, having drawn nothing, where it can't. Context
 * activation is done by the caller. */
bool wined3d_context_gl_resolve_srgb(struct wined3d_context_gl *context_gl, struct wined3d_texture_gl *texture_gl,
        bool srgb, unsigned int layer, const RECT *src_rect, const RECT *dst_rect)
{
    struct gl_texture *gl_texture = srgb ? &texture_gl->texture_srgb : &texture_gl->texture_rgb;
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    GLenum target = texture_gl->target;
    bool array = target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
    GLint location, samples = 0, internal = 0;
    bool fetch_decodes;
    GLuint program;

    if ((target != GL_TEXTURE_2D_MULTISAMPLE && !array) || !gl_texture->name
            || gl_info->glsl_version < MAKEDWORD_VERSION(3, 30))
        return false;
    if (dst_rect->right <= dst_rect->left || dst_rect->bottom <= dst_rect->top
            || src_rect->right - src_rect->left != dst_rect->right - dst_rect->left
            || src_rect->bottom - src_rect->top != dst_rect->bottom - dst_rect->top)
        return false;

    wined3d_context_gl_prepare_generated_draw(context_gl);
    if (!(program = resolve_get_program(context_gl, array, &location)))
        return false;

    wined3d_context_gl_bind_texture(context_gl, target, gl_texture->name);
    gl_info->gl_ops.gl.p_glGetTexLevelParameteriv(target, 0, GL_TEXTURE_SAMPLES, &samples);
    gl_info->gl_ops.gl.p_glGetTexLevelParameteriv(target, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal);
    fetch_decodes = internal == GL_SRGB8_ALPHA8 || internal == GL_SRGB8 || internal == GL_SRGB_ALPHA
            || internal == GL_SRGB;
    GL_EXTCALL(glBindSampler(0, 0));
    gl_info->gl_ops.gl.p_glViewport(dst_rect->left, dst_rect->top,
            dst_rect->right - dst_rect->left, dst_rect->bottom - dst_rect->top);
    GL_EXTCALL(glUseProgram(program));
    GL_EXTCALL(glUniform4i(location, src_rect->left - dst_rect->left, src_rect->top - dst_rect->top,
            layer, max(samples, 1)));
    GL_EXTCALL(glUniform1i(wined3d_device_gl(context_gl->c.device)->resolve_state->decode_locations[array],
            fetch_decodes));
    gl_info->gl_ops.gl.p_glDrawArrays(GL_TRIANGLES, 0, 3);
    GL_EXTCALL(glUseProgram(0));
    wined3d_context_gl_bind_texture(context_gl, GL_NONE, 0);
    checkGLcall("sRGB resolve");
    return true;
}

void wined3d_device_gl_destroy_resolve_state(struct wined3d_device_gl *device_gl,
        struct wined3d_context_gl *context_gl)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_gl_resolve_state *state;
    unsigned int i;

    if (!(state = device_gl->resolve_state))
        return;
    for (i = 0; i < ARRAY_SIZE(state->programs); ++i)
    {
        if (state->programs[i])
            GL_EXTCALL(glDeleteProgram(state->programs[i]));
    }
    checkGLcall("destroy resolve state");
    free(state);
    device_gl->resolve_state = NULL;
}
