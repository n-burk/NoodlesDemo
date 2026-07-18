#import "NoodlesDemoViewController.h"

#import <NoodlesApple/UIKit/NoodlesAppleGraphView.h>

#include "../DemoGraphFixture.h"
#include "../DemoImageProcessor.h"

#include <noodles/apple/GraphEditor.h>
#include <noodles/apple/InMemoryGraphDocument.h>

#include <algorithm>
#include <memory>

static UIColor *DemoBackgroundColor() {
  return [UIColor colorWithRed:0.055 green:0.065 blue:0.085 alpha:1.0];
}

// A deliberately small drawing surface proves the public Pencil policy without
// introducing an application-specific ink engine. The overlay forwards the
// original UITouch, including force/tilt/coalesced event identity, to this
// view.
@interface DemoPencilCanvas : UIView <NoodlesApplePencilForwardingTarget>
- (void)setOutputImage:(UIImage *)image;
@end

@implementation DemoPencilCanvas {
  UIImageView *_outputView;
  CAShapeLayer *_inkLayer;
  UIBezierPath *_activePath;
  __weak UITouch *_activeTouch;
}

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (!self)
    return nil;
  self.backgroundColor = DemoBackgroundColor();
  _outputView = [[UIImageView alloc] initWithFrame:self.bounds];
  _outputView.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  _outputView.contentMode = UIViewContentModeScaleAspectFill;
  _outputView.clipsToBounds = YES;
  _outputView.userInteractionEnabled = NO;
  [self addSubview:_outputView];

  // Keep the Pencil surface between the generated image and the transparent
  // graph overlay: the demo remains a real drawing host, not an image-only
  // graph sample.
  _inkLayer = [CAShapeLayer layer];
  _inkLayer.fillColor = UIColor.clearColor.CGColor;
  _inkLayer.strokeColor =
      [UIColor colorWithRed:0.35 green:0.76 blue:1.0 alpha:0.72].CGColor;
  _inkLayer.lineWidth = 3.0;
  _inkLayer.lineCap = kCALineCapRound;
  _inkLayer.lineJoin = kCALineJoinRound;
  [self.layer addSublayer:_inkLayer];
  return self;
}

- (void)setOutputImage:(UIImage *)image {
  _outputView.image = image;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  _inkLayer.frame = self.bounds;
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
  (void)event;
  UITouch *touch = touches.anyObject;
  if (!touch || touch.type != UITouchTypePencil || _activeTouch)
    return;
  _activeTouch = touch;
  _activePath = [UIBezierPath bezierPath];
  [_activePath moveToPoint:[touch locationInView:self]];
  _inkLayer.path = _activePath.CGPath;
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
  if (![touches containsObject:_activeTouch] || !_activePath)
    return;
  NSArray<UITouch *> *samples = [event coalescedTouchesForTouch:_activeTouch];
  if (samples.count == 0)
    samples = @[ _activeTouch ];
  for (UITouch *sample in samples) {
    [_activePath addLineToPoint:[sample locationInView:self]];
  }
  _inkLayer.path = _activePath.CGPath;
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
  [self touchesMoved:touches withEvent:event];
  if ([touches containsObject:_activeTouch]) {
    _activeTouch = nil;
    _activePath = nil;
  }
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches
               withEvent:(UIEvent *)event {
  (void)event;
  if ([touches containsObject:_activeTouch]) {
    _activeTouch = nil;
    _activePath = nil;
  }
}

- (void)noodlesAppleCancelForwardedPencilGesture {
  _activeTouch = nil;
  _activePath = nil;
}

@end

static UILabel *HudLabel(CGFloat size, UIFontWeight weight) {
  UILabel *label = [[UILabel alloc] initWithFrame:CGRectZero];
  label.translatesAutoresizingMaskIntoConstraints = NO;
  label.textColor = UIColor.whiteColor;
  label.font = [UIFont systemFontOfSize:size weight:weight];
  label.backgroundColor = [UIColor colorWithWhite:0.04 alpha:0.76];
  label.layer.cornerRadius = 8.0;
  label.layer.masksToBounds = YES;
  label.textAlignment = NSTextAlignmentCenter;
  label.adjustsFontSizeToFitWidth = YES;
  label.minimumScaleFactor = 0.72;
  return label;
}

static UILabel *ControlLabel(NSString *text) {
  UILabel *label = [[UILabel alloc] initWithFrame:CGRectZero];
  label.text = text;
  label.textColor = UIColor.whiteColor;
  label.font = [UIFont monospacedDigitSystemFontOfSize:12.0
                                               weight:UIFontWeightMedium];
  label.textAlignment = NSTextAlignmentCenter;
  return label;
}

static UIImage *ImageFromRgba(
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
  UIImage *result = cgImage ? [UIImage imageWithCGImage:cgImage] : nil;
  if (cgImage)
    CGImageRelease(cgImage);
  CGColorSpaceRelease(colorSpace);
  CGDataProviderRelease(provider);
  return result;
}

@interface NoodlesDemoViewController ()
- (void)refreshOutputImage;
@end

@implementation NoodlesDemoViewController {
  NSString *_assetsPath;
  noodles::apple::examples::DemoGraphFixture _fixture;
  DemoPencilCanvas *_canvas;
  NoodlesAppleGraphView *_graphView;
  UILabel *_statusLabel;
  UILabel *_selectionLabel;
  UILabel *_opacityLabel;
  UILabel *_frameLabel;
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
  DemoPencilCanvas *canvas =
      [[DemoPencilCanvas alloc] initWithFrame:CGRectZero];
  self.view = canvas;
  _canvas = canvas;
  _displayFrame = 12.0;

  _fixture = noodles::apple::examples::CreateDemoGraphFixture();
  _graphView = [[NoodlesAppleGraphView alloc] initWithFrame:canvas.bounds
                                                     editor:_fixture.editor
                                                 assetsPath:_assetsPath];
  _graphView.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  _graphView.pencilForwardingTarget = canvas;
  [canvas addSubview:_graphView];

  _statusLabel = HudLabel(12.0, UIFontWeightMedium);
  _statusLabel.text = @"Finger edits graph · Pencil draws through empty space";
  [canvas addSubview:_statusLabel];

  _selectionLabel = HudLabel(12.0, UIFontWeightRegular);
  _selectionLabel.text = @"No selection";
  [canvas addSubview:_selectionLabel];

  UIView *controls = [[UIView alloc] initWithFrame:CGRectZero];
  controls.translatesAutoresizingMaskIntoConstraints = NO;
  controls.backgroundColor = [UIColor colorWithWhite:0.04 alpha:0.82];
  controls.layer.cornerRadius = 10.0;
  [canvas addSubview:controls];

  _opacityLabel = ControlLabel(@"Opacity 50%");
  UISlider *opacitySlider = [[UISlider alloc] initWithFrame:CGRectZero];
  opacitySlider.minimumValue = 0.15f;
  opacitySlider.maximumValue = 1.0f;
  opacitySlider.value = 0.5f;
  opacitySlider.continuous = YES;
  [opacitySlider addTarget:self
                    action:@selector(opacityChanged:)
          forControlEvents:UIControlEventValueChanged];

  _frameLabel = ControlLabel(@"Frame 12.0");
  UISlider *frameSlider = [[UISlider alloc] initWithFrame:CGRectZero];
  frameSlider.minimumValue = 0.0f;
  frameSlider.maximumValue = 24.0f;
  frameSlider.value = 12.0f;
  frameSlider.continuous = YES;
  [frameSlider addTarget:self
                  action:@selector(frameChanged:)
        forControlEvents:UIControlEventValueChanged];

  UIButton *addSource = [UIButton buttonWithType:UIButtonTypeSystem];
  [addSource setTitle:@"Add Source" forState:UIControlStateNormal];
  [addSource setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
  addSource.titleLabel.font = [UIFont systemFontOfSize:12.0
                                               weight:UIFontWeightSemibold];
  addSource.backgroundColor =
      [UIColor colorWithRed:0.12 green:0.44 blue:0.78 alpha:0.9];
  addSource.layer.cornerRadius = 7.0;
  [addSource addTarget:self
                action:@selector(addSource:)
      forControlEvents:UIControlEventTouchUpInside];

  UIButton *fitGraph = [UIButton buttonWithType:UIButtonTypeSystem];
  [fitGraph setTitle:@"Fit" forState:UIControlStateNormal];
  [fitGraph setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
  fitGraph.titleLabel.font = [UIFont systemFontOfSize:12.0
                                               weight:UIFontWeightSemibold];
  fitGraph.backgroundColor =
      [UIColor colorWithWhite:0.22 alpha:0.9];
  fitGraph.layer.cornerRadius = 7.0;
  [fitGraph addTarget:self
               action:@selector(fitGraph:)
     forControlEvents:UIControlEventTouchUpInside];

  UIStackView *controlRow = [[UIStackView alloc]
      initWithArrangedSubviews:@[ _opacityLabel, opacitySlider, _frameLabel,
                                  frameSlider, addSource, fitGraph ]];
  controlRow.translatesAutoresizingMaskIntoConstraints = NO;
  controlRow.axis = UILayoutConstraintAxisHorizontal;
  controlRow.alignment = UIStackViewAlignmentCenter;
  controlRow.spacing = 10.0;
  [controls addSubview:controlRow];

  [NSLayoutConstraint activateConstraints:@[
    [_statusLabel.bottomAnchor
        constraintEqualToAnchor:canvas.safeAreaLayoutGuide.bottomAnchor
                       constant:-12.0],
    [_statusLabel.leadingAnchor constraintEqualToAnchor:canvas.leadingAnchor
                                               constant:12.0],
    [_statusLabel.trailingAnchor
        constraintLessThanOrEqualToAnchor:canvas.trailingAnchor
                              constant:-12.0],
    [_statusLabel.heightAnchor constraintEqualToConstant:32.0],
    [_selectionLabel.bottomAnchor constraintEqualToAnchor:_statusLabel.topAnchor
                                                 constant:-8.0],
    [_selectionLabel.leadingAnchor
        constraintEqualToAnchor:_statusLabel.leadingAnchor],
    [_selectionLabel.trailingAnchor
        constraintLessThanOrEqualToAnchor:canvas.trailingAnchor
                              constant:-12.0],
    [_selectionLabel.heightAnchor constraintEqualToConstant:28.0],
    [controls.topAnchor
        constraintEqualToAnchor:canvas.safeAreaLayoutGuide.topAnchor
                       constant:12.0],
    [controls.centerXAnchor constraintEqualToAnchor:canvas.centerXAnchor],
    [controls.leadingAnchor
        constraintGreaterThanOrEqualToAnchor:canvas.leadingAnchor
                                      constant:12.0],
    [controls.trailingAnchor
        constraintLessThanOrEqualToAnchor:canvas.trailingAnchor
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
    [addSource.heightAnchor constraintEqualToConstant:30.0],
    [fitGraph.widthAnchor constraintEqualToConstant:54.0],
    [fitGraph.heightAnchor constraintEqualToConstant:30.0],
  ]];

  __weak NoodlesDemoViewController *weakSelf = self;
  _graphView.onStatus = ^(NSString *message) {
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller)
      return;
    controller->_statusLabel.text = message.length > 0 ? message : @"Ready";
  };
  _graphView.onSelectionChanged = ^(NSString *nodeId) {
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller)
      return;
    controller->_selectionLabel.text =
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
  if (!_fixture.document || !_canvas)
    return;
  const noodles::apple::GraphSnapshot snapshot =
      _fixture.document->snapshot(_displayFrame);
  const noodles::apple::examples::DemoRgbaImage output =
      noodles::apple::examples::RenderDemoImage(snapshot, 640, 400);
  [_canvas setOutputImage:ImageFromRgba(output)];
}

- (void)opacityChanged:(UISlider *)sender {
  [_graphView setOverlayOpacity:sender.value];
  _opacityLabel.text =
      [NSString stringWithFormat:@"Opacity %.0f%%", sender.value * 100.0f];
}

- (void)frameChanged:(UISlider *)sender {
  _displayFrame = sender.value;
  [_graphView setDisplayFrame:sender.value];
  _frameLabel.text = [NSString stringWithFormat:@"Frame %.1f", sender.value];
  [self refreshOutputImage];
}

- (void)addSource:(UIButton *)sender {
  (void)sender;
  double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
  CGPoint drop = CGPointMake(CGRectGetMidX(_graphView.bounds),
                             CGRectGetMidY(_graphView.bounds));
  if (_fixture.editor->nodePosition("/Demo/Grade", &x, &y) &&
      _fixture.editor->nodeSize("/Demo/Grade", &w, &h)) {
    const double scale = std::max((double)_graphView.contentScaleFactor, 1.0);
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
  _statusLabel.text = added ? @"Added Source through the public host API"
                            : @"Source is already on the canvas";
}

- (void)fitGraph:(UIButton *)sender {
  (void)sender;
  _didFrameGraph = [_graphView frameAllWithPadding:32.0];
  if (_didFrameGraph)
    _statusLabel.text = @"Fit graph with 32-point padding";
}

- (void)viewDidDisappear:(BOOL)animated {
  [super viewDidDisappear:animated];
  [_graphView cancelActivePencilRouting];
}

- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  if (!_didFrameGraph) {
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
