/* HIToolbox/Events.h with the virtual key code the 10.12 SDK adds for the right Command key. */
#include_next <HIToolbox/Events.h>

#ifndef WINESKIN_HITOOLBOX_EVENTS_H
#define WINESKIN_HITOOLBOX_EVENTS_H
#if MAC_OS_X_VERSION_MAX_ALLOWED < 101200
enum {
    kVK_RightCommand = 0x36
};
#endif
#endif
