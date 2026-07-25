// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT

#include "HairTools.h"

#include <algorithm>
#include <cmath>

namespace noodles::demo::hair {
HairToolKind ToolKindForProperty(const std::string& propertyName) {
  if (propertyName == props::kToolDraw) return HairToolKind::DrawGuides;
  if (propertyName == props::kToolEditPoints) return HairToolKind::EditPoints;
  if (propertyName == props::kToolComb) return HairToolKind::CombBrush;
  if (propertyName == props::kToolEditClump) return HairToolKind::EditClump;
  if (propertyName == props::kToolPaintClump) return HairToolKind::PaintClump;
  return HairToolKind::None;
}

HairToolKind ToolKindForSwitch(const ToolSwitch& toolSwitch) {
  return ToolKindForProperty(toolSwitch.propertyName);
}

ActiveToolInfo ActiveToolIn(const GraphSnapshot& snapshot) {
  // If several switches are somehow on at once, the first in snapshot order
  // wins so the active tool is never ambiguous.
  for (const ToolSwitch& toolSwitch : ToolSwitchesIn(snapshot)) {
    if (PropertyBool(snapshot, toolSwitch.nodeId, toolSwitch.propertyName,
                     false)) {
      return ActiveToolInfo{ToolKindForSwitch(toolSwitch), toolSwitch.nodeId};
    }
  }
  return ActiveToolInfo{};
}

HairToolKind ActiveTool(const GraphSnapshot& snapshot) {
  return ActiveToolIn(snapshot).kind;
}

std::string ToolNodeId(const GraphSnapshot& snapshot, HairToolKind kind) {
  const std::vector<ToolSwitch> switches = ToolSwitchesIn(snapshot);
  // An already-armed node wins, so a graph with two Clumps keeps using the one
  // the user actually switched on.
  for (const ToolSwitch& toolSwitch : switches) {
    if (ToolKindForSwitch(toolSwitch) != kind) continue;
    if (PropertyBool(snapshot, toolSwitch.nodeId, toolSwitch.propertyName,
                     false)) {
      return toolSwitch.nodeId;
    }
  }
  for (const ToolSwitch& toolSwitch : switches) {
    if (ToolKindForSwitch(toolSwitch) == kind) return toolSwitch.nodeId;
  }
  return std::string();
}

float ViewDistanceToPoint(const Camera& camera, const Vec3& world, float viewX,
                          float viewY) {
  Vec2 projected;
  if (!ProjectToView(camera, world, &projected)) return -1.0f;
  return Length(Vec2{projected.x - viewX, projected.y - viewY});
}

GuidePointRef PickGuidePoint(const GuideSet& guides, const Camera& camera,
                             float viewX, float viewY, float radiusPoints,
                             bool includeRoots, float* outDistancePoints) {
  GuidePointRef best;
  float bestDistance = radiusPoints;
  for (std::size_t curveIndex = 0; curveIndex < guides.curves.size();
       ++curveIndex) {
    const std::vector<Vec3>& points = guides.curves[curveIndex].points;
    for (std::size_t pointIndex = includeRoots ? 0u : 1u;
         pointIndex < points.size(); ++pointIndex) {
      const float distance = ViewDistanceToPoint(
          camera, points[pointIndex], viewX, viewY);
      if (distance < 0.0f || distance > bestDistance) continue;
      bestDistance = distance;
      best.curveIndex = static_cast<int>(curveIndex);
      best.pointIndex = static_cast<int>(pointIndex);
    }
  }
  if (best.valid() && outDistancePoints) *outDistancePoints = bestDistance;
  return best;
}

bool HitsGizmo(const Camera& camera, const Vec3& center, float radiusPoints,
               float viewX, float viewY) {
  const float distance = ViewDistanceToPoint(camera, center, viewX, viewY);
  return distance >= 0.0f && distance <= radiusPoints;
}

Vec3 AxisDirection(HairDragAxis axis) {
  switch (axis) {
    case HairDragAxis::AxisX:
      return Vec3{1.0f, 0.0f, 0.0f};
    case HairDragAxis::AxisY:
      return Vec3{0.0f, 1.0f, 0.0f};
    case HairDragAxis::AxisZ:
      return Vec3{0.0f, 0.0f, 1.0f};
    case HairDragAxis::CameraPlane:
      break;
  }
  return Vec3{0.0f, 0.0f, 0.0f};
}

HairDragAxis PickAxisHandle(const Camera& camera, const Vec3& origin,
                            float handleLength, float viewX, float viewY,
                            float radiusPoints) {
  const HairDragAxis axes[3] = {HairDragAxis::AxisX, HairDragAxis::AxisY,
                                HairDragAxis::AxisZ};
  HairDragAxis best = HairDragAxis::CameraPlane;
  float bestDistance = radiusPoints;
  for (const HairDragAxis axis : axes) {
    // The handle tip is the grab target; the shaft is only a visual cue, so a
    // press near the origin still selects the point itself.
    const Vec3 tip = origin + AxisDirection(axis) * handleLength;
    const float distance = ViewDistanceToPoint(camera, tip, viewX, viewY);
    if (distance < 0.0f || distance > bestDistance) continue;
    bestDistance = distance;
    best = axis;
  }
  return best;
}

bool PointerOnCameraPlane(const Camera& camera, const Vec3& anchor,
                          float viewX, float viewY, Vec3* outPoint) {
  const Ray ray = CameraRayThroughPoint(camera, viewX, viewY);
  const Vec3 normal = -CameraForward(camera);
  float t = 0.0f;
  if (!IntersectRayPlane(ray, anchor, normal, &t)) return false;
  if (outPoint) *outPoint = PointOnRay(ray, t);
  return true;
}

bool PointerOnAxis(const Camera& camera, const Vec3& anchor, const Vec3& axis,
                   float viewX, float viewY, Vec3* outPoint) {
  const Vec3 direction = Normalized(axis);
  if (LengthSquared(direction) <= 0.0f) return false;
  const Ray ray = CameraRayThroughPoint(camera, viewX, viewY);

  // Closest point on the axis line to the pointer ray. With both directions
  // unit length the standard 2x2 system reduces to
  //   s = (b·e - d) / (1 - b²)
  // where b = u·v, d = u·w0, e = v·w0, and w0 = anchor - rayOrigin. The
  // determinant vanishes only when the axis is seen exactly edge-on, where a
  // constrained drag has no defined answer anyway.
  const Vec3 between = anchor - ray.origin;
  const float axisDotRay = Dot(direction, ray.direction);
  const float determinant = 1.0f - axisDotRay * axisDotRay;
  if (std::fabs(determinant) <= 1e-6f) return false;
  const float alongAxis =
      (axisDotRay * Dot(between, ray.direction) - Dot(between, direction)) /
      determinant;
  if (outPoint) *outPoint = anchor + direction * alongAxis;
  return true;
}

void SolveStoredPointForTarget(const ScalpMesh& scalp, GuideCurve& stored,
                               int pointIndex, const Vec3& target,
                               int pointCount, float smoothing, bool snapRoot,
                               int iterations) {
  if (pointIndex < 0 ||
      pointIndex >= static_cast<int>(stored.points.size())) {
    return;
  }
  const std::size_t index = static_cast<std::size_t>(pointIndex);
  stored.points[index] = target;
  for (int iteration = 0; iteration < std::max(1, iterations); ++iteration) {
    const GuideCurve evaluated =
        EvaluateStoredCurve(scalp, stored, pointCount, smoothing, snapRoot);
    if (index >= evaluated.points.size()) return;
    const Vec3 error = target - evaluated.points[index];
    if (LengthSquared(error) <= 1e-12f) return;
    stored.points[index] += error;
  }
}

bool BeginGuideStroke(const ScalpMesh& scalp, const Camera& camera,
                      float viewX, float viewY, bool snapRoot,
                      GuideStroke* outStroke) {
  if (!outStroke) return false;
  *outStroke = GuideStroke{};
  const Ray ray = CameraRayThroughPoint(camera, viewX, viewY);
  const ScalpHit hit = RayHitScalp(scalp, ray);
  if (!hit.hit) return false;

  Vec3 root = hit.position;
  Vec3 normal = hit.normal;
  Vec2 uv = hit.uv;
  if (snapRoot) root = SnapToScalp(scalp, root, &normal, &uv);

  outStroke->samples.push_back(root);
  outStroke->planePoint = root;
  // Freeze the drawing plane at pointer-down. The camera may still move (an
  // iPad two-finger gesture, a trackpad nudge) and the stroke must not warp.
  outStroke->planeNormal = -CameraForward(camera);
  outStroke->rootNormal = normal;
  outStroke->rootUV = uv;
  outStroke->active = true;
  return true;
}

void ExtendGuideStroke(const Camera& camera, float viewX, float viewY,
                       float spacing, GuideStroke* stroke) {
  if (!stroke || !stroke->active || stroke->samples.empty()) return;
  const Ray ray = CameraRayThroughPoint(camera, viewX, viewY);
  float t = 0.0f;
  if (!IntersectRayPlane(ray, stroke->planePoint, stroke->planeNormal, &t)) {
    return;
  }
  const Vec3 sample = PointOnRay(ray, t);
  const float minimumSpacing = std::max(1e-4f, spacing);
  if (Distance(sample, stroke->samples.back()) < minimumSpacing) return;
  stroke->samples.push_back(sample);
}

GuideCurve PreviewGuideStroke(const GuideStroke& stroke) {
  GuideCurve curve;
  if (!stroke.active || stroke.samples.size() < 2) return curve;
  curve.points = stroke.samples;
  curve.rootNormal = stroke.rootNormal;
  curve.rootUV = stroke.rootUV;
  return curve;
}

GuideCurve CommitGuideStroke(const ScalpMesh& scalp, const GuideStroke& stroke,
                             int pointCount, float smoothing, bool snapRoot,
                             float maxLength) {
  GuideCurve curve;
  if (!stroke.active || stroke.samples.size() < 2) return curve;

  std::vector<Vec3> samples = stroke.samples;
  // Clamp the drawn length so one long sweep cannot produce a guide that
  // dwarfs the head; the excess is trimmed, not scaled, so the shape the user
  // drew is preserved up to that point.
  const float limit = std::max(0.02f, maxLength);
  float accumulated = 0.0f;
  for (std::size_t i = 1; i < samples.size(); ++i) {
    const float segment = Distance(samples[i - 1], samples[i]);
    if (accumulated + segment <= limit) {
      accumulated += segment;
      continue;
    }
    const float remaining = limit - accumulated;
    const float fraction = segment > 1e-8f ? remaining / segment : 0.0f;
    samples[i] = Lerp(samples[i - 1], samples[i], Saturate(fraction));
    samples.resize(i + 1);
    break;
  }
  if (samples.size() < 2) return curve;

  curve = MakeGuideFromStroke(scalp, samples, pointCount, smoothing, snapRoot);
  return curve;
}

}  // namespace noodles::demo::hair
