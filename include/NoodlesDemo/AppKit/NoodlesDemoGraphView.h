// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT

// The public adapter intentionally hosts Noodles' OpenGL renderer. Suppress the
// SDK's blanket OpenGL deprecation attribute at declaration sites; consumers
// can still choose the UIKit shell or provide another GraphEditor presenter.
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION 1
#define NOODLES_DEMO_UNDEFINE_GL_SILENCE_DEPRECATION 1
#endif
#import <AppKit/AppKit.h>
#ifdef NOODLES_DEMO_UNDEFINE_GL_SILENCE_DEPRECATION
#undef GL_SILENCE_DEPRECATION
#undef NOODLES_DEMO_UNDEFINE_GL_SILENCE_DEPRECATION
#endif

#import <NoodlesDemo/NoodlesDemoBackgroundHost.h>

#ifdef __cplusplus
#include <memory>

namespace noodles::demo {
class GraphEditor;
}
#endif

NS_ASSUME_NONNULL_BEGIN

/// OpenGL 3.2 Core/AppKit presentation shell for noodles::demo::GraphEditor.
/// Mouse press/drag maps to the editor pointer stream, trackpad scroll pans,
/// magnify performs anchored zoom, and right-click is the desktop equivalent of
/// the iPad title-row long press.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
@interface NoodlesDemoGraphView : NSOpenGLView

#ifdef __cplusplus
- (instancetype)initWithFrame:(NSRect)frame
                       editor:(std::shared_ptr<noodles::demo::GraphEditor>)editor
                   assetsPath:(NSString *)assetsPath NS_DESIGNATED_INITIALIZER;

- (instancetype)initWithEditor:(std::shared_ptr<noodles::demo::GraphEditor>)editor
                    assetsPath:(NSString *)assetsPath;

- (std::shared_ptr<noodles::demo::GraphEditor>)graphEditor;
#endif

- (instancetype)initWithFrame:(NSRect)frame NS_UNAVAILABLE;
- (instancetype)initWithFrame:(NSRect)frame
                  pixelFormat:(nullable NSOpenGLPixelFormat *)format NS_UNAVAILABLE;
- (instancetype)initWithCoder:(NSCoder *)coder NS_UNAVAILABLE;

@property(nonatomic, copy, readonly) NSString *assetsPath;

- (void)reloadGraph;
- (BOOL)frameAllWithPadding:(CGFloat)padding;
- (void)setNeedsRender;
- (void)renderNow;

- (void)setOverlayOpacity:(float)opacity;
- (void)setClearColorRed:(float)red green:(float)green blue:(float)blue alpha:(float)alpha;
- (void)setValueScrubEnabled:(BOOL)enabled;
/// Distance-based fading of noodles far from the viewport center (on by
/// default); see GraphEditor::setLinkFadingEnabled.
- (void)setLinkFadingEnabled:(BOOL)enabled;
- (void)setDisplayFrame:(double)frame;
- (NSString *)selectedNodeId;
- (BOOL)addNode:(NSString *)nodeId atPoint:(NSPoint)point;

- (void)pointerDown:(NSPoint)point;
- (void)pointerMove:(NSPoint)point;
- (void)pointerUp:(NSPoint)point;
- (void)pinchBegin;
- (void)pinchUpdate:(CGFloat)scale anchor:(NSPoint)anchor;
- (void)pinchEnd;

@property(nonatomic, copy, nullable) void (^onBeginEdit)(void);
@property(nonatomic, copy, nullable) void (^onEndEdit)(void);
@property(nonatomic, copy, nullable) void (^onStatus)(NSString *message);
@property(nonatomic, copy, nullable) void (^onSelectionChanged)(NSString *nodeId);
/// A stationary middle-row tap on a display-only attribute. Hosts may present
/// contextual UI such as an asset picker; no document edit has occurred yet.
@property(nonatomic, copy, nullable) void (^onAttributeActivated)
    (NSString *nodeId, NSString *attributeName);
/// A successful editor-authored topology edit, delivered synchronously inside
/// the corresponding onBeginEdit/onEndEdit envelope.
@property(nonatomic, copy, nullable) void (^onTopologyEdited)(void);
/// An external document topology change; hosts normally schedule reloadGraph.
@property(nonatomic, copy, nullable) void (^onGraphStructureChanged)(void);
@property(nonatomic, copy, nullable) void (^onAttributeEdited)
    (NSString *nodeId, NSString *attributeName, BOOL live);
/// An editor-authored topology edit left the graph in an invalid configuration
/// (for example a feedback loop). The edit is still applied; hosts typically
/// present the message as a transient error toast.
@property(nonatomic, copy, nullable) void (^onConfigurationError)(NSString *message);

#pragma mark - Background host

/// Draw the host's own scene into the drawable, immediately before the editor
/// composites the graph over it. The GL context is current and the target
/// framebuffer is bound. Sizes are physical pixels.
///
/// A host using this must also call
/// GraphEditor::setOverlayBlendsWithBackground(true); otherwise the graph's
/// composite pass overwrites every pixel this block drew.
@property(nonatomic, copy, nullable) void (^onRenderBackground)
    (CGFloat widthPixels, CGFloat heightPixels, CGFloat contentScale);

/// Release the host's GL objects. Called with the context current, while it is
/// still valid, before the view tears down its own resources.
@property(nonatomic, copy, nullable) void (^onTeardownBackgroundGL)(void);

/// Consulted on pointer-down when the press did not land on graph content
/// (see GraphEditor::hitsGraphElementAt). Returning YES gives the host the
/// whole gesture: move and up are delivered to the background hooks and the
/// editor never sees them. Returning NO leaves the gesture with the editor,
/// which treats an empty-canvas drag as a graph pan.
///
/// Ownership is decided once, here, and is never revised mid-gesture.
@property(nonatomic, copy, nullable) BOOL (^onBackgroundPointerDown)
    (CGPoint point, NoodlesDemoInputModifiers modifiers);
@property(nonatomic, copy, nullable) void (^onBackgroundPointerMove)
    (CGPoint point, NoodlesDemoInputModifiers modifiers);
@property(nonatomic, copy, nullable) void (^onBackgroundPointerUp)
    (CGPoint point, NoodlesDemoInputModifiers modifiers);

/// Pointer motion with no button held, for hover feedback. Never delivered
/// while a gesture is in flight or over graph content.
@property(nonatomic, copy, nullable) void (^onBackgroundHover)
    (CGPoint point, NoodlesDemoInputModifiers modifiers);

/// Consulted when a magnify gesture begins away from graph content. Returning
/// YES routes the whole gesture to onBackgroundZoomUpdate instead of zooming
/// the graph.
@property(nonatomic, copy, nullable) BOOL (^onBackgroundZoomBegin)(CGPoint anchor);
@property(nonatomic, copy, nullable) void (^onBackgroundZoomUpdate)(CGFloat scale);
@property(nonatomic, copy, nullable) void (^onBackgroundZoomEnd)(void);

/// While YES, background gestures bypass the host hooks and go to the editor,
/// so graph panning and zooming stay reachable even when a host owns the
/// background. Held automatically while the space bar is down.
@property(nonatomic, assign) BOOL graphPanLock;

@end
#pragma clang diagnostic pop

NS_ASSUME_NONNULL_END
