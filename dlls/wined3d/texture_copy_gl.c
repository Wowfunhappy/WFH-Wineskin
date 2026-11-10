/*
 * WINESKIN: copies between texture images of different formats, on the GPU.
 *
 * Without ARB_texture_view, ARB_copy_image and image load/store (macOS's
 * OpenGL 4.1), wined3d keeps second textures in step with a texture: the
 * shadows OpenCL kernels read and write for formats OpenCL can't share
 * (cl_compute.c), and the textures of views their texture can't stand for
 * (view_copy_gl.c). wined3d_context_gl_copy_texels() copies one image (a
 * level of a layer, cube face or 3D slice) to another by drawing over the
 * destination with a fragment shader that fetches the source texel. It copies
 * values (an R11G11B10F texel into an RGBA16F one) or reinterprets the texel's
 * bits (an R32F texel as R32UI), and never waits for the GPU.
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

struct wined3d_gl_copy_program
{
    struct list entry;
    GLenum src_target;
    struct wined3d_gl_texel_format src_format, dst_format;
    bool reinterpret;
    GLuint id;
    GLint location;
};

struct wined3d_gl_copy_state
{
    struct list programs;
    GLuint vs, sampler;
};

void wined3d_gl_texel_format_from_format(struct wined3d_gl_texel_format *texel, const struct wined3d_format *format)
{
    const unsigned int sizes[4] = {format->red_size, format->green_size, format->blue_size, format->alpha_size};
    unsigned int i;

    memset(texel, 0, sizeof(*texel));
    for (i = 0; i < 4; ++i)
        texel->bits[i] = sizes[i];

    if (format->depth_size || format->stencil_size)
        texel->type = WINED3D_GL_TEXEL_DEPTH;
    else if (format->attrs & WINED3D_FORMAT_ATTR_INTEGER)
        texel->type = (format->attrs & WINED3D_FORMAT_ATTR_UNSIGNED) ? WINED3D_GL_TEXEL_UINT : WINED3D_GL_TEXEL_SINT;
    else if (format->attrs & WINED3D_FORMAT_ATTR_FLOAT)
        texel->type = format->red_size == 32 ? WINED3D_GL_TEXEL_FLOAT32
                : format->red_size == 16 ? WINED3D_GL_TEXEL_FLOAT16 : WINED3D_GL_TEXEL_FLOAT;
    else if ((format->attrs & WINED3D_FORMAT_ATTR_NORMALISED) && !(format->attrs & WINED3D_FORMAT_ATTR_UNSIGNED))
        texel->type = WINED3D_GL_TEXEL_SNORM;
    else
        texel->type = WINED3D_GL_TEXEL_UNORM;
}

void wined3d_gl_texel_format_from_internal(struct wined3d_gl_texel_format *texel, GLenum internal)
{
    memset(texel, 0, sizeof(*texel));
    switch (internal)
    {
        case GL_RGBA32UI:
        case GL_RGBA32I:
            texel->type = internal == GL_RGBA32UI ? WINED3D_GL_TEXEL_UINT : WINED3D_GL_TEXEL_SINT;
            memset(texel->bits, 32, sizeof(texel->bits));
            break;
        case GL_R32F:
            texel->type = WINED3D_GL_TEXEL_FLOAT32;
            texel->bits[0] = 32;
            break;
        case GL_RGBA32F:
            texel->type = WINED3D_GL_TEXEL_FLOAT32;
            memset(texel->bits, 32, sizeof(texel->bits));
            break;
        case GL_RGBA16F:
            texel->type = WINED3D_GL_TEXEL_FLOAT16;
            memset(texel->bits, 16, sizeof(texel->bits));
            break;
        default:
            ERR("Unhandled internal format %#x.\n", internal);
            texel->type = WINED3D_GL_TEXEL_FLOAT;
            break;
    }
}

static const char *copy_sampler_prefix(enum wined3d_gl_texel_type type)
{
    switch (type)
    {
        case WINED3D_GL_TEXEL_UINT: return "u";
        case WINED3D_GL_TEXEL_SINT: return "i";
        default: return "";
    }
}

static void copy_shader_add_fetch(struct wined3d_string_buffer *buffer, GLenum target, const char *prefix)
{
    const char *sampler, *fetch;

    switch (target)
    {
        case GL_TEXTURE_1D:
            sampler = "sampler1D";
            fetch = "texelFetch(src, c.x, loc.x)";
            break;
        case GL_TEXTURE_1D_ARRAY:
            sampler = "sampler1DArray";
            fetch = "texelFetch(src, ivec2(c.x, loc.y), loc.x)";
            break;
        case GL_TEXTURE_2D_ARRAY:
            sampler = "sampler2DArray";
            fetch = "texelFetch(src, ivec3(c, loc.y), loc.x)";
            break;
        case GL_TEXTURE_3D:
            sampler = "sampler3D";
            fetch = "texelFetch(src, ivec3(c, loc.y), loc.x)";
            break;
        case GL_TEXTURE_CUBE_MAP:
            sampler = "samplerCube";
            fetch = "textureLod(src, cube_dir(c, loc.y, textureSize(src, loc.x).x), float(loc.x))";
            break;
        case GL_TEXTURE_CUBE_MAP_ARRAY:
            sampler = "samplerCubeArray";
            fetch = "textureLod(src, vec4(cube_dir(c, loc.y % 6, textureSize(src, loc.x).x), float(loc.y / 6)),"
                    " float(loc.x))";
            break;
        default:
            sampler = "sampler2D";
            fetch = "texelFetch(src, c, loc.x)";
            break;
    }

    shader_addline(buffer, "uniform %s%s src;\n", prefix, sampler);
    if (target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_CUBE_MAP_ARRAY)
    {
        /* The direction of the centre of texel "c" of a face. */
        shader_addline(buffer, "vec3 cube_dir(ivec2 c, int face, int size)\n{\n");
        shader_addline(buffer, "    vec2 p = (vec2(c) + 0.5) / float(size) * 2.0 - 1.0;\n");
        shader_addline(buffer, "    if (face == 0) return vec3(1.0, -p.y, -p.x);\n");
        shader_addline(buffer, "    if (face == 1) return vec3(-1.0, -p.y, p.x);\n");
        shader_addline(buffer, "    if (face == 2) return vec3(p.x, 1.0, p.y);\n");
        shader_addline(buffer, "    if (face == 3) return vec3(p.x, -1.0, -p.y);\n");
        shader_addline(buffer, "    if (face == 4) return vec3(p.x, -p.y, 1.0);\n");
        shader_addline(buffer, "    return vec3(-p.x, -p.y, -1.0);\n}\n");
    }
    shader_addline(buffer, "%svec4 fetch(ivec2 c)\n{\n    return %s;\n}\n", prefix, fetch);
}

/* Per channel constants of a texel format: the largest unsigned and signed
 * values, the bit mask, and the shift that sign-extends. */
static void copy_shader_add_channel_constants(struct wined3d_string_buffer *buffer,
        const struct wined3d_gl_texel_format *format)
{
    unsigned int i, bits;

    shader_addline(buffer, "const vec4 umax = vec4(");
    for (i = 0; i < 4; ++i)
    {
        bits = format->bits[i];
        shader_addline(buffer, "%s%u.0", i ? ", " : "", bits && bits < 32 ? (1u << bits) - 1 : bits ? ~0u : 1u);
    }
    shader_addline(buffer, ");\nconst vec4 smax = vec4(");
    for (i = 0; i < 4; ++i)
    {
        bits = format->bits[i];
        shader_addline(buffer, "%s%u.0", i ? ", " : "", bits > 1 ? (1u << (bits - 1)) - 1 : 1u);
    }
    shader_addline(buffer, ");\nconst uvec4 mask = uvec4(");
    for (i = 0; i < 4; ++i)
    {
        bits = format->bits[i];
        shader_addline(buffer, "%s%#xu", i ? ", " : "", bits && bits < 32 ? (1u << bits) - 1 : bits ? ~0u : 0u);
    }
    shader_addline(buffer, ");\nconst ivec4 sext = ivec4(");
    for (i = 0; i < 4; ++i)
    {
        bits = format->bits[i];
        shader_addline(buffer, "%s%u", i ? ", " : "", bits && bits < 32 ? 32 - bits : 0u);
    }
    shader_addline(buffer, ");\n");
}

static const char copy_shader_half_functions[] =
    /* The bits of the 16-bit float with the value of "f", which has one. */
    "uint pack_half(float f)\n"
    "{\n"
    "    uint u = floatBitsToUint(f);\n"
    "    uint s = (u >> 16) & 0x8000u, e = (u >> 23) & 0xffu, m = u & 0x7fffffu;\n"
    "    if (e == 0xffu) return s | 0x7c00u | (m >> 13);\n"
    "    if (e > 142u) return s | 0x7c00u;\n"
    "    if (e >= 113u) return s | ((e - 112u) << 10) | (m >> 13);\n"
    "    if (e >= 103u) return s | ((m | 0x800000u) >> (126u - e));\n"
    "    return s;\n"
    "}\n"
    "float unpack_half(uint h)\n"
    "{\n"
    "    uint s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1fu, m = h & 0x3ffu;\n"
    "    if (e == 0x1fu) return uintBitsToFloat(s | 0x7f800000u | (m << 13));\n"
    "    if (e == 0u) return uintBitsToFloat(s | floatBitsToUint(float(m) * 5.9604644775390625e-8));\n"
    "    return uintBitsToFloat(s | ((e + 112u) << 23) | (m << 13));\n"
    "}\n";

static const char *copy_output_type(enum wined3d_gl_texel_type type)
{
    switch (type)
    {
        case WINED3D_GL_TEXEL_UINT: return "uvec4";
        case WINED3D_GL_TEXEL_SINT: return "ivec4";
        default: return "vec4";
    }
}

static bool copy_shader_add_main(struct wined3d_string_buffer *buffer, const struct wined3d_gl_copy_program *program)
{
    enum wined3d_gl_texel_type src = program->src_format.type, dst = program->dst_format.type;
    const char *prefix = copy_sampler_prefix(src);

    shader_addline(buffer, "void main()\n{\n");
    shader_addline(buffer, "    %svec4 t = fetch(ivec2(gl_FragCoord.xy));\n", prefix);

    if (dst == WINED3D_GL_TEXEL_DEPTH)
    {
        shader_addline(buffer, "    gl_FragDepth = t.x;\n}\n");
        return true;
    }
    if (!program->reinterpret)
    {
        shader_addline(buffer, "    dst = %s(t);\n}\n", copy_output_type(dst));
        return true;
    }

    switch (src)
    {
        case WINED3D_GL_TEXEL_FLOAT32:
            shader_addline(buffer, "    uvec4 b = floatBitsToUint(t);\n");
            break;
        case WINED3D_GL_TEXEL_FLOAT16:
            shader_addline(buffer, "    uvec4 b = uvec4(pack_half(t.x), pack_half(t.y), pack_half(t.z), pack_half(t.w));\n");
            break;
        case WINED3D_GL_TEXEL_UNORM:
            shader_addline(buffer, "    uvec4 b = uvec4(t * umax + 0.5);\n");
            break;
        case WINED3D_GL_TEXEL_SNORM:
            shader_addline(buffer, "    uvec4 b = uvec4(ivec4(round(clamp(t, -1.0, 1.0) * smax))) & mask;\n");
            break;
        case WINED3D_GL_TEXEL_UINT:
            shader_addline(buffer, "    uvec4 b = t;\n");
            break;
        case WINED3D_GL_TEXEL_SINT:
            shader_addline(buffer, "    uvec4 b = uvec4(t) & mask;\n");
            break;
        default:
            return false;
    }

    switch (dst)
    {
        case WINED3D_GL_TEXEL_FLOAT32:
            shader_addline(buffer, "    dst = uintBitsToFloat(b);\n");
            break;
        case WINED3D_GL_TEXEL_FLOAT16:
            shader_addline(buffer, "    dst = vec4(unpack_half(b.x), unpack_half(b.y), unpack_half(b.z), unpack_half(b.w));\n");
            break;
        case WINED3D_GL_TEXEL_UNORM:
            shader_addline(buffer, "    dst = vec4(b) / umax;\n");
            break;
        case WINED3D_GL_TEXEL_SNORM:
            shader_addline(buffer, "    dst = max(vec4((ivec4(b) << sext) >> sext) / smax, -1.0);\n");
            break;
        case WINED3D_GL_TEXEL_UINT:
            shader_addline(buffer, "    dst = b;\n");
            break;
        case WINED3D_GL_TEXEL_SINT:
            shader_addline(buffer, "    dst = (ivec4(b) << sext) >> sext;\n");
            break;
        default:
            return false;
    }
    shader_addline(buffer, "}\n");
    return true;
}

static GLuint copy_compile_shader(const struct wined3d_gl_info *gl_info, GLenum type, const char *source)
{
    GLint status = 0;
    GLuint id;

    id = GL_EXTCALL(glCreateShader(type));
    GL_EXTCALL(glShaderSource(id, 1, &source, NULL));
    GL_EXTCALL(glCompileShader(id));
    GL_EXTCALL(glGetShaderiv(id, GL_COMPILE_STATUS, &status));
    if (!status)
    {
        ERR("Failed to compile a copy shader:\n%s\n", debugstr_a(source));
        print_glsl_info_log(gl_info, id, FALSE);
        GL_EXTCALL(glDeleteShader(id));
        return 0;
    }
    return id;
}

static struct wined3d_gl_copy_state *copy_get_state(struct wined3d_context_gl *context_gl)
{
    static const char vs_source[] =
        "#version 330\n"
        "void main()\n"
        "{\n"
        "    gl_Position = vec4(vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    struct wined3d_device_gl *device_gl = wined3d_device_gl(context_gl->c.device);
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_gl_copy_state *state;

    if (device_gl->copy_state)
        return device_gl->copy_state;

    if (gl_info->glsl_version < MAKEDWORD_VERSION(3, 30) || !gl_info->supported[ARB_SAMPLER_OBJECTS]
            || !(state = calloc(1, sizeof(*state))))
        return NULL;
    list_init(&state->programs);
    if (!(state->vs = copy_compile_shader(gl_info, GL_VERTEX_SHADER, vs_source)))
    {
        free(state);
        return NULL;
    }

    /* Texels as they are stored: no filtering, comparison or sRGB decoding. */
    GL_EXTCALL(glGenSamplers(1, &state->sampler));
    GL_EXTCALL(glSamplerParameteri(state->sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST));
    GL_EXTCALL(glSamplerParameteri(state->sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    GL_EXTCALL(glSamplerParameteri(state->sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL_EXTCALL(glSamplerParameteri(state->sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    GL_EXTCALL(glSamplerParameteri(state->sampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE));
    GL_EXTCALL(glSamplerParameteri(state->sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE));
    if (gl_info->supported[EXT_TEXTURE_SRGB_DECODE])
        GL_EXTCALL(glSamplerParameteri(state->sampler, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT));
    checkGLcall("create copy state");

    device_gl->copy_state = state;
    return state;
}

static struct wined3d_gl_copy_program *copy_get_program(struct wined3d_context_gl *context_gl,
        struct wined3d_gl_copy_state *state, GLenum src_target, const struct wined3d_gl_texel_format *src_format,
        const struct wined3d_gl_texel_format *dst_format, bool reinterpret)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_gl_copy_program *program;
    struct wined3d_string_buffer buffer;
    GLint status = 0;
    GLuint fs;

    LIST_FOR_EACH_ENTRY(program, &state->programs, struct wined3d_gl_copy_program, entry)
    {
        if (program->src_target == src_target && program->reinterpret == reinterpret
                && program->src_format.type == src_format->type && program->dst_format.type == dst_format->type
                && (!reinterpret || !memcmp(program->src_format.bits, src_format->bits, sizeof(src_format->bits))))
            return program->id ? program : NULL;
    }

    if (!(program = calloc(1, sizeof(*program))))
        return NULL;
    program->src_target = src_target;
    program->src_format = *src_format;
    program->dst_format = *dst_format;
    program->reinterpret = reinterpret;
    list_add_head(&state->programs, &program->entry);

    if (!string_buffer_init(&buffer))
        return NULL;
    shader_addline(&buffer, "#version %s\n", src_target == GL_TEXTURE_CUBE_MAP_ARRAY ? "400" : "330");
    shader_addline(&buffer, "uniform ivec2 loc;\n");
    if (dst_format->type != WINED3D_GL_TEXEL_DEPTH)
        shader_addline(&buffer, "layout(location = 0) out %s dst;\n", copy_output_type(dst_format->type));
    copy_shader_add_fetch(&buffer, src_target, copy_sampler_prefix(src_format->type));
    if (reinterpret)
    {
        copy_shader_add_channel_constants(&buffer, src_format);
        shader_addline(&buffer, "%s", copy_shader_half_functions);
    }
    if (!copy_shader_add_main(&buffer, program))
    {
        FIXME("No conversion from texel type %u to %u.\n", src_format->type, dst_format->type);
        string_buffer_free(&buffer);
        return NULL;
    }

    fs = copy_compile_shader(gl_info, GL_FRAGMENT_SHADER, buffer.buffer);
    string_buffer_free(&buffer);
    if (!fs)
        return NULL;

    program->id = GL_EXTCALL(glCreateProgram());
    GL_EXTCALL(glAttachShader(program->id, state->vs));
    GL_EXTCALL(glAttachShader(program->id, fs));
    GL_EXTCALL(glLinkProgram(program->id));
    GL_EXTCALL(glDeleteShader(fs));
    GL_EXTCALL(glGetProgramiv(program->id, GL_LINK_STATUS, &status));
    if (!status)
    {
        ERR("Failed to link a copy program.\n");
        print_glsl_info_log(gl_info, program->id, TRUE);
        GL_EXTCALL(glDeleteProgram(program->id));
        program->id = 0;
        return NULL;
    }
    GL_EXTCALL(glUseProgram(program->id));
    program->location = GL_EXTCALL(glGetUniformLocation(program->id, "src"));
    GL_EXTCALL(glUniform1i(program->location, 0));
    program->location = GL_EXTCALL(glGetUniformLocation(program->id, "loc"));
    checkGLcall("create copy program");
    TRACE("Created copy program %u: target %#x, texel type %u to %u, reinterpret %d.\n",
            program->id, src_target, src_format->type, dst_format->type, reinterpret);
    return program;
}

void wined3d_device_gl_destroy_copy_state(struct wined3d_device_gl *device_gl, struct wined3d_context_gl *context_gl)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_gl_copy_state *state = device_gl->copy_state;
    struct wined3d_gl_copy_program *program, *next;

    if (!state)
        return;
    LIST_FOR_EACH_ENTRY_SAFE(program, next, &state->programs, struct wined3d_gl_copy_program, entry)
    {
        if (program->id)
            GL_EXTCALL(glDeleteProgram(program->id));
        free(program);
    }
    GL_EXTCALL(glDeleteShader(state->vs));
    GL_EXTCALL(glDeleteSamplers(1, &state->sampler));
    checkGLcall("destroy copy state");
    free(state);
    device_gl->copy_state = NULL;
}

/* Makes every level of a wined3d texture reachable (a view may have narrowed
 * its level range, see wined3d_shader_resource_view_gl_apply_view()), and
 * leaves it bound to the active unit. Returns the texture's gl_texture. */
static struct gl_texture *copy_prepare_texture(struct wined3d_context_gl *context_gl,
        struct wined3d_texture_gl *texture_gl)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct gl_texture *gl_tex = wined3d_texture_gl_get_gl_texture(texture_gl, FALSE);

    wined3d_context_gl_bind_texture(context_gl, texture_gl->target, gl_tex->name);
    if (texture_gl->target == GL_TEXTURE_2D_MULTISAMPLE || texture_gl->target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY)
        return gl_tex;
    if (gl_tex->view_levels || gl_tex->sampler_desc.mip_base_level)
    {
        gl_info->gl_ops.gl.p_glTexParameteri(texture_gl->target, GL_TEXTURE_BASE_LEVEL, 0);
        gl_info->gl_ops.gl.p_glTexParameteri(texture_gl->target, GL_TEXTURE_MAX_LEVEL, texture_gl->t.level_count - 1);
        gl_tex->sampler_desc.mip_base_level = 0;
        gl_tex->view_levels = false;
    }
    return gl_tex;
}

static void copy_attach(const struct wined3d_gl_info *gl_info, GLenum attachment,
        const struct wined3d_gl_copy_image *image)
{
    switch (image->target)
    {
        case GL_TEXTURE_1D:
            gl_info->fbo_ops.glFramebufferTexture1D(GL_DRAW_FRAMEBUFFER, attachment,
                    GL_TEXTURE_1D, image->name, image->level);
            break;
        case GL_TEXTURE_1D_ARRAY:
        case GL_TEXTURE_2D_ARRAY:
        case GL_TEXTURE_3D:
        case GL_TEXTURE_CUBE_MAP_ARRAY:
            gl_info->fbo_ops.glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, attachment,
                    image->name, image->level, image->layer);
            break;
        case GL_TEXTURE_CUBE_MAP:
            gl_info->fbo_ops.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment,
                    GL_TEXTURE_CUBE_MAP_POSITIVE_X + image->layer, image->name, image->level);
            break;
        default:
            gl_info->fbo_ops.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment,
                    image->target, image->name, image->level);
            break;
    }
}

/* Copies the "width" x "height" image "src" to "dst". With "reinterpret" the
 * destination texel gets the source texel's bits (the two formats have the
 * same channel sizes), otherwise its value. sRGB textures are read and
 * written as they are stored. Returns false when the destination can't be
 * drawn to or the conversion has no shader; nothing is copied then.
 *
 * This draws: it runs where wined3d loads resources, never while a draw's
 * state is being applied. Context activation is done by the caller. */
bool wined3d_context_gl_copy_texels(struct wined3d_context_gl *context_gl,
        const struct wined3d_gl_copy_image *src, const struct wined3d_gl_texel_format *src_format,
        const struct wined3d_gl_copy_image *dst, const struct wined3d_gl_texel_format *dst_format,
        unsigned int width, unsigned int height, bool reinterpret)
{
    bool depth = dst_format->type == WINED3D_GL_TEXEL_DEPTH;
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_gl_copy_program *program;
    struct wined3d_gl_copy_state *state;
    bool texture_decodes = false;
    GLenum status;
    GLuint fbo;

    if (!(state = copy_get_state(context_gl)))
        return false;

    wined3d_context_gl_prepare_generated_draw(context_gl);

    if (!(program = copy_get_program(context_gl, state, src->target, src_format, dst_format, reinterpret)))
        return false;

    if (dst->texture_gl)
        copy_prepare_texture(context_gl, dst->texture_gl);
    if (src->texture_gl)
    {
        /* The sampler's sRGB decode setting governs sampling; texel
         * fetches take the texture's. */
        texture_decodes = copy_prepare_texture(context_gl, src->texture_gl)->sampler_desc.srgb_decode
                && gl_info->supported[EXT_TEXTURE_SRGB_DECODE];
        if (texture_decodes)
            gl_info->gl_ops.gl.p_glTexParameteri(src->target, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
    }
    else
    {
        wined3d_context_gl_bind_texture(context_gl, src->target, src->name);
    }
    GL_EXTCALL(glBindSampler(0, state->sampler));

    gl_info->fbo_ops.glGenFramebuffers(1, &fbo);
    gl_info->fbo_ops.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    copy_attach(gl_info, depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0, dst);
    if (depth)
        gl_info->gl_ops.gl.p_glDrawBuffer(GL_NONE);
    if ((status = gl_info->fbo_ops.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)) == GL_FRAMEBUFFER_COMPLETE)
    {
        if (depth)
        {
            gl_info->gl_ops.gl.p_glEnable(GL_DEPTH_TEST);
            gl_info->gl_ops.gl.p_glDepthFunc(GL_ALWAYS);
            gl_info->gl_ops.gl.p_glDepthMask(GL_TRUE);
            if (gl_info->supported[ARB_DEPTH_CLAMP])
                gl_info->gl_ops.gl.p_glDisable(GL_DEPTH_CLAMP);
            gl_info->gl_ops.gl.p_glDepthRange(0.0, 1.0);
            gl_info->gl_ops.gl.p_glDisable(GL_POLYGON_OFFSET_FILL);
        }
        gl_info->gl_ops.gl.p_glViewport(0, 0, width, height);
        GL_EXTCALL(glUseProgram(program->id));
        GL_EXTCALL(glUniform2i(program->location, src->level, src->layer));
        gl_info->gl_ops.gl.p_glDrawArrays(GL_TRIANGLES, 0, 3);
        if (depth)
            gl_info->gl_ops.gl.p_glDisable(GL_DEPTH_TEST);
    }
    else
    {
        WARN("Texture %u (target %#x, level %u, layer %u) can't be drawn to, status %#x.\n",
                dst->name, dst->target, dst->level, dst->layer, status);
    }

    GL_EXTCALL(glUseProgram(0));
    GL_EXTCALL(glBindSampler(0, 0));
    if (texture_decodes)
        gl_info->gl_ops.gl.p_glTexParameteri(src->target, GL_TEXTURE_SRGB_DECODE_EXT, GL_DECODE_EXT);
    wined3d_context_gl_bind_texture(context_gl, GL_NONE, 0);
    gl_info->fbo_ops.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, context_gl->fbo_draw_binding);
    gl_info->fbo_ops.glDeleteFramebuffers(1, &fbo);
    checkGLcall("copy texels");

    return status == GL_FRAMEBUFFER_COMPLETE;
}
