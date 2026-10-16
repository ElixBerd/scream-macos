// Scream: menu bar app for the Scream Audio output device.
//
// Turns streaming on and off (by switching the system output to Scream Audio
// and back), picks the receiver, latency and quality, and starts at login.
// All network settings live in the driver; this app only flips them.
//
// Licensed under MS-PL, like the rest of Scream.

#import <Cocoa/Cocoa.h>
#import <ServiceManagement/ServiceManagement.h>
#include <arpa/inet.h>
#include "../common/scream_device.h"

static NSString *const kPrevOutputKey = @"PreviousOutputUID";
static NSString *const kRecentReceiversKey = @"RecentReceivers";
static NSString *const kFirstRunDoneKey = @"FirstRunDone";

typedef struct { const char *title; int payload; } latency_preset_t;
static const latency_preset_t kLatencyPresets[] = {
  { "Lowest", 288 },
  { "Low", 576 },
  { "Standard (most compatible)", 1152 },
};

static const int kBitDepths[] = { 16, 24, 32 };
static const Float64 kRates[] = { 44100, 48000, 88200, 96000 };

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

@interface ScreamApp : NSObject <NSApplicationDelegate, NSMenuDelegate>
@property (strong) NSStatusItem *item;
@end

@implementation ScreamApp

// ---------------------------------------------------------------- lifecycle

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
  self.item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
  self.item.menu = [[NSMenu alloc] init];
  self.item.menu.delegate = self;
  self.item.menu.autoenablesItems = NO;

  // Follow output switches made elsewhere (Sound settings, Control Center)
  // and the driver appearing after install.
  AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice,
                                   kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
  __weak ScreamApp *weakSelf = self;
  AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &a, dispatch_get_main_queue(),
                                      ^(UInt32 n, const AudioObjectPropertyAddress *addrs) { [weakSelf refreshIcon]; });
  a.mSelector = kAudioHardwarePropertyDevices;
  AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &a, dispatch_get_main_queue(),
                                      ^(UInt32 n, const AudioObjectPropertyAddress *addrs) { [weakSelf refreshIcon]; });

  NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
  if (![ud boolForKey:kFirstRunDoneKey]) {
    // Installing Scream means you want to stream: start at login and switch
    // the output over once. Both are one click to undo in the menu.
    [ud setBool:YES forKey:kFirstRunDoneKey];
    [self setLoginItemEnabled:YES quiet:YES];
    if (scream_find_device() != kAudioObjectUnknown) [self setStreaming:YES];
  }
  [self refreshIcon];
}

// ---------------------------------------------------------------- state

- (AudioObjectID)device { return scream_find_device(); }

- (BOOL)isStreaming
{
  AudioObjectID dev = [self device];
  return dev != kAudioObjectUnknown && scream_default_output() == dev;
}

- (void)refreshIcon
{
  NSStatusBarButton *b = self.item.button;
  AudioObjectID dev = [self device];
  NSString *symbol = dev == kAudioObjectUnknown ? @"exclamationmark.triangle" : @"dot.radiowaves.left.and.right";
  NSImage *img = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:@"Scream"];
  img.template = YES;
  b.image = img;
  b.appearsDisabled = ![self isStreaming];
  b.toolTip = [self isStreaming] ? [NSString stringWithFormat:@"Scream: streaming to %@", [self destinationText]]
                                 : @"Scream: not streaming";
}

- (NSString *)configAddress:(CFDictionaryRef)cfg
{
  CFStringRef s = cfg ? CFDictionaryGetValue(cfg, CFSTR("address")) : NULL;
  return s ? (__bridge NSString *)s : @SCREAM_DEFAULT_GROUP;
}

- (BOOL)isMulticast:(NSString *)addr
{
  struct in_addr ia;
  return inet_pton(AF_INET, addr.UTF8String, &ia) == 1 && IN_MULTICAST(ntohl(ia.s_addr));
}

- (NSString *)destinationText
{
  CFDictionaryRef cfg = scream_copy_config([self device]);
  NSString *addr = [self configAddress:cfg];
  int port = scream_config_int(cfg, CFSTR("port"));
  if (cfg) CFRelease(cfg);
  if ([self isMulticast:addr]) return port == 4010 ? @"all receivers" : [NSString stringWithFormat:@"all receivers, port %d", port];
  return port == 4010 ? addr : [NSString stringWithFormat:@"%@:%d", addr, port];
}

// ---------------------------------------------------------------- menu

- (NSMenuItem *)add:(NSMenu *)menu title:(NSString *)title action:(SEL)action tag:(NSInteger)tag on:(BOOL)on
{
  NSMenuItem *mi = [menu addItemWithTitle:title action:action keyEquivalent:@""];
  mi.target = self;
  mi.tag = tag;
  mi.state = on ? NSControlStateValueOn : NSControlStateValueOff;
  return mi;
}

- (NSMenuItem *)info:(NSMenu *)menu title:(NSString *)title
{
  NSMenuItem *mi = [menu addItemWithTitle:title action:nil keyEquivalent:@""];
  mi.enabled = NO;
  return mi;
}

- (void)menuNeedsUpdate:(NSMenu *)menu
{
  [menu removeAllItems];
  AudioObjectID dev = [self device];

  if (dev == kAudioObjectUnknown) {
    [self info:menu title:@"Scream Audio driver is not installed"];
    [self info:menu title:@"Run the Scream installer, then reopen this menu"];
    [menu addItem:[NSMenuItem separatorItem]];
    [self addFooter:menu];
    return;
  }

  CFDictionaryRef cfg = scream_copy_config(dev);
  NSString *addr = [self configAddress:cfg];
  int bits = scream_config_int(cfg, CFSTR("bits"));
  int payload = scream_config_int(cfg, CFSTR("payload"));
  if (cfg) CFRelease(cfg);
  Float64 rate = scream_get_rate(dev);
  BOOL streaming = [self isStreaming];

  [self info:menu title:streaming ? [NSString stringWithFormat:@"Streaming to %@", [self destinationText]]
                                  : @"Not streaming"];
  NSMenuItem *toggle = [self add:menu title:@"Stream to Network" action:@selector(toggleStreaming:) tag:0 on:streaming];
  toggle.keyEquivalent = @"s";
  [menu addItem:[NSMenuItem separatorItem]];

  // Receiver
  NSMenu *recv = [[NSMenu alloc] init];
  recv.autoenablesItems = NO;
  BOOL multicast = [self isMulticast:addr];
  [self add:recv title:@"All Receivers (multicast)" action:@selector(chooseMulticast:) tag:0 on:multicast];
  NSArray<NSString *> *recent = [[NSUserDefaults standardUserDefaults] stringArrayForKey:kRecentReceiversKey] ?: @[];
  if (!multicast && ![recent containsObject:addr]) recent = [@[addr] arrayByAddingObjectsFromArray:recent];
  for (NSString *ip in recent) {
    NSMenuItem *mi = [self add:recv title:ip action:@selector(chooseRecent:) tag:0 on:!multicast && [ip isEqual:addr]];
    mi.representedObject = ip;
  }
  [recv addItem:[NSMenuItem separatorItem]];
  [self add:recv title:@"Other Receiver…" action:@selector(chooseOther:) tag:0 on:NO];
  [menu addItemWithTitle:@"Receiver" action:nil keyEquivalent:@""].submenu = recv;

  // Latency
  NSMenu *lat = [[NSMenu alloc] init];
  lat.autoenablesItems = NO;
  int frameBytes = 2 * (bits > 0 ? bits / 8 : 2);
  for (NSUInteger i = 0; i < COUNT(kLatencyPresets); i++) {
    double ms = rate > 0 ? 1000.0 * (kLatencyPresets[i].payload / frameBytes) / rate : 0;
    NSString *t = [NSString stringWithFormat:@"%s, %.1f ms packets", kLatencyPresets[i].title, ms];
    [self add:lat title:t action:@selector(chooseLatency:) tag:kLatencyPresets[i].payload
           on:payload == kLatencyPresets[i].payload];
  }
  [menu addItemWithTitle:@"Latency" action:nil keyEquivalent:@""].submenu = lat;

  // Quality
  NSMenu *q = [[NSMenu alloc] init];
  q.autoenablesItems = NO;
  for (NSUInteger i = 0; i < COUNT(kBitDepths); i++)
    [self add:q title:[NSString stringWithFormat:@"%d-bit", kBitDepths[i]] action:@selector(chooseBits:)
          tag:kBitDepths[i] on:bits == kBitDepths[i]];
  [q addItem:[NSMenuItem separatorItem]];
  for (NSUInteger i = 0; i < COUNT(kRates); i++)
    [self add:q title:[NSString stringWithFormat:@"%g kHz", kRates[i] / 1000.0] action:@selector(chooseRate:)
          tag:(NSInteger)kRates[i] on:rate == kRates[i]];
  [menu addItemWithTitle:@"Quality" action:nil keyEquivalent:@""].submenu = q;

  [menu addItem:[NSMenuItem separatorItem]];
  [self addFooter:menu];
}

- (void)addFooter:(NSMenu *)menu
{
  [self add:menu title:@"Open at Login" action:@selector(toggleLogin:) tag:0
         on:SMAppService.mainAppService.status == SMAppServiceStatusEnabled];
  [self add:menu title:@"About Scream" action:@selector(about:) tag:0 on:NO];
  [self add:menu title:@"Uninstall Scream…" action:@selector(uninstall:) tag:0 on:NO];
  NSMenuItem *quit = [self add:menu title:@"Quit Scream" action:@selector(quit:) tag:0 on:NO];
  quit.keyEquivalent = @"q";
}

// ---------------------------------------------------------------- actions

- (void)toggleStreaming:(id)sender { [self setStreaming:![self isStreaming]]; }

- (void)setStreaming:(BOOL)on
{
  AudioObjectID dev = [self device];
  if (dev == kAudioObjectUnknown) return;
  NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];

  if (on) {
    AudioObjectID cur = scream_default_output();
    if (cur != dev && cur != kAudioObjectUnknown) {
      CFStringRef uid = scream_copy_uid(cur);
      if (uid) { [ud setObject:(__bridge NSString *)uid forKey:kPrevOutputKey]; CFRelease(uid); }
    }
    [self check:scream_set_default_output(dev) what:@"switch the output to Scream Audio"];
  } else {
    AudioObjectID prev = kAudioObjectUnknown;
    NSString *uid = [ud stringForKey:kPrevOutputKey];
    if (uid) prev = scream_device_for_uid((__bridge CFStringRef)uid);
    if (prev == kAudioObjectUnknown || prev == dev) prev = [self fallbackOutput];
    if (prev != kAudioObjectUnknown)
      [self check:scream_set_default_output(prev) what:@"switch the output back"];
  }
  [self refreshIcon];
}

// First output-capable device that is not Scream, preferring built-in speakers.
- (AudioObjectID)fallbackOutput
{
  AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size) != noErr) return kAudioObjectUnknown;
  UInt32 n = size / sizeof(AudioObjectID);
  AudioObjectID *ids = calloc(n, sizeof(AudioObjectID));
  AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);
  AudioObjectID scream = [self device], best = kAudioObjectUnknown;
  for (UInt32 i = 0; i < n; i++) {
    if (ids[i] == scream) continue;
    AudioObjectPropertyAddress s = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32 ss = 0;
    if (AudioObjectGetPropertyDataSize(ids[i], &s, 0, NULL, &ss) != noErr || ss == 0) continue;
    AudioObjectPropertyAddress c = { kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32 can = 0, cs = sizeof(can);
    if (AudioObjectGetPropertyData(ids[i], &c, 0, NULL, &cs, &can) != noErr || !can) continue;
    AudioObjectPropertyAddress t = { kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 transport = 0, ts = sizeof(transport);
    AudioObjectGetPropertyData(ids[i], &t, 0, NULL, &ts, &transport);
    if (transport == kAudioDeviceTransportTypeBuiltIn) { best = ids[i]; break; }
    if (best == kAudioObjectUnknown) best = ids[i];
  }
  free(ids);
  return best;
}

- (void)chooseMulticast:(id)sender
{
  [self check:scream_set_config_string([self device], CFSTR("address"), SCREAM_DEFAULT_GROUP) what:@"change the receiver"];
  [self refreshIcon];
}

- (void)chooseRecent:(NSMenuItem *)sender { [self useReceiver:sender.representedObject]; }

- (void)chooseOther:(id)sender
{
  NSAlert *alert = [[NSAlert alloc] init];
  alert.messageText = @"Stream to a specific receiver";
  alert.informativeText = @"Enter the IP address of the computer running the Scream receiver. "
                          @"Start the receiver in unicast mode (scream -u).";
  [alert addButtonWithTitle:@"Use Receiver"];
  [alert addButtonWithTitle:@"Cancel"];
  NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 240, 24)];
  field.placeholderString = @"192.168.1.50";
  alert.accessoryView = field;
  alert.window.initialFirstResponder = field;
  [NSApp activateIgnoringOtherApps:YES];
  if ([alert runModal] != NSAlertFirstButtonReturn) return;

  NSString *ip = [field.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
  struct in_addr ia;
  if (inet_pton(AF_INET, ip.UTF8String, &ia) != 1) {
    [self showError:[NSString stringWithFormat:@"\"%@\" is not an IPv4 address.", ip]];
    return;
  }
  [self useReceiver:ip];
}

- (void)useReceiver:(NSString *)ip
{
  if ([self check:scream_set_config_string([self device], CFSTR("address"), ip.UTF8String) what:@"change the receiver"]) {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    if (![self isMulticast:ip]) {
      NSMutableArray *recent = [NSMutableArray arrayWithArray:[ud stringArrayForKey:kRecentReceiversKey] ?: @[]];
      [recent removeObject:ip];
      [recent insertObject:ip atIndex:0];
      while (recent.count > 5) [recent removeLastObject];
      [ud setObject:recent forKey:kRecentReceiversKey];
    }
  }
  [self refreshIcon];
}

- (void)chooseLatency:(NSMenuItem *)sender
{
  [self check:scream_set_config_int([self device], CFSTR("payload"), (int)sender.tag) what:@"change the latency"];
}

- (void)chooseBits:(NSMenuItem *)sender
{
  [self check:scream_set_config_int([self device], CFSTR("bits"), (int)sender.tag) what:@"change the sample width"];
}

- (void)chooseRate:(NSMenuItem *)sender
{
  [self check:scream_set_rate([self device], (Float64)sender.tag) what:@"change the sample rate"];
}

- (void)toggleLogin:(id)sender
{
  [self setLoginItemEnabled:SMAppService.mainAppService.status != SMAppServiceStatusEnabled quiet:NO];
}

- (void)setLoginItemEnabled:(BOOL)on quiet:(BOOL)quiet
{
  NSError *err = nil;
  BOOL ok = on ? [SMAppService.mainAppService registerAndReturnError:&err]
               : [SMAppService.mainAppService unregisterAndReturnError:&err];
  if (!ok && !quiet)
    [self showError:[NSString stringWithFormat:@"Could not change Open at Login: %@. "
                                               @"You can also add Scream in System Settings > General > Login Items.",
                                               err.localizedDescription]];
}

- (void)about:(id)sender
{
  [NSApp activateIgnoringOtherApps:YES];
  [NSApp orderFrontStandardAboutPanelWithOptions:@{
    NSAboutPanelOptionCredits: [[NSAttributedString alloc] initWithString:
      @"Streams the Mac's audio to Scream receivers on your network.\n"
      @"Based on Scream by Tom Kistner (duncanthrax)."],
  }];
}

- (void)uninstall:(id)sender
{
  NSAlert *alert = [[NSAlert alloc] init];
  alert.messageText = @"Uninstall Scream?";
  alert.informativeText = @"This removes the Scream app, the Scream Audio device and screamctl. "
                          @"Sound stops for a second while Core Audio reloads.";
  [alert addButtonWithTitle:@"Uninstall"];
  [alert addButtonWithTitle:@"Cancel"];
  [NSApp activateIgnoringOtherApps:YES];
  if ([alert runModal] != NSAlertFirstButtonReturn) return;

  NSString *script = [[NSBundle mainBundle] pathForResource:@"uninstall" ofType:@"sh"];
  if (!script) { [self showError:@"The uninstall script is missing from the app bundle."]; return; }
  if ([self isStreaming]) [self setStreaming:NO];
  [self setLoginItemEnabled:NO quiet:YES];

  // --from-app: leave this process alone, it quits once the script is done.
  NSString *source = [NSString stringWithFormat:
    @"do shell script (quoted form of \"%@\") & \" --from-app\" with administrator privileges", script];
  NSDictionary *err = nil;
  if (![[[NSAppleScript alloc] initWithSource:source] executeAndReturnError:&err]) {
    if ([err[NSAppleScriptErrorNumber] intValue] != -128)   // -128: user cancelled
      [self showError:[NSString stringWithFormat:@"Uninstall failed: %@", err[NSAppleScriptErrorMessage]]];
    return;
  }
  [NSApp terminate:nil];
}

- (void)quit:(id)sender { [NSApp terminate:nil]; }

// ---------------------------------------------------------------- errors

- (BOOL)check:(OSStatus)st what:(NSString *)what
{
  if (st == noErr) return YES;
  [self showError:[NSString stringWithFormat:@"Could not %@ (error %d).", what, (int)st]];
  return NO;
}

- (void)showError:(NSString *)text
{
  NSAlert *alert = [[NSAlert alloc] init];
  alert.alertStyle = NSAlertStyleWarning;
  alert.messageText = @"Scream";
  alert.informativeText = text;
  [NSApp activateIgnoringOtherApps:YES];
  [alert runModal];
}

@end

int main(int argc, const char *argv[])
{
  @autoreleasepool {
    NSApplication *app = [NSApplication sharedApplication];
    app.activationPolicy = NSApplicationActivationPolicyAccessory;
    ScreamApp *delegate = [[ScreamApp alloc] init];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
