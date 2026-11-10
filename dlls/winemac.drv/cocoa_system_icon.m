/*
 * WINESKIN: the Mac's own alert icons for Windows' stock icons (include/wine/wineskin_system_icon.h)
 *
 * The icons come from the running system (CoreTypes.bundle) and are drawn at the size asked
 * for from the icns representation that fits it best, so they are sharp at any size.
 *
 * Copyright 2026 WFH Wineskin
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

#import "config.h"
#import "macdrv.h"

#import <Foundation/Foundation.h>

#define CORE_TYPES "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/"

static const char *icon_path(UINT id)
{
    switch (id)
    {
    case 32513: return CORE_TYPES "AlertStopIcon.icns";     /* IDI_HAND, IDI_ERROR */
    case 32515: return CORE_TYPES "AlertCautionIcon.icns";  /* IDI_EXCLAMATION, IDI_WARNING */
    case 32516:                                             /* IDI_ASTERISK, IDI_INFORMATION */
    case 32514: return CORE_TYPES "AlertNoteIcon.icns";     /* IDI_QUESTION: OS X has no question alert */
    }
    return NULL;
}

/***********************************************************************
 *              macdrv_system_icon
 */
NTSTATUS macdrv_system_icon(void *arg)
{
@autoreleasepool
{
    struct system_icon_params *params = arg;
    UINT width = params->width, height = params->height, *bits = (UINT *)(ULONG_PTR)params->bits;
    const char *path = icon_path(params->id);
    CGImageSourceRef source;
    CGImageRef image = NULL;
    CGColorSpaceRef space;
    CGContextRef context;
    size_t i, count, best = 0, best_width = 0;

    if (!path || !width || !height || width > 1024 || height > 1024 || !bits) return STATUS_INVALID_PARAMETER;
    if (!(source = CGImageSourceCreateWithURL((CFURLRef)[NSURL fileURLWithPath:@(path)], NULL)))
        return STATUS_NOT_FOUND;

    /* The smallest representation at least as large as the target, else the largest. */
    count = CGImageSourceGetCount(source);
    for (i = 0; i < count; i++)
    {
        CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(source, i, NULL);
        CFNumberRef number = props ? CFDictionaryGetValue(props, kCGImagePropertyPixelWidth) : NULL;
        int w = 0;

        if (number) CFNumberGetValue(number, kCFNumberIntType, &w);
        if (props) CFRelease(props);
        if (w <= 0) continue;
        if (!best_width || (best_width < max(width, height) ? (size_t)w > best_width
                                                           : ((size_t)w >= max(width, height) && (size_t)w < best_width)))
        {
            best = i;
            best_width = w;
        }
    }
    if (count) image = CGImageSourceCreateImageAtIndex(source, best, NULL);
    CFRelease(source);
    if (!image) return STATUS_NOT_FOUND;

    space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    context = CGBitmapContextCreate(bits, width, height, 8, width * 4, space,
                                    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(space);
    if (!context)
    {
        CGImageRelease(image);
        return STATUS_NO_MEMORY;
    }
    CGContextClearRect(context, CGRectMake(0, 0, width, height));
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    CGImageRelease(image);

    /* Windows icons have straight alpha. */
    for (i = 0; i < (size_t)width * height; i++)
    {
        UINT p = bits[i], a = p >> 24;
        if (a && a < 255)
        {
            UINT b = (p & 0xff) * 255 / a, g = ((p >> 8) & 0xff) * 255 / a, r = ((p >> 16) & 0xff) * 255 / a;
            bits[i] = (a << 24) | (min(r, 255) << 16) | (min(g, 255) << 8) | min(b, 255);
        }
    }
    return STATUS_SUCCESS;
}
}
