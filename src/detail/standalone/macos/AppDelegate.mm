#import "AppDelegate.h"

#include <AVFoundation/AVFoundation.h>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <map>
#include <vector>

#include <clapwrapper/standalone-services.h>
#include "detail/standalone/entry.h"
#include "detail/standalone/standalone_details.h"
#include "detail/standalone/standalone_host.h"
#include "detail/webview/webview_host.h"

#include "detail/clap/fsutil.h"

namespace
{
constexpr uint32_t minimumSampleRate = 22050;
constexpr uint32_t maximumSampleRate = 192000;
std::unique_ptr<freeaudio::clap_wrapper::WebViewHost> webviewHost;
}

@interface ClapWrapperAppDelegate ()

@property(copy) AudioObjectPropertyListenerBlock defaultOutputDeviceListener;

- (void)installDefaultOutputDeviceListener;
- (void)removeDefaultOutputDeviceListener;
- (void)defaultOutputDeviceChanged;

@end

@interface AudioSettingsWindow : NSWindow
{
  NSPopUpButton *outputSelection, *outputChannelSelection;
  NSPopUpButton *inputSelection, *inputChannelSelection;
  NSPopUpButton *sampleRateSelection, *bufferSizeSelection;
  std::vector<clap_wrapper_standalone_audio_device_t> outputDevices, inputDevices;
  std::vector<clap_wrapper_standalone_midi_port_t> midiPorts;
  std::vector<uint64_t> selectedMidiPortIds;
  std::vector<NSButton *> midiPortButtons;
  clap_wrapper_standalone_audio_settings_t selectedAudio;
  NSTextField *statusLabel;
}

- (void)setupContents;
- (BOOL)loadServiceSnapshots;
- (void)resetSampleRateSelection;
- (void)resetChannelSelections;
- (NSInteger)selectedOutputDeviceIndex;
- (void)applySelections;
- (void)onSelectionChanged:(id)sender;
- (void)showConfigurationFailure:(NSString *)message restored:(BOOL)restored;

@end

@implementation ClapWrapperAppDelegate

- (void)timerCallback:(NSTimer *)instance
{
  auto *standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
  standaloneHost->serviceMainThreadRequests();
}

- (void)doSetup
{
  // Insert code here to initialize your application
  const char *argv[2] = {OUTPUT_NAME, 0};

  const clap_plugin_entry *entry{nullptr};
#ifdef STATICALLY_LINKED_CLAP_ENTRY
  extern const clap_plugin_entry clap_entry;
  entry = &clap_entry;
#else
  // Library shenanigans t/k
  std::string clapName{HOSTED_CLAP_NAME};
  LOGINFO("Loading '{}'", clapName);

  auto pts = Clap::getValidCLAPSearchPaths();

  auto lib = Clap::Library();

  for (const auto &searchPaths : pts)
  {
    auto clapPath = searchPaths / (clapName + ".clap");

    if (fs::is_directory(clapPath) && !entry)
    {
      lib.load(clapPath);
      entry = lib._pluginEntry;
    }
  }
#endif

  if (!entry)
  {
    return;
  }
  self.requestCallbackTimer = [NSTimer timerWithTimeInterval:0.08
                                                      target:self
                                                    selector:@selector(timerCallback:)
                                                    userInfo:nil
                                                     repeats:YES];
  auto *runLoop = [NSRunLoop currentRunLoop];
  [runLoop addTimer:self.requestCallbackTimer forMode:NSRunLoopCommonModes];
  std::string pid{PLUGIN_ID};
  int pindex{PLUGIN_INDEX};

#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 101400
  switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio])
  {
    case AVAuthorizationStatusNotDetermined:
    {
      // The app hasn't yet asked the user for camera access.
      [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                               completionHandler:^(BOOL granted) {
                                 if (granted)
                                 {
                                 }
                               }];
      break;
    }
    default:
      break;
  }
#endif

  auto plugin =
      freeaudio::clap_wrapper::standalone::mainCreatePlugin(entry, pid, pindex, 1, (char **)argv);

  [[self window] orderFrontRegardless];
  [[self window] setDelegate:self];

  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onRequestResize = [self](uint32_t w,
                                                                                     uint32_t h) {
    NSSize sz;
    sz.width = w;
    sz.height = h;
    [[self window] setContentSize:sz];
    return false;
  };

  if (plugin->_ext._webview)
  {
    auto *standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
    webviewHost = std::make_unique<freeaudio::clap_wrapper::WebViewHost>(
        plugin->_plugin, plugin->_ext._webview, plugin->_ext._webviewGui, plugin.get(), true,
        [standaloneHost](const void *data, uint32_t size)
        { return standaloneHost->receiveWebviewMessage(data, size); });

    if (webviewHost->isOpen())
    {
      auto *view = (__bridge NSView *)webviewHost->viewHandle();
      auto *content = [[self window] contentView];
      [view setFrame:[content bounds]];
      [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
      [content addSubview:view];
      webviewHost->navigate();
      [[self window] setContentSize:NSMakeSize(webviewHost->width(), webviewHost->height())];

      standaloneHost->sendWebviewMessage =
          [](const void *buffer, uint32_t size) { return webviewHost->send(buffer, size); };
      standaloneHost->syncMappingUI();
    }
    else
    {
      webviewHost.reset();
    }
  }

  if (!webviewHost && plugin->_ext._gui)
  {
    auto ui = plugin->_ext._gui;
    auto p = plugin->_plugin;
    if (!ui->is_api_supported(p, CLAP_WINDOW_API_COCOA, false))
      LOGINFO("[WARNING] GUI API not supported");

    if (!ui->create(p, CLAP_WINDOW_API_COCOA, false))
    {
      LOGINFO("[ERROR] Plugin GUI create() failed");
      @autoreleasepool
      {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Plugin Error"];
        [alert setInformativeText:@"The plugin failed to create its user interface."];
        [alert addButtonWithTitle:@"OK"];
        [alert runModal];
      }
      return;
    }
    ui->set_scale(p, 1);

    uint32_t w = 0, h = 0;
    bool sizeValid = ui->get_size(p, &w, &h);
    NSString *sizeError = nil;
    if (!sizeValid)
      sizeError = @"The plugin failed to report its window size.";
    else if (w == 0 || h == 0 || w > 16384 || h > 16384)
      sizeError =
          [NSString stringWithFormat:@"The plugin reported an invalid window size (%u x %u).", w, h];

    if (sizeError)
    {
      LOGINFO("[ERROR] Plugin GUI get_size() failed: {}", [sizeError UTF8String]);
      @autoreleasepool
      {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Plugin Error"];
        [alert setInformativeText:sizeError];
        [alert addButtonWithTitle:@"OK"];
        [alert runModal];
      }
      ui->destroy(p);
      return;
    }

    if (ui->can_resize(p))
    {
      ui->adjust_size(p, &w, &h);
    }

    NSView *view = [[self window] contentView];

    NSSize sz;
    sz.width = w;
    sz.height = h;
    [[self window] setContentSize:sz];

    clap_window win;
    win.api = CLAP_WINDOW_API_COCOA;
    win.cocoa = view;
    if (!ui->set_parent(p, &win))
    {
      LOGINFO("[ERROR] Plugin GUI set_parent() failed");
      @autoreleasepool
      {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Plugin Error"];
        [alert
            setInformativeText:
                @"The plugin failed to embed its user interface. Please contact the plugin developer."];
        [alert addButtonWithTitle:@"OK"];
        [alert runModal];
      }
      return;
    }
    ui->show(p);
  }

  freeaudio::clap_wrapper::standalone::getStandaloneHost()->displayAudioError = [](auto &s) {
    NSLog(@"Error Reported: %s", s.c_str());
    @autoreleasepool
    {
      NSAlert *alert = [[NSAlert alloc] init];
      [alert setMessageText:@"Unable to configure audio"];
      [alert setInformativeText:[[NSString alloc] initWithUTF8String:s.c_str()]];
      [alert addButtonWithTitle:@"OK"];
      [alert runModal];
    }
  };

  if (!freeaudio::clap_wrapper::standalone::mainStartAudio()) NSLog(@"Standalone audio startup failed");
  [self installDefaultOutputDeviceListener];
}

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification
{
  for (NSString *title in @[ @"File", @"Window" ])
  {
    if (auto *item = [[NSApp mainMenu] itemWithTitle:title])
      [[NSApp mainMenu] removeItem:item];
  }
  [NSApp activateIgnoringOtherApps:YES];
  [NSTimer scheduledTimerWithTimeInterval:0.001
                                   target:self
                                 selector:@selector(doSetup)
                                 userInfo:nil
                                  repeats:NO];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
  return true;
}

- (void)applicationWillTerminate:(NSNotification *)aNotification
{
  LOGDETAIL("Application terminating");
  [self removeDefaultOutputDeviceListener];
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->displayAudioError = nullptr;
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onRequestResize = nullptr;

  auto plugin = freeaudio::clap_wrapper::standalone::getMainPlugin();

  const auto usedWebview = webviewHost != nullptr;
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->sendWebviewMessage = nullptr;
  webviewHost.reset();

  if (!usedWebview && plugin && plugin->_ext._gui)
  {
    plugin->_ext._gui->hide(plugin->_plugin);
    plugin->_ext._gui->destroy(plugin->_plugin);
  }

  [self.requestCallbackTimer invalidate];
  self.requestCallbackTimer = nil;

  freeaudio::clap_wrapper::standalone::mainFinish();
}

- (void)installDefaultOutputDeviceListener
{
  if (self.defaultOutputDeviceListener != nil) return;
  const AudioObjectPropertyAddress address{
      kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain};
  self.defaultOutputDeviceListener =
      ^(UInt32, const AudioObjectPropertyAddress *) {
        auto *delegate = (ClapWrapperAppDelegate *)[NSApp delegate];
        [delegate defaultOutputDeviceChanged];
      };
  const auto status = AudioObjectAddPropertyListenerBlock(
      kAudioObjectSystemObject, &address, dispatch_get_main_queue(),
      self.defaultOutputDeviceListener);
  if (status != noErr)
  {
    LOGINFO("[ERROR] Unable to observe the macOS default output device ({})", status);
    self.defaultOutputDeviceListener = nil;
  }
}

- (void)removeDefaultOutputDeviceListener
{
  if (self.defaultOutputDeviceListener == nil) return;
  const AudioObjectPropertyAddress address{
      kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain};
  AudioObjectRemovePropertyListenerBlock(
      kAudioObjectSystemObject, &address, dispatch_get_main_queue(),
      self.defaultOutputDeviceListener);
  self.defaultOutputDeviceListener = nil;
}

- (void)defaultOutputDeviceChanged
{
  auto *standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
  if (!standaloneHost->followSystemDefaultOutput || !standaloneHost->startAudioOutputUsed) return;

  standaloneHost->stopAudioThread();
  standaloneHost->stopMIDIThread();
  standaloneHost->deactivatePlugin();
  standaloneHost->refreshAudioServiceSnapshot();
  standaloneHost->refreshMidiServiceSnapshot();
  if (!standaloneHost->rebuildMIDIEndpoints() || !standaloneHost->startAudioThread())
  {
    const std::string message{"Unable to use the new macOS default output device."};
    if (standaloneHost->displayAudioError) standaloneHost->displayAudioError(message);
  }
}

- (IBAction)openAudioSettingsWindow:(id)sender
{
  @autoreleasepool
  {
    NSRect windowRect = NSMakeRect(0, 0, 520, 580);

    auto *window = [[AudioSettingsWindow alloc]
        initWithContentRect:windowRect
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];

    [window setupContents];

    // Center the window and make it key window and front.
    [window center];
    [window makeKeyAndOrderFront:nil];
  }
}

- (void)windowDidResize:(NSNotification *)notification
{
  auto plugin = freeaudio::clap_wrapper::standalone::getMainPlugin();

  if (webviewHost)
  {
    auto *content = [[self window] contentView];
    auto size = [content bounds].size;
    webviewHost->setSize(static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height));
    return;
  }

  if (plugin && plugin->_ext._gui)
  {
    auto canRS = plugin->_ext._gui->can_resize(plugin->_plugin);
    if (canRS)
    {
      auto w = [self window];
      auto f = [w frame];
      auto cr = [w contentRectForFrameRect:f];
      plugin->_ext._gui->set_size(plugin->_plugin, cr.size.width, cr.size.height);
    }
  }
}

- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize
{
  auto plugin = freeaudio::clap_wrapper::standalone::getMainPlugin();

  if (webviewHost)
  {
    if (!webviewHost->canResize())
    {
      auto *window = [self window];
      auto content = [window contentRectForFrameRect:[window frame]];
      content.size = NSMakeSize(webviewHost->width(), webviewHost->height());
      return [window frameRectForContentRect:content].size;
    }

    auto *window = [self window];
    auto frame = [window frame];
    frame.size = frameSize;
    auto content = [window contentRectForFrameRect:frame];
    uint32_t width = static_cast<uint32_t>(content.size.width);
    uint32_t height = static_cast<uint32_t>(content.size.height);
    webviewHost->adjustSize(width, height);
    content.size = NSMakeSize(width, height);
    return [window frameRectForContentRect:content].size;
  }

  if (plugin && plugin->_ext._gui)
  {
    auto w = [self window];
    auto f = [w frame];
    f.size = frameSize;
    auto cr = [w contentRectForFrameRect:f];

    auto canRS = plugin->_ext._gui->can_resize(plugin->_plugin);
    if (!canRS)
    {
      uint32_t w, h;
      plugin->_ext._gui->get_size(plugin->_plugin, &w, &h);
      cr.size.width = w;
      cr.size.height = h;
    }
    else
    {
      uint32_t w = frameSize.width, h = frameSize.height;
      plugin->_ext._gui->adjust_size(plugin->_plugin, &w, &h);
      cr.size.width = w;
      cr.size.height = h;
    }
    auto fr = [w frameRectForContentRect:cr];
    frameSize = fr.size;
  }
  return frameSize;
}

@end

@implementation AudioSettingsWindow

- (BOOL)loadServiceSnapshots
{
  auto *standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
  standaloneHost->refreshAudioServiceSnapshot();
  standaloneHost->refreshMidiServiceSnapshot();

  clap_wrapper_standalone_audio_snapshot_t audioSnapshot{};
  audioSnapshot.struct_size = sizeof(audioSnapshot);
  if (!standaloneHost->services.getAudioSnapshot(audioSnapshot))
  {
    outputDevices.resize(audioSnapshot.output_device_count);
    inputDevices.resize(audioSnapshot.input_device_count);
    audioSnapshot.output_devices = outputDevices.data();
    audioSnapshot.output_device_capacity = static_cast<uint32_t>(outputDevices.size());
    audioSnapshot.input_devices = inputDevices.data();
    audioSnapshot.input_device_capacity = static_cast<uint32_t>(inputDevices.size());
    if (!standaloneHost->services.getAudioSnapshot(audioSnapshot)) return NO;
  }
  selectedAudio = audioSnapshot.selected;

  clap_wrapper_standalone_midi_snapshot_t midiSnapshot{};
  midiSnapshot.struct_size = sizeof(midiSnapshot);
  if (!standaloneHost->services.getMidiSnapshot(midiSnapshot))
  {
    midiPorts.resize(midiSnapshot.port_count);
    selectedMidiPortIds.resize(midiSnapshot.selected_port_count);
    midiSnapshot.ports = midiPorts.data();
    midiSnapshot.port_capacity = static_cast<uint32_t>(midiPorts.size());
    midiSnapshot.selected_port_ids = selectedMidiPortIds.data();
    midiSnapshot.selected_port_capacity = static_cast<uint32_t>(selectedMidiPortIds.size());
    if (!standaloneHost->services.getMidiSnapshot(midiSnapshot)) return NO;
  }

  return YES;
}

- (void)setupContents
{
  @autoreleasepool
  {
    auto addLabel = [](NSString *s, int x, int y) {
      NSTextField *label = [[NSTextField alloc] initWithFrame:NSMakeRect(x, y, 140, 24)];
      [label setStringValue:s];
      [label setEditable:NO];
      [label setSelectable:NO];
      [label setBezeled:NO];
      [label setDrawsBackground:NO];
      return label;
    };
    [self setTitle:@"Audio/MIDI Settings"];

    NSButton *cancelButton = [[NSButton alloc] initWithFrame:NSMakeRect(428, 16, 80, 30)];
    [cancelButton setTitle:@"Close"];
    [cancelButton setTarget:self];
    [cancelButton setAction:@selector(cancelButtonPressed:)];

    statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(16, 20, 404, 24)];
    [statusLabel setEditable:NO];
    [statusLabel setSelectable:NO];
    [statusLabel setBezeled:NO];
    [statusLabel setDrawsBackground:NO];
    [statusLabel setTextColor:[NSColor secondaryLabelColor]];
    [statusLabel setStringValue:@"Changes apply automatically"];
    [[self contentView] addSubview:statusLabel];

    [[self contentView] addSubview:addLabel(@"Output", 16, 540)];
    outputSelection = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, 536, 354, 28)];
    [[self contentView] addSubview:addLabel(@"Output Channels", 16, 504)];
    outputChannelSelection = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, 500, 140, 28)];
    [[self contentView] addSubview:addLabel(@"Input", 16, 468)];
    inputSelection = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, 464, 354, 28)];
    [[self contentView] addSubview:addLabel(@"Input Channels", 16, 432)];
    inputChannelSelection = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, 428, 140, 28)];
    [[self contentView] addSubview:addLabel(@"Sample Rate", 16, 396)];
    sampleRateSelection = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, 392, 140, 28)];
    [[self contentView] addSubview:addLabel(@"Buffer Size", 16, 360)];
    bufferSizeSelection = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, 356, 140, 28)];

    NSButton *defaultButton = [[NSButton alloc] initWithFrame:NSMakeRect(150, 316, 220, 28)];
    [defaultButton setTitle:@"Reset to System Default"];
    [defaultButton setTarget:self];
    [defaultButton setAction:@selector(defaultButtonPressed:)];
    [[self contentView] addSubview:defaultButton];

    NSBox *horizontalRule = [[NSBox alloc] initWithFrame:NSMakeRect(16, 300, 488, 1)];
    [horizontalRule setBoxType:NSBoxSeparator];
    [[self contentView] addSubview:horizontalRule];

    [[self contentView] addSubview:addLabel(@"MIDI Inputs", 16, 264)];

    if (![self loadServiceSnapshots])
    {
      [self showConfigurationFailure:@"Standalone audio services are unavailable." restored:YES];
      return;
    }

    [outputSelection addItemWithTitle:@"No Output"];
    [outputSelection addItemWithTitle:@"System Default Output"];
    for (const auto &device : outputDevices)
      [outputSelection addItemWithTitle:[[NSString alloc] initWithUTF8String:device.name]];
    if ((selectedAudio.flags & CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED) != 0)
    {
      if ((selectedAudio.flags & CLAP_WRAPPER_STANDALONE_AUDIO_FOLLOW_DEFAULT_OUTPUT) != 0)
        [outputSelection selectItemAtIndex:1];
      else
        for (auto index = 0u; index < outputDevices.size(); ++index)
          if (outputDevices[index].id == selectedAudio.output_device_id)
            [outputSelection selectItemAtIndex:index + 2];
    }
    [outputSelection setAction:@selector(onAudioMenuChanged:)];
    [outputSelection setTarget:self];

    [inputSelection addItemWithTitle:@"No Input"];
    for (const auto &device : inputDevices)
      [inputSelection addItemWithTitle:[[NSString alloc] initWithUTF8String:device.name]];
    if ((selectedAudio.flags & CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED) != 0)
    {
      for (auto index = 0u; index < inputDevices.size(); ++index)
        if (inputDevices[index].id == selectedAudio.input_device_id)
          [inputSelection selectItemAtIndex:index + 1];
    }
    [inputSelection setAction:@selector(onAudioMenuChanged:)];
    [inputSelection setTarget:self];

    [self resetChannelSelections];
    [self resetSampleRateSelection];

    [[self contentView] addSubview:outputSelection];
    [[self contentView] addSubview:outputChannelSelection];
    [[self contentView] addSubview:inputSelection];
    [[self contentView] addSubview:inputChannelSelection];
    [[self contentView] addSubview:sampleRateSelection];
    [[self contentView] addSubview:bufferSizeSelection];
    for (NSPopUpButton *selection in
         @[ outputChannelSelection, inputChannelSelection, sampleRateSelection, bufferSizeSelection ])
    {
      [selection setTarget:self];
      [selection setAction:@selector(onSelectionChanged:)];
    }

    for (const auto bufferSize :
         {16, 32, 48, 64, 96, 128, 144, 160, 192, 224, 256, 480, 512, 1024, 2048, 4096})
      [bufferSizeSelection addItemWithTitle:[NSString stringWithFormat:@"%d", bufferSize]];
    const auto requestedBufferSize = selectedAudio.buffer_size != 0 ? selectedAudio.buffer_size : 256;
    const auto requestedBufferSizeTitle = [NSString stringWithFormat:@"%u", requestedBufferSize];
    if ([bufferSizeSelection indexOfItemWithTitle:requestedBufferSizeTitle] == -1)
    {
      [bufferSizeSelection addItemWithTitle:requestedBufferSizeTitle];
    }
    [bufferSizeSelection selectItemWithTitle:requestedBufferSizeTitle];

    const auto selectedMidi = selectedMidiPortIds;
    auto isSelected = [&selectedMidi](uint64_t portId) {
      return std::find(selectedMidi.begin(), selectedMidi.end(), portId) != selectedMidi.end();
    };
    auto *midiScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(16, 68, 488, 184)];
    [midiScroll setBorderType:NSBezelBorder];
    [midiScroll setHasVerticalScroller:YES];
    auto *midiView = [[NSView alloc]
        initWithFrame:NSMakeRect(0, 0, 466,
                                 std::max<CGFloat>(184, static_cast<CGFloat>(midiPorts.size() * 28)))];
    for (auto index = 0u; index < midiPorts.size(); ++index)
    {
      auto *button = [[NSButton alloc]
          initWithFrame:NSMakeRect(12, midiView.frame.size.height - 28 * (index + 1), 430, 24)];
      [button setButtonType:NSButtonTypeSwitch];
      [button setTitle:[[NSString alloc] initWithUTF8String:midiPorts[index].name]];
      [button setState:isSelected(midiPorts[index].id) ? NSControlStateValueOn : NSControlStateValueOff];
      [button setTarget:self];
      [button setAction:@selector(onSelectionChanged:)];
      [midiView addSubview:button];
      midiPortButtons.push_back(button);
    }
    [midiScroll setDocumentView:midiView];
    [[self contentView] addSubview:midiScroll];

    [[self contentView] addSubview:cancelButton];
  }
}

- (void)applySelections
{
  @autoreleasepool
  {
    clap_wrapper_standalone_audio_settings_t settings{};
    settings.struct_size = sizeof(settings);
    settings.sample_rate =
        static_cast<uint32_t>([[[sampleRateSelection selectedItem] title] integerValue]);
    settings.buffer_size =
        static_cast<uint32_t>([[[bufferSizeSelection selectedItem] title] integerValue]);

    const auto outputIndex = [outputSelection indexOfSelectedItem];
    if (outputIndex > 0)
    {
      if (![outputChannelSelection isEnabled])
      {
        [self showConfigurationFailure:@"Selected output does not support the required stereo route."
                              restored:YES];
        return;
      }
      const auto deviceIndex = [self selectedOutputDeviceIndex];
      if (deviceIndex < 0)
      {
        [self showConfigurationFailure:@"The system default output is unavailable." restored:YES];
        return;
      }
      const auto &device = outputDevices[static_cast<std::size_t>(deviceIndex)];
      settings.output_device_id = device.id;
      settings.output_channels =
          static_cast<uint32_t>([[[outputChannelSelection selectedItem] title] integerValue]);
      settings.flags |= CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED;
      if (outputIndex == 1)
        settings.flags |= CLAP_WRAPPER_STANDALONE_AUDIO_FOLLOW_DEFAULT_OUTPUT;
    }

    const auto inputIndex = [inputSelection indexOfSelectedItem];
    if (inputIndex > 0)
    {
      if (![inputChannelSelection isEnabled])
      {
        [self showConfigurationFailure:@"Selected input does not support a usable channel route."
                              restored:YES];
        return;
      }
      const auto &device = inputDevices[inputIndex - 1];
      settings.input_device_id = device.id;
      settings.input_channels =
          static_cast<uint32_t>([[[inputChannelSelection selectedItem] title] integerValue]);
      settings.flags |= CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED;
    }

    if (settings.flags == 0 || ![sampleRateSelection isEnabled] || settings.sample_rate == 0 ||
        settings.buffer_size == 0 ||
        ((settings.flags & CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED) != 0 &&
         (settings.input_channels == 0 || settings.input_channels > 2)) ||
        ((settings.flags & CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED) != 0 &&
         settings.output_channels != 2))
    {
      [self showConfigurationFailure:@"Select an audio input or output, sample rate, and buffer size."
                            restored:YES];
      return;
    }

    auto *standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
    const auto previousAudio = standaloneHost->services.selectedAudioSettings();
    const auto previousMidi = standaloneHost->services.selectedMidiPortIds();
    const auto wasRunning = standaloneHost->services.isAudioRunning();
    const auto restore = [&] {
      const auto restored = standaloneHost->restoreServiceSettings(previousAudio, previousMidi);
      const auto restoredMidi = restored && standaloneHost->rebuildMIDIEndpoints();
      return restoredMidi && (!wasRunning || standaloneHost->startAudioThread());
    };

    standaloneHost->stopAudioThread();
    standaloneHost->stopMIDIThread();
    standaloneHost->deactivatePlugin();
    standaloneHost->refreshAudioServiceSnapshot();
    standaloneHost->refreshMidiServiceSnapshot();

    if (!standaloneHost->services.applyAudioSettings(settings))
    {
      [self showConfigurationFailure:@"Selected audio configuration is not supported."
                            restored:restore()];
      return;
    }

    standaloneHost->setStartupAudio(
        static_cast<unsigned int>(settings.input_device_id), settings.input_channels,
        static_cast<unsigned int>(settings.output_device_id), settings.output_channels,
        static_cast<int>(settings.sample_rate),
        (settings.flags & CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED) != 0,
        (settings.flags & CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED) != 0,
        (settings.flags & CLAP_WRAPPER_STANDALONE_AUDIO_FOLLOW_DEFAULT_OUTPUT) != 0);
    standaloneHost->currentBufferSize = settings.buffer_size;

    bool midiApplied = true;
    for (auto index = 0u; index < midiPorts.size(); ++index)
      midiApplied = standaloneHost->services.setMidiPortOpen(
                        midiPorts[index].id, [midiPortButtons[index] state] == NSControlStateValueOn) &&
                    midiApplied;

    if (!midiApplied || !standaloneHost->rebuildMIDIEndpoints() || !standaloneHost->startAudioThread())
    {
      [self showConfigurationFailure:@"Unable to restart audio or selected MIDI inputs."
                            restored:restore()];
      return;
    }

    selectedAudio = standaloneHost->services.selectedAudioSettings();
    const auto actualBufferSize = selectedAudio.buffer_size;
    const auto actualBufferSizeTitle = [NSString stringWithFormat:@"%u", actualBufferSize];
    if ([bufferSizeSelection indexOfItemWithTitle:actualBufferSizeTitle] == -1)
      [bufferSizeSelection addItemWithTitle:actualBufferSizeTitle];
    [bufferSizeSelection selectItemWithTitle:actualBufferSizeTitle];
    [statusLabel setTextColor:[NSColor systemGreenColor]];
    [statusLabel setStringValue:[NSString
        stringWithFormat:@"Applied — %u Hz, %u samples", selectedAudio.sample_rate,
                         actualBufferSize]];
  }
}

- (void)defaultButtonPressed:(id)sender
{
  auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
  auto [in, ignoredOutput, sr] = standaloneHost->getDefaultAudioInOutSampleRate();
  (void)ignoredOutput;
  int idx = 1;
  for (const auto &device : inputDevices)
  {
    if (device.id == in)
    {
      [inputSelection selectItemAtIndex:idx];
    }
    idx++;
  }

  [outputSelection selectItemAtIndex:1];

  [self resetChannelSelections];
  [self resetSampleRateSelection];

  for (NSMenuItem *item in [sampleRateSelection itemArray])
  {
    const auto sri = [[item title] integerValue];
    if ((int)sr == (int)sri)
    {
      [sampleRateSelection selectItem:item];
    }
  }
  [self applySelections];
}

- (void)cancelButtonPressed:(id)sender
{
  @autoreleasepool
  {
    [self close];
  }
}

- (void)onAudioMenuChanged:(id)sender
{
  [self resetChannelSelections];
  [self resetSampleRateSelection];
  [self applySelections];
}

- (void)onSelectionChanged:(id)sender
{
  [self applySelections];
}

- (NSInteger)selectedOutputDeviceIndex
{
  const auto selection = [outputSelection indexOfSelectedItem];
  if (selection <= 0) return -1;
  if (selection >= 2) return selection - 2;

  auto *standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
  auto [ignoredInput, output, ignoredSampleRate] =
      standaloneHost->getDefaultAudioInOutSampleRate();
  (void)ignoredInput;
  (void)ignoredSampleRate;
  for (auto index = 0u; index < outputDevices.size(); ++index)
    if (outputDevices[index].id == output) return static_cast<NSInteger>(index);
  return -1;
}

- (void)resetChannelSelections
{
  [outputChannelSelection removeAllItems];
  const auto outputIndex = [outputSelection indexOfSelectedItem];
  if (outputIndex <= 0)
  {
    [outputChannelSelection addItemWithTitle:@"Disabled"];
    [outputChannelSelection setEnabled:NO];
  }
  else if ([self selectedOutputDeviceIndex] < 0 ||
           outputDevices[static_cast<std::size_t>([self selectedOutputDeviceIndex])].output_channels < 2)
  {
    [outputChannelSelection addItemWithTitle:@"Unavailable (stereo required)"];
    [outputChannelSelection setEnabled:NO];
  }
  else
  {
    [outputChannelSelection addItemWithTitle:@"2"];
    [outputChannelSelection setEnabled:YES];
  }

  [inputChannelSelection removeAllItems];
  const auto inputIndex = [inputSelection indexOfSelectedItem];
  if (inputIndex <= 0)
  {
    [inputChannelSelection addItemWithTitle:@"Disabled"];
    [inputChannelSelection setEnabled:NO];
    return;
  }

  const auto &input = inputDevices[inputIndex - 1];
  const auto channels = std::min(input.input_channels, 2u);
  if (channels == 0)
  {
    [inputChannelSelection addItemWithTitle:@"Unavailable"];
    [inputChannelSelection setEnabled:NO];
    return;
  }

  for (auto channel = 1u; channel <= channels; ++channel)
    [inputChannelSelection addItemWithTitle:[NSString stringWithFormat:@"%u", channel]];
  const auto preservesActiveInput =
      (selectedAudio.flags & CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED) != 0 &&
      selectedAudio.input_device_id == input.id && selectedAudio.input_channels != 0 &&
      selectedAudio.input_channels <= channels;
  const auto selectedChannels = preservesActiveInput ? selectedAudio.input_channels : channels;
  [inputChannelSelection selectItemWithTitle:[NSString stringWithFormat:@"%u", selectedChannels]];
  [inputChannelSelection setEnabled:YES];
}

- (void)resetSampleRateSelection
{
  [sampleRateSelection removeAllItems];
  auto *standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
  const auto preferredSampleRate = selectedAudio.sample_rate;
  for (const auto sampleRate : standaloneHost->getSampleRates())
    if (sampleRate >= minimumSampleRate && sampleRate <= maximumSampleRate)
      [sampleRateSelection addItemWithTitle:[NSString stringWithFormat:@"%u", sampleRate]];

  if ([sampleRateSelection numberOfItems] == 0)
  {
    [sampleRateSelection addItemWithTitle:@"No compatible sample rate"];
    [sampleRateSelection setEnabled:NO];
    return;
  }

  [sampleRateSelection setEnabled:YES];
  const auto selectedRate = [NSString stringWithFormat:@"%u", selectedAudio.sample_rate];
  const auto preferredRate = [NSString stringWithFormat:@"%u", preferredSampleRate];
  if ([sampleRateSelection indexOfItemWithTitle:selectedRate] != -1)
    [sampleRateSelection selectItemWithTitle:selectedRate];
  else if ([sampleRateSelection indexOfItemWithTitle:preferredRate] != -1)
    [sampleRateSelection selectItemWithTitle:preferredRate];
  else
    [sampleRateSelection selectItemAtIndex:0];
}

- (void)showConfigurationFailure:(NSString *)message restored:(BOOL)restored
{
  NSAlert *alert = [[NSAlert alloc] init];
  [alert setMessageText:@"Unable to configure audio/MIDI"];
  [alert setInformativeText:[message stringByAppendingFormat:
                                         @"\n%@",
                                         restored ? @"Previous configuration was restored."
                                                  : @"Previous configuration could not be restored."]];
  [alert addButtonWithTitle:@"OK"];
  [alert runModal];
}

@end
