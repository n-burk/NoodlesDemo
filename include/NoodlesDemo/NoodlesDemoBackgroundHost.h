// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT

// Shared vocabulary for hosts that render their own content into the same
// drawable as the graph and want background gestures routed to it.
//
// The graph views own the GL context and the native event stream, so a host
// that draws underneath the graph needs exactly two seams: a place to draw
// with the context current and the drawable bound, and a way to claim pointer
// gestures that did not land on graph content. Both are declared identically
// on the AppKit and UIKit shells so the host's own logic can stay in shared
// C++ and only the event translation differs per platform.

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

/// Modifier state accompanying a background gesture. UIKit always reports
/// None; AppKit maps Shift and Option so a desktop host can offer secondary
/// camera controls without the shells knowing what they mean.
typedef NS_OPTIONS(NSUInteger, NoodlesDemoInputModifiers) {
  NoodlesDemoInputModifierNone = 0,
  NoodlesDemoInputModifierPrimary = 1 << 0,
  NoodlesDemoInputModifierSecondary = 1 << 1,
};
