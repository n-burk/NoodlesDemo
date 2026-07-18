#import <NoodlesApple/UIKit/NoodlesAppleGraphView.h>
#import <UIKit/UIKit.h>

#include <noodles/apple/GraphEditor.h>

#include <memory>

@interface NoodlesAppleCompileSmokePencilTarget
    : UIView <NoodlesApplePencilForwardingTarget>
@end

@implementation NoodlesAppleCompileSmokePencilTarget
- (void)noodlesAppleCancelForwardedPencilGesture {
}
@end

// Cross-compiled into an object library for the generic iOS device destination.
// It intentionally does not create a UIWindow or GL drawable; device/demo tests
// own runtime validation, while this pins the public Objective-C++ ABI and the
// Pencil target contract on every iOS build.
void NoodlesAppleCompileUIKitShellAPI() {
  auto editor = std::make_shared<noodles::apple::GraphEditor>();
  NoodlesAppleGraphView *view =
      [[NoodlesAppleGraphView alloc] initWithFrame:CGRectMake(0, 0, 1024, 768)
                                            editor:editor
                                        assetsPath:@"/tmp/noodles-assets"];
  NoodlesAppleCompileSmokePencilTarget *target =
      [[NoodlesAppleCompileSmokePencilTarget alloc] initWithFrame:CGRectZero];
  view.pencilForwardingTarget = target;
  [view setOverlayOpacity:0.75f];
  [view setClearColorRed:0 green:0 blue:0 alpha:0];
  [view setValueScrubEnabled:YES];
  [view setDisplayFrame:12.0];
  (void)[view frameAllWithPadding:24.0];
  [view pointerDown:CGPointMake(12, 16)];
  [view pointerUp:CGPointMake(12, 16)];
  [view pinchBegin];
  [view pinchUpdate:1.1 anchor:CGPointMake(512, 384)];
  [view pinchEnd];
  [view cancelActivePencilRouting];
}
