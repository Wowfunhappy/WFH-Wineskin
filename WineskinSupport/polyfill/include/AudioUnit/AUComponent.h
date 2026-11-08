/* AUComponent.h with the audio unit subtypes later SDKs add. */
#include_next <AudioUnit/AUComponent.h>

#if !defined(WINESKIN_AUCOMPONENT_H) && MAC_OS_X_VERSION_MAX_ALLOWED < 101000
#define WINESKIN_AUCOMPONENT_H
enum {
    kAudioUnitSubType_SpatialMixer = '3dem'
};

/* src/AudioUnit.c registers a spatial mixer where the system has none; this pulls it into the image. */
extern const char wineskin_audiounit_polyfill;
static const char *const wineskin_audiounit_polyfill_ref __attribute__((used)) = &wineskin_audiounit_polyfill;
#endif
