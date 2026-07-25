// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT

#include "HairScene.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace noodles::demo::hair {
namespace {

constexpr double kDisplayFrame = 0.0;
constexpr float kOrbitRadiansPerPoint = 0.010f;
constexpr float kTruckUnitsPerPoint = 0.0045f;
constexpr float kMinCameraDistance = 0.9f;
constexpr float kMaxCameraDistance = 14.0f;
constexpr float kMaxPitch = 1.35f;
// Screen radius used to find a brush center on existing hair. Wider than the
// point pick radius because the brush only needs somewhere sensible to start.
constexpr float kBrushSeekRadiusPoints = 70.0f;

// A node's display name, for status messages. Falls back to the id so a
// message is never blank.
std::string NodeLabel(const GraphSnapshot& graph, const std::string& nodeId) {
  const GraphNode* node = FindNode(graph, nodeId);
  return node && !node->name.empty() ? node->name : nodeId;
}

// Why a tool armed on `toolNode` cannot act, given the stage node that is
// actually on the chain to the Output. Naming both is the difference between
// "something is wrong" and knowing which node to rewire.
std::string OffChainReason(const GraphSnapshot& graph,
                           const std::string& toolNode,
                           const std::string& chainNode,
                           const char* stageName) {
  if (chainNode.empty()) {
    return std::string("no ") + stageName + " reaches the Output";
  }
  return NodeLabel(graph, toolNode) + " is not the " + stageName +
         " feeding the Output (" + NodeLabel(graph, chainNode) + " is)";
}

const Vec3* PointAt(const GuideSet& guides, const GuidePointRef& ref) {
  if (!ref.valid()) return nullptr;
  const std::size_t curveIndex = static_cast<std::size_t>(ref.curveIndex);
  if (curveIndex >= guides.curves.size()) return nullptr;
  const std::vector<Vec3>& points = guides.curves[curveIndex].points;
  const std::size_t pointIndex = static_cast<std::size_t>(ref.pointIndex);
  if (pointIndex >= points.size()) return nullptr;
  return &points[pointIndex];
}

}  // namespace

HairScene::HairScene(std::shared_ptr<GraphDocument> document)
    : document_(std::move(document)) {
  reseedGuides();
}

// ── viewport and camera ──────────────────────────────────────────────────────

void HairScene::setViewport(float widthPoints, float heightPoints) {
  const float width = std::max(1.0f, widthPoints);
  const float height = std::max(1.0f, heightPoints);
  if (camera_.viewportWidth == width && camera_.viewportHeight == height) {
    return;
  }
  camera_.viewportWidth = width;
  camera_.viewportHeight = height;
  generations_.bumpCamera();
}

void HairScene::orbitBy(float deltaXPoints, float deltaYPoints) {
  camera_.yaw -= deltaXPoints * kOrbitRadiansPerPoint;
  camera_.pitch = Clamp(camera_.pitch + deltaYPoints * kOrbitRadiansPerPoint,
                        -kMaxPitch, kMaxPitch);
  generations_.bumpCamera();
}

void HairScene::truckBy(float deltaXPoints, float deltaYPoints) {
  // Scale with distance so the model tracks the pointer at any zoom level.
  const float scale = kTruckUnitsPerPoint * camera_.distance;
  const Vec3 right = CameraRight(camera_);
  const Vec3 up = CameraUp(camera_);
  camera_.target -= right * (deltaXPoints * scale);
  camera_.target += up * (deltaYPoints * scale);
  generations_.bumpCamera();
}

void HairScene::dollyBy(float factor) {
  if (factor <= 0.0f) return;
  camera_.distance = Clamp(camera_.distance / factor, kMinCameraDistance,
                           kMaxCameraDistance);
  generations_.bumpCamera();
}

void HairScene::frameGroom() {
  const HairEvalResult& eval = result();
  Vec3 center = eval.scalp.valid ? eval.scalp.center : Vec3{0.0f, 0.9f, 0.0f};
  float extent = eval.scalp.valid ? eval.scalp.radii.y : 1.0f;
  if (!eval.guides.curves.empty()) {
    // Include the guide tips so a long groom is not cropped by the frame.
    for (const GuideCurve& curve : eval.guides.curves) {
      if (curve.points.empty()) continue;
      extent = std::max(extent, Distance(curve.tip(), center));
    }
  }
  camera_.target = center;
  camera_.distance =
      Clamp(extent * 3.1f, kMinCameraDistance, kMaxCameraDistance);
  generations_.bumpCamera();
}

// ── evaluation ───────────────────────────────────────────────────────────────

GraphSnapshot HairScene::snapshot() const {
  return document_ ? document_->snapshot(kDisplayFrame) : GraphSnapshot{};
}

void HairScene::conformStores(const GraphSnapshot& graph) {
  const GraphNode* create = FindNodeBySchema(graph, schema::kCreateGuides);
  if (!create) return;
  const int pointCount = ClampInt(
      static_cast<int>(std::lround(
          PropertyNumber(graph, create->id, props::kDrawPoints, 14.0))),
      3, 48);

  auto stored = stores_.guides.find(create->id);
  if (stored == stores_.guides.end()) return;

  // Keep the store at the node's point count. Evaluation would resample
  // anyway, but holding the store at the same resolution is what lets Edit
  // Points address a picked point by index without an inverse mapping.
  bool changed = false;
  for (GuideCurve& curve : stored->second.curves) {
    if (curve.points.size() < 2) continue;
    if (static_cast<int>(curve.points.size()) == pointCount) continue;
    curve.points = ResampleByArcLength(curve.points, pointCount);
    changed = true;
  }
  if (changed) generations_.bumpGuideStore();
}

const HairEvalResult& HairScene::result() {
  const GraphSnapshot graph = snapshot();
  conformStores(graph);
  const HairEvalKey key = MakeEvalKey(graph, generations_);
  if (hasResult_ && key == resultKey_) {
    recomputed_ = false;
    return result_;
  }
  result_ = EvaluateHairGraph(graph, stores_);
  resultKey_ = key;
  hasResult_ = true;
  recomputed_ = true;
  ++generations_.evaluation;
  return result_;
}

void HairScene::reseedGuides() {
  const GraphSnapshot graph = snapshot();
  const GraphNode* create = FindNodeBySchema(graph, schema::kCreateGuides);
  const GraphNode* scalpNode = FindNodeBySchema(graph, schema::kScalp);
  if (!create || !scalpNode) return;

  const ScalpMesh scalp = BuildScalpMesh(ReadScalpParameters(graph,
                                                             scalpNode->id));
  SeedGuideParameters seed;
  seed.pointCount = ClampInt(
      static_cast<int>(std::lround(
          PropertyNumber(graph, create->id, props::kDrawPoints, 14.0))),
      3, 48);
  seed.length = Clamp(
      static_cast<float>(
          PropertyNumber(graph, create->id, props::kDrawLength, 0.62)),
      0.05f, 3.0f);
  stores_.guides[create->id] = SeedGuides(scalp, seed);
  generations_.bumpGuideStore();
  hasResult_ = false;
}

void HairScene::onAttributeEdited(const std::string& nodeId,
                                  const std::string& propertyName) {
  generations_.bumpGraph();
  // A tool switch is authoritative: turning one on turns every other mutating
  // tool off, so only one can ever be armed.
  if (ToolKindForProperty(propertyName) == HairToolKind::None) return;

  const GraphSnapshot graph = snapshot();
  if (!PropertyBool(graph, nodeId, propertyName, false)) {
    status_ = "Tool: None";
    return;
  }
  for (const ToolSwitch& toolSwitch : ToolSwitchesIn(graph)) {
    if (nodeId == toolSwitch.nodeId && propertyName == toolSwitch.propertyName) {
      continue;
    }
    if (!PropertyBool(graph, toolSwitch.nodeId, toolSwitch.propertyName,
                      false)) {
      continue;
    }
    setNumber(toolSwitch.nodeId, toolSwitch.propertyName, 0.0);
  }
  status_ = std::string("Tool: ") + HairToolKindName(activeTool());
}

void HairScene::onTopologyEdited() {
  generations_.bumpGraph();
  hasResult_ = false;
}

// ── tools ────────────────────────────────────────────────────────────────────

HairToolKind HairScene::activeTool() { return ActiveTool(snapshot()); }

void HairScene::setActiveTool(HairToolKind kind) {
  const GraphSnapshot graph = snapshot();
  // Prefer the node that is actually in the evaluated chain, so arming a tool
  // from the control bar picks the one whose edits will be visible.
  const std::string preferred = preferredToolNodeId(kind, graph);
  // Exclusivity is expressed by writing every switch, not just the new one, so
  // the document is left in a state ActiveTool() can read unambiguously.
  for (const ToolSwitch& toolSwitch : ToolSwitchesIn(graph)) {
    const bool enable = kind != HairToolKind::None &&
                        ToolKindForSwitch(toolSwitch) == kind &&
                        toolSwitch.nodeId == preferred;
    setNumber(toolSwitch.nodeId, toolSwitch.propertyName, enable ? 1.0 : 0.0);
  }
  // Painting a map you cannot see would be pointless, so arming the brush
  // reveals it — by writing the node's own display row, not by overriding it.
  if (kind == HairToolKind::PaintClump && !preferred.empty()) {
    setNumber(preferred, props::kClumpShowMap, 1.0);
  }
  status_ = std::string("Tool: ") + HairToolKindName(kind);
}

std::string HairScene::preferredToolNodeId(HairToolKind kind,
                                           const GraphSnapshot& graph) {
  const HairEvalResult& eval = result();
  switch (kind) {
    case HairToolKind::DrawGuides:
    case HairToolKind::EditPoints:
      if (!eval.createGuidesNodeId.empty()) return eval.createGuidesNodeId;
      break;
    case HairToolKind::CombBrush:
      if (!eval.sculptNodeId.empty()) return eval.sculptNodeId;
      break;
    case HairToolKind::EditClump:
    case HairToolKind::PaintClump:
      if (!eval.clumpNodeId.empty()) return eval.clumpNodeId;
      break;
    case HairToolKind::None:
      return std::string();
  }
  return ToolNodeId(graph, kind);
}

void HairScene::setNumber(const std::string& nodeId,
                          const std::string& propertyName, double value) {
  if (!document_) return;
  if (document_->setAttributeValue(nodeId, propertyName, value,
                                   kDisplayFrame)) {
    generations_.bumpGraph();
  }
}

// ── input ────────────────────────────────────────────────────────────────────

bool HairScene::pointerDown(float x, float y, bool alternate) {
  lastX_ = x;
  lastY_ = y;
  const GraphSnapshot graph = snapshot();
  const HairEvalResult& eval = result();
  const HairToolKind tool = ActiveTool(graph);

  // Ownership is decided here and only here. A tool that declines (the press
  // missed the scalp, no point was under the cursor, the gizmo was not hit)
  // hands the gesture to the camera for its whole duration.
  if (!alternate && tool != HairToolKind::None &&
      beginTool(tool, graph, eval, x, y)) {
    gestureOwner_ = HairGestureOwner::Tool;
    gestureTool_ = tool;
    return true;
  }

  gestureTool_ = HairToolKind::None;
  gestureOwner_ = alternate ? HairGestureOwner::CameraTruck
                            : HairGestureOwner::CameraOrbit;
  return true;
}

void HairScene::pointerMove(float x, float y) {
  const float deltaX = x - lastX_;
  const float deltaY = y - lastY_;
  lastX_ = x;
  lastY_ = y;

  switch (gestureOwner_) {
    case HairGestureOwner::None:
      return;
    case HairGestureOwner::CameraOrbit:
      orbitBy(deltaX, deltaY);
      return;
    case HairGestureOwner::CameraTruck:
      truckBy(deltaX, deltaY);
      return;
    case HairGestureOwner::Tool:
      continueTool(x, y);
      return;
  }
}

void HairScene::pointerUp(float x, float y) {
  if (gestureOwner_ == HairGestureOwner::Tool) finishTool(x, y);
  gestureOwner_ = HairGestureOwner::None;
  gestureTool_ = HairToolKind::None;
}

void HairScene::hoverMove(float x, float y) {
  if (gestureOwner_ != HairGestureOwner::None) return;
  const GraphSnapshot graph = snapshot();
  const HairEvalResult& eval = result();
  const HairToolKind tool = ActiveTool(graph);

  hoveredPoint_ = GuidePointRef{};
  clumpHovered_ = false;
  switch (tool) {
    case HairToolKind::EditPoints:
      hoveredPoint_ = PickGuidePoint(eval.guidesAtCreate, camera_, x, y,
                                     kPointPickRadiusPoints,
                                     /*includeRoots=*/true, nullptr);
      break;
    case HairToolKind::CombBrush: {
      Vec3 center;
      const GuidePointRef nearest = PickGuidePoint(
          eval.guides, camera_, x, y, kBrushSeekRadiusPoints,
          /*includeRoots=*/true, nullptr);
      const Vec3* point = PointAt(eval.guides, nearest);
      if (point) {
        center = *point;
      } else {
        const ScalpHit hit =
            RayHitScalp(eval.scalp, CameraRayThroughPoint(camera_, x, y));
        if (!hit.hit) {
          brushHasCenter_ = false;
          break;
        }
        center = hit.position;
      }
      brushCenter_ = center;
      brushHasCenter_ = true;
      break;
    }
    case HairToolKind::EditClump:
      clumpHovered_ = !eval.clumpNodeId.empty() &&
                      HitsGizmo(camera_, eval.clump.center,
                                kGizmoPickRadiusPoints, x, y);
      break;
    case HairToolKind::PaintClump: {
      const ScalpHit hit =
          RayHitScalp(eval.scalp, CameraRayThroughPoint(camera_, x, y));
      paintHasCenter_ = hit.hit;
      if (hit.hit) {
        paintCenter_ = hit.position;
        paintNormal_ = hit.normal;
      }
      break;
    }
    case HairToolKind::DrawGuides:
    case HairToolKind::None:
      break;
  }
}

void HairScene::pinchBegin() {
  pinching_ = true;
  pinchStartDistance_ = camera_.distance;
}

void HairScene::pinchUpdate(float scale) {
  if (!pinching_ || scale <= 0.0f) return;
  // Recognizers report scale cumulatively from the gesture start, so the
  // distance is derived from the start value rather than compounded per call.
  camera_.distance = Clamp(pinchStartDistance_ / scale, kMinCameraDistance,
                           kMaxCameraDistance);
  generations_.bumpCamera();
}

void HairScene::pinchEnd() { pinching_ = false; }

// ── tool dispatch ────────────────────────────────────────────────────────────

bool HairScene::beginTool(HairToolKind kind, const GraphSnapshot& graph,
                          const HairEvalResult& eval, float x, float y) {
  switch (kind) {
    case HairToolKind::DrawGuides:
      return beginDraw(graph, eval, x, y);
    case HairToolKind::EditPoints:
      return beginEditPoints(graph, eval, x, y);
    case HairToolKind::CombBrush:
      return beginComb(graph, eval, x, y);
    case HairToolKind::EditClump:
      return beginEditClump(graph, eval, x, y);
    case HairToolKind::PaintClump:
      return beginPaintClump(graph, eval, x, y);
    case HairToolKind::None:
      break;
  }
  return false;
}

bool HairScene::beginPaintClump(const GraphSnapshot& graph,
                                const HairEvalResult& eval, float x, float y) {
  const std::string toolNode = ToolNodeId(graph, HairToolKind::PaintClump);
  if (toolNode.empty() || toolNode != eval.clumpNodeId) {
    status_ =
        "Paint Clump: " + OffChainReason(graph, toolNode, eval.clumpNodeId,
                                         "Clump");
    return false;
  }
  if (!eval.scalp.valid) {
    status_ = "Paint Clump: no scalp to paint on";
    return false;
  }
  paintNodeId_ = toolNode;
  if (!paintClumpAt(x, y)) {
    status_ = "Paint Clump: paint on the scalp";
    return false;
  }
  paintStroking_ = true;
  status_ = eval.clump.paintErase ? "Paint Clump: erasing clump weight"
                                  : "Paint Clump: painting clump weight";
  return true;
}

bool HairScene::paintClumpAt(float x, float y) {
  if (paintNodeId_.empty()) return false;
  const HairEvalResult& eval = result();
  if (!eval.scalp.valid) return false;

  const ScalpHit hit =
      RayHitScalp(eval.scalp, CameraRayThroughPoint(camera_, x, y));
  if (!hit.hit) return false;
  paintCenter_ = hit.position;
  paintNormal_ = hit.normal;
  paintHasCenter_ = true;

  ClumpPaint& paint = stores_.clumpPaint[paintNodeId_];
  // The map starts fully weighted, so the first stroke of the erase brush has
  // something to remove and the Clump node never looks inert.
  if (paint.empty()) paint = MakeClumpPaint(1.0f);

  const ClumpState state = eval.clump;
  const int changed =
      PaintClumpWeight(eval.scalp, paint, paintCenter_, state.paintRadius,
                       state.paintErase ? 0.0f : 1.0f, state.paintAmount);
  if (changed > 0) generations_.bumpPaintStore();
  return true;
}

bool HairScene::beginDraw(const GraphSnapshot& graph,
                          const HairEvalResult& eval, float x, float y) {
  const std::string toolNode = ToolNodeId(graph, HairToolKind::DrawGuides);
  // The tool only operates on a node that is genuinely part of the evaluated
  // chain: arming Draw on a disconnected Create Guides must not silently edit
  // geometry nobody can see.
  if (toolNode.empty() || toolNode != eval.createGuidesNodeId) {
    status_ = "Draw Guides: " +
              OffChainReason(graph, toolNode, eval.createGuidesNodeId, "Create Guides");
    return false;
  }
  const bool snapRoot =
      PropertyBool(graph, toolNode, props::kDrawSnapRoot, true);
  if (!BeginGuideStroke(eval.scalp, camera_, x, y, snapRoot, &stroke_)) {
    status_ = "Draw Guides: start the stroke on the scalp";
    return false;
  }
  strokeNodeId_ = toolNode;
  status_ = "Draw Guides: drawing";
  return true;
}

bool HairScene::beginEditPoints(const GraphSnapshot& graph,
                                const HairEvalResult& eval, float x, float y) {
  const std::string toolNode = ToolNodeId(graph, HairToolKind::EditPoints);
  if (toolNode.empty() || toolNode != eval.createGuidesNodeId) {
    status_ = "Edit Points: " +
              OffChainReason(graph, toolNode, eval.createGuidesNodeId, "Create Guides");
    return false;
  }

  // An armed axis handle wins over picking a new point, so a constrained drag
  // is always reachable even where handles overlap other guides.
  if (const Vec3* selected = PointAt(eval.guidesAtCreate, selectedPoint_)) {
    const HairDragAxis axis =
        PickAxisHandle(camera_, *selected, kAxisHandleLength, x, y,
                       kHandlePickRadiusPoints);
    if (axis != HairDragAxis::CameraPlane) {
      dragPoint_ = selectedPoint_;
      dragAxis_ = axis;
      dragAnchor_ = *selected;
      Vec3 grab;
      if (PointerOnAxis(camera_, dragAnchor_, AxisDirection(axis), x, y,
                        &grab)) {
        dragAxisOffset_ = Dot(dragAnchor_ - grab, AxisDirection(axis));
      } else {
        dragAxisOffset_ = 0.0f;
      }
      editNodeId_ = toolNode;
      status_ = std::string("Edit Points: ") +
                (axis == HairDragAxis::AxisX
                     ? "X"
                     : (axis == HairDragAxis::AxisY ? "Y" : "Z")) +
                " axis";
      return true;
    }
  }

  const GuidePointRef picked =
      PickGuidePoint(eval.guidesAtCreate, camera_, x, y,
                     kPointPickRadiusPoints, /*includeRoots=*/true, nullptr);
  const Vec3* point = PointAt(eval.guidesAtCreate, picked);
  if (!point) {
    status_ = "Edit Points: no guide point under the pointer";
    return false;
  }
  selectedPoint_ = picked;
  dragPoint_ = picked;
  dragAxis_ = HairDragAxis::CameraPlane;
  dragAnchor_ = *point;
  Vec3 grab;
  if (PointerOnCameraPlane(camera_, dragAnchor_, x, y, &grab)) {
    dragPlaneOffset_ = dragAnchor_ - grab;
  } else {
    dragPlaneOffset_ = Vec3{};
  }
  editNodeId_ = toolNode;
  status_ = "Edit Points: moving point " +
            std::to_string(picked.pointIndex) + " of guide " +
            std::to_string(picked.curveIndex);
  return true;
}

bool HairScene::beginComb(const GraphSnapshot& graph,
                          const HairEvalResult& eval, float x, float y) {
  const std::string toolNode = ToolNodeId(graph, HairToolKind::CombBrush);
  if (toolNode.empty() || toolNode != eval.sculptNodeId) {
    status_ = "Comb Brush: " +
              OffChainReason(graph, toolNode, eval.sculptNodeId,
                             "Guide Sculpt");
    return false;
  }
  if (!eval.sculptInputGuides.valid) {
    status_ = "Comb Brush: no guides to sculpt";
    return false;
  }

  Vec3 center;
  const GuidePointRef nearest =
      PickGuidePoint(eval.guides, camera_, x, y, kBrushSeekRadiusPoints,
                     /*includeRoots=*/true, nullptr);
  if (const Vec3* point = PointAt(eval.guides, nearest)) {
    center = *point;
  } else {
    const ScalpHit hit =
        RayHitScalp(eval.scalp, CameraRayThroughPoint(camera_, x, y));
    if (!hit.hit) {
      status_ = "Comb Brush: start over the hair or the scalp";
      return false;
    }
    center = hit.position;
  }

  brushCenter_ = center;
  brushHasCenter_ = true;
  brushStroking_ = true;
  brushPlanePoint_ = center;
  // Freeze the brush plane for the whole stroke so the push direction stays
  // consistent even if the camera is nudged.
  brushPlaneNormal_ = -CameraForward(camera_);
  sculptNodeId_ = toolNode;
  status_ = "Comb Brush: combing";
  return true;
}

bool HairScene::beginEditClump(const GraphSnapshot& graph,
                               const HairEvalResult& eval, float x, float y) {
  const std::string toolNode = ToolNodeId(graph, HairToolKind::EditClump);
  if (toolNode.empty() || toolNode != eval.clumpNodeId) {
    status_ =
        "Edit Clump: " + OffChainReason(graph, toolNode, eval.clumpNodeId,
                                        "Clump");
    return false;
  }

  const HairDragAxis axis =
      PickAxisHandle(camera_, eval.clump.center, kAxisHandleLength * 1.6f, x, y,
                     kHandlePickRadiusPoints);
  if (axis == HairDragAxis::CameraPlane &&
      !HitsGizmo(camera_, eval.clump.center, kGizmoPickRadiusPoints, x, y)) {
    status_ = "Edit Clump: grab the clump gizmo";
    return false;
  }

  clumpNodeId_ = toolNode;
  dragAxis_ = axis;
  dragAnchor_ = eval.clump.center;
  if (axis == HairDragAxis::CameraPlane) {
    Vec3 grab;
    dragPlaneOffset_ =
        PointerOnCameraPlane(camera_, dragAnchor_, x, y, &grab)
            ? dragAnchor_ - grab
            : Vec3{};
  } else {
    Vec3 grab;
    dragAxisOffset_ = PointerOnAxis(camera_, dragAnchor_, AxisDirection(axis),
                                    x, y, &grab)
                          ? Dot(dragAnchor_ - grab, AxisDirection(axis))
                          : 0.0f;
  }
  status_ = "Edit Clump: moving the clump center";
  return true;
}

void HairScene::continueTool(float x, float y) {
  switch (gestureTool_) {
    case HairToolKind::DrawGuides: {
      const GraphSnapshot graph = snapshot();
      const float spacing = Clamp(
          static_cast<float>(
              PropertyNumber(graph, strokeNodeId_, props::kDrawSpacing, 0.05)),
          0.002f, 1.0f);
      ExtendGuideStroke(camera_, x, y, spacing, &stroke_);
      return;
    }

    case HairToolKind::EditPoints: {
      if (!dragPoint_.valid() || editNodeId_.empty()) return;
      auto stored = stores_.guides.find(editNodeId_);
      if (stored == stores_.guides.end()) return;
      const std::size_t curveIndex =
          static_cast<std::size_t>(dragPoint_.curveIndex);
      if (curveIndex >= stored->second.curves.size()) return;

      Vec3 target;
      if (dragAxis_ == HairDragAxis::CameraPlane) {
        Vec3 plane;
        if (!PointerOnCameraPlane(camera_, dragAnchor_, x, y, &plane)) return;
        target = plane + dragPlaneOffset_;
      } else {
        const Vec3 axis = AxisDirection(dragAxis_);
        Vec3 onAxis;
        if (!PointerOnAxis(camera_, dragAnchor_, axis, x, y, &onAxis)) return;
        target = onAxis + axis * dragAxisOffset_;
      }

      const GraphSnapshot graph = snapshot();
      const int pointCount = ClampInt(
          static_cast<int>(std::lround(PropertyNumber(
              graph, editNodeId_, props::kDrawPoints, 14.0))),
          3, 48);
      const float smoothing = static_cast<float>(
          PropertyNumber(graph, editNodeId_, props::kDrawSmoothing, 0.35));
      const bool snapRoot =
          PropertyBool(graph, editNodeId_, props::kDrawSnapRoot, true);

      GuideCurve& curve = stored->second.curves[curveIndex];
      const HairEvalResult& eval = result();
      if (dragPoint_.pointIndex == 0 && snapRoot) {
        // A root cannot leave the scalp, so solving for an off-surface target
        // would never converge. Write the target straight in and let the snap
        // stage slide the whole curve along the surface.
        if (!curve.points.empty()) curve.points.front() = target;
      } else {
        SolveStoredPointForTarget(eval.scalp, curve, dragPoint_.pointIndex,
                                  target, pointCount, smoothing, snapRoot);
      }
      generations_.bumpGuideStore();
      return;
    }

    case HairToolKind::CombBrush: {
      if (!brushStroking_ || sculptNodeId_.empty()) return;
      const Ray ray = CameraRayThroughPoint(camera_, x, y);
      float t = 0.0f;
      if (!IntersectRayPlane(ray, brushPlanePoint_, brushPlaneNormal_, &t)) {
        return;
      }
      const Vec3 newCenter = PointOnRay(ray, t);
      const Vec3 motion = newCenter - brushCenter_;
      brushCenter_ = newCenter;
      if (LengthSquared(motion) <= 1e-12f) return;

      const HairEvalResult& eval = result();
      if (!eval.sculptInputGuides.valid) return;
      BrushState brush = eval.brush;
      brush.center = newCenter;
      brush.active = true;
      brush.valid = true;
      ApplyCombBrush(eval.sculptInputGuides, brush, motion,
                     stores_.sculpt[sculptNodeId_]);
      generations_.bumpSculptStore();
      return;
    }

    case HairToolKind::EditClump: {
      if (clumpNodeId_.empty()) return;
      Vec3 target;
      if (dragAxis_ == HairDragAxis::CameraPlane) {
        Vec3 plane;
        if (!PointerOnCameraPlane(camera_, dragAnchor_, x, y, &plane)) return;
        target = plane + dragPlaneOffset_;
      } else {
        const Vec3 axis = AxisDirection(dragAxis_);
        Vec3 onAxis;
        if (!PointerOnAxis(camera_, dragAnchor_, axis, x, y, &onAxis)) return;
        target = onAxis + axis * dragAxisOffset_;
      }
      // The gizmo has no state of its own: it writes the node's scalars, and
      // the next evaluation reads them back. That is what keeps the node
      // authoritative and the gizmo genuinely node-driven.
      setNumber(clumpNodeId_, props::kClumpCenterX,
                static_cast<double>(target.x));
      setNumber(clumpNodeId_, props::kClumpCenterY,
                static_cast<double>(target.y));
      setNumber(clumpNodeId_, props::kClumpCenterZ,
                static_cast<double>(target.z));
      return;
    }

    case HairToolKind::PaintClump: {
      if (!paintStroking_) return;
      paintClumpAt(x, y);
      return;
    }

    case HairToolKind::None:
      return;
  }
}

void HairScene::finishTool(float x, float y) {
  (void)x;
  (void)y;
  switch (gestureTool_) {
    case HairToolKind::DrawGuides: {
      if (!stroke_.active || strokeNodeId_.empty()) {
        stroke_ = GuideStroke{};
        return;
      }
      const GraphSnapshot graph = snapshot();
      const int pointCount = ClampInt(
          static_cast<int>(std::lround(PropertyNumber(
              graph, strokeNodeId_, props::kDrawPoints, 14.0))),
          3, 48);
      const float smoothing = static_cast<float>(
          PropertyNumber(graph, strokeNodeId_, props::kDrawSmoothing, 0.35));
      const bool snapRoot =
          PropertyBool(graph, strokeNodeId_, props::kDrawSnapRoot, true);
      const float maxLength = Clamp(
          static_cast<float>(
              PropertyNumber(graph, strokeNodeId_, props::kDrawLength, 0.62)),
          0.05f, 3.0f);

      const HairEvalResult& eval = result();
      const GuideCurve curve = CommitGuideStroke(
          eval.scalp, stroke_, pointCount, smoothing, snapRoot, maxLength);
      stroke_ = GuideStroke{};
      if (curve.points.size() < 2) {
        status_ = "Draw Guides: stroke too short";
        return;
      }
      GuideSet& store = stores_.guides[strokeNodeId_];
      store.curves.push_back(curve);
      store.valid = true;
      generations_.bumpGuideStore();
      status_ = "Draw Guides: added guide " +
                std::to_string(store.curves.size());
      return;
    }

    case HairToolKind::EditPoints:
      dragPoint_ = GuidePointRef{};
      status_ = "Edit Points: ready";
      return;

    case HairToolKind::CombBrush:
      brushStroking_ = false;
      status_ = "Comb Brush: ready";
      return;

    case HairToolKind::EditClump:
      status_ = "Edit Clump: ready";
      return;

    case HairToolKind::PaintClump:
      paintStroking_ = false;
      status_ = "Paint Clump: ready";
      return;

    case HairToolKind::None:
      return;
  }
}

// ── view state ───────────────────────────────────────────────────────────────

HairViewState HairScene::viewState() {
  const HairEvalResult& eval = result();
  HairViewState state;
  state.eval = &eval;
  state.camera = camera_;
  state.tool = ActiveTool(snapshot());
  state.strokePreview = PreviewGuideStroke(stroke_);
  state.selectedPoint = selectedPoint_;
  state.hoveredPoint = hoveredPoint_;
  state.activeAxis = dragAxis_;

  state.brushVisible = state.tool == HairToolKind::CombBrush &&
                       eval.display.showBrushGizmo && brushHasCenter_;
  state.brushStroking = brushStroking_;
  state.brushCenter = brushCenter_;
  state.brushRadius = eval.brush.radius;

  state.clumpGizmoVisible =
      !eval.clumpNodeId.empty() && eval.display.showClumpGizmo;
  state.clumpGizmoHovered =
      clumpHovered_ || (gestureOwner_ == HairGestureOwner::Tool &&
                        gestureTool_ == HairToolKind::EditClump);
  state.clumpCenter = eval.clump.center;

  state.paintBrushVisible = state.tool == HairToolKind::PaintClump &&
                            !eval.clumpNodeId.empty() && paintHasCenter_;
  state.paintBrushStroking = paintStroking_;
  state.paintBrushCenter = paintCenter_;
  state.paintBrushNormal = paintNormal_;
  state.paintBrushRadius = eval.clump.paintRadius;
  state.paintErase = eval.clump.paintErase;
  return state;
}

}  // namespace noodles::demo::hair
