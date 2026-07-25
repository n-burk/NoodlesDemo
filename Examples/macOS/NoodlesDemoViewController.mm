#import "NoodlesDemoViewController.h"

#import <NoodlesDemo/AppKit/NoodlesDemoGraphView.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "../AppleDemoImageLoader.h"
#include "../DemoGraphFixture.h"
#include "../DemoImageProcessor.h"
#include "HairGLRenderer.h"
#include "HairGraph.h"
#include "HairScene.h"

#include <noodles/demo/GraphEditor.h>
#include <noodles/demo/InMemoryGraphDocument.h>

#include <array>
#include <memory>
#include <string>
#include <utility>

namespace hair = noodles::demo::hair;

namespace {

// Control-bar order. Index 0 is the groom, which is what the app opens on;
// the image-processing graphs keep their original order after it.
constexpr NSInteger kHairSegment = 0;

const char *kToolTitles[] = {"No Tool", "Draw",  "Edit Pts",
                            "Comb",    "Clump", "Paint"};
constexpr int kToolCount = 6;

hair::HairToolKind ToolForSegment(NSInteger segment) {
  switch (segment) {
    case 1: return hair::HairToolKind::DrawGuides;
    case 2: return hair::HairToolKind::EditPoints;
    case 3: return hair::HairToolKind::CombBrush;
    case 4: return hair::HairToolKind::EditClump;
    case 5: return hair::HairToolKind::PaintClump;
    default: return hair::HairToolKind::None;
  }
}

NSInteger SegmentForTool(hair::HairToolKind kind) {
  switch (kind) {
    case hair::HairToolKind::DrawGuides: return 1;
    case hair::HairToolKind::EditPoints: return 2;
    case hair::HairToolKind::CombBrush: return 3;
    case hair::HairToolKind::EditClump: return 4;
    case hair::HairToolKind::PaintClump: return 5;
    case hair::HairToolKind::None: break;
  }
  return 0;
}

}  // namespace

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
  label.layer.backgroundColor = [NSColor colorWithWhite:0.04 alpha:0.76].CGColor;
  label.layer.cornerRadius = 8.0;
  label.lineBreakMode = NSLineBreakByTruncatingTail;
  label.maximumNumberOfLines = 1;
  return label;
}

NSTextField *ControlLabel(NSString *text) {
  NSTextField *label = [NSTextField labelWithString:text];
  label.textColor = NSColor.whiteColor;
  label.font = [NSFont monospacedDigitSystemFontOfSize:12.0 weight:NSFontWeightMedium];
  label.alignment = NSTextAlignmentCenter;
  label.lineBreakMode = NSLineBreakByClipping;
  return label;
}

NSImage *ImageFromRgba(const noodles::demo::examples::DemoRgbaImage &image) {
  if (image.empty()) return nil;
  const size_t rowBytes = static_cast<size_t>(image.width) * 4;
  NSData *data = [NSData dataWithBytes:image.pixels.data() length:image.pixels.size()];
  CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
  CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
  const CGBitmapInfo bitmapInfo = kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast;
  CGImageRef cgImage = CGImageCreate(
      static_cast<size_t>(image.width), static_cast<size_t>(image.height), 8, 32, rowBytes,
      colorSpace, bitmapInfo, provider, nullptr, true, kCGRenderingIntentDefault);
  NSImage *result =
      cgImage ? [[NSImage alloc] initWithCGImage:cgImage size:NSMakeSize(image.width, image.height)]
              : nil;
  if (cgImage) CGImageRelease(cgImage);
  CGColorSpaceRelease(colorSpace);
  CGDataProviderRelease(provider);
  return result;
}

}  // namespace

@interface NoodlesDemoViewController ()
- (void)refreshOutputImage;
- (void)refreshOutputImageLive:(BOOL)live;
- (void)presentSourceImagePicker;
- (void)loadSourceImageAtURL:(NSURL *)url;
- (void)showErrorToast:(NSString *)message;
- (void)setHairModeEnabled:(BOOL)enabled;
- (void)syncToolPicker;
- (void)addHairNodeOfKind:(NSInteger)kindIndex;
@end

@implementation NoodlesDemoViewController {
  NSString *_assetsPath;
  noodles::demo::examples::DemoGraphFixture _fixture;
  std::array<std::shared_ptr<noodles::demo::InMemoryGraphDocument>,
             noodles::demo::examples::kDemoGraphVariantCount>
      _documents;
  NSInteger _activeVariant;
  NSImageView *_outputView;
  NoodlesDemoGraphView *_graphView;
  NSTextField *_statusLabel;
  NSTextField *_selectionLabel;
  NSTextField *_opacityLabel;
  NSTextField *_frameLabel;
  NSTextField *_toastLabel;
  NSUInteger _toastGeneration;
  double _displayFrame;
  BOOL _didFrameGraph;
  noodles::demo::examples::DemoRgbaImage _sourceImage;
  NSString *_sourceImagePath;
  NSUInteger _sourceLoadGeneration;

  // Hair grooming demo. The scene is platform neutral and shared verbatim
  // with the iPad app; only the event translation below is AppKit specific.
  std::shared_ptr<noodles::demo::InMemoryGraphDocument> _hairDocument;
  std::unique_ptr<hair::HairScene> _hairScene;
  std::unique_ptr<hair::HairGLRenderer> _hairRenderer;
  BOOL _hairMode;
  // The viewport tools live in their own side panel, shown only while the
  // groom is up: they act on the 3D scene, not on the graph, so they do not
  // belong in the graph control bar.
  NSView *_toolPanel;
  NSMutableArray<NSButton *> *_toolButtons;
}

- (instancetype)initWithAssetsPath:(NSString *)assetsPath {
  self = [super initWithNibName:nil bundle:nil];
  if (!self) return nil;
  _assetsPath = [assetsPath copy];
  return self;
}

- (void)loadView {
  NSView *container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1200, 760)];
  container.wantsLayer = YES;
  container.layer.backgroundColor = DemoBackgroundColor().CGColor;
  self.view = container;

  _fixture = noodles::demo::examples::CreateDemoGraphFixture();
  _documents[0] = _fixture.document;
  _activeVariant = kHairSegment;
  _displayFrame = 12.0;

  // The groom the app opens on: six connected /Hair nodes, a procedural
  // scalp, and a deterministic seeded guide set.
  _hairDocument = hair::CreateHairGraphDocument();
  _hairScene = std::make_unique<hair::HairScene>(_hairDocument);
  _outputView = [[NSImageView alloc] initWithFrame:container.bounds];
  _outputView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  _outputView.imageAlignment = NSImageAlignCenter;
  _outputView.imageScaling = NSImageScaleProportionallyUpOrDown;
  _outputView.editable = NO;
  _outputView.animates = NO;
  [container addSubview:_outputView];

  _graphView = [[NoodlesDemoGraphView alloc] initWithFrame:container.bounds
                                                     editor:_fixture.editor
                                                 assetsPath:_assetsPath];
  _graphView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  [container addSubview:_graphView];

  _statusLabel = HudLabel(@"Tap Source Image path to choose · Trackpad scroll pans");
  [container addSubview:_statusLabel];
  _selectionLabel = HudLabel(@"No selection");
  [container addSubview:_selectionLabel];

  NSView *controls = [[NSView alloc] initWithFrame:NSZeroRect];
  controls.translatesAutoresizingMaskIntoConstraints = NO;
  controls.wantsLayer = YES;
  controls.layer.backgroundColor = [NSColor colorWithWhite:0.04 alpha:0.82].CGColor;
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

  NSButton *fitGraph = [NSButton buttonWithTitle:@"Fit" target:self action:@selector(fitGraph:)];
  fitGraph.bezelStyle = NSBezelStyleRounded;
  fitGraph.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightSemibold];

  NSButton *addNode = [NSButton buttonWithTitle:@"+ Node"
                                         target:self
                                         action:@selector(addDemoNode:)];
  addNode.bezelStyle = NSBezelStyleRounded;
  addNode.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightSemibold];

  NSMutableArray<NSString *> *graphTitles = [NSMutableArray arrayWithObject:@"Hair Groom"];
  for (int variant = 0; variant < noodles::demo::examples::kDemoGraphVariantCount; ++variant) {
    [graphTitles addObject:@(noodles::demo::examples::DemoGraphVariantTitle(
                     static_cast<noodles::demo::examples::DemoGraphVariant>(variant)))];
  }
  NSSegmentedControl *graphPicker =
      [NSSegmentedControl segmentedControlWithLabels:graphTitles
                                        trackingMode:NSSegmentSwitchTrackingSelectOne
                                              target:self
                                              action:@selector(graphChanged:)];
  graphPicker.selectedSegment = kHairSegment;
  graphPicker.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightMedium];

  NSStackView *controlRow = [NSStackView stackViewWithViews:@[
    graphPicker, _opacityLabel, opacitySlider, _frameLabel, frameSlider,
    fitGraph, addNode
  ]];
  controlRow.translatesAutoresizingMaskIntoConstraints = NO;
  controlRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  controlRow.alignment = NSLayoutAttributeCenterY;
  controlRow.spacing = 10.0;
  [controls addSubview:controlRow];

  // ── viewport tool panel ──
  _toolPanel = [[NSView alloc] initWithFrame:NSZeroRect];
  _toolPanel.translatesAutoresizingMaskIntoConstraints = NO;
  _toolPanel.wantsLayer = YES;
  _toolPanel.layer.backgroundColor = [NSColor colorWithWhite:0.04 alpha:0.82].CGColor;
  _toolPanel.layer.cornerRadius = 10.0;
  [container addSubview:_toolPanel];

  NSTextField *toolHeading = ControlLabel(@"Viewport");
  toolHeading.font = [NSFont systemFontOfSize:11.0 weight:NSFontWeightSemibold];
  toolHeading.textColor = NSColor.secondaryLabelColor;

  _toolButtons = [NSMutableArray array];
  NSMutableArray<NSView *> *panelViews = [NSMutableArray arrayWithObject:toolHeading];
  for (int tool = 0; tool < kToolCount; ++tool) {
    NSButton *button = [NSButton buttonWithTitle:@(kToolTitles[tool])
                                          target:self
                                          action:@selector(toolButtonPressed:)];
    // Push-on/push-off gives the armed tool a native filled appearance, so the
    // panel reads its state from the nodes rather than inventing a highlight.
    [button setButtonType:NSButtonTypePushOnPushOff];
    button.bezelStyle = NSBezelStyleRounded;
    button.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightMedium];
    button.tag = tool;
    [_toolButtons addObject:button];
    [panelViews addObject:button];
  }

  NSStackView *toolColumn = [NSStackView stackViewWithViews:panelViews];
  toolColumn.translatesAutoresizingMaskIntoConstraints = NO;
  toolColumn.orientation = NSUserInterfaceLayoutOrientationVertical;
  toolColumn.alignment = NSLayoutAttributeLeading;
  toolColumn.spacing = 6.0;
  [_toolPanel addSubview:toolColumn];

  _toastLabel = HudLabel(@"");
  _toastLabel.layer.backgroundColor =
      [NSColor colorWithSRGBRed:0.55 green:0.10 blue:0.12 alpha:0.92].CGColor;
  _toastLabel.hidden = YES;
  [container addSubview:_toastLabel];

  [NSLayoutConstraint activateConstraints:@[
    [_statusLabel.bottomAnchor constraintEqualToAnchor:container.bottomAnchor constant:-12.0],
    [_statusLabel.leadingAnchor constraintEqualToAnchor:container.leadingAnchor constant:12.0],
    [_statusLabel.trailingAnchor constraintLessThanOrEqualToAnchor:container.trailingAnchor
                                                          constant:-12.0],
    [_statusLabel.heightAnchor constraintEqualToConstant:32.0],
    [_selectionLabel.bottomAnchor constraintEqualToAnchor:_statusLabel.topAnchor constant:-8.0],
    [_selectionLabel.leadingAnchor constraintEqualToAnchor:_statusLabel.leadingAnchor],
    [_selectionLabel.trailingAnchor constraintLessThanOrEqualToAnchor:container.trailingAnchor
                                                             constant:-12.0],
    [_selectionLabel.heightAnchor constraintEqualToConstant:28.0],
    [controls.topAnchor constraintEqualToAnchor:container.topAnchor constant:12.0],
    [controls.centerXAnchor constraintEqualToAnchor:container.centerXAnchor],
    [controls.leadingAnchor constraintGreaterThanOrEqualToAnchor:container.leadingAnchor
                                                        constant:12.0],
    [controls.trailingAnchor constraintLessThanOrEqualToAnchor:container.trailingAnchor
                                                      constant:-12.0],
    [controlRow.topAnchor constraintEqualToAnchor:controls.topAnchor constant:8.0],
    [controlRow.bottomAnchor constraintEqualToAnchor:controls.bottomAnchor constant:-8.0],
    [controlRow.leadingAnchor constraintEqualToAnchor:controls.leadingAnchor constant:12.0],
    [controlRow.trailingAnchor constraintEqualToAnchor:controls.trailingAnchor constant:-12.0],
    [_opacityLabel.widthAnchor constraintEqualToConstant:82.0],
    [opacitySlider.widthAnchor constraintEqualToConstant:130.0],
    [_frameLabel.widthAnchor constraintEqualToConstant:82.0],
    [frameSlider.widthAnchor constraintEqualToConstant:150.0],
    [fitGraph.widthAnchor constraintEqualToConstant:54.0],
    [fitGraph.heightAnchor constraintEqualToConstant:28.0],
    [addNode.widthAnchor constraintEqualToConstant:72.0],
    [addNode.heightAnchor constraintEqualToConstant:28.0],
    [_toolPanel.leadingAnchor constraintEqualToAnchor:container.leadingAnchor constant:12.0],
    [_toolPanel.centerYAnchor constraintEqualToAnchor:container.centerYAnchor],
    [toolColumn.topAnchor constraintEqualToAnchor:_toolPanel.topAnchor constant:10.0],
    [toolColumn.bottomAnchor constraintEqualToAnchor:_toolPanel.bottomAnchor constant:-10.0],
    [toolColumn.leadingAnchor constraintEqualToAnchor:_toolPanel.leadingAnchor constant:10.0],
    [toolColumn.trailingAnchor constraintEqualToAnchor:_toolPanel.trailingAnchor constant:-10.0],
    [_toastLabel.topAnchor constraintEqualToAnchor:controls.bottomAnchor constant:10.0],
    [_toastLabel.centerXAnchor constraintEqualToAnchor:container.centerXAnchor],
    [_toastLabel.leadingAnchor constraintGreaterThanOrEqualToAnchor:container.leadingAnchor
                                                            constant:12.0],
    [_toastLabel.trailingAnchor constraintLessThanOrEqualToAnchor:container.trailingAnchor
                                                         constant:-12.0],
    [_toastLabel.heightAnchor constraintEqualToConstant:30.0],
  ]];

  __weak NoodlesDemoViewController *weakSelf = self;
  _graphView.onStatus = ^(NSString *message) {
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller) return;
    controller->_statusLabel.stringValue = message.length > 0 ? message : @"Ready";
  };
  _graphView.onSelectionChanged = ^(NSString *nodeId) {
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller) return;
    controller->_selectionLabel.stringValue =
        nodeId.length > 0 ? [@"Selected  " stringByAppendingString:nodeId] : @"No selection";
  };
  _graphView.onAttributeActivated = ^(NSString *nodeId, NSString *attributeName) {
    NoodlesDemoViewController *controller = weakSelf;
    if (controller && [nodeId isEqualToString:@"/Demo/SourceImage"] &&
        [attributeName isEqualToString:@"path"]) {
      [controller presentSourceImagePicker];
    }
  };
  _graphView.onAttributeEdited = ^(NSString *nodeId, NSString *attributeName, BOOL live) {
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller) return;
    if (controller->_hairMode) {
      // Value edits authored in the graph feed straight back into the groom,
      // including the tool: switches, whose exclusivity the scene enforces.
      controller->_hairScene->onAttributeEdited(nodeId.UTF8String,
                                                attributeName.UTF8String);
      [controller syncToolPicker];
      [controller->_graphView setNeedsRender];
      return;
    }
    [controller refreshOutputImageLive:live];
  };
  _graphView.onTopologyEdited = ^{
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller) return;
    if (controller->_hairMode) {
      controller->_hairScene->onTopologyEdited();
      [controller->_graphView setNeedsRender];
      controller->_statusLabel.stringValue =
          @(controller->_hairScene->result().status.empty()
                ? "Groom rewired"
                : controller->_hairScene->result().status.c_str());
      return;
    }
    [controller refreshOutputImage];
  };
  _graphView.onGraphStructureChanged = ^{
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller) return;
    // An external document topology change must restructure the editor's
    // graph, not just the rendered output (the shell schedules the refresh on
    // its serialized render thread through reloadGraph).
    [controller->_graphView reloadGraph];
    [controller refreshOutputImage];
  };
  _graphView.onConfigurationError = ^(NSString *message) {
    NoodlesDemoViewController *controller = weakSelf;
    if (controller) [controller showErrorToast:message];
  };

  // ── 3D viewport hooks ──
  //
  // The groom is drawn into the graph view's own drawable, immediately before
  // the editor composites the transparent graph over it. Everything below is
  // event translation; the decisions all live in the shared C++ scene.
  _graphView.onRenderBackground = ^(CGFloat width, CGFloat height, CGFloat scale) {
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller || !controller->_hairMode) return;
    if (!controller->_hairRenderer) {
      controller->_hairRenderer = std::make_unique<hair::HairGLRenderer>();
      if (!controller->_hairRenderer->initialize()) {
        NSString *message = @(controller->_hairRenderer->lastError().c_str());
        dispatch_async(dispatch_get_main_queue(), ^{
          NoodlesDemoViewController *inner = weakSelf;
          if (inner) inner->_statusLabel.stringValue = message;
        });
        return;
      }
    }
    controller->_hairRenderer->render(*controller->_hairScene, (int)width,
                                      (int)height, (float)scale);
  };
  _graphView.onTeardownBackgroundGL = ^{
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller || !controller->_hairRenderer) return;
    controller->_hairRenderer->shutdown();
    controller->_hairRenderer.reset();
  };
  _graphView.onBackgroundPointerDown = ^BOOL(CGPoint point,
                                             NoodlesDemoInputModifiers modifiers) {
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller || !controller->_hairMode) return NO;
    const bool claimed = controller->_hairScene->pointerDown(
        (float)point.x, (float)point.y,
        (modifiers & NoodlesDemoInputModifierPrimary) != 0);
    [controller->_graphView setNeedsRender];
    return claimed ? YES : NO;
  };
  _graphView.onBackgroundPointerMove = ^(CGPoint point,
                                         NoodlesDemoInputModifiers modifiers) {
    (void)modifiers;
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller || !controller->_hairMode) return;
    controller->_hairScene->pointerMove((float)point.x, (float)point.y);
    controller->_statusLabel.stringValue =
        @(controller->_hairScene->status().c_str());
    [controller->_graphView setNeedsRender];
  };
  _graphView.onBackgroundPointerUp = ^(CGPoint point,
                                       NoodlesDemoInputModifiers modifiers) {
    (void)modifiers;
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller || !controller->_hairMode) return;
    controller->_hairScene->pointerUp((float)point.x, (float)point.y);
    controller->_statusLabel.stringValue =
        @(controller->_hairScene->status().c_str());
    [controller->_graphView setNeedsRender];
  };
  _graphView.onBackgroundHover = ^(CGPoint point,
                                   NoodlesDemoInputModifiers modifiers) {
    (void)modifiers;
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller || !controller->_hairMode) return;
    controller->_hairScene->hoverMove((float)point.x, (float)point.y);
    [controller->_graphView setNeedsRender];
  };
  _graphView.onBackgroundZoomBegin = ^BOOL(CGPoint anchor) {
    (void)anchor;
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller || !controller->_hairMode) return NO;
    controller->_hairScene->pinchBegin();
    return YES;
  };
  _graphView.onBackgroundZoomUpdate = ^(CGFloat scale) {
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller || !controller->_hairMode) return;
    controller->_hairScene->pinchUpdate((float)scale);
    [controller->_graphView setNeedsRender];
  };
  _graphView.onBackgroundZoomEnd = ^{
    NoodlesDemoViewController *controller = weakSelf;
    if (controller && controller->_hairMode) controller->_hairScene->pinchEnd();
  };

  [self setHairModeEnabled:YES];
  [self refreshOutputImage];
}

#pragma mark - Hair grooming mode

- (void)setHairModeEnabled:(BOOL)enabled {
  _hairMode = enabled;
  _outputView.hidden = enabled;
  _toolPanel.hidden = !enabled;

  if (enabled) {
    _fixture.document = _hairDocument;
    _fixture.editor->setDocument(_hairDocument);
    // Without this the editor's composite pass would overwrite every pixel the
    // groom just drew into the shared drawable.
    _fixture.editor->setOverlayBlendsWithBackground(true);
    // Frame the whole groom, guide tips included, rather than trusting the
    // camera defaults to suit whatever the Scalp node is currently set to.
    _hairScene->frameGroom();
  } else {
    _fixture.editor->setOverlayBlendsWithBackground(false);
  }
  [_graphView reloadGraph];
  _didFrameGraph = [_graphView frameAllWithPadding:32.0];
  [self syncToolPicker];
  [_graphView setNeedsRender];
}

- (void)syncToolPicker {
  if (!_hairScene) return;
  const NSInteger active = SegmentForTool(_hairScene->activeTool());
  for (NSButton *button in _toolButtons) {
    button.state = button.tag == active ? NSControlStateValueOn
                                        : NSControlStateValueOff;
  }
}

- (void)toolButtonPressed:(NSButton *)sender {
  if (!_hairMode || !_hairScene) {
    [self syncToolPicker];
    return;
  }
  _hairScene->setActiveTool(ToolForSegment(sender.tag));
  // Re-read from the nodes rather than trusting the click: the scene may have
  // chosen a different node, or declined the tool entirely.
  [self syncToolPicker];
  _statusLabel.stringValue = @(_hairScene->status().c_str());
  [_graphView setNeedsRender];
}

- (void)showErrorToast:(NSString *)message {
  const NSUInteger generation = ++_toastGeneration;
  _toastLabel.stringValue = message.length > 0 ? message : @"Invalid graph";
  _toastLabel.hidden = NO;
  _toastLabel.alphaValue = 1.0;
  __weak NoodlesDemoViewController *weakSelf = self;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.4 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller || generation != controller->_toastGeneration) return;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) {
      context.duration = 0.3;
      controller->_toastLabel.animator.alphaValue = 0.0;
    } completionHandler:^{
      NoodlesDemoViewController *inner = weakSelf;
      if (!inner || generation != inner->_toastGeneration) return;
      inner->_toastLabel.hidden = YES;
      inner->_toastLabel.alphaValue = 1.0;
    }];
  });
}

- (void)addDemoNode:(NSButton *)sender {
  namespace demo = noodles::demo::examples;
  NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Add Node"];
  // The palette follows the active graph: /Hair node types while the groom is
  // up, image ops otherwise. Offering image ops to a groom would list nodes
  // that can never be wired into it.
  if (_hairMode) {
    for (int kind = 0; kind < hair::kHairNodeKindCount; ++kind) {
      NSMenuItem *item = [[NSMenuItem alloc]
          initWithTitle:@(hair::HairNodeKindTitle(
                            static_cast<hair::HairNodeKind>(kind)))
                 action:@selector(addNodeKindChosen:)
          keyEquivalent:@""];
      item.target = self;
      item.tag = kind;
      [menu addItem:item];
    }
  } else {
    for (int kind = 0; kind < demo::kDemoOpKindCount; ++kind) {
      NSMenuItem *item = [[NSMenuItem alloc]
          initWithTitle:@(demo::DemoOpKindTitle(static_cast<demo::DemoOpKind>(kind)))
                 action:@selector(addNodeKindChosen:)
          keyEquivalent:@""];
      item.target = self;
      item.tag = kind;
      [menu addItem:item];
    }
  }
  [menu popUpMenuPositioningItem:nil
                      atLocation:NSMakePoint(0.0, NSHeight(sender.bounds))
                          inView:sender];
}

- (void)addNodeKindChosen:(NSMenuItem *)item {
  if (_hairMode) {
    [self addHairNodeOfKind:item.tag];
    return;
  }
  [self addDemoNodeOfKind:item.tag];
}

- (void)addHairNodeOfKind:(NSInteger)kindIndex {
  if (!_fixture.editor || !_hairDocument) return;
  if (kindIndex < 0 || kindIndex >= hair::kHairNodeKindCount) return;
  const auto kind = static_cast<hair::HairNodeKind>(kindIndex);
  const std::string title = hair::HairNodeKindTitle(kind);
  std::string bare;
  for (const char character : title) {
    if (character != ' ') bare.push_back(character);
  }
  int index = 1;
  std::string nodeId = "/Hair/" + bare + "1";
  while (_hairDocument->containsNode(nodeId)) {
    ++index;
    nodeId = "/Hair/" + bare + std::to_string(index);
  }
  noodles::demo::GraphNode node =
      hair::MakeHairNode(kind, nodeId, title + " " + std::to_string(index));
  if (!_fixture.editor->createNodeAutoPlaced(std::move(node))) {
    _statusLabel.stringValue = @"Could not add a node";
    return;
  }
  _hairScene->onTopologyEdited();
  [_graphView setNeedsRender];
  _statusLabel.stringValue = [NSString
      stringWithFormat:@"Added %s — wire it in to change the groom", title.c_str()];
}

- (void)addDemoNodeOfKind:(NSInteger)kindIndex {
  namespace demo = noodles::demo::examples;
  if (!_fixture.editor || !_fixture.document) return;
  if (kindIndex < 0 || kindIndex >= demo::kDemoOpKindCount) return;
  const auto kind = static_cast<demo::DemoOpKind>(kindIndex);
  const std::string title = demo::DemoOpKindTitle(kind);
  int index = 1;
  std::string nodeId = "/Demo/" + title + "1";
  while (_fixture.document->containsNode(nodeId)) {
    ++index;
    nodeId = "/Demo/" + title + std::to_string(index);
  }
  noodles::demo::GraphNode node = demo::MakeDemoOpNode(
      kind, nodeId, title + " " + std::to_string(index));
  if (!_fixture.editor->createNodeAutoPlaced(std::move(node))) {
    _statusLabel.stringValue = @"Could not add a node";
  }
}

- (void)graphChanged:(NSSegmentedControl *)sender {
  [self activateGraphVariant:sender.selectedSegment];
}

- (void)activateGraphVariant:(NSInteger)segment {
  namespace demo = noodles::demo::examples;
  if (segment == _activeVariant) return;

  if (segment == kHairSegment) {
    _activeVariant = segment;
    [self setHairModeEnabled:YES];
    _statusLabel.stringValue = @"Graph: Hair Groom";
    _selectionLabel.stringValue = @"No selection";
    return;
  }

  const NSInteger index = segment - 1;
  if (index < 0 || index >= demo::kDemoGraphVariantCount) return;
  const auto variant = static_cast<demo::DemoGraphVariant>(index);
  auto &document = _documents[static_cast<std::size_t>(index)];
  if (!document) {
    document = demo::CreateDemoGraphDocument(variant);
    // A lazily built graph adopts the already-chosen source image so its
    // Source Image row matches what RenderDemoImage will show.
    if (_sourceImagePath.length > 0) {
      document->setStringAttributeValue("/Demo/SourceImage", "path",
                                        _sourceImagePath.UTF8String,
                                        _displayFrame);
    }
  }
  _activeVariant = segment;
  _hairMode = NO;
  _outputView.hidden = NO;
  _toolPanel.hidden = YES;
  // Back to the image demos: the graph owns the whole surface again, so the
  // cheaper unblended composite is correct.
  _fixture.editor->setOverlayBlendsWithBackground(false);
  _fixture.document = document;
  _fixture.editor->setDocument(document);
  [_graphView reloadGraph];
  _didFrameGraph = [_graphView frameAllWithPadding:32.0];
  [self refreshOutputImage];
  _statusLabel.stringValue =
      [NSString stringWithFormat:@"Graph: %s", demo::DemoGraphVariantTitle(variant)];
  _selectionLabel.stringValue = @"No selection";
}

- (void)refreshOutputImageLive:(BOOL)live {
  // The groom renders itself into the GL drawable; there is no 2D image to
  // recompute for it.
  if (_hairMode) return;
  if (!_fixture.document || !_outputView) return;
  const noodles::demo::GraphSnapshot snapshot = _fixture.document->snapshot(_displayFrame);
  const noodles::demo::examples::DemoRgbaImage *source =
      _sourceImage.empty() ? nullptr : &_sourceImage;
  const int width = live ? 320 : 640;
  const int height = live ? 200 : 400;
  const noodles::demo::examples::DemoRgbaImage output =
      noodles::demo::examples::RenderDemoImage(snapshot, width, height, source);
  _outputView.image = ImageFromRgba(output);
}

- (void)refreshOutputImage {
  [self refreshOutputImageLive:NO];
}

- (void)presentSourceImagePicker {
  NSOpenPanel *panel = NSOpenPanel.openPanel;
  panel.allowedContentTypes = @[ UTTypeImage ];
  panel.allowsMultipleSelection = NO;
  panel.canChooseDirectories = NO;
  panel.canChooseFiles = YES;
  __weak NoodlesDemoViewController *weakSelf = self;
  void (^completion)(NSModalResponse) = ^(NSModalResponse response) {
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller) return;
    if (response == NSModalResponseOK) {
      [controller loadSourceImageAtURL:panel.URL];
    } else {
      controller->_statusLabel.stringValue = @"Source image unchanged";
    }
  };
  if (self.view.window) {
    [panel beginSheetModalForWindow:self.view.window completionHandler:completion];
  } else {
    [panel beginWithCompletionHandler:completion];
  }
}

- (void)loadSourceImageAtURL:(NSURL *)url {
  if (!url) return;
  const NSUInteger generation = ++_sourceLoadGeneration;
  _statusLabel.stringValue =
      [NSString stringWithFormat:@"Loading source image %@…", url.lastPathComponent];
  NSString *path = [url.path copy];
  __weak NoodlesDemoViewController *weakSelf = self;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const BOOL accessed = [url startAccessingSecurityScopedResource];
    auto decoded = std::make_shared<noodles::demo::examples::DemoRgbaImage>();
    auto decodeError = std::make_shared<std::string>();
    __block NSError *coordinationError = nil;
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    [coordinator coordinateReadingItemAtURL:url
                                    options:NSFileCoordinatorReadingWithoutChanges
                                      error:&coordinationError
                                 byAccessor:^(NSURL *coordinatedURL) {
                                   *decoded = noodles::demo::examples::DecodeDemoImageAtURL(
                                       coordinatedURL, decodeError.get());
                                 }];
    if (accessed) [url stopAccessingSecurityScopedResource];

    NSString *failure = nil;
    if (coordinationError) {
      failure = coordinationError.localizedDescription;
    } else if (decoded->empty()) {
      failure = decodeError->empty() ? @"The selected image could not be loaded"
                                     : [NSString stringWithUTF8String:decodeError->c_str()];
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      NoodlesDemoViewController *controller = weakSelf;
      if (!controller || generation != controller->_sourceLoadGeneration) return;
      if (failure) {
        controller->_statusLabel.stringValue = failure;
        return;
      }
      controller->_sourceImage = std::move(*decoded);
      controller->_sourceImagePath = path;
      const char *utf8Path = path.UTF8String;
      // The decoded image feeds every graph's renderer, so author the path
      // into each already-built document, not only the visible one.
      for (const auto &document : controller->_documents) {
        if (!document) continue;
        document->setStringAttributeValue(
            "/Demo/SourceImage", "path", utf8Path ? utf8Path : "",
            controller->_displayFrame);
      }
      [controller refreshOutputImage];
      controller->_statusLabel.stringValue =
          [NSString stringWithFormat:@"Source image: %@", path.lastPathComponent];
    });
  });
}

- (void)opacityChanged:(NSSlider *)sender {
  [_graphView setOverlayOpacity:(float)sender.doubleValue];
  _opacityLabel.stringValue =
      [NSString stringWithFormat:@"Opacity %.0f%%", sender.doubleValue * 100.0];
}

- (void)frameChanged:(NSSlider *)sender {
  _displayFrame = sender.doubleValue;
  [_graphView setDisplayFrame:sender.doubleValue];
  _frameLabel.stringValue = [NSString stringWithFormat:@"Frame %.1f", sender.doubleValue];
  [self refreshOutputImage];
}

- (void)fitGraph:(NSButton *)sender {
  (void)sender;
  _didFrameGraph = [_graphView frameAllWithPadding:32.0];
  if (_didFrameGraph) _statusLabel.stringValue = @"Fit graph with 32-point padding";
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
        controller->_didFrameGraph = [controller->_graphView frameAllWithPadding:32.0];
      }
    });
  }
}

@end
