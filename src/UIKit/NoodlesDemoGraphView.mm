// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT

#define GLES_SILENCE_DEPRECATION 1
#import <NoodlesDemo/UIKit/NoodlesDemoGraphView.h>

#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES3/gl.h>
#import <QuartzCore/QuartzCore.h>

#include <noodles/demo/GraphEditor.h>

#include <cmath>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace {

std::string ToStdString(NSString *value) {
  return value ? std::string(value.UTF8String) : std::string();
}

NSString *ToNSString(const std::string &value) {
  return [NSString stringWithUTF8String:value.c_str()] ?: @"";
}

} // namespace

@interface NoodlesDemoGraphView ()
- (void)scheduleRender;
- (void)renderIfNeeded;
- (void)teardownGL;
@end

@implementation NoodlesDemoGraphView {
  std::shared_ptr<noodles::demo::GraphEditor> _editor;
  NSString *_assetsPath;

  EAGLContext *_context;
  GLuint _framebuffer;
  GLuint _colorRenderbuffer;
  GLuint _depthRenderbuffer;
  GLint _bufferWidth;
  GLint _bufferHeight;
  BOOL _glReady;
  BOOL _editorInitialized;

  BOOL _needsRender;
  BOOL _renderScheduled;
  CADisplayLink *_displayLink;

  __weak UIView<NoodlesDemoPencilForwardingTarget> *_pencilTargetCache;
  UIView<NoodlesDemoPencilForwardingTarget> *_activeCanvasPencilTarget;
  __weak UITouch *_canvasPencilTouch;

  __weak UITouch *_graphPencilTouch;
  BOOL _pencilGraphActive;
  CGPoint _graphPencilLastPoint;
  BOOL _panOwnsEditor;
  BOOL _pinchOwnsEditor;

  NSUInteger _pencilPressSequence;
  CGPoint _pencilDownPoint;

  // Gesture ownership, decided at pointer-down and held until pointer-up.
  BOOL _backgroundOwnsPointer;
  BOOL _backgroundOwnsPinch;
  UIHoverGestureRecognizer *_hoverRecognizer;
}

+ (Class)layerClass {
  return [CAEAGLLayer class];
}

- (instancetype)initWithEditor:
                    (std::shared_ptr<noodles::demo::GraphEditor>)editor
                    assetsPath:(NSString *)assetsPath {
  return [self initWithFrame:CGRectZero
                      editor:std::move(editor)
                  assetsPath:assetsPath];
}

- (instancetype)initWithFrame:(CGRect)frame
                       editor:
                           (std::shared_ptr<noodles::demo::GraphEditor>)editor
                   assetsPath:(NSString *)assetsPath {
  self = [super initWithFrame:frame];
  if (!self)
    return nil;

  _editor = std::move(editor);
  _assetsPath = [assetsPath copy];

  // The graph is a transparent overlay by default. Fading is performed by the
  // editor's compositing pass, never UIView.alpha, so hit geometry is
  // unchanged.
  self.opaque = NO;
  self.backgroundColor = UIColor.clearColor;
  self.contentScaleFactor = UIScreen.mainScreen.scale;

  CAEAGLLayer *layer = (CAEAGLLayer *)self.layer;
  layer.opaque = NO;
  layer.drawableProperties = @{
    kEAGLDrawablePropertyRetainedBacking : @NO,
    kEAGLDrawablePropertyColorFormat : kEAGLColorFormatRGBA8,
  };

  [self wireEditorDelegate];
  [self addGraphGestureRecognizers];
  return self;
}

- (void)dealloc {
  [self cancelActivePencilRouting];
  if (_editor)
    _editor->setDelegate(noodles::demo::GraphEditDelegate{});
  [self teardownGL];
}

- (NSString *)assetsPath {
  return _assetsPath;
}

- (std::shared_ptr<noodles::demo::GraphEditor>)graphEditor {
  return _editor;
}

#pragma mark - Editor delegate

- (void)wireEditorDelegate {
  if (!_editor)
    return;

  __weak NoodlesDemoGraphView *weakSelf = self;
  noodles::demo::GraphEditDelegate delegate;
  delegate.beginEdit = [weakSelf]() {
    NoodlesDemoGraphView *view = weakSelf;
    if (view.onBeginEdit)
      view.onBeginEdit();
  };
  delegate.endEdit = [weakSelf]() {
    NoodlesDemoGraphView *view = weakSelf;
    if (view.onEndEdit)
      view.onEndEdit();
  };
  delegate.status = [weakSelf](const std::string &message) {
    NSString *text = ToNSString(message);
    dispatch_async(dispatch_get_main_queue(), ^{
      NoodlesDemoGraphView *view = weakSelf;
      if (view.onStatus)
        view.onStatus(text);
    });
  };
  delegate.requestRender = [weakSelf]() {
    NoodlesDemoGraphView *view = weakSelf;
    if (view)
      [view scheduleRender];
  };
  delegate.selectionChanged = [weakSelf](const std::string &nodeId) {
    NSString *identifier = ToNSString(nodeId);
    dispatch_async(dispatch_get_main_queue(), ^{
      NoodlesDemoGraphView *view = weakSelf;
      if (view.onSelectionChanged)
        view.onSelectionChanged(identifier);
    });
  };
  delegate.attributeActivated =
      [weakSelf](const std::string &nodeId,
                 const std::string &attributeName) {
        NSString *identifier = ToNSString(nodeId);
        NSString *attribute = ToNSString(attributeName);
        NoodlesDemoGraphView *view = weakSelf;
        if (view.onAttributeActivated)
          view.onAttributeActivated(identifier, attribute);
      };
  delegate.topologyEdited = [weakSelf]() {
    NoodlesDemoGraphView *view = weakSelf;
    if (view.onTopologyEdited)
      view.onTopologyEdited();
  };
  delegate.graphStructureChanged = [weakSelf]() {
    dispatch_async(dispatch_get_main_queue(), ^{
      NoodlesDemoGraphView *view = weakSelf;
      if (view.onGraphStructureChanged)
        view.onGraphStructureChanged();
    });
  };
  delegate.attributeEdited = [weakSelf](const std::string &nodeId,
                                        const std::string &attributeName,
                                        bool live) {
    NSString *identifier = ToNSString(nodeId);
    NSString *attribute = ToNSString(attributeName);
    NoodlesDemoGraphView *view = weakSelf;
    // GraphEditDelegate guarantees this callback is synchronous. Preserve that
    // contract so a live scrub can update a consuming renderer before the next
    // requested frame, matching the original iPad integration.
    if (view.onAttributeEdited) {
      view.onAttributeEdited(identifier, attribute, live ? YES : NO);
    }
  };
  delegate.configurationError = [weakSelf](const std::string &message) {
    NSString *text = ToNSString(message);
    dispatch_async(dispatch_get_main_queue(), ^{
      NoodlesDemoGraphView *view = weakSelf;
      if (view.onConfigurationError)
        view.onConfigurationError(text);
    });
  };
  _editor->setDelegate(std::move(delegate));
}

#pragma mark - OpenGL ES lifecycle

- (void)layoutSubviews {
  [super layoutSubviews];
  [self ensureGLReady];
  [self resizeBackingIfNeeded];
}

- (void)ensureGLReady {
  if (_glReady || CGRectIsEmpty(self.bounds) || !_editor)
    return;

  _context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
  if (!_context || ![EAGLContext setCurrentContext:_context]) {
    _context = nil;
    [self publishStatus:@"OpenGL ES 3 is unavailable"];
    return;
  }

  glGenFramebuffers(1, &_framebuffer);
  glGenRenderbuffers(1, &_colorRenderbuffer);
  glGenRenderbuffers(1, &_depthRenderbuffer);
  _glReady = YES;
}

- (void)resizeBackingIfNeeded {
  if (!_glReady || !_editor)
    return;
  [EAGLContext setCurrentContext:_context];

  CAEAGLLayer *layer = (CAEAGLLayer *)self.layer;
  const CGFloat scale = self.contentScaleFactor;
  const GLint requestedWidth =
      (GLint)std::floor(self.bounds.size.width * scale + 0.5);
  const GLint requestedHeight =
      (GLint)std::floor(self.bounds.size.height * scale + 0.5);
  if (requestedWidth <= 0 || requestedHeight <= 0)
    return;

  glBindRenderbuffer(GL_RENDERBUFFER, _colorRenderbuffer);
  [_context renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer];
  GLint width = 0;
  GLint height = 0;
  glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
  glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT,
                               &height);
  if (width <= 0 || height <= 0)
    return;

  const BOOL sizeChanged = width != _bufferWidth || height != _bufferHeight;
  _bufferWidth = width;
  _bufferHeight = height;

  glBindRenderbuffer(GL_RENDERBUFFER, _depthRenderbuffer);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);

  glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_RENDERBUFFER, _colorRenderbuffer);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, _depthRenderbuffer);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    [self publishStatus:@"Could not create the NoodlesDemo framebuffer"];
    return;
  }

  if (!_editorInitialized) {
    NSString *fontAtlas = [_assetsPath
        stringByAppendingPathComponent:@"fonts/Poppins-Regular.png"];
    NSString *fontMetrics = [_assetsPath
        stringByAppendingPathComponent:@"fonts/Poppins-Regular.json"];
    try {
      _editor->initializeGL(ToStdString(_assetsPath), ToStdString(fontAtlas),
                            ToStdString(fontMetrics));
      _editorInitialized = YES;
      _editor->resize(width, height, (float)scale);
      _editor->refresh();
      [self setNeedsRender];
    } catch (const std::exception &error) {
      [self publishStatus:ToNSString(error.what())];
    } catch (...) {
      [self publishStatus:@"NoodlesDemo GL initialization failed"];
    }
  } else if (sizeChanged) {
    _editor->resize(width, height, (float)scale);
    [self setNeedsRender];
  }
}

- (void)teardownGL {
  [_displayLink invalidate];
  _displayLink = nil;
  if (!_glReady)
    return;

  [EAGLContext setCurrentContext:_context];
  // The host's GL objects were created against this context, so they have to
  // go before it does — and before the editor's own teardown, mirroring the
  // order they were created in.
  if (self.onTeardownBackgroundGL)
    self.onTeardownBackgroundGL();
  if (_editor && _editorInitialized)
    _editor->cleanupGL();
  if (_framebuffer)
    glDeleteFramebuffers(1, &_framebuffer);
  if (_colorRenderbuffer)
    glDeleteRenderbuffers(1, &_colorRenderbuffer);
  if (_depthRenderbuffer)
    glDeleteRenderbuffers(1, &_depthRenderbuffer);
  _framebuffer = _colorRenderbuffer = _depthRenderbuffer = 0;

  if (EAGLContext.currentContext == _context) {
    [EAGLContext setCurrentContext:nil];
  }
  _context = nil;
  _bufferWidth = _bufferHeight = 0;
  _glReady = NO;
  _editorInitialized = NO;
}

#pragma mark - Render scheduling

- (void)setNeedsRender {
  [self scheduleRender];
}

- (void)scheduleRender {
  if (!NSThread.isMainThread) {
    __weak NoodlesDemoGraphView *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakSelf scheduleRender];
    });
    return;
  }

  _needsRender = YES;
  if (_displayLink || _renderScheduled)
    return;
  _renderScheduled = YES;
  __weak NoodlesDemoGraphView *weakSelf = self;
  dispatch_async(dispatch_get_main_queue(), ^{
    NoodlesDemoGraphView *view = weakSelf;
    if (!view)
      return;
    view->_renderScheduled = NO;
    [view renderIfNeeded];
  });
}

- (void)renderIfNeeded {
  if (_needsRender)
    [self renderNow];
}

- (void)renderNow {
  if (!_glReady || !_editorInitialized || !_editor)
    return;
  _needsRender = NO;
  [EAGLContext setCurrentContext:_context];
  glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
  if (self.onRenderBackground) {
    // The host draws first, into the same drawable; the editor then composites
    // the graph over it. This ordering is the whole point of the hook, and it
    // only works when the editor was told to blend rather than overwrite.
    self.onRenderBackground((CGFloat)_bufferWidth, (CGFloat)_bufferHeight,
                            self.contentScaleFactor);
  }
  _editor->renderFrame();
  glBindRenderbuffer(GL_RENDERBUFFER, _colorRenderbuffer);
  [_context presentRenderbuffer:GL_RENDERBUFFER];
}

- (void)startGestureDisplayLink {
  if (_displayLink)
    return;
  _displayLink =
      [CADisplayLink displayLinkWithTarget:self
                                  selector:@selector(onDisplayLink:)];
  [_displayLink addToRunLoop:NSRunLoop.mainRunLoop
                     forMode:NSRunLoopCommonModes];
}

- (void)stopGestureDisplayLink {
  [_displayLink invalidate];
  _displayLink = nil;
  [self renderIfNeeded];
}

- (void)onDisplayLink:(CADisplayLink *)displayLink {
  (void)displayLink;
  [self renderIfNeeded];
}

#pragma mark - Gesture routing

/// Single decision point for pointer ownership, shared by finger and Pencil.
/// Graph content always wins; otherwise the host may claim the gesture, and if
/// it declines the editor keeps its empty-canvas pan.
- (BOOL)routePointerDown:(CGPoint)point {
  _backgroundOwnsPointer = NO;
  if (!self.graphPanLock && self.onBackgroundPointerDown && _editor &&
      !_editor->hitsGraphElementAt(point.x, point.y)) {
    _backgroundOwnsPointer =
        self.onBackgroundPointerDown(point, NoodlesDemoInputModifierNone);
  }
  if (!_backgroundOwnsPointer)
    [self pointerDown:point];
  return _backgroundOwnsPointer;
}

- (void)routePointerMove:(CGPoint)point {
  if (_backgroundOwnsPointer) {
    if (self.onBackgroundPointerMove)
      self.onBackgroundPointerMove(point, NoodlesDemoInputModifierNone);
    return;
  }
  [self pointerMove:point];
}

- (void)routePointerUp:(CGPoint)point {
  if (_backgroundOwnsPointer) {
    if (self.onBackgroundPointerUp)
      self.onBackgroundPointerUp(point, NoodlesDemoInputModifierNone);
    _backgroundOwnsPointer = NO;
    return;
  }
  [self pointerUp:point];
}

#pragma mark - Finger gestures

- (void)addGraphGestureRecognizers {
  NSArray<NSNumber *> *directOnly = @[ @(UITouchTypeDirect) ];

  UIPanGestureRecognizer *pan =
      [[UIPanGestureRecognizer alloc] initWithTarget:self
                                              action:@selector(handlePan:)];
  pan.minimumNumberOfTouches = 1;
  pan.maximumNumberOfTouches = 1;
  pan.allowedTouchTypes = directOnly;
  pan.delegate = self;
  [self addGestureRecognizer:pan];

  UIPinchGestureRecognizer *pinch =
      [[UIPinchGestureRecognizer alloc] initWithTarget:self
                                                action:@selector(handlePinch:)];
  pinch.allowedTouchTypes = directOnly;
  pinch.delegate = self;
  [self addGestureRecognizer:pinch];

  UITapGestureRecognizer *tap =
      [[UITapGestureRecognizer alloc] initWithTarget:self
                                              action:@selector(handleTap:)];
  tap.allowedTouchTypes = directOnly;
  tap.delegate = self;
  [self addGestureRecognizer:tap];

  // Pencil hover, where the hardware reports it. Harmless elsewhere: the
  // recognizer simply never fires.
  _hoverRecognizer =
      [[UIHoverGestureRecognizer alloc] initWithTarget:self
                                                action:@selector(handleHover:)];
  _hoverRecognizer.delegate = self;
  [self addGestureRecognizer:_hoverRecognizer];

  UILongPressGestureRecognizer *longPress =
      [[UILongPressGestureRecognizer alloc]
          initWithTarget:self
                  action:@selector(handleLongPress:)];
  longPress.allowedTouchTypes = directOnly;
  longPress.minimumPressDuration = 0.55;
  longPress.allowableMovement = 12.0;
  longPress.delegate = self;
  [self addGestureRecognizer:longPress];
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)first
    shouldRecognizeSimultaneouslyWithGestureRecognizer:
        (UIGestureRecognizer *)second {
  (void)first;
  (void)second;
  return YES;
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)recognizer
       shouldReceiveTouch:(UITouch *)touch {
  (void)recognizer;
  return touch.type == UITouchTypeDirect && !_pencilGraphActive;
}

- (void)handlePan:(UIPanGestureRecognizer *)gesture {
  const CGPoint point = [gesture locationInView:self];
  switch (gesture.state) {
  case UIGestureRecognizerStateBegan: {
    if (_pencilGraphActive)
      return;
    _panOwnsEditor = YES;
    [self startGestureDisplayLink];
    // Recognition begins after UIKit's movement threshold. Reconstruct the
    // original down point so tiny ports and link bands retain their hit target.
    const CGPoint translation = [gesture translationInView:self];
    [self routePointerDown:CGPointMake(point.x - translation.x,
                                       point.y - translation.y)];
    break;
  }
  case UIGestureRecognizerStateChanged:
    if (_panOwnsEditor)
      [self routePointerMove:point];
    break;
  case UIGestureRecognizerStateEnded:
  case UIGestureRecognizerStateCancelled:
  case UIGestureRecognizerStateFailed:
    if (!_panOwnsEditor)
      return;
    _panOwnsEditor = NO;
    [self routePointerUp:point];
    [self stopFingerDisplayLinkIfIdle];
    break;
  default:
    break;
  }
}

- (void)handlePinch:(UIPinchGestureRecognizer *)gesture {
  const CGPoint anchor = [gesture locationInView:self];
  switch (gesture.state) {
  case UIGestureRecognizerStateBegan:
    if (_pencilGraphActive)
      return;
    _pinchOwnsEditor = YES;
    [self startGestureDisplayLink];
    // Same rule as the pointer stream: ownership is settled from where the
    // gesture started and is not revisited.
    _backgroundOwnsPinch = !self.graphPanLock && self.onBackgroundZoomBegin &&
                           _editor &&
                           !_editor->hitsGraphElementAt(anchor.x, anchor.y) &&
                           self.onBackgroundZoomBegin(anchor);
    if (!_backgroundOwnsPinch)
      [self pinchBegin];
    break;
  case UIGestureRecognizerStateChanged:
    if (!_pinchOwnsEditor)
      break;
    if (_backgroundOwnsPinch) {
      if (self.onBackgroundZoomUpdate)
        self.onBackgroundZoomUpdate(gesture.scale);
    } else {
      [self pinchUpdate:gesture.scale anchor:anchor];
    }
    break;
  case UIGestureRecognizerStateEnded:
  case UIGestureRecognizerStateCancelled:
  case UIGestureRecognizerStateFailed:
    if (!_pinchOwnsEditor)
      return;
    _pinchOwnsEditor = NO;
    if (_backgroundOwnsPinch) {
      if (self.onBackgroundZoomEnd)
        self.onBackgroundZoomEnd();
      _backgroundOwnsPinch = NO;
    } else {
      [self pinchEnd];
    }
    [self stopFingerDisplayLinkIfIdle];
    break;
  default:
    break;
  }
}

- (void)handleLongPress:(UILongPressGestureRecognizer *)gesture {
  if (gesture.state != UIGestureRecognizerStateBegan || _pencilGraphActive ||
      !_editor) {
    return;
  }
  const CGPoint point = [gesture locationInView:self];
  if (_editor->longPressAt(point.x, point.y))
    [self setNeedsRender];
}

- (void)handleTap:(UITapGestureRecognizer *)gesture {
  if (gesture.state != UIGestureRecognizerStateEnded || _pencilGraphActive) {
    return;
  }
  const CGPoint point = [gesture locationInView:self];
  [self routePointerDown:point];
  [self routePointerUp:point];
}

- (void)handleHover:(UIHoverGestureRecognizer *)gesture {
  if (!self.onBackgroundHover || self.graphPanLock || _panOwnsEditor ||
      _pinchOwnsEditor || _pencilGraphActive) {
    return;
  }
  if (gesture.state != UIGestureRecognizerStateBegan &&
      gesture.state != UIGestureRecognizerStateChanged) {
    return;
  }
  const CGPoint point = [gesture locationInView:self];
  if (_editor && _editor->hitsGraphElementAt(point.x, point.y))
    return;
  self.onBackgroundHover(point, NoodlesDemoInputModifierNone);
}

- (void)stopFingerDisplayLinkIfIdle {
  if (!_panOwnsEditor && !_pinchOwnsEditor)
    [self stopGestureDisplayLink];
}

#pragma mark - Pencil routing

- (UIView<NoodlesDemoPencilForwardingTarget> *)resolvePencilForwardingTarget {
  UIView<NoodlesDemoPencilForwardingTarget> *provided =
      self.pencilForwardingTarget;
  if (provided)
    return provided;

  UIView<NoodlesDemoPencilForwardingTarget> *cached = _pencilTargetCache;
  if (cached)
    return cached;
  UIWindow *window = self.window;
  if (!window)
    return nil;
  UIView<NoodlesDemoPencilForwardingTarget> *found =
      [self forwardingTargetInView:window];
  _pencilTargetCache = found;
  return found;
}

- (UIView<NoodlesDemoPencilForwardingTarget> *)forwardingTargetInView:
    (UIView *)view {
  if ([view conformsToProtocol:@protocol(NoodlesDemoPencilForwardingTarget)]) {
    return (UIView<NoodlesDemoPencilForwardingTarget> *)view;
  }
  for (UIView *subview in view.subviews) {
    UIView<NoodlesDemoPencilForwardingTarget> *found =
        [self forwardingTargetInView:subview];
    if (found)
      return found;
  }
  return nil;
}

- (void)finishCanvasPencilRouting {
  _canvasPencilTouch = nil;
  _activeCanvasPencilTarget = nil;
  _pencilTargetCache = nil;
}

- (void)cancelActivePencilRouting {
  UIView<NoodlesDemoPencilForwardingTarget> *target =
      _activeCanvasPencilTarget;
  [self finishCanvasPencilRouting];
  [target noodlesDemoCancelForwardedPencilGesture];

  // Removing the overlay does not require UIKit to synthesize
  // touchesCancelled:. Finish a graph-owned pointer stream explicitly too.
  // GraphEditor uses pointerUp to close live value scrubs (including the final
  // live=false attribute callback and end-edit bracket), so merely dropping the
  // UITouch would leave the edit envelope active.
  if (_pencilGraphActive) {
    [self routePointerUp:_graphPencilLastPoint];
    [self endGraphPencil];
  }
}

- (BOOL)hitsGraphElementAt:(CGPoint)point {
  return _editor && _editor->hitsGraphElementAt(point.x, point.y);
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
  NSMutableSet<UITouch *> *forwardedPencilTouches = nil;
  for (UITouch *touch in touches) {
    if (touch.type != UITouchTypePencil)
      continue;
    const CGPoint point = [touch locationInView:self];
    const BOOL graphRouteAvailable =
        !_pencilGraphActive && !_panOwnsEditor && !_pinchOwnsEditor;
    UIView<NoodlesDemoPencilForwardingTarget> *forwardingTarget = nil;
    const BOOL hitsGraph = [self hitsGraphElementAt:point];
    if (graphRouteAvailable && !hitsGraph) {
      forwardingTarget = [self resolvePencilForwardingTarget];
    }
    // A configured canvas keeps the sticky Pencil pass-through policy. With
    // no canvas, Pencil owns the same graph pointer stream as touch, including
    // empty-space panning and selection clearing.
    if (graphRouteAvailable && (hitsGraph || !forwardingTarget)) {
      _graphPencilTouch = touch;
      _pencilGraphActive = YES;
      _graphPencilLastPoint = point;
      [self startGestureDisplayLink];
      [self routePointerDown:point];

      _pencilDownPoint = point;
      const NSUInteger sequence = ++_pencilPressSequence;
      __weak NoodlesDemoGraphView *weakSelf = self;
      dispatch_after(
          dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.6 * NSEC_PER_SEC)),
          dispatch_get_main_queue(), ^{
            NoodlesDemoGraphView *view = weakSelf;
            if (!view || !view->_editor ||
                sequence != view->_pencilPressSequence ||
                !view->_pencilGraphActive ||
                view->_backgroundOwnsPointer) {
              // A background-owned Pencil stroke belongs to the host's tool
              // for its whole lifetime; it must never also remove a node.
              return;
            }
            UITouch *activeTouch = view->_graphPencilTouch;
            if (!activeTouch)
              return;
            const CGPoint current = [activeTouch locationInView:view];
            const CGFloat dx = current.x - view->_pencilDownPoint.x;
            const CGFloat dy = current.y - view->_pencilDownPoint.y;
            if (dx * dx + dy * dy > 12.0 * 12.0)
              return;
            if (view->_editor->longPressAt(current.x, current.y)) {
              [view setNeedsRender];
            }
          });
      continue;
    }

    if (!forwardedPencilTouches)
      forwardedPencilTouches = [NSMutableSet set];
    [forwardedPencilTouches addObject:touch];
  }

  if (forwardedPencilTouches.count > 0) {
    UIView<NoodlesDemoPencilForwardingTarget> *target =
        [self resolvePencilForwardingTarget];
    if (target) {
      _canvasPencilTouch = forwardedPencilTouches.anyObject;
      _activeCanvasPencilTarget = target;
      [target touchesBegan:forwardedPencilTouches withEvent:event];
    }
  }
  [super touchesBegan:touches withEvent:event];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
  NSMutableSet<UITouch *> *forwardedPencilTouches = nil;
  for (UITouch *touch in touches) {
    if (touch.type != UITouchTypePencil)
      continue;
    if (touch == _graphPencilTouch) {
      const CGPoint point = [touch locationInView:self];
      _graphPencilLastPoint = point;
      [self routePointerMove:point];
    } else {
      if (!forwardedPencilTouches)
        forwardedPencilTouches = [NSMutableSet set];
      [forwardedPencilTouches addObject:touch];
    }
  }
  if (forwardedPencilTouches.count > 0) {
    [_activeCanvasPencilTarget touchesMoved:forwardedPencilTouches
                                  withEvent:event];
  }
  [super touchesMoved:touches withEvent:event];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
  NSMutableSet<UITouch *> *forwardedPencilTouches = nil;
  for (UITouch *touch in touches) {
    if (touch.type != UITouchTypePencil)
      continue;
    if (touch == _graphPencilTouch) {
      const CGPoint point = [touch locationInView:self];
      _graphPencilLastPoint = point;
      [self routePointerUp:point];
      [self endGraphPencil];
    } else {
      if (!forwardedPencilTouches)
        forwardedPencilTouches = [NSMutableSet set];
      [forwardedPencilTouches addObject:touch];
    }
  }
  if (forwardedPencilTouches.count > 0) {
    [_activeCanvasPencilTarget touchesEnded:forwardedPencilTouches
                                  withEvent:event];
  }
  if ([forwardedPencilTouches containsObject:_canvasPencilTouch]) {
    [self finishCanvasPencilRouting];
  }
  [super touchesEnded:touches withEvent:event];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches
               withEvent:(UIEvent *)event {
  NSMutableSet<UITouch *> *forwardedPencilTouches = nil;
  for (UITouch *touch in touches) {
    if (touch.type != UITouchTypePencil)
      continue;
    if (touch == _graphPencilTouch) {
      // GraphEditor has no separate cancel operation. Finish the matched
      // pointer stream at its last location so no drag remains active. The
      // same applies to a host-owned stream, whose tool would otherwise stay
      // mid-stroke.
      const CGPoint point = [touch locationInView:self];
      _graphPencilLastPoint = point;
      [self routePointerUp:point];
      [self endGraphPencil];
    } else {
      if (!forwardedPencilTouches)
        forwardedPencilTouches = [NSMutableSet set];
      [forwardedPencilTouches addObject:touch];
    }
  }
  if (forwardedPencilTouches.count > 0) {
    [_activeCanvasPencilTarget touchesCancelled:forwardedPencilTouches
                                      withEvent:event];
  }
  if ([forwardedPencilTouches containsObject:_canvasPencilTouch]) {
    [self finishCanvasPencilRouting];
  }
  [super touchesCancelled:touches withEvent:event];
}

- (void)endGraphPencil {
  _graphPencilTouch = nil;
  _pencilGraphActive = NO;
  ++_pencilPressSequence;
  _pencilTargetCache = nil;
  [self stopGestureDisplayLink];
}

#pragma mark - Public editor forwarding

- (void)reloadGraph {
  if (!_editor)
    return;
  _editor->refresh();
  [self setNeedsRender];
}

- (BOOL)frameAllWithPadding:(CGFloat)padding {
  if (!_editor)
    return NO;
  return _editor->frameAll((double)padding) ? YES : NO;
}

- (void)pointerDown:(CGPoint)point {
  if (_editor)
    _editor->pointerDown(point.x, point.y);
}

- (void)pointerMove:(CGPoint)point {
  if (_editor)
    _editor->pointerMove(point.x, point.y);
}

- (void)pointerUp:(CGPoint)point {
  if (_editor)
    _editor->pointerUp(point.x, point.y);
}

- (void)pinchBegin {
  if (_editor)
    _editor->pinchBegin();
}

- (void)pinchUpdate:(CGFloat)scale anchor:(CGPoint)anchor {
  if (_editor)
    _editor->pinchUpdate(scale, anchor.x, anchor.y);
}

- (void)pinchEnd {
  if (_editor)
    _editor->pinchEnd();
}

- (void)setOverlayOpacity:(float)opacity {
  if (_editor)
    _editor->setOverlayOpacity(opacity);
  [self setNeedsRender];
}

- (void)setClearColorRed:(float)red
                   green:(float)green
                    blue:(float)blue
                   alpha:(float)alpha {
  if (_editor)
    _editor->setClearColor(red, green, blue, alpha);
  [self setNeedsRender];
}

- (void)setValueScrubEnabled:(BOOL)enabled {
  if (_editor)
    _editor->setValueScrubEnabled(enabled == YES);
}

- (void)setLinkFadingEnabled:(BOOL)enabled {
  if (_editor)
    _editor->setLinkFadingEnabled(enabled == YES);
}

- (void)setDisplayFrame:(double)frame {
  if (_editor)
    _editor->setDisplayFrame(frame);
}

- (NSString *)selectedNodeId {
  return _editor ? ToNSString(_editor->selectedNodeId()) : @"";
}

- (BOOL)addNode:(NSString *)nodeId atPoint:(CGPoint)point {
  if (!_editor)
    return NO;
  const bool added = _editor->addNodeAt(ToStdString(nodeId), point.x, point.y);
  if (added)
    [self setNeedsRender];
  return added ? YES : NO;
}

- (void)publishStatus:(NSString *)message {
  if (!self.onStatus)
    return;
  if (NSThread.isMainThread) {
    self.onStatus(message);
  } else {
    __weak NoodlesDemoGraphView *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      NoodlesDemoGraphView *view = weakSelf;
      if (view.onStatus)
        view.onStatus(message);
    });
  }
}

@end
