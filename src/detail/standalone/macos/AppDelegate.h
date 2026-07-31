#import <Cocoa/Cocoa.h>

// @class AudioSettingsWindowDelegate;

@interface ClapWrapperAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
{
  // AudioSettingsWindowDelegate *audioSettingsWindowDelegate;
}

@property(assign) NSTimer *requestCallbackTimer;
@property(assign) IBOutlet NSWindow *window;

- (IBAction)openAudioSettingsWindow:(id)sender;

@end
