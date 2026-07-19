#import <NoodlesDemo/UIKit/NoodlesDemoGraphView.h>
#import <UIKit/UIKit.h>

#include <noodles/demo/GraphEditor.h>

#include <memory>

@interface NoodlesDemoCompileSmokePencilTarget
    : UIView <NoodlesDemoPencilForwardingTarget>
@end

@implementation NoodlesDemoCompileSmokePencilTarget
- (void)noodlesDemoCancelForwardedPencilGesture {
}
@end

// Cross-compiled into an object library for the generic iOS device destination.
// It intentionally does not create a UIWindow or GL drawable; device/demo tests
// own runtime validation, while this pins the public Objective-C++ ABI and the
// Pencil target contract on every iOS build.
void NoodlesDemoCompileUIKitShellAPI() {
  auto editor = std::make_shared<noodles::demo::GraphEditor>();
  NoodlesDemoGraphView *view =
      [[NoodlesDemoGraphView alloc] initWithFrame:CGRectMake(0, 0, 1024, 768)
                                            editor:editor
                                        assetsPath:@"/tmp/noodles-assets"];
  NoodlesDemoCompileSmokePencilTarget *target =
      [[NoodlesDemoCompileSmokePencilTarget alloc] initWithFrame:CGRectZero];
  view.pencilForwardingTarget = target;
  view.onAttributeActivated = ^(NSString *nodeId, NSString *attributeName) {
    (void)nodeId;
    (void)attributeName;
  };
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
