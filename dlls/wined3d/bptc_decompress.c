/*
 * WINESKIN: BC6H and BC7 textures decoded on upload.
 *
 * macOS's OpenGL has no BPTC texture compression. Direct3D 11 games ship
 * BC7 and BC6H textures, so where ARB_texture_compression_bptc is missing
 * wined3d offers these formats through its decompress-on-upload path
 * (WINED3D_FORMAT_CAP_DECOMPRESS): the texture keeps its blocks in system
 * memory and is B8G8R8A8 (BC7; sRGB for BC7_UNORM_SRGB) or R16G16B16A16_FLOAT
 * (BC6H) in OpenGL. The decoder is bcdec (bcdec.h).
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

#define BCDECDEF static inline
#define BCDEC_IMPLEMENTATION
#define WINE_UNUSED
#include "bcdec.h"

void wined3d_decompress_bc7(const BYTE *src, BYTE *dst, unsigned int src_row_pitch,
        unsigned int src_slice_pitch, unsigned int dst_row_pitch, unsigned int dst_slice_pitch,
        unsigned int width, unsigned int height, unsigned int depth)
{
    unsigned int x, y, z, i, j, block_w, block_h;
    const BYTE *src_row, *texel;
    BYTE block[4][4][4];
    BYTE *dst_texel;

    for (z = 0; z < depth; ++z)
    {
        for (y = 0; y < height; y += 4)
        {
            src_row = &src[z * src_slice_pitch + (y / 4) * src_row_pitch];
            block_h = min(height - y, 4);
            for (x = 0; x < width; x += 4)
            {
                block_w = min(width - x, 4);
                bcdec_bc7(&src_row[(x / 4) * BCDEC_BC7_BLOCK_SIZE], block, sizeof(block[0]));
                /* R8G8B8A8 blocks into B8G8R8A8 texels. */
                for (j = 0; j < block_h; ++j)
                {
                    dst_texel = &dst[z * dst_slice_pitch + (y + j) * dst_row_pitch + x * 4];
                    for (i = 0; i < block_w; ++i, dst_texel += 4)
                    {
                        texel = block[j][i];
                        dst_texel[0] = texel[2];
                        dst_texel[1] = texel[1];
                        dst_texel[2] = texel[0];
                        dst_texel[3] = texel[3];
                    }
                }
            }
        }
    }
}

static void decompress_bc6h(const BYTE *src, BYTE *dst, unsigned int src_row_pitch,
        unsigned int src_slice_pitch, unsigned int dst_row_pitch, unsigned int dst_slice_pitch,
        unsigned int width, unsigned int height, unsigned int depth, int is_signed)
{
    unsigned int x, y, z, i, j, block_w, block_h;
    unsigned short block[4][4][3];
    unsigned short *dst_texel;
    const BYTE *src_row;

    for (z = 0; z < depth; ++z)
    {
        for (y = 0; y < height; y += 4)
        {
            src_row = &src[z * src_slice_pitch + (y / 4) * src_row_pitch];
            block_h = min(height - y, 4);
            for (x = 0; x < width; x += 4)
            {
                block_w = min(width - x, 4);
                /* The pitch is in units of the half floats it writes. */
                bcdec_bc6h_half(&src_row[(x / 4) * BCDEC_BC6H_BLOCK_SIZE], block, 4 * 3, is_signed);
                for (j = 0; j < block_h; ++j)
                {
                    dst_texel = (unsigned short *)&dst[z * dst_slice_pitch + (y + j) * dst_row_pitch + x * 8];
                    for (i = 0; i < block_w; ++i, dst_texel += 4)
                    {
                        dst_texel[0] = block[j][i][0];
                        dst_texel[1] = block[j][i][1];
                        dst_texel[2] = block[j][i][2];
                        dst_texel[3] = 0x3c00; /* 1.0 */
                    }
                }
            }
        }
    }
}

void wined3d_decompress_bc6h_uf16(const BYTE *src, BYTE *dst, unsigned int src_row_pitch,
        unsigned int src_slice_pitch, unsigned int dst_row_pitch, unsigned int dst_slice_pitch,
        unsigned int width, unsigned int height, unsigned int depth)
{
    decompress_bc6h(src, dst, src_row_pitch, src_slice_pitch, dst_row_pitch, dst_slice_pitch,
            width, height, depth, 0);
}

void wined3d_decompress_bc6h_sf16(const BYTE *src, BYTE *dst, unsigned int src_row_pitch,
        unsigned int src_slice_pitch, unsigned int dst_row_pitch, unsigned int dst_slice_pitch,
        unsigned int width, unsigned int height, unsigned int depth)
{
    decompress_bc6h(src, dst, src_row_pitch, src_slice_pitch, dst_row_pitch, dst_slice_pitch,
            width, height, depth, 1);
}
