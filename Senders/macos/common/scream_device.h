// Helpers for talking to the Scream Audio device from user space.
// Shared by screamctl and the menu bar app.
//
// Licensed under MS-PL, like the rest of Scream.

#ifndef SCREAM_DEVICE_H
#define SCREAM_DEVICE_H

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#define SCREAM_DEVICE_UID "ScreamAudio_UID"
#define SCREAM_CONFIG_PROPERTY 'scfg'
#define SCREAM_DEFAULT_GROUP "239.255.77.77"

// Device lookup.
AudioObjectID scream_device_for_uid(CFStringRef uid);
AudioObjectID scream_find_device(void);
CFStringRef scream_copy_uid(AudioObjectID dev);          // caller releases

// Driver configuration: a dictionary with "address" (string), "port",
// "bits", "payload", "ttl" (numbers). Set merges only the keys given.
CFDictionaryRef scream_copy_config(AudioObjectID dev);   // caller releases
OSStatus scream_set_config(AudioObjectID dev, CFDictionaryRef changes);
OSStatus scream_set_config_int(AudioObjectID dev, CFStringRef key, int value);
OSStatus scream_set_config_string(AudioObjectID dev, CFStringRef key, const char *value);
int scream_config_int(CFDictionaryRef config, CFStringRef key);

// Sample rate of a device.
Float64 scream_get_rate(AudioObjectID dev);
OSStatus scream_set_rate(AudioObjectID dev, Float64 rate);

// System default output (both the main and the alert/system output).
AudioObjectID scream_default_output(void);
OSStatus scream_set_default_output(AudioObjectID dev);

#endif
