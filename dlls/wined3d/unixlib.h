/*
 * Copyright 2024 Elizabeth Figura for CodeWeavers
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

#ifndef __WINE_WINED3D_UNIXLIB_H
#define __WINE_WINED3D_UNIXLIB_H

/* We cannot put these definitions in wined3d_private.h, because
 * wined3d_private.h uses vkd3d-shader definitions, and we cannot access
 * vkd3d_shader.h from the Unix side. */

#define COBJMACROS
#include <stdint.h>
#include "objbase.h"
#include "wine/wined3d.h"
#include "wine/unixlib.h"

#define VA_DECODER_SURFACE_COUNT 17

struct va_get_profiles_vk_params
{
    UINT64 physical_device; /* VkPhysicalDevice */
    UINT64 count; /* unsigned int * */
    UINT64 profiles; /* GUID * */
};

struct va_decoder_create_vk_params
{
    struct wined3d_decoder_desc desc;
    UINT64 physical_device; /* VkPhysicalDevice */
    UINT64 device; /* VkDevice */
    UINT64 decoder;
    unsigned int width;
    unsigned int height;
    struct
    {
        UINT64 image; /* VkImage */
    } surfaces[VA_DECODER_SURFACE_COUNT];
};

struct va_decoder_destroy_vk_params
{
    UINT64 device; /* VkDevice */
    UINT64 decoder;
};

struct va_decoder_decode_params
{
    UINT64 decoder;
    UINT64 bitstream, parameters, matrix, slice_control;
    UINT32 bitstream_size, slice_control_size;
    UINT32 output_idx;
};

/* WINESKIN: compute shaders on OpenCL with OpenGL sharing (cl.c, cl_compute.c).
 * Every call runs on the thread whose OpenGL context is current; the OpenCL
 * context shares the host OpenGL share group of that context. */
#define WINED3D_CL_MAX_ARG_DATA 32

struct wined3d_cl_init_params
{
    UINT32 max_work_group_size;
    UINT32 local_mem_size;
    UINT32 max_write_images;
    UINT32 caps; /* WINED3D_CL_CAP_* */
};

#define WINED3D_CL_CAP_DEPTH_IMAGES 0x1u
#define WINED3D_CL_CAP_MSAA_IMAGES  0x2u
#define WINED3D_CL_CAP_3D_WRITES    0x4u

struct wined3d_cl_build_params
{
    UINT64 group;  /* see struct wined3d_cl_register_params */
    UINT64 source; /* const char * */
    UINT32 group_size[3];
    UINT32 work_group_size; /* out: the kernel's maximum */
    UINT64 kernel; /* out */
};

struct wined3d_cl_release_kernel_params
{
    UINT64 kernel;
};

enum wined3d_cl_arg_type
{
    WINED3D_CL_ARG_VALUE,       /* data[size] */
    WINED3D_CL_ARG_GL_BUFFER,   /* the whole buffer gl_name */
    WINED3D_CL_ARG_GL_TEXTURE,  /* level of texture gl_name, image target gl_target */
    WINED3D_CL_ARG_NULL_BUFFER, /* a small zeroed buffer */
    WINED3D_CL_ARG_NULL_IMAGE,  /* a zeroed 1x1 image of image_type and channel */
    WINED3D_CL_ARG_SAMPLER,     /* data: struct wined3d_cl_sampler_desc */
    /* size bytes of system memory at data's UINT64 pointer, copied for the
     * dispatch into a buffer OpenCL owns; the argument after it is a value
     * whose first UINT32 is a byte offset in that buffer, to which the copy's
     * position is added. */
    WINED3D_CL_ARG_HOST_DATA,
};

enum wined3d_cl_image_dim
{
    WINED3D_CL_IMAGE_NONE,
    WINED3D_CL_IMAGE_1D,
    WINED3D_CL_IMAGE_1D_ARRAY,
    WINED3D_CL_IMAGE_2D,
    WINED3D_CL_IMAGE_2D_ARRAY,
    WINED3D_CL_IMAGE_3D,
    WINED3D_CL_IMAGE_2D_MSAA,
    WINED3D_CL_IMAGE_2D_ARRAY_MSAA,
};

enum wined3d_cl_channel
{
    WINED3D_CL_CHANNEL_FLOAT,
    WINED3D_CL_CHANNEL_INT,
    WINED3D_CL_CHANNEL_UINT,
    WINED3D_CL_CHANNEL_DEPTH,
};

struct wined3d_cl_sampler_desc
{
    UINT32 address_mode; /* enum wined3d_texture_address */
    UINT32 linear;
};

struct wined3d_cl_arg
{
    UINT32 type;
    UINT32 gl_name;
    UINT32 gl_target;
    UINT32 level;
    UINT32 size;
    UINT32 image_type; /* enum wined3d_cl_image_dim */
    UINT32 channel;    /* enum wined3d_cl_channel */
    UINT32 status;     /* out: the OpenCL error for this argument, or WINED3D_CL_UNREGISTERED */
    BYTE data[WINED3D_CL_MAX_ARG_DATA];
};

/* The status of an OpenGL object argument unix_cl_register has not been told
 * of; the dispatch then fails with STATUS_OBJECT_NAME_NOT_FOUND. */
#define WINED3D_CL_UNREGISTERED 0x7fffffffu

struct wined3d_cl_dispatch_params
{
    UINT64 group;  /* whose OpenGL names the arguments are; see struct wined3d_cl_register_params */
    UINT64 kernel;
    UINT64 args; /* struct wined3d_cl_arg * */
    UINT32 arg_count;
    UINT32 group_count[3];
    UINT32 group_size[3];
    UINT32 sync;  /* finish before returning */
    /* Add the dispatch to the open batch: its kernel is enqueued at the next
     * unix_cl_flush, with the batch's other kernels. Until then OpenGL must
     * not touch the objects the batch uses. */
    UINT32 batch;
};

enum wined3d_cl_object_type
{
    WINED3D_CL_OBJECT_BUFFER,
    WINED3D_CL_OBJECT_TEXTURE,
};

struct wined3d_cl_forget_params
{
    UINT64 group; /* 0: the name in every group */
    UINT32 type; /* enum wined3d_cl_object_type */
    UINT32 name;
};

/* The names wined3d has for OpenGL objects are opengl32's, which gives each
 * group of contexts that share objects (a wined3d device, "group") names of
 * its own and keeps every context in one share group of the host's OpenGL.
 * OpenCL takes the host's names. unix_cl_register is called with the object
 * bound (a buffer to GL_COPY_READ_BUFFER, a texture to the active unit) and
 * asks the host's OpenGL for the name bound there. */
struct wined3d_cl_register_params
{
    UINT64 group;
    UINT32 type;   /* enum wined3d_cl_object_type */
    UINT32 name;
    UINT32 target; /* textures: the image target */
    UINT32 level;
};

struct wined3d_cl_release_context_params
{
    UINT64 group;
};

enum unix_funcs
{
    unix_va_get_profiles_vk,
    unix_va_decoder_create_vk,
    unix_va_decoder_destroy_vk,
    unix_va_decoder_decode,
    unix_cl_init, /* WINESKIN */
    unix_cl_build, /* WINESKIN */
    unix_cl_release_kernel, /* WINESKIN */
    unix_cl_dispatch, /* WINESKIN */
    unix_cl_forget, /* WINESKIN */
    unix_cl_finish, /* WINESKIN */
    unix_cl_release_context, /* WINESKIN */
    unix_cl_register, /* WINESKIN */
    unix_cl_flush, /* WINESKIN */
};

#endif
