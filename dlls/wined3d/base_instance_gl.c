/*
 * WINESKIN: start instances without ARB_base_instance.
 *
 * macOS's OpenGL 4.1 lacks ARB_base_instance (OpenGL 4.2), so a draw can't
 * start at an instance other than 0. In Direct3D the start instance only
 * offsets the per-instance vertex data: SV_InstanceID starts at 0 whatever
 * it is, and an instanced element's index is the start instance plus the
 * instance / step rate, as with OpenGL's base instance. A draw with a start
 * instance therefore points each per-instance attribute array start instance
 * elements further for the draw, and back afterwards
 * (wined3d_context_gl_offset_instanced_arrays()).
 *
 * Indirect draws (ARB_draw_indirect, OpenGL 4.0) read their arguments on the
 * GPU, where the start instance is a reserved field that must be 0 (NVIDIA's
 * driver on macOS 10.9 honours it all the same). An
 * indirect draw whose arguments are in system memory and start at another
 * instance is drawn as a direct draw with those arguments
 * (wined3d_context_gl_indirect_start_instance()); arguments only the GPU has
 * go to the indirect draw as they are.
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

/* Points the per-instance attribute arrays "start_instance" instances
 * further (0 puts them back where wined3d_context_gl_load_numbered_arrays()
 * left them). */
void wined3d_context_gl_offset_instanced_arrays(struct wined3d_context_gl *context_gl,
        const struct wined3d_state *state, unsigned int start_instance)
{
    const struct wined3d_shader *vs = state->shader[WINED3D_SHADER_TYPE_VERTEX];
    const struct wined3d_stream_info *si = &context_gl->c.stream_info;
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    uint32_t use_map = si->use_map, location_map = 0;
    GLuint bo, current_bo = ~0u;

    while (use_map)
    {
        unsigned int idx = wined3d_bit_scan(&use_map);
        unsigned int location = wined3d_vs_attrib_location(vs, gl_info->limits.vertex_attribs, idx);
        const struct wined3d_stream_info_element *element = &si->elements[idx];
        const struct wined3d_format_gl *format_gl = wined3d_format_gl(element->format);
        const uint8_t *offset;

        if (location >= gl_info->limits.vertex_attribs || (location_map & (1u << location)))
            continue;
        location_map |= 1u << location;
        if (!element->instanced || !element->stride || !format_gl->vtx_type)
            continue;

        offset = element->data.addr + state->load_base_vertex_index * element->stride;
        if (element->data.buffer_object)
            offset += element->data.buffer_object->buffer_offset;
        offset += (size_t)start_instance * element->stride;

        bo = wined3d_bo_gl_id(element->data.buffer_object);
        if (bo != current_bo)
        {
            GL_EXTCALL(glBindBuffer(GL_ARRAY_BUFFER, bo));
            current_bo = bo;
        }
        if (vs && vs->reg_maps.shader_version.major >= 4 && (format_gl->f.attrs & WINED3D_FORMAT_ATTR_INTEGER))
            GL_EXTCALL(glVertexAttribIPointer(location, format_gl->vtx_format,
                    format_gl->vtx_type, element->stride, offset));
        else
            GL_EXTCALL(glVertexAttribPointer(location, format_gl->vtx_format, format_gl->vtx_type,
                    !!(format_gl->f.attrs & WINED3D_FORMAT_ATTR_NORMALISED), element->stride, offset));
    }
    checkGLcall("offset instanced arrays");
}

/* The start instance of an indirect draw's arguments, when a copy of them
 * in system memory is current (the buffer's own, or the mirror of its buffer
 * object that writes from the CPU keep); 0 otherwise. "args" receives the
 * whole argument structure (5 words for an indexed draw, 4 otherwise). */
unsigned int wined3d_context_gl_indirect_start_instance(struct wined3d_context_gl *context_gl,
        const struct wined3d_indirect_draw_parameters *parameters, bool indexed, uint32_t *args)
{
    struct wined3d_buffer *buffer = parameters->buffer;
    unsigned int count = indexed ? 5 : 4;
    struct wined3d_bo *bo = buffer->buffer_object;
    const uint8_t *data;

    if (parameters->offset + count * sizeof(*args) > buffer->resource.size)
        return 0;
    if ((buffer->locations & WINED3D_LOCATION_SYSMEM) && buffer->resource.heap_memory)
        data = buffer->resource.heap_memory;
    else if (bo && bo->cl_mirror && bo->cl_mirror_serial == bo->cl_serial)
        data = bo->cl_mirror;
    else
        return 0;
    memcpy(args, data + parameters->offset, count * sizeof(*args));
    return args[count - 1];
}
