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
 * share of a program's uniform storage (wined3d_shader_cb_is_uniform,
 * cb_uniform_limit), and reads them through
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
 * wined3d_bo's cl_mirror). A buffer object bound as a constant buffer gets one
 * when it is created (wined3d_bo_gl_mirror_create), and every write that
 * reaches it from the CPU keeps it current: updates from system memory
 * (wined3d_cl_update_mirror), maps, whose written ranges are copied from the
 * mapping when it is unmapped, copies from a buffer object whose own copy is
 * current, and the new buffer object that takes a busy one's place on a
 * DISCARD map. Only after a write by the GPU (stream output, unordered access,
 * a copy from a buffer without a current copy) and before the next whole write
 * is the buffer read back. A bound range shorter than the declared array is
 * padded with zeros, as Direct3D reads past the end of a constant buffer.
 *
 * It is off unless HKCU\Software\Wine\Direct3D\ConstantBufferUniforms is
 * "y" (or WINE_D3D_CONFIG has "ConstantBufferUniforms=y").
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

/* A program whose vertex and fragment shaders' plain uniforms come to more
 * than 512 vectors together runs its vertex processing on the CPU on 10.9
 * (~/WineskinTests/perf/tools/vpfallback.c: a 64-vector fragment shader array
 * with a 448-vector vertex one stays on the GPU, with 480 it does not; each
 * stage alone may use 1024). Each stage takes half. */
#define CB_UNIFORM_STAGE_MAX 256

static unsigned int cb_uniform_limit(const struct wined3d_gl_info *gl_info, enum wined3d_shader_type type)
{
    if (type == WINED3D_SHADER_TYPE_VERTEX)
        return min(gl_info->limits.glsl_vs_float_constants, CB_UNIFORM_STAGE_MAX);
    if (type == WINED3D_SHADER_TYPE_PIXEL)
        return min(gl_info->limits.glsl_ps_float_constants, CB_UNIFORM_STAGE_MAX);
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

/* A new buffer object: one for a constant buffer gets a copy of its contents in
 * system memory, current from the first write of the whole of it on. */
void wined3d_bo_gl_mirror_create(struct wined3d_bo_gl *bo_gl)
{
    if (wined3d_settings.cb_uniforms && bo_gl->binding == GL_UNIFORM_BUFFER && bo_gl->size <= CB_MIRROR_MAX_SIZE)
        bo_gl->b.cl_mirror = malloc(bo_gl->size);
}

/* While a buffer object is mapped for writing, a copy that was current before
 * the map has the complement of the new cl_serial as its serial: it is current
 * again once the written ranges are copied at the unmap. Until then readers
 * take it as stale. */
void wined3d_bo_mirror_map(struct wined3d_bo *bo, uint32_t old_serial)
{
    if (bo->cl_mirror && old_serial && bo->cl_mirror_serial == old_serial)
        bo->cl_mirror_serial = ~bo->cl_serial;
}

/* The ranges of a map that reach the buffer object at its unmap reach the copy too. */
void wined3d_bo_gl_mirror_unmap(struct wined3d_bo_gl *bo_gl, const struct wined3d_bo_address *data,
        unsigned int range_count, const struct wined3d_range *ranges)
{
    struct wined3d_bo *bo = &bo_gl->b;
    size_t offset = (uintptr_t)data->addr;
    const BYTE *src;
    unsigned int i;
    bool whole;

    if (!bo->cl_mirror || !bo->map_ptr || !range_count || offset > bo_gl->size)
        return;
    whole = range_count == 1 && !offset && !ranges[0].offset && ranges[0].size >= bo_gl->size;
    if (!whole && bo->cl_mirror_serial != ~bo->cl_serial)
        return;
    src = (const BYTE *)bo->map_ptr + bo->buffer_offset + offset;
    for (i = 0; i < range_count; ++i)
    {
        if (ranges[i].offset > bo_gl->size - offset || ranges[i].size > bo_gl->size - offset - ranges[i].offset)
        {
            bo->cl_mirror_serial = 0;
            return;
        }
        memcpy((BYTE *)bo->cl_mirror + offset + ranges[i].offset, src + ranges[i].offset, ranges[i].size);
    }
    bo->cl_mirror_serial = bo->cl_serial;
}

/* "dst", a new buffer object, takes the place of "src" on a DISCARD map: with
 * its copy, the copy's serials and the contents' serial. */
void wined3d_bo_mirror_move(struct wined3d_bo *dst, struct wined3d_bo *src)
{
    free(dst->cl_mirror);
    dst->cl_mirror = src->cl_mirror;
    src->cl_mirror = NULL;
    dst->cl_serial = src->cl_serial;
    dst->cl_mirror_serial = src->cl_mirror_serial;
    dst->content_serial = src->content_serial;
}

/* A copy on the GPU between buffer objects: their copies follow while the
 * source's is current and the destination's was current before (its serial is
 * now a new one) or the copy writes the whole of it. */
void wined3d_bo_gl_mirror_copy(struct wined3d_bo_gl *dst, uint32_t old_serial, size_t dst_offset,
        const struct wined3d_bo_gl *src, size_t src_offset, unsigned int range_count, const struct wined3d_range *ranges)
{
    unsigned int i;
    bool whole;

    if (!dst->b.cl_mirror)
        return;
    if (!src->b.cl_mirror || src->b.cl_mirror_serial != src->b.cl_serial || src == dst)
        return;
    whole = range_count == 1 && !dst_offset && !ranges[0].offset && ranges[0].size >= dst->size;
    if (!whole && (!old_serial || dst->b.cl_mirror_serial != old_serial))
        return;
    if (dst_offset > dst->size || src_offset > src->size)
        return;
    for (i = 0; i < range_count; ++i)
    {
        if (ranges[i].offset > dst->size - dst_offset || ranges[i].size > dst->size - dst_offset - ranges[i].offset
                || ranges[i].offset > src->size - src_offset
                || ranges[i].size > src->size - src_offset - ranges[i].offset)
            return;
    }
    for (i = 0; i < range_count; ++i)
        memcpy((BYTE *)dst->b.cl_mirror + dst_offset + ranges[i].offset,
                (const BYTE *)src->b.cl_mirror + src_offset + ranges[i].offset, ranges[i].size);
    dst->b.cl_mirror_serial = dst->b.cl_serial;
}
