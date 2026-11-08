/*
 * CGDirectDisplayCopyCurrentMetalDevice (10.11): no display has a Metal device on 10.9.
 */
#import <CoreGraphics/CoreGraphics.h>

id<MTLDevice> CGDirectDisplayCopyCurrentMetalDevice(CGDirectDisplayID display)
{
    return nil;
}
