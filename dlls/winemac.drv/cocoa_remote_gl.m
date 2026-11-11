/*
 * WINESKIN: OpenGL in a child window of another process's window
 *
 * A Windows program can draw with OpenGL (and so with Direct3D) into a child window whose
 * top-level window belongs to another process, as a launcher that starts the game in a
 * window of its own does (Rez Infinite). The Cocoa window is the other process's, so the
 * drawing process has no view of it to attach its context to.
 *
 * The drawing process gives the context an off-screen window of its own, as the drawable, and
 * at each buffer swap copies the frame into an IOSurface. The pixels of a window that isn't on
 * screen fail OpenGL's pixel ownership test, so the program draws into a framebuffer object
 * instead (win32u's framebuffer surface, which win32u uses for such windows), and the copy reads
 * that framebuffer: win32u has it bound for reading when it swaps the window's drawable. A layer of a CAContext shows the
 * IOSurfaces; the process that owns the top-level window hosts that context in its window's
 * content view (a CALayerHost, as for the Metal swap chains of such windows) once a
 * WM_MACDRV_CREATE_REMOTE_LAYER message reaches it. Frames then go from the drawing process to
 * the window server without passing through the owner.
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

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/CGLIOSurface.h>
#import <OpenGL/gl3.h>

#import "macdrv_cocoa.h"
#import "cocoa_app.h"
#import "cocoa_event.h"
#import "cocoa_opengl.h"

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"

typedef uint32_t CGSConnectionID;
extern CGSConnectionID CGSMainConnectionID(void);

@interface CAContext : NSObject
+ (id) contextWithCGSConnection:(CGSConnectionID)connection options:(NSDictionary*)options;
@property(readonly) uint32_t contextId;
@property(retain) CALayer* layer;
@end

#define REMOTE_GL_BUFFERS 3

@class WineRemoteGLSurface;

/* The drawable: the content view of an off-screen window. */
@interface WineRemoteGLView : NSView
{
@public
    WineRemoteGLSurface* remote;    /* not retained: it owns the view */
}
@end

@implementation WineRemoteGLView

    - (BOOL) isOpaque
    {
        return YES;
    }

    /* macdrv_remove_view_opengl_context() calls this when a context leaves the view. */
    - (void) removeGLContext:(WineOpenGLContext*)context
    {
    }

@end


@interface WineRemoteGLSurface : NSObject
{
@public
    NSWindow* window;
    WineRemoteGLView* view;
    CAContext* caContext;
    CALayer* layer;
    uint32_t contextId;

    /* What the drawing threads read; @synchronized(self). */
    int width, height;              /* the drawable's size in pixels */
    unsigned int generation;        /* changes with the size */
    BOOL hidden;

    /* The IOSurfaces and the OpenGL objects that draw into them, in one context. */
    IOSurfaceRef surfaces[REMOTE_GL_BUFFERS];
    int surfaceWidth, surfaceHeight;
    CGLContextObj glContext;
    GLuint textures[REMOTE_GL_BUFFERS];
    GLuint fbos[REMOTE_GL_BUFFERS];
    int glWidth, glHeight;
    unsigned int contextGeneration; /* the generation the context's drawable was updated for */
    unsigned int next;
}
@end

@implementation WineRemoteGLSurface

    - (void) dealloc
    {
        unsigned int i;

        for (i = 0; i < REMOTE_GL_BUFFERS; i++)
            if (surfaces[i]) CFRelease(surfaces[i]);
        [super dealloc];
    }

@end


/***********************************************************************
 *              macdrv_remote_gl_create
 *
 * Creates the drawable and the layer for a child window of another process's window. The
 * caller sends the owner macdrv_remote_gl_context_id() to host.
 */
WineRemoteGLSurface *macdrv_remote_gl_create(void)
{
@autoreleasepool
{
    WineRemoteGLSurface *remote = [[WineRemoteGLSurface alloc] init];

    OnMainThread(^{
        NSRect frame = NSMakeRect(-32000, -32000, 64, 64);

        remote->window = [[NSWindow alloc] initWithContentRect:frame
                                                     styleMask:NSWindowStyleMaskBorderless
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
        [remote->window setReleasedWhenClosed:NO];
        [remote->window setAlphaValue:0];
        [remote->window setIgnoresMouseEvents:YES];
        [remote->window setExcludedFromWindowsMenu:YES];
        [remote->window setHasShadow:NO];
        [remote->window setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
                                              NSWindowCollectionBehaviorTransient |
                                              NSWindowCollectionBehaviorIgnoresCycle];
        remote->view = [[WineRemoteGLView alloc] initWithFrame:NSMakeRect(0, 0, 64, 64)];
        remote->view->remote = remote;
        [remote->window setContentView:remote->view];
        /* An OpenGL surface exists only for a window that is ordered in. */
        [remote->window orderBack:nil];

        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        remote->layer = [[CALayer alloc] init];
        remote->layer.anchorPoint = CGPointMake(0, 0);
        remote->layer.opaque = YES;
        remote->layer.hidden = YES;
        remote->layer.contentsGravity = kCAGravityResize;
        remote->layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        remote->caContext = [[CAContext contextWithCGSConnection:CGSMainConnectionID()
                                                         options:[NSDictionary dictionary]] retain];
        remote->caContext.layer = remote->layer;
        remote->contextId = remote->caContext.contextId;
        [CATransaction commit];
    });

    if (!remote->contextId)
    {
        macdrv_remote_gl_destroy(remote);
        return NULL;
    }
    return remote;
}
}

unsigned int macdrv_remote_gl_context_id(WineRemoteGLSurface *remote)
{
    return remote->contextId;
}

/***********************************************************************
 *              macdrv_remote_gl_destroy
 */
void macdrv_remote_gl_destroy(WineRemoteGLSurface *remote)
{
@autoreleasepool
{
    OnMainThread(^{
        [CATransaction begin];
        remote->caContext.layer = nil;
        [CATransaction commit];
        [remote->caContext release];
        [remote->layer release];
        [remote->window orderOut:nil];
        [remote->window setContentView:nil];
        remote->view->remote = nil;
        [remote->view release];
        [remote->window release];
    });
    /* The OpenGL objects die with their context; a context still alive keeps a texture and a
       framebuffer per buffer until it goes. */
    [remote release];
}
}

/***********************************************************************
 *              macdrv_remote_gl_set_geometry
 *
 * frame: the child's client area in the top-level window's content view, in Windows'
 * raw coordinates (top-left origin); width, height: the drawable's size in pixels.
 */
void macdrv_remote_gl_set_geometry(WineRemoteGLSurface *remote, CGRect frame, int width, int height, bool hidden)
{
@autoreleasepool
{
    if (width < 1) width = 1;
    if (height < 1) height = 1;

    @synchronized(remote)
    {
        if (remote->width != width || remote->height != height)
        {
            remote->width = width;
            remote->height = height;
            remote->generation++;
        }
        remote->hidden = hidden;
    }

    OnMainThread(^{
        NSRect content = NSMakeRect(-32000, -32000, width, height);
        if (!NSEqualRects([remote->window frame], content))
            [remote->window setFrame:content display:NO];

        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        remote->layer.frame = cgrect_mac_from_win(frame);
        if (hidden || remote->layer.contents) remote->layer.hidden = hidden;
        [CATransaction commit];
    });
}
}

/***********************************************************************
 *              macdrv_remote_gl_make_current
 */
void macdrv_remote_gl_make_current(WineOpenGLContext *context, WineRemoteGLSurface *remote)
{
@autoreleasepool
{
    unsigned int generation;

    @synchronized(remote) { generation = remote->generation; }

    if ((NSView*)[context view] != remote->view)
    {
        GLint enabled;

        macdrv_make_context_current(context, NULL, CGRectNull);
        if (CGLIsEnabled(context.CGLContextObj, kCGLCESurfaceBackingSize, &enabled) == kCGLNoError && enabled)
            CGLDisable(context.CGLContextObj, kCGLCESurfaceBackingSize);
        context.backingSizeEnabled = NO;
        [context setView:remote->view];
    }
    else if (remote->contextGeneration != generation)
        OnMainThread(^{ [context update]; });
    remote->contextGeneration = generation;

    [context makeCurrentContext];
}
}

static void create_surfaces(WineRemoteGLSurface *remote, int width, int height)
{
    NSDictionary *props = @{
        (id)kIOSurfaceWidth: @(width),
        (id)kIOSurfaceHeight: @(height),
        (id)kIOSurfaceBytesPerElement: @4,
        (id)kIOSurfacePixelFormat: @((unsigned int)'BGRA'),
        (id)kIOSurfaceIsGlobal: @YES,
    };
    unsigned int i;

    for (i = 0; i < REMOTE_GL_BUFFERS; i++)
    {
        if (remote->surfaces[i]) CFRelease(remote->surfaces[i]);
        remote->surfaces[i] = IOSurfaceCreate((CFDictionaryRef)props);
        /* A new IOSurface holds whatever its memory held before, other programs' pixels included,
           until the first frame is copied in; it starts black. */
        if (remote->surfaces[i] && IOSurfaceLock(remote->surfaces[i], 0, NULL) == kIOReturnSuccess)
        {
            memset(IOSurfaceGetBaseAddress(remote->surfaces[i]), 0, IOSurfaceGetAllocSize(remote->surfaces[i]));
            IOSurfaceUnlock(remote->surfaces[i], 0, NULL);
        }
    }
    remote->surfaceWidth = width;
    remote->surfaceHeight = height;
}

/* Makes a texture on each IOSurface and a framebuffer for it, in the current context. */
static BOOL create_gl_objects(WineRemoteGLSurface *remote, CGLContextObj cgl)
{
    GLint old_texture, old_draw_fbo;
    unsigned int i;
    BOOL ok = YES;

    if (remote->glContext == cgl && remote->textures[0])
    {
        glDeleteFramebuffers(REMOTE_GL_BUFFERS, remote->fbos);
        glDeleteTextures(REMOTE_GL_BUFFERS, remote->textures);
    }

    glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE, &old_texture);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_fbo);
    glGenTextures(REMOTE_GL_BUFFERS, remote->textures);
    glGenFramebuffers(REMOTE_GL_BUFFERS, remote->fbos);
    for (i = 0; i < REMOTE_GL_BUFFERS; i++)
    {
        CGLError err;

        glBindTexture(GL_TEXTURE_RECTANGLE, remote->textures[i]);
        err = CGLTexImageIOSurface2D(cgl, GL_TEXTURE_RECTANGLE, GL_RGBA, remote->surfaceWidth, remote->surfaceHeight,
                                     GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, remote->surfaces[i], 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, remote->fbos[i]);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, remote->textures[i], 0);
        if (err != kCGLNoError || glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            ok = NO;
    }
    glBindTexture(GL_TEXTURE_RECTANGLE, old_texture);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw_fbo);

    remote->glContext = cgl;
    remote->glWidth = remote->surfaceWidth;
    remote->glHeight = remote->surfaceHeight;
    return ok;
}

/***********************************************************************
 *              macdrv_remote_gl_present
 *
 * Called with the context current, before it swaps its buffers: copies the back buffer into the
 * next IOSurface and shows that.
 */
void macdrv_remote_gl_present(WineOpenGLContext *context)
{
@autoreleasepool
{
    WineRemoteGLSurface *remote;
    CGLContextObj cgl = context.CGLContextObj;
    GLint read_fbo, draw_fbo, read_buffer;
    GLboolean scissor, srgb, discard = GL_FALSE, mask[4], doublebuffer;
    unsigned int i, generation;
    int width, height;
    BOOL core, hidden;

    if (![[context view] isKindOfClass:[WineRemoteGLView class]]) return;
    remote = ((WineRemoteGLView*)[context view])->remote;
    if (!remote) return;

    @synchronized(remote)
    {
        width = remote->width;
        height = remote->height;
        generation = remote->generation;
        hidden = remote->hidden;
    }
    if (remote->contextGeneration != generation)
    {
        OnMainThread(^{ [context update]; });
        remote->contextGeneration = generation;
    }

    if (remote->surfaceWidth != width || remote->surfaceHeight != height)
        create_surfaces(remote, width, height);
    if (!remote->surfaces[0]) return;
    if ((remote->glContext != cgl || remote->glWidth != width || remote->glHeight != height) &&
        !create_gl_objects(remote, cgl))
        return;

    core = atoi((const char*)glGetString(GL_VERSION)) >= 3;
    i = remote->next;
    remote->next = (i + 1) % REMOTE_GL_BUFFERS;

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    if (core) discard = glIsEnabled(GL_RASTERIZER_DISCARD);

    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    if (read_fbo)
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    else
    {
        glGetBooleanv(GL_DOUBLEBUFFER, &doublebuffer);
        glReadBuffer(doublebuffer ? GL_BACK : GL_FRONT);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, remote->fbos[i]);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    if (srgb) glDisable(GL_FRAMEBUFFER_SRGB);
    if (discard) glDisable(GL_RASTERIZER_DISCARD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    /* The back buffer is bottom-up, the IOSurface top-down. */
    glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (discard) glEnable(GL_RASTERIZER_DISCARD);
    if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glReadBuffer(read_buffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
    glFlush();

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    remote->layer.contents = (id)remote->surfaces[i];
    remote->layer.hidden = hidden;
    [CATransaction commit];
    [CATransaction flush];
}
}
