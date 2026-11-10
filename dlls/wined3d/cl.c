/*
 * WINESKIN: compute shaders on OpenCL 1.2 with OpenGL sharing (macOS).
 *
 * The PE side (cl_compute.c) translates a compute shader to an OpenCL C kernel
 * and describes each dispatch's arguments; this file builds the kernels and
 * runs them. The OpenCL context shares the current OpenGL context's share
 * group, so kernels read and write wined3d's buffers and textures in place:
 * an OpenCL memory object is made for each OpenGL object (and texture level)
 * the first time a kernel uses it, and lives until the PE side reports the
 * OpenGL object deleted (unix_cl_forget).
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#ifdef __APPLE__

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>
#include <OpenCL/opencl.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLDevice.h>
#include <OpenGL/gl.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "unixlib.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d_cl);

/* The OpenCL memory object of an OpenGL buffer or texture level. "name" is
 * the name its group (a wined3d device) has for it, "host_name" the host
 * OpenGL's; see struct wined3d_cl_register_params. */
struct cl_mem_entry
{
    struct cl_mem_entry *next;
    UINT64 group;
    UINT32 type, name, target, level, host_name;
    cl_mem mem;
};

#define CL_MEM_HASH_SIZE 1024

#define CL_HOST_RING_SIZE (8u << 20)
#define CL_HOST_RING_SEGMENTS 8
#define CL_HOST_SEGMENT_SIZE (CL_HOST_RING_SIZE / CL_HOST_RING_SEGMENTS)
#define CL_HOST_ALIGN 256

#define CL_BATCH_MAX_DISPATCHES 128
#define CL_BATCH_MAX_ARGS 4096
#define CL_BATCH_MAX_OBJECTS 256

/* An argument's value as clSetKernelArg() takes it: the bytes of a value, or
 * a memory object or sampler. */
struct cl_batch_arg
{
    size_t size;
    BYTE data[WINED3D_CL_MAX_ARG_DATA];
};

struct cl_batch_dispatch
{
    struct cl_kernel *kernel;
    unsigned int arg_start, arg_count;
    size_t global[3], local[3];
};

struct cl_sampler_entry
{
    struct cl_sampler_entry *next;
    struct wined3d_cl_sampler_desc desc;
    cl_sampler sampler;
};

struct cl_null_image
{
    struct cl_null_image *next;
    UINT32 image_type, channel;
    cl_mem mem;
};

struct cl_device_context
{
    struct cl_device_context *next;
    CGLShareGroupObj share_group;
    cl_context context;
    cl_device_id device;
    cl_command_queue queue;
    cl_mem null_buffer;
    struct cl_mem_entry *mems[CL_MEM_HASH_SIZE];
    struct cl_sampler_entry *samplers;
    struct cl_null_image *null_images;
    BOOL pending;
    /* cl_khr_gl_event makes clEnqueueAcquireGLObjects() and
     * clEnqueueReleaseGLObjects() synchronise with the OpenGL context bound
     * on the calling thread: the acquire waits for that context's commands,
     * and its later commands see the kernel's results. That is the
     * extension's guarantee, and what dispatches rest on. An explicit event
     * from an OpenGL fence (clCreateEventFromGLsyncKHR) before the acquire
     * orders the same thing and costs a sixth of Bit.Trip Rerunner's frame
     * rate. Without the extension each dispatch finishes OpenGL first and
     * OpenCL afterwards. */
    BOOL implicit_sync;
    /* The copies of WINED3D_CL_ARG_HOST_DATA: system memory is copied to
     * host_staging and written from there to the same place in host_ring,
     * which kernels read. Each of the ring's segments, when the copies move
     * on to the next, gets a marker; copies come back to a segment only once
     * its marker has completed, that is, once the writes from it and the
     * kernels that read it have run. */
    cl_mem host_ring;
    BYTE *host_staging;
    size_t host_pos;
    unsigned int host_segment;
    cl_event host_markers[CL_HOST_RING_SEGMENTS];
    /* The open batch (wined3d_cl_dispatch_params.batch): dispatches whose
     * kernels are enqueued together by batch_flush(), the values of their
     * arguments, the OpenGL objects they use, and the range of host_staging
     * their copies take. One acquire and one release of the OpenGL objects
     * serve them all; each acquire or release, like each glFlush(), makes
     * the driver walk the objects in flight, which with a frame of Bit.Trip
     * Rerunner's costs about 0.15 ms. */
    struct cl_batch_dispatch *batch;
    unsigned int batch_count;
    struct cl_batch_arg *batch_args;
    unsigned int batch_arg_count;
    cl_mem batch_objects[CL_BATCH_MAX_OBJECTS];
    unsigned int batch_object_count;
    size_t batch_host_start, batch_host_end;
    LONG refcount; /* the list's, and each kernel's */
    BOOL released; /* out of the list: no group uses it */
    /* The groups that have registered objects or built kernels. */
    UINT64 *groups;
    unsigned int group_count;
};

struct cl_kernel
{
    struct cl_device_context *ctx;
    cl_program program;
    cl_kernel kernel; /* NULL until a build in the background has finished */
    struct cl_build_job *job; /* that build */
    unsigned int group_threads;
};

static void batch_flush(struct cl_device_context *ctx);

static pthread_mutex_t cl_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct cl_device_context *cl_contexts;

/* Whether the OpenCL device CGL names for the OpenGL context can run
 * wined3d's kernels: a GPU (a Mac whose GPU has no OpenCL, such as one with
 * Intel HD Graphics 3000, can name the CPU, which copies every shared
 * object through system memory), with images, OpenCL C 1.2, and OpenGL
 * sharing. WINESKIN_CL_ANY_DEVICE=1 accepts a device of any type. */
static BOOL device_usable(cl_device_id device, const char *name)
{
    char version[128], extensions[4096];
    cl_device_type type = 0;
    cl_bool images = CL_FALSE;
    const char *why = NULL;
    int major = 0, minor = 0;

    version[0] = extensions[0] = 0;
    clGetDeviceInfo(device, CL_DEVICE_TYPE, sizeof(type), &type, NULL);
    clGetDeviceInfo(device, CL_DEVICE_IMAGE_SUPPORT, sizeof(images), &images, NULL);
    clGetDeviceInfo(device, CL_DEVICE_OPENCL_C_VERSION, sizeof(version), version, NULL);
    version[sizeof(version) - 1] = 0;
    if (clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, sizeof(extensions), extensions, NULL))
        extensions[0] = 0;
    extensions[sizeof(extensions) - 1] = 0;
    sscanf(version, "OpenCL C %d.%d", &major, &minor);

    if (!(type & CL_DEVICE_TYPE_GPU) && !getenv("WINESKIN_CL_ANY_DEVICE"))
        why = "it is not a GPU";
    else if (!images)
        why = "it has no images";
    else if (major < 1 || (major == 1 && minor < 2))
        why = "its OpenCL C is older than 1.2";
    else if (!strstr(extensions, "cl_APPLE_gl_sharing") && !strstr(extensions, "cl_khr_gl_sharing"))
        why = "it has no OpenGL sharing";
    if (why)
    {
        ERR("The OpenCL device for the OpenGL context, %s (%s), can't run compute shaders: %s.\n",
                debugstr_a(name), debugstr_a(version), why);
        return FALSE;
    }
    return TRUE;
}

/* The OpenCL context for the current OpenGL context's host share group. */
static struct cl_device_context *get_device_context(void)
{
    struct cl_device_context *ctx;
    cl_context_properties props[3];
    CGLShareGroupObj share_group;
    CGLContextObj gl_context;
    cl_device_id device;
    char name[256];
    cl_int err;
    size_t size;

    if (!(gl_context = CGLGetCurrentContext()))
    {
        ERR("No current OpenGL context.\n");
        return NULL;
    }
    share_group = CGLGetShareGroup(gl_context);

    for (ctx = cl_contexts; ctx; ctx = ctx->next)
    {
        if (ctx->share_group == share_group)
            return ctx;
    }

    props[0] = CL_CONTEXT_PROPERTY_USE_CGL_SHAREGROUP_APPLE;
    props[1] = (cl_context_properties)share_group;
    props[2] = 0;
    if (!(ctx = calloc(1, sizeof(*ctx))))
        return NULL;
    if (!(ctx->context = clCreateContext(props, 0, NULL, NULL, NULL, &err)))
    {
        ERR("Failed to create an OpenCL context, error %d.\n", err);
        free(ctx);
        return NULL;
    }

    /* The device that drives the current virtual screen, which for an
     * accelerated renderer is its GPU. */
    if ((err = clGetGLContextInfoAPPLE(ctx->context, gl_context,
            CL_CGL_DEVICE_FOR_CURRENT_VIRTUAL_SCREEN_APPLE, sizeof(device), &device, &size)) || !size)
    {
        ERR("Failed to find the OpenCL device for the OpenGL context, error %d.\n", err);
        clReleaseContext(ctx->context);
        free(ctx);
        return NULL;
    }
    ctx->device = device;
    clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(name), name, NULL);
    name[sizeof(name) - 1] = 0;
    TRACE("Using OpenCL device %s.\n", debugstr_a(name));

    if (!device_usable(device, name))
    {
        clReleaseContext(ctx->context);
        free(ctx);
        return NULL;
    }

    if (!(ctx->queue = clCreateCommandQueue(ctx->context, device, 0, &err)))
    {
        ERR("Failed to create an OpenCL command queue, error %d.\n", err);
        clReleaseContext(ctx->context);
        free(ctx);
        return NULL;
    }

    {
        static const UINT32 zero[64];

        ctx->null_buffer = clCreateBuffer(ctx->context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                sizeof(zero), (void *)zero, &err);
    }

    {
        char extensions[4096];

        if (clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, sizeof(extensions), extensions, NULL))
            extensions[0] = 0;
        extensions[sizeof(extensions) - 1] = 0;
        ctx->implicit_sync = !!strstr(extensions, "cl_khr_gl_event");
        if (!ctx->implicit_sync)
            WARN("No cl_khr_gl_event; dispatches wait for OpenGL and OpenCL.\n");
    }

    ctx->share_group = share_group;
    ctx->refcount = 1;
    ctx->next = cl_contexts;
    cl_contexts = ctx;
    return ctx;
}

/* Called with cl_mutex held. */
static void cl_context_add_group(struct cl_device_context *ctx, UINT64 group)
{
    unsigned int i;
    UINT64 *groups;

    for (i = 0; i < ctx->group_count; ++i)
    {
        if (ctx->groups[i] == group)
            return;
    }
    if (!(groups = realloc(ctx->groups, (ctx->group_count + 1) * sizeof(*groups))))
        return;
    ctx->groups = groups;
    ctx->groups[ctx->group_count++] = group;
}

/* Called with cl_mutex held. */
static void cl_context_release(struct cl_device_context *ctx)
{
    struct cl_sampler_entry *sampler;
    struct cl_null_image *image;
    unsigned int i;

    if (--ctx->refcount)
        return;

    while ((sampler = ctx->samplers))
    {
        ctx->samplers = sampler->next;
        clReleaseSampler(sampler->sampler);
        free(sampler);
    }
    while ((image = ctx->null_images))
    {
        ctx->null_images = image->next;
        clReleaseMemObject(image->mem);
        free(image);
    }
    if (ctx->null_buffer)
        clReleaseMemObject(ctx->null_buffer);
    for (i = 0; i < CL_HOST_RING_SEGMENTS; ++i)
    {
        if (ctx->host_markers[i])
            clReleaseEvent(ctx->host_markers[i]);
    }
    if (ctx->host_ring)
        clReleaseMemObject(ctx->host_ring);
    free(ctx->host_staging);
    free(ctx->batch);
    free(ctx->batch_args);
    free(ctx->groups);
    clReleaseCommandQueue(ctx->queue);
    clReleaseContext(ctx->context);
    free(ctx);
}

/* OpenGL 3.0 framebuffer functions, which 10.9's OpenGL/gl.h declares only as EXT ones. */
extern void glGenFramebuffers(GLsizei n, GLuint *framebuffers);
extern void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers);
extern void glBindFramebuffer(GLenum target, GLuint framebuffer);
extern void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
#define WINED3D_GL_DRAW_FRAMEBUFFER 0x8ca9
#define WINED3D_GL_DRAW_FRAMEBUFFER_BINDING 0x8ca6
#define WINED3D_GL_DEPTH_ATTACHMENT 0x8d00
#define WINED3D_GL_DEPTH_STENCIL_ATTACHMENT 0x821a
#define WINED3D_GL_RASTERIZER_DISCARD 0x8c89

/* Whether a kernel reads the depth OpenGL rendered into a texture, through
 * an image2d_t with normalised coordinates, as wined3d's kernels read it
 * (shader_cl_image_type() and shader_cl_ld() in glsl_cl.h). The texture's
 * quadrants are cleared to four depths, and each is read at a texel centre.
 * A depth texture shares as an image of CL_DEPTH channel order, which OpenCL
 * C reads as an image2d_depth_t (cl_khr_depth_images); NVIDIA's OpenCL on
 * 10.9 reads it as an image2d_t, and its image2d_depth_t reads the wrong
 * texel (~/WineskinTests/d3d11tools/probe/cldepth.c). Intel's for HD Graphics
 * 5000 returns 0 either way (~/WineskinTests/probe-air). */
static BOOL test_depth_read(struct cl_device_context *ctx, GLuint texture, BOOL stencil, cl_mem mem)
{
    static const char source[] =
        "__constant sampler_t s = CLK_NORMALIZED_COORDS_TRUE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;\n"
        "__kernel void k(read_only image2d_t img, __global float *o)\n"
        "{\n"
        "    int i = get_global_id(0);\n"
        "    o[i] = read_imagef(img, s, (float2)(i & 1 ? 0.875f : 0.125f, i & 2 ? 0.875f : 0.125f)).x;\n"
        "}\n";
    static const float depths[4] = {0.125f, 0.375f, 0.625f, 0.875f};
    GLint old_fbo = 0, old_box[4], old_mask = 0;
    GLboolean old_scissor, old_discard;
    const char *src = source;
    GLfloat old_clear = 0.0f;
    float result[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    size_t four = 4;
    BOOL ok = FALSE;
    cl_program program;
    cl_kernel kernel;
    cl_mem out;
    GLuint fbo;
    cl_int err;
    int i;

    glGetIntegerv(WINED3D_GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv(GL_SCISSOR_BOX, old_box);
    glGetIntegerv(GL_DEPTH_WRITEMASK, &old_mask);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &old_clear);
    old_scissor = glIsEnabled(GL_SCISSOR_TEST);
    old_discard = glIsEnabled(WINED3D_GL_RASTERIZER_DISCARD);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(WINED3D_GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(WINED3D_GL_DRAW_FRAMEBUFFER,
            stencil ? WINED3D_GL_DEPTH_STENCIL_ATTACHMENT : WINED3D_GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texture, 0);
    glDepthMask(GL_TRUE);
    glDisable(WINED3D_GL_RASTERIZER_DISCARD);
    glEnable(GL_SCISSOR_TEST);
    for (i = 0; i < 4; ++i)
    {
        glScissor(i & 1 ? 2 : 0, i & 2 ? 2 : 0, 2, 2);
        glClearDepth(depths[i]);
        glClear(GL_DEPTH_BUFFER_BIT);
    }
    glFinish();

    glBindFramebuffer(WINED3D_GL_DRAW_FRAMEBUFFER, old_fbo);
    glDeleteFramebuffers(1, &fbo);
    glScissor(old_box[0], old_box[1], old_box[2], old_box[3]);
    if (!old_scissor)
        glDisable(GL_SCISSOR_TEST);
    if (old_discard)
        glEnable(WINED3D_GL_RASTERIZER_DISCARD);
    glDepthMask(old_mask ? GL_TRUE : GL_FALSE);
    glClearDepth(old_clear);

    if (!(program = clCreateProgramWithSource(ctx->context, 1, &src, NULL, &err)))
        return FALSE;
    if (!clBuildProgram(program, 1, &ctx->device, NULL, NULL, NULL)
            && (kernel = clCreateKernel(program, "k", &err)))
    {
        if ((out = clCreateBuffer(ctx->context, CL_MEM_READ_WRITE, sizeof(result), NULL, &err)))
        {
            clSetKernelArg(kernel, 0, sizeof(mem), &mem);
            clSetKernelArg(kernel, 1, sizeof(out), &out);
            if (!clEnqueueAcquireGLObjects(ctx->queue, 1, &mem, 0, NULL, NULL))
            {
                clEnqueueNDRangeKernel(ctx->queue, kernel, 1, NULL, &four, NULL, 0, NULL, NULL);
                clEnqueueReleaseGLObjects(ctx->queue, 1, &mem, 0, NULL, NULL);
                if (!clEnqueueReadBuffer(ctx->queue, out, CL_TRUE, 0, sizeof(result), result, 0, NULL, NULL))
                {
                    ok = TRUE;
                    for (i = 0; i < 4; ++i)
                    {
                        if (fabsf(result[i] - depths[i]) > 1.0e-3f)
                            ok = FALSE;
                    }
                }
            }
            clFinish(ctx->queue);
            clReleaseMemObject(out);
        }
        clReleaseKernel(kernel);
    }
    clReleaseProgram(program);
    TRACE("Depth read through OpenCL: %.4f %.4f %.4f %.4f (expected 0.125 0.375 0.625 0.875).\n",
            result[0], result[1], result[2], result[3]);
    return ok;
}

/* Which of the formats OpenCL makes images of: each is tried on a 4x4 OpenGL
 * texture, made and deleted on the host with the current context's bindings
 * and the state its tests change restored. Drivers differ: the OpenCL
 * specification lists RGBA, BGRA and, with OpenCL 1.2, R and RG formats, and
 * each driver adds some of its own. A depth format counts only if a kernel
 * reads what OpenGL rendered (test_depth_read()); a depth texture of another
 * reaches kernels through an R32F shadow. WINESKIN_CL_NO_DEPTH=1 shares no
 * depth format. */
static UINT64 test_shareable_formats(struct cl_device_context *ctx, const struct wined3d_cl_init_params *params)
{
    GLint old_texture = 0, old_unpack = 0;
    BOOL depth_tested = FALSE, depth_ok = FALSE, no_depth = !!getenv("WINESKIN_CL_NO_DEPTH");
    UINT64 mask = 0;
    unsigned int i;
    GLuint texture;
    cl_int err;
    cl_mem mem;

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &old_unpack);
    if (old_unpack)
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    for (i = 0; i < params->format_count && i < WINED3D_CL_MAX_FORMATS; ++i)
    {
        const struct wined3d_cl_format *f = &params->formats[i];

        if (f->depth && (no_depth || (depth_tested && !depth_ok)))
            continue;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        while (glGetError());
        glTexImage2D(GL_TEXTURE_2D, 0, f->internal, 4, 4, 0, f->format, f->type, NULL);
        if (!glGetError() && (mem = clCreateFromGLTexture(ctx->context, CL_MEM_READ_WRITE, GL_TEXTURE_2D, 0, texture, &err)))
        {
            if (!f->depth || test_depth_read(ctx, texture, f->format != GL_DEPTH_COMPONENT, mem))
                mask |= (UINT64)1 << i;
            else
                TRACE("OpenCL reads no depth from internal format %#x.\n", f->internal);
            if (f->depth && !depth_tested)
            {
                depth_tested = TRUE;
                depth_ok = !!(mask & ((UINT64)1 << i));
            }
            clReleaseMemObject(mem);
        }
        else
        {
            TRACE("OpenCL makes no image of internal format %#x.\n", f->internal);
        }
        glBindTexture(GL_TEXTURE_2D, old_texture);
        glDeleteTextures(1, &texture);
    }
    if (old_unpack)
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, old_unpack);
    while (glGetError());
    TRACE("Shareable formats %#llx.\n", (unsigned long long)mask);
    return mask;
}

static NTSTATUS cl_init(void *args)
{
    struct wined3d_cl_init_params *params = args;
    struct cl_device_context *ctx;
    char extensions[4096];
    cl_ulong local_mem;
    cl_uint write_images;
    size_t wg_size;

    pthread_mutex_lock(&cl_mutex);
    if (!(ctx = get_device_context()))
    {
        pthread_mutex_unlock(&cl_mutex);
        return STATUS_NOT_SUPPORTED;
    }

    clGetDeviceInfo(ctx->device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(wg_size), &wg_size, NULL);
    clGetDeviceInfo(ctx->device, CL_DEVICE_LOCAL_MEM_SIZE, sizeof(local_mem), &local_mem, NULL);
    clGetDeviceInfo(ctx->device, CL_DEVICE_MAX_WRITE_IMAGE_ARGS, sizeof(write_images), &write_images, NULL);
    if (clGetDeviceInfo(ctx->device, CL_DEVICE_EXTENSIONS, sizeof(extensions), extensions, NULL))
        extensions[0] = 0;
    extensions[sizeof(extensions) - 1] = 0;
    params->shareable = test_shareable_formats(ctx, params);
    pthread_mutex_unlock(&cl_mutex);

    params->max_work_group_size = wg_size;
    params->local_mem_size = local_mem;
    params->max_write_images = write_images;
    params->caps = 0;
    if (strstr(extensions, "cl_khr_gl_depth_images"))
        params->caps |= WINED3D_CL_CAP_DEPTH_IMAGES;
    if (strstr(extensions, "cl_khr_gl_msaa_sharing"))
        params->caps |= WINED3D_CL_CAP_MSAA_IMAGES;
    if (strstr(extensions, "cl_khr_3d_image_writes"))
        params->caps |= WINED3D_CL_CAP_3D_WRITES;
    TRACE("Work group size %u, local memory %u, caps %#x.\n",
            params->max_work_group_size, params->local_mem_size, params->caps);

    return STATUS_SUCCESS;
}

/* Kernel builds run on a thread of their own. Apple's clBuildProgram()
 * builds before it returns, callback or not, and a driver's compiler can take
 * minutes over a large kernel and then fail: NVIDIA's on 10.9 says "Error
 * getting function data from server" after two, Intel's for HD Graphics 5000
 * gives up after 14 s on a kernel of 256 inlined format decodes
 * (~/WineskinTests/probe-air). The command stream waits for a build at most
 * WINESKIN_CL_BUILD_WAIT seconds (10 by default; 0 waits for every build to
 * end). A build still running then goes on in the background, and the
 * kernel's dispatches do nothing until it has finished. The driver's compile
 * server keeps finished builds by source, so a later run has them at once. */
static const char cl_build_options[] = "-cl-single-precision-constant -cl-denorms-are-zero";

struct cl_build_job
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    cl_program program; /* a reference of the job's own */
    cl_device_id device;
    cl_int err;
    BOOL done;
    BOOL abandoned; /* the kernel was released first; the thread frees the job */
};

static void build_job_free(struct cl_build_job *job)
{
    clReleaseProgram(job->program);
    pthread_cond_destroy(&job->cond);
    pthread_mutex_destroy(&job->mutex);
    free(job);
}

static void *build_thread(void *arg)
{
    struct cl_build_job *job = arg;
    BOOL abandoned;

    job->err = clBuildProgram(job->program, 1, &job->device, cl_build_options, NULL, NULL);
    pthread_mutex_lock(&job->mutex);
    job->done = TRUE;
    abandoned = job->abandoned;
    pthread_cond_broadcast(&job->cond);
    pthread_mutex_unlock(&job->mutex);
    if (abandoned)
        build_job_free(job);
    return NULL;
}

/* Builds "program", waiting for the build at most WINESKIN_CL_BUILD_WAIT
 * seconds. Returns NULL with the build's result in *err when the build has
 * ended, or the job of a build still running. Called with cl_mutex held,
 * which is released while the build runs. */
static struct cl_build_job *build_program(cl_program program, cl_device_id device, cl_int *err)
{
    static double wait = -1.0;
    struct cl_build_job *job;
    struct timespec deadline;
    struct timeval now;
    pthread_attr_t attr;
    pthread_t thread;
    BOOL done;

    if (wait < 0.0)
    {
        const char *env = getenv("WINESKIN_CL_BUILD_WAIT");

        wait = env ? strtod(env, NULL) : 10.0;
        if (wait < 0.0)
            wait = 0.0;
    }

    if (!(job = calloc(1, sizeof(*job))))
    {
        *err = clBuildProgram(program, 1, &device, cl_build_options, NULL, NULL);
        return NULL;
    }
    pthread_mutex_init(&job->mutex, NULL);
    pthread_cond_init(&job->cond, NULL);
    clRetainProgram(program);
    job->program = program;
    job->device = device;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* The drivers' compilers recurse deeply over large kernels. */
    pthread_attr_setstacksize(&attr, 16 << 20);
    if (pthread_create(&thread, &attr, build_thread, job))
    {
        pthread_attr_destroy(&attr);
        build_job_free(job);
        *err = clBuildProgram(program, 1, &device, cl_build_options, NULL, NULL);
        return NULL;
    }
    pthread_attr_destroy(&attr);

    pthread_mutex_unlock(&cl_mutex);
    gettimeofday(&now, NULL);
    deadline.tv_sec = now.tv_sec + (time_t)wait;
    deadline.tv_nsec = now.tv_usec * 1000 + (long)((wait - (time_t)wait) * 1e9);
    if (deadline.tv_nsec >= 1000000000)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&job->mutex);
    while (!job->done)
    {
        if (wait == 0.0)
            pthread_cond_wait(&job->cond, &job->mutex);
        else if (pthread_cond_timedwait(&job->cond, &job->mutex, &deadline) == ETIMEDOUT)
            break;
    }
    done = job->done;
    pthread_mutex_unlock(&job->mutex);
    pthread_mutex_lock(&cl_mutex);

    if (!done)
    {
        WARN("A kernel build has taken %g s; the kernel's dispatches do nothing until it has finished.\n", wait);
        return job;
    }
    *err = job->err;
    build_job_free(job);
    return NULL;
}

static void print_build_log(cl_program program, cl_device_id device, cl_int err)
{
    size_t log_size;
    char *log;

    if (!clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size)
            && log_size > 1 && (log = malloc(log_size + 1)))
    {
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, log_size, log, NULL);
        log[log_size] = 0;
        if (err)
            ERR("Failed to build an OpenCL kernel, error %d:\n", err);
        if (err || TRACE_ON(d3d_cl))
        {
            char *line = log, *end;

            /* A line at a time: Wine's debug output takes a few hundred bytes per call. */
            while (*line)
            {
                if (!(end = strchr(line, '\n')))
                    end = line + strlen(line);
                if (err)
                    ERR("    %.*s\n", (int)min(end - line, 400), line);
                else
                    TRACE("    %.*s\n", (int)min(end - line, 400), line);
                line = *end ? end + 1 : end;
            }
        }
        free(log);
    }
    else if (err)
    {
        ERR("Failed to build an OpenCL kernel, error %d.\n", err);
    }
}

/* Makes the kernel of a built program; false if it has none, or a work
 * group smaller than the shader's thread group. */
static BOOL create_kernel(struct cl_kernel *kernel, size_t *wg)
{
    cl_int err;

    if (!(kernel->kernel = clCreateKernel(kernel->program, "wined3d_cs", &err)))
    {
        ERR("Failed to create the OpenCL kernel, error %d.\n", err);
        return FALSE;
    }
    if (clGetKernelWorkGroupInfo(kernel->kernel, kernel->ctx->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof(*wg), wg, NULL))
        *wg = 0;
    return TRUE;
}

/* Whether a kernel can run: one whose build went on in the background is
 * made once the build has finished. Called with cl_mutex held. */
static BOOL kernel_ready(struct cl_kernel *kernel)
{
    struct cl_build_job *job = kernel->job;
    size_t wg;
    BOOL done;

    if (kernel->kernel)
        return TRUE;
    if (!job)
        return FALSE;
    pthread_mutex_lock(&job->mutex);
    done = job->done;
    pthread_mutex_unlock(&job->mutex);
    if (!done)
        return FALSE;

    kernel->job = NULL;
    print_build_log(kernel->program, kernel->ctx->device, job->err);
    if (!job->err && create_kernel(kernel, &wg))
    {
        if (wg >= kernel->group_threads)
        {
            TRACE("Kernel %p built in the background.\n", kernel);
        }
        else
        {
            FIXME("The kernel's work group size %zu is smaller than its thread group (%u threads).\n",
                    wg, kernel->group_threads);
            clReleaseKernel(kernel->kernel);
            kernel->kernel = NULL;
        }
    }
    build_job_free(job);
    return !!kernel->kernel;
}

static NTSTATUS cl_build(void *args)
{
    struct wined3d_cl_build_params *params = args;
    const char *source = (const char *)(ULONG_PTR)params->source;
    struct cl_build_job *job = NULL;
    struct cl_device_context *ctx;
    struct cl_kernel *kernel;
    cl_program program;
    cl_int err = 0;
    size_t wg;

    params->kernel = 0;

    pthread_mutex_lock(&cl_mutex);
    if (!(ctx = get_device_context()))
    {
        pthread_mutex_unlock(&cl_mutex);
        return STATUS_NOT_SUPPORTED;
    }

    if (!(program = clCreateProgramWithSource(ctx->context, 1, &source, NULL, &err)))
    {
        pthread_mutex_unlock(&cl_mutex);
        ERR("Failed to create an OpenCL program, error %d.\n", err);
        return STATUS_UNSUCCESSFUL;
    }

    job = build_program(program, ctx->device, &err);
    {
        /* WINESKIN_CL_DUMP names a directory that receives the source of
         * every kernel; the first line of a compute shader's kernel
         * names the shader. */
        const char *dir = getenv("WINESKIN_CL_DUMP");
        static unsigned int count;
        char path[1024];
        FILE *f;

        if (dir && strlen(dir) < sizeof(path) - 40)
        {
            snprintf(path, sizeof(path), "%s/kernel-%u%s.cl", dir, count++,
                    job ? "-slow" : err ? "-failed" : "");
            if ((f = fopen(path, "w")))
            {
                fputs(source, f);
                fclose(f);
            }
        }
    }
    if (!job)
        print_build_log(program, ctx->device, err);
    if (err)
    {
        clReleaseProgram(program);
        pthread_mutex_unlock(&cl_mutex);
        return STATUS_UNSUCCESSFUL;
    }

    if (!(kernel = calloc(1, sizeof(*kernel))))
    {
        if (job)
        {
            BOOL done;

            pthread_mutex_lock(&job->mutex);
            done = job->done;
            job->abandoned = !done;
            pthread_mutex_unlock(&job->mutex);
            if (done)
                build_job_free(job);
        }
        clReleaseProgram(program);
        pthread_mutex_unlock(&cl_mutex);
        return STATUS_NO_MEMORY;
    }
    kernel->ctx = ctx;
    kernel->program = program;
    kernel->job = job;
    kernel->group_threads = params->group_size[0] * params->group_size[1] * params->group_size[2];
    if (job)
    {
        /* Checked when the build has finished (kernel_ready()). */
        clGetDeviceInfo(ctx->device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(wg), &wg, NULL);
    }
    else if (!create_kernel(kernel, &wg))
    {
        clReleaseProgram(program);
        free(kernel);
        pthread_mutex_unlock(&cl_mutex);
        return STATUS_UNSUCCESSFUL;
    }
    ++ctx->refcount;
    cl_context_add_group(ctx, params->group);
    params->work_group_size = wg;
    pthread_mutex_unlock(&cl_mutex);

    params->kernel = (ULONG_PTR)kernel;
    return STATUS_SUCCESS;
}

static NTSTATUS cl_release_kernel(void *args)
{
    const struct wined3d_cl_release_kernel_params *params = args;
    struct cl_kernel *kernel = (struct cl_kernel *)(ULONG_PTR)params->kernel;
    struct cl_build_job *job;

    if (!kernel)
        return STATUS_SUCCESS;
    pthread_mutex_lock(&cl_mutex);
    batch_flush(kernel->ctx);
    if ((job = kernel->job))
    {
        BOOL done;

        pthread_mutex_lock(&job->mutex);
        done = job->done;
        job->abandoned = !done;
        pthread_mutex_unlock(&job->mutex);
        if (done)
            build_job_free(job);
    }
    if (kernel->kernel)
        clReleaseKernel(kernel->kernel);
    clReleaseProgram(kernel->program);
    cl_context_release(kernel->ctx);
    pthread_mutex_unlock(&cl_mutex);
    free(kernel);
    return STATUS_SUCCESS;
}

/* WINESKIN_CL_STATS=1 prints, every 1000 dispatches, the time each part of a
 * dispatch took on average, in microseconds. */
enum cl_stat
{
    CL_STAT_ARGS,
    CL_STAT_WRITE,
    CL_STAT_GL_FLUSH,
    CL_STAT_ACQUIRE,
    CL_STAT_ENQUEUE,
    CL_STAT_RELEASE,
    CL_STAT_CL_FLUSH,
    CL_STAT_REGISTER,
    CL_STAT_COUNT
};

static int cl_stats = -1;
static uint64_t cl_stat_time[CL_STAT_COUNT], cl_stat_last;
static unsigned int cl_stat_dispatches, cl_stat_registers, cl_stat_host, cl_stat_gl_buffers;
static unsigned int cl_stat_batches;

static void cl_stat(enum cl_stat stat)
{
    uint64_t now;

    if (!cl_stats)
        return;
    now = mach_absolute_time();
    if (stat < CL_STAT_COUNT)
        cl_stat_time[stat] += now - cl_stat_last;
    cl_stat_last = now;
}

static void cl_stats_dispatch_done(void)
{
    static const char *names[CL_STAT_COUNT] = {"args", "write", "glFlush", "acquire", "enqueue", "release", "clFlush", "register"};
    mach_timebase_info_data_t tb;
    char line[512];
    unsigned int i, len;

    if (++cl_stat_dispatches < 1000)
        return;
    mach_timebase_info(&tb);
    len = snprintf(line, sizeof(line), "CLSTATS time per dispatch (us):");
    for (i = 0; i < CL_STAT_COUNT; ++i)
    {
        len += snprintf(line + len, sizeof(line) - len, " %s %.1f", names[i],
                cl_stat_time[i] * (double)tb.numer / tb.denom / 1000.0 / cl_stat_dispatches);
        cl_stat_time[i] = 0;
    }
    snprintf(line + len, sizeof(line) - len, "; per dispatch: batches %.2f, registers %.2f, host copies %.2f,"
            " buffers %.2f\n", cl_stat_batches / 1000.0, cl_stat_registers / 1000.0, cl_stat_host / 1000.0,
            cl_stat_gl_buffers / 1000.0);
    fputs(line, stderr);
    cl_stat_dispatches = cl_stat_registers = cl_stat_host = cl_stat_gl_buffers = 0;
    cl_stat_batches = 0;
}

static unsigned int mem_hash(UINT32 name)
{
    return (name * 2654435761u) >> 22;
}

static UINT32 mem_type(const struct wined3d_cl_arg *arg)
{
    return arg->type == WINED3D_CL_ARG_GL_BUFFER ? WINED3D_CL_OBJECT_BUFFER : WINED3D_CL_OBJECT_TEXTURE;
}

/* The memory object of a registered OpenGL object, or NULL.
 *
 * A buffer's memory object reads what the buffer holds when it is acquired,
 * however the buffer was written since it was made (glBufferSubData(),
 * glBufferData(), a map that invalidates it; checked in Bit.Trip Rerunner by
 * reading 15800 such buffers through their memory object and through a new
 * one, and natively by ~/WineskinTests/d3d11tools/probe/clrename.c), so it
 * lives as long as the buffer. */
static cl_mem get_gl_mem(struct cl_device_context *ctx, UINT64 group, const struct wined3d_cl_arg *arg)
{
    UINT32 type = mem_type(arg);
    struct cl_mem_entry *entry;

    for (entry = ctx->mems[mem_hash(arg->gl_name) % CL_MEM_HASH_SIZE]; entry; entry = entry->next)
    {
        if (entry->group != group || entry->type != type || entry->name != arg->gl_name)
            continue;
        if (type == WINED3D_CL_OBJECT_BUFFER)
            return entry->mem;
        if (entry->target == arg->gl_target && entry->level == arg->level)
            return entry->mem;
    }
    return NULL;
}

/* The host OpenGL's query for the texture bound to the target an image
 * target belongs to. */
static GLenum texture_binding_query(GLenum target)
{
    switch (target)
    {
        case 0x0de0 /* GL_TEXTURE_1D */: return 0x8068;
        case 0x0de1 /* GL_TEXTURE_2D */: return 0x8069;
        case 0x806f /* GL_TEXTURE_3D */: return 0x806a;
        case 0x84f5 /* GL_TEXTURE_RECTANGLE */: return 0x84f6;
        case 0x8c18 /* GL_TEXTURE_1D_ARRAY */: return 0x8c1c;
        case 0x8c1a /* GL_TEXTURE_2D_ARRAY */: return 0x8c1d;
        case 0x9100 /* GL_TEXTURE_2D_MULTISAMPLE */: return 0x9104;
        case 0x9102 /* GL_TEXTURE_2D_MULTISAMPLE_ARRAY */: return 0x9105;
        case 0x9009 /* GL_TEXTURE_CUBE_MAP_ARRAY */: return 0x900a;
        default:
            if (target >= 0x8515 && target <= 0x851a) /* cube map faces */
                return 0x8514;
            return 0;
    }
}

static NTSTATUS cl_register(void *args)
{
    const struct wined3d_cl_register_params *params = args;
    struct cl_mem_entry *entry, **bucket;
    struct cl_device_context *ctx;
    GLint host_name = 0;
    uint64_t start = 0;
    GLenum query;
    cl_int err;
    cl_mem mem;

    if (cl_stats > 0)
        start = mach_absolute_time();
    pthread_mutex_lock(&cl_mutex);
    if (!(ctx = get_device_context()))
    {
        pthread_mutex_unlock(&cl_mutex);
        return STATUS_NOT_SUPPORTED;
    }

    query = params->type == WINED3D_CL_OBJECT_BUFFER ? 0x8f36 /* GL_COPY_READ_BUFFER_BINDING */
            : texture_binding_query(params->target);
    if (query)
        glGetIntegerv(query, &host_name);
    if (!host_name)
    {
        pthread_mutex_unlock(&cl_mutex);
        ERR("No host name for %s %u (target %#x).\n",
                params->type == WINED3D_CL_OBJECT_BUFFER ? "buffer" : "texture", params->name, params->target);
        return STATUS_INVALID_PARAMETER;
    }

    if (params->type == WINED3D_CL_OBJECT_BUFFER)
        mem = clCreateFromGLBuffer(ctx->context, CL_MEM_READ_WRITE, host_name, &err);
    else
        mem = clCreateFromGLTexture(ctx->context, CL_MEM_READ_WRITE, params->target, params->level, host_name, &err);
    if (!mem)
    {
        pthread_mutex_unlock(&cl_mutex);
        WARN("Failed to share %s %u (host name %u, target %#x, level %u), error %d.\n",
                params->type == WINED3D_CL_OBJECT_BUFFER ? "buffer" : "texture", params->name,
                (unsigned int)host_name, params->target, params->level, err);
        return STATUS_UNSUCCESSFUL;
    }
    TRACE("Created OpenCL memory %p for %s %u of group %#llx (host name %u), target %#x, level %u.\n", mem,
            params->type == WINED3D_CL_OBJECT_BUFFER ? "buffer" : "texture", params->name,
            (unsigned long long)params->group, (unsigned int)host_name, params->target, params->level);

    if (!(entry = malloc(sizeof(*entry))))
    {
        clReleaseMemObject(mem);
        pthread_mutex_unlock(&cl_mutex);
        return STATUS_NO_MEMORY;
    }
    entry->group = params->group;
    entry->type = params->type;
    entry->name = params->name;
    entry->target = params->target;
    entry->level = params->level;
    entry->host_name = host_name;
    entry->mem = mem;
    bucket = &ctx->mems[mem_hash(params->name) % CL_MEM_HASH_SIZE];
    entry->next = *bucket;
    *bucket = entry;
    cl_context_add_group(ctx, params->group);
    if (start)
    {
        ++cl_stat_registers;
        cl_stat_time[CL_STAT_REGISTER] += mach_absolute_time() - start;
    }
    pthread_mutex_unlock(&cl_mutex);
    return STATUS_SUCCESS;
}

static cl_mem get_null_image(struct cl_device_context *ctx, UINT32 image_type, UINT32 channel, cl_int *err)
{
    static const UINT32 zero[4];
    struct cl_null_image *entry;
    cl_image_format format;
    cl_image_desc desc;
    cl_mem mem;

    for (entry = ctx->null_images; entry; entry = entry->next)
    {
        if (entry->image_type == image_type && entry->channel == channel)
            return entry->mem;
    }

    memset(&desc, 0, sizeof(desc));
    switch (image_type)
    {
        case WINED3D_CL_IMAGE_1D: desc.image_type = CL_MEM_OBJECT_IMAGE1D; break;
        case WINED3D_CL_IMAGE_1D_ARRAY: desc.image_type = CL_MEM_OBJECT_IMAGE1D_ARRAY; break;
        case WINED3D_CL_IMAGE_2D_ARRAY: desc.image_type = CL_MEM_OBJECT_IMAGE2D_ARRAY; break;
        case WINED3D_CL_IMAGE_3D: desc.image_type = CL_MEM_OBJECT_IMAGE3D; break;
        case WINED3D_CL_IMAGE_2D: desc.image_type = CL_MEM_OBJECT_IMAGE2D; break;
        default:
            *err = CL_INVALID_IMAGE_DESCRIPTOR;
            ERR("No null image of type %u.\n", image_type);
            return NULL;
    }
    desc.image_width = 1;
    desc.image_height = 1;
    desc.image_depth = 1;
    desc.image_array_size = 1;
    format.image_channel_order = CL_RGBA;
    switch (channel)
    {
        case WINED3D_CL_CHANNEL_INT: format.image_channel_data_type = CL_SIGNED_INT32; break;
        case WINED3D_CL_CHANNEL_UINT: format.image_channel_data_type = CL_UNSIGNED_INT32; break;
        case WINED3D_CL_CHANNEL_DEPTH:
            format.image_channel_order = CL_DEPTH;
            format.image_channel_data_type = CL_FLOAT;
            break;
        default: format.image_channel_data_type = CL_FLOAT; break;
    }
    if (!(mem = clCreateImage(ctx->context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
            &format, &desc, (void *)zero, err)))
    {
        ERR("Failed to create a null image of type %u, channel %u, error %d.\n", image_type, channel, *err);
        return NULL;
    }
    if (!(entry = malloc(sizeof(*entry))))
    {
        clReleaseMemObject(mem);
        return NULL;
    }
    entry->image_type = image_type;
    entry->channel = channel;
    entry->mem = mem;
    entry->next = ctx->null_images;
    ctx->null_images = entry;
    return mem;
}

static cl_sampler get_sampler(struct cl_device_context *ctx, const struct wined3d_cl_sampler_desc *desc, cl_int *err)
{
    struct cl_sampler_entry *entry;
    cl_addressing_mode address;
    cl_sampler sampler;

    for (entry = ctx->samplers; entry; entry = entry->next)
    {
        if (!memcmp(&entry->desc, desc, sizeof(*desc)))
            return entry->sampler;
    }

    /* enum wined3d_texture_address; OpenCL has no "mirror once". */
    switch (desc->address_mode)
    {
        case 1: address = CL_ADDRESS_REPEAT; break;
        case 2: address = CL_ADDRESS_MIRRORED_REPEAT; break;
        case 4: address = CL_ADDRESS_CLAMP; break;
        default: address = CL_ADDRESS_CLAMP_TO_EDGE; break;
    }
    if (!(sampler = clCreateSampler(ctx->context, CL_TRUE, address,
            desc->linear ? CL_FILTER_LINEAR : CL_FILTER_NEAREST, err)))
    {
        ERR("Failed to create a sampler (address %u, linear %u), error %d.\n",
                desc->address_mode, desc->linear, *err);
        return NULL;
    }
    if (!(entry = malloc(sizeof(*entry))))
    {
        clReleaseSampler(sampler);
        return NULL;
    }
    entry->desc = *desc;
    entry->sampler = sampler;
    entry->next = ctx->samplers;
    ctx->samplers = entry;
    return sampler;
}

/* Room for "size" bytes of WINED3D_CL_ARG_HOST_DATA copies; see host_ring. */
static BOOL host_reserve(struct cl_device_context *ctx, size_t size, size_t *pos)
{
    unsigned int segment, next;
    cl_int err;

    if (size > CL_HOST_SEGMENT_SIZE)
        return FALSE;
    if (!ctx->host_ring)
    {
        if (!(ctx->host_staging = malloc(CL_HOST_RING_SIZE)))
            return FALSE;
        if (!(ctx->host_ring = clCreateBuffer(ctx->context, CL_MEM_READ_ONLY, CL_HOST_RING_SIZE, NULL, &err)))
        {
            ERR("Failed to create a buffer for constant data, error %d.\n", err);
            free(ctx->host_staging);
            ctx->host_staging = NULL;
            return FALSE;
        }
        ctx->host_pos = 0;
        ctx->host_segment = 0;
    }

    segment = ctx->host_segment;
    if (ctx->host_pos + size > (segment + 1) * (size_t)CL_HOST_SEGMENT_SIZE)
    {
        next = (segment + 1) % CL_HOST_RING_SEGMENTS;
        if (ctx->host_markers[segment])
            clReleaseEvent(ctx->host_markers[segment]);
        if ((err = clEnqueueMarkerWithWaitList(ctx->queue, 0, NULL, &ctx->host_markers[segment])))
        {
            ERR("Failed to enqueue a marker, error %d.\n", err);
            ctx->host_markers[segment] = NULL;
            clFinish(ctx->queue);
        }
        if (ctx->host_markers[next])
        {
            clWaitForEvents(1, &ctx->host_markers[next]);
            clReleaseEvent(ctx->host_markers[next]);
            ctx->host_markers[next] = NULL;
        }
        ctx->host_segment = next;
        ctx->host_pos = next * (size_t)CL_HOST_SEGMENT_SIZE;
    }
    *pos = ctx->host_pos;
    ctx->host_pos += size;
    return TRUE;
}

/* Enqueues the open batch's kernels; see cl_device_context.batch. Called with
 * cl_mutex held, on the thread whose OpenGL context issued the commands the
 * kernels must follow. */
static void batch_flush(struct cl_device_context *ctx)
{
    const struct cl_batch_dispatch *dispatch;
    unsigned int i, j;
    cl_int err;

    if (!ctx->batch_count)
        return;

    cl_stat(CL_STAT_COUNT);
    if (ctx->batch_host_end > ctx->batch_host_start
            && (err = clEnqueueWriteBuffer(ctx->queue, ctx->host_ring, CL_FALSE, ctx->batch_host_start,
            ctx->batch_host_end - ctx->batch_host_start, ctx->host_staging + ctx->batch_host_start, 0, NULL, NULL)))
        ERR("Failed to write constant data, error %d.\n", err);
    cl_stat(CL_STAT_WRITE);

    /* The kernels must run after the OpenGL commands issued so far, and
     * OpenGL commands issued later must see their results; see implicit_sync. */
    if (ctx->implicit_sync)
        glFlush();
    else
        glFinish();
    cl_stat(CL_STAT_GL_FLUSH);
    if (ctx->batch_object_count && (err = clEnqueueAcquireGLObjects(ctx->queue,
            ctx->batch_object_count, ctx->batch_objects, 0, NULL, NULL)))
        ERR("Failed to acquire OpenGL objects, error %d.\n", err);
    cl_stat(CL_STAT_ACQUIRE);

    for (i = 0; i < ctx->batch_count; ++i)
    {
        dispatch = &ctx->batch[i];
        for (j = 0; j < dispatch->arg_count; ++j)
        {
            const struct cl_batch_arg *arg = &ctx->batch_args[dispatch->arg_start + j];

            if ((err = clSetKernelArg(dispatch->kernel->kernel, j, arg->size, arg->data)))
                ERR("Failed to set argument %u, error %d.\n", j, err);
        }
        if ((err = clEnqueueNDRangeKernel(ctx->queue, dispatch->kernel->kernel, 3, NULL,
                dispatch->global, dispatch->local, 0, NULL, NULL)))
            ERR("Failed to enqueue the kernel (global %zux%zux%zu, local %zux%zux%zu), error %d.\n",
                    dispatch->global[0], dispatch->global[1], dispatch->global[2],
                    dispatch->local[0], dispatch->local[1], dispatch->local[2], err);
    }
    cl_stat(CL_STAT_ENQUEUE);

    if (ctx->batch_object_count && (err = clEnqueueReleaseGLObjects(ctx->queue,
            ctx->batch_object_count, ctx->batch_objects, 0, NULL, NULL)))
        ERR("Failed to release OpenGL objects, error %d.\n", err);
    cl_stat(CL_STAT_RELEASE);

    {
        /* WINESKIN_CL_SYNC=1 waits for every batch, to tell ordering faults from others. */
        static int force_sync = -1;

        if (force_sync < 0)
            force_sync = getenv("WINESKIN_CL_SYNC") != NULL;
        if (force_sync || !ctx->implicit_sync)
        {
            clFinish(ctx->queue);
            ctx->pending = FALSE;
        }
        else
        {
            clFlush(ctx->queue);
            ctx->pending = TRUE;
        }
    }
    cl_stat(CL_STAT_CL_FLUSH);
    ++cl_stat_batches;

    ctx->batch_count = 0;
    ctx->batch_arg_count = 0;
    ctx->batch_object_count = 0;
    ctx->batch_host_start = ctx->batch_host_end = 0;
}

static BOOL host_fits(const struct cl_device_context *ctx, size_t size)
{
    return ctx->host_ring && ctx->host_pos + size <= (ctx->host_segment + 1) * (size_t)CL_HOST_SEGMENT_SIZE;
}

/* Adds a dispatch to the open batch, and enqueues the batch unless the
 * dispatch asks to wait for more. */
static NTSTATUS cl_dispatch(void *args)
{
    const struct wined3d_cl_dispatch_params *params = args;
    struct cl_kernel *kernel = (struct cl_kernel *)(ULONG_PTR)params->kernel;
    struct wined3d_cl_arg *cl_args = (struct wined3d_cl_arg *)(ULONG_PTR)params->args;
    size_t host_size = 0, host_start = 0, host_pos = 0, host_offset = 0;
    unsigned int i, j, object_args = 0;
    struct cl_batch_dispatch *dispatch;
    struct cl_device_context *ctx;
    NTSTATUS status = STATUS_SUCCESS;
    struct cl_batch_arg *value;
    cl_sampler sampler;
    cl_int err;
    cl_mem mem;

    if (!kernel)
        return STATUS_INVALID_PARAMETER;
    ctx = kernel->ctx;

    pthread_mutex_lock(&cl_mutex);

    if (ctx->released)
    {
        pthread_mutex_unlock(&cl_mutex);
        WARN("The kernel's OpenCL context is released.\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* A kernel still building, or whose build failed, runs nothing. */
    if (!kernel_ready(kernel))
    {
        pthread_mutex_unlock(&cl_mutex);
        return STATUS_PENDING;
    }

    if (cl_stats < 0)
        cl_stats = !!getenv("WINESKIN_CL_STATS");
    cl_stat(CL_STAT_COUNT);

    /* Every OpenGL object must be registered before any argument is set. */
    for (i = 0; i < params->arg_count; ++i)
    {
        struct wined3d_cl_arg *arg = &cl_args[i];

        arg->status = 0;
        if (arg->type == WINED3D_CL_ARG_GL_BUFFER || arg->type == WINED3D_CL_ARG_GL_TEXTURE)
        {
            ++object_args;
            if (!get_gl_mem(ctx, params->group, arg))
            {
                arg->status = WINED3D_CL_UNREGISTERED;
                status = STATUS_OBJECT_NAME_NOT_FOUND;
            }
        }
        else if (arg->type == WINED3D_CL_ARG_HOST_DATA)
        {
            host_size += (arg->size + CL_HOST_ALIGN - 1) & ~(size_t)(CL_HOST_ALIGN - 1);
        }
    }
    if (status)
    {
        pthread_mutex_unlock(&cl_mutex);
        return status;
    }

    /* Room in the batch, and for the copies in the segment of host_ring the
     * batch's copies are in. */
    if (ctx->batch_count && (ctx->batch_count == CL_BATCH_MAX_DISPATCHES
            || ctx->batch_arg_count + params->arg_count > CL_BATCH_MAX_ARGS
            || ctx->batch_object_count + object_args > CL_BATCH_MAX_OBJECTS
            || (host_size && ctx->batch_host_end > ctx->batch_host_start && !host_fits(ctx, host_size))))
        batch_flush(ctx);
    if (params->arg_count > CL_BATCH_MAX_ARGS || object_args > CL_BATCH_MAX_OBJECTS)
    {
        pthread_mutex_unlock(&cl_mutex);
        ERR("Too many arguments (%u).\n", params->arg_count);
        return STATUS_INVALID_PARAMETER;
    }
    if (!ctx->batch && (!(ctx->batch = malloc(CL_BATCH_MAX_DISPATCHES * sizeof(*ctx->batch)))
            || !(ctx->batch_args = malloc(CL_BATCH_MAX_ARGS * sizeof(*ctx->batch_args)))))
    {
        free(ctx->batch);
        ctx->batch = NULL;
        pthread_mutex_unlock(&cl_mutex);
        return STATUS_NO_MEMORY;
    }
    if (host_size)
    {
        if (!host_reserve(ctx, host_size, &host_start))
        {
            pthread_mutex_unlock(&cl_mutex);
            return STATUS_NO_MEMORY;
        }
        if (ctx->batch_host_end == ctx->batch_host_start)
            ctx->batch_host_start = host_start;
        ctx->batch_host_end = host_start + host_size;
    }
    host_pos = host_start;

    for (i = 0; i < params->arg_count; ++i)
    {
        struct wined3d_cl_arg *arg = &cl_args[i];

        value = &ctx->batch_args[ctx->batch_arg_count + i];
        value->size = sizeof(cl_mem);
        err = CL_SUCCESS;
        switch (arg->type)
        {
            case WINED3D_CL_ARG_VALUE:
                if (arg->size > sizeof(value->data))
                {
                    err = CL_INVALID_ARG_SIZE;
                    break;
                }
                value->size = arg->size;
                memcpy(value->data, arg->data, arg->size);
                if (i && cl_args[i - 1].type == WINED3D_CL_ARG_HOST_DATA && arg->size >= sizeof(UINT32))
                {
                    UINT32 offset;

                    memcpy(&offset, arg->data, sizeof(offset));
                    offset += host_offset;
                    memcpy(value->data, &offset, sizeof(offset));
                }
                break;

            case WINED3D_CL_ARG_HOST_DATA:
            {
                UINT64 ptr;

                memcpy(&ptr, arg->data, sizeof(ptr));
                memcpy(ctx->host_staging + host_pos, (const void *)(ULONG_PTR)ptr, arg->size);
                host_offset = host_pos;
                host_pos += (arg->size + CL_HOST_ALIGN - 1) & ~(size_t)(CL_HOST_ALIGN - 1);
                memcpy(value->data, &ctx->host_ring, sizeof(cl_mem));
                ++cl_stat_host;
                break;
            }

            case WINED3D_CL_ARG_GL_BUFFER:
            case WINED3D_CL_ARG_GL_TEXTURE:
                mem = get_gl_mem(ctx, params->group, arg);
                for (j = 0; j < ctx->batch_object_count; ++j)
                {
                    if (ctx->batch_objects[j] == mem)
                        break;
                }
                if (j == ctx->batch_object_count)
                    ctx->batch_objects[ctx->batch_object_count++] = mem;
                memcpy(value->data, &mem, sizeof(mem));
                if (arg->type == WINED3D_CL_ARG_GL_BUFFER)
                    ++cl_stat_gl_buffers;
                break;

            case WINED3D_CL_ARG_NULL_BUFFER:
                memcpy(value->data, &ctx->null_buffer, sizeof(cl_mem));
                break;

            case WINED3D_CL_ARG_NULL_IMAGE:
                if (!(mem = get_null_image(ctx, arg->image_type, arg->channel, &err)))
                    break;
                memcpy(value->data, &mem, sizeof(mem));
                break;

            case WINED3D_CL_ARG_SAMPLER:
                if (!(sampler = get_sampler(ctx, (const struct wined3d_cl_sampler_desc *)arg->data, &err)))
                    break;
                value->size = sizeof(sampler);
                memcpy(value->data, &sampler, sizeof(sampler));
                break;

            default:
                err = CL_INVALID_VALUE;
                break;
        }
        arg->status = err;
        if (err)
        {
            WARN("Failed to set argument %u (type %u, name %u, target %#x, level %u), error %d.\n",
                    i, arg->type, arg->gl_name, arg->gl_target, arg->level, err);
            status = STATUS_INVALID_PARAMETER;
        }
    }

    if (!status)
    {
        dispatch = &ctx->batch[ctx->batch_count++];
        dispatch->kernel = kernel;
        dispatch->arg_start = ctx->batch_arg_count;
        dispatch->arg_count = params->arg_count;
        for (i = 0; i < 3; ++i)
        {
            dispatch->local[i] = params->group_size[i];
            dispatch->global[i] = (size_t)params->group_count[i] * params->group_size[i];
        }
        ctx->batch_arg_count += params->arg_count;
    }
    cl_stat(CL_STAT_ARGS);

    if (!params->batch || params->sync)
        batch_flush(ctx);
    if (params->sync && ctx->pending)
    {
        clFinish(ctx->queue);
        ctx->pending = FALSE;
    }
    if (cl_stats)
        cl_stats_dispatch_done();

    pthread_mutex_unlock(&cl_mutex);
    return status;
}

/* Enqueues the open batch of the current OpenGL context's share group. */
static NTSTATUS cl_flush(void *args)
{
    CGLContextObj gl_context = CGLGetCurrentContext();
    struct cl_device_context *ctx;

    if (!gl_context)
        return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&cl_mutex);
    for (ctx = cl_contexts; ctx; ctx = ctx->next)
    {
        if (ctx->share_group == CGLGetShareGroup(gl_context))
            batch_flush(ctx);
    }
    pthread_mutex_unlock(&cl_mutex);
    return STATUS_SUCCESS;
}

/* Waits for the kernels in flight; OpenGL may use their results afterwards. */
static NTSTATUS cl_finish(void *args)
{
    struct cl_device_context *ctx;

    pthread_mutex_lock(&cl_mutex);
    for (ctx = cl_contexts; ctx; ctx = ctx->next)
    {
        batch_flush(ctx);
        if (ctx->pending)
        {
            clFinish(ctx->queue);
            ctx->pending = FALSE;
        }
    }
    pthread_mutex_unlock(&cl_mutex);
    return STATUS_SUCCESS;
}

/* Releases the memory objects of an OpenGL object that is being deleted. */
static NTSTATUS cl_forget(void *args)
{
    const struct wined3d_cl_forget_params *params = args;
    struct cl_mem_entry *entry, **prev;
    struct cl_device_context *ctx;

    pthread_mutex_lock(&cl_mutex);
    for (ctx = cl_contexts; ctx; ctx = ctx->next)
    {
        prev = &ctx->mems[mem_hash(params->name) % CL_MEM_HASH_SIZE];
        while ((entry = *prev))
        {
            if (entry->type == params->type && entry->name == params->name
                    && (!params->group || entry->group == params->group))
            {
                TRACE("Releasing OpenCL memory %p for %s %u.\n", entry->mem,
                        params->type == WINED3D_CL_OBJECT_BUFFER ? "buffer" : "texture", params->name);
                batch_flush(ctx);
                if (ctx->pending)
                {
                    clFinish(ctx->queue);
                    ctx->pending = FALSE;
                }
                clReleaseMemObject(entry->mem);
                *prev = entry->next;
                free(entry);
                continue;
            }
            prev = &entry->next;
        }
    }
    pthread_mutex_unlock(&cl_mutex);
    return STATUS_SUCCESS;
}

/* Releases the memory objects of a group whose OpenGL contexts are about to
 * be destroyed, and the OpenCL context when no group is left: its share
 * group may go with the last OpenGL context. */
static NTSTATUS cl_release_context(void *args)
{
    const struct wined3d_cl_release_context_params *params = args;
    struct cl_device_context *ctx, **prev_ctx;
    struct cl_mem_entry *entry, **prev;
    unsigned int i;

    pthread_mutex_lock(&cl_mutex);
    for (prev_ctx = &cl_contexts; (ctx = *prev_ctx);)
    {
        for (i = 0; i < ctx->group_count; ++i)
        {
            if (ctx->groups[i] == params->group)
                break;
        }
        if (i == ctx->group_count)
        {
            prev_ctx = &ctx->next;
            continue;
        }
        ctx->groups[i] = ctx->groups[--ctx->group_count];

        /* The group's OpenGL objects go with it: nothing may be in flight. */
        batch_flush(ctx);
        clFinish(ctx->queue);
        ctx->pending = FALSE;
        for (i = 0; i < CL_MEM_HASH_SIZE; ++i)
        {
            for (prev = &ctx->mems[i]; (entry = *prev);)
            {
                if (entry->group != params->group)
                {
                    prev = &entry->next;
                    continue;
                }
                *prev = entry->next;
                clReleaseMemObject(entry->mem);
                free(entry);
            }
        }

        if (ctx->group_count)
        {
            prev_ctx = &ctx->next;
            continue;
        }
        *prev_ctx = ctx->next;
        ctx->released = TRUE;
        TRACE("Released the OpenCL context %p.\n", ctx->context);
        cl_context_release(ctx);
    }
    pthread_mutex_unlock(&cl_mutex);
    return STATUS_SUCCESS;
}

#if !defined(SONAME_LIBVA) || !defined(SONAME_LIBVA_DRM)

static NTSTATUS va_unsupported(void *args)
{
    return STATUS_NOT_SUPPORTED;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    [unix_va_get_profiles_vk] = va_unsupported,
    [unix_va_decoder_create_vk] = va_unsupported,
    [unix_va_decoder_destroy_vk] = va_unsupported,
    [unix_va_decoder_decode] = va_unsupported,
    [unix_cl_init] = cl_init,
    [unix_cl_build] = cl_build,
    [unix_cl_release_kernel] = cl_release_kernel,
    [unix_cl_dispatch] = cl_dispatch,
    [unix_cl_forget] = cl_forget,
    [unix_cl_finish] = cl_finish,
    [unix_cl_release_context] = cl_release_context,
    [unix_cl_register] = cl_register,
    [unix_cl_flush] = cl_flush,
};

/* The parameter structures hold no pointer-sized fields. */
const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    [unix_va_get_profiles_vk] = va_unsupported,
    [unix_va_decoder_create_vk] = va_unsupported,
    [unix_va_decoder_destroy_vk] = va_unsupported,
    [unix_va_decoder_decode] = va_unsupported,
    [unix_cl_init] = cl_init,
    [unix_cl_build] = cl_build,
    [unix_cl_release_kernel] = cl_release_kernel,
    [unix_cl_dispatch] = cl_dispatch,
    [unix_cl_forget] = cl_forget,
    [unix_cl_finish] = cl_finish,
    [unix_cl_release_context] = cl_release_context,
    [unix_cl_register] = cl_register,
    [unix_cl_flush] = cl_flush,
};

#endif

#endif /* __APPLE__ */
