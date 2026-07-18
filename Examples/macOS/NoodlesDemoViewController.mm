#import "NoodlesDemoViewController.h"

#import <NoodlesApple/AppKit/NoodlesAppleGraphView.h>

#include "../DemoGraphFixture.h"
#include "../DemoImageProcessor.h"

#include <noodles/apple/GraphEditor.h>
#include <noodles/apple/InMemoryGraphDocument.h>

namespace {

NSColor *DemoBackgroundColor() {
  return [NSColor colorWithSRGBRed:0.055 green:0.065 blue:0.085 alpha:1.0];
}

NSTextField *HudLabel(NSString *text) {
  NSTextField *label = [NSTextField labelWithString:text];
  label.translatesAutoresizingMaskIntoConstraints = NO;
  label.textColor = NSColor.whiteColor;
  label.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightMedium];
  label.alignment = NSTextAlignmentCenter;
  label.wantsLayer = YES;
  label.layer.backgroundColor =
      [NSColor colorWithWhite:0.04 alpha:0.76].CGColor;
  label.layer.cornerRadius = 8.0;
  label.lineBreakMode = NSLineBreakByTruncatingTail;
  label.maximumNumberOfLines = 1;
  return label;
}

NSTextField *ControlLabel(NSString *text) {
  NSTextField *label = [NSTextField labelWithString:text];
  label.textColor = NSColor.whiteColor;
  label.font = [NSFont monospacedDigitSystemFontOfSize:12.0
                                               weight:NSFontWeightMedium];
  label.alignment = NSTextAlignmentCenter;
  label.lineBreakMode = NSLineBreakByClipping;
  return label;
}

NSImage *ImageFromRgba(
    const noodles::apple::examples::DemoRgbaImage &image) {
  if (image.empty())
    return nil;
  const size_t rowBytes = static_cast<size_t>(image.width) * 4;
  NSData *data = [NSData dataWithBytes:image.pixels.data()
                                length:image.pixels.size()];
  CGDataProviderRef provider =
      CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
  CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
  const CGBitmapInfo bitmapInfo =
      kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast;
  CGImageRef cgImage = CGImageCreate(
      static_cast<size_t>(image.width), static_cast<size_t>(image.height), 8,
      32, rowBytes, colorSpace, bitmapInfo, provider, nullptr, true,
      kCGRenderingIntentDefault);
  NSImage *result =
      cgImage ? [[NSImage alloc]
                    initWithCGImage:cgImage
                              size:NSMakeSize(image.width, image.height)]
              : nil;
  if (cgImage)
    CGImageRelease(cgImage);
  CGColorSpaceRelease(colorSpace);
  CGDataProviderRelease(provider);
  return result;
}

} // namespace

@interface NoodlesDemoViewController ()
- (void)refreshOutputImage;
@end

@implementation NoodlesDemoViewController {
  NSString *_assetsPath;
  noodles::apple::examples::DemoGraphFixture _fixture;
  NSImageView *_outputView;
  NoodlesAppleGraphView *_graphView;
  NSTextField *_statusLabel;
  NSTextField *_selectionLabel;
  NSTextField *_opacityLabel;
  NSTextField *_frameLabel;
  double _displayFrame;
  BOOL _didFrameGraph;
}

- (instancetype)initWithAssetsPath:(NSString *)assetsPath {
  self = [super initWithNibName:nil bundle:nil];
  if (!self)
    return nil;
  _assetsPath = [assetsPath copy];
  return self;
}

- (void)loadView {
  NSView *container =
      [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1200, 760)];
  container.wantsLayer = YES;
  container.layer.backgroundColor = DemoBackgroundColor().CGColor;
  self.view = container;

  _fixture = noodles::apple::examples::CreateDemoGraphFixture();
  _displayFrame = 12.0;
  _outputView = [[NSImageView alloc] initWithFrame:container.bounds];
  _outputView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  _outputView.imageAlignment = NSImageAlignCenter;
  _outputView.imageScaling = NSImageScaleProportionallyUpOrDown;
  _outputView.editable = NO;
  _outputView.animates = NO;
  [container addSubview:_outputView];

  _graphView = [[NoodlesAppleGraphView alloc] initWithFrame:container.bounds
                                                     editor:_fixture.editor
                                                 assetsPath:_assetsPath];
  _graphView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  [container addSubview:_graphView];

  _statusLabel = HudLabel(@"Drag to edit · Trackpad scroll pans · Pinch zooms");
  [container addSubview:_statusLabel];
  _selectionLabel = HudLabel(@"No selection");
  [container addSubview:_selectionLabel];

  NSView *controls = [[NSView alloc] initWithFrame:NSZeroRect];
  controls.translatesAutoresizingMaskIntoConstraints = NO;
  controls.wantsLayer = YES;
  controls.layer.backgroundColor =
      [NSColor colorWithWhite:0.04 alpha:0.82].CGColor;
  controls.layer.cornerRadius = 10.0;
  [container addSubview:controls];

  _opacityLabel = ControlLabel(@"Opacity 50%");
  NSSlider *opacitySlider = [NSSlider sliderWithValue:0.5
                                            minValue:0.15
                                            maxValue:1.0
                                              target:self
                                              action:@selector(opacityChanged:)];
  opacitySlider.continuous = YES;

  _frameLabel = ControlLabel(@"Frame 12.0");
  NSSlider *frameSlider = [NSSlider sliderWithValue:12.0
                                          minValue:0.0
                                          maxValue:24.0
                                            target:self
                                            action:@selector(frameChanged:)];
  frameSlider.continuous = YES;

  NSButton *addSource = [NSButton buttonWithTitle:@"Add Source"
                                           target:self
                                           action:@selector(addSource:)];
  addSource.bezelStyle = NSBezelStyleRounded;
  addSource.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightSemibold];

  NSButton *fitGraph = [NSButton buttonWithTitle:@"Fit"
                                          target:self
                                          action:@selector(fitGraph:)];
  fitGraph.bezelStyle = NSBezelStyleRounded;
  fitGraph.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightSemibold];

  NSStackView *controlRow = [NSStackView
      stackViewWithViews:@[ _opacityLabel, opacitySlider, _frameLabel,
                            frameSlider, addSource, fitGraph ]];
  controlRow.translatesAutoresizingMaskIntoConstraints = NO;
  controlRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  controlRow.alignment = NSLayoutAttributeCenterY;
  controlRow.spacing = 10.0;
  [controls addSubview:controlRow];

  [NSLayoutConstraint activateConstraints:@[
    [_statusLabel.bottomAnchor constraintEqualToAnchor:container.bottomAnchor
                                              constant:-12.0],
    [_statusLabel.leadingAnchor constraintEqualToAnchor:container.leadingAnchor
                                               constant:12.0],
    [_statusLabel.trailingAnchor
        constraintLessThanOrEqualToAnchor:container.trailingAnchor
                              constant:-12.0],
    [_statusLabel.heightAnchor constraintEqualToConstant:32.0],
    [_selectionLabel.bottomAnchor constraintEqualToAnchor:_statusLabel.topAnchor
                                                 constant:-8.0],
    [_selectionLabel.leadingAnchor
        constraintEqualToAnchor:_statusLabel.leadingAnchor],
    [_selectionLabel.trailingAnchor
        constraintLessThanOrEqualToAnchor:container.trailingAnchor
                              constant:-12.0],
    [_selectionLabel.heightAnchor constraintEqualToConstant:28.0],
    [controls.topAnchor constraintEqualToAnchor:container.topAnchor
                                        constant:12.0],
    [controls.centerXAnchor constraintEqualToAnchor:container.centerXAnchor],
    [controls.leadingAnchor
        constraintGreaterThanOrEqualToAnchor:container.leadingAnchor
                                      constant:12.0],
    [controls.trailingAnchor
        constraintLessThanOrEqualToAnchor:container.trailingAnchor
                                   constant:-12.0],
    [controlRow.topAnchor constraintEqualToAnchor:controls.topAnchor
                                          constant:8.0],
    [controlRow.bottomAnchor constraintEqualToAnchor:controls.bottomAnchor
                                             constant:-8.0],
    [controlRow.leadingAnchor constraintEqualToAnchor:controls.leadingAnchor
                                              constant:12.0],
    [controlRow.trailingAnchor constraintEqualToAnchor:controls.trailingAnchor
                                               constant:-12.0],
    [_opacityLabel.widthAnchor constraintEqualToConstant:82.0],
    [opacitySlider.widthAnchor constraintEqualToConstant:130.0],
    [_frameLabel.widthAnchor constraintEqualToConstant:82.0],
    [frameSlider.widthAnchor constraintEqualToConstant:150.0],
    [addSource.widthAnchor constraintEqualToConstant:96.0],
    [addSource.heightAnchor constraintEqualToConstant:28.0],
    [fitGraph.widthAnchor constraintEqualToConstant:54.0],
    [fitGraph.heightAnchor constraintEqualToConstant:28.0],
  ]];

  __weak NoodlesDemoViewController *weakSelf = self;
  _graphView.onStatus = ^(NSString *message) {
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller)
      return;
    controller->_statusLabel.stringValue =
        message.length > 0 ? message : @"Ready";
  };
  _graphView.onSelectionChanged = ^(NSString *nodeId) {
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller)
      return;
    controller->_selectionLabel.stringValue =
        nodeId.length > 0 ? [@"Selected  " stringByAppendingString:nodeId]
                          : @"No selection";
  };
  _graphView.onAttributeEdited =
      ^(NSString *nodeId, NSString *attributeName, BOOL live) {
        (void)nodeId;
        (void)attributeName;
        (void)live;
        NoodlesDemoViewController *controller = weakSelf;
        if (controller)
          [controller refreshOutputImage];
      };
  _graphView.onTopologyEdited = ^{
    NoodlesDemoViewController *controller = weakSelf;
    if (controller)
      [controller refreshOutputImage];
  };
  _graphView.onGraphStructureChanged = ^{
    NoodlesDemoViewController *controller = weakSelf;
    if (controller)
      [controller refreshOutputImage];
  };
  [self refreshOutputImage];
}

- (void)refreshOutputImage {
  if (!_fixture.document || !_outputView)
    return;
  const noodles::apple::GraphSnapshot snapshot =
      _fixture.document->snapshot(_displayFrame);
  const noodles::apple::examples::DemoRgbaImage output =
      noodles::apple::examples::RenderDemoImage(snapshot, 640, 400);
  _outputView.image = ImageFromRgba(output);
}

- (void)opacityChanged:(NSSlider *)sender {
  [_graphView setOverlayOpacity:(float)sender.doubleValue];
  _opacityLabel.stringValue =
      [NSString stringWithFormat:@"Opacity %.0f%%", sender.doubleValue * 100.0];
}

- (void)frameChanged:(NSSlider *)sender {
  _displayFrame = sender.doubleValue;
  [_graphView setDisplayFrame:sender.doubleValue];
  _frameLabel.stringValue =
      [NSString stringWithFormat:@"Frame %.1f", sender.doubleValue];
  [self refreshOutputImage];
}

- (void)addSource:(NSButton *)sender {
  (void)sender;
  // Put the host-added node below Color Grade rather than blindly at the
  // viewport center (which is often occupied). addNodeAt receives view points,
  // so project the chosen graph-space drop point through the public viewport.
  double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
  NSPoint drop = NSMakePoint(NSMidX(_graphView.bounds),
                            NSMidY(_graphView.bounds));
  if (_fixture.editor->nodePosition("/Demo/Grade", &x, &y) &&
      _fixture.editor->nodeSize("/Demo/Grade", &w, &h)) {
    const NSRect backing = [_graphView convertRectToBacking:_graphView.bounds];
    const double scale = _graphView.bounds.size.width > 0.0
                             ? backing.size.width / _graphView.bounds.size.width
                             : 1.0;
    const double worldX = x + w * 0.5;
    const double worldY = y + h + 70.0;
    drop.x = (worldX - _fixture.editor->panX()) *
             _fixture.editor->zoom() / scale;
    drop.y = (worldY - _fixture.editor->panY()) *
             _fixture.editor->zoom() / scale;
  }
  const BOOL added = [_graphView addNode:@"/Demo/Source" atPoint:drop];
  if (added)
    _didFrameGraph = [_graphView frameAllWithPadding:32.0];
  _statusLabel.stringValue = added
                                 ? @"Added Source through the public host API"
                                 : @"Source is already on the canvas";
}

- (void)fitGraph:(NSButton *)sender {
  (void)sender;
  _didFrameGraph = [_graphView frameAllWithPadding:32.0];
  if (_didFrameGraph)
    _statusLabel.stringValue = @"Fit graph with 32-point padding";
}

- (void)viewDidAppear {
  [super viewDidAppear];
  if (!_didFrameGraph) {
    // Defer until AppKit has committed the presented window's final backing
    // size and the OpenGL view has swapped fallback text metrics for the real
    // atlas. The same action remains user-accessible through Fit.
    __weak NoodlesDemoViewController *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      NoodlesDemoViewController *controller = weakSelf;
      if (controller && !controller->_didFrameGraph) {
        controller->_didFrameGraph =
            [controller->_graphView frameAllWithPadding:32.0];
      }
    });
  }
}

@end
