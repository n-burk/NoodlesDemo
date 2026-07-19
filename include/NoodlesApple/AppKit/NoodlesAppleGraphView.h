// Copyright (c) 2026 NoodlesApple contributors.
// SPDX-License-Identifier: MIT

// The public adapter intentionally hosts Noodles' OpenGL renderer. Suppress the
// SDK's blanket OpenGL deprecation attribute at declaration sites; consumers
// can still choose the UIKit shell or provide another GraphEditor presenter.
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION 1
#define NOODLES_APPLE_UNDEFINE_GL_SILENCE_DEPRECATION 1
#endif
#import <AppKit/AppKit.h>
#ifdef NOODLES_APPLE_UNDEFINE_GL_SILENCE_DEPRECATION
#undef GL_SILENCE_DEPRECATION
#undef NOODLES_APPLE_UNDEFINE_GL_SILENCE_DEPRECATION
#endif

#ifdef __cplusplus
#include <memory>

namespace noodles::apple {
class GraphEditor;
}
#endif

NS_ASSUME_NONNULL_BEGIN

/// OpenGL 3.2 Core/AppKit presentation shell for noodles::apple::GraphEditor.
/// Mouse press/drag maps to the editor pointer stream, trackpad scroll pans,
/// magnify performs anchored zoom, and right-click is the desktop equivalent of
/// the iPad title-row long press.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
@interface NoodlesAppleGraphView : NSOpenGLView

#ifdef __cplusplus
- (instancetype)initWithFrame:(NSRect)frame
                       editor:(std::shared_ptr<noodles::apple::GraphEditor>)editor
                   assetsPath:(NSString *)assetsPath NS_DESIGNATED_INITIALIZER;

- (instancetype)initWithEditor:(std::shared_ptr<noodles::apple::GraphEditor>)editor
                    assetsPath:(NSString *)assetsPath;

- (std::shared_ptr<noodles::apple::GraphEditor>)graphEditor;
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

@end
#pragma clang diagnostic pop

NS_ASSUME_NONNULL_END
