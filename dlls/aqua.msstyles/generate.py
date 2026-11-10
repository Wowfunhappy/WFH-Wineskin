#!/usr/bin/env python
#
# WINESKIN: generator for the Aqua visual style (aqua.msstyles).
#
# Draws every bitmap the theme uses with CoreGraphics and writes them, together
# with aqua.rc (the resource script holding the theme ini), into this directory.
# The bitmaps are 32-bit BMPs with a BITMAPV5HEADER and straight alpha, the
# format of the aero.msstyles bitmaps.
#
#   python generate.py                 regenerate aqua.rc and the *.bmp files
#   python generate.py --preview DIR   also write PNG previews and a contact sheet
#
# Needs PyObjC's Quartz bindings: the system Python of OS X has them, and so
# does any Python with pyobjc-framework-Quartz installed. Runs on Python 2.7 and 3.
#
# Copyright 2026 WFH Wineskin contributors
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA

from __future__ import division, print_function

import math
import os
import struct
import sys

import Quartz as Q

HERE = os.path.dirname(os.path.abspath(__file__))
SRGB = Q.CGColorSpaceCreateWithName(Q.kCGColorSpaceSRGB)

# Bitmaps for parts drawn at their true size come in one variant per DPI step.
DPIS = (96, 120, 144, 192)


def scale_of(dpi):
    return dpi / 96.0


def ipx(v):
    return int(math.floor(v + 0.5))


# ---------------------------------------------------------------------------
# Colours and gradients
# ---------------------------------------------------------------------------

def C(*v):
    """A colour from 0-255 components: C(g), C(g, a), C(r, g, b) or C(r, g, b, a); alpha is 0-1."""
    if len(v) == 1:
        return (v[0] / 255.0,) * 3 + (1.0,)
    if len(v) == 2:
        return (v[0] / 255.0,) * 3 + (float(v[1]),)
    if len(v) == 3:
        return (v[0] / 255.0, v[1] / 255.0, v[2] / 255.0, 1.0)
    return (v[0] / 255.0, v[1] / 255.0, v[2] / 255.0, float(v[3]))


def stops(*pairs):
    """Gradient stops: stops((0.0, colour), (0.5, colour), ...)."""
    return [(float(p), c) for p, c in pairs]


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(4))


def shade(st, f):
    """Darken (f < 1) or lighten (f > 1) every stop of a gradient."""
    out = []
    for p, c in st:
        if f <= 1:
            out.append((p, (c[0] * f, c[1] * f, c[2] * f, c[3])))
        else:
            g = f - 1
            out.append((p, (c[0] + (1 - c[0]) * g, c[1] + (1 - c[1]) * g, c[2] + (1 - c[2]) * g, c[3])))
    return out


def fade(st, a):
    return [(p, (c[0], c[1], c[2], c[3] * a)) for p, c in st]


def sub_range(st, t0, t1):
    """The part of a gradient between t0 and t1, renormalised to 0..1."""
    def at(t):
        return colour_at(st, t)
    out = [(0.0, at(t0))]
    for p, c in st:
        if t0 < p < t1:
            out.append(((p - t0) / (t1 - t0), c))
    out.append((1.0, at(t1)))
    return out


# The light bezel shared by push buttons, check boxes, radio buttons, steppers,
# segments and slider knobs: white upper half, a soft step at the middle, and a
# slightly brighter bottom edge.
LIGHT = stops((0.00, C(255)), (0.24, C(255)), (0.49, C(243)), (0.50, C(236)),
              (0.82, C(237)), (1.00, C(242)))
LIGHT_PRESSED = stops((0.00, C(214)), (0.49, C(205)), (0.50, C(198)), (1.00, C(206)))
LIGHT_BORDER = stops((0.00, C(154)), (0.15, C(145)), (1.00, C(144)))
SHADOW = C(0, 0.06)

# Accent colours: the default and pressed push buttons and the progress fill.
ACCENTS = {
    'Blue': {
        'prefix': 'aqua',
        'fill': stops((0.00, C(166, 212, 240)), (0.18, C(124, 190, 233)), (0.49, C(36, 165, 229)),
                      (0.50, C(0, 148, 228)), (0.80, C(0, 172, 234)), (1.00, C(126, 208, 238))),
        'border': stops((0.00, C(38, 78, 158)), (0.30, C(52, 86, 146)), (1.00, C(34, 50, 98))),
        # The default push button (the middle of its pulse) and the pressed one, as HIThemeDrawButton
        # draws them on OS X 10.9.
        'button': stops((0.00, C(216, 238, 249)), (0.055, C(200, 224, 249)), (0.11, C(192, 216, 245)),
                        (0.52, C(152, 198, 245)), (0.53, C(124, 186, 245)), (0.78, C(158, 208, 249)),
                        (1.00, C(200, 236, 249))),
        'button_border': stops((0.00, C(64, 71, 166, 0.86)), (0.50, C(60, 65, 133, 0.86)),
                               (1.00, C(56, 59, 95, 0.86))),
        'button_pressed': stops((0.00, C(188, 214, 239)), (0.055, C(163, 193, 239)), (0.11, C(152, 184, 233)),
                                (0.52, C(103, 161, 233)), (0.53, C(70, 147, 233)), (0.78, C(111, 174, 238)),
                                (1.00, C(171, 212, 239))),
        'button_pressed_border': stops((0.00, C(52, 60, 162, 0.86)), (0.50, C(48, 52, 130, 0.86)),
                                       (1.00, C(42, 45, 95, 0.86))),
        'highlight': (56, 117, 215),
        'menu_highlight': (64, 128, 236),
        'hot_tracking': (36, 104, 210),
    },
    'Graphite': {
        'prefix': 'graphite',
        'fill': stops((0.00, C(232, 236, 242)), (0.18, C(212, 219, 228)), (0.49, C(174, 185, 199)),
                      (0.50, C(158, 170, 186)), (0.80, C(172, 184, 199)), (1.00, C(214, 221, 230))),
        'border': stops((0.00, C(110, 119, 134)), (0.30, C(102, 111, 126)), (1.00, C(76, 83, 96))),
        'highlight': (117, 129, 146),
        'menu_highlight': (124, 136, 153),
        'hot_tracking': (90, 102, 120),
    },
}


def colour_at(st, t):
    """The colour of a gradient at t; two stops at one position make a hard step."""
    if t <= st[0][0]:
        return st[0][1]
    for i in range(1, len(st)):
        if t < st[i][0] or (t == st[i][0] and (i + 1 == len(st) or st[i + 1][0] != t)):
            p0, c0 = st[i - 1]
            p1, c1 = st[i]
            return c1 if p1 == p0 else mix(c0, c1, (t - p0) / (p1 - p0))
    return st[-1][1]


# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

def rrect(x, y, w, h, r, corners=('tl', 'tr', 'br', 'bl')):
    r = max(0.0, min(r, w / 2.0, h / 2.0))
    rtl = r if 'tl' in corners else 0
    rtr = r if 'tr' in corners else 0
    rbr = r if 'br' in corners else 0
    rbl = r if 'bl' in corners else 0
    p = Q.CGPathCreateMutable()
    Q.CGPathMoveToPoint(p, None, x + rtl, y)
    Q.CGPathAddLineToPoint(p, None, x + w - rtr, y)
    if rtr:
        Q.CGPathAddArcToPoint(p, None, x + w, y, x + w, y + rtr, rtr)
    Q.CGPathAddLineToPoint(p, None, x + w, y + h - rbr)
    if rbr:
        Q.CGPathAddArcToPoint(p, None, x + w, y + h, x + w - rbr, y + h, rbr)
    Q.CGPathAddLineToPoint(p, None, x + rbl, y + h)
    if rbl:
        Q.CGPathAddArcToPoint(p, None, x, y + h, x, y + h - rbl, rbl)
    Q.CGPathAddLineToPoint(p, None, x, y + rtl)
    if rtl:
        Q.CGPathAddArcToPoint(p, None, x, y, x + rtl, y, rtl)
    Q.CGPathCloseSubpath(p)
    return p


def rect(x, y, w, h):
    p = Q.CGPathCreateMutable()
    Q.CGPathAddRect(p, None, Q.CGRectMake(x, y, w, h))
    return p


def ellipse(x, y, w, h):
    p = Q.CGPathCreateMutable()
    Q.CGPathAddEllipseInRect(p, None, Q.CGRectMake(x, y, w, h))
    return p


def poly(points, close=True):
    p = Q.CGPathCreateMutable()
    Q.CGPathMoveToPoint(p, None, points[0][0], points[0][1])
    for x, y in points[1:]:
        Q.CGPathAddLineToPoint(p, None, x, y)
    if close:
        Q.CGPathCloseSubpath(p)
    return p


def knob_path(x, y, w, h, r, tip):
    """A slider knob pointing down: rounded top, straight sides, a point of height tip."""
    body = h - tip
    p = Q.CGPathCreateMutable()
    Q.CGPathMoveToPoint(p, None, x + r, y)
    Q.CGPathAddLineToPoint(p, None, x + w - r, y)
    Q.CGPathAddArcToPoint(p, None, x + w, y, x + w, y + r, r)
    Q.CGPathAddLineToPoint(p, None, x + w, y + body)
    Q.CGPathAddLineToPoint(p, None, x + w / 2.0, y + h)
    Q.CGPathAddLineToPoint(p, None, x, y + body)
    Q.CGPathAddLineToPoint(p, None, x, y + r)
    Q.CGPathAddArcToPoint(p, None, x, y, x + r, y, r)
    Q.CGPathCloseSubpath(p)
    return p


# ---------------------------------------------------------------------------
# Rasters
# ---------------------------------------------------------------------------

class Raster(object):
    """Straight-alpha RGBA pixels, rows top to bottom."""

    def __init__(self, w, h, data=None):
        self.w, self.h = w, h
        self.data = bytearray(data) if data is not None else bytearray(w * h * 4)

    def px(self, x, y):
        i = (y * self.w + x) * 4
        return tuple(self.data[i:i + 4])

    def set(self, x, y, c):
        i = (y * self.w + x) * 4
        self.data[i:i + 4] = bytearray(c)

    def transposed(self):
        out = Raster(self.h, self.w)
        for y in range(self.h):
            for x in range(self.w):
                out.set(y, x, self.px(x, y))
        return out

    def flipped(self):
        out = Raster(self.w, self.h)
        row = self.w * 4
        for y in range(self.h):
            out.data[y * row:(y + 1) * row] = self.data[(self.h - 1 - y) * row:(self.h - y) * row]
        return out

    def opaque(self):
        return all(self.data[i] == 255 for i in range(3, len(self.data), 4))


def vstack(frames):
    w = frames[0].w
    for f in frames:
        assert f.w == w, 'frame widths differ'
    out = Raster(w, sum(f.h for f in frames))
    off = 0
    for f in frames:
        out.data[off:off + len(f.data)] = f.data
        off += len(f.data)
    return out


class Canvas(object):
    """A CoreGraphics bitmap with a top-left origin, y growing downwards."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.ctx = Q.CGBitmapContextCreate(None, w, h, 8, w * 4, SRGB, Q.kCGImageAlphaPremultipliedLast)
        Q.CGContextTranslateCTM(self.ctx, 0, h)
        Q.CGContextScaleCTM(self.ctx, 1, -1)
        Q.CGContextSetShouldAntialias(self.ctx, True)

    def fill(self, path, paint, y0=None, y1=None, x0=None, x1=None, eo=False):
        c = self.ctx
        Q.CGContextSaveGState(c)
        Q.CGContextAddPath(c, path)
        if isinstance(paint, tuple):
            Q.CGContextSetRGBFillColor(c, *paint)
            if eo:
                Q.CGContextEOFillPath(c)
            else:
                Q.CGContextFillPath(c)
        else:
            box = Q.CGPathGetBoundingBox(path)
            if eo:
                Q.CGContextEOClip(c)
            else:
                Q.CGContextClip(c)
            if x0 is not None:
                self.gradient(paint, x0, x1, horizontal=True)
            else:
                if y0 is None:
                    y0, y1 = box.origin.y, box.origin.y + box.size.height
                self.gradient(paint, y0, y1)
        Q.CGContextRestoreGState(c)

    def stroke(self, path, width, paint, y0=None, y1=None, cap=None, join=None):
        c = self.ctx
        Q.CGContextSaveGState(c)
        Q.CGContextAddPath(c, path)
        Q.CGContextSetLineWidth(c, width)
        Q.CGContextSetLineJoin(c, join if join is not None else Q.kCGLineJoinRound)
        Q.CGContextSetLineCap(c, cap if cap is not None else Q.kCGLineCapButt)
        if isinstance(paint, tuple):
            Q.CGContextSetRGBStrokeColor(c, *paint)
            Q.CGContextStrokePath(c)
        else:
            box = Q.CGPathGetBoundingBox(path)
            if y0 is None:
                y0, y1 = box.origin.y - width / 2.0, box.origin.y + box.size.height + width / 2.0
            Q.CGContextReplacePathWithStrokedPath(c)
            Q.CGContextClip(c)
            self.gradient(paint, y0, y1)
        Q.CGContextRestoreGState(c)

    def gradient(self, st, a, b, horizontal=False):
        """Fill the clip area with a linear gradient from a to b, one solid band per pixel row or column."""
        c = self.ctx
        n = self.w if horizontal else self.h
        for i in range(n):
            t = (i + 0.5 - a) / float(b - a) if b != a else 0.0
            Q.CGContextSetRGBFillColor(c, *colour_at(st, max(0.0, min(1.0, t))))
            if horizontal:
                Q.CGContextFillRect(c, Q.CGRectMake(i, 0, 1, self.h))
            else:
                Q.CGContextFillRect(c, Q.CGRectMake(0, i, self.w, 1))

    def clip(self, path):
        Q.CGContextAddPath(self.ctx, path)
        Q.CGContextClip(self.ctx)

    def save(self):
        Q.CGContextSaveGState(self.ctx)

    def restore(self):
        Q.CGContextRestoreGState(self.ctx)

    def alpha(self, a):
        Q.CGContextSetAlpha(self.ctx, a)

    def raster(self):
        img = Q.CGBitmapContextCreateImage(self.ctx)
        raw = bytearray(bytes(Q.CGDataProviderCopyData(Q.CGImageGetDataProvider(img))))
        stride = Q.CGImageGetBytesPerRow(img)
        out = Raster(self.w, self.h)
        for y in range(self.h):
            for x in range(self.w):
                i = y * stride + x * 4
                r, g, b, a = raw[i], raw[i + 1], raw[i + 2], raw[i + 3]
                if a and a < 255:
                    r = min(255, (r * 255 + a // 2) // a)
                    g = min(255, (g * 255 + a // 2) // a)
                    b = min(255, (b * 255 + a // 2) // a)
                elif not a:
                    r = g = b = 0
                out.set(x, y, (r, g, b, a))
        return out


def bezel(cv, x, y, w, h, r, fill, border, bw=1.0, corners=('tl', 'tr', 'br', 'bl'), shadow=None):
    """A filled shape with a border of width bw lying inside the box (x, y, w, h)."""
    if shadow:
        cv.fill(rrect(x, y + bw, w, h, r, corners), shadow)
    half = bw / 2.0
    path = rrect(x + half, y + half, w - bw, h - bw, max(0.0, r - half), corners)
    cv.fill(path, fill, y0=y + bw, y1=y + h - bw)
    cv.stroke(path, bw, border, y0=y, y1=y + h)


# ---------------------------------------------------------------------------
# Bitmap output
# ---------------------------------------------------------------------------

def bmp_bytes(r):
    """A 32-bit BITMAPV5HEADER BMP, bottom-up, BI_BITFIELDS, straight alpha."""
    size_image = r.w * r.h * 4
    off = 14 + 124
    out = bytearray(b'BM')
    out += struct.pack('<IHHI', off + size_image, 0, 0, off)
    out += struct.pack('<IiiHHIIiiII', 124, r.w, r.h, 1, 32, 3, size_image, 0, 0, 0, 0)
    out += struct.pack('<IIII', 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000)
    out += b'BGRs'
    out += b'\0' * 36            # CIEXYZTRIPLE endpoints
    out += struct.pack('<III', 0, 0, 0)  # gamma
    out += struct.pack('<IIII', 4, 0, 0, 0)  # LCS_GM_IMAGES, profile data, profile size, reserved
    assert len(out) == off
    row = r.w * 4
    for y in range(r.h - 1, -1, -1):
        src = r.data[y * row:(y + 1) * row]
        for x in range(r.w):
            rr, gg, bb, aa = src[x * 4:x * 4 + 4]
            out += bytearray((bb, gg, rr, aa))
    return bytes(out)


def to_cgimage(r):
    """A premultiplied CGImage of a raster, for previews."""
    pre = bytearray(len(r.data))
    for i in range(0, len(r.data), 4):
        a = r.data[i + 3]
        pre[i] = (r.data[i] * a + 127) // 255
        pre[i + 1] = (r.data[i + 1] * a + 127) // 255
        pre[i + 2] = (r.data[i + 2] * a + 127) // 255
        pre[i + 3] = a
    provider = Q.CGDataProviderCreateWithCFData(Q.NSData.dataWithBytes_length_(bytes(pre), len(pre)))
    return Q.CGImageCreate(r.w, r.h, 8, 32, r.w * 4, SRGB, Q.kCGImageAlphaPremultipliedLast,
                           provider, None, False, Q.kCGRenderingIntentDefault)


def write_png(path, cgimage):
    url = Q.CFURLCreateFromFileSystemRepresentation(None, path.encode('utf-8'), len(path.encode('utf-8')), False)
    dest = Q.CGImageDestinationCreateWithURL(url, 'public.png', 1, None)
    Q.CGImageDestinationAddImage(dest, cgimage, None)
    Q.CGImageDestinationFinalize(dest)


# ---------------------------------------------------------------------------
# The bitmaps
# ---------------------------------------------------------------------------

BITMAPS = {}       # file name -> Raster
BITMAP_ORDER = []


def emit(name, frames):
    r = vstack(frames) if isinstance(frames, list) else frames
    for i in range(0, r.w * r.h * 4, 4):
        # RGB(255, 0, 255) marks transparent pixels in opaque theme bitmaps; keep clear of it.
        if r.data[i] == 255 and r.data[i + 1] == 0 and r.data[i + 2] == 255 and r.data[i + 3] == 255:
            r.data[i + 1] = 1
    BITMAPS[name] = r
    BITMAP_ORDER.append(name)
    return name


def dpi_name(base, dpi):
    return '%s_%ddpi.bmp' % (base, dpi)


def frames_of(w, h, n, draw):
    out = []
    for i in range(n):
        cv = Canvas(w, h)
        draw(cv, i)
        out.append(cv.raster())
    return out


# --- push button -----------------------------------------------------------

BUTTON_MARGIN = 9    # sizing margin; covers the 2x corner radius


def gen_button(accent, dpi):
    s = scale_of(dpi)
    w, h = ipx(26 * s), ipx(23 * s)
    inset = ipx(1 * s)
    bh = h - inset - ipx(1 * s)
    r = 4 * s
    blue_fill, blue_border = accent.get('button', accent['fill']), accent.get('button_border', accent['border'])
    pressed_fill = accent.get('button_pressed') or shade(blue_fill, 0.88)
    pressed_border = accent.get('button_pressed_border') or shade(blue_border, 0.85)
    # PBS_NORMAL, PBS_HOT, PBS_PRESSED, PBS_DISABLED, PBS_DEFAULTED, PBS_DEFAULTED_ANIMATING
    looks = [(LIGHT, LIGHT_BORDER, 1.0), (LIGHT, LIGHT_BORDER, 1.0), (pressed_fill, pressed_border, 1.0),
             (LIGHT, LIGHT_BORDER, 0.5), (blue_fill, blue_border, 1.0), (blue_fill, blue_border, 1.0)]

    def draw(cv, i):
        fill, border, alpha = looks[i]
        cv.alpha(alpha)
        bezel(cv, inset, inset, w - 2 * inset, bh, r, fill, border, bw=s, shadow=SHADOW)
    return frames_of(w, h, 6, draw)


# --- check box and radio button --------------------------------------------

CHECK_DARK = C(53)


def gen_checkbox(dpi):
    s = scale_of(dpi)
    w, h = ipx(16 * s), ipx(18 * s)
    box = ipx(14 * s)
    by = ipx(2 * s)
    r = 3.5 * s

    def mark(cv, kind, colour):
        if kind == 'check':
            pts = [(3.3 * s, by + 7.4 * s), (6.1 * s, by + 10.9 * s), (14.6 * s, by - 1.2 * s)]
            path = poly(pts, close=False)
            cv.stroke(path, 2.2 * s, colour, cap=Q.kCGLineCapRound)
        elif kind == 'mixed':
            cv.fill(rrect(3.5 * s, by + box / 2.0 - 1.1 * s, box - 7 * s, 2.2 * s, 1.0 * s), colour)

    # CBS_{UNCHECKED,CHECKED,MIXED,IMPLICIT,EXCLUDED} x {NORMAL,HOT,PRESSED,DISABLED}
    def draw(cv, i):
        kind = ('none', 'check', 'mixed', 'check', 'none')[i // 4]
        state = i % 4
        fill, border, alpha, colour = LIGHT, LIGHT_BORDER, 1.0, CHECK_DARK
        if state == 2:
            fill = LIGHT_PRESSED
        if i // 4 == 3:
            colour = C(140)
        if state == 3:
            alpha = 0.5
        cv.alpha(alpha)
        bezel(cv, 0, by, box, box, r, fill, border, bw=s, shadow=SHADOW)
        mark(cv, kind, colour)
    return frames_of(w, h, 20, draw)


def gen_radio(dpi):
    s = scale_of(dpi)
    w, h = ipx(16 * s), ipx(18 * s)
    d = ipx(16 * s)
    oy = ipx(1 * s)

    # RBS_{UNCHECKED,CHECKED} x {NORMAL,HOT,PRESSED,DISABLED}
    def draw(cv, i):
        checked, state = i // 4, i % 4
        cv.alpha(0.5 if state == 3 else 1.0)
        cv.fill(ellipse(0, oy + s, d, d), SHADOW)
        half = s / 2.0
        path = ellipse(half, oy + half, d - s, d - s)
        cv.fill(path, LIGHT_PRESSED if state == 2 else LIGHT, y0=oy + s, y1=oy + d - s)
        cv.stroke(path, s, stops((0, C(143)), (0.5, C(135)), (1, C(134))), y0=oy, y1=oy + d)
        if checked:
            dot = 4.8 * s
            cv.fill(ellipse(d / 2.0 - dot / 2.0, oy + d / 2.0 - dot / 2.0, dot, dot), CHECK_DARK)
    return frames_of(w, h, 8, draw)


# --- group box --------------------------------------------------------------

def gen_groupbox():
    w = h = 12
    cv = Canvas(w, h)
    outer = rrect(0, 0, w, h, 4.5)
    ring1 = Q.CGPathCreateMutableCopy(outer)
    Q.CGPathAddPath(ring1, None, rrect(1, 1, w - 2, h - 2, 3.5))
    cv.fill(ring1, C(0, 0.15), eo=True)
    ring2 = Q.CGPathCreateMutableCopy(rrect(1, 1, w - 2, h - 2, 3.5))
    Q.CGPathAddPath(ring2, None, rrect(2, 2, w - 4, h - 4, 2.5))
    cv.fill(ring2, stops((0, C(0, 0.09)), (0.4, C(0, 0.05)), (1, C(0, 0.04))), y0=1, y1=h - 1, eo=True)
    return [cv.raster()]


# --- text field (edit, combo box frame, list borders) -----------------------

def field_frame(w, h, outer_top, outer_side, outer_bottom, inner_top, inner_side, inner_bottom, centre=255):
    r = Raster(w, h)
    for y in range(h):
        for x in range(w):
            r.set(x, y, (centre, centre, centre, 255))
    for x in range(w):
        r.set(x, 0, outer_top + (255,))
        r.set(x, h - 1, outer_bottom + (255,))
    for y in range(1, h - 1):
        t = (y - 1) / float(max(1, h - 3))
        col = tuple(int(round(outer_side[0][k] + (outer_side[1][k] - outer_side[0][k]) * t)) for k in range(3))
        r.set(0, y, col + (255,))
        r.set(w - 1, y, col + (255,))
    for x in range(1, w - 1):
        r.set(x, 1, inner_top + (255,))
        r.set(x, h - 2, inner_bottom + (255,))
    for y in range(2, h - 2):
        r.set(1, y, inner_side + (255,))
        r.set(w - 2, y, inner_side + (255,))
    return r


def G(v):
    return (v, v, v)


def gen_edit():
    w = h = 8
    normal = field_frame(w, h, G(155), (G(162), G(181)), G(186), G(227), G(245), G(255))
    disabled = field_frame(w, h, G(196), (G(200), G(208)), G(210), G(240), G(250), G(255))
    focus = field_frame(w, h, (128, 172, 232), ((128, 172, 232), (128, 172, 232)), (128, 172, 232),
                        (92, 151, 226), (92, 151, 226), (92, 151, 226))
    # ETS_NORMAL, ETS_HOT, ETS_SELECTED, ETS_DISABLED, ETS_FOCUSED, ETS_READONLY
    return [normal, normal, normal, disabled, focus, normal]


def gen_listbox():
    return [field_frame(8, 8, G(168), (G(186), G(190)), G(190), G(255), G(255), G(255))]


# --- combo box drop-down button ---------------------------------------------

def gen_combo_button():
    w, h = 15, 17

    # CBXS_NORMAL, CBXS_HOT, CBXS_PRESSED, CBXS_DISABLED
    def draw(cv, i):
        fill = LIGHT_PRESSED if i == 2 else LIGHT
        if i == 3:
            fill = fade(LIGHT, 0.5)
        cv.fill(rect(1, 0, w - 1, h), sub_range(fill, 0.1, 1.0))
        line = stops((0, C(152)), (1, C(179))) if i != 3 else stops((0, C(206)), (1, C(214)))
        cv.fill(rect(0, 0, 1, h), line)
    return frames_of(w, h, 4, draw)


def gen_combo_glyph(dpi):
    s = scale_of(dpi)
    w, h = ipx(9 * s), ipx(7 * s)

    def draw(cv, i):
        colour = C(178) if i == 3 else C(66)
        tw, th = 7 * s, 4.6 * s
        x0, y0 = (w - tw) / 2.0, (h - th) / 2.0 + 0.3 * s
        cv.fill(poly([(x0, y0), (x0 + tw, y0), (x0 + tw / 2.0, y0 + th)]), colour)
    return frames_of(w, h, 4, draw)


# --- header -----------------------------------------------------------------

def gen_header_item():
    w, h = 10, 18

    # HIS_NORMAL, HOT, PRESSED, SORTEDNORMAL, SORTEDHOT, SORTEDPRESSED, ICON*
    def draw(cv, i):
        pressed = (i % 3) == 2
        cv.fill(rect(0, 0, w, h - 1), shade(LIGHT, 0.9) if pressed else LIGHT)
        cv.fill(rect(0, h - 1, w, 1), C(182))
        cv.fill(rect(w - 1, 2, 1, h - 4), stops((0, C(206)), (0.5, C(190)), (1, C(205))))
    return frames_of(w, h, 12, draw)


# --- progress bar -------------------------------------------------------------

PROGRESS_TRACK_IN = stops((0, C(197)), (0.12, C(209)), (0.3, C(216)), (1, C(226)))


def gen_progress_track():
    w, h = 16, 18
    cv = Canvas(w, h)
    cv.fill(rrect(0, 1, w, h - 1, 3.5), C(255, 0.25))
    bezel(cv, 0, 0, w, h - 1, 3.5, PROGRESS_TRACK_IN, stops((0, C(152)), (1, C(166))))
    return [cv.raster()]


def glossy(top, mid_hi, mid_lo, bottom):
    return stops((0, top), (0.49, mid_hi), (0.5, mid_lo), (1, bottom))


def progress_fills(accent):
    return [
        sub_range(accent['fill'], 0.0, 1.0),                                    # PBFS_NORMAL
        glossy(C(250, 196, 192), C(222, 92, 84), C(206, 52, 44), C(240, 140, 130)),   # PBFS_ERROR
        glossy(C(255, 240, 180), C(240, 206, 72), C(230, 186, 30), C(250, 226, 120)),  # PBFS_PAUSED
        glossy(C(232), C(200), C(184), C(223)),                                # PBFS_PARTIAL
    ]


def gen_progress_fill(accent):
    w, h = 16, 16
    track = sub_range(PROGRESS_TRACK_IN, 1 / 17.0, 16 / 17.0)

    def draw(cv, i):
        fill = progress_fills(accent)[i]
        cv.fill(rect(0, 0, w, h), track)
        cv.fill(rrect(0, 0, w, h, 2.5, ('tl', 'bl')), fill)
        cv.fill(rect(w - 1, 0, 1, h), C(0, 0.18))
    return frames_of(w, h, 4, draw)


# --- slider -------------------------------------------------------------------

def gen_trackbar_track():
    w, h = 12, 5
    cv = Canvas(w, h)
    bezel(cv, 0, 0, w, h, 2.5, stops((0, C(170)), (0.5, C(193)), (1, C(205))), stops((0, C(116)), (1, C(174))))
    return [cv.raster()]


TRACKBAR_THUMB_WIDTH = {96: 11, 120: 13, 144: 15, 192: 21}   # (thumb length / 2) | 1


def knob_frames(dpi, shape):
    """Slider knobs: 'down' and 'up' point at the ticks, 'pill' is used with ticks on both sides."""
    s = scale_of(dpi)
    w = TRACKBAR_THUMB_WIDTH[dpi]
    h = ipx(19 * s)
    body = h - ipx(1 * s)
    half = s / 2.0

    def shaped(x, y, ww, hh):
        if shape == 'pill':
            return rrect(x, y, ww, hh, ww / 2.0)
        p = knob_path(x, y, ww, hh, 3.5 * s, 6.0 * s)
        if shape == 'up':
            t = Q.CGAffineTransformMake(1, 0, 0, -1, 0, 2 * y + hh)
            flipped = Q.CGPathCreateMutable()
            Q.CGPathAddPath(flipped, t, p)
            p = flipped
        return p

    # TUS_NORMAL, TUS_HOT, TUS_PRESSED, TUS_FOCUSED, TUS_DISABLED
    def draw(cv, i):
        fill = LIGHT_PRESSED if i == 2 else LIGHT
        cv.alpha(0.5 if i == 4 else 1.0)
        outline = shaped(half, half, w - s, body - s)
        cv.fill(shaped(0, s, w, body), SHADOW)
        cv.fill(outline, fill, y0=s, y1=body - s)
        cv.stroke(outline, s, LIGHT_BORDER, y0=0, y1=body)
    return frames_of(w, h, 5, draw)


# --- stepper (up-down) ----------------------------------------------------------

def gen_spin(half):
    """One half of a stepper: the up half is the top of a rounded bezel, the down half its bottom."""
    w, h = 13, 11
    r = 3.5
    extra = 8      # how far the open end of each half reaches past the image

    # UPS_NORMAL, UPS_HOT, UPS_PRESSED, UPS_DISABLED
    def draw(cv, i):
        fill = LIGHT_PRESSED if i == 2 else LIGHT
        cv.alpha(0.5 if i == 3 else 1.0)
        cv.save()
        cv.clip(rect(0, 0, w, h))
        if half == 'up':
            path = rrect(1.5, 1.5, w - 3, h - 1.5 + extra, r - 0.5)
            cv.fill(path, sub_range(fill, 0.0, 0.5), y0=2, y1=h)
            cv.stroke(path, 1, LIGHT_BORDER, y0=1, y1=2 * h)
        else:
            cv.fill(rrect(1, -extra + 1, w - 2, h - 1 + extra, r), SHADOW)
            path = rrect(1.5, -extra + 0.5, w - 3, h - 2 + extra, r - 0.5)
            cv.fill(path, sub_range(fill, 0.5, 1.0), y0=0, y1=h - 2)
            cv.stroke(path, 1, C(144))
        cv.restore()
    return frames_of(w, h, 4, draw)


def gen_spin_glyph(dpi, direction):
    s = scale_of(dpi)
    w, h = ipx(7 * s), ipx(5 * s)

    def draw(cv, i):
        colour = C(180) if i == 3 else C(72)
        tw, th = 5.4 * s, 3.4 * s
        x0, y0 = (w - tw) / 2.0, (h - th) / 2.0
        if direction == 'up':
            pts = [(x0, y0 + th), (x0 + tw, y0 + th), (x0 + tw / 2.0, y0)]
        else:
            pts = [(x0, y0), (x0 + tw, y0), (x0 + tw / 2.0, y0 + th)]
        cv.fill(poly(pts), colour)
    return frames_of(w, h, 4, draw)


# --- scroll bar ------------------------------------------------------------------

# Cross-section of the legacy (always shown) OS X 10.9 scroller track, 15 pixels wide.
TRACK_PROFILE = [228, 242, 244, 245, 248, 249, 250, 252, 252, 253, 253, 252, 250, 248, 239]
KNOB = {'normal': 193, 'hot': 166, 'pressed': 132, 'hover': 182}


def track_profile(width):
    """The track cross-section resampled to width pixels; the edge line stays one pixel."""
    out = [TRACK_PROFILE[0]]
    rest = TRACK_PROFILE[1:]
    n = width - 1
    for i in range(n):
        t = (i + 0.5) / n * len(rest) - 0.5
        t = max(0.0, min(len(rest) - 1.0, t))
        a = int(math.floor(t))
        b = min(len(rest) - 1, a + 1)
        out.append(int(round(rest[a] + (rest[b] - rest[a]) * (t - a))))
    return out


def scroll_width(dpi):
    return ipx(15 * scale_of(dpi))


def track_canvas(w, h, delta=0):
    prof = track_profile(w)
    cv = Canvas(w, h)
    for x, v in enumerate(prof):
        cv.fill(rect(x, 0, 1, h), C(max(0, v - delta)))
    return cv


def gen_scroll_track(dpi):
    w = scroll_width(dpi)
    # SCRBS_NORMAL, HOT, PRESSED, DISABLED, HOVER
    return [track_canvas(w, ipx(8 * scale_of(dpi))).raster() for _ in range(5)]


def gen_scroll_thumb(dpi):
    s = scale_of(dpi)
    w = scroll_width(dpi)
    h = ipx(24 * s)
    kw = 8 * s
    kx = ipx(4 * s)
    looks = ['normal', 'hot', 'pressed', None, 'hover']

    def draw(cv, i):
        prof = track_profile(w)
        for x, v in enumerate(prof):
            cv.fill(rect(x, 0, 1, h), C(v))
        if looks[i]:
            v = KNOB[looks[i]]
            cv.fill(rrect(kx, s, kw, h - 2 * s, kw / 2.0),
                    stops((0, C(v - 2)), (1, C(v + 2))), x0=kx, x1=kx + kw)
    return frames_of(w, h, 5, draw)


def gen_scroll_arrow_bg():
    w = h = 15
    frames = []
    # ABS_UP*, ABS_DOWN* (vertical), ABS_LEFT*, ABS_RIGHT* (horizontal), then the four *HOVER states.
    for i in range(20):
        horizontal = (8 <= i < 16) or i >= 18
        state = i % 4 if i < 16 else 4
        delta = {0: 0, 1: 3, 2: 8, 3: 0, 4: 0}[state]
        r = track_canvas(w, h, delta).raster()
        frames.append(r.transposed() if horizontal else r)
    return frames


def gen_scroll_arrow_glyph(dpi):
    s = scale_of(dpi)
    w = h = scroll_width(dpi)
    colours = {0: C(138), 1: C(98), 2: C(64), 3: C(212), 4: C(124)}
    out = []
    for i in range(20):
        if i < 16:
            direction = ('up', 'down', 'left', 'right')[i // 4]
            state = i % 4
        else:
            direction = ('up', 'down', 'left', 'right')[i - 16]
            state = 4
        cv = Canvas(w, h)
        tw, th = 7 * s, 4 * s
        axis = 1 + (w - 1) / 2.0
        if direction in ('up', 'down'):
            x0, y0 = axis - tw / 2.0, (h - th) / 2.0
            if direction == 'up':
                pts = [(x0, y0 + th), (x0 + tw, y0 + th), (axis, y0)]
            else:
                pts = [(x0, y0), (x0 + tw, y0), (axis, y0 + th)]
        else:
            y0, x0 = axis - tw / 2.0, (w - th) / 2.0
            if direction == 'left':
                pts = [(x0 + th, y0), (x0 + th, y0 + tw), (x0, axis)]
            else:
                pts = [(x0, y0), (x0, y0 + tw), (x0 + th, axis)]
        cv.fill(poly(pts), colours[state])
        out.append(cv.raster())
    return out


# --- tabs --------------------------------------------------------------------------

TAB_BORDER = C(150)
TAB_SELECTED = stops((0, C(158)), (0.12, C(176)), (0.5, C(190)), (1, C(200)))


def gen_tab(selected, left, right):
    """A tab segment.

    comctl32 draws an unselected tab into its rectangle extended 3 pixels down
    into the pane, and the selected one into a rectangle 2 pixels larger at the
    top and both sides, extended 2 pixels into the pane. Both images put the
    segment at the same place: from the top of the unselected rectangle to the
    top edge of the pane. Every segment draws its right edge; the first one
    also draws its left edge.
    """
    m = 2 if selected else 0
    w, h = 16 + 2 * m, 24
    r = 4.0
    corners = (('tl', 'bl') if left else ()) + (('tr', 'br') if right else ())
    top, bottom = m, h - (2 if selected else 3)
    x0, x1 = m, w - m
    fill = TAB_SELECTED if selected else LIGHT

    def draw(cv, i):
        lx = x0 if left else x0 - 1
        path = rrect(lx + 0.5, top + 0.5, (x1 - lx) - 1, (bottom - top) - 1, r - 0.5, corners)
        cv.save()
        cv.clip(rect(x0, top, x1 - x0, bottom - top))
        cv.fill(path, fill, y0=top + 1, y1=bottom - 1)
        if selected:
            cv.fill(rect(x0, top + 1, x1 - x0, 1), C(0, 0.10))
        cv.stroke(path, 1, stops((0, C(154)), (0.15, TAB_BORDER), (1, C(140))), y0=top, y1=bottom)
        cv.restore()
    return frames_of(w, h, 5, draw)


# The pane keeps the dialog background colour: controls placed directly on a
# tab control paint their backgrounds with the dialog colour.
PANE_FILL = 236


def gen_tab_pane():
    w = h = 12
    cv = Canvas(w, h)
    cv.fill(rrect(0, 0, w, h, 4.5, ('br', 'bl')), C(PANE_FILL))
    outer = rrect(0, 0, w, h, 4.5, ('br', 'bl'))
    ring = Q.CGPathCreateMutableCopy(outer)
    Q.CGPathAddPath(ring, None, rrect(1, 1, w - 2, h - 2, 3.5, ('br', 'bl')))
    cv.fill(ring, stops((0, C(0, 0.22)), (0.3, C(0, 0.16)), (1, C(0, 0.13))), y0=0, y1=h, eo=True)
    ring2 = Q.CGPathCreateMutableCopy(rrect(1, 1, w - 2, h - 2, 3.5, ('br', 'bl')))
    Q.CGPathAddPath(ring2, None, rrect(2, 2, w - 4, h - 4, 2.5, ('br', 'bl')))
    cv.fill(ring2, stops((0, C(0, 0.06)), (1, C(0, 0.03))), y0=1, y1=h - 1, eo=True)
    return [cv.raster()]


def gen_tab_body():
    cv = Canvas(8, 8)
    cv.fill(rect(0, 0, 8, 8), C(PANE_FILL))
    return [cv.raster()]


# --- tree view disclosure triangles --------------------------------------------------

def gen_tree_glyph(dpi):
    s = scale_of(dpi)
    w = h = ipx(11 * s)

    # GLPS_CLOSED, GLPS_OPENED
    def draw(cv, i):
        c = (w / 2.0, h / 2.0)
        if i == 0:
            tw, th = 6.4 * s, 8.0 * s
            pts = [(c[0] - tw / 2.0 + 0.4 * s, c[1] - th / 2.0), (c[0] - tw / 2.0 + 0.4 * s, c[1] + th / 2.0),
                   (c[0] + tw / 2.0 + 0.4 * s, c[1])]
        else:
            tw, th = 8.0 * s, 6.4 * s
            pts = [(c[0] - tw / 2.0, c[1] - th / 2.0 + 0.4 * s), (c[0] + tw / 2.0, c[1] - th / 2.0 + 0.4 * s),
                   (c[0], c[1] + th / 2.0 + 0.4 * s)]
        cv.fill(poly(pts), C(128))
    return frames_of(w, h, 2, draw)


# --- toolbar, rebar, status bar ---------------------------------------------------------

TEXTURED_FILL = stops((0, C(255, 0.75)), (1, C(255, 0.35)))
TEXTURED_BORDER = C(0, 0.40)
TEXTURED_PRESSED = stops((0, C(0, 0.24)), (0.15, C(0, 0.15)), (1, C(0, 0.06)))


def gen_toolbar_button():
    w, h = 16, 22
    r = 4.0

    # TS_NORMAL, HOT, PRESSED, DISABLED, CHECKED, HOTCHECKED, NEARHOT, OTHERSIDEHOT
    def draw(cv, i):
        if i in (0, 3):
            return
        if i in (1, 6, 7):
            cv.alpha(1.0 if i == 1 else 0.6)
            bezel(cv, 0, 0, w, h - 1, r, TEXTURED_FILL, TEXTURED_BORDER, shadow=C(255, 0.35))
        elif i == 2:
            bezel(cv, 0, 0, w, h - 1, r, TEXTURED_PRESSED, C(0, 0.45), shadow=C(255, 0.35))
        else:
            bezel(cv, 0, 0, w, h - 1, r, fade(TEXTURED_PRESSED, 0.75 if i == 4 else 0.95),
                  C(0, 0.42), shadow=C(255, 0.35))
    return frames_of(w, h, 8, draw)


def gen_empty(w, h, n):
    return [Raster(w, h) for _ in range(n)]


def gen_toolbar_separator(vertical_line):
    if vertical_line:
        w, h = 8, 16
        cv = Canvas(w, h)
        cv.fill(rect(3, 3, 1, h - 6), C(0, 0.18))
    else:
        w, h = 16, 8
        cv = Canvas(w, h)
        cv.fill(rect(3, 3, w - 6, 1), C(0, 0.18))
    return [cv.raster()]


def gen_small_arrow_glyph(dpi, n, colours, shape='down'):
    s = scale_of(dpi)
    w, h = ipx(9 * s), ipx(7 * s)

    def draw(cv, i):
        colour = colours[i]
        if shape == 'down':
            tw, th = 5.4 * s, 3.4 * s
            x0, y0 = (w - tw) / 2.0, (h - th) / 2.0
            cv.fill(poly([(x0, y0), (x0 + tw, y0), (x0 + tw / 2.0, y0 + th)]), colour)
        else:
            # A double chevron pointing right.
            for k in (0, 1):
                x0 = 1.0 * s + k * 3.4 * s
                p = poly([(x0, 1.0 * s), (x0 + 2.6 * s, h / 2.0), (x0, h - 1.0 * s)], close=False)
                cv.stroke(p, 1.2 * s, colour)
    return frames_of(w, h, n, draw)


def gen_rebar():
    w, h = 4, 24
    cv = Canvas(w, h)
    cv.fill(rect(0, 0, w, h - 1), stops((0, C(238)), (1, C(226))))
    cv.fill(rect(0, h - 1, w, 1), C(186))
    return [cv.raster()]


def gen_chevron_bg():
    w, h = 14, 22
    r = 4.0

    def draw(cv, i):
        if i == 1:
            bezel(cv, 0, 0, w, h - 1, r, TEXTURED_FILL, TEXTURED_BORDER, shadow=C(255, 0.35))
        elif i == 2:
            bezel(cv, 0, 0, w, h - 1, r, TEXTURED_PRESSED, C(0, 0.45), shadow=C(255, 0.35))
    return frames_of(w, h, 3, draw)


def gen_status():
    w, h = 4, 22
    cv = Canvas(w, h)
    cv.fill(rect(0, 1, w, h - 1), stops((0, C(240)), (1, C(226))))
    cv.fill(rect(0, 0, w, 1), C(190))
    return [cv.raster()]


def gen_status_pane():
    w, h = 6, 22
    cv = Canvas(w, h)
    cv.fill(rect(w - 1, 5, 1, h - 9), C(0, 0.16))
    return [cv.raster()]


# ---------------------------------------------------------------------------
# Theme ini
# ---------------------------------------------------------------------------

class Ini(object):
    def __init__(self):
        self.lines = []

    def section(self, name, *props, **kw):
        if kw.get('comment'):
            self.lines.append('')
            self.lines.append('; ' + kw['comment'])
        self.lines.append('[%s]' % name)
        for p in props:
            if p is None:
                continue
            if isinstance(p, (list, tuple)):
                self.lines.extend(p)
            else:
                self.lines.append(p)
        self.lines.append('')


def dpi_files(base):
    lines = []
    for i, dpi in enumerate(DPIS):
        lines.append('ImageFile%d = %s' % (i + 1, dpi_name(base, dpi)))
    for i, dpi in enumerate(DPIS):
        lines.append('MinDpi%d = %d' % (i + 1, dpi))
    return lines


def rgbstr(c):
    return '%d %d %d' % c


def build_ini(color_name):
    a = ACCENTS[color_name]
    px = a['prefix']
    ini = Ini()

    ini.section('Globals',
                'EdgeLightColor = 255 255 255',
                'EdgeHighLightColor = 255 255 255',
                'EdgeShadowColor = 190 190 190',
                'EdgeDkShadowColor = 150 150 150',
                'EdgeFillColor = 236 236 236')

    ini.section('SysMetrics',
                'ScrollbarWidth = 15',
                'ScrollbarHeight = 15',
                '',
                '; System colours',
                'Scrollbar = 245 245 245',
                'Background = 61 106 154',
                'ActiveCaption = 220 220 220',
                'InactiveCaption = 240 240 240',
                'Menu = 255 255 255',
                'Window = 255 255 255',
                'WindowFrame = 160 160 160',
                'MenuText = 0 0 0',
                'WindowText = 0 0 0',
                'CaptionText = 38 38 38',
                'ActiveBorder = 236 236 236',
                'InactiveBorder = 236 236 236',
                'AppWorkSpace = 178 178 178',
                'Highlight = ' + rgbstr(a['highlight']),
                'HighlightText = 255 255 255',
                'BtnFace = 236 236 236',
                'BtnShadow = 170 170 170',
                'GrayText = 152 152 152',
                'BtnText = 0 0 0',
                'InactiveCaptionText = 158 158 158',
                'BtnHighlight = 255 255 255',
                'DkShadow3d = 140 140 140',
                'Light3d = 245 245 245',
                'InfoText = 0 0 0',
                'InfoBk = 255 255 199',
                'ButtonAlternateFace = 190 190 190',
                'HotTracking = ' + rgbstr(a['hot_tracking']),
                'GradientActiveCaption = 220 220 220',
                'GradientInactiveCaption = 240 240 240',
                'MenuHilight = ' + rgbstr(a['menu_highlight']),
                'MenuBar = 236 236 236',
                '',
                '; Fonts: sizes are in points, 10 points being 13 pixels at 96 DPI',
                'CaptionFont = Lucida Grande, 10',
                'SmallCaptionFont = Lucida Grande, 8',
                'MenuFont = Lucida Grande, 10',
                'StatusFont = Lucida Grande, 9',
                'MsgBoxFont = Lucida Grande, 9',
                'IconTitleFont = Lucida Grande, 9',
                '',
                'FlatMenus = true')

    # Button
    ini.section('Button.Pushbutton',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = %d, %d, %d, %d' % ((BUTTON_MARGIN,) * 4),
                'ContentMargins = 4, 4, 3, 4',
                'ImageCount = 6',
                'ImageLayout = Vertical',
                'ImageSelectType = Dpi',
                dpi_files(px + '_button'),
                'Transparent = True',
                'TextColor = 0 0 0',
                'MinSize = 10, 5',
                comment='Button')
    ini.section('Button.Pushbutton(Disabled)', 'TextColor = 160 160 160')
    ini.section('Button.Checkbox',
                'BgType = ImageFile',
                'SizingType = TrueSize',
                'SizingMargins = 0, 0, 0, 0',
                'ContentMargins = 0, 0, 0, 0',
                'ImageCount = 20',
                'ImageLayout = Vertical',
                'ImageSelectType = Dpi',
                'TrueSizeScalingType = Dpi',
                'UniformSizing = True',
                'MirrorImage = False',
                'Transparent = True',
                dpi_files('aqua_checkbox'))
    for st in ('Uncheckeddisabled', 'Checkeddisabled', 'Mixeddisabled', 'ImplicitDisabled', 'ExcludedDisabled'):
        ini.section('Button.Checkbox(%s)' % st, 'TextColor = 160 160 160')
    ini.section('Button.Radiobutton',
                'BgType = ImageFile',
                'SizingType = TrueSize',
                'SizingMargins = 0, 0, 0, 0',
                'ContentMargins = 0, 0, 0, 0',
                'ImageCount = 8',
                'ImageLayout = Vertical',
                'ImageSelectType = Dpi',
                'TrueSizeScalingType = Dpi',
                'UniformSizing = True',
                'Transparent = True',
                dpi_files('aqua_radiobutton'))
    for st in ('Uncheckeddisabled', 'Checkeddisabled'):
        ini.section('Button.Radiobutton(%s)' % st, 'TextColor = 160 160 160')
    ini.section('Button.Groupbox',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 5, 5, 5, 5',
                'ImageFile = aqua_groupbox.bmp',
                'BorderOnly = True',
                'Transparent = True',
                'TextColor = 0 0 0')
    ini.section('Button.Groupbox(Disabled)', 'TextColor = 160 160 160')

    # ComboBox: comctl32 draws the frame with the class part and the button inside it.
    ini.section('ComboBox',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 2, 2, 2, 2',
                'ContentMargins = 2, 2, 2, 2',
                'ImageFile = aqua_edit.bmp',
                'ImageCount = 6',
                'ImageLayout = Vertical',
                comment='ComboBox')
    ini.section('ComboBox.DropDownButton',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 2, 2, 3, 3',
                'ContentMargins = 0, 0, 0, 0',
                'ImageFile = aqua_combobox_button.bmp',
                'ImageCount = 4',
                'ImageLayout = Vertical',
                'GlyphType = ImageGlyph',
                'GlyphTransparent = True',
                'ImageSelectType = Dpi',
                dpi_files('aqua_combobox_glyph'))
    ini.section('ComboBox.CueBanner', 'TextColor = 160 160 160')

    # Edit
    ini.section('Edit',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 2, 2, 2, 2',
                'ContentMargins = 2, 2, 2, 2',
                'ImageFile = aqua_edit.bmp',
                'ImageCount = 6',
                'ImageLayout = Vertical',
                comment='Edit')
    ini.section('Edit.EditText(Disabled)', 'TextColor = 160 160 160')
    ini.section('Edit.EditText(CueBanner)', 'TextColor = 160 160 160')

    # Header
    ini.section('Header', 'BgType = None', comment='Header')
    ini.section('Header.HeaderItem',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 1, 2, 3, 3',
                'ContentMargins = 0, 0, 0, 0',
                'ImageFile = aqua_header_item.bmp',
                'ImageCount = 12',
                'ImageLayout = Vertical',
                'TextColor = 0 0 0')

    # Scrolled lists: the frame is the window border of the control.
    for cls in ('ListBox', 'ListView', 'TreeView'):
        ini.section(cls,
                    'BgType = ImageFile',
                    'SizingType = Stretch',
                    'SizingMargins = 2, 2, 2, 2',
                    'ImageFile = aqua_listbox.bmp',
                    comment=cls)
    ini.section('TreeView.Glyph',
                'BgType = ImageFile',
                'SizingType = TrueSize',
                'ImageCount = 2',
                'ImageLayout = Vertical',
                'ImageSelectType = Dpi',
                'TrueSizeScalingType = Dpi',
                'Transparent = True',
                dpi_files('aqua_treeview_glyph'))

    # Progress
    ini.section('Progress',
                'BgType = None',
                'ProgressChunkSize = 1',
                'ProgressSpaceSize = 0',
                comment='Progress')
    ini.section('Progress.Bar',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 4, 4, 5, 5',
                'ContentMargins = 1, 1, 1, 2',
                'ImageFile = aqua_progress_track.bmp',
                'Transparent = True',
                'MinSize = 10, 10')
    ini.section('Progress.BarVert',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 5, 5, 4, 4',
                'ContentMargins = 1, 2, 1, 1',
                'ImageFile = aqua_progress_track_vert.bmp',
                'Transparent = True')
    ini.section('Progress.Fill',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 3, 1, 4, 4',
                'ImageFile = %s_progress_fill.bmp' % px,
                'ImageCount = 4',
                'ImageLayout = Vertical')
    ini.section('Progress.FillVert',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 4, 4, 1, 3',
                'ImageFile = %s_progress_fill_vert.bmp' % px,
                'ImageCount = 4',
                'ImageLayout = Vertical')

    # Rebar
    ini.section('Rebar',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 0, 0, 2, 2',
                'ImageFile = aqua_rebar.bmp',
                'TextColor = 0 0 0',
                comment='Rebar')
    ini.section('Rebar.Band', 'BgType = None', 'ContentMargins = 2, 0, 0, 0')
    ini.section('Rebar.Gripper', 'BgType = None')
    ini.section('Rebar.GripperVert', 'BgType = None')
    ini.section('Rebar.Chevron',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 5, 5, 5, 5',
                'ContentMargins = 0, 0, 0, 0',
                'ImageFile = aqua_rebar_chevron.bmp',
                'ImageCount = 3',
                'ImageLayout = Vertical',
                'Transparent = True',
                'GlyphType = ImageGlyph',
                'GlyphTransparent = True',
                'ImageSelectType = Dpi',
                dpi_files('aqua_rebar_chevron_glyph'))
    ini.section('Rebar.ChevronVert', 'BgType = None')
    ini.section('Rebar.Splitter', 'BgType = None')
    ini.section('Rebar.SplitterVert', 'BgType = None')

    # Scroll bar: arrows, thumb and track are opaque, since comctl32 and user32
    # draw them over whatever the bar showed before.
    ini.section('ScrollBar', 'BgType = None', comment='ScrollBar')
    ini.section('ScrollBar.ArrowBtn',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 1, 1, 1, 1',
                'ContentMargins = 0, 0, 0, 0',
                'ImageFile = aqua_scrollbar_arrow.bmp',
                'ImageCount = 20',
                'ImageLayout = Vertical',
                'GlyphType = ImageGlyph',
                'GlyphTransparent = True',
                'ImageSelectType = Dpi',
                dpi_files('aqua_scrollbar_arrow_glyph'))
    ini.section('ScrollBar.ThumbBtnVert',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 0, 0, 10, 10',
                'ContentMargins = 0, 0, 4, 4',
                'ImageCount = 5',
                'ImageLayout = Vertical',
                'ImageSelectType = Dpi',
                dpi_files('aqua_scrollbar_thumb_vert'))
    ini.section('ScrollBar.ThumbBtnHorz',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 10, 10, 0, 0',
                'ContentMargins = 4, 4, 0, 0',
                'ImageCount = 5',
                'ImageLayout = Vertical',
                'ImageSelectType = Dpi',
                dpi_files('aqua_scrollbar_thumb_horz'))
    for part, orient in (('LowerTrackVert', 'vert'), ('UpperTrackVert', 'vert'),
                         ('LowerTrackHorz', 'horz'), ('UpperTrackHorz', 'horz')):
        ini.section('ScrollBar.' + part,
                    'BgType = ImageFile',
                    'SizingType = Stretch',
                    'SizingMargins = 0, 0, 0, 0',
                    'ImageCount = 5',
                    'ImageLayout = Vertical',
                    'ImageSelectType = Dpi',
                    dpi_files('aqua_scrollbar_track_' + orient))
    ini.section('ScrollBar.GripperVert', 'BgType = None')
    ini.section('ScrollBar.GripperHorz', 'BgType = None')
    ini.section('ScrollBar.SizeBox', 'BgType = None')

    # Spin
    ini.section('Spin', 'BgType = None', comment='Spin')
    for part, img, glyph in (('Up', 'aqua_spin_up', 'aqua_spin_glyph_up'),
                             ('Down', 'aqua_spin_down', 'aqua_spin_glyph_down'),
                             ('UpHorz', 'aqua_spin_right', 'aqua_spin_glyph_right'),
                             ('DownHorz', 'aqua_spin_left', 'aqua_spin_glyph_left')):
        ini.section('Spin.' + part,
                    'BgType = ImageFile',
                    'SizingType = Stretch',
                    'SizingMargins = 5, 5, 5, 5',
                    'ContentMargins = 0, 0, 0, 0',
                    'ImageFile = %s.bmp' % img,
                    'ImageCount = 4',
                    'ImageLayout = Vertical',
                    'Transparent = True',
                    'GlyphType = ImageGlyph',
                    'GlyphTransparent = True',
                    'ImageSelectType = Dpi',
                    dpi_files(glyph))

    # Status
    ini.section('Status',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 0, 0, 2, 1',
                'ImageFile = aqua_status.bmp',
                comment='Status')
    ini.section('Status.Pane',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 0, 1, 5, 4',
                'ImageFile = aqua_status_pane.bmp',
                'Transparent = True')
    ini.section('Status.GripperPane', 'BgType = None')
    ini.section('Status.Gripper', 'BgType = None')

    # TaskDialog: a Mac alert is one plain window background, with no white
    # message panel above the buttons. Qt draws its message boxes and input
    # dialogs with these two panels.
    for part in ('PrimaryPanel', 'SecondaryPanel'):
        ini.section('TaskDialog.' + part,
                    'BgType = BorderFill',
                    'BorderSize = 0',
                    'FillColor = 236 236 236',
                    comment='TaskDialog' if part == 'PrimaryPanel' else None)

    # Tab: segments sit on the top edge of the pane; comctl32 draws the selected
    # one 2 pixels larger at the top and the sides, and every tab 3 pixels into the pane.
    ini.section('Tab', 'BgType = None', comment='Tab')
    ini.section('Tab.Pane',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 5, 5, 5, 5',
                'ImageFile = aqua_tab_pane.bmp',
                'Transparent = True')
    ini.section('Tab.Body',
                'BgType = ImageFile',
                'SizingType = Tile',
                'ImageFile = aqua_tab_body.bmp')
    for part, img, sel in (('TabItem', 'aqua_tab_item', False),
                           ('TabItemLeftEdge', 'aqua_tab_item_left', False),
                           ('TabItemRightEdge', 'aqua_tab_item_right', False),
                           ('TabItemBothEdge', 'aqua_tab_item_both', False),
                           ('TopTabItem', 'aqua_tab_selected', True),
                           ('TopTabItemLeftEdge', 'aqua_tab_selected_left', True),
                           ('TopTabItemRightEdge', 'aqua_tab_selected_right', True),
                           ('TopTabItemBothEdge', 'aqua_tab_selected_both', True)):
        ini.section('Tab.' + part,
                    'BgType = ImageFile',
                    'SizingType = Stretch',
                    'SizingMargins = 6, 6, 6, 6',
                    'ContentMargins = %s' % ('2, 2, 2, 2' if sel else '0, 0, 0, 3'),
                    'ImageFile = %s.bmp' % img,
                    'ImageCount = 5',
                    'ImageLayout = Vertical',
                    'Transparent = True')

    # Toolbar
    ini.section('Toolbar', 'BgType = None', 'TextColor = 0 0 0', comment='Toolbar')
    for part in ('Button', 'DropDownButton', 'SplitButton'):
        ini.section('Toolbar.' + part,
                    'BgType = ImageFile',
                    'SizingType = Stretch',
                    'SizingMargins = 5, 5, 5, 5',
                    'ContentMargins = 0, 0, 0, 0',
                    'ImageFile = aqua_toolbar_button.bmp',
                    'ImageCount = 8',
                    'ImageLayout = Vertical',
                    'Transparent = True')
    ini.section('Toolbar.SplitButtonDropDown',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 0, 0, 0, 0',
                'ContentMargins = 0, 0, 1, 0',
                'ImageFile = aqua_empty8.bmp',
                'ImageCount = 8',
                'ImageLayout = Vertical',
                'Transparent = True',
                'GlyphType = ImageGlyph',
                'GlyphTransparent = True',
                'ImageSelectType = Dpi',
                dpi_files('aqua_toolbar_dropdown_glyph'))
    ini.section('Toolbar.Separator',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 4, 3, 4, 4',
                'ImageFile = aqua_toolbar_separator.bmp',
                'Transparent = True')
    ini.section('Toolbar.SeparatorVert',
                'BgType = ImageFile',
                'SizingType = Stretch',
                'SizingMargins = 4, 4, 4, 3',
                'ImageFile = aqua_toolbar_separator_vert.bmp',
                'Transparent = True')

    # TrackBar
    ini.section('TrackBar', 'BgType = None', comment='TrackBar')
    for part in ('Track', 'TrackVert'):
        ini.section('TrackBar.' + part,
                    'BgType = ImageFile',
                    'SizingType = Stretch',
                    'SizingMargins = %s' % ('3, 3, 1, 1' if part == 'Track' else '1, 1, 3, 3'),
                    'ImageFile = %s.bmp' % ('aqua_trackbar_track' if part == 'Track' else 'aqua_trackbar_track_vert'),
                    'Transparent = True')
    for part, base in (('Thumb', 'aqua_trackbar_thumb'), ('ThumbVert', 'aqua_trackbar_thumb_vert'),
                       ('ThumbBottom', 'aqua_trackbar_thumb_down'), ('ThumbTop', 'aqua_trackbar_thumb_up'),
                       ('ThumbRight', 'aqua_trackbar_thumb_right'), ('ThumbLeft', 'aqua_trackbar_thumb_left')):
        ini.section('TrackBar.' + part,
                    'BgType = ImageFile',
                    'SizingType = TrueSize',
                    'ImageCount = 5',
                    'ImageLayout = Vertical',
                    'ImageSelectType = Dpi',
                    'TrueSizeScalingType = Dpi',
                    'Transparent = True',
                    dpi_files(base))
    ini.section('TrackBar.Tics', 'Color = 146 146 146')
    ini.section('TrackBar.TicsVert', 'Color = 146 146 146')

    return '\r\n'.join(ini.lines) + '\r\n'


# ---------------------------------------------------------------------------
# Generation
# ---------------------------------------------------------------------------

def generate_bitmaps():
    for name, acc in sorted(ACCENTS.items()):
        for dpi in DPIS:
            emit(dpi_name(acc['prefix'] + '_button', dpi), gen_button(acc, dpi))
        fill = gen_progress_fill(acc)
        emit(acc['prefix'] + '_progress_fill.bmp', fill)
        # Vertical bars fill upwards from the bottom.
        emit(acc['prefix'] + '_progress_fill_vert.bmp', [f.transposed().flipped() for f in fill])

    for dpi in DPIS:
        emit(dpi_name('aqua_checkbox', dpi), gen_checkbox(dpi))
        emit(dpi_name('aqua_radiobutton', dpi), gen_radio(dpi))
        emit(dpi_name('aqua_combobox_glyph', dpi), gen_combo_glyph(dpi))
        emit(dpi_name('aqua_treeview_glyph', dpi), gen_tree_glyph(dpi))
        emit(dpi_name('aqua_scrollbar_arrow_glyph', dpi), gen_scroll_arrow_glyph(dpi))
        thumb = gen_scroll_thumb(dpi)
        emit(dpi_name('aqua_scrollbar_thumb_vert', dpi), thumb)
        emit(dpi_name('aqua_scrollbar_thumb_horz', dpi), [f.transposed() for f in thumb])
        track = gen_scroll_track(dpi)
        emit(dpi_name('aqua_scrollbar_track_vert', dpi), track)
        emit(dpi_name('aqua_scrollbar_track_horz', dpi), [f.transposed() for f in track])
        up, down = gen_spin_glyph(dpi, 'up'), gen_spin_glyph(dpi, 'down')
        emit(dpi_name('aqua_spin_glyph_up', dpi), up)
        emit(dpi_name('aqua_spin_glyph_down', dpi), down)
        emit(dpi_name('aqua_spin_glyph_left', dpi), [f.transposed() for f in up])
        emit(dpi_name('aqua_spin_glyph_right', dpi), [f.transposed() for f in down])
        down, up = knob_frames(dpi, 'down'), knob_frames(dpi, 'up')
        emit(dpi_name('aqua_trackbar_thumb_down', dpi), down)
        emit(dpi_name('aqua_trackbar_thumb_up', dpi), up)
        emit(dpi_name('aqua_trackbar_thumb_right', dpi), [f.transposed() for f in down])
        emit(dpi_name('aqua_trackbar_thumb_left', dpi), [f.transposed() for f in up])
        pill = knob_frames(dpi, 'pill')
        emit(dpi_name('aqua_trackbar_thumb', dpi), pill)
        emit(dpi_name('aqua_trackbar_thumb_vert', dpi), [f.transposed() for f in pill])
        grey = [C(72)] * 8
        grey[3] = C(180)
        emit(dpi_name('aqua_toolbar_dropdown_glyph', dpi), gen_small_arrow_glyph(dpi, 8, grey))
        emit(dpi_name('aqua_rebar_chevron_glyph', dpi), gen_small_arrow_glyph(dpi, 3, [C(72)] * 3, 'chevron'))

    emit('aqua_groupbox.bmp', gen_groupbox())
    emit('aqua_edit.bmp', gen_edit())
    emit('aqua_listbox.bmp', gen_listbox())
    emit('aqua_combobox_button.bmp', gen_combo_button())
    emit('aqua_header_item.bmp', gen_header_item())
    track = gen_progress_track()
    emit('aqua_progress_track.bmp', track)
    emit('aqua_progress_track_vert.bmp', [f.transposed() for f in track])
    tt = gen_trackbar_track()
    emit('aqua_trackbar_track.bmp', tt)
    emit('aqua_trackbar_track_vert.bmp', [f.transposed() for f in tt])
    up, down = gen_spin('up'), gen_spin('down')
    emit('aqua_spin_up.bmp', up)
    emit('aqua_spin_down.bmp', down)
    emit('aqua_spin_left.bmp', [f.transposed() for f in up])
    emit('aqua_spin_right.bmp', [f.transposed() for f in down])
    emit('aqua_scrollbar_arrow.bmp', gen_scroll_arrow_bg())
    for sel in (False, True):
        base = 'aqua_tab_selected' if sel else 'aqua_tab_item'
        emit(base + '.bmp', gen_tab(sel, False, False))
        emit(base + '_left.bmp', gen_tab(sel, True, False))
        emit(base + '_right.bmp', gen_tab(sel, False, True))
        emit(base + '_both.bmp', gen_tab(sel, True, True))
    emit('aqua_tab_pane.bmp', gen_tab_pane())
    emit('aqua_tab_body.bmp', gen_tab_body())
    emit('aqua_toolbar_button.bmp', gen_toolbar_button())
    emit('aqua_empty8.bmp', gen_empty(2, 2, 8))
    emit('aqua_toolbar_separator.bmp', gen_toolbar_separator(True))
    emit('aqua_toolbar_separator_vert.bmp', gen_toolbar_separator(False))
    emit('aqua_rebar.bmp', gen_rebar())
    emit('aqua_rebar_chevron.bmp', gen_chevron_bg())
    emit('aqua_status.bmp', gen_status())
    emit('aqua_status_pane.bmp', gen_status_pane())


RC_HEADER = '''/*
 * Aqua visual style
 *
 * WINESKIN: an OS X 10.9 (Mavericks) look for themed controls, with the
 * Blue and Graphite colour variants of that system. This file and the
 * bitmaps are written by generate.py; edit that script and run it again.
 *
 * Copyright 2026 WFH Wineskin contributors
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

#include <winnt.h>

#define IDS_COLOR_DISPLAY_NAME_BLUE      1000
#define IDS_COLOR_DISPLAY_NAME_GRAPHITE  1001
#define IDS_COLOR_TOOLTIP_BLUE           2000
#define IDS_COLOR_TOOLTIP_GRAPHITE       2001
#define IDS_SIZE_DISPLAY_NAME_NORMAL     3000
#define IDS_SIZE_TOOLTIP_NORMAL          4000

LANGUAGE LANG_ENGLISH, SUBLANG_DEFAULT

/* Theme color display names */
STRINGTABLE
{
    IDS_COLOR_DISPLAY_NAME_BLUE     "Blue"
    IDS_COLOR_DISPLAY_NAME_GRAPHITE "Graphite"
}

/* Theme color tooltips */
STRINGTABLE
{
    IDS_COLOR_TOOLTIP_BLUE     "Blue"
    IDS_COLOR_TOOLTIP_GRAPHITE "Graphite"
}

/* Theme size display names */
STRINGTABLE
{
    IDS_SIZE_DISPLAY_NAME_NORMAL "Normal"
}

/* Theme size tooltips */
STRINGTABLE
{
    IDS_SIZE_TOOLTIP_NORMAL "Normal"
}

LANGUAGE LANG_NEUTRAL, SUBLANG_NEUTRAL

/* Theme version */
1 PACKTHEM_VERSION {0x3}

/* Theme color names */
1 COLORNAMES
{
"Blue\\0"
"Graphite\\0"
"\\0"
}

/* Theme size names */
1 SIZENAMES
{
"NormalSize\\0"
"\\0"
}

/* Theme ini files, one per color and size */
1 FILERESNAMES
{
"BLUE_INI\\0"
"GRAPHITE_INI\\0"
"\\0"
}

/* Theme definition */
THEMES_INI TEXTFILE
{
"[documentation]\\r\\n"
"DisplayName = Aqua\\r\\n"
"ToolTip = Aqua Visual Style\\r\\n"
}
'''


def rc_string(text):
    out = []
    for line in text.split('\r\n')[:-1]:
        esc = line.replace('\\', '\\\\').replace('"', '\\"')
        out.append('"%s\\r\\n"' % esc)
    return '\n'.join(out)


def write_rc():
    parts = [RC_HEADER]
    for name in ('Blue', 'Graphite'):
        parts.append('\n/* %s */\n%s_INI TEXTFILE\n{\n%s\n}\n' % (name, name.upper(), rc_string(build_ini(name))))
    parts.append('')
    for f in sorted(BITMAPS):
        res = f.replace('.', '_').upper()
        parts.append('/* @makedep: %s */\n%s BITMAP "%s"\n' % (f, res, f))
    with open(os.path.join(HERE, 'aqua.rc'), 'w') as fp:
        fp.write('\n'.join(parts))


def referenced_bitmaps():
    names = set()
    for name in ('Blue', 'Graphite'):
        for line in build_ini(name).split('\r\n'):
            if '=' in line and line.split('=')[0].strip().lower().startswith(('imagefile', 'glyphimagefile')):
                names.add(line.split('=', 1)[1].strip())
    return names

# ---------------------------------------------------------------------------
# Preview: the bitmaps drawn the way uxtheme draws them, on a mock dialog
# ---------------------------------------------------------------------------

class Sheet(object):
    def __init__(self, w, h, bg=236):
        self.cv = Canvas(w, h)
        self.c = self.cv.ctx
        self.cv.fill(rect(0, 0, w, h), C(bg))
        self.images = {}

    def image(self, name):
        if name not in self.images:
            self.images[name] = to_cgimage(BITMAPS[name])
        return self.images[name]

    def blit(self, img, sx, sy, sw, sh, dx, dy, dw, dh):
        if sw <= 0 or sh <= 0 or dw <= 0 or dh <= 0:
            return
        sub = Q.CGImageCreateWithImageInRect(img, Q.CGRectMake(sx, sy, sw, sh))
        Q.CGContextSaveGState(self.c)
        Q.CGContextTranslateCTM(self.c, dx, dy + dh)
        Q.CGContextScaleCTM(self.c, 1, -1)
        Q.CGContextSetInterpolationQuality(self.c, Q.kCGInterpolationHigh)
        Q.CGContextDrawImage(self.c, Q.CGRectMake(0, 0, dw, dh), sub)
        Q.CGContextRestoreGState(self.c)

    def part(self, name, frame, frames, x, y, w, h, margins=(0, 0, 0, 0), truesize=False, borderonly=False):
        """Draw like UXTHEME_DrawImageBackground: nine-slice stretch or true size, centred."""
        img = self.image(name)
        r = BITMAPS[name]
        fw, fh = r.w, r.h // frames
        oy = frame * fh
        if truesize:
            self.blit(img, 0, oy, fw, fh, x + (w - fw) // 2, y + (h - fh) // 2, fw, fh)
            return
        l, rr, t, b = margins
        if t + b > h:
            t = t * h // fh
            b = h - t
        if l + rr > w:
            l = l * w // fw
            rr = w - l
        cw, ch = fw - l - rr, fh - t - b
        dcw, dch = w - l - rr, h - t - b
        self.blit(img, 0, oy, l, t, x, y, l, t)
        self.blit(img, fw - rr, oy, rr, t, x + w - rr, y, rr, t)
        self.blit(img, 0, oy + fh - b, l, b, x, y + h - b, l, b)
        self.blit(img, fw - rr, oy + fh - b, rr, b, x + w - rr, y + h - b, rr, b)
        self.blit(img, l, oy, cw, t, x + l, y, dcw, t)
        self.blit(img, l, oy + fh - b, cw, b, x + l, y + h - b, dcw, b)
        self.blit(img, 0, oy + t, l, ch, x, y + t, l, dch)
        self.blit(img, fw - rr, oy + t, rr, ch, x + w - rr, y + t, rr, dch)
        if not borderonly:
            self.blit(img, l, oy + t, cw, ch, x + l, y + t, dcw, dch)

    def text(self, s, x, y, size=12, colour=(0, 0, 0, 1), bold=False, centre_w=None):
        import CoreText as CT
        font = CT.CTFontCreateWithName('LucidaGrande-Bold' if bold else 'LucidaGrande', size, None)
        col = Q.CGColorCreate(SRGB, colour)
        attrs = {CT.kCTFontAttributeName: font, CT.kCTForegroundColorAttributeName: col}
        astr = Q.CFAttributedStringCreate(None, s, attrs)
        line = CT.CTLineCreateWithAttributedString(astr)
        width = CT.CTLineGetTypographicBounds(line, None, None, None)[0]
        if centre_w is not None:
            x = x + (centre_w - width) / 2.0
        Q.CGContextSaveGState(self.c)
        Q.CGContextSetTextMatrix(self.c, Q.CGAffineTransformMakeScale(1, -1))
        Q.CGContextSetTextPosition(self.c, x, y)
        CT.CTLineDraw(line, self.c)
        Q.CGContextRestoreGState(self.c)

    def solid(self, x, y, w, h, v):
        self.cv.fill(rect(x, y, w, h), C(*v) if isinstance(v, tuple) else C(v))


def preview(outdir):
    if not os.path.isdir(outdir):
        os.makedirs(outdir)
    for f, r in BITMAPS.items():
        write_png(os.path.join(outdir, f.replace('.bmp', '.png')), to_cgimage(r))

    S = Sheet(760, 600)
    BM = (BUTTON_MARGIN,) * 4

    def button(x, y, w, label, frame, prefix='aqua', h=23):
        S.part(dpi_name(prefix + '_button', 96), frame, 6, x, y, w, h, BM)
        S.text(label, x, y + 16, 12, C(160) if frame == 3 else C(0), centre_w=w)

    S.text('Push buttons', 16, 24, 11, C(90))
    button(16, 32, 80, 'Cancel', 0)
    button(104, 32, 80, 'OK', 4)
    button(192, 32, 80, 'Pressed', 2)
    button(280, 32, 80, 'Disabled', 3)
    button(368, 32, 80, 'Graphite', 4, 'graphite')
    button(456, 30, 100, 'Taller (27)', 0, h=27)

    # check boxes and radio buttons
    y = 72
    for k, (label, frame) in enumerate([('Unchecked', 0), ('Checked', 4), ('Mixed', 8), ('Pressed', 6), ('Disabled', 7)]):
        x = 16 + k * 104
        S.part(dpi_name('aqua_checkbox', 96), frame, 20, x, y, 16, 18, truesize=True)
        S.text(label, x + 20, y + 13, 12, C(160) if frame == 7 else C(0))
    y = 96
    for k, (label, frame) in enumerate([('Radio', 0), ('Selected', 4), ('Pressed', 6), ('Disabled', 7)]):
        x = 16 + k * 104
        S.part(dpi_name('aqua_radiobutton', 96), frame, 8, x, y, 16, 18, truesize=True)
        S.text(label, x + 20, y + 13, 12, C(160) if frame == 7 else C(0))

    # text fields and combo boxes
    y = 124
    S.part('aqua_edit.bmp', 0, 6, 16, y, 150, 21, (2, 2, 2, 2))
    S.text('Text field', 20, y + 15, 12)
    S.part('aqua_edit.bmp', 4, 6, 176, y, 150, 21, (2, 2, 2, 2))
    S.text('Focused field', 180, y + 15, 12)
    S.part('aqua_edit.bmp', 0, 6, 336, y, 150, 21, (2, 2, 2, 2))
    S.part('aqua_combobox_button.bmp', 0, 4, 336 + 150 - 2 - 15, y + 2, 15, 17, (2, 2, 3, 3))
    S.part(dpi_name('aqua_combobox_glyph', 96), 0, 4, 336 + 150 - 2 - 15, y + 2, 15, 17, truesize=True)
    S.text('Combo box', 340, y + 15, 12)
    S.part('aqua_edit.bmp', 0, 6, 496, y, 70, 21, (2, 2, 2, 2))
    S.part('aqua_edit.bmp', 0, 6, 496 + 70 - 2, y, 16 + 2, 21, (2, 2, 2, 2))
    S.text('42', 500, y + 15, 12)
    for half, frame, yy, hh in (('up', 0, y + 1, 9), ('down', 0, y + 10, 10)):
        S.part('aqua_spin_%s.bmp' % half, frame, 4, 496 + 70, yy, 15, hh, (5, 5, 5, 5))
        S.part(dpi_name('aqua_spin_glyph_%s' % half, 96), frame, 4, 496 + 70, yy, 15, hh, truesize=True)
    # standalone stepper
    for half, yy in (('up', y), ('down', y + 11)):
        S.part('aqua_spin_%s.bmp' % half, 0, 4, 600, yy, 15, 11, (5, 5, 5, 5))
        S.part(dpi_name('aqua_spin_glyph_%s' % half, 96), 0, 4, 600, yy, 15, 11, truesize=True)

    # progress and sliders
    y = 160
    S.part('aqua_progress_track.bmp', 0, 1, 16, y, 200, 18, (4, 4, 5, 5))
    S.part('aqua_progress_fill.bmp', 0, 4, 17, y + 1, 80, 15, (3, 1, 4, 4))
    S.part('aqua_progress_track.bmp', 0, 1, 16, y + 24, 200, 18, (4, 4, 5, 5))
    S.part('graphite_progress_fill.bmp', 0, 4, 17, y + 25, 140, 15, (3, 1, 4, 4))
    S.part('aqua_progress_track.bmp', 0, 1, 230, y, 120, 18, (4, 4, 5, 5))
    S.part('aqua_progress_fill.bmp', 1, 4, 231, y + 1, 60, 15, (3, 1, 4, 4))
    S.part('aqua_progress_track.bmp', 0, 1, 230, y + 24, 120, 18, (4, 4, 5, 5))
    S.part('aqua_progress_fill.bmp', 2, 4, 231, y + 25, 90, 15, (3, 1, 4, 4))
    # slider: TBS_BOTTOM with ticks
    sx, sy = 370, y - 2
    S.part('aqua_trackbar_track.bmp', 0, 1, sx + 8, sy + 9, 170, 4, (3, 3, 1, 1))
    for k in range(6):
        S.solid(sx + 8 + 5 + k * 32, sy + 25, 1, 4, 146)
    S.part(dpi_name('aqua_trackbar_thumb_down', 96), 0, 5, sx + 60, sy + 2, 11, 21, truesize=True)
    S.part('aqua_trackbar_track.bmp', 0, 1, sx + 200, sy + 9, 150, 4, (3, 3, 1, 1))
    S.part(dpi_name('aqua_trackbar_thumb', 96), 0, 5, sx + 300, sy + 2, 11, 21, truesize=True)
    S.part(dpi_name('aqua_trackbar_thumb_up', 96), 2, 5, sx + 250, sy + 2, 11, 21, truesize=True)

    # group box and tab control
    y = 216
    S.part('aqua_groupbox.bmp', 0, 1, 16, y + 6, 220, 110, (5, 5, 5, 5), borderonly=True)
    S.solid(24, y, 50, 14, 236)
    S.text('Options', 26, y + 11, 12)
    S.part(dpi_name('aqua_checkbox', 96), 4, 20, 28, y + 26, 16, 18, truesize=True)
    S.text('Show hidden files', 48, y + 39, 12)
    S.part(dpi_name('aqua_radiobutton', 96), 4, 8, 28, y + 50, 16, 18, truesize=True)
    S.text('Large icons', 48, y + 63, 12)
    S.part(dpi_name('aqua_radiobutton', 96), 0, 8, 28, y + 74, 16, 18, truesize=True)
    S.text('Small icons', 48, y + 87, 12)

    tx, ty, tw, th = 256, y, 300, 130
    H = 19
    S.part('aqua_tab_pane.bmp', 0, 1, tx, ty + H + 2, tw, th - H - 2, (5, 5, 5, 5))
    S.part('aqua_tab_body.bmp', 0, 1, tx + 4, ty + H + 6, tw - 8, th - H - 10, (0, 0, 0, 0))
    labels = ['General', 'Advanced', 'Other']
    widths = [62, 76, 52]
    lefts = []
    xx = tx + 2
    for wdt in widths:
        lefts.append(xx)
        xx += wdt
    sel = 1
    for k in range(3):
        if k == sel:
            continue
        name = 'aqua_tab_item_left.bmp' if k == 0 else 'aqua_tab_item.bmp'
        S.part(name, 0, 5, lefts[k], ty + 2, widths[k], H + 3, (6, 6, 6, 6))
    S.part('aqua_tab_pane.bmp', 0, 1, tx, ty + H + 2, tw, th - H - 2, (5, 5, 5, 5))
    S.part('aqua_tab_body.bmp', 0, 1, tx + 4, ty + H + 6, tw - 8, th - H - 10, (0, 0, 0, 0))
    S.part('aqua_tab_selected.bmp', 2, 5, lefts[sel] - 2, ty, widths[sel] + 4, H + 4, (6, 6, 6, 6))
    for k in range(3):
        S.text(labels[k], lefts[k], ty + 2 + 14, 12, centre_w=widths[k])
    S.part(dpi_name('aqua_checkbox', 96), 0, 20, tx + 16, ty + 40, 16, 18, truesize=True)
    S.text('Inside a tab page', tx + 36, ty + 53, 12)
    button(tx + 200, ty + 96, 80, 'Apply', 0)

    # list view with header and scroll bars
    y = 360
    lx, ly, lw, lh = 16, y, 300, 150
    S.part('aqua_listbox.bmp', 0, 1, lx, ly, lw, lh, (2, 2, 2, 2))
    hx = lx + 2
    for k, (label, wdt) in enumerate([('Name', 150), ('Size', 80), ('', lw - 4 - 15 - 230)]):
        S.part('aqua_header_item.bmp', 2 if k == 1 else 0, 12, hx, ly + 2, wdt, 19, (1, 2, 3, 3))
        S.text(label, hx + 6, ly + 2 + 14, 12)
        hx += wdt
    rows = ['Applications', 'Documents', 'Library', 'Music', 'Pictures']
    for k, name in enumerate(rows):
        ry = ly + 23 + k * 18
        if k == 1:
            S.solid(lx + 2, ry, lw - 4 - 15, 18, ACCENTS['Blue']['highlight'])
        if k == 3:
            S.solid(lx + 2, ry, lw - 4 - 15, 18, 212)
        S.part(dpi_name('aqua_treeview_glyph', 96), 1 if k == 0 else 0, 2, lx + 4, ry, 14, 18, truesize=True)
        S.text(name, lx + 22, ry + 13, 12, C(255) if k == 1 else C(0))
    # vertical scroll bar
    vx, vy, vh = lx + lw - 2 - 15, ly + 2, lh - 4 - 15
    S.part('aqua_scrollbar_arrow.bmp', 0, 20, vx, vy, 15, 15, (1, 1, 1, 1))
    S.part(dpi_name('aqua_scrollbar_arrow_glyph', 96), 0, 20, vx, vy, 15, 15, truesize=True)
    S.part(dpi_name('aqua_scrollbar_track_vert', 96), 0, 5, vx, vy + 15, 15, vh - 30, (0, 0, 0, 0))
    S.part(dpi_name('aqua_scrollbar_thumb_vert', 96), 0, 5, vx, vy + 40, 15, 50, (0, 0, 10, 10))
    S.part('aqua_scrollbar_arrow.bmp', 4, 20, vx, vy + vh - 15, 15, 15, (1, 1, 1, 1))
    S.part(dpi_name('aqua_scrollbar_arrow_glyph', 96), 4, 20, vx, vy + vh - 15, 15, 15, truesize=True)
    # horizontal scroll bar
    hx, hy, hw = lx + 2, ly + lh - 2 - 15, lw - 4 - 15
    S.part('aqua_scrollbar_arrow.bmp', 8, 20, hx, hy, 15, 15, (1, 1, 1, 1))
    S.part(dpi_name('aqua_scrollbar_arrow_glyph', 96), 8, 20, hx, hy, 15, 15, truesize=True)
    S.part(dpi_name('aqua_scrollbar_track_horz', 96), 0, 5, hx + 15, hy, hw - 30, 15, (0, 0, 0, 0))
    S.part(dpi_name('aqua_scrollbar_thumb_horz', 96), 1, 5, hx + 60, hy, 90, 15, (10, 10, 0, 0))
    S.part('aqua_scrollbar_arrow.bmp', 12, 20, hx + hw - 15, hy, 15, 15, (1, 1, 1, 1))
    S.part(dpi_name('aqua_scrollbar_arrow_glyph', 96), 12, 20, hx + hw - 15, hy, 15, 15, truesize=True)
    S.solid(hx + hw, hy, 15, 15, 245)

    # rebar with toolbar, status bar
    rx, ry, rw = 336, 360, 400
    S.part('aqua_rebar.bmp', 0, 1, rx, ry, rw, 30, (0, 0, 2, 2))
    for k, (frame, label) in enumerate([(0, 'New'), (1, 'Open'), (2, 'Save'), (4, 'Bold'), (0, 'Print')]):
        bx = rx + 6 + k * 52 + (12 if k >= 3 else 0)
        S.part('aqua_toolbar_button.bmp', frame, 8, bx, ry + 3, 48, 23, (5, 5, 5, 5))
        S.text(label, bx, ry + 3 + 16, 12, centre_w=48)
        if k == 2:
            S.part('aqua_toolbar_separator.bmp', 0, 1, bx + 50, ry + 3, 8, 23, (4, 3, 4, 4))
    S.part('aqua_rebar_chevron.bmp', 1, 3, rx + rw - 18, ry + 3, 14, 23, (5, 5, 5, 5))
    S.part(dpi_name('aqua_rebar_chevron_glyph', 96), 1, 3, rx + rw - 18, ry + 3, 14, 23, truesize=True)
    S.solid(rx, ry + 30, rw, 90, 255)
    S.part('aqua_status.bmp', 0, 1, rx, ry + 120, rw, 22, (0, 0, 2, 1))
    for k, (label, wdt) in enumerate([('Ready', 200), ('Ln 1, Col 1', 110)]):
        px0 = rx + sum([200, 110][:k])
        S.part('aqua_status_pane.bmp', 0, 1, px0, ry + 122, wdt, 20, (0, 1, 5, 4))
        S.text(label, px0 + 6, ry + 122 + 14, 12)

    # disabled and default-graphite samples, and 2x checkbox
    y = 520
    S.text('200%:', 336, y + 20, 11, C(90))
    S.part(dpi_name('aqua_checkbox', 192), 4, 20, 380, y, 32, 36, truesize=True)
    S.part(dpi_name('aqua_radiobutton', 192), 4, 8, 420, y, 32, 36, truesize=True)
    S.part(dpi_name('aqua_button', 192), 4, 6, 460, y, 120, 46, BM)
    S.text('OK', 460, y + 30, 24, centre_w=120)
    S.part(dpi_name('aqua_trackbar_thumb_down', 192), 0, 5, 590, y, 21, 42, truesize=True)

    write_png(os.path.join(outdir, 'contact_sheet.png'), Q.CGBitmapContextCreateImage(S.c))


def main(argv):
    outdir = None
    if len(argv) >= 2 and argv[0] == '--preview':
        outdir = argv[1]
    generate_bitmaps()
    refs = referenced_bitmaps()
    missing = refs - set(BITMAPS)
    unused = set(BITMAPS) - refs
    if missing or unused:
        raise SystemExit('bitmap mismatch: missing %s, unused %s' % (sorted(missing), sorted(unused)))
    for f in os.listdir(HERE):
        if f.endswith('.bmp') and f not in BITMAPS:
            os.remove(os.path.join(HERE, f))
    for f, r in BITMAPS.items():
        data = bmp_bytes(r)
        path = os.path.join(HERE, f)
        if os.path.exists(path):
            with open(path, 'rb') as fp:
                if fp.read() == data:
                    continue
        with open(path, 'wb') as fp:
            fp.write(data)
    write_rc()
    if outdir:
        preview(outdir)


if __name__ == '__main__':
    main(sys.argv[1:])
