#import <UIKit/UIKit.h>

#import "NoodlesDemoViewController.h"

@interface NoodlesDemoAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation NoodlesDemoAppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
  (void)application;
  (void)launchOptions;
  NSString *assets = [NSBundle.mainBundle.resourcePath
      stringByAppendingPathComponent:@"noodles-assets"];
  NoodlesDemoViewController *controller =
      [[NoodlesDemoViewController alloc] initWithAssetsPath:assets];
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
        NSStringFromClass(NoodlesDemoAppDelegate.class));
  }
}
