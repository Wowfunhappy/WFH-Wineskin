/* CoreGraphics with CGDirectDisplayCopyCurrentMetalDevice (10.11): 10.9 has no Metal devices
   (src/objc/CoreGraphics.m). */
#include_next <CoreGraphics/CoreGraphics.h>

#if !defined(WINESKIN_COREGRAPHICS_METAL_H) && MAC_OS_X_VERSION_MAX_ALLOWED < 101100
#define WINESKIN_COREGRAPHICS_METAL_H
#ifdef __OBJC__
@protocol MTLDevice;
CG_EXTERN id<MTLDevice> CGDirectDisplayCopyCurrentMetalDevice(CGDirectDisplayID display);
#endif
#endif
