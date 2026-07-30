/*
    IOSHostAppDelegate.mm

    The iOS counterpart to macOS's AUv3HostAppDelegate. Instantiates the
    embedded AUv3 plugin in-process and embeds its view controller as the
    root of the host app's window.

    Why not the usual AVAudioUnitComponentManager / AVAudioUnit
    instantiateWithComponentDescription: flow? On iOS 18+ the process-local
    AudioComponent catalog hides third-party AUv3 extensions from every
    host except GarageBand-class Apple-blessed ones — even for the host's
    own embedded .appex. componentsMatchingDescription and the lower-level
    AudioComponentFindNext both return 0 third-party matches. Instead we
    statically link the AUv3 wrapper runtime and its generated factory
    class into the host (via wrap_auv3_standalone_ios.cmake) and
    instantiate the factory as ObjC directly, bypassing the registry. The
    same .appex is still produced and still works in external hosts that
    don't hit the sandbox (GarageBand, AUM, etc.).

    AU identity + factory class name are baked in at compile time via
    -DAU_TYPE_STR=... and -DAUV3_FACTORY_CLASS_NAME_STR=... set by the
    CMake wrapper function.
*/

#import "IOSHostAppDelegate.h"
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMIDI/CoreMIDI.h>
#include "ios_midi_out_timing.h"
#include <atomic>
#include <cmath>
#include <limits>
#include <mach/mach_time.h>
#include <memory>

static_assert(AUEventSampleTimeImmediate == Clap::Standalone::IOSMidiOut::immediateSampleTime,
              "iOS MIDI output immediate sample-time must keep AUAudioUnit semantics");

static OSType FourCCFromString(const char *s)
{
  if (!s || strlen(s) < 4) return 0;
  return ((OSType)(unsigned char)s[0] << 24) | ((OSType)(unsigned char)s[1] << 16) |
         ((OSType)(unsigned char)s[2] << 8) | (OSType)(unsigned char)s[3];
}

#ifndef AU_TYPE_STR
#define AU_TYPE_STR "aumu"
#endif
#ifndef AU_SUBTYPE_STR
#define AU_SUBTYPE_STR "none"
#endif
#ifndef AU_MANUFACTURER_STR
#define AU_MANUFACTURER_STR "none"
#endif
#ifndef AUV3_FACTORY_CLASS_NAME_STR
#define AUV3_FACTORY_CLASS_NAME_STR ""
#endif

// MIDI-out ring. Render thread (single producer) writes packets here; a
// dispatch-timer drain (single consumer) emits them via MIDIReceived on a
// virtual CoreMIDI source so other apps can subscribe. Power-of-two size
// for fast index masking; 512 slots @ ~1ms drain is far more headroom than
// any reasonable plugin will use.
namespace
{
struct MidiOutPacket
{
  MIDITimeStamp hostTime;
  uint8_t length;
  uint8_t data[3];
};

// The AU reports MIDI output in its render sample-time domain. The source
// node gives us the matching host-time/sample-time anchor for each render
// cycle, so convert while still on that cycle instead of guessing at drain
// time. CoreMIDI virtual-source packets always need a nonzero host timestamp.
static uint64_t hostTicksPerSampleQ32(double sampleRate)
{
  mach_timebase_info_data_t timebase{};
  if (sampleRate <= 0 || mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0)
    return 0;

  const long double ticksPerSample =
      (1000000000.0L * timebase.denom) / (timebase.numer * (long double)sampleRate);
  const long double scaled = ticksPerSample * (1ULL << 32);
  if (scaled <= 0 || scaled >= (long double)std::numeric_limits<uint64_t>::max()) return 0;
  return (uint64_t)(scaled + 0.5L);
}

constexpr uint32_t kMidiOutRingSize = 512;
constexpr uint32_t kMidiOutRingMask = kMidiOutRingSize - 1;
static_assert((kMidiOutRingSize & kMidiOutRingMask) == 0, "kMidiOutRingSize must be a power of two");

struct MidiOutContext
{
  std::atomic<uint32_t> head{0};  // reader cursor, drain side
  std::atomic<uint32_t> tail{0};  // writer cursor, render side
  MidiOutPacket ring[kMidiOutRingSize]{};
  Clap::Standalone::IOSMidiOut::RenderAnchor renderAnchor;
  MIDIEndpointRef source = 0;
};

static char midiOutDrainQueueKey;
}  // namespace

@interface IOSHostViewController : UIViewController
{
  std::shared_ptr<MidiOutContext> _midiOutContext;
  BOOL _renderResourcesAllocated;
}
@property(nonatomic, strong) AUAudioUnit *audioUnit;
@property(nonatomic, strong) UIViewController *pluginViewController;
@property(nonatomic, strong) UILabel *statusLabel;
@property(nonatomic, strong) AVAudioEngine *audioEngine;
@property(nonatomic, strong) NSMutableArray<AVAudioSourceNode *> *auSourceNodes;
@property(nonatomic, assign) MIDIClientRef midiClient;
@property(nonatomic, assign) MIDIPortRef midiInputPort;
@property(nonatomic, strong) dispatch_source_t midiOutDrainTimer;
@property(nonatomic, strong) dispatch_queue_t midiOutDrainQueue;
@property(nonatomic, strong) dispatch_group_t midiOutTeardownFence;
@property(nonatomic, copy) AUScheduleMIDIEventBlock scheduleMIDIBlock;
@end

// CoreMIDI receive callback. Runs on a dedicated high-priority MIDI thread.
// We forward each short (status + up to 2 data bytes) packet directly to
// the AU's scheduleMIDIEventBlock, which is safe to call from any thread.
// SysEx and other long packets are dropped — the AU's short-event API
// won't accept them in a single call; wiring SysEx would require splitting
// across the block's length limit or using the newer MIDI2 event list API.
static void IOSHostMIDIReadProc(const MIDIPacketList *pktlist, void *readProcRefCon, void *srcConnRefCon)
{
  AUScheduleMIDIEventBlock block = (__bridge AUScheduleMIDIEventBlock)readProcRefCon;
  if (!block) return;
  const MIDIPacket *packet = &pktlist->packet[0];
  for (UInt32 i = 0; i < pktlist->numPackets; ++i)
  {
    if (packet->length > 0 && packet->length <= 3)
      block(AUEventSampleTimeImmediate, 0, packet->length, packet->data);
    packet = MIDIPacketNext(packet);
  }
}

@implementation IOSHostViewController

- (void)viewDidLoad
{
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor systemBackgroundColor];

  self.statusLabel = [[UILabel alloc] init];
  self.statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
  self.statusLabel.text = @"Loading AUv3…";
  self.statusLabel.textAlignment = NSTextAlignmentCenter;
  self.statusLabel.numberOfLines = 0;
  [self.view addSubview:self.statusLabel];
  [NSLayoutConstraint activateConstraints:@[
    [self.statusLabel.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
    [self.statusLabel.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
    [self.statusLabel.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor constant:20],
    [self.statusLabel.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor constant:-20],
  ]];

  [self instantiateAU];
}

- (void)setStatus:(NSString *)msg
{
  self.statusLabel.text = msg;
  NSLog(@"[ios-host] %@", msg);
}

- (void)instantiateAU
{
  // Look up the factory class statically linked into our binary. The
  // name comes from target_add_auv3_standalone_ios_wrapper's MD5 of the
  // manufacturer+subtype codes — identical to what wrap_auv3.cmake uses
  // for the .appex, so the class symbol is the same.
  NSString *factoryName = @AUV3_FACTORY_CLASS_NAME_STR;
  if (factoryName.length == 0)
  {
    [self setStatus:@"No factory class name was compiled into the host. "
                    @"Check target_add_auv3_standalone_ios_wrapper CMake integration."];
    return;
  }
  Class factoryClass = NSClassFromString(factoryName);
  if (!factoryClass)
  {
    [self setStatus:[NSString stringWithFormat:
                                  @"Factory class %@ is not linked into this binary. "
                                   "The AUv3 wrapper sources must be compiled into the host target.",
                                  factoryName]];
    return;
  }
  if (![factoryClass conformsToProtocol:@protocol(AUAudioUnitFactory)])
  {
    [self setStatus:[NSString
                        stringWithFormat:@"%@ does not conform to AUAudioUnitFactory.", factoryName]];
    return;
  }

  AudioComponentDescription desc = {};
  desc.componentType = FourCCFromString(AU_TYPE_STR);
  desc.componentSubType = FourCCFromString(AU_SUBTYPE_STR);
  desc.componentManufacturer = FourCCFromString(AU_MANUFACTURER_STR);

  // The factory class IS an AUViewController subclass that conforms to
  // AUAudioUnitFactory — alloc/init is safe without an NSExtensionContext;
  // beginRequestWithExtensionContext: is only invoked by the extension
  // runtime when the VC is hosted out-of-process.
  id<AUAudioUnitFactory> factory = [[factoryClass alloc] init];
  NSError *err = nil;
  AUAudioUnit *au = [factory createAudioUnitWithComponentDescription:desc error:&err];
  if (!au)
  {
    [self setStatus:[NSString stringWithFormat:@"AU creation failed: %@",
                                               err ? err.localizedDescription : @"unknown error"]];
    return;
  }
  self.audioUnit = au;
  self.pluginViewController = (UIViewController *)factory;

  // MIDI client + input port + connections. Done before the audio graph
  // because setupMIDIOutput (next) needs the MIDI client to register a
  // virtual source, and the AU's MIDIOutputEventBlock must be set BEFORE
  // allocateRenderResources is called inside buildEngineGraph (the
  // wrapper captures the block at allocate time).
  [self setupMIDI];
  [self setupMIDIOutput];

  // Audio graph setup. Routes the AU's render output to the system mixer
  // so notes actually produce sound. Effects (aufx) aren't currently
  // wired for input — this iOS host is an instrument host for the
  // common case (aumu / aufg). Kept simple on purpose.
  [self configureAudioSession];
  [self registerSessionObservers];
  [self buildEngineGraph];

  [self embedPluginVC:self.pluginViewController];
}

- (void)setupMIDI
{
  // An AU only accepts MIDI if it exposes scheduleMIDIEventBlock; pure
  // audio effects return nil here. aumu/aumi instruments set it.
  self.scheduleMIDIBlock = self.audioUnit.scheduleMIDIEventBlock;
  if (!self.scheduleMIDIBlock)
  {
    NSLog(@"[ios-host] AU does not accept MIDI (no scheduleMIDIEventBlock)");
    return;
  }

  MIDIClientRef client = 0;
  // MIDIClientCreateWithBlock (iOS 9+) is preferred over the C-callback
  // MIDIClientCreate because it lets us react to topology changes
  // (new USB keyboard plugged in after launch) inline.
  OSStatus st = MIDIClientCreateWithBlock(CFSTR("ClapWrapperIOSStandalone"), &client,
                                          ^(const MIDINotification *message) {
                                            if (message->messageID == kMIDIMsgSetupChanged)
                                              [self connectAllMIDISources];
                                          });
  if (st != noErr)
  {
    NSLog(@"[ios-host] MIDIClientCreateWithBlock failed: %d", (int)st);
    return;
  }
  self.midiClient = client;

  MIDIPortRef port = 0;
  st = MIDIInputPortCreate(client, CFSTR("Input"), IOSHostMIDIReadProc,
                           (__bridge void *)self.scheduleMIDIBlock, &port);
  if (st != noErr)
  {
    NSLog(@"[ios-host] MIDIInputPortCreate failed: %d", (int)st);
    return;
  }
  self.midiInputPort = port;

  [self connectAllMIDISources];
}

- (void)connectAllMIDISources
{
  if (!self.midiInputPort) return;
  ItemCount count = MIDIGetNumberOfSources();
  for (ItemCount i = 0; i < count; ++i)
  {
    MIDIEndpointRef src = MIDIGetSource(i);
    // Idempotent — CoreMIDI ignores a duplicate connect call with
    // kMIDIUnknownProperty status rather than creating two routes.
    MIDIPortConnectSource(self.midiInputPort, src, NULL);
  }
  NSLog(@"[ios-host] MIDI connected to %lu source(s)", (unsigned long)count);
}

- (void)setupMIDIOutput
{
  if (!self.audioUnit || !self.midiClient) return;

  // Skip when the plugin doesn't expose any MIDI output ports — saves the
  // user a phantom virtual source in their MIDI routing UI.
  if (self.audioUnit.MIDIOutputNames.count == 0)
  {
    NSLog(@"[ios-host] AU has no MIDI output ports — skipping MIDI-out");
    return;
  }

  auto context = std::make_shared<MidiOutContext>();
  _midiOutContext = context;

  NSString *displayName = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleDisplayName"];
  if (displayName.length == 0)
    displayName = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
  if (displayName.length == 0) displayName = @"Clap Wrapper";
  NSString *sourceName = [NSString stringWithFormat:@"%@ Out", displayName];

  MIDIEndpointRef src = 0;
  OSStatus st = MIDISourceCreate(self.midiClient, (__bridge CFStringRef)sourceName, &src);
  if (st != noErr)
  {
    NSLog(@"[ios-host] MIDISourceCreate failed: %d", (int)st);
    _midiOutContext.reset();
    return;
  }
  context->source = src;

  // Render-thread block. Writes are bounded — no allocation, no Obj-C
  // method dispatch, or locks. The captured context keeps its ring and
  // render anchor alive until both the AU and timer callbacks are gone.

  self.audioUnit.MIDIOutputEventBlock =
      ^OSStatus(AUEventSampleTime sampleTime, uint8_t cable, NSInteger length, const uint8_t *data) {
        (void)cable;
        if (length <= 0 || length > 3) return noErr;
        uint32_t tail = context->tail.load(std::memory_order_relaxed);
        uint32_t head = context->head.load(std::memory_order_acquire);
        uint32_t next = (tail + 1) & kMidiOutRingMask;
        if (next == head) return noErr;  // ring full — drop
        MidiOutPacket &slot = context->ring[tail];
        const MIDITimeStamp enqueueHostTime = mach_absolute_time();
        slot.hostTime = Clap::Standalone::IOSMidiOut::hostTimeForSample(
            static_cast<Clap::Standalone::IOSMidiOut::SampleTime>(sampleTime), context->renderAnchor,
            enqueueHostTime);
        slot.length = (uint8_t)length;
        for (NSInteger i = 0; i < length && i < 3; ++i) slot.data[i] = data[i];
        context->tail.store(next, std::memory_order_release);
        return noErr;
      };

  // A serial queue lets teardown fence any in-flight drain before it disposes
  // the CoreMIDI source. MIDIReceived stays off the render thread.
  dispatch_queue_t q = dispatch_queue_create("org.freeaudio.clapwrapper.midi-out", DISPATCH_QUEUE_SERIAL);
  dispatch_queue_set_specific(q, &midiOutDrainQueueKey, &midiOutDrainQueueKey, NULL);
  dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
  dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), 1 * NSEC_PER_MSEC,
                            100 * NSEC_PER_USEC);

  dispatch_source_set_event_handler(timer, ^{
    uint32_t head = context->head.load(std::memory_order_relaxed);
    uint32_t tail = context->tail.load(std::memory_order_acquire);
    while (head != tail)
    {
      MidiOutPacket &slot = context->ring[head];
      // MIDIReceived publishes packets when this timer runs. A missed target
      // cannot be recovered retroactively, so publish it at the current host
      // time rather than forwarding an already-past timestamp.
      const MIDITimeStamp now = mach_absolute_time();
      const MIDITimeStamp hostTime =
          Clap::Standalone::IOSMidiOut::hostTimeForDrain(slot.hostTime, now);
      MIDIPacketList list;
      MIDIPacket *pkt = MIDIPacketListInit(&list);
      pkt = MIDIPacketListAdd(&list, sizeof(list), pkt, hostTime, slot.length, slot.data);
      if (pkt && context->source) MIDIReceived(context->source, &list);
      head = (head + 1) & kMidiOutRingMask;
    }
    context->head.store(head, std::memory_order_release);
  });
  self.midiOutDrainTimer = timer;
  self.midiOutDrainQueue = q;
  dispatch_resume(timer);

  NSLog(@"[ios-host] MIDI-out: virtual source \"%@\" ready (%lu port(s) advertised)", sourceName,
        (unsigned long)self.audioUnit.MIDIOutputNames.count);
}

- (void)tearDownMIDIOutput
{
  // Stop/deallocate render production before releasing the block that owns
  // the context. The AU wrapper waits for an in-flight render before its
  // process adapter is destroyed.
  [self tearDownEngineGraph];

  self.audioUnit.MIDIOutputEventBlock = nil;
  auto context = _midiOutContext;
  dispatch_source_t timer = self.midiOutDrainTimer;
  dispatch_queue_t queue = self.midiOutDrainQueue;
  if (timer)
  {
    dispatch_source_cancel(timer);
    self.midiOutDrainTimer = nil;
  }
  self.midiOutDrainQueue = nil;

  const auto teardownAction = Clap::Standalone::IOSMidiOut::teardownAction(
      queue && dispatch_get_specific(&midiOutDrainQueueKey) == &midiOutDrainQueueKey);
  if (queue && teardownAction == Clap::Standalone::IOSMidiOut::TeardownAction::asynchronouslyFence)
  {
    // Reentrant teardown from MIDIReceived cannot synchronously wait on its
    // own serial queue. The fence also keeps dealloc from disposing the
    // owning MIDI client before this source is gone.
    dispatch_group_t fence = self.midiOutTeardownFence;
    if (!fence)
    {
      fence = dispatch_group_create();
      self.midiOutTeardownFence = fence;
    }
    dispatch_group_enter(fence);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      dispatch_sync(queue, ^{});
      if (context && context->source)
      {
        MIDIEndpointDispose(context->source);
        context->source = 0;
      }
      dispatch_group_leave(fence);
    });
  }
  else
  {
    if (queue) dispatch_sync(queue, ^{});
    if (context && context->source)
    {
      MIDIEndpointDispose(context->source);
      context->source = 0;
    }
  }
  _midiOutContext.reset();
}

- (void)dealloc
{
  [[NSNotificationCenter defaultCenter] removeObserver:self];

  MIDIClientRef client = _midiClient;
  MIDIPortRef inputPort = _midiInputPort;
  _midiClient = 0;
  _midiInputPort = 0;
  [self tearDownMIDIOutput];

  dispatch_group_t fence = self.midiOutTeardownFence;
  void (^disposeClient)(void) = ^{
    if (inputPort) MIDIPortDispose(inputPort);
    if (client) MIDIClientDispose(client);
  };

  const auto clientTeardownAction = Clap::Standalone::IOSMidiOut::teardownAction(
      fence && dispatch_get_specific(&midiOutDrainQueueKey) == &midiOutDrainQueueKey);
  if (fence && clientTeardownAction == Clap::Standalone::IOSMidiOut::TeardownAction::asynchronouslyFence)
  {
    // Waiting here would deadlock: the fence needs this event handler to
    // return before its queued drain barrier can run.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      dispatch_group_wait(fence, DISPATCH_TIME_FOREVER);
      disposeClient();
    });
  }
  else
  {
    if (fence) dispatch_group_wait(fence, DISPATCH_TIME_FOREVER);
    disposeClient();
  }
}

// Audio graph setup is split into three pieces so we can rebuild the engine
// half independently when the hardware sample rate changes (route change,
// AirPlay handoff, post-interruption rate flip): configure session once,
// rebuild engine + AU render resources whenever rate moves.
- (void)configureAudioSession
{
  NSError *err = nil;
  AVAudioSession *session = [AVAudioSession sharedInstance];
  [session setCategory:AVAudioSessionCategoryPlayback error:&err];
  if (err) NSLog(@"[ios-host] AVAudioSession setCategory: %@", err);
  err = nil;

  // Request a 256-frame I/O buffer for snappier note response. iOS
  // rounds to its nearest supported size (typically powers of two
  // between 64 and 4096); the actual value is read back after
  // setActive:. Must be set BEFORE activation for the hardware to
  // honor it on this session.
  const AVAudioFrameCount preferredFrames = 256;
  double rateForPrefs = session.sampleRate > 0 ? session.sampleRate : 48000.0;
  [session setPreferredIOBufferDuration:((double)preferredFrames / rateForPrefs) error:&err];
  if (err) NSLog(@"[ios-host] setPreferredIOBufferDuration: %@", err);
  err = nil;

  [session setActive:YES error:&err];
  if (err) NSLog(@"[ios-host] AVAudioSession setActive: %@", err);

  NSLog(@"[ios-host] audio session: sampleRate=%.0f ioBuffer=%.3fms (~%.0f frames)", session.sampleRate,
        session.IOBufferDuration * 1000.0, session.IOBufferDuration * session.sampleRate);
}

- (void)buildEngineGraph
{
  NSError *err = nil;
  AVAudioSession *session = [AVAudioSession sharedInstance];

  // Pin every output bus to a stereo float32 format at the session's
  // current hardware rate. An AUv3 instrument's default is often mono
  // or a hardware-defined format that doesn't line up with what the
  // engine's mainMixer wants. For multi-output plugins (main + aux,
  // multi-stems) we mix all busses into the single mainMixer here —
  // a richer host with per-bus routing UI would split them.
  //
  // Input busses (side-chain, audio-in for effects) are intentionally
  // left unwired: the standalone has no UI for picking an input source
  // (mic, file, inter-app), and silently feeding them with the system
  // mic on every effect launch would surprise users. Effect-host work
  // is a separate scope.
  double sr = session.sampleRate > 0 ? session.sampleRate : 48000.0;
  AVAudioFormat *stereoFmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:sr channels:2];
  NSUInteger outBusCount = self.audioUnit.outputBusses.count;
  if (outBusCount == 0)
  {
    NSLog(@"[ios-host] AU declares no output busses — no audio graph to build");
    return;
  }
  for (NSUInteger i = 0; i < outBusCount; ++i)
  {
    AUAudioUnitBus *bus = self.audioUnit.outputBusses[i];
    if (![bus setFormat:stereoFmt error:&err])
      NSLog(@"[ios-host] outputBus[%lu] setFormat: %@", (unsigned long)i, err);
    err = nil;
  }

  // AVAudioEngine can ask the source node for fairly large blocks; the
  // AU must accept at least that frame count or render fails. 4096 is
  // comfortably above any iOS session buffer size we're likely to see.
  self.audioUnit.maximumFramesToRender = 4096;

  if (![self.audioUnit allocateRenderResourcesAndReturnError:&err])
  {
    NSLog(@"[ios-host] allocateRenderResources failed: %@", err);
    return;
  }
  _renderResourcesAllocated = YES;

  // Use renderBlock — NOT internalRenderBlock. renderBlock is the
  // public entry that wraps the internal block with parameter
  // automation + MIDI event scheduling; internalRenderBlock is what the
  // AU subclass *implements* and calling it directly skips the plumbing
  // that delivers MIDI events from scheduleMIDIEventBlock into the
  // render cycle.
  AURenderBlock auRender = self.audioUnit.renderBlock;
  const uint64_t ticksPerSample = hostTicksPerSampleQ32(sr);
  auto midiOutContext = _midiOutContext;

  self.audioEngine = [[AVAudioEngine alloc] init];
  self.auSourceNodes = [NSMutableArray arrayWithCapacity:outBusCount];

  for (NSUInteger i = 0; i < outBusCount; ++i)
  {
    AVAudioFormat *auFormat = self.audioUnit.outputBusses[i].format;
    NSInteger busIndex = (NSInteger)i;
    AVAudioSourceNode *srcNode = [[AVAudioSourceNode alloc]
        initWithFormat:auFormat
           renderBlock:^OSStatus(BOOL *isSilence, const AudioTimeStamp *timestamp,
                                 AVAudioFrameCount frameCount, AudioBufferList *outputData) {
             if (midiOutContext)
               Clap::Standalone::IOSMidiOut::updateRenderAnchor(
                   midiOutContext->renderAnchor, timestamp ? timestamp->mSampleTime : 0,
                   timestamp ? timestamp->mHostTime : 0,
                   timestamp && (timestamp->mFlags & kAudioTimeStampSampleTimeValid) != 0,
                   timestamp && (timestamp->mFlags & kAudioTimeStampHostTimeValid) != 0, frameCount,
                   ticksPerSample);
             AudioUnitRenderActionFlags flags = 0;
             return auRender(&flags, timestamp, frameCount, busIndex, outputData, NULL);
           }];
    [self.audioEngine attachNode:srcNode];
    [self.audioEngine connect:srcNode to:self.audioEngine.mainMixerNode format:auFormat];
    [self.auSourceNodes addObject:srcNode];
  }

  if (![self.audioEngine startAndReturnError:&err]) NSLog(@"[ios-host] AVAudioEngine start: %@", err);
}

- (void)tearDownEngineGraph
{
  if (_midiOutContext)
    _midiOutContext->renderAnchor.valid.store(0, std::memory_order_release);
  if (self.audioEngine.isRunning) [self.audioEngine stop];
  for (AVAudioSourceNode *node in self.auSourceNodes) [self.audioEngine detachNode:node];
  self.auSourceNodes = nil;
  self.audioEngine = nil;
  if (_renderResourcesAllocated)
  {
    [self.audioUnit deallocateRenderResources];
    _renderResourcesAllocated = NO;
  }
}

// Notification observers for AVAudioSession interruption + route change and
// for app lifecycle. Without these the standalone host loses audio on phone
// calls / Siri / AirPods unplug, and never recovers.
- (void)registerSessionObservers
{
  NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
  [nc addObserver:self
         selector:@selector(handleInterruption:)
             name:AVAudioSessionInterruptionNotification
           object:nil];
  [nc addObserver:self
         selector:@selector(handleRouteChange:)
             name:AVAudioSessionRouteChangeNotification
           object:nil];
  [nc addObserver:self
         selector:@selector(handleAppDidBecomeActive:)
             name:UIApplicationDidBecomeActiveNotification
           object:nil];
  [nc addObserver:self
         selector:@selector(handleAppWillTerminate:)
             name:UIApplicationWillTerminateNotification
           object:nil];
}

- (void)handleInterruption:(NSNotification *)note
{
  // AVAudioSession posts on its own thread; the engine graph is owned by
  // the main thread, so hop over before touching it.
  if (!NSThread.isMainThread)
  {
    dispatch_async(dispatch_get_main_queue(), ^{
      [self handleInterruption:note];
    });
    return;
  }

  NSDictionary *info = note.userInfo;
  AVAudioSessionInterruptionType type =
      (AVAudioSessionInterruptionType)[info[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];

  if (type == AVAudioSessionInterruptionTypeBegan)
  {
    // OS deactivated the session for us. Stop the engine; leave AU +
    // render resources allocated so a quick resume doesn't have to
    // rebuild. Per Apple TN2629 we do not call setActive:NO ourselves.
    NSLog(@"[ios-host] interruption began — stopping engine");
    if (self.audioEngine.isRunning) [self.audioEngine stop];
    return;
  }

  if (type == AVAudioSessionInterruptionTypeEnded)
  {
    AVAudioSessionInterruptionOptions opts = (AVAudioSessionInterruptionOptions)
        [info[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
    BOOL shouldResume = (opts & AVAudioSessionInterruptionOptionShouldResume) != 0;
    NSLog(@"[ios-host] interruption ended — shouldResume=%d", (int)shouldResume);
    if (!shouldResume) return;

    NSError *err = nil;
    [[AVAudioSession sharedInstance] setActive:YES error:&err];
    if (err)
    {
      NSLog(@"[ios-host] setActive after interruption: %@", err);
      return;
    }

    // Rate may have changed during interruption (different route picked
    // up by the system). Rebuild graph if so; otherwise restart engine.
    // A zero-output AU (MIDI effect) has no bus to compare — skip the check.
    double currentRate = [AVAudioSession sharedInstance].sampleRate;
    AVAudioFormat *currentFmt =
        self.audioUnit.outputBusses.count > 0 ? self.audioUnit.outputBusses[0].format : nil;
    if (currentFmt && fabs(currentRate - currentFmt.sampleRate) > 1.0)
    {
      NSLog(@"[ios-host] sample rate changed %g -> %g; rebuilding graph", currentFmt.sampleRate,
            currentRate);
      [self tearDownEngineGraph];
      [self buildEngineGraph];
      return;
    }

    if (![self.audioEngine startAndReturnError:&err])
      NSLog(@"[ios-host] AVAudioEngine restart: %@", err);
  }
}

- (void)handleRouteChange:(NSNotification *)note
{
  // AVAudioSession posts on its own thread; the engine graph is owned by
  // the main thread, so hop over before touching it.
  if (!NSThread.isMainThread)
  {
    dispatch_async(dispatch_get_main_queue(), ^{
      [self handleRouteChange:note];
    });
    return;
  }

  NSDictionary *info = note.userInfo;
  AVAudioSessionRouteChangeReason reason =
      (AVAudioSessionRouteChangeReason)[info[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];

  AVAudioSession *session = [AVAudioSession sharedInstance];
  NSLog(@"[ios-host] route change: reason=%lu rate=%.0f buffer=%.3fms", (unsigned long)reason,
        session.sampleRate, session.IOBufferDuration * 1000.0);

  // Hardware rate change (AirPlay handoff, USB-C audio swap, etc.) — the
  // AU output bus format no longer matches reality. Rebuild graph.
  // A zero-output AU (MIDI effect) has no bus to compare — skip the check.
  AVAudioFormat *currentFmt =
      self.audioUnit.outputBusses.count > 0 ? self.audioUnit.outputBusses[0].format : nil;
  if (currentFmt && fabs(session.sampleRate - currentFmt.sampleRate) > 1.0)
  {
    NSLog(@"[ios-host] route change sample rate %g -> %g; rebuilding graph", currentFmt.sampleRate,
          session.sampleRate);
    [self tearDownEngineGraph];
    [self buildEngineGraph];
    return;
  }

  // Route change without rate change (headphones unplug to speaker, etc.)
  // — engine usually keeps running, but on some routes iOS quietly stops
  // it. Restart if needed.
  if (!self.audioEngine.isRunning)
  {
    NSError *err = nil;
    if (![self.audioEngine startAndReturnError:&err])
      NSLog(@"[ios-host] AVAudioEngine restart after route change: %@", err);
  }
}

- (void)handleAppDidBecomeActive:(NSNotification *)note
{
  // Belt-and-suspenders: any silent failure that left the engine stopped
  // (a notification we missed, an OS-side route change without callback)
  // gets a recovery chance here.
  if (!self.audioEngine || self.audioEngine.isRunning) return;
  NSError *err = nil;
  if (![self.audioEngine startAndReturnError:&err])
    NSLog(@"[ios-host] AVAudioEngine restart on becomeActive: %@", err);
}

- (void)handleAppWillTerminate:(NSNotification *)note
{
  NSLog(@"[ios-host] app terminating — clean shutdown");
  [self tearDownMIDIOutput];
  NSError *err = nil;
  [[AVAudioSession sharedInstance] setActive:NO
                                 withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                       error:&err];
  if (err) NSLog(@"[ios-host] setActive:NO on terminate: %@", err);
}

- (void)embedPluginVC:(UIViewController *)vc
{
  [self addChildViewController:vc];
  vc.view.translatesAutoresizingMaskIntoConstraints = NO;
  [self.view addSubview:vc.view];
  [NSLayoutConstraint activateConstraints:@[
    [vc.view.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
    [vc.view.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
    [vc.view.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
    [vc.view.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
  ]];
  [vc didMoveToParentViewController:self];
  self.statusLabel.hidden = YES;
}

@end

@implementation IOSHostAppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
  self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
  self.window.rootViewController = [[IOSHostViewController alloc] init];
  [self.window makeKeyAndVisible];
  return YES;
}

@end
