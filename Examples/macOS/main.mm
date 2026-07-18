#import <AppKit/AppKit.h>

#import "NoodlesDemoViewController.h"

@interface NoodlesDemoAppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow *window;
@end

@implementation NoodlesDemoAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
  (void)notification;
  NSString *assets = [NSBundle.mainBundle.resourcePath
      stringByAppendingPathComponent:@"noodles-assets"];
  NoodlesDemoViewController *controller =
      [[NoodlesDemoViewController alloc] initWithAssetsPath:assets];
  self.window = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(0, 0, 1200, 760)
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                          NSWindowStyleMaskMiniaturizable |
                          NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  self.window.title = @"NoodlesDemo";
  self.window.contentViewController = controller;
  [self.window center];
  [self.window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:
    (NSApplication *)sender {
  (void)sender;
  return YES;
}

@end

static void InstallMainMenu(NSApplication *application) {
  // A programmatic AppKit application does not receive the main menu normally
  // supplied by a storyboard. Install a small, deterministic menu so Quit is
  // available and accessibility clients do not ask AppKit to synthesize and
  // inspect the host's entire recent-items menu.
  NSMenu *mainMenu = [[NSMenu alloc] initWithTitle:@""];
  NSMenuItem *applicationItem =
      [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
  [mainMenu addItem:applicationItem];

  NSMenu *applicationMenu = [[NSMenu alloc] initWithTitle:@"NoodlesDemo"];
  NSMenuItem *quitItem =
      [[NSMenuItem alloc] initWithTitle:@"Quit NoodlesDemo"
                                 action:@selector(terminate:)
                          keyEquivalent:@"q"];
  [applicationMenu addItem:quitItem];
  applicationItem.submenu = applicationMenu;
  application.mainMenu = mainMenu;
}

int main(int argc, const char *argv[]) {
  (void)argc;
  (void)argv;
  @autoreleasepool {
    NSApplication *application = NSApplication.sharedApplication;
    application.activationPolicy = NSApplicationActivationPolicyRegular;
    InstallMainMenu(application);
    NoodlesDemoAppDelegate *delegate = [[NoodlesDemoAppDelegate alloc] init];
    application.delegate = delegate;
    [application run];
  }
  return 0;
}
