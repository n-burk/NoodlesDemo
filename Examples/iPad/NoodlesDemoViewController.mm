#import "NoodlesDemoViewController.h"

#import <NoodlesDemo/UIKit/NoodlesDemoGraphView.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "../AppleDemoImageLoader.h"
#include "../DemoGraphFixture.h"
#include "../DemoImageProcessor.h"

#include <noodles/demo/GraphEditor.h>
#include <noodles/demo/InMemoryGraphDocument.h>

#include <array>
#include <memory>
#include <string>
#include <utility>

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
  _activeVariant = 0;
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
  __weak NoodlesDemoViewController *weakMenuSelf = self;
  NSMutableArray<UIAction *> *addActions = [NSMutableArray array];
  for (int kind = 0; kind < noodles::demo::examples::kDemoOpKindCount; ++kind) {
    NSString *title = @(noodles::demo::examples::DemoOpKindTitle(
        static_cast<noodles::demo::examples::DemoOpKind>(kind)));
    [addActions addObject:[UIAction actionWithTitle:title
                                              image:nil
                                         identifier:nil
                                            handler:^(UIAction *action) {
                                              (void)action;
                                              [weakMenuSelf addDemoNodeOfKind:kind];
                                            }]];
  }
  addNode.menu = [UIMenu menuWithTitle:@"" children:addActions];
  addNode.showsMenuAsPrimaryAction = YES;

  NSMutableArray<NSString *> *graphTitles = [NSMutableArray array];
  for (int variant = 0; variant < noodles::demo::examples::kDemoGraphVariantCount; ++variant) {
    [graphTitles addObject:@(noodles::demo::examples::DemoGraphVariantTitle(
                     static_cast<noodles::demo::examples::DemoGraphVariant>(variant)))];
  }
  UISegmentedControl *graphPicker =
      [[UISegmentedControl alloc] initWithItems:graphTitles];
  graphPicker.selectedSegmentIndex = 0;
  graphPicker.apportionsSegmentWidthsByContent = YES;
  [graphPicker addTarget:self
                  action:@selector(graphChanged:)
        forControlEvents:UIControlEventValueChanged];

  UIStackView *controlRow = [[UIStackView alloc] initWithArrangedSubviews:@[
    graphPicker, _opacityLabel, opacitySlider, _frameLabel, frameSlider, fitGraph, addNode
  ]];
  controlRow.translatesAutoresizingMaskIntoConstraints = NO;
  controlRow.axis = UILayoutConstraintAxisHorizontal;
  controlRow.alignment = UIStackViewAlignmentCenter;
  controlRow.spacing = 10.0;
  [controls addSubview:controlRow];

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
    (void)nodeId;
    (void)attributeName;
    NoodlesDemoViewController *controller = weakSelf;
    if (controller) [controller refreshOutputImageLive:live];
  };
  _graphView.onTopologyEdited = ^{
    NoodlesDemoViewController *controller = weakSelf;
    if (controller) [controller refreshOutputImage];
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
  [self refreshOutputImage];
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

- (void)activateGraphVariant:(NSInteger)index {
  namespace demo = noodles::demo::examples;
  if (index < 0 || index >= demo::kDemoGraphVariantCount ||
      index == _activeVariant) {
    return;
  }
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
  _activeVariant = index;
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
