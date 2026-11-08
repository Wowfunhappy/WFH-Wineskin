/* QuartzCore with CAMetalLayer (10.11). The class is weak and looked up by name at load
   (polyfill/build/link-<arch>.rsp), so on 10.9 it is nil. */
#include_next <QuartzCore/QuartzCore.h>

#if defined(__OBJC__) && !defined(WINESKIN_CAMETALLAYER_H) && MAC_OS_X_VERSION_MAX_ALLOWED < 101100
#define WINESKIN_CAMETALLAYER_H
#import <Metal/Metal.h>

__attribute__((weak_import))
@interface CAMetalLayer : CALayer
@property (retain) id<MTLDevice> device;
@property MTLPixelFormat pixelFormat;
@property BOOL framebufferOnly;
@property CGSize drawableSize;
@property NSUInteger maximumDrawableCount;
@property BOOL presentsWithTransaction;
@property CGColorSpaceRef colorspace;
@property BOOL displaySyncEnabled;
@property BOOL allowsNextDrawableTimeout;
@end
#endif
