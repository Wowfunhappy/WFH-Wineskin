/* CoreFoundation declarations newer than the 10.9 SDK; src/CoreFoundation.c defines the constants. */
#include_next <CoreFoundation/CoreFoundation.h>

#ifndef WINESKIN_COREFOUNDATION_H
#define WINESKIN_COREFOUNDATION_H

#if MAC_OS_X_VERSION_MAX_ALLOWED < 101100
/* The 10.11 SDK names the type of CFRunLoopRunInMode's result, the kCFRunLoopRun* values. */
typedef SInt32 CFRunLoopRunResult;
#endif

#if MAC_OS_X_VERSION_MAX_ALLOWED < 101300
CF_EXPORT const CFStringRef kCFURLVolumeAvailableCapacityForImportantUsageKey;
#endif

#endif
