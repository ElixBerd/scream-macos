// scream-sender: stream macOS system audio to Scream receivers on the LAN.
//
// Capture uses a Core Audio process tap (macOS 14.2+): a global stereo tap
// is wrapped in a private aggregate device, and the aggregate device's IOProc
// hands us every HAL buffer on the real-time audio thread. There is no kernel
// extension or HAL plug-in to install, and the system output keeps working.
//
// Samples are converted to LSB PCM and pushed out as Scream UDP packets
// (5 byte header + PCM payload) straight from the IOProc with a non-blocking
// sendto(), so the only added latency is the HAL buffer plus packetization.
//
// Licensed under MS-PL, like the rest of Scream.

#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_MULTICAST_GROUP "239.255.77.77"
#define DEFAULT_PORT 4010
#define HEADER_SIZE 5
#define DEFAULT_PAYLOAD 1152
#define MAX_PAYLOAD 1152
#define CHANNELS 2
#define CHANNEL_MASK_STEREO 0x0003 // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT

// ---------------------------------------------------------------- options

static struct {
  const char *dest;
  int port;
  const char *iface;
  int ttl;
  int bits;
  int payload;
  UInt32 bufferFrames;
  BOOL mute;
  int verbose;
} opt = {
  .dest = DEFAULT_MULTICAST_GROUP,
  .port = DEFAULT_PORT,
  .iface = NULL,
  .ttl = 1,
  .bits = 16,
  .payload = DEFAULT_PAYLOAD,
  .bufferFrames = 128,
  .mute = NO,
  .verbose = 0,
};

// ---------------------------------------------------------------- state

typedef struct {
  int sock;
  struct sockaddr_in addr;

  // Touched only on the IO thread once streaming has started.
  uint8_t packet[HEADER_SIZE + MAX_PAYLOAD];
  int fill;          // bytes of PCM currently in packet
  int payload;       // bytes of PCM per packet (multiple of frame size)
  int bytesPerSample;
  uint32_t rng;      // dither PRNG state

  // Counters read by the stats timer.
  _Atomic uint64_t packets;
  _Atomic uint64_t sendErrors;
  _Atomic uint64_t callbacks;
  _Atomic uint64_t frames;
} sender_t;

static sender_t S;

static AudioObjectID gTap = kAudioObjectUnknown;
static AudioObjectID gAggregate = kAudioObjectUnknown;
static AudioDeviceIOProcID gProc = NULL;
static dispatch_queue_t gControlQueue;

// ---------------------------------------------------------------- helpers

static void fail(const char *what, OSStatus st)
{
  char code[8] = {0};
  UInt32 be = CFSwapInt32HostToBig((UInt32)st);
  memcpy(code, &be, 4);
  BOOL printable = YES;
  for (int i = 0; i < 4; i++) if (code[i] < 32 || code[i] > 126) printable = NO;
  if (printable) fprintf(stderr, "%s failed: '%s' (%d)\n", what, code, (int)st);
  else fprintf(stderr, "%s failed: %d\n", what, (int)st);
}

static AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel)
{
  return (AudioObjectPropertyAddress){ sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
}

static AudioObjectID defaultOutputDevice(void)
{
  AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyDefaultOutputDevice);
  AudioObjectID dev = kAudioObjectUnknown;
  UInt32 size = sizeof(dev);
  OSStatus st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &dev);
  if (st != noErr) { fail("get default output device", st); return kAudioObjectUnknown; }
  return dev;
}

static NSString *deviceUID(AudioObjectID dev)
{
  AudioObjectPropertyAddress a = addr(kAudioDevicePropertyDeviceUID);
  CFStringRef uid = NULL;
  UInt32 size = sizeof(uid);
  OSStatus st = AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &uid);
  if (st != noErr) { fail("get device UID", st); return nil; }
  return (__bridge_transfer NSString *)uid;
}

static NSString *deviceName(AudioObjectID dev)
{
  AudioObjectPropertyAddress a = addr(kAudioObjectPropertyName);
  CFStringRef name = NULL;
  UInt32 size = sizeof(name);
  if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &name) != noErr) return @"?";
  return (__bridge_transfer NSString *)name;
}

static Float64 nominalRate(AudioObjectID dev)
{
  AudioObjectPropertyAddress a = addr(kAudioDevicePropertyNominalSampleRate);
  Float64 rate = 0;
  UInt32 size = sizeof(rate);
  AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &rate);
  return rate;
}

// Scream rate byte: bit 7 selects the 44.1 kHz base (else 48 kHz), the low
// bits are the multiplier.
static int rateByte(Float64 rate)
{
  int r = (int)lround(rate);
  if (r > 0 && r % 44100 == 0) return 0x80 | (r / 44100);
  if (r > 0 && r % 48000 == 0) return r / 48000;
  return -1;
}

// ---------------------------------------------------------------- IO path

static inline void flushPacket(void)
{
  ssize_t n = sendto(S.sock, S.packet, HEADER_SIZE + S.fill, MSG_DONTWAIT,
                     (struct sockaddr *)&S.addr, sizeof(S.addr));
  if (n < 0) atomic_fetch_add_explicit(&S.sendErrors, 1, memory_order_relaxed);
  else atomic_fetch_add_explicit(&S.packets, 1, memory_order_relaxed);
  S.fill = 0;
}

static inline void putSample(float x)
{
  uint8_t *p = S.packet + HEADER_SIZE + S.fill;
  if (S.bytesPerSample == 2) {
    // TPDF dither (+-1 LSB) before quantizing to 16 bit.
    S.rng = S.rng * 1664525u + 1013904223u;
    float r1 = (float)(S.rng >> 8) * (1.0f / 16777216.0f);
    S.rng = S.rng * 1664525u + 1013904223u;
    float r2 = (float)(S.rng >> 8) * (1.0f / 16777216.0f);
    float v = x * 32767.0f + (r1 - r2);
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    int16_t s = (int16_t)lrintf(v);
    p[0] = (uint8_t)s; p[1] = (uint8_t)(s >> 8);
  } else {
    double v = (double)x * 2147483647.0;
    if (v > 2147483647.0) v = 2147483647.0;
    if (v < -2147483648.0) v = -2147483648.0;
    int32_t s = (int32_t)lrint(v);
    if (S.bytesPerSample == 3) {
      p[0] = (uint8_t)(s >> 8); p[1] = (uint8_t)(s >> 16); p[2] = (uint8_t)(s >> 24);
    } else {
      p[0] = (uint8_t)s; p[1] = (uint8_t)(s >> 8); p[2] = (uint8_t)(s >> 16); p[3] = (uint8_t)(s >> 24);
    }
  }
  S.fill += S.bytesPerSample;
}

static OSStatus ioProc(AudioObjectID dev, const AudioTimeStamp *now,
                       const AudioBufferList *in, const AudioTimeStamp *inTime,
                       AudioBufferList *out, const AudioTimeStamp *outTime, void *ctx)
{
  (void)dev; (void)now; (void)inTime; (void)out; (void)outTime; (void)ctx;
  if (!in || in->mNumberBuffers == 0) return noErr;

  // The tap shows up as the aggregate device's input. Accept both one
  // interleaved stereo buffer and two mono (non-interleaved) buffers.
  const float *L, *R;
  UInt32 frames, stride;
  const AudioBuffer *b0 = &in->mBuffers[0];
  if (!b0->mData) return noErr;
  if (b0->mNumberChannels >= 2) {
    stride = b0->mNumberChannels;
    frames = b0->mDataByteSize / (sizeof(float) * stride);
    L = (const float *)b0->mData;
    R = L + 1;
  } else if (in->mNumberBuffers >= 2 && in->mBuffers[1].mData) {
    stride = 1;
    frames = b0->mDataByteSize / sizeof(float);
    L = (const float *)b0->mData;
    R = (const float *)in->mBuffers[1].mData;
  } else {
    stride = 1;
    frames = b0->mDataByteSize / sizeof(float);
    L = R = (const float *)b0->mData;
  }

  for (UInt32 i = 0; i < frames; i++) {
    putSample(L[i * stride]);
    putSample(R[i * stride]);
    if (S.fill >= S.payload) flushPacket();
  }

  atomic_fetch_add_explicit(&S.callbacks, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&S.frames, frames, memory_order_relaxed);
  return noErr;
}

// ---------------------------------------------------------------- setup

static OSStatus aggregateChanged(AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *a, void *ctx);

static void teardown(void)
{
  if (gAggregate != kAudioObjectUnknown) {
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyNominalSampleRate);
    AudioObjectRemovePropertyListener(gAggregate, &a, aggregateChanged, NULL);
  }
  if (gAggregate != kAudioObjectUnknown && gProc) {
    AudioDeviceStop(gAggregate, gProc);
    AudioDeviceDestroyIOProcID(gAggregate, gProc);
  }
  gProc = NULL;
  if (gAggregate != kAudioObjectUnknown) AudioHardwareDestroyAggregateDevice(gAggregate);
  gAggregate = kAudioObjectUnknown;
  if (gTap != kAudioObjectUnknown) AudioHardwareDestroyProcessTap(gTap);
  gTap = kAudioObjectUnknown;
}

static BOOL setup(void)
{
  AudioObjectID out = defaultOutputDevice();
  if (out == kAudioObjectUnknown) return NO;
  NSString *outUID = deviceUID(out);
  if (!outUID) return NO;

  // Global stereo tap of everything except nothing, i.e. all processes.
  CATapDescription *desc = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
  desc.name = @"Scream sender tap";
  desc.privateTap = YES;
  desc.muteBehavior = opt.mute ? CATapMuted : CATapUnmuted;

  OSStatus st = AudioHardwareCreateProcessTap(desc, &gTap);
  if (st != noErr) {
    fail("AudioHardwareCreateProcessTap", st);
    fprintf(stderr, "Allow \"System Audio Recording\" for your terminal in "
                    "System Settings > Privacy & Security, then run again.\n");
    gTap = kAudioObjectUnknown;
    return NO;
  }

  AudioObjectPropertyAddress a = addr(kAudioTapPropertyFormat);
  AudioStreamBasicDescription fmt = {0};
  UInt32 size = sizeof(fmt);
  st = AudioObjectGetPropertyData(gTap, &a, 0, NULL, &size, &fmt);
  if (st != noErr) { fail("get tap format", st); teardown(); return NO; }
  if (fmt.mFormatID != kAudioFormatLinearPCM || !(fmt.mFormatFlags & kAudioFormatFlagIsFloat) || fmt.mBitsPerChannel != 32) {
    fprintf(stderr, "Unexpected tap format (id %u, flags 0x%x, %u bits)\n",
            (unsigned)fmt.mFormatID, (unsigned)fmt.mFormatFlags, (unsigned)fmt.mBitsPerChannel);
    teardown();
    return NO;
  }

  NSString *aggUID = [[NSUUID UUID] UUIDString];
  NSDictionary *aggDesc = @{
    @kAudioAggregateDeviceNameKey: @"Scream sender",
    @kAudioAggregateDeviceUIDKey: aggUID,
    @kAudioAggregateDeviceMainSubDeviceKey: outUID,
    @kAudioAggregateDeviceIsPrivateKey: @YES,
    @kAudioAggregateDeviceIsStackedKey: @NO,
    @kAudioAggregateDeviceTapAutoStartKey: @YES,
    @kAudioAggregateDeviceSubDeviceListKey: @[ @{ @kAudioSubDeviceUIDKey: outUID } ],
    @kAudioAggregateDeviceTapListKey: @[ @{
      @kAudioSubTapDriftCompensationKey: @YES,
      @kAudioSubTapUIDKey: desc.UUID.UUIDString,
    } ],
  };
  st = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggDesc, &gAggregate);
  if (st != noErr) { fail("AudioHardwareCreateAggregateDevice", st); gAggregate = kAudioObjectUnknown; teardown(); return NO; }

  // Ask for a small HAL buffer: this is the main latency knob on our side.
  AudioValueRange range = {0};
  a = addr(kAudioDevicePropertyBufferFrameSizeRange);
  size = sizeof(range);
  UInt32 frames = opt.bufferFrames;
  if (AudioObjectGetPropertyData(gAggregate, &a, 0, NULL, &size, &range) == noErr) {
    if (frames < range.mMinimum) frames = (UInt32)range.mMinimum;
    if (frames > range.mMaximum) frames = (UInt32)range.mMaximum;
  }
  a = addr(kAudioDevicePropertyBufferFrameSize);
  st = AudioObjectSetPropertyData(gAggregate, &a, 0, NULL, sizeof(frames), &frames);
  if (st != noErr) fail("set buffer frame size", st);
  size = sizeof(frames);
  AudioObjectGetPropertyData(gAggregate, &a, 0, NULL, &size, &frames);

  Float64 rate = nominalRate(gAggregate);
  if (rate <= 0) rate = fmt.mSampleRate;
  int rb = rateByte(rate);
  if (rb < 0) {
    fprintf(stderr, "Sample rate %.0f Hz cannot be expressed in the Scream header; "
                    "set the output device to 44.1/48/88.2/96 kHz in Audio MIDI Setup.\n", rate);
    teardown();
    return NO;
  }

  int frameBytes = CHANNELS * S.bytesPerSample;
  S.payload = (opt.payload / frameBytes) * frameBytes;
  if (S.payload < frameBytes) S.payload = frameBytes;
  S.fill = 0;
  S.packet[0] = (uint8_t)rb;
  S.packet[1] = (uint8_t)opt.bits;
  S.packet[2] = CHANNELS;
  S.packet[3] = CHANNEL_MASK_STEREO & 0xff;
  S.packet[4] = (CHANNEL_MASK_STEREO >> 8) & 0xff;

  st = AudioDeviceCreateIOProcID(gAggregate, ioProc, NULL, &gProc);
  if (st != noErr) { fail("AudioDeviceCreateIOProcID", st); gProc = NULL; teardown(); return NO; }
  st = AudioDeviceStart(gAggregate, gProc);
  if (st != noErr) { fail("AudioDeviceStart", st); teardown(); return NO; }

  a = addr(kAudioDevicePropertyNominalSampleRate);
  AudioObjectAddPropertyListener(gAggregate, &a, aggregateChanged, NULL);

  double pktMs = 1000.0 * S.payload / frameBytes / rate;
  double bufMs = 1000.0 * frames / rate;
  fprintf(stderr, "Capturing \"%s\" at %.0f Hz -> %s:%d, %d bit stereo, "
                  "HAL buffer %u frames (%.2f ms), packet %d bytes (%.2f ms)%s\n",
          deviceName(out).UTF8String, rate, opt.dest, opt.port, opt.bits,
          (unsigned)frames, bufMs, S.payload, pktMs, opt.mute ? ", local output muted" : "");
  return YES;
}

static void restart(const char *why)
{
  dispatch_async(gControlQueue, ^{
    fprintf(stderr, "%s, restarting capture\n", why);
    teardown();
    if (!setup()) fprintf(stderr, "Capture restart failed; waiting for the next device change\n");
  });
}

static OSStatus aggregateChanged(AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *a, void *ctx)
{
  (void)obj; (void)n; (void)a; (void)ctx;
  restart("Sample rate changed");
  return noErr;
}

static OSStatus defaultOutputChanged(AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *a, void *ctx)
{
  (void)obj; (void)n; (void)a; (void)ctx;
  restart("Default output device changed");
  return noErr;
}

// ---------------------------------------------------------------- network

static BOOL openSocket(void)
{
  S.sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (S.sock < 0) { perror("socket"); return NO; }

  memset(&S.addr, 0, sizeof(S.addr));
  S.addr.sin_family = AF_INET;
  S.addr.sin_port = htons(opt.port);
  if (inet_pton(AF_INET, opt.dest, &S.addr.sin_addr) != 1) {
    fprintf(stderr, "Invalid destination address: %s\n", opt.dest);
    return NO;
  }

  // Low-delay DSCP (EF) so Wi-Fi WMM puts us in the voice queue.
  int tos = 0xb8;
  setsockopt(S.sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
  int svc = NET_SERVICE_TYPE_VO;
  setsockopt(S.sock, SOL_SOCKET, SO_NET_SERVICE_TYPE, &svc, sizeof(svc));

  if (IN_MULTICAST(ntohl(S.addr.sin_addr.s_addr))) {
    unsigned char ttl = (unsigned char)opt.ttl;
    setsockopt(S.sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    unsigned char loop = 1;
    setsockopt(S.sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    if (opt.iface) {
      struct in_addr ifa;
      if (inet_pton(AF_INET, opt.iface, &ifa) != 1) {
        fprintf(stderr, "Invalid interface address: %s\n", opt.iface);
        return NO;
      }
      if (setsockopt(S.sock, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa)) != 0) {
        perror("IP_MULTICAST_IF");
        return NO;
      }
    }
  }
  return YES;
}

// ---------------------------------------------------------------- main

static void usage(const char *argv0)
{
  fprintf(stderr,
    "Usage: %s [options]\n"
    "Streams macOS system audio to Scream receivers (macOS 14.2+).\n\n"
    "  -u ADDR    unicast destination IP (default: multicast %s)\n"
    "  -g ADDR    multicast group (default: %s)\n"
    "  -p PORT    UDP port (default: %d)\n"
    "  -i ADDR    local interface IP for multicast\n"
    "  -t TTL     multicast TTL (default: 1)\n"
    "  -b BITS    sample width 16, 24 or 32 (default: 16)\n"
    "  -s BYTES   PCM bytes per packet, max %d (default: %d)\n"
    "  -f FRAMES  requested HAL buffer size in frames (default: 128)\n"
    "  -m         mute local output while streaming\n"
    "  -v         print stats every second\n"
    "  -h         this help\n",
    argv0, DEFAULT_MULTICAST_GROUP, DEFAULT_MULTICAST_GROUP, DEFAULT_PORT, MAX_PAYLOAD, DEFAULT_PAYLOAD);
}

int main(int argc, char *argv[])
{
  @autoreleasepool {
    int c;
    while ((c = getopt(argc, argv, "u:g:p:i:t:b:s:f:mvh")) != -1) {
      switch (c) {
        case 'u': case 'g': opt.dest = optarg; break;
        case 'p': opt.port = atoi(optarg); break;
        case 'i': opt.iface = optarg; break;
        case 't': opt.ttl = atoi(optarg); break;
        case 'b': opt.bits = atoi(optarg); break;
        case 's': opt.payload = atoi(optarg); break;
        case 'f': opt.bufferFrames = (UInt32)atoi(optarg); break;
        case 'm': opt.mute = YES; break;
        case 'v': opt.verbose++; break;
        default: usage(argv[0]); return c == 'h' ? 0 : 1;
      }
    }
    if (opt.bits != 16 && opt.bits != 24 && opt.bits != 32) { usage(argv[0]); return 1; }
    if (opt.payload <= 0 || opt.payload > MAX_PAYLOAD) { usage(argv[0]); return 1; }
    if (opt.port <= 0 || opt.port > 65535) { usage(argv[0]); return 1; }
    if (opt.bufferFrames < 1) opt.bufferFrames = 1;
    S.bytesPerSample = opt.bits / 8;
    S.rng = 0x12345678u;

    if (@available(macOS 14.2, *)) {} else {
      fprintf(stderr, "macOS 14.2 or newer is required for system audio capture.\n");
      return 1;
    }

    if (!openSocket()) return 1;

    gControlQueue = dispatch_queue_create("scream.control", DISPATCH_QUEUE_SERIAL);
    __block BOOL ok = NO;
    dispatch_sync(gControlQueue, ^{ ok = setup(); });
    if (!ok) return 1;

    AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyDefaultOutputDevice);
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &a, defaultOutputChanged, NULL);

    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    void (^quit)(void) = ^{
      dispatch_sync(gControlQueue, ^{ teardown(); });
      fprintf(stderr, "\nStopped.\n");
      exit(0);
    };
    dispatch_source_t sigint = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
    dispatch_source_t sigterm = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(sigint, quit);
    dispatch_source_set_event_handler(sigterm, quit);
    dispatch_resume(sigint);
    dispatch_resume(sigterm);

    if (opt.verbose) {
      dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
      dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
      __block uint64_t lp = 0, le = 0, lc = 0, lf = 0;
      dispatch_source_set_event_handler(timer, ^{
        uint64_t p = atomic_load(&S.packets), e = atomic_load(&S.sendErrors);
        uint64_t cb = atomic_load(&S.callbacks), f = atomic_load(&S.frames);
        fprintf(stderr, "packets/s %llu  errors/s %llu  callbacks/s %llu  frames/s %llu\n",
                p - lp, e - le, cb - lc, f - lf);
        lp = p; le = e; lc = cb; lf = f;
      });
      dispatch_resume(timer);
    }

    dispatch_main();
  }
}
