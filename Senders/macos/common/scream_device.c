// Licensed under MS-PL, like the rest of Scream.

#include "scream_device.h"

static AudioObjectPropertyAddress global(AudioObjectPropertySelector sel)
{
  return (AudioObjectPropertyAddress){ sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
}

AudioObjectID scream_device_for_uid(CFStringRef uid)
{
  AudioObjectPropertyAddress a = global(kAudioHardwarePropertyTranslateUIDToDevice);
  AudioObjectID dev = kAudioObjectUnknown;
  UInt32 size = sizeof(dev);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(uid), &uid, &size, &dev) != noErr)
    return kAudioObjectUnknown;
  return dev;
}

AudioObjectID scream_find_device(void)
{
  return scream_device_for_uid(CFSTR(SCREAM_DEVICE_UID));
}

CFStringRef scream_copy_uid(AudioObjectID dev)
{
  AudioObjectPropertyAddress a = global(kAudioDevicePropertyDeviceUID);
  CFStringRef uid = NULL;
  UInt32 size = sizeof(uid);
  if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &uid) != noErr) return NULL;
  return uid;
}

CFDictionaryRef scream_copy_config(AudioObjectID dev)
{
  AudioObjectPropertyAddress a = global(SCREAM_CONFIG_PROPERTY);
  CFPropertyListRef pl = NULL;
  UInt32 size = sizeof(pl);
  if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &pl) != noErr || !pl) return NULL;
  if (CFGetTypeID(pl) != CFDictionaryGetTypeID()) { CFRelease(pl); return NULL; }
  return pl;
}

OSStatus scream_set_config(AudioObjectID dev, CFDictionaryRef changes)
{
  AudioObjectPropertyAddress a = global(SCREAM_CONFIG_PROPERTY);
  CFPropertyListRef pl = changes;
  return AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(pl), &pl);
}

OSStatus scream_set_config_int(AudioObjectID dev, CFStringRef key, int value)
{
  CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &value);
  CFDictionaryRef d = CFDictionaryCreate(NULL, (const void **)&key, (const void **)&n, 1,
                                         &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  OSStatus st = scream_set_config(dev, d);
  CFRelease(d);
  CFRelease(n);
  return st;
}

OSStatus scream_set_config_string(AudioObjectID dev, CFStringRef key, const char *value)
{
  CFStringRef s = CFStringCreateWithCString(NULL, value, kCFStringEncodingUTF8);
  if (!s) return kAudioHardwareIllegalOperationError;
  CFDictionaryRef d = CFDictionaryCreate(NULL, (const void **)&key, (const void **)&s, 1,
                                         &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  OSStatus st = scream_set_config(dev, d);
  CFRelease(d);
  CFRelease(s);
  return st;
}

int scream_config_int(CFDictionaryRef config, CFStringRef key)
{
  int v = 0;
  CFNumberRef n = config ? CFDictionaryGetValue(config, key) : NULL;
  if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue(n, kCFNumberIntType, &v);
  return v;
}

Float64 scream_get_rate(AudioObjectID dev)
{
  AudioObjectPropertyAddress a = global(kAudioDevicePropertyNominalSampleRate);
  Float64 rate = 0;
  UInt32 size = sizeof(rate);
  AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &rate);
  return rate;
}

OSStatus scream_set_rate(AudioObjectID dev, Float64 rate)
{
  AudioObjectPropertyAddress a = global(kAudioDevicePropertyNominalSampleRate);
  return AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(rate), &rate);
}

AudioObjectID scream_default_output(void)
{
  AudioObjectPropertyAddress a = global(kAudioHardwarePropertyDefaultOutputDevice);
  AudioObjectID dev = kAudioObjectUnknown;
  UInt32 size = sizeof(dev);
  AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &dev);
  return dev;
}

OSStatus scream_set_default_output(AudioObjectID dev)
{
  AudioObjectPropertySelector sels[] = { kAudioHardwarePropertyDefaultOutputDevice,
                                         kAudioHardwarePropertyDefaultSystemOutputDevice };
  for (int i = 0; i < 2; i++) {
    AudioObjectPropertyAddress a = global(sels[i]);
    OSStatus st = AudioObjectSetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, sizeof(dev), &dev);
    if (st != noErr) return st;
  }
  return noErr;
}
