/*
 * AppKit methods 10.9 lacks, added to their classes when the image loads. class_addMethod leaves a
 * method the system already has in place.
 *
 * -[NSGraphicsContext CGContext] (10.10) is the context's graphicsPort, which on 10.9 is its CGContext.
 * +[NSWindow allowsAutomaticWindowTabbing] (10.12): 10.9 has no window tabbing, so windows never tab and
 * the setting has nothing to change.
 * -[NSScreen maximumPotentialExtendedDynamicRangeColorComponentValue] (10.15) is 1.0 for a display without
 * extended dynamic range, which every display is on 10.9.
 *
 * -[NSOpenGLContext dealloc] on 10.9 tears the context down by messaging -clearDrawable to the object being
 * deallocated, which reaches a subclass's override after the subclass's -dealloc has run. Later AppKit
 * tears the context down itself. The replacement finishes deallocation as an NSOpenGLContext, so the
 * teardown is NSOpenGLContext's own.
 *
 * -[NSOpenGLContext setView:] on 10.9 gives a context attached to a layer-backed view no drawable; its
 * default framebuffer is GL_FRAMEBUFFER_UNDEFINED unless the view draws through NSOpenGLLayer. Later
 * AppKit gives such a context a surface of its own. On 10.9 the replacement has AppKit attach the
 * context as it does to a view without a layer: a window server surface that -update keeps over the
 * view's frame. -setView: also detaches the context from its current view by whether that view has a
 * layer, so the current view, attached the same way, is treated as having none too.
 *
 * -[NSWindow release] on 10.9, releasing the last reference off the main thread, marks the window as
 * deallocating and queues its -dealloc to the main thread. Until that runs, main-thread code that reaches
 * the window through its views (Core Animation layout retains and releases a view's window) can take the
 * count to zero again and deallocate it first; the queued -dealloc then runs on freed memory. On 10.9 the
 * replacement performs a release made off the main thread on the main queue, so the count only falls
 * on the main thread and a window still deallocates there.
 */
#include <dispatch/dispatch.h>
#import <AppKit/AppKit.h>
#include <objc/runtime.h>
#include <pthread.h>

const char wineskin_appkit_polyfill = 0;

static CGContextRef graphicscontext_CGContext(NSGraphicsContext *self, SEL _cmd)
{
    return (CGContextRef)[self graphicsPort];
}

static BOOL window_allowsAutomaticWindowTabbing(Class self, SEL _cmd)
{
    return NO;
}

static void window_setAllowsAutomaticWindowTabbing(Class self, SEL _cmd, BOOL allows)
{
}

static CGFloat screen_maximumPotentialEDR(NSScreen *self, SEL _cmd)
{
    return 1.0;
}

static IMP glcontext_original_dealloc;

static void glcontext_dealloc(NSOpenGLContext *self, SEL _cmd)
{
    object_setClass(self, [NSOpenGLContext class]);
    ((void (*)(id, SEL))glcontext_original_dealloc)(self, _cmd);
}

static IMP glcontext_original_setView;
static IMP view_original_isLayerBacked;
static NSView *glcontext_attaching_views[2];

static BOOL view_isLayerBacked(NSView *self, SEL _cmd)
{
    if (self && (self == glcontext_attaching_views[0] || self == glcontext_attaching_views[1])) return NO;
    return ((BOOL (*)(id, SEL))view_original_isLayerBacked)(self, _cmd);
}

static void glcontext_setView(NSOpenGLContext *self, SEL _cmd, NSView *view)
{
    NSView *previous[2] = { glcontext_attaching_views[0], glcontext_attaching_views[1] };
    glcontext_attaching_views[0] = [self view];
    glcontext_attaching_views[1] = view;
    ((void (*)(id, SEL, NSView *))glcontext_original_setView)(self, _cmd, view);
    glcontext_attaching_views[0] = previous[0];
    glcontext_attaching_views[1] = previous[1];
}

static IMP window_original_release;

static void window_release_on_main_thread(void *window)
{
    ((void (*)(id, SEL))window_original_release)((id)window, @selector(release));
}

static void window_release(NSWindow *self, SEL _cmd)
{
    if (pthread_main_np())
        ((void (*)(id, SEL))window_original_release)(self, _cmd);
    else
        dispatch_async_f(dispatch_get_main_queue(), self, window_release_on_main_thread);
}

__attribute__((constructor))
static void install_appkit_methods(void)
{
    char types[64];
    snprintf(types, sizeof(types), "%s@:", @encode(CGContextRef));
    class_addMethod([NSGraphicsContext class], @selector(CGContext), (IMP)graphicscontext_CGContext, types);
    snprintf(types, sizeof(types), "%s@:", @encode(CGFloat));
    class_addMethod([NSScreen class], @selector(maximumPotentialExtendedDynamicRangeColorComponentValue),
                    (IMP)screen_maximumPotentialEDR, types);
    Method dealloc = class_getInstanceMethod([NSOpenGLContext class], @selector(dealloc));
    if (dealloc != class_getInstanceMethod([NSObject class], @selector(dealloc)))
        glcontext_original_dealloc = method_setImplementation(dealloc, (IMP)glcontext_dealloc);

    Method setView = class_getInstanceMethod([NSOpenGLContext class], @selector(setView:));
    Method isLayerBacked = class_getInstanceMethod([NSView class], sel_registerName("_isLayerBacked"));
    if (floor(NSAppKitVersionNumber) < 1343 /* 10.10 */ && setView && isLayerBacked)
    {
        view_original_isLayerBacked = method_setImplementation(isLayerBacked, (IMP)view_isLayerBacked);
        glcontext_original_setView = method_setImplementation(setView, (IMP)glcontext_setView);
    }

    Method release = class_getInstanceMethod([NSWindow class], @selector(release));
    if (floor(NSAppKitVersionNumber) < 1343 /* 10.10 */ && release &&
        release != class_getInstanceMethod([NSObject class], @selector(release)))
        window_original_release = method_setImplementation(release, (IMP)window_release);

    Class window_meta = object_getClass([NSWindow class]);
    snprintf(types, sizeof(types), "%s@:", @encode(BOOL));
    class_addMethod(window_meta, @selector(allowsAutomaticWindowTabbing), (IMP)window_allowsAutomaticWindowTabbing, types);
    snprintf(types, sizeof(types), "v@:%s", @encode(BOOL));
    class_addMethod(window_meta, @selector(setAllowsAutomaticWindowTabbing:), (IMP)window_setAllowsAutomaticWindowTabbing, types);
}
