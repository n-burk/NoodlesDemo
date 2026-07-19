// Copyright (c) 2026 NoodlesApple contributors.
// SPDX-License-Identifier: MIT

#import <UIKit/UIKit.h>

#ifdef __cplusplus
#include <memory>

namespace noodles::apple {
class GraphEditor;
}
#endif

NS_ASSUME_NONNULL_BEGIN

/// A drawing view underneath NoodlesAppleGraphView can adopt this marker to
/// receive Apple Pencil touches which miss graph content. The explicit cancel
/// callback closes the forwarded stream if the graph overlay is removed while
/// a Pencil is still down.
@protocol NoodlesApplePencilForwardingTarget <NSObject>
- (void)noodlesAppleCancelForwardedPencilGesture;
@end

/// Public OpenGL ES 3/UIKit presentation shell for noodles::apple::GraphEditor.
///
/// The view owns only the GL drawable and platform input lifecycle. The
/// supplied editor and its GraphDocument own graph data and edits. Finger
/// gestures always drive the graph. Pencil does the same unless it begins on
/// empty space and a pencilForwardingTarget can be resolved; that optional
/// route forwards the original touch unchanged for its whole lifetime.
///
/// This header intentionally requires Objective-C++ at construction sites: the
/// editor is shared with other public NoodlesApple adapters through
/// std::shared_ptr.
@interface NoodlesAppleGraphView : UIView <UIGestureRecognizerDelegate>

#ifdef __cplusplus
- (instancetype)initWithFrame:(CGRect)frame
                       editor:(std::shared_ptr<noodles::apple::GraphEditor>)editor
                   assetsPath:(NSString *)assetsPath NS_DESIGNATED_INITIALIZER;

- (instancetype)initWithEditor:(std::shared_ptr<noodles::apple::GraphEditor>)editor
                    assetsPath:(NSString *)assetsPath;

/// The exact editor supplied at initialization.
- (std::shared_ptr<noodles::apple::GraphEditor>)graphEditor;
#endif

- (instancetype)initWithFrame:(CGRect)frame NS_UNAVAILABLE;
- (instancetype)initWithCoder:(NSCoder *)coder NS_UNAVAILABLE;

/// Root containing Noodles' `shaders/` and `fonts/` directories. It is captured
/// at initialization so GL resources are always torn down against the same set.
@property(nonatomic, copy, readonly) NSString *assetsPath;

/// Optional explicit Pencil sink. If nil, resolvePencilForwardingTarget
/// searches the current window for the first view adopting the forwarding
/// protocol. If no target resolves, Pencil behaves like touch everywhere.
@property(nonatomic, weak, nullable)
    UIView<NoodlesApplePencilForwardingTarget> *pencilForwardingTarget;

/// Override to provide an application-specific target lookup. The default first
/// returns pencilForwardingTarget, then recursively searches the current
/// window.
- (nullable UIView<NoodlesApplePencilForwardingTarget> *)resolvePencilForwardingTarget;

/// Cancel either active Pencil route: a stream forwarded to the drawing target
/// or a graph-owned pointer stream. A graph value scrub receives its terminal
/// pointer-up so the live edit envelope closes. Idempotent.
- (void)cancelActivePencilRouting;

/// Rebuild the editor from its current GraphDocument and request a frame.
- (void)reloadGraph;
/// Fit every visible node in the current drawable without zooming above 1:1.
/// Returns NO until the view has a drawable size or for an empty graph.
- (BOOL)frameAllWithPadding:(CGFloat)padding;

/// Coalesced on-demand rendering. During gestures a CADisplayLink is alive only
/// long enough to service frames requested by the editor.
- (void)setNeedsRender;
- (void)renderNow;

/// GraphEditor conveniences kept here so Swift/Objective-C hosts do not need a
/// C++ façade for common overlay controls.
- (void)setOverlayOpacity:(float)opacity;
- (void)setClearColorRed:(float)red green:(float)green blue:(float)blue alpha:(float)alpha;
- (void)setValueScrubEnabled:(BOOL)enabled;
- (void)setDisplayFrame:(double)frame;
- (NSString *)selectedNodeId;
- (BOOL)addNode:(NSString *)nodeId atPoint:(CGPoint)point;

/// Input entry points are public for embedding tests and non-recognizer hosts.
- (void)pointerDown:(CGPoint)point;
- (void)pointerMove:(CGPoint)point;
- (void)pointerUp:(CGPoint)point;
- (void)pinchBegin;
- (void)pinchUpdate:(CGFloat)scale anchor:(CGPoint)anchor;
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

@end

NS_ASSUME_NONNULL_END
