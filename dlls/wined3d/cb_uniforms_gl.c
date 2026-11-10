/*
 * WINESKIN: Direct3D constant buffers as plain uniform arrays.
 *
 * macOS 10.9's NVIDIA OpenGL driver makes a program switch cost far more when
 * the programs read uniform blocks: switching between two of Ori and the Will
 * of the Wisps' programs costs 5.4 microseconds of CPU time and stalls the GPU
 * for 9.7 per draw, against 3.3 and 1.0 when the same programs read their
 * constants from plain uniform arrays (~/WineskinTests/perf/tools/drawcost2.c).
 * The size and number of the blocks, the buffers' usage and binding ranges,
 * and program pipelines make no difference; any block does it.
 *
 * So a shader model 4 or 5 vertex or pixel shader declares its constant
 * buffers as "uniform uvec4 <stage>_cb<n>[size]" while they fit the stage's
 * uniform storage (wined3d_shader_cb_is_uniform), and reads them through
 * uintBitsToFloat(): unsigned uniforms keep every bit, where float ones may
 * lose NaN payloads and denormals, which integer constants are as floats. A
 * constant buffer the shader indexes dynamically stays a block: Direct3D reads
 * past its declared size as far as the bound buffer goes. So do those that
 * don't fit.
 *
 * Before each draw, a program loads (glProgramUniform4uiv()) the contents of
 * each such constant buffer that changed since it last loaded it: another
 * buffer, a write to the buffer, another range (wined3d_cb_uniforms_load).
 * The contents come from the buffer object's copy in system memory (struct
 * wined3d_bo's cl_mirror, which writes from the CPU keep current); after a
 * copy on the GPU, or before the first whole write, the buffer is read back. A
 * bound range shorter than the declared array is padded with zeros, as
 * Direct3D reads past the end of a constant buffer.
 *
 * It is off unless HKCU\Software\Wine\Direct3D\ConstantBufferUniforms is
 * "y" (or WINE_D3D_CONFIG has "ConstantBufferUniforms=y"): a buffer that an
 * update replaces with another buffer object has no copy in system memory,
 * and the read back that takes stalls on the GPU at each draw that uses it
 * (Bit.Trip Rerunner's title drops to 1.5 frames a second).
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
WINE_DECLARE_DEBUG_CHANNEL(d3d_perf);

/* Uniform vectors left to the stage's other uniforms (positions fixups,
 * level counts, the immediate constant buffer is counted besides). */
#define CB_UNIFORM_RESERVE 64
/* Buffer objects up to this size get a copy in system memory to load from;
 * a larger one is read back for each change. */
#define CB_MIRROR_MAX_SIZE (4u << 20)

static unsigned int cb_uniform_limit(const struct wined3d_gl_info *gl_info, enum wined3d_shader_type type)
{
    if (type == WINED3D_SHADER_TYPE_VERTEX)
        return gl_info->limits.glsl_vs_float_constants;
    if (type == WINED3D_SHADER_TYPE_PIXEL)
        return gl_info->limits.glsl_ps_float_constants;
    return 0;
}

/* Whether constant buffer "idx" of a shader is a uniform array: buffers the
 * shader doesn't index dynamically are taken in order while their declared
 * sizes fit what is left of the stage's uniform vectors. */
bool wined3d_shader_cb_is_uniform(const struct wined3d_gl_info *gl_info,
        const struct wined3d_shader_reg_maps *reg_maps, unsigned int idx)
{
    unsigned int limit, reserve, budget, i;

    if (!wined3d_settings.cb_uniforms || reg_maps->shader_version.major < 4
            || idx >= WINED3D_MAX_CBS || !reg_maps->cb_sizes[idx]
            || (reg_maps->cb_dynamic_map & (1u << idx)) || !gl_info->gl_ops.ext.p_glProgramUniform4uiv)
        return false;
    limit = cb_uniform_limit(gl_info, reg_maps->shader_version.type);
    reserve = CB_UNIFORM_RESERVE + (reg_maps->icb ? reg_maps->icb->vec4_count : 0);
    if (limit <= reserve)
        return false;
    budget = limit - reserve;
    for (i = 0; i <= idx; ++i)
    {
        if (!reg_maps->cb_sizes[i] || (reg_maps->cb_dynamic_map & (1u << i)))
            continue;
        if (reg_maps->cb_sizes[i] > budget)
        {
            if (i == idx)
                return false;
            continue;
        }
        if (i == idx)
            return true;
        budget -= reg_maps->cb_sizes[i];
    }
    return false;
}

/* Find the uniform arrays of a linked program's vertex and pixel shaders. */
void wined3d_cb_uniforms_init(struct wined3d_cb_uniforms *u, const struct wined3d_gl_info *gl_info,
        GLuint program, const struct wined3d_shader *vs, const struct wined3d_shader *ps)
{
    const struct wined3d_shader *shaders[2] = {vs, ps};
    struct wined3d_cb_uniform list[2 * WINED3D_MAX_CBS];
    unsigned int s, i, count = 0;
    char name[16];
    GLint location;

    u->count = 0;
    u->uniforms = NULL;
    for (s = 0; s < ARRAY_SIZE(shaders); ++s)
    {
        const struct wined3d_shader_reg_maps *reg_maps;

        if (!shaders[s])
            continue;
        reg_maps = &shaders[s]->reg_maps;
        for (i = 0; i < WINED3D_MAX_CBS; ++i)
        {
            if (!wined3d_shader_cb_is_uniform(gl_info, reg_maps, i))
                continue;
            snprintf(name, sizeof(name), "%s_cb%u", s ? "ps" : "vs", i);
            if ((location = GL_EXTCALL(glGetUniformLocation(program, name))) == -1)
                continue;
            memset(&list[count], 0, sizeof(list[count]));
            list[count].location = location;
            list[count].shader_type = s ? WINED3D_SHADER_TYPE_PIXEL : WINED3D_SHADER_TYPE_VERTEX;
            list[count].idx = i;
            list[count].count = reg_maps->cb_sizes[i];
            ++count;
        }
    }
    checkGLcall("constant buffer uniform locations");
    if (count && (u->uniforms = malloc(count * sizeof(*u->uniforms))))
    {
        memcpy(u->uniforms, list, count * sizeof(*u->uniforms));
        u->count = count;
    }
}

void wined3d_cb_uniforms_cleanup(struct wined3d_cb_uniforms *u)
{
    free(u->uniforms);
    u->uniforms = NULL;
    u->count = 0;
}

/* The current contents of a buffer object in system memory. */
static const BYTE *cb_bo_contents(struct wined3d_context_gl *context_gl, struct wined3d_bo_gl *bo_gl)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_bo *bo = &bo_gl->b;
    static unsigned int once;

    if (!bo->cl_mirror && (bo_gl->size > CB_MIRROR_MAX_SIZE || !(bo->cl_mirror = malloc(bo_gl->size))))
        return NULL;
    if (bo->cl_mirror_serial == bo->cl_serial)
        return bo->cl_mirror;

    if (!once++)
        FIXME_(d3d_perf)("Reading constant buffer contents back from the GPU.\n");
    else
        WARN_(d3d_perf)("Reading buffer object %u back (%u bytes).\n", bo_gl->id, (unsigned int)bo_gl->size);
    wined3d_context_gl_bind_bo(context_gl, bo_gl->binding, bo_gl->id);
    GL_EXTCALL(glGetBufferSubData(bo_gl->binding, bo->buffer_offset, bo_gl->size, bo->cl_mirror));
    checkGLcall("constant buffer read back");
    bo->cl_mirror_serial = bo->cl_serial;
    return bo->cl_mirror;
}

/* Load the constant buffers bound for a draw into the uniform arrays of
 * "program" that hold other contents. */
void wined3d_cb_uniforms_load(struct wined3d_cb_uniforms *u, struct wined3d_context_gl *context_gl,
        GLuint program, const struct wined3d_state *state)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    static struct wined3d_vec4 padded[1024];
    struct wined3d_cb_uniform *uniform;
    struct wined3d_bo_gl *bo_gl;
    struct wined3d_buffer *buffer;
    unsigned int i, offset, size;
    const BYTE *data;

    for (i = 0; i < u->count; ++i)
    {
        const struct wined3d_constant_buffer_state *cb;

        uniform = &u->uniforms[i];
        cb = &state->cb[uniform->shader_type][uniform->idx];
        if (!(buffer = cb->buffer) || !buffer->buffer_object || cb->offset >= buffer->resource.size)
        {
            if (uniform->loaded && !uniform->bo)
                continue;
            memset(padded, 0, min(uniform->count, ARRAY_SIZE(padded)) * sizeof(*padded));
            GL_EXTCALL(glProgramUniform4uiv(program, uniform->location,
                    min(uniform->count, ARRAY_SIZE(padded)), (const GLuint *)padded));
            uniform->bo = NULL;
            uniform->loaded = true;
            continue;
        }

        bo_gl = wined3d_bo_gl(buffer->buffer_object);
        offset = cb->offset;
        size = min(cb->size, buffer->resource.size - offset);
        if (uniform->loaded && uniform->bo == &bo_gl->b && uniform->cl_serial == bo_gl->b.cl_serial
                && uniform->content_serial == bo_gl->b.content_serial && uniform->mirror_serial == bo_gl->b.cl_mirror_serial
                && uniform->offset == offset && uniform->size == size)
            continue;

        if (!(data = cb_bo_contents(context_gl, bo_gl)))
        {
            static unsigned int once;

            if (!once++)
                FIXME("Buffer object of %u bytes is too large to load into uniforms.\n", (unsigned int)bo_gl->size);
            continue;
        }
        data += offset;

        if (size >= uniform->count * sizeof(struct wined3d_vec4))
        {
            GL_EXTCALL(glProgramUniform4uiv(program, uniform->location, uniform->count, (const GLuint *)data));
        }
        else
        {
            unsigned int count = min(uniform->count, ARRAY_SIZE(padded));

            memcpy(padded, data, min(size, count * sizeof(*padded)));
            memset((BYTE *)padded + size, 0, count * sizeof(*padded) - min(size, count * sizeof(*padded)));
            GL_EXTCALL(glProgramUniform4uiv(program, uniform->location, count, (const GLuint *)padded));
        }

        uniform->bo = &bo_gl->b;
        uniform->cl_serial = bo_gl->b.cl_serial;
        uniform->content_serial = bo_gl->b.content_serial;
        uniform->mirror_serial = bo_gl->b.cl_mirror_serial;
        uniform->offset = offset;
        uniform->size = size;
        uniform->loaded = true;
    }
    checkGLcall("load constant buffer uniforms");
}
