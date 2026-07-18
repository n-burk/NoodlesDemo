#import <UIKit/UIKit.h>

#import "NoodlesAppleiPadDemoViewController.h"

@interface NoodlesAppleiPadDemoAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation NoodlesAppleiPadDemoAppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
  (void)application;
  (void)launchOptions;
  NSString *assets = [NSBundle.mainBundle.resourcePath
      stringByAppendingPathComponent:@"noodles-assets"];
  NoodlesAppleiPadDemoViewController *controller =
      [[NoodlesAppleiPadDemoViewController alloc] initWithAssetsPath:assets];
  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  self.window.rootViewController = controller;
  [self.window makeKeyAndVisible];
  return YES;
}

@end

int main(int argc, char *argv[]) {
  @autoreleasepool {
    return UIApplicationMain(
        argc, argv, nil,
        NSStringFromClass(NoodlesAppleiPadDemoAppDelegate.class));
  }
}
