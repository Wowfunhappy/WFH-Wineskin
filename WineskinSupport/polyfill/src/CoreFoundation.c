/*
 * CoreFoundation constants 10.9 lacks.
 *
 * kCFURLVolumeAvailableCapacityForImportantUsageKey (10.13) is the space available once purgeable
 * storage is reclaimed. 10.9's volumes have no purgeable storage, so that space is the volume's
 * available capacity, and the key is 10.9's NSURLVolumeAvailableCapacityKey. CFURL matches resource
 * keys by string value.
 */
#include <CoreFoundation/CoreFoundation.h>

const CFStringRef kCFURLVolumeAvailableCapacityForImportantUsageKey = CFSTR("NSURLVolumeAvailableCapacityKey");
