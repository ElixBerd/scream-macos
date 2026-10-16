// Scream Audio: a virtual macOS output device that puts everything played
// through it onto the LAN as a Scream PCM stream.
//
// This is an AudioServerPlugIn (user-space HAL driver) loaded by coreaudiod,
// structured after Apple's NullAudio sample. The device clock runs off
// mach_absolute_time(). The stream's physical format is 16, 24 or 32 bit
// integer PCM, i.e. exactly what Scream puts on the wire, so the HAL does the
// float -> integer conversion and every IO cycle DoIOOperation(WriteMix) just
// copies the bytes into packets and sends them with a non-blocking sendto(),
// the same way the Windows driver does from its kernel DMA callback. Volume,
// mute and 16 bit dither are applied to the float mix in ProcessMix. Nothing
// is buffered on our side besides filling the current packet.
//
// The sample width can be picked in Audio MIDI Setup (the physical format),
// in the menu bar app or with screamctl; all three end up in the same place.
//
// Destination, sample width and packet size live in a custom property
// ('scfg', a CFDictionary) that screamctl reads and writes; the driver keeps
// it in the HAL's per-plug-in storage so it survives reboots.
//
// Licensed under MS-PL, like the rest of Scream.

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <syslog.h>

// ---------------------------------------------------------------- constants

#define kPlugIn_BundleID          "com.github.elixberd.ScreamAudio"
#define kDevice_Name              "Scream Audio"
#define kDevice_UID               "ScreamAudio_UID"
#define kDevice_ModelUID          "ScreamAudio_ModelUID"
#define kManufacturer             "Scream"

#define kScreamConfigProperty     'scfg'
#define kConfigStorageKey         CFSTR("config")

#define kChannels                 2
#define kBytesPerFrameFloat       (kChannels * sizeof(Float32))
#define kRingBufferFrames         16384   // zero time stamp period
#define kHeaderSize               5
#define kMaxPayload               1152
#define kChannelMaskStereo        0x0003

#define kDefaultAddress           "239.255.77.77"
#define kDefaultPort              4010
#define kDefaultBits              16
#define kDefaultPayload           1152
#define kDefaultTTL               1

#define kVolume_MinDB             (-96.0f)
#define kVolume_MaxDB             (0.0f)

enum {
  kObjectID_PlugIn  = kAudioObjectPlugInObject,
  kObjectID_Device  = 2,
  kObjectID_Stream  = 3,
  kObjectID_Volume  = 4,
  kObjectID_Mute    = 5,
};

static const Float64 kSampleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };
#define kNumSampleRates (sizeof(kSampleRates) / sizeof(kSampleRates[0]))
static const UInt32 kBitDepths[] = { 16, 24, 32 };
#define kNumBitDepths (sizeof(kBitDepths) / sizeof(kBitDepths[0]))
#define kNumPhysicalFormats (kNumSampleRates * kNumBitDepths)

// ---------------------------------------------------------------- state

static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gIOMutex = PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef gHost = NULL;
static UInt32 gRefCount = 0;

static Float64 gSampleRate = 48000.0;
static UInt32 gBits = kDefaultBits;     // physical format = wire format
static UInt32 gIOClients = 0;
static Float64 gHostTicksPerFrame = 0;
static UInt64 gAnchorHostTime = 0;
static UInt64 gTimestampCount = 0;

static _Atomic float gVolume = 1.0f;     // scalar 0..1
static _Atomic bool gMute = false;

// Network configuration. Writers hold gConfigLock; the IO thread only ever
// trylocks it and picks up a new generation when it gets the lock.
typedef struct {
  char address[64];
  int port;
  int bits;
  int payload;
  int ttl;
} scream_config_t;

static os_unfair_lock gConfigLock = OS_UNFAIR_LOCK_INIT;
static scream_config_t gConfig = { kDefaultAddress, kDefaultPort, kDefaultBits, kDefaultPayload, kDefaultTTL };
static _Atomic uint32_t gConfigGeneration = 1;

// Owned by the IO thread.
static struct {
  int sock;
  struct sockaddr_in addr;
  bool addrValid;
  uint32_t generation;
  int bytesPerSample;
  int payload;
  int fill;
  uint32_t rng;
  uint8_t packet[kHeaderSize + kMaxPayload];
} gTx = { .sock = -1, .rng = 0x9e3779b9u };

// ---------------------------------------------------------------- helpers

static int rateByte(Float64 rate)
{
  int r = (int)lround(rate);
  if (r % 44100 == 0) return 0x80 | (r / 44100);
  return r / 48000;
}

static float volumeToGain(float scalar) { return scalar * scalar; }

static float volumeToDB(float scalar)
{
  float g = volumeToGain(scalar);
  if (g <= 0.0f) return kVolume_MinDB;
  float db = 20.0f * log10f(g);
  return db < kVolume_MinDB ? kVolume_MinDB : db;
}

static float dbToVolume(float db)
{
  if (db <= kVolume_MinDB) return 0.0f;
  if (db > kVolume_MaxDB) db = kVolume_MaxDB;
  return sqrtf(powf(10.0f, db / 20.0f));
}

static bool validRate(Float64 rate)
{
  for (UInt32 i = 0; i < kNumSampleRates; i++) if (rate == kSampleRates[i]) return true;
  return false;
}

static bool validBits(UInt32 bits)
{
  return bits == 16 || bits == 24 || bits == 32;
}

// Physical format: packed little-endian signed integers, as Scream sends them.
static void fillPhysical(AudioStreamBasicDescription *f, Float64 rate, UInt32 bits)
{
  f->mSampleRate = rate;
  f->mFormatID = kAudioFormatLinearPCM;
  f->mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
  f->mBytesPerPacket = kChannels * (bits / 8);
  f->mFramesPerPacket = 1;
  f->mBytesPerFrame = kChannels * (bits / 8);
  f->mChannelsPerFrame = kChannels;
  f->mBitsPerChannel = bits;
  f->mReserved = 0;
}

// Virtual format: what clients see and the HAL mixes in.
static void fillFormat(AudioStreamBasicDescription *f, Float64 rate)
{
  f->mSampleRate = rate;
  f->mFormatID = kAudioFormatLinearPCM;
  f->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
  f->mBytesPerPacket = kBytesPerFrameFloat;
  f->mFramesPerPacket = 1;
  f->mBytesPerFrame = kBytesPerFrameFloat;
  f->mChannelsPerFrame = kChannels;
  f->mBitsPerChannel = 32;
  f->mReserved = 0;
}

static void updateTicksPerFrame(void)
{
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  Float64 ticksPerSecond = 1e9 * (Float64)tb.denom / (Float64)tb.numer;
  gHostTicksPerFrame = ticksPerSecond / gSampleRate;
}

// ---------------------------------------------------------------- config

static CFDictionaryRef configCopyDictionary(const scream_config_t *c)
{
  CFStringRef keys[5] = { CFSTR("address"), CFSTR("port"), CFSTR("bits"), CFSTR("payload"), CFSTR("ttl") };
  CFTypeRef values[5];
  values[0] = CFStringCreateWithCString(NULL, c->address, kCFStringEncodingUTF8);
  values[1] = CFNumberCreate(NULL, kCFNumberIntType, &c->port);
  values[2] = CFNumberCreate(NULL, kCFNumberIntType, &c->bits);
  values[3] = CFNumberCreate(NULL, kCFNumberIntType, &c->payload);
  values[4] = CFNumberCreate(NULL, kCFNumberIntType, &c->ttl);
  CFDictionaryRef d = CFDictionaryCreate(NULL, (const void **)keys, (const void **)values, 5,
                                         &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  for (int i = 0; i < 5; i++) CFRelease(values[i]);
  return d;
}

static bool readInt(CFDictionaryRef d, CFStringRef key, int *out)
{
  CFTypeRef v = CFDictionaryGetValue(d, key);
  if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) return false;
  return CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, out);
}

// Merges the keys present in d into *c. Returns false if anything is invalid.
static bool configMerge(scream_config_t *c, CFDictionaryRef d)
{
  scream_config_t n = *c;
  CFTypeRef a = CFDictionaryGetValue(d, CFSTR("address"));
  if (a) {
    struct in_addr tmp;
    if (CFGetTypeID(a) != CFStringGetTypeID()) return false;
    if (!CFStringGetCString((CFStringRef)a, n.address, sizeof(n.address), kCFStringEncodingUTF8)) return false;
    if (inet_pton(AF_INET, n.address, &tmp) != 1) return false;
  }
  readInt(d, CFSTR("port"), &n.port);
  readInt(d, CFSTR("bits"), &n.bits);
  readInt(d, CFSTR("payload"), &n.payload);
  readInt(d, CFSTR("ttl"), &n.ttl);
  if (n.port <= 0 || n.port > 65535) return false;
  if (n.bits != 16 && n.bits != 24 && n.bits != 32) return false;
  if (n.payload < 6 || n.payload > kMaxPayload) return false;
  if (n.ttl < 0 || n.ttl > 255) return false;
  *c = n;
  return true;
}

static void configStore(void)
{
  if (!gHost) return;
  os_unfair_lock_lock(&gConfigLock);
  scream_config_t c = gConfig;
  os_unfair_lock_unlock(&gConfigLock);
  CFDictionaryRef d = configCopyDictionary(&c);
  gHost->WriteToStorage(gHost, kConfigStorageKey, d);
  CFRelease(d);
}

static void configLoad(void)
{
  CFPropertyListRef stored = NULL;
  if (gHost->CopyFromStorage(gHost, kConfigStorageKey, &stored) == noErr && stored) {
    if (CFGetTypeID(stored) == CFDictionaryGetTypeID()) {
      os_unfair_lock_lock(&gConfigLock);
      configMerge(&gConfig, (CFDictionaryRef)stored);
      os_unfair_lock_unlock(&gConfigLock);
    }
    CFRelease(stored);
  }
  atomic_fetch_add(&gConfigGeneration, 1);
}

// IO thread: adopt a new configuration if one is pending and the lock is free.
static void txRefreshConfig(void)
{
  uint32_t gen = atomic_load_explicit(&gConfigGeneration, memory_order_acquire);
  if (gen == gTx.generation) return;
  if (!os_unfair_lock_trylock(&gConfigLock)) return;
  scream_config_t c = gConfig;
  os_unfair_lock_unlock(&gConfigLock);

  gTx.generation = gen;
  gTx.bytesPerSample = (int)gBits / 8;   // gBits only changes while IO is stopped
  int frameBytes = kChannels * gTx.bytesPerSample;
  gTx.payload = (c.payload / frameBytes) * frameBytes;
  gTx.fill = 0;

  memset(&gTx.addr, 0, sizeof(gTx.addr));
  gTx.addr.sin_family = AF_INET;
  gTx.addr.sin_port = htons((uint16_t)c.port);
  gTx.addrValid = inet_pton(AF_INET, c.address, &gTx.addr.sin_addr) == 1;

  if (gTx.sock >= 0 && gTx.addrValid && IN_MULTICAST(ntohl(gTx.addr.sin_addr.s_addr))) {
    unsigned char ttl = (unsigned char)c.ttl;
    setsockopt(gTx.sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  }

  gTx.packet[2] = kChannels;
  gTx.packet[3] = kChannelMaskStereo & 0xff;
  gTx.packet[4] = (kChannelMaskStereo >> 8) & 0xff;
}

static void openSocket(void)
{
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) {
    syslog(LOG_ERR, "ScreamAudio: socket() failed: %d", errno);
    return;
  }
  int tos = 0xb8;   // DSCP EF: low-delay queue on switches and Wi-Fi (WMM voice)
  setsockopt(s, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
  int svc = NET_SERVICE_TYPE_VO;
  setsockopt(s, SOL_SOCKET, SO_NET_SERVICE_TYPE, &svc, sizeof(svc));
  unsigned char loop = 1;
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
  gTx.sock = s;
}

// ---------------------------------------------------------------- IO path

static inline void txFlush(void)
{
  if (gTx.sock >= 0 && gTx.addrValid)
    sendto(gTx.sock, gTx.packet, kHeaderSize + gTx.fill, MSG_DONTWAIT,
           (const struct sockaddr *)&gTx.addr, sizeof(gTx.addr));
  gTx.fill = 0;
}

// ProcessMix: the full mix in float, before the HAL converts it to integers.
static void processMix(Float32 *buf, UInt32 frames)
{
  float gain = atomic_load_explicit(&gMute, memory_order_relaxed)
             ? 0.0f : volumeToGain(atomic_load_explicit(&gVolume, memory_order_relaxed));
  bool dither = gBits == 16;
  if (gain == 1.0f && !dither) return;
  for (UInt32 i = 0; i < frames * kChannels; i++) {
    float x = buf[i] * gain;
    if (dither) {
      // TPDF dither, +-1 LSB of 16 bit.
      gTx.rng = gTx.rng * 1664525u + 1013904223u;
      float r1 = (float)(gTx.rng >> 8) * (1.0f / 16777216.0f);
      gTx.rng = gTx.rng * 1664525u + 1013904223u;
      float r2 = (float)(gTx.rng >> 8) * (1.0f / 16777216.0f);
      x += (r1 - r2) * (1.0f / 32768.0f);
    }
    buf[i] = x;
  }
}

// WriteMix: the mix in the physical format, which is already Scream PCM.
static void txWrite(const uint8_t *buf, UInt32 frames)
{
  txRefreshConfig();
  if (gTx.payload <= 0) return;
  gTx.packet[0] = (uint8_t)rateByte(gSampleRate);
  gTx.packet[1] = (uint8_t)gBits;

  UInt32 left = frames * kChannels * (UInt32)gTx.bytesPerSample;
  while (left > 0) {
    UInt32 n = (UInt32)(gTx.payload - gTx.fill);
    if (n > left) n = left;
    memcpy(gTx.packet + kHeaderSize + gTx.fill, buf, n);
    gTx.fill += (int)n;
    buf += n;
    left -= n;
    if (gTx.fill >= gTx.payload) txFlush();
  }
}

// ---------------------------------------------------------------- COM boilerplate

static HRESULT Scream_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG Scream_AddRef(void *inDriver);
static ULONG Scream_Release(void *inDriver);
static OSStatus Scream_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus Scream_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
static OSStatus Scream_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus Scream_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus Scream_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus Scream_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static OSStatus Scream_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static Boolean Scream_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
static OSStatus Scream_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
static OSStatus Scream_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
static OSStatus Scream_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, UInt32 *, void *);
static OSStatus Scream_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, const void *);
static OSStatus Scream_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus Scream_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus Scream_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
static OSStatus Scream_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
static OSStatus Scream_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);
static OSStatus Scream_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *, void *, void *);
static OSStatus Scream_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);

static AudioServerPlugInDriverInterface gInterface = {
  NULL,
  Scream_QueryInterface,
  Scream_AddRef,
  Scream_Release,
  Scream_Initialize,
  Scream_CreateDevice,
  Scream_DestroyDevice,
  Scream_AddDeviceClient,
  Scream_RemoveDeviceClient,
  Scream_PerformDeviceConfigurationChange,
  Scream_AbortDeviceConfigurationChange,
  Scream_HasProperty,
  Scream_IsPropertySettable,
  Scream_GetPropertyDataSize,
  Scream_GetPropertyData,
  Scream_SetPropertyData,
  Scream_StartIO,
  Scream_StopIO,
  Scream_GetZeroTimeStamp,
  Scream_WillDoIOOperation,
  Scream_BeginIOOperation,
  Scream_DoIOOperation,
  Scream_EndIOOperation,
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

// Entry point named in Info.plist (CFPlugInFactories).
void *ScreamAudio_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID)
{
  (void)inAllocator;
  if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return gDriverRef;
  return NULL;
}

static HRESULT Scream_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface)
{
  if (inDriver != gDriverRef || !outInterface) return kAudioHardwareBadObjectError;
  CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
  HRESULT r = E_NOINTERFACE;
  if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
    pthread_mutex_lock(&gStateMutex);
    gRefCount++;
    pthread_mutex_unlock(&gStateMutex);
    *outInterface = gDriverRef;
    r = S_OK;
  }
  CFRelease(req);
  return r;
}

static ULONG Scream_AddRef(void *inDriver)
{
  if (inDriver != gDriverRef) return 0;
  pthread_mutex_lock(&gStateMutex);
  ULONG r = ++gRefCount;
  pthread_mutex_unlock(&gStateMutex);
  return r;
}

static ULONG Scream_Release(void *inDriver)
{
  if (inDriver != gDriverRef) return 0;
  pthread_mutex_lock(&gStateMutex);
  if (gRefCount > 0) gRefCount--;
  ULONG r = gRefCount;
  pthread_mutex_unlock(&gStateMutex);
  return r;
}

// ---------------------------------------------------------------- basic operations

static OSStatus Scream_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
  if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
  gHost = inHost;

  CFPropertyListRef stored = NULL;
  if (gHost->CopyFromStorage(gHost, CFSTR("sample rate"), &stored) == noErr && stored) {
    Float64 rate = 0;
    if (CFGetTypeID(stored) == CFNumberGetTypeID() &&
        CFNumberGetValue((CFNumberRef)stored, kCFNumberFloat64Type, &rate) && validRate(rate))
      gSampleRate = rate;
    CFRelease(stored);
  }
  configLoad();
  gBits = (UInt32)gConfig.bits;
  updateTicksPerFrame();
  openSocket();
  return noErr;
}

static OSStatus Scream_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci, AudioObjectID *out)
{ return kAudioHardwareUnsupportedOperationError; }

static OSStatus Scream_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID dev)
{ return kAudioHardwareUnsupportedOperationError; }

static OSStatus Scream_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *ci)
{ return d == gDriverRef && dev == kObjectID_Device ? noErr : kAudioHardwareBadObjectError; }

static OSStatus Scream_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *ci)
{ return d == gDriverRef && dev == kObjectID_Device ? noErr : kAudioHardwareBadObjectError; }

static OSStatus Scream_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void *info)
{
  if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
  // action = (sample rate << 8) | bits, see requestFormat().
  Float64 rate = (Float64)(action >> 8);
  UInt32 bits = (UInt32)(action & 0xff);
  if (!validRate(rate) || !validBits(bits)) return kAudioHardwareBadObjectError;
  pthread_mutex_lock(&gStateMutex);
  gSampleRate = rate;
  gBits = bits;
  updateTicksPerFrame();
  pthread_mutex_unlock(&gStateMutex);

  os_unfair_lock_lock(&gConfigLock);
  gConfig.bits = (int)bits;
  os_unfair_lock_unlock(&gConfigLock);
  atomic_fetch_add_explicit(&gConfigGeneration, 1, memory_order_release);
  configStore();

  CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloat64Type, &rate);
  gHost->WriteToStorage(gHost, CFSTR("sample rate"), n);
  CFRelease(n);
  return noErr;
}

static OSStatus Scream_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void *info)
{ return noErr; }

// ---------------------------------------------------------------- properties

// Copies a value of type T into outData after checking the buffer size.
#define RETURN_VALUE(T, v) do { \
    if (inDataSize < sizeof(T)) return kAudioHardwareBadPropertySizeError; \
    *(T *)outData = (v); *outDataSize = sizeof(T); return noErr; } while (0)

#define RETURN_CFSTRING(s) RETURN_VALUE(CFStringRef, CFSTR(s))

static UInt32 copyIDs(const AudioObjectID *ids, UInt32 count, UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
  UInt32 n = inDataSize / sizeof(AudioObjectID);
  if (n > count) n = count;
  memcpy(outData, ids, n * sizeof(AudioObjectID));
  *outDataSize = n * sizeof(AudioObjectID);
  return n;
}

static bool scopeHasOutput(AudioObjectPropertyScope scope)
{
  return scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput;
}

static const AudioServerPlugInCustomPropertyInfo kCustomProps[] = {
  { kScreamConfigProperty, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
};

static Boolean Scream_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a)
{
  if (d != gDriverRef || !a) return false;
  switch (obj) {
    case kObjectID_PlugIn:
      switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyBundleID:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
          return true;
      }
      return false;

    case kObjectID_Device:
      switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kScreamConfigProperty:
          return true;
      }
      return false;

    case kObjectID_Stream:
      switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
          return true;
      }
      return false;

    case kObjectID_Volume:
      switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioControlPropertyScope:
        case kAudioControlPropertyElement:
        case kAudioLevelControlPropertyScalarValue:
        case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyDecibelRange:
        case kAudioLevelControlPropertyConvertScalarToDecibels:
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
          return true;
      }
      return false;

    case kObjectID_Mute:
      switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioControlPropertyScope:
        case kAudioControlPropertyElement:
        case kAudioBooleanControlPropertyValue:
          return true;
      }
      return false;
  }
  return false;
}

static OSStatus Scream_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out)
{
  if (d != gDriverRef || !a || !out) return kAudioHardwareIllegalOperationError;
  if (!Scream_HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
  *out = false;
  switch (obj) {
    case kObjectID_Device:
      *out = a->mSelector == kAudioDevicePropertyNominalSampleRate || a->mSelector == kScreamConfigProperty;
      break;
    case kObjectID_Stream:
      *out = a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat;
      break;
    case kObjectID_Volume:
      *out = a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue;
      break;
    case kObjectID_Mute:
      *out = a->mSelector == kAudioBooleanControlPropertyValue;
      break;
  }
  return noErr;
}

static OSStatus Scream_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                           UInt32 qs, const void *q, UInt32 *outSize)
{
  if (d != gDriverRef || !a || !outSize) return kAudioHardwareIllegalOperationError;
  if (!Scream_HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;

  switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioPlugInPropertyTranslateUIDToDevice:
      *outSize = sizeof(AudioObjectID); return noErr;
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioPlugInPropertyBundleID:
    case kAudioPlugInPropertyResourceBundle:
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:
      *outSize = sizeof(CFStringRef); return noErr;
    case kScreamConfigProperty:
      *outSize = sizeof(CFPropertyListRef); return noErr;
    case kAudioObjectPropertyCustomPropertyInfoList:
      *outSize = sizeof(kCustomProps); return noErr;
  }

  switch (obj) {
    case kObjectID_PlugIn:
      switch (a->mSelector) {
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
          *outSize = sizeof(AudioObjectID); return noErr;
      }
      break;

    case kObjectID_Device:
      switch (a->mSelector) {
        case kAudioObjectPropertyOwnedObjects:
          *outSize = scopeHasOutput(a->mScope) ? 3 * sizeof(AudioObjectID) : 0; return noErr;
        case kAudioDevicePropertyStreams:
          *outSize = scopeHasOutput(a->mScope) ? sizeof(AudioObjectID) : 0; return noErr;
        case kAudioObjectPropertyControlList:
          *outSize = 2 * sizeof(AudioObjectID); return noErr;
        case kAudioDevicePropertyRelatedDevices:
          *outSize = sizeof(AudioObjectID); return noErr;
        case kAudioDevicePropertyNominalSampleRate:
          *outSize = sizeof(Float64); return noErr;
        case kAudioDevicePropertyAvailableNominalSampleRates:
          *outSize = kNumSampleRates * sizeof(AudioValueRange); return noErr;
        case kAudioDevicePropertyPreferredChannelsForStereo:
          *outSize = 2 * sizeof(UInt32); return noErr;
        case kAudioDevicePropertyPreferredChannelLayout:
          *outSize = offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription); return noErr;
        default:
          *outSize = sizeof(UInt32); return noErr;
      }

    case kObjectID_Stream:
      switch (a->mSelector) {
        case kAudioObjectPropertyOwnedObjects:
          *outSize = 0; return noErr;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
          *outSize = sizeof(AudioStreamBasicDescription); return noErr;
        case kAudioStreamPropertyAvailableVirtualFormats:
          *outSize = kNumSampleRates * sizeof(AudioStreamRangedDescription); return noErr;
        case kAudioStreamPropertyAvailablePhysicalFormats:
          *outSize = kNumPhysicalFormats * sizeof(AudioStreamRangedDescription); return noErr;
        default:
          *outSize = sizeof(UInt32); return noErr;
      }

    case kObjectID_Volume:
    case kObjectID_Mute:
      switch (a->mSelector) {
        case kAudioObjectPropertyOwnedObjects:
          *outSize = 0; return noErr;
        case kAudioLevelControlPropertyDecibelRange:
          *outSize = sizeof(AudioValueRange); return noErr;
        case kAudioLevelControlPropertyScalarValue:
        case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyConvertScalarToDecibels:
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
          *outSize = sizeof(Float32); return noErr;
        default:
          *outSize = sizeof(UInt32); return noErr;
      }
  }
  return kAudioHardwareUnknownPropertyError;
}

static OSStatus Scream_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                       UInt32 qs, const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
  if (d != gDriverRef || !a || !outDataSize || !outData) return kAudioHardwareIllegalOperationError;
  if (!Scream_HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;

  switch (obj) {
    // ------------------------------------------------ plug-in
    case kObjectID_PlugIn:
      switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RETURN_VALUE(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: RETURN_VALUE(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: RETURN_VALUE(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: RETURN_CFSTRING(kManufacturer);
        case kAudioPlugInPropertyBundleID: RETURN_CFSTRING(kPlugIn_BundleID);
        case kAudioPlugInPropertyResourceBundle: RETURN_CFSTRING("");
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
          AudioObjectID ids[] = { kObjectID_Device };
          copyIDs(ids, 1, inDataSize, outDataSize, outData);
          return noErr;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
          if (qs != sizeof(CFStringRef) || !q) return kAudioHardwareBadPropertySizeError;
          CFStringRef uid = *(const CFStringRef *)q;
          RETURN_VALUE(AudioObjectID, CFStringCompare(uid, CFSTR(kDevice_UID), 0) == kCFCompareEqualTo
                                        ? kObjectID_Device : kAudioObjectUnknown);
        }
      }
      break;

    // ------------------------------------------------ device
    case kObjectID_Device:
      switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RETURN_VALUE(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: RETURN_VALUE(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: RETURN_VALUE(AudioObjectID, kObjectID_PlugIn);
        case kAudioObjectPropertyName: RETURN_CFSTRING(kDevice_Name);
        case kAudioObjectPropertyManufacturer: RETURN_CFSTRING(kManufacturer);
        case kAudioDevicePropertyDeviceUID: RETURN_CFSTRING(kDevice_UID);
        case kAudioDevicePropertyModelUID: RETURN_CFSTRING(kDevice_ModelUID);
        case kAudioDevicePropertyTransportType: RETURN_VALUE(UInt32, kAudioDeviceTransportTypeVirtual);
        case kAudioDevicePropertyClockDomain: RETURN_VALUE(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: RETURN_VALUE(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: {
          pthread_mutex_lock(&gStateMutex);
          UInt32 running = gIOClients > 0;
          pthread_mutex_unlock(&gStateMutex);
          RETURN_VALUE(UInt32, running);
        }
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
          RETURN_VALUE(UInt32, scopeHasOutput(a->mScope) ? 1 : 0);
        case kAudioDevicePropertyLatency: RETURN_VALUE(UInt32, 0);
        case kAudioDevicePropertySafetyOffset: RETURN_VALUE(UInt32, 0);
        case kAudioDevicePropertyIsHidden: RETURN_VALUE(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: RETURN_VALUE(UInt32, kRingBufferFrames);
        case kAudioDevicePropertyNominalSampleRate: {
          pthread_mutex_lock(&gStateMutex);
          Float64 r = gSampleRate;
          pthread_mutex_unlock(&gStateMutex);
          RETURN_VALUE(Float64, r);
        }
        case kAudioObjectPropertyOwnedObjects: {
          AudioObjectID ids[] = { kObjectID_Stream, kObjectID_Volume, kObjectID_Mute };
          if (scopeHasOutput(a->mScope)) copyIDs(ids, 3, inDataSize, outDataSize, outData);
          else *outDataSize = 0;
          return noErr;
        }
        case kAudioDevicePropertyStreams: {
          AudioObjectID ids[] = { kObjectID_Stream };
          if (scopeHasOutput(a->mScope)) copyIDs(ids, 1, inDataSize, outDataSize, outData);
          else *outDataSize = 0;
          return noErr;
        }
        case kAudioObjectPropertyControlList: {
          AudioObjectID ids[] = { kObjectID_Volume, kObjectID_Mute };
          copyIDs(ids, 2, inDataSize, outDataSize, outData);
          return noErr;
        }
        case kAudioDevicePropertyRelatedDevices: {
          AudioObjectID ids[] = { kObjectID_Device };
          copyIDs(ids, 1, inDataSize, outDataSize, outData);
          return noErr;
        }
        case kAudioDevicePropertyAvailableNominalSampleRates: {
          UInt32 n = inDataSize / sizeof(AudioValueRange);
          if (n > kNumSampleRates) n = kNumSampleRates;
          AudioValueRange *r = (AudioValueRange *)outData;
          for (UInt32 i = 0; i < n; i++) r[i].mMinimum = r[i].mMaximum = kSampleRates[i];
          *outDataSize = n * sizeof(AudioValueRange);
          return noErr;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: {
          if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
          ((UInt32 *)outData)[0] = 1;
          ((UInt32 *)outData)[1] = 2;
          *outDataSize = 2 * sizeof(UInt32);
          return noErr;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
          UInt32 size = offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription);
          if (inDataSize < size) return kAudioHardwareBadPropertySizeError;
          AudioChannelLayout *l = (AudioChannelLayout *)outData;
          memset(l, 0, size);
          l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
          l->mNumberChannelDescriptions = kChannels;
          l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
          l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
          *outDataSize = size;
          return noErr;
        }
        case kAudioObjectPropertyCustomPropertyInfoList: {
          if (inDataSize < sizeof(kCustomProps)) return kAudioHardwareBadPropertySizeError;
          memcpy(outData, kCustomProps, sizeof(kCustomProps));
          *outDataSize = sizeof(kCustomProps);
          return noErr;
        }
        case kScreamConfigProperty: {
          os_unfair_lock_lock(&gConfigLock);
          scream_config_t c = gConfig;
          os_unfair_lock_unlock(&gConfigLock);
          if (inDataSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
          *(CFPropertyListRef *)outData = configCopyDictionary(&c);   // caller releases
          *outDataSize = sizeof(CFPropertyListRef);
          return noErr;
        }
      }
      break;

    // ------------------------------------------------ stream
    case kObjectID_Stream:
      switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RETURN_VALUE(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: RETURN_VALUE(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: RETURN_VALUE(AudioObjectID, kObjectID_Device);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return noErr;
        case kAudioStreamPropertyIsActive: RETURN_VALUE(UInt32, 1);
        case kAudioStreamPropertyDirection: RETURN_VALUE(UInt32, 0);   // output
        case kAudioStreamPropertyTerminalType: RETURN_VALUE(UInt32, kAudioStreamTerminalTypeSpeaker);
        case kAudioStreamPropertyStartingChannel: RETURN_VALUE(UInt32, 1);
        case kAudioStreamPropertyLatency: RETURN_VALUE(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
          if (inDataSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
          pthread_mutex_lock(&gStateMutex);
          if (a->mSelector == kAudioStreamPropertyPhysicalFormat)
            fillPhysical((AudioStreamBasicDescription *)outData, gSampleRate, gBits);
          else
            fillFormat((AudioStreamBasicDescription *)outData, gSampleRate);
          pthread_mutex_unlock(&gStateMutex);
          *outDataSize = sizeof(AudioStreamBasicDescription);
          return noErr;
        }
        case kAudioStreamPropertyAvailablePhysicalFormats: {
          UInt32 n = inDataSize / sizeof(AudioStreamRangedDescription);
          if (n > kNumPhysicalFormats) n = kNumPhysicalFormats;
          AudioStreamRangedDescription *r = (AudioStreamRangedDescription *)outData;
          for (UInt32 i = 0; i < n; i++) {
            Float64 rate = kSampleRates[i / kNumBitDepths];
            fillPhysical(&r[i].mFormat, rate, kBitDepths[i % kNumBitDepths]);
            r[i].mSampleRateRange.mMinimum = r[i].mSampleRateRange.mMaximum = rate;
          }
          *outDataSize = n * sizeof(AudioStreamRangedDescription);
          return noErr;
        }
        case kAudioStreamPropertyAvailableVirtualFormats: {
          UInt32 n = inDataSize / sizeof(AudioStreamRangedDescription);
          if (n > kNumSampleRates) n = kNumSampleRates;
          AudioStreamRangedDescription *r = (AudioStreamRangedDescription *)outData;
          for (UInt32 i = 0; i < n; i++) {
            fillFormat(&r[i].mFormat, kSampleRates[i]);
            r[i].mSampleRateRange.mMinimum = r[i].mSampleRateRange.mMaximum = kSampleRates[i];
          }
          *outDataSize = n * sizeof(AudioStreamRangedDescription);
          return noErr;
        }
      }
      break;

    // ------------------------------------------------ volume
    case kObjectID_Volume:
      switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RETURN_VALUE(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: RETURN_VALUE(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: RETURN_VALUE(AudioObjectID, kObjectID_Device);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return noErr;
        case kAudioControlPropertyScope: RETURN_VALUE(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: RETURN_VALUE(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioLevelControlPropertyScalarValue: RETURN_VALUE(Float32, atomic_load(&gVolume));
        case kAudioLevelControlPropertyDecibelValue: RETURN_VALUE(Float32, volumeToDB(atomic_load(&gVolume)));
        case kAudioLevelControlPropertyDecibelRange: {
          AudioValueRange r = { kVolume_MinDB, kVolume_MaxDB };
          RETURN_VALUE(AudioValueRange, r);
        }
        case kAudioLevelControlPropertyConvertScalarToDecibels: {
          if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
          Float32 v = *(Float32 *)outData;
          if (v < 0) v = 0;
          if (v > 1) v = 1;
          RETURN_VALUE(Float32, volumeToDB(v));
        }
        case kAudioLevelControlPropertyConvertDecibelsToScalar: {
          if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
          RETURN_VALUE(Float32, dbToVolume(*(Float32 *)outData));
        }
      }
      break;

    // ------------------------------------------------ mute
    case kObjectID_Mute:
      switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RETURN_VALUE(AudioClassID, kAudioBooleanControlClassID);
        case kAudioObjectPropertyClass: RETURN_VALUE(AudioClassID, kAudioMuteControlClassID);
        case kAudioObjectPropertyOwner: RETURN_VALUE(AudioObjectID, kObjectID_Device);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return noErr;
        case kAudioControlPropertyScope: RETURN_VALUE(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: RETURN_VALUE(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioBooleanControlPropertyValue: RETURN_VALUE(UInt32, atomic_load(&gMute) ? 1 : 0);
      }
      break;
  }
  return kAudioHardwareUnknownPropertyError;
}

static void notify(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
  AudioObjectPropertyAddress a[2] = {
    { sel, scope, kAudioObjectPropertyElementMain },
  };
  UInt32 n = 1;
  if (sel == kAudioLevelControlPropertyScalarValue) {
    a[1] = (AudioObjectPropertyAddress){ kAudioLevelControlPropertyDecibelValue, scope, kAudioObjectPropertyElementMain };
    n = 2;
  }
  gHost->PropertiesChanged(gHost, obj, n, a);
}

// Sample rate and width change together: the HAL stops IO, calls
// PerformDeviceConfigurationChange with this action, and restarts IO.
// bits == 0 keeps the current width.
static OSStatus requestFormat(Float64 rate, UInt32 bits)
{
  pthread_mutex_lock(&gStateMutex);
  Float64 curRate = gSampleRate;
  UInt32 curBits = gBits;
  pthread_mutex_unlock(&gStateMutex);
  if (bits == 0) bits = curBits;
  if (!validRate(rate) || !validBits(bits)) return kAudioHardwareIllegalOperationError;
  if (rate == curRate && bits == curBits) return noErr;
  UInt64 action = ((UInt64)lround(rate) << 8) | bits;
  dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
    gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, action, NULL);
  });
  return noErr;
}

static OSStatus Scream_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                       UInt32 qs, const void *q, UInt32 inDataSize, const void *inData)
{
  if (d != gDriverRef || !a || !inData) return kAudioHardwareIllegalOperationError;
  Boolean settable = false;
  OSStatus st = Scream_IsPropertySettable(d, obj, pid, a, &settable);
  if (st != noErr) return st;
  if (!settable) return kAudioHardwareUnsupportedOperationError;

  switch (obj) {
    case kObjectID_Device:
      if (a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return requestFormat(*(const Float64 *)inData, 0);
      }
      if (a->mSelector == kScreamConfigProperty) {
        if (inDataSize != sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
        CFPropertyListRef pl = *(const CFPropertyListRef *)inData;
        if (!pl || CFGetTypeID(pl) != CFDictionaryGetTypeID()) return kAudioHardwareIllegalOperationError;
        os_unfair_lock_lock(&gConfigLock);
        bool ok = configMerge(&gConfig, (CFDictionaryRef)pl);
        os_unfair_lock_unlock(&gConfigLock);
        if (!ok) return kAudioHardwareIllegalOperationError;
        atomic_fetch_add_explicit(&gConfigGeneration, 1, memory_order_release);
        configStore();
        notify(kObjectID_Device, kScreamConfigProperty, kAudioObjectPropertyScopeGlobal);
        // A new sample width is a new physical format.
        os_unfair_lock_lock(&gConfigLock);
        UInt32 bits = (UInt32)gConfig.bits;
        os_unfair_lock_unlock(&gConfigLock);
        pthread_mutex_lock(&gStateMutex);
        Float64 rate = gSampleRate;
        pthread_mutex_unlock(&gStateMutex);
        return requestFormat(rate, bits);
      }
      break;

    case kObjectID_Stream: {
      if (inDataSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
      const AudioStreamBasicDescription *f = (const AudioStreamBasicDescription *)inData;
      if (f->mFormatID != kAudioFormatLinearPCM || f->mChannelsPerFrame != kChannels)
        return kAudioDeviceUnsupportedFormatError;
      if (a->mSelector == kAudioStreamPropertyVirtualFormat) {
        if (f->mBitsPerChannel != 32 || !(f->mFormatFlags & kAudioFormatFlagIsFloat))
          return kAudioDeviceUnsupportedFormatError;
        return requestFormat(f->mSampleRate, 0);
      }
      // Physical format, e.g. from the Format menu in Audio MIDI Setup.
      if ((f->mFormatFlags & kAudioFormatFlagIsFloat) || !validBits(f->mBitsPerChannel))
        return kAudioDeviceUnsupportedFormatError;
      return requestFormat(f->mSampleRate, f->mBitsPerChannel);
    }

    case kObjectID_Volume: {
      if (inDataSize != sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
      Float32 v = *(const Float32 *)inData;
      if (a->mSelector == kAudioLevelControlPropertyDecibelValue) v = dbToVolume(v);
      if (v < 0) v = 0;
      if (v > 1) v = 1;
      if (atomic_exchange(&gVolume, v) != v)
        notify(kObjectID_Volume, kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeOutput);
      return noErr;
    }

    case kObjectID_Mute: {
      if (inDataSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
      bool m = *(const UInt32 *)inData != 0;
      if (atomic_exchange(&gMute, m) != m)
        notify(kObjectID_Mute, kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeOutput);
      return noErr;
    }
  }
  return kAudioHardwareUnknownPropertyError;
}

// ---------------------------------------------------------------- IO

static OSStatus Scream_StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
  if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
  pthread_mutex_lock(&gStateMutex);
  if (gIOClients == 0) {
    pthread_mutex_lock(&gIOMutex);
    gTimestampCount = 0;
    gAnchorHostTime = mach_absolute_time();
    pthread_mutex_unlock(&gIOMutex);
  }
  gIOClients++;
  pthread_mutex_unlock(&gStateMutex);
  return noErr;
}

static OSStatus Scream_StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
  if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
  pthread_mutex_lock(&gStateMutex);
  if (gIOClients > 0) gIOClients--;
  pthread_mutex_unlock(&gStateMutex);
  return noErr;
}

static OSStatus Scream_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                                        Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed)
{
  if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
  pthread_mutex_lock(&gIOMutex);
  UInt64 now = mach_absolute_time();
  Float64 ticksPerRing = gHostTicksPerFrame * (Float64)kRingBufferFrames;
  UInt64 next = gAnchorHostTime + (UInt64)((Float64)(gTimestampCount + 1) * ticksPerRing);
  if (next <= now) gTimestampCount++;
  *outSampleTime = (Float64)(gTimestampCount * kRingBufferFrames);
  *outHostTime = gAnchorHostTime + (UInt64)((Float64)gTimestampCount * ticksPerRing);
  *outSeed = 1;
  pthread_mutex_unlock(&gIOMutex);
  return noErr;
}

static OSStatus Scream_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                         Boolean *outWillDo, Boolean *outInPlace)
{
  if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
  *outWillDo = op == kAudioServerPlugInIOOperationProcessMix || op == kAudioServerPlugInIOOperationWriteMix;
  *outInPlace = true;
  return noErr;
}

static OSStatus Scream_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                        UInt32 frames, const AudioServerPlugInIOCycleInfo *info)
{
  return d == gDriverRef && dev == kObjectID_Device ? noErr : kAudioHardwareBadObjectError;
}

static OSStatus Scream_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream, UInt32 client,
                                     UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info,
                                     void *mainBuffer, void *secondaryBuffer)
{
  if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
  if (!mainBuffer) return noErr;
  if (op == kAudioServerPlugInIOOperationProcessMix)
    processMix((Float32 *)mainBuffer, frames);
  else if (op == kAudioServerPlugInIOOperationWriteMix)
    txWrite((const uint8_t *)mainBuffer, frames);
  return noErr;
}

static OSStatus Scream_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                      UInt32 frames, const AudioServerPlugInIOCycleInfo *info)
{
  return d == gDriverRef && dev == kObjectID_Device ? noErr : kAudioHardwareBadObjectError;
}
