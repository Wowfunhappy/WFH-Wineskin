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

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <OpenCL/opencl.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLDevice.h>
#include <OpenGL/gl.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "unixlib.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d_cl);

struct cl_mem_entry
{
    struct cl_mem_entry *next;
    UINT32 type, name, target, level;
    cl_mem mem;
};

#define CL_MEM_HASH_SIZE 1024

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
    /* cl_khr_gl_event: acquiring OpenGL objects waits for the commands of the
     * OpenGL context bound on the calling thread, and that context's commands
     * after the release see the kernel's results. Without it each dispatch
     * finishes OpenGL first and OpenCL afterwards. */
    BOOL implicit_sync;
    LONG refcount; /* the list's, and each kernel's */
    BOOL released; /* out of the list: its share group is going away */
};

struct cl_kernel
{
    struct cl_device_context *ctx;
    cl_program program;
    cl_kernel kernel;
};

static pthread_mutex_t cl_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct cl_device_context *cl_contexts;

/* The OpenCL context for the current OpenGL context's share group. */
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
static void cl_context_release(struct cl_device_context *ctx)
{
    struct cl_sampler_entry *sampler;
    struct cl_null_image *image;

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
    clReleaseCommandQueue(ctx->queue);
    clReleaseContext(ctx->context);
    free(ctx);
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

static NTSTATUS cl_build(void *args)
{
    struct wined3d_cl_build_params *params = args;
    const char *source = (const char *)(ULONG_PTR)params->source;
    static const char options[] = "-cl-single-precision-constant -cl-denorms-are-zero";
    struct cl_device_context *ctx;
    struct cl_kernel *kernel;
    cl_program program;
    size_t log_size, wg;
    cl_int err;
    char *log;

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

    err = clBuildProgram(program, 1, &ctx->device, options, NULL, NULL);
    if (!clGetProgramBuildInfo(program, ctx->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size)
            && log_size > 1 && (log = malloc(log_size + 1)))
    {
        clGetProgramBuildInfo(program, ctx->device, CL_PROGRAM_BUILD_LOG, log_size, log, NULL);
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
    if (err)
    {
        clReleaseProgram(program);
        pthread_mutex_unlock(&cl_mutex);
        return STATUS_UNSUCCESSFUL;
    }

    if (!(kernel = calloc(1, sizeof(*kernel))))
    {
        clReleaseProgram(program);
        pthread_mutex_unlock(&cl_mutex);
        return STATUS_NO_MEMORY;
    }
    kernel->ctx = ctx;
    ++ctx->refcount;
    kernel->program = program;
    if (!(kernel->kernel = clCreateKernel(program, "wined3d_cs", &err)))
    {
        ERR("Failed to create the OpenCL kernel, error %d.\n", err);
        clReleaseProgram(program);
        free(kernel);
        pthread_mutex_unlock(&cl_mutex);
        return STATUS_UNSUCCESSFUL;
    }
    if (!clGetKernelWorkGroupInfo(kernel->kernel, ctx->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof(wg), &wg, NULL))
        params->work_group_size = wg;
    else
        params->work_group_size = 0;
    pthread_mutex_unlock(&cl_mutex);

    params->kernel = (ULONG_PTR)kernel;
    return STATUS_SUCCESS;
}

static NTSTATUS cl_release_kernel(void *args)
{
    const struct wined3d_cl_release_kernel_params *params = args;
    struct cl_kernel *kernel = (struct cl_kernel *)(ULONG_PTR)params->kernel;

    if (!kernel)
        return STATUS_SUCCESS;
    pthread_mutex_lock(&cl_mutex);
    clReleaseKernel(kernel->kernel);
    clReleaseProgram(kernel->program);
    cl_context_release(kernel->ctx);
    pthread_mutex_unlock(&cl_mutex);
    free(kernel);
    return STATUS_SUCCESS;
}

static unsigned int mem_hash(UINT32 name)
{
    return (name * 2654435761u) >> 22;
}

static cl_mem get_gl_mem(struct cl_device_context *ctx, const struct wined3d_cl_arg *arg, cl_int *err)
{
    UINT32 type = arg->type == WINED3D_CL_ARG_GL_BUFFER ? WINED3D_CL_OBJECT_BUFFER : WINED3D_CL_OBJECT_TEXTURE;
    struct cl_mem_entry *entry, **bucket = &ctx->mems[mem_hash(arg->gl_name) % CL_MEM_HASH_SIZE];
    cl_mem mem;

    for (entry = *bucket; entry; entry = entry->next)
    {
        if (entry->type == type && entry->name == arg->gl_name
                && (type == WINED3D_CL_OBJECT_BUFFER
                || (entry->target == arg->gl_target && entry->level == arg->level)))
            return entry->mem;
    }

    if (type == WINED3D_CL_OBJECT_BUFFER)
    {
        mem = clCreateFromGLBuffer(ctx->context, CL_MEM_READ_WRITE, arg->gl_name, err);
    }
    else
    {
        mem = clCreateFromGLTexture(ctx->context, CL_MEM_READ_WRITE,
                arg->gl_target, arg->level, arg->gl_name, err);
    }
    if (!mem)
        return NULL;
    TRACE("Created OpenCL memory %p for %s %u, target %#x, level %u.\n", mem,
            type == WINED3D_CL_OBJECT_BUFFER ? "buffer" : "texture", arg->gl_name, arg->gl_target, arg->level);

    if (!(entry = malloc(sizeof(*entry))))
    {
        clReleaseMemObject(mem);
        *err = CL_OUT_OF_HOST_MEMORY;
        return NULL;
    }
    entry->type = type;
    entry->name = arg->gl_name;
    entry->target = arg->gl_target;
    entry->level = arg->level;
    entry->mem = mem;
    entry->next = *bucket;
    *bucket = entry;
    return mem;
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

static NTSTATUS cl_dispatch(void *args)
{
    const struct wined3d_cl_dispatch_params *params = args;
    struct cl_kernel *kernel = (struct cl_kernel *)(ULONG_PTR)params->kernel;
    struct wined3d_cl_arg *cl_args = (struct wined3d_cl_arg *)(ULONG_PTR)params->args;
    cl_mem acquire[128], mem;
    size_t global[3], local[3];
    unsigned int i, j, acquire_count = 0;
    struct cl_device_context *ctx;
    NTSTATUS status = STATUS_SUCCESS;
    cl_sampler sampler;
    cl_int err;

    if (!kernel)
        return STATUS_INVALID_PARAMETER;
    ctx = kernel->ctx;

    pthread_mutex_lock(&cl_mutex);

    if (ctx->released)
    {
        pthread_mutex_unlock(&cl_mutex);
        WARN("The kernel's share group is gone.\n");
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0; i < params->arg_count; ++i)
    {
        struct wined3d_cl_arg *arg = &cl_args[i];

        err = CL_SUCCESS;
        switch (arg->type)
        {
            case WINED3D_CL_ARG_VALUE:
                err = clSetKernelArg(kernel->kernel, i, arg->size, arg->data);
                break;

            case WINED3D_CL_ARG_GL_BUFFER:
            case WINED3D_CL_ARG_GL_TEXTURE:
                if (!(mem = get_gl_mem(ctx, arg, &err)))
                    break;
                for (j = 0; j < acquire_count; ++j)
                {
                    if (acquire[j] == mem)
                        break;
                }
                if (j == acquire_count && acquire_count < ARRAY_SIZE(acquire))
                    acquire[acquire_count++] = mem;
                err = clSetKernelArg(kernel->kernel, i, sizeof(mem), &mem);
                break;

            case WINED3D_CL_ARG_NULL_BUFFER:
                err = clSetKernelArg(kernel->kernel, i, sizeof(ctx->null_buffer), &ctx->null_buffer);
                break;

            case WINED3D_CL_ARG_NULL_IMAGE:
                if (!(mem = get_null_image(ctx, arg->image_type, arg->channel, &err)))
                    break;
                err = clSetKernelArg(kernel->kernel, i, sizeof(mem), &mem);
                break;

            case WINED3D_CL_ARG_SAMPLER:
                if (!(sampler = get_sampler(ctx, (const struct wined3d_cl_sampler_desc *)arg->data, &err)))
                    break;
                err = clSetKernelArg(kernel->kernel, i, sizeof(sampler), &sampler);
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

    if (status)
    {
        pthread_mutex_unlock(&cl_mutex);
        return status;
    }

    for (i = 0; i < 3; ++i)
    {
        local[i] = params->group_size[i];
        global[i] = (size_t)params->group_count[i] * params->group_size[i];
    }

    /* OpenGL's commands must reach the device before OpenCL's that use the
     * same objects (cl_APPLE_gl_sharing); see implicit_sync. */
    if (ctx->implicit_sync)
        glFlush();
    else
        glFinish();
    if (acquire_count && (err = clEnqueueAcquireGLObjects(ctx->queue, acquire_count, acquire, 0, NULL, NULL)))
        ERR("Failed to acquire OpenGL objects, error %d.\n", err);
    if ((err = clEnqueueNDRangeKernel(ctx->queue, kernel->kernel, 3, NULL, global, local, 0, NULL, NULL)))
    {
        ERR("Failed to enqueue the kernel (groups %ux%ux%u of %ux%ux%u), error %d.\n",
                params->group_count[0], params->group_count[1], params->group_count[2],
                params->group_size[0], params->group_size[1], params->group_size[2], err);
        status = STATUS_UNSUCCESSFUL;
    }
    if (acquire_count && (err = clEnqueueReleaseGLObjects(ctx->queue, acquire_count, acquire, 0, NULL, NULL)))
        ERR("Failed to release OpenGL objects, error %d.\n", err);

    if (params->sync || !ctx->implicit_sync)
    {
        clFinish(ctx->queue);
        ctx->pending = FALSE;
    }
    else
    {
        clFlush(ctx->queue);
        ctx->pending = TRUE;
    }

    pthread_mutex_unlock(&cl_mutex);
    return status;
}

/* Waits for the kernels in flight; OpenGL may use their results afterwards. */
static NTSTATUS cl_finish(void *args)
{
    struct cl_device_context *ctx;

    pthread_mutex_lock(&cl_mutex);
    for (ctx = cl_contexts; ctx; ctx = ctx->next)
    {
        if (ctx->pending)
        {
            clFinish(ctx->queue);
            ctx->pending = FALSE;
        }
    }
    pthread_mutex_unlock(&cl_mutex);
    return STATUS_SUCCESS;
}

/* OpenGL names are per share group: the current context's. */
static NTSTATUS cl_forget(void *args)
{
    const struct wined3d_cl_forget_params *params = args;
    struct cl_mem_entry *entry, **prev;
    struct cl_device_context *ctx;
    CGLContextObj gl_context;

    if (!(gl_context = CGLGetCurrentContext()))
    {
        ERR("No current OpenGL context.\n");
        return STATUS_INVALID_PARAMETER;
    }

    pthread_mutex_lock(&cl_mutex);
    for (ctx = cl_contexts; ctx; ctx = ctx->next)
    {
        if (ctx->share_group != CGLGetShareGroup(gl_context))
            continue;
        prev = &ctx->mems[mem_hash(params->name) % CL_MEM_HASH_SIZE];
        while ((entry = *prev))
        {
            if (entry->type == params->type && entry->name == params->name)
            {
                TRACE("Releasing OpenCL memory %p for %s %u.\n", entry->mem,
                        params->type == WINED3D_CL_OBJECT_BUFFER ? "buffer" : "texture", params->name);
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

/* Releases the OpenCL context of the current OpenGL context's share group,
 * which is about to be destroyed, with its memory objects. */
static NTSTATUS cl_release_context(void *args)
{
    struct cl_device_context *ctx, **prev;
    struct cl_mem_entry *entry;
    CGLShareGroupObj share_group;
    CGLContextObj gl_context;
    unsigned int i;

    if (!(gl_context = CGLGetCurrentContext()))
    {
        ERR("No current OpenGL context.\n");
        return STATUS_INVALID_PARAMETER;
    }
    share_group = CGLGetShareGroup(gl_context);

    pthread_mutex_lock(&cl_mutex);
    for (prev = &cl_contexts; (ctx = *prev); prev = &ctx->next)
    {
        if (ctx->share_group == share_group)
            break;
    }
    if (!ctx)
    {
        pthread_mutex_unlock(&cl_mutex);
        return STATUS_SUCCESS;
    }
    *prev = ctx->next;
    ctx->released = TRUE;

    /* The OpenGL objects go with the share group: nothing may be in flight. */
    clFinish(ctx->queue);
    ctx->pending = FALSE;
    for (i = 0; i < CL_MEM_HASH_SIZE; ++i)
    {
        while ((entry = ctx->mems[i]))
        {
            ctx->mems[i] = entry->next;
            clReleaseMemObject(entry->mem);
            free(entry);
        }
    }
    TRACE("Released the OpenCL context %p.\n", ctx->context);
    cl_context_release(ctx);
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
};

#endif

#endif /* __APPLE__ */
