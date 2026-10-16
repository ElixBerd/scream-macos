// screamctl: show or change where the Scream Audio device sends its stream.
//
// Licensed under MS-PL, like the rest of Scream.

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define kDevice_UID "ScreamAudio_UID"
#define kScreamConfigProperty 'scfg'

static AudioObjectID findDevice(void)
{
  AudioObjectPropertyAddress a = { kAudioHardwarePropertyTranslateUIDToDevice,
                                   kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
  CFStringRef uid = CFSTR(kDevice_UID);
  AudioObjectID dev = kAudioObjectUnknown;
  UInt32 size = sizeof(dev);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(uid), &uid, &size, &dev) != noErr)
    return kAudioObjectUnknown;
  return dev;
}

static int getInt(CFDictionaryRef d, CFStringRef key)
{
  int v = 0;
  CFNumberRef n = CFDictionaryGetValue(d, key);
  if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue(n, kCFNumberIntType, &v);
  return v;
}

static int show(AudioObjectID dev)
{
  AudioObjectPropertyAddress a = { kScreamConfigProperty, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
  CFPropertyListRef pl = NULL;
  UInt32 size = sizeof(pl);
  OSStatus st = AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &pl);
  if (st != noErr || !pl || CFGetTypeID(pl) != CFDictionaryGetTypeID()) {
    fprintf(stderr, "Could not read the Scream Audio configuration (%d)\n", (int)st);
    if (pl) CFRelease(pl);
    return 1;
  }
  CFDictionaryRef d = pl;
  char addr[64] = "?";
  CFStringRef s = CFDictionaryGetValue(d, CFSTR("address"));
  if (s) CFStringGetCString(s, addr, sizeof(addr), kCFStringEncodingUTF8);
  struct in_addr ia;
  int multicast = inet_pton(AF_INET, addr, &ia) == 1 && IN_MULTICAST(ntohl(ia.s_addr));

  AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
  Float64 rate = 0;
  size = sizeof(rate);
  AudioObjectGetPropertyData(dev, &ra, 0, NULL, &size, &rate);

  int bits = getInt(d, CFSTR("bits"));
  int payload = getInt(d, CFSTR("payload"));
  int frameBytes = 2 * (bits / 8);
  printf("destination  %s:%d (%s)\n", addr, getInt(d, CFSTR("port")), multicast ? "multicast" : "unicast");
  if (multicast) printf("ttl          %d\n", getInt(d, CFSTR("ttl")));
  printf("format       %.0f Hz, %d bit, stereo\n", rate, bits);
  if (frameBytes > 0 && rate > 0)
    printf("packet       %d bytes (%.2f ms)\n", payload, 1000.0 * (payload / frameBytes) / rate);
  CFRelease(pl);
  return 0;
}

static int makeDefault(AudioObjectID dev)
{
  AudioObjectPropertySelector sels[] = { kAudioHardwarePropertyDefaultOutputDevice, kAudioHardwarePropertyDefaultSystemOutputDevice };
  for (int i = 0; i < 2; i++) {
    AudioObjectPropertyAddress a = { sels[i], kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    OSStatus st = AudioObjectSetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, sizeof(dev), &dev);
    if (st != noErr) { fprintf(stderr, "Could not set default output (%d)\n", (int)st); return 1; }
  }
  return 0;
}

static void usage(const char *argv0)
{
  fprintf(stderr,
    "Usage: %s [options]\n"
    "Shows or changes where the \"Scream Audio\" output device streams to.\n"
    "Settings are kept by the driver across reboots.\n\n"
    "  -u ADDR    send unicast to ADDR\n"
    "  -g [ADDR]  send multicast (default group 239.255.77.77)\n"
    "  -p PORT    UDP port (default 4010)\n"
    "  -b BITS    sample width 16, 24 or 32\n"
    "  -s BYTES   PCM bytes per packet, 6..1152 (smaller = lower latency)\n"
    "  -t TTL     multicast TTL\n"
    "  -d         make Scream Audio the default output device\n"
    "  -h         this help\n\n"
    "Without options, prints the current settings.\n", argv0);
}

int main(int argc, char *argv[])
{
  CFMutableDictionaryRef changes = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  int setDefault = 0;
  int c;

  while ((c = getopt(argc, argv, "u:g::p:b:s:t:dh")) != -1) {
    CFStringRef key = NULL;
    switch (c) {
      case 'u':
      case 'g': {
        const char *addr = optarg;
        if (c == 'g' && !addr && optind < argc && argv[optind][0] != '-') addr = argv[optind++];
        if (!addr) addr = "239.255.77.77";
        struct in_addr ia;
        if (inet_pton(AF_INET, addr, &ia) != 1) { fprintf(stderr, "Invalid address: %s\n", addr); return 1; }
        if (c == 'u' && IN_MULTICAST(ntohl(ia.s_addr))) { fprintf(stderr, "%s is a multicast address, use -g\n", addr); return 1; }
        if (c == 'g' && !IN_MULTICAST(ntohl(ia.s_addr))) { fprintf(stderr, "%s is not a multicast address, use -u\n", addr); return 1; }
        CFStringRef s = CFStringCreateWithCString(NULL, addr, kCFStringEncodingUTF8);
        CFDictionarySetValue(changes, CFSTR("address"), s);
        CFRelease(s);
        continue;
      }
      case 'p': key = CFSTR("port"); break;
      case 'b': key = CFSTR("bits"); break;
      case 's': key = CFSTR("payload"); break;
      case 't': key = CFSTR("ttl"); break;
      case 'd': setDefault = 1; continue;
      default: usage(argv[0]); return c == 'h' ? 0 : 1;
    }
    char *end;
    long v = strtol(optarg, &end, 10);
    if (*end) { fprintf(stderr, "Not a number: %s\n", optarg); return 1; }
    int iv = (int)v;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &iv);
    CFDictionarySetValue(changes, key, n);
    CFRelease(n);
  }

  AudioObjectID dev = findDevice();
  if (dev == kAudioObjectUnknown) {
    fprintf(stderr, "Scream Audio device not found. Is the driver installed? (make install)\n");
    return 1;
  }

  if (CFDictionaryGetCount(changes) > 0) {
    AudioObjectPropertyAddress a = { kScreamConfigProperty, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFPropertyListRef pl = changes;
    OSStatus st = AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(pl), &pl);
    if (st != noErr) {
      fprintf(stderr, "The driver rejected these settings (%d). Check the values with -h.\n", (int)st);
      return 1;
    }
  }
  CFRelease(changes);

  if (setDefault && makeDefault(dev)) return 1;
  return show(dev);
}
