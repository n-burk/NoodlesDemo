#import "NoodlesDemoViewController.h"

#import <NoodlesDemo/UIKit/NoodlesDemoGraphView.h>
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

static UIColor *DemoBackgroundColor() {
  return [UIColor colorWithRed:0.055 green:0.065 blue:0.085 alpha:1.0];
}

// The generated output sits underneath the transparent graph overlay. This
// view deliberately does not adopt the optional Pencil-forwarding protocol, so
// Pencil and touch both remain graph-editing inputs in this demo.
@interface DemoOutputCanvas : UIView
- (void)setOutputImage:(UIImage *)image;
@end

@implementation DemoOutputCanvas {
  UIImageView *_outputView;
}

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (!self) return nil;
  self.backgroundColor = DemoBackgroundColor();
  _outputView = [[UIImageView alloc] initWithFrame:self.bounds];
  _outputView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  _outputView.contentMode = UIViewContentModeScaleAspectFill;
  _outputView.clipsToBounds = YES;
  _outputView.userInteractionEnabled = NO;
  [self addSubview:_outputView];
  return self;
}

- (void)setOutputImage:(UIImage *)image {
  _outputView.image = image;
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
  label.font = [UIFont monospacedDigitSystemFontOfSize:12.0 weight:UIFontWeightMedium];
  label.textAlignment = NSTextAlignmentCenter;
  return label;
}

static UIImage *ImageFromRgba(const noodles::demo::examples::DemoRgbaImage &image) {
  if (image.empty()) return nil;
  const size_t rowBytes = static_cast<size_t>(image.width) * 4;
  NSData *data = [NSData dataWithBytes:image.pixels.data() length:image.pixels.size()];
  CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
  CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
  const CGBitmapInfo bitmapInfo = kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast;
  CGImageRef cgImage = CGImageCreate(
      static_cast<size_t>(image.width), static_cast<size_t>(image.height), 8, 32, rowBytes,
      colorSpace, bitmapInfo, provider, nullptr, true, kCGRenderingIntentDefault);
  UIImage *result = cgImage ? [UIImage imageWithCGImage:cgImage] : nil;
  if (cgImage) CGImageRelease(cgImage);
  CGColorSpaceRelease(colorSpace);
  CGDataProviderRelease(provider);
  return result;
}

@interface NoodlesDemoViewController () <UIDocumentPickerDelegate>
- (void)refreshOutputImage;
- (void)refreshOutputImageLive:(BOOL)live;
- (void)presentSourceImagePicker;
- (void)loadSourceImageAtURL:(NSURL *)url;
- (void)showErrorToast:(NSString *)message;
- (void)setHairModeEnabled:(BOOL)enabled;
- (void)syncToolPicker;
- (void)updateGraphPanButton;
- (void)toolButtonPressed:(UIButton *)sender;
- (void)addHairNodeOfKind:(NSInteger)kindIndex;
- (NSArray<UIMenuElement *> *)addNodeMenuItems;
@end

@implementation NoodlesDemoViewController {
  NSString *_assetsPath;
  noodles::demo::examples::DemoGraphFixture _fixture;
  std::array<std::shared_ptr<noodles::demo::InMemoryGraphDocument>,
             noodles::demo::examples::kDemoGraphVariantCount>
      _documents;
  NSInteger _activeVariant;
  DemoOutputCanvas *_canvas;
  NoodlesDemoGraphView *_graphView;
  UILabel *_statusLabel;
  UILabel *_selectionLabel;
  UILabel *_opacityLabel;
  UILabel *_frameLabel;
  UILabel *_toastLabel;
  NSUInteger _toastGeneration;
  double _displayFrame;
  BOOL _didFrameGraph;
  noodles::demo::examples::DemoRgbaImage _sourceImage;
  NSString *_sourceImagePath;
  NSUInteger _sourceLoadGeneration;

  // Hair grooming demo. The scene is platform neutral and shared verbatim
  // with the macOS app; only the event translation below is UIKit specific.
  std::shared_ptr<noodles::demo::InMemoryGraphDocument> _hairDocument;
  std::unique_ptr<hair::HairScene> _hairScene;
  std::unique_ptr<hair::HairGLRenderer> _hairRenderer;
  BOOL _hairMode;
  // The viewport tools live in their own side panel, shown only while the
  // groom is up: they act on the 3D scene, not on the graph, so they do not
  // belong in the graph control bar.
  UIView *_toolPanel;
  NSMutableArray<UIButton *> *_toolButtons;
  UIButton *_graphPanButton;
}

- (instancetype)initWithAssetsPath:(NSString *)assetsPath {
  self = [super initWithNibName:nil bundle:nil];
  if (!self) return nil;
  _assetsPath = [assetsPath copy];
  return self;
}

- (void)loadView {
  DemoOutputCanvas *canvas = [[DemoOutputCanvas alloc] initWithFrame:CGRectZero];
  self.view = canvas;
  _canvas = canvas;
  _displayFrame = 12.0;

  _fixture = noodles::demo::examples::CreateDemoGraphFixture();
  _documents[0] = _fixture.document;
  _activeVariant = kHairSegment;

  // The groom the app opens on: six connected /Hair nodes, a procedural
  // scalp, and a deterministic seeded guide set.
  _hairDocument = hair::CreateHairGraphDocument();
  _hairScene = std::make_unique<hair::HairScene>(_hairDocument);
  _graphView = [[NoodlesDemoGraphView alloc] initWithFrame:canvas.bounds
                                                     editor:_fixture.editor
                                                 assetsPath:_assetsPath];
  _graphView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [canvas addSubview:_graphView];

  _statusLabel = HudLabel(12.0, UIFontWeightMedium);
  _statusLabel.text = @"Tap Source Image path to choose · Pencil and touch edit the graph";
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

  UIButton *fitGraph = [UIButton buttonWithType:UIButtonTypeSystem];
  [fitGraph setTitle:@"Fit" forState:UIControlStateNormal];
  [fitGraph setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
  fitGraph.titleLabel.font = [UIFont systemFontOfSize:12.0 weight:UIFontWeightSemibold];
  fitGraph.backgroundColor = [UIColor colorWithWhite:0.22 alpha:0.9];
  fitGraph.layer.cornerRadius = 7.0;
  [fitGraph addTarget:self
                action:@selector(fitGraph:)
      forControlEvents:UIControlEventTouchUpInside];

  UIButton *addNode = [UIButton buttonWithType:UIButtonTypeSystem];
  [addNode setTitle:@"+ Node" forState:UIControlStateNormal];
  [addNode setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
  addNode.titleLabel.font = [UIFont systemFontOfSize:12.0 weight:UIFontWeightSemibold];
  addNode.backgroundColor = [UIColor colorWithWhite:0.22 alpha:0.9];
  addNode.layer.cornerRadius = 7.0;
  // The palette follows the active graph, so it is rebuilt on every press:
  // /Hair node types while the groom is up, image ops otherwise.
  __weak NoodlesDemoViewController *weakMenuSelf = self;
  addNode.menu = [UIMenu
      menuWithTitle:@""
           children:@[ [UIDeferredMenuElement
                          elementWithProvider:^(void (^completion)(
                              NSArray<UIMenuElement *> *)) {
                            NoodlesDemoViewController *controller = weakMenuSelf;
                            completion(controller ? [controller addNodeMenuItems]
                                                  : @[]);
                          }] ]];
  addNode.showsMenuAsPrimaryAction = YES;

  NSMutableArray<NSString *> *graphTitles = [NSMutableArray arrayWithObject:@"Hair Groom"];
  for (int variant = 0; variant < noodles::demo::examples::kDemoGraphVariantCount; ++variant) {
    [graphTitles addObject:@(noodles::demo::examples::DemoGraphVariantTitle(
                     static_cast<noodles::demo::examples::DemoGraphVariant>(variant)))];
  }
  UISegmentedControl *graphPicker =
      [[UISegmentedControl alloc] initWithItems:graphTitles];
  graphPicker.selectedSegmentIndex = kHairSegment;
  graphPicker.apportionsSegmentWidthsByContent = YES;
  [graphPicker addTarget:self
                  action:@selector(graphChanged:)
        forControlEvents:UIControlEventValueChanged];

  // The touch equivalent of the desktop space-drag: while it is on, a
  // one-finger background drag pans the graph instead of orbiting the camera.
  _graphPanButton = [UIButton buttonWithType:UIButtonTypeSystem];
  [_graphPanButton setTitle:@"Graph Pan" forState:UIControlStateNormal];
  [_graphPanButton setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
  _graphPanButton.titleLabel.font = [UIFont systemFontOfSize:12.0
                                                      weight:UIFontWeightSemibold];
  _graphPanButton.backgroundColor = [UIColor colorWithWhite:0.22 alpha:0.9];
  _graphPanButton.layer.cornerRadius = 7.0;
  [_graphPanButton addTarget:self
                      action:@selector(graphPanToggled:)
            forControlEvents:UIControlEventTouchUpInside];

  UIStackView *controlRow = [[UIStackView alloc] initWithArrangedSubviews:@[
    graphPicker, _opacityLabel, opacitySlider, _frameLabel,
    frameSlider, fitGraph, addNode
  ]];
  controlRow.translatesAutoresizingMaskIntoConstraints = NO;
  controlRow.axis = UILayoutConstraintAxisHorizontal;
  controlRow.alignment = UIStackViewAlignmentCenter;
  controlRow.spacing = 10.0;
  [controls addSubview:controlRow];

  // ── viewport tool panel ──
  _toolPanel = [[UIView alloc] initWithFrame:CGRectZero];
  _toolPanel.translatesAutoresizingMaskIntoConstraints = NO;
  _toolPanel.backgroundColor = [UIColor colorWithWhite:0.04 alpha:0.82];
  _toolPanel.layer.cornerRadius = 10.0;
  [canvas addSubview:_toolPanel];

  UILabel *toolHeading = ControlLabel(@"Viewport");
  toolHeading.font = [UIFont systemFontOfSize:11.0 weight:UIFontWeightSemibold];
  toolHeading.textColor = [UIColor colorWithWhite:0.72 alpha:1.0];

  _toolButtons = [NSMutableArray array];
  NSMutableArray<UIView *> *panelViews =
      [NSMutableArray arrayWithObject:toolHeading];
  for (int tool = 0; tool < kToolCount; ++tool) {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    [button setTitle:@(kToolTitles[tool]) forState:UIControlStateNormal];
    [button setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    button.titleLabel.font = [UIFont systemFontOfSize:12.0
                                               weight:UIFontWeightSemibold];
    button.backgroundColor = [UIColor colorWithWhite:0.22 alpha:0.9];
    button.layer.cornerRadius = 7.0;
    button.tag = tool;
    [button addTarget:self
                  action:@selector(toolButtonPressed:)
        forControlEvents:UIControlEventTouchUpInside];
    [button.heightAnchor constraintEqualToConstant:34.0].active = YES;
    [_toolButtons addObject:button];
    [panelViews addObject:button];
  }

  // Graph Pan is a viewport interaction mode too, and is groom-only, so it
  // belongs beside the tools rather than in the graph control bar.
  [panelViews addObject:_graphPanButton];

  UIStackView *toolColumn =
      [[UIStackView alloc] initWithArrangedSubviews:panelViews];
  toolColumn.translatesAutoresizingMaskIntoConstraints = NO;
  toolColumn.axis = UILayoutConstraintAxisVertical;
  toolColumn.alignment = UIStackViewAlignmentFill;
  toolColumn.spacing = 6.0;
  [_toolPanel addSubview:toolColumn];

  _toastLabel = HudLabel(12.0, UIFontWeightMedium);
  _toastLabel.backgroundColor = [UIColor colorWithRed:0.55 green:0.10 blue:0.12 alpha:0.92];
  _toastLabel.hidden = YES;
  [canvas addSubview:_toastLabel];

  [NSLayoutConstraint activateConstraints:@[
    [_statusLabel.bottomAnchor constraintEqualToAnchor:canvas.safeAreaLayoutGuide.bottomAnchor
                                              constant:-12.0],
    [_statusLabel.leadingAnchor constraintEqualToAnchor:canvas.leadingAnchor constant:12.0],
    [_statusLabel.trailingAnchor constraintLessThanOrEqualToAnchor:canvas.trailingAnchor
                                                          constant:-12.0],
    [_statusLabel.heightAnchor constraintEqualToConstant:32.0],
    [_selectionLabel.bottomAnchor constraintEqualToAnchor:_statusLabel.topAnchor constant:-8.0],
    [_selectionLabel.leadingAnchor constraintEqualToAnchor:_statusLabel.leadingAnchor],
    [_selectionLabel.trailingAnchor constraintLessThanOrEqualToAnchor:canvas.trailingAnchor
                                                             constant:-12.0],
    [_selectionLabel.heightAnchor constraintEqualToConstant:28.0],
    [controls.topAnchor constraintEqualToAnchor:canvas.safeAreaLayoutGuide.topAnchor constant:12.0],
    [controls.centerXAnchor constraintEqualToAnchor:canvas.centerXAnchor],
    [controls.leadingAnchor constraintGreaterThanOrEqualToAnchor:canvas.leadingAnchor
                                                        constant:12.0],
    [controls.trailingAnchor constraintLessThanOrEqualToAnchor:canvas.trailingAnchor
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
    [fitGraph.heightAnchor constraintEqualToConstant:30.0],
    [addNode.widthAnchor constraintEqualToConstant:72.0],
    [addNode.heightAnchor constraintEqualToConstant:30.0],
    [_graphPanButton.heightAnchor constraintEqualToConstant:34.0],
    [_toolPanel.leadingAnchor constraintEqualToAnchor:canvas.safeAreaLayoutGuide.leadingAnchor
                                             constant:12.0],
    [_toolPanel.centerYAnchor constraintEqualToAnchor:canvas.centerYAnchor],
    [_toolPanel.widthAnchor constraintEqualToConstant:112.0],
    [toolColumn.topAnchor constraintEqualToAnchor:_toolPanel.topAnchor constant:10.0],
    [toolColumn.bottomAnchor constraintEqualToAnchor:_toolPanel.bottomAnchor constant:-10.0],
    [toolColumn.leadingAnchor constraintEqualToAnchor:_toolPanel.leadingAnchor constant:10.0],
    [toolColumn.trailingAnchor constraintEqualToAnchor:_toolPanel.trailingAnchor constant:-10.0],
    [_toastLabel.topAnchor constraintEqualToAnchor:controls.bottomAnchor constant:10.0],
    [_toastLabel.centerXAnchor constraintEqualToAnchor:canvas.centerXAnchor],
    [_toastLabel.leadingAnchor constraintGreaterThanOrEqualToAnchor:canvas.leadingAnchor
                                                            constant:12.0],
    [_toastLabel.trailingAnchor constraintLessThanOrEqualToAnchor:canvas.trailingAnchor
                                                         constant:-12.0],
    [_toastLabel.heightAnchor constraintEqualToConstant:30.0],
  ]];

  __weak NoodlesDemoViewController *weakSelf = self;
  _graphView.onStatus = ^(NSString *message) {
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller) return;
    controller->_statusLabel.text = message.length > 0 ? message : @"Ready";
  };
  _graphView.onSelectionChanged = ^(NSString *nodeId) {
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller) return;
    controller->_selectionLabel.text =
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
      controller->_statusLabel.text =
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
  // event translation; the decisions all live in the shared C++ scene, which
  // is byte-for-byte the same code the macOS app runs.
  _graphView.onRenderBackground = ^(CGFloat width, CGFloat height, CGFloat scale) {
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller || !controller->_hairMode) return;
    if (!controller->_hairRenderer) {
      controller->_hairRenderer = std::make_unique<hair::HairGLRenderer>();
      if (!controller->_hairRenderer->initialize()) {
        NSString *message = @(controller->_hairRenderer->lastError().c_str());
        dispatch_async(dispatch_get_main_queue(), ^{
          NoodlesDemoViewController *inner = weakSelf;
          if (inner) inner->_statusLabel.text = message;
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
    controller->_statusLabel.text = @(controller->_hairScene->status().c_str());
    [controller->_graphView setNeedsRender];
  };
  _graphView.onBackgroundPointerUp = ^(CGPoint point,
                                       NoodlesDemoInputModifiers modifiers) {
    (void)modifiers;
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller || !controller->_hairMode) return;
    controller->_hairScene->pointerUp((float)point.x, (float)point.y);
    controller->_statusLabel.text = @(controller->_hairScene->status().c_str());
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
  _toolPanel.hidden = !enabled;

  if (enabled) {
    [_graphView cancelActivePencilRouting];
    _fixture.document = _hairDocument;
    _fixture.editor->setDocument(_hairDocument);
    // Without this the editor's composite pass would overwrite every pixel the
    // groom just drew into the shared drawable.
    _fixture.editor->setOverlayBlendsWithBackground(true);
    // Frame the whole groom, guide tips included, rather than trusting the
    // camera defaults to suit whatever the Scalp node is currently set to.
    _hairScene->frameGroom();
    [_canvas setOutputImage:nil];
  } else {
    _fixture.editor->setOverlayBlendsWithBackground(false);
    _graphView.graphPanLock = NO;
    [self updateGraphPanButton];
  }
  [_graphView reloadGraph];
  _didFrameGraph = [_graphView frameAllWithPadding:32.0];
  [self syncToolPicker];
  [_graphView setNeedsRender];
}

- (void)syncToolPicker {
  if (!_hairScene) return;
  const NSInteger active = SegmentForTool(_hairScene->activeTool());
  for (UIButton *button in _toolButtons) {
    button.backgroundColor =
        button.tag == active
            ? [UIColor colorWithRed:0.20 green:0.45 blue:0.85 alpha:0.95]
            : [UIColor colorWithWhite:0.22 alpha:0.9];
  }
}

- (void)toolButtonPressed:(UIButton *)sender {
  if (!_hairMode || !_hairScene) {
    [self syncToolPicker];
    return;
  }
  _hairScene->setActiveTool(ToolForSegment(sender.tag));
  // Re-read from the nodes rather than trusting the tap: the scene may have
  // chosen a different node, or declined the tool entirely.
  [self syncToolPicker];
  _statusLabel.text = @(_hairScene->status().c_str());
  [_graphView setNeedsRender];
}

- (void)updateGraphPanButton {
  const BOOL locked = _graphView.graphPanLock;
  _graphPanButton.backgroundColor =
      locked ? [UIColor colorWithRed:0.20 green:0.45 blue:0.85 alpha:0.95]
             : [UIColor colorWithWhite:0.22 alpha:0.9];
}

- (void)graphPanToggled:(UIButton *)sender {
  (void)sender;
  _graphView.graphPanLock = !_graphView.graphPanLock;
  [self updateGraphPanButton];
  _statusLabel.text = _graphView.graphPanLock
                          ? @"Graph Pan: one-finger drag pans the graph"
                          : @"Graph Pan off: one-finger drag orbits the camera";
}

- (void)showErrorToast:(NSString *)message {
  const NSUInteger generation = ++_toastGeneration;
  _toastLabel.text = message.length > 0 ? message : @"Invalid graph";
  _toastLabel.hidden = NO;
  _toastLabel.alpha = 1.0;
  __weak NoodlesDemoViewController *weakSelf = self;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.4 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
    NoodlesDemoViewController *controller = weakSelf;
    if (!controller || generation != controller->_toastGeneration) return;
    [UIView animateWithDuration:0.3
        animations:^{
          controller->_toastLabel.alpha = 0.0;
        }
        completion:^(BOOL finished) {
          (void)finished;
          NoodlesDemoViewController *inner = weakSelf;
          if (!inner || generation != inner->_toastGeneration) return;
          inner->_toastLabel.hidden = YES;
          inner->_toastLabel.alpha = 1.0;
        }];
  });
}

- (NSArray<UIMenuElement *> *)addNodeMenuItems {
  __weak NoodlesDemoViewController *weakSelf = self;
  NSMutableArray<UIAction *> *actions = [NSMutableArray array];
  if (_hairMode) {
    for (int kind = 0; kind < hair::kHairNodeKindCount; ++kind) {
      NSString *title =
          @(hair::HairNodeKindTitle(static_cast<hair::HairNodeKind>(kind)));
      [actions addObject:[UIAction actionWithTitle:title
                                             image:nil
                                        identifier:nil
                                           handler:^(UIAction *action) {
                                             (void)action;
                                             [weakSelf addHairNodeOfKind:kind];
                                           }]];
    }
    return actions;
  }
  for (int kind = 0; kind < noodles::demo::examples::kDemoOpKindCount; ++kind) {
    NSString *title = @(noodles::demo::examples::DemoOpKindTitle(
        static_cast<noodles::demo::examples::DemoOpKind>(kind)));
    [actions addObject:[UIAction actionWithTitle:title
                                           image:nil
                                      identifier:nil
                                         handler:^(UIAction *action) {
                                           (void)action;
                                           [weakSelf addDemoNodeOfKind:kind];
                                         }]];
  }
  return actions;
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
    _statusLabel.text = @"Could not add a node";
    return;
  }
  _hairScene->onTopologyEdited();
  [_graphView setNeedsRender];
  _statusLabel.text = [NSString
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
    _statusLabel.text = @"Could not add a node";
  }
}

- (void)graphChanged:(UISegmentedControl *)sender {
  [self activateGraphVariant:sender.selectedSegmentIndex];
}

- (void)activateGraphVariant:(NSInteger)segment {
  namespace demo = noodles::demo::examples;
  if (segment == _activeVariant) return;

  if (segment == kHairSegment) {
    _activeVariant = segment;
    [self setHairModeEnabled:YES];
    _statusLabel.text = @"Graph: Hair Groom";
    _selectionLabel.text = @"No selection";
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
  [_graphView cancelActivePencilRouting];
  _activeVariant = segment;
  _hairMode = NO;
  _toolPanel.hidden = YES;
  // Back to the image demos: the graph owns the whole surface again, so the
  // cheaper unblended composite is correct.
  _fixture.editor->setOverlayBlendsWithBackground(false);
  _fixture.document = document;
  _fixture.editor->setDocument(document);
  [_graphView reloadGraph];
  _didFrameGraph = [_graphView frameAllWithPadding:32.0];
  [self refreshOutputImage];
  _statusLabel.text =
      [NSString stringWithFormat:@"Graph: %s", demo::DemoGraphVariantTitle(variant)];
  _selectionLabel.text = @"No selection";
}

- (void)refreshOutputImageLive:(BOOL)live {
  // The groom renders itself into the GL drawable; there is no 2D image to
  // recompute for it.
  if (_hairMode) return;
  if (!_fixture.document || !_canvas) return;
  const noodles::demo::GraphSnapshot snapshot = _fixture.document->snapshot(_displayFrame);
  const noodles::demo::examples::DemoRgbaImage *source =
      _sourceImage.empty() ? nullptr : &_sourceImage;
  const int width = live ? 320 : 640;
  const int height = live ? 200 : 400;
  const noodles::demo::examples::DemoRgbaImage output =
      noodles::demo::examples::RenderDemoImage(snapshot, width, height, source);
  [_canvas setOutputImage:ImageFromRgba(output)];
}

- (void)refreshOutputImage {
  [self refreshOutputImageLive:NO];
}

- (void)presentSourceImagePicker {
  UIDocumentPickerViewController *picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeImage ]
                                                                  asCopy:YES];
  picker.delegate = self;
  picker.allowsMultipleSelection = NO;
  [self presentViewController:picker animated:YES completion:nil];
}

- (void)loadSourceImageAtURL:(NSURL *)url {
  if (!url) return;
  const NSUInteger generation = ++_sourceLoadGeneration;
  _statusLabel.text =
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
        controller->_statusLabel.text = failure;
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
      controller->_statusLabel.text =
          [NSString stringWithFormat:@"Source image: %@", path.lastPathComponent];
    });
  });
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
  (void)controller;
  [self loadSourceImageAtURL:urls.firstObject];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
  (void)controller;
  _statusLabel.text = @"Source image unchanged";
}

- (void)opacityChanged:(UISlider *)sender {
  [_graphView setOverlayOpacity:sender.value];
  _opacityLabel.text = [NSString stringWithFormat:@"Opacity %.0f%%", sender.value * 100.0f];
}

- (void)frameChanged:(UISlider *)sender {
  _displayFrame = sender.value;
  [_graphView setDisplayFrame:sender.value];
  _frameLabel.text = [NSString stringWithFormat:@"Frame %.1f", sender.value];
  [self refreshOutputImage];
}

- (void)fitGraph:(UIButton *)sender {
  (void)sender;
  _didFrameGraph = [_graphView frameAllWithPadding:32.0];
  if (_didFrameGraph) _statusLabel.text = @"Fit graph with 32-point padding";
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
        controller->_didFrameGraph = [controller->_graphView frameAllWithPadding:32.0];
      }
    });
  }
}

@end
