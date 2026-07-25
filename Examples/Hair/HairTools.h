// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT
#pragma once

// The picking and drag math the node-owned tools are built from. Every
// function here is pure and expressed in view points (the same top-left
// origin, Y-down space GraphEditor uses for pointer input), so both the macOS
// and iPadOS shells drive identical behavior and the tools are testable
// without a window.

#include "HairEvaluator.h"
#include "HairGeometry.h"
#include "HairTypes.h"

#include <string>
#include <vector>

namespace noodles::demo::hair {

// Screen-space pick radii, in view points. Sized for a fingertip on iPad,
// which is also comfortable for a mouse.
inline constexpr float kPointPickRadiusPoints = 22.0f;
inline constexpr float kHandlePickRadiusPoints = 18.0f;
inline constexpr float kGizmoPickRadiusPoints = 26.0f;
// World length of the X/Y/Z handles drawn around the selected guide point.
inline constexpr float kAxisHandleLength = 0.22f;

// The mutating tool the graph currently says is active, together with the node
// whose switch armed it. Exactly one `tool:*` switch may be on; if several are
// somehow set, the first in ToolSwitchesIn() order wins so the result is never
// ambiguous.
struct ActiveToolInfo {
  HairToolKind kind = HairToolKind::None;
  std::string nodeId;
};
ActiveToolInfo ActiveToolIn(const GraphSnapshot& snapshot);
HairToolKind ActiveTool(const GraphSnapshot& snapshot);

// Id of the node that would own `kind`: the one whose switch is already on if
// there is one, else the first node of that type. Empty when the graph has no
// node able to host the tool.
std::string ToolNodeId(const GraphSnapshot& snapshot, HairToolKind kind);

// The tool a given switch arms.
HairToolKind ToolKindForSwitch(const ToolSwitch& toolSwitch);
HairToolKind ToolKindForProperty(const std::string& propertyName);

// Nearest guide point to the pointer, measured in view points. Returns an
// invalid ref when nothing is within `radiusPoints`.
GuidePointRef PickGuidePoint(const GuideSet& guides, const Camera& camera,
                             float viewX, float viewY, float radiusPoints,
                             bool includeRoots, float* outDistancePoints);

// Screen-space distance from the pointer to a world point, or a negative
// value when the point is behind the eye.
float ViewDistanceToPoint(const Camera& camera, const Vec3& world, float viewX,
                          float viewY);

bool HitsGizmo(const Camera& camera, const Vec3& center, float radiusPoints,
               float viewX, float viewY);

// Which axis handle around `origin` the pointer is over, or CameraPlane when
// none of them is.
HairDragAxis PickAxisHandle(const Camera& camera, const Vec3& origin,
                            float handleLength, float viewX, float viewY,
                            float radiusPoints);

// Intersect the pointer ray with the plane through `anchor` facing the camera.
bool PointerOnCameraPlane(const Camera& camera, const Vec3& anchor,
                          float viewX, float viewY, Vec3* outPoint);

// Closest point to the pointer ray on the world axis line through `anchor`.
// This is what turns a free 2D drag into a constrained X/Y/Z move.
bool PointerOnAxis(const Camera& camera, const Vec3& anchor, const Vec3& axis,
                   float viewX, float viewY, Vec3* outPoint);

Vec3 AxisDirection(HairDragAxis axis);

// ── Edit Points drag solver ──────────────────────────────────────────────────

// Move a stored point so that its EVALUATED position lands on `target`.
//
// Smoothing and root snapping sit between the store and what the user sees, so
// writing `target` straight into the store would leave the point trailing the
// cursor by the smoothing amount. Those stages are linear and contractive, so
// a few correction passes converge quickly and dragging stays exact.
void SolveStoredPointForTarget(const ScalpMesh& scalp, GuideCurve& stored,
                               int pointIndex, const Vec3& target,
                               int pointCount, float smoothing, bool snapRoot,
                               int iterations = 4);

// ── Draw Guides stroke ───────────────────────────────────────────────────────

// A stroke in progress. The plane is fixed at pointer-down so the drawn curve
// cannot warp when the camera is nudged mid-stroke.
struct GuideStroke {
  std::vector<Vec3> samples;
  Vec3 planePoint;
  Vec3 planeNormal;
  Vec3 rootNormal{0.0f, 1.0f, 0.0f};
  Vec2 rootUV;
  bool active = false;
};

// Begin a stroke from a scalp ray hit. Returns false when the pointer misses
// the scalp, which is what lets the gesture fall through to the camera.
bool BeginGuideStroke(const ScalpMesh& scalp, const Camera& camera,
                      float viewX, float viewY, bool snapRoot,
                      GuideStroke* outStroke);

// Append a sample if it is at least `spacing` from the previous one.
void ExtendGuideStroke(const Camera& camera, float viewX, float viewY,
                       float spacing, GuideStroke* stroke);

// Turn the stroke into a guide, clamped to `maxLength`. Returns an empty curve
// when the stroke never left the root.
GuideCurve CommitGuideStroke(const ScalpMesh& scalp, const GuideStroke& stroke,
                             int pointCount, float smoothing, bool snapRoot,
                             float maxLength);

// The live preview curve for an in-progress stroke.
GuideCurve PreviewGuideStroke(const GuideStroke& stroke);

}  // namespace noodles::demo::hair
