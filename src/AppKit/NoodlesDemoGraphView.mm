// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT

#define GL_SILENCE_DEPRECATION 1
#import <NoodlesDemo/AppKit/NoodlesDemoGraphView.h>

#import <OpenGL/gl3.h>

#include <noodles/demo/GraphEditor.h>

#include <algorithm>
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

NSOpenGLPixelFormat *GraphPixelFormat() {
  NSOpenGLPixelFormatAttribute attributes[] = {
      NSOpenGLPFAOpenGLProfile,
      NSOpenGLProfileVersion3_2Core,
      NSOpenGLPFAAccelerated,
      NSOpenGLPFADoubleBuffer,
      NSOpenGLPFAColorSize,
      24,
      NSOpenGLPFAAlphaSize,
      8,
      NSOpenGLPFADepthSize,
      24,
      0,
  };
  return [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
}

} // namespace

@interface NoodlesDemoGraphView ()
- (void)scheduleRender;
- (void)renderIfNeeded;
@end

@implementation NoodlesDemoGraphView {
  std::shared_ptr<noodles::demo::GraphEditor> _editor;
  NSString *_assetsPath;
  BOOL _editorInitialized;
  BOOL _needsRender;
  BOOL _renderScheduled;

  NSTimer *_gestureTimer;
  BOOL _mouseOwnsEditor;
  NSPoint _lastMousePoint;
  BOOL _scrollActive;
  BOOL _magnifyActive;
  CGFloat _magnifyScale;
}

- (instancetype)initWithEditor:
                    (std::shared_ptr<noodles::demo::GraphEditor>)editor
                    assetsPath:(NSString *)assetsPath {
  return [self initWithFrame:NSZeroRect
                      editor:std::move(editor)
                  assetsPath:assetsPath];
}

- (instancetype)initWithFrame:(NSRect)frame
                       editor:
                           (std::shared_ptr<noodles::demo::GraphEditor>)editor
                   assetsPath:(NSString *)assetsPath {
  self = [super initWithFrame:frame pixelFormat:GraphPixelFormat()];
  if (!self)
    return nil;

  _editor = std::move(editor);
  _assetsPath = [assetsPath copy];
  _magnifyScale = 1.0;
  self.wantsBestResolutionOpenGLSurface = YES;
  [self wireEditorDelegate];
  return self;
}

- (void)dealloc {
  [_gestureTimer invalidate];
  _gestureTimer = nil;
  if (_editor)
    _editor->setDelegate(noodles::demo::GraphEditDelegate{});

  NSOpenGLContext *context = self.openGLContext;
  [context makeCurrentContext];
  if (_editor && _editorInitialized)
    _editor->cleanupGL();
  [NSOpenGLContext clearCurrentContext];
}

- (BOOL)isFlipped {
  // Match UIKit and GraphEditor's public coordinate contract: origin top-left,
  // positive Y down, coordinates expressed in logical view points.
  return YES;
}

- (BOOL)isOpaque {
  return NO;
}

- (BOOL)acceptsFirstResponder {
  return YES;
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

#pragma mark - OpenGL lifecycle and rendering

- (void)prepareOpenGL {
  [super prepareOpenGL];
  if (!_editor || _editorInitialized)
    return;

  [self.openGLContext makeCurrentContext];
  GLint swapInterval = 1;
  [self.openGLContext setValues:&swapInterval
                   forParameter:NSOpenGLContextParameterSwapInterval];
  GLint surfaceOpaque = 0;
  [self.openGLContext setValues:&surfaceOpaque
                   forParameter:NSOpenGLContextParameterSurfaceOpacity];

  NSString *fontAtlas =
      [_assetsPath stringByAppendingPathComponent:@"fonts/Poppins-Regular.png"];
  NSString *fontMetrics = [_assetsPath
      stringByAppendingPathComponent:@"fonts/Poppins-Regular.json"];
  try {
    _editor->initializeGL(ToStdString(_assetsPath), ToStdString(fontAtlas),
                          ToStdString(fontMetrics));
    _editorInitialized = YES;
    [self resizeEditor];
    _editor->refresh();
    [self setNeedsRender];
  } catch (const std::exception &error) {
    [self publishStatus:ToNSString(error.what())];
  } catch (...) {
    [self publishStatus:@"NoodlesDemo OpenGL initialization failed"];
  }
}

- (void)reshape {
  [super reshape];
  [self resizeEditor];
}

- (void)resizeEditor {
  if (!_editor || !_editorInitialized || NSIsEmptyRect(self.bounds))
    return;
  [self.openGLContext makeCurrentContext];
  [self.openGLContext update];

  const NSRect backing = [self convertRectToBacking:self.bounds];
  const CGFloat scale = self.bounds.size.width > 0.0
                            ? backing.size.width / self.bounds.size.width
                            : 1.0;
  const int width = std::max(1, (int)std::lround(backing.size.width));
  const int height = std::max(1, (int)std::lround(backing.size.height));
  _editor->resize(width, height, (float)scale);
  [self setNeedsRender];
}

- (void)drawRect:(NSRect)dirtyRect {
  (void)dirtyRect;
  [self renderNow];
}

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
  if (_gestureTimer || _renderScheduled)
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
  if (!_editor || !_editorInitialized)
    return;
  _needsRender = NO;
  [self.openGLContext makeCurrentContext];
  _editor->renderFrame();
  [self.openGLContext flushBuffer];
}

- (void)startGestureTimer {
  if (_gestureTimer)
    return;
  __weak NoodlesDemoGraphView *weakSelf = self;
  _gestureTimer = [NSTimer timerWithTimeInterval:(1.0 / 60.0)
                                         repeats:YES
                                           block:^(NSTimer *timer) {
                                             (void)timer;
                                             [weakSelf renderIfNeeded];
                                           }];
  [NSRunLoop.mainRunLoop addTimer:_gestureTimer forMode:NSRunLoopCommonModes];
}

- (void)stopGestureTimerIfIdle {
  if (_mouseOwnsEditor || _scrollActive || _magnifyActive)
    return;
  [_gestureTimer invalidate];
  _gestureTimer = nil;
  [self renderIfNeeded];
}

#pragma mark - AppKit input

- (NSPoint)pointForEvent:(NSEvent *)event {
  return [self convertPoint:event.locationInWindow fromView:nil];
}

- (void)mouseDown:(NSEvent *)event {
  [self.window makeFirstResponder:self];
  _mouseOwnsEditor = YES;
  _lastMousePoint = [self pointForEvent:event];
  [self startGestureTimer];
  [self pointerDown:_lastMousePoint];
}

- (void)mouseDragged:(NSEvent *)event {
  if (_mouseOwnsEditor) {
    _lastMousePoint = [self pointForEvent:event];
    [self pointerMove:_lastMousePoint];
  }
}

- (void)mouseUp:(NSEvent *)event {
  if (!_mouseOwnsEditor)
    return;
  _mouseOwnsEditor = NO;
  _lastMousePoint = [self pointForEvent:event];
  [self pointerUp:_lastMousePoint];
  [self stopGestureTimerIfIdle];
}

- (void)rightMouseDown:(NSEvent *)event {
  if (!_editor)
    return;
  const NSPoint point = [self pointForEvent:event];
  if (_editor->longPressAt(point.x, point.y))
    [self setNeedsRender];
}

- (void)scrollWheel:(NSEvent *)event {
  if (!_editor)
    return;
  if (event.phase == NSEventPhaseBegan && !_scrollActive) {
    _scrollActive = YES;
    [self startGestureTimer];
  }

  const NSRect backing = [self convertRectToBacking:self.bounds];
  const double scale = self.bounds.size.width > 0.0
                           ? backing.size.width / self.bounds.size.width
                           : 1.0;
  const double zoom = std::max(0.0001, _editor->zoom());
  _editor->setViewportPan(
      _editor->panX() - event.scrollingDeltaX * scale / zoom,
      _editor->panY() - event.scrollingDeltaY * scale / zoom);
  [self setNeedsRender];

  const NSEventPhase terminal =
      NSEventPhaseEnded | NSEventPhaseCancelled | NSEventPhaseMayBegin;
  if ((event.phase & terminal) != 0 || event.phase == NSEventPhaseNone) {
    _scrollActive = NO;
    [self stopGestureTimerIfIdle];
  }
}

- (void)magnifyWithEvent:(NSEvent *)event {
  if (!_editor)
    return;
  const BOOL oneShot = event.phase == NSEventPhaseNone;
  if (!_magnifyActive || event.phase == NSEventPhaseBegan) {
    _magnifyActive = YES;
    _magnifyScale = 1.0;
    [self startGestureTimer];
    [self pinchBegin];
  }

  _magnifyScale *= std::max<CGFloat>(0.01, 1.0 + event.magnification);
  [self pinchUpdate:_magnifyScale anchor:[self pointForEvent:event]];

  if (oneShot || event.phase == NSEventPhaseEnded ||
      event.phase == NSEventPhaseCancelled) {
    _magnifyActive = NO;
    [self pinchEnd];
    [self stopGestureTimerIfIdle];
  }
}

- (void)cancelOperation:(id)sender {
  (void)sender;
  if (_mouseOwnsEditor) {
    _mouseOwnsEditor = NO;
    [self pointerUp:_lastMousePoint];
  }
  if (_magnifyActive) {
    _magnifyActive = NO;
    [self pinchEnd];
  }
  _scrollActive = NO;
  [self stopGestureTimerIfIdle];
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

- (void)pointerDown:(NSPoint)point {
  if (_editor)
    _editor->pointerDown(point.x, point.y);
}

- (void)pointerMove:(NSPoint)point {
  if (_editor)
    _editor->pointerMove(point.x, point.y);
}

- (void)pointerUp:(NSPoint)point {
  if (_editor)
    _editor->pointerUp(point.x, point.y);
}

- (void)pinchBegin {
  if (_editor)
    _editor->pinchBegin();
}

- (void)pinchUpdate:(CGFloat)scale anchor:(NSPoint)anchor {
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

- (BOOL)addNode:(NSString *)nodeId atPoint:(NSPoint)point {
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
