/*
 * WINESKIN: redundant OpenGL state call filter.
 *
 * Every OpenGL call wined3d makes crosses from Windows code to the Unix
 * library, 0.15 to 0.2 microseconds a call in a 64-bit process before OpenGL
 * does anything (0.5 where the %gs base is switched, see
 * wineskin_init_teb_slots in ntdll's unix/signal_x86_64.c). wined3d applies Direct3D
 * state an object at a time, so a game that sets the same state for every draw
 * makes it reissue the same OpenGL calls: binding the uniform buffers,
 * textures and samplers that are bound already, disabling what is disabled.
 *
 * The functions here replace a few state setting entries of the OpenGL
 * function table (wined3d_gl_filter_install). Each remembers, per OpenGL
 * context, the value it last passed on, and returns without calling OpenGL
 * when the new value is the same. Calls that change the remembered state
 * behind its setter forget it: deleting an object forgets the bindings that
 * name it, an indexed enable forgets the capability, binding a vertex array
 * forgets the attribute divisors.
 *
 * HKCU\Software\Wine\Direct3D\GLStateFilter = "n" (or WINE_D3D_CONFIG
 * "GLStateFilter=n") turns the filter off.
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

#define FILTER_CAPS 32
#define FILTER_UNITS 256
#define FILTER_TEXTURE_TARGETS 11
#define FILTER_UBO_BINDINGS 128
#define FILTER_ATTRIBS 32
#define FILTER_CONTEXTS 16

/* What one OpenGL context was last told. A zero field is "not known": names are
 * kept plus one, enumerants are never zero, and booleans as 1 and 2. */
struct gl_filter
{
    HGLRC rc;
    struct { GLenum cap; unsigned int state; } caps[FILTER_CAPS];
    unsigned int active_unit; /* index, or ~0u when not known */
    GLuint textures[FILTER_UNITS][FILTER_TEXTURE_TARGETS];
    GLuint samplers[FILTER_UNITS];
    struct { GLuint buffer; GLintptr offset; GLsizeiptr size; } ubos[FILTER_UBO_BINDINGS];
    GLuint divisors[FILTER_ATTRIBS];
    GLenum front_face, cull_face, depth_func, polygon_mode_face, polygon_mode;
    unsigned int depth_mask, color_mask;
};

static struct
{
    void (WINAPI *glEnable)(GLenum);
    void (WINAPI *glDisable)(GLenum);
    void (WINAPI *glEnablei)(GLenum, GLuint);
    void (WINAPI *glDisablei)(GLenum, GLuint);
    void (WINAPI *glActiveTexture)(GLenum);
    void (WINAPI *glBindTexture)(GLenum, GLuint);
    void (WINAPI *glDeleteTextures)(GLsizei, const GLuint *);
    void (WINAPI *glBindSampler)(GLuint, GLuint);
    void (WINAPI *glDeleteSamplers)(GLsizei, const GLuint *);
    void (WINAPI *glBindBufferBase)(GLenum, GLuint, GLuint);
    void (WINAPI *glBindBufferRange)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
    void (WINAPI *glDeleteBuffers)(GLsizei, const GLuint *);
    void (WINAPI *glVertexAttribDivisor)(GLuint, GLuint);
    void (WINAPI *glBindVertexArray)(GLuint);
    void (WINAPI *glFrontFace)(GLenum);
    void (WINAPI *glCullFace)(GLenum);
    void (WINAPI *glDepthFunc)(GLenum);
    void (WINAPI *glDepthMask)(GLboolean);
    void (WINAPI *glPolygonMode)(GLenum, GLenum);
    void (WINAPI *glColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
    void (WINAPI *glColorMaski)(GLuint, GLboolean, GLboolean, GLboolean, GLboolean);
    void (WINAPI *glCompileShader)(GLuint);
    void (WINAPI *glLinkProgram)(GLuint);
} gl;

/* A slot's filter is allocated once and never freed, and is in use while its rc is set:
 * get_filter(), which runs for every filtered OpenGL call, reads the table without a lock. */
static struct gl_filter *filters[FILTER_CONTEXTS];
static SRWLOCK filters_lock = SRWLOCK_INIT;

/* The current OpenGL context's filter state; NULL when the context is not one
 * wined3d_gl_filter_add_context() was told of, and the caller then passes its call on. */
static struct gl_filter *get_filter(void)
{
    struct gl_filter *filter;
    unsigned int i;
    HGLRC rc;

    if (!(rc = wglGetCurrentContext()))
        return NULL;

    for (i = 0; i < FILTER_CONTEXTS; ++i)
    {
        if ((filter = ReadPointerAcquire((void **)&filters[i])) && filter->rc == rc)
            return filter;
    }
    return NULL;
}

/* A context wined3d draws with was destroyed. */
void wined3d_gl_filter_remove_context(HGLRC rc)
{
    unsigned int i;

    AcquireSRWLockExclusive(&filters_lock);
    for (i = 0; i < FILTER_CONTEXTS; ++i)
    {
        if (filters[i] && filters[i]->rc == rc)
        {
            filters[i]->rc = NULL;
            break;
        }
    }
    ReleaseSRWLockExclusive(&filters_lock);
}

/* A context wined3d draws with was created: nothing is known of its state. Contexts beyond
 * FILTER_CONTEXTS are not filtered. */
void wined3d_gl_filter_add_context(HGLRC rc)
{
    struct gl_filter *filter;
    unsigned int i;

    wined3d_gl_filter_remove_context(rc);
    if (!gl.glEnable)
        return;
    AcquireSRWLockExclusive(&filters_lock);
    for (i = 0; i < FILTER_CONTEXTS; ++i)
    {
        if ((filter = filters[i]) && filter->rc)
            continue;
        if (filter)
            memset(filter, 0, sizeof(*filter));
        else if (!(filter = calloc(1, sizeof(*filter))))
            break;
        filter->active_unit = ~0u;
        WritePointerRelease((void **)&filters[i], filter);
        filter->rc = rc;
        break;
    }
    ReleaseSRWLockExclusive(&filters_lock);
}

/* Whether a capability's state is one switch for the whole context, which glEnablei() and
 * glDisablei() alone change besides glEnable() and glDisable(). */
static bool cap_is_filtered(GLenum cap)
{
    switch (cap)
    {
        case GL_BLEND:
        case GL_CULL_FACE:
        case GL_DEPTH_TEST:
        case GL_DEPTH_CLAMP:
        case GL_DITHER:
        case GL_FRAMEBUFFER_SRGB:
        case GL_MULTISAMPLE:
        case GL_POLYGON_OFFSET_FILL:
        case GL_POLYGON_OFFSET_LINE:
        case GL_POLYGON_OFFSET_POINT:
        case GL_PRIMITIVE_RESTART:
        case GL_PRIMITIVE_RESTART_FIXED_INDEX:
        case GL_PROGRAM_POINT_SIZE:
        case GL_RASTERIZER_DISCARD:
        case GL_SAMPLE_ALPHA_TO_COVERAGE:
        case GL_SAMPLE_MASK:
        case GL_SCISSOR_TEST:
        case GL_STENCIL_TEST:
        case GL_TEXTURE_CUBE_MAP_SEAMLESS:
            return true;
        default:
            return false;
    }
}

/* Capabilities */

static unsigned int *filter_cap(struct gl_filter *filter, GLenum cap)
{
    unsigned int i;

    if (!cap_is_filtered(cap))
        return NULL;
    for (i = 0; i < FILTER_CAPS; ++i)
    {
        if (filter->caps[i].cap == cap)
            return &filter->caps[i].state;
        if (!filter->caps[i].cap)
        {
            filter->caps[i].cap = cap;
            return &filter->caps[i].state;
        }
    }
    return NULL;
}

static void WINAPI filter_glEnable(GLenum cap)
{
    struct gl_filter *filter = get_filter();
    unsigned int *state;

    if (filter && (state = filter_cap(filter, cap)))
    {
        if (*state == 2)
            return;
        *state = 2;
    }
    gl.glEnable(cap);
}

static void WINAPI filter_glDisable(GLenum cap)
{
    struct gl_filter *filter = get_filter();
    unsigned int *state;

    if (filter && (state = filter_cap(filter, cap)))
    {
        if (*state == 1)
            return;
        *state = 1;
    }
    gl.glDisable(cap);
}

static void WINAPI filter_glEnablei(GLenum cap, GLuint index)
{
    struct gl_filter *filter = get_filter();
    unsigned int *state;

    if (filter && (state = filter_cap(filter, cap)))
        *state = 0;
    gl.glEnablei(cap, index);
}

static void WINAPI filter_glDisablei(GLenum cap, GLuint index)
{
    struct gl_filter *filter = get_filter();
    unsigned int *state;

    if (filter && (state = filter_cap(filter, cap)))
        *state = 0;
    gl.glDisablei(cap, index);
}

/* Textures and samplers */

static unsigned int texture_target_index(GLenum target)
{
    switch (target)
    {
        case GL_TEXTURE_1D: return 0;
        case GL_TEXTURE_2D: return 1;
        case GL_TEXTURE_3D: return 2;
        case GL_TEXTURE_CUBE_MAP: return 3;
        case GL_TEXTURE_RECTANGLE: return 4;
        case GL_TEXTURE_1D_ARRAY: return 5;
        case GL_TEXTURE_2D_ARRAY: return 6;
        case GL_TEXTURE_CUBE_MAP_ARRAY: return 7;
        case GL_TEXTURE_BUFFER: return 8;
        case GL_TEXTURE_2D_MULTISAMPLE: return 9;
        case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: return 10;
        default: return FILTER_TEXTURE_TARGETS;
    }
}

static void WINAPI filter_glActiveTexture(GLenum texture)
{
    struct gl_filter *filter = get_filter();

    if (filter)
    {
        if (filter->active_unit == texture - GL_TEXTURE0)
            return;
        filter->active_unit = texture - GL_TEXTURE0;
    }
    gl.glActiveTexture(texture);
}

static void WINAPI filter_glBindTexture(GLenum target, GLuint texture)
{
    unsigned int idx = texture_target_index(target);
    struct gl_filter *filter = get_filter();

    if (filter && filter->active_unit < FILTER_UNITS && idx < FILTER_TEXTURE_TARGETS)
    {
        if (filter->textures[filter->active_unit][idx] == texture + 1)
            return;
        filter->textures[filter->active_unit][idx] = texture + 1;
    }
    gl.glBindTexture(target, texture);
}

/* A deleted object's name can be given to a new object, and a context that shares the objects keeps
 * the deleted one bound: every context forgets its bindings of the name. */
static void WINAPI filter_glDeleteTextures(GLsizei count, const GLuint *textures)
{
    unsigned int c, unit, idx;
    struct gl_filter *filter;
    GLsizei i;

    AcquireSRWLockShared(&filters_lock);
    for (c = 0; c < FILTER_CONTEXTS; ++c)
    {
        if (!(filter = filters[c]))
            continue;
        for (unit = 0; unit < FILTER_UNITS; ++unit)
        {
            for (idx = 0; idx < FILTER_TEXTURE_TARGETS; ++idx)
            {
                if (!filter->textures[unit][idx])
                    continue;
                for (i = 0; i < count; ++i)
                {
                    if (filter->textures[unit][idx] == textures[i] + 1)
                        filter->textures[unit][idx] = 0;
                }
            }
        }
    }
    ReleaseSRWLockShared(&filters_lock);
    gl.glDeleteTextures(count, textures);
}

static void WINAPI filter_glBindSampler(GLuint unit, GLuint sampler)
{
    struct gl_filter *filter = get_filter();

    if (filter && unit < FILTER_UNITS)
    {
        if (filter->samplers[unit] == sampler + 1)
            return;
        filter->samplers[unit] = sampler + 1;
    }
    gl.glBindSampler(unit, sampler);
}

static void WINAPI filter_glDeleteSamplers(GLsizei count, const GLuint *samplers)
{
    struct gl_filter *filter;
    unsigned int c, unit;
    GLsizei i;

    AcquireSRWLockShared(&filters_lock);
    for (c = 0; c < FILTER_CONTEXTS; ++c)
    {
        if (!(filter = filters[c]))
            continue;
        for (unit = 0; unit < FILTER_UNITS; ++unit)
        {
            for (i = 0; i < count; ++i)
            {
                if (filter->samplers[unit] == samplers[i] + 1)
                    filter->samplers[unit] = 0;
            }
        }
    }
    ReleaseSRWLockShared(&filters_lock);
    gl.glDeleteSamplers(count, samplers);
}

/* Uniform buffer bindings */

static void WINAPI filter_glBindBufferBase(GLenum target, GLuint index, GLuint buffer)
{
    struct gl_filter *filter = get_filter();

    if (filter && target == GL_UNIFORM_BUFFER && index < FILTER_UBO_BINDINGS)
    {
        if (filter->ubos[index].buffer == buffer + 1 && filter->ubos[index].size == -1)
            return;
        filter->ubos[index].buffer = buffer + 1;
        filter->ubos[index].offset = 0;
        filter->ubos[index].size = -1;
    }
    gl.glBindBufferBase(target, index, buffer);
}

static void WINAPI filter_glBindBufferRange(GLenum target, GLuint index, GLuint buffer,
        GLintptr offset, GLsizeiptr size)
{
    struct gl_filter *filter = get_filter();

    if (filter && target == GL_UNIFORM_BUFFER && index < FILTER_UBO_BINDINGS)
    {
        if (filter->ubos[index].buffer == buffer + 1 && filter->ubos[index].offset == offset
                && filter->ubos[index].size == size)
            return;
        filter->ubos[index].buffer = buffer + 1;
        filter->ubos[index].offset = offset;
        filter->ubos[index].size = size;
    }
    gl.glBindBufferRange(target, index, buffer, offset, size);
}

static void WINAPI filter_glDeleteBuffers(GLsizei count, const GLuint *buffers)
{
    struct gl_filter *filter;
    unsigned int c, index;
    GLsizei i;

    AcquireSRWLockShared(&filters_lock);
    for (c = 0; c < FILTER_CONTEXTS; ++c)
    {
        if (!(filter = filters[c]))
            continue;
        for (index = 0; index < FILTER_UBO_BINDINGS; ++index)
        {
            for (i = 0; i < count; ++i)
            {
                if (filter->ubos[index].buffer == buffers[i] + 1)
                    filter->ubos[index].buffer = 0;
            }
        }
    }
    ReleaseSRWLockShared(&filters_lock);
    gl.glDeleteBuffers(count, buffers);
}

/* Vertex attribute divisors belong to the vertex array object. */

static void WINAPI filter_glVertexAttribDivisor(GLuint index, GLuint divisor)
{
    struct gl_filter *filter = get_filter();

    /* A divisor is kept plus one, 0 meaning not known; ~0u (an instance step rate of 0, all instances
     * reading the first element) has no such value and is always passed on. */
    if (filter && index < FILTER_ATTRIBS)
    {
        if (divisor != ~0u && filter->divisors[index] == divisor + 1)
            return;
        filter->divisors[index] = divisor + 1;
    }
    gl.glVertexAttribDivisor(index, divisor);
}

static void WINAPI filter_glBindVertexArray(GLuint array)
{
    struct gl_filter *filter = get_filter();

    if (filter)
        memset(filter->divisors, 0, sizeof(filter->divisors));
    gl.glBindVertexArray(array);
}

/* Rasterizer, depth and colour write state */

static void WINAPI filter_glFrontFace(GLenum mode)
{
    struct gl_filter *filter = get_filter();

    if (filter)
    {
        if (filter->front_face == mode)
            return;
        filter->front_face = mode;
    }
    gl.glFrontFace(mode);
}

static void WINAPI filter_glCullFace(GLenum mode)
{
    struct gl_filter *filter = get_filter();

    if (filter)
    {
        if (filter->cull_face == mode)
            return;
        filter->cull_face = mode;
    }
    gl.glCullFace(mode);
}

static void WINAPI filter_glDepthFunc(GLenum func)
{
    struct gl_filter *filter = get_filter();

    if (filter)
    {
        if (filter->depth_func == func)
            return;
        filter->depth_func = func;
    }
    gl.glDepthFunc(func);
}

static void WINAPI filter_glDepthMask(GLboolean flag)
{
    struct gl_filter *filter = get_filter();

    if (filter)
    {
        if (filter->depth_mask == 1u + !!flag)
            return;
        filter->depth_mask = 1u + !!flag;
    }
    gl.glDepthMask(flag);
}

static void WINAPI filter_glPolygonMode(GLenum face, GLenum mode)
{
    struct gl_filter *filter = get_filter();

    if (filter)
    {
        if (filter->polygon_mode_face == face && filter->polygon_mode == mode)
            return;
        filter->polygon_mode_face = face;
        filter->polygon_mode = mode;
    }
    gl.glPolygonMode(face, mode);
}

static void WINAPI filter_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    unsigned int mask = 0x10 | !!r | !!g << 1 | !!b << 2 | !!a << 3;
    struct gl_filter *filter = get_filter();

    if (filter)
    {
        if (filter->color_mask == mask)
            return;
        filter->color_mask = mask;
    }
    gl.glColorMask(r, g, b, a);
}

static void WINAPI filter_glColorMaski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    struct gl_filter *filter = get_filter();

    if (filter)
        filter->color_mask = 0;
    gl.glColorMaski(index, r, g, b, a);
}

/* With the d3d_perf channel on, shader compiles and program links are timed: they are what makes a
 * frame late the first time a game draws something. */

static unsigned int elapsed_us(const LARGE_INTEGER *start)
{
    LARGE_INTEGER now, freq;

    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return (now.QuadPart - start->QuadPart) * 1000000 / freq.QuadPart;
}

static void WINAPI timed_glCompileShader(GLuint shader)
{
    LARGE_INTEGER start;

    QueryPerformanceCounter(&start);
    gl.glCompileShader(shader);
    TRACE_(d3d_perf)("Compiled shader %u in %u us.\n", shader, elapsed_us(&start));
}

static void WINAPI timed_glLinkProgram(GLuint program)
{
    LARGE_INTEGER start;

    QueryPerformanceCounter(&start);
    gl.glLinkProgram(program);
    TRACE_(d3d_perf)("Linked program %u in %u us.\n", program, elapsed_us(&start));
}

/* Replaces the entries of an adapter's OpenGL function table. A setter is
 * replaced only together with every function that changes its state. */
void wined3d_gl_filter_install(struct wined3d_gl_info *gl_info)
{
    struct wined3d_gl_funcs *ops = &gl_info->gl_ops;

#define SAVE(table, name) do { if (!gl.name) gl.name = (void *)ops->table.p_##name; } while (0)
#define REPLACE(table, name) do { SAVE(table, name); ops->table.p_##name = (void *)filter_##name; } while (0)

    if (TRACE_ON(d3d_perf) && ops->ext.p_glCompileShader && ops->ext.p_glLinkProgram)
    {
        SAVE(ext, glCompileShader);
        SAVE(ext, glLinkProgram);
        ops->ext.p_glCompileShader = (void *)timed_glCompileShader;
        ops->ext.p_glLinkProgram = (void *)timed_glLinkProgram;
    }

    if (!wined3d_settings.gl_state_filter)
        return;

    if (ops->gl.p_glEnable && ops->gl.p_glDisable)
    {
        REPLACE(gl, glEnable);
        REPLACE(gl, glDisable);
        if (ops->ext.p_glEnablei) REPLACE(ext, glEnablei);
        if (ops->ext.p_glDisablei) REPLACE(ext, glDisablei);
    }
    if (ops->ext.p_glActiveTexture && ops->gl.p_glBindTexture && ops->gl.p_glDeleteTextures)
    {
        REPLACE(ext, glActiveTexture);
        REPLACE(gl, glBindTexture);
        REPLACE(gl, glDeleteTextures);
    }
    if (ops->ext.p_glBindSampler && ops->ext.p_glDeleteSamplers)
    {
        REPLACE(ext, glBindSampler);
        REPLACE(ext, glDeleteSamplers);
    }
    if (ops->ext.p_glBindBufferBase && ops->ext.p_glBindBufferRange && ops->ext.p_glDeleteBuffers)
    {
        REPLACE(ext, glBindBufferBase);
        REPLACE(ext, glBindBufferRange);
        REPLACE(ext, glDeleteBuffers);
    }
    if (ops->ext.p_glVertexAttribDivisor && ops->ext.p_glBindVertexArray)
    {
        REPLACE(ext, glVertexAttribDivisor);
        REPLACE(ext, glBindVertexArray);
    }
    if (ops->gl.p_glFrontFace) REPLACE(gl, glFrontFace);
    if (ops->gl.p_glCullFace) REPLACE(gl, glCullFace);
    if (ops->gl.p_glDepthFunc) REPLACE(gl, glDepthFunc);
    if (ops->gl.p_glDepthMask) REPLACE(gl, glDepthMask);
    if (ops->gl.p_glPolygonMode) REPLACE(gl, glPolygonMode);
    if (ops->gl.p_glColorMask)
    {
        REPLACE(gl, glColorMask);
        if (ops->ext.p_glColorMaski) REPLACE(ext, glColorMaski);
    }

#undef REPLACE
#undef SAVE

    TRACE("Installed the OpenGL state filter.\n");
}
