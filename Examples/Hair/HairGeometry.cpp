// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT

#include "HairGeometry.h"

#include <algorithm>
#include <cmath>

namespace noodles::demo::hair {
namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// Dome cut as a function of the user-facing roundness control: a low value
// keeps only the crown, a high value carries the scalp below the equator.
float CutHeightForRoundness(float roundness) {
  return Lerp(0.42f, -0.55f, Saturate(roundness));
}

Vec3 LocalDirectionToWorld(const ScalpMesh& scalp, const Vec3& direction) {
  return Vec3{scalp.center.x + direction.x * scalp.radii.x,
              scalp.center.y + direction.y * scalp.radii.y,
              scalp.center.z + direction.z * scalp.radii.z};
}

Vec3 WorldToLocalDirection(const ScalpMesh& scalp, const Vec3& point) {
  const Vec3 radii{scalp.radii.x > 1e-6f ? scalp.radii.x : 1e-6f,
                   scalp.radii.y > 1e-6f ? scalp.radii.y : 1e-6f,
                   scalp.radii.z > 1e-6f ? scalp.radii.z : 1e-6f};
  return Vec3{(point.x - scalp.center.x) / radii.x,
              (point.y - scalp.center.y) / radii.y,
              (point.z - scalp.center.z) / radii.z};
}

// Ellipsoid surface normal at a unit-sphere direction: the gradient of
// (p/r)·(p/r) is 2p/r², so the world normal divides by the radii twice.
Vec3 EllipsoidNormal(const ScalpMesh& scalp, const Vec3& localDirection) {
  const Vec3 radii{scalp.radii.x > 1e-6f ? scalp.radii.x : 1e-6f,
                   scalp.radii.y > 1e-6f ? scalp.radii.y : 1e-6f,
                   scalp.radii.z > 1e-6f ? scalp.radii.z : 1e-6f};
  return Normalized(Vec3{localDirection.x / (radii.x * radii.x),
                         localDirection.y / (radii.y * radii.y),
                         localDirection.z / (radii.z * radii.z)});
}

Vec2 UVForLocalDirection(const ScalpMesh& scalp, const Vec3& direction) {
  float azimuth = std::atan2(direction.z, direction.x);
  if (azimuth < 0.0f) azimuth += kTwoPi;
  const float span = 1.0f - scalp.cutHeight;
  const float polar =
      span > 1e-6f ? Saturate((1.0f - direction.y) / span) : 0.0f;
  return Vec2{azimuth / kTwoPi, polar};
}

Vec3 LocalDirectionForUV(const ScalpMesh& scalp, const Vec2& uv) {
  const float span = 1.0f - scalp.cutHeight;
  const float localY = 1.0f - Saturate(uv.y) * span;
  const float ringRadius = std::sqrt(std::max(0.0f, 1.0f - localY * localY));
  const float azimuth = uv.x * kTwoPi;
  return Vec3{ringRadius * std::cos(azimuth), localY,
              ringRadius * std::sin(azimuth)};
}

// Resample a displacement list by normalized index. Deltas are per-point
// offsets, not a curve in space, so index parameterization is the correct
// mapping when an upstream point count changes.
std::vector<Vec3> ResampleByIndex(const std::vector<Vec3>& values, int count) {
  std::vector<Vec3> result;
  if (count <= 0) return result;
  result.resize(static_cast<std::size_t>(count));
  if (values.empty()) return result;
  if (values.size() == 1) {
    for (Vec3& value : result) value = values.front();
    return result;
  }
  const float last = static_cast<float>(values.size() - 1);
  for (int i = 0; i < count; ++i) {
    const float t = count == 1 ? 0.0f : static_cast<float>(i) /
                                            static_cast<float>(count - 1);
    const float position = t * last;
    const int low = ClampInt(static_cast<int>(std::floor(position)), 0,
                             static_cast<int>(values.size()) - 1);
    const int high = ClampInt(low + 1, 0, static_cast<int>(values.size()) - 1);
    const float fraction = position - static_cast<float>(low);
    result[static_cast<std::size_t>(i)] =
        Lerp(values[static_cast<std::size_t>(low)],
             values[static_cast<std::size_t>(high)], fraction);
  }
  return result;
}

}  // namespace

const char* HairToolKindName(HairToolKind kind) {
  switch (kind) {
    case HairToolKind::None:
      return "None";
    case HairToolKind::DrawGuides:
      return "Draw Guides";
    case HairToolKind::EditPoints:
      return "Edit Points";
    case HairToolKind::CombBrush:
      return "Comb Brush";
    case HairToolKind::EditClump:
      return "Edit Clump";
    case HairToolKind::PaintClump:
      return "Paint Clump";
  }
  return "None";
}

// ── scalp ────────────────────────────────────────────────────────────────────

ScalpMesh BuildScalpMesh(const ScalpParameters& parameters) {
  ScalpMesh mesh;
  const float radius = std::max(0.05f, parameters.radius);
  mesh.center = parameters.center;
  mesh.radii = Vec3{radius, radius * std::max(0.2f, parameters.height), radius};
  mesh.cutHeight = CutHeightForRoundness(parameters.roundness);

  const int segments = ClampInt(parameters.segments, 8, 256);
  const int rings = ClampInt(parameters.rings, 3, 128);

  mesh.vertices.reserve(static_cast<std::size_t>((rings + 1) * (segments + 1)));
  for (int ring = 0; ring <= rings; ++ring) {
    const float v = static_cast<float>(ring) / static_cast<float>(rings);
    for (int segment = 0; segment <= segments; ++segment) {
      const float u = static_cast<float>(segment) / static_cast<float>(segments);
      const Vec3 direction = LocalDirectionForUV(mesh, Vec2{u, v});
      ScalpVertex vertex;
      vertex.position = LocalDirectionToWorld(mesh, direction);
      vertex.normal = EllipsoidNormal(mesh, direction);
      vertex.uv = Vec2{u, v};
      mesh.vertices.push_back(vertex);
    }
  }

  const int stride = segments + 1;
  mesh.indices.reserve(static_cast<std::size_t>(rings * segments * 6));
  for (int ring = 0; ring < rings; ++ring) {
    for (int segment = 0; segment < segments; ++segment) {
      const std::uint32_t topLeft =
          static_cast<std::uint32_t>(ring * stride + segment);
      const std::uint32_t topRight = topLeft + 1;
      const std::uint32_t bottomLeft =
          static_cast<std::uint32_t>((ring + 1) * stride + segment);
      const std::uint32_t bottomRight = bottomLeft + 1;
      // Counter-clockwise seen from OUTSIDE the dome. u runs with the azimuth
      // and v runs down from the crown, so this ordering is the one whose
      // winding normal agrees with the outward surface normal; the opposite
      // ordering leaves back-face culling showing the dome's interior.
      mesh.indices.push_back(topLeft);
      mesh.indices.push_back(topRight);
      mesh.indices.push_back(bottomLeft);
      mesh.indices.push_back(topRight);
      mesh.indices.push_back(bottomRight);
      mesh.indices.push_back(bottomLeft);
    }
  }
  mesh.valid = true;
  return mesh;
}

ScalpHit RayHitScalp(const ScalpMesh& scalp, const Ray& ray) {
  ScalpHit hit;
  if (!scalp.valid) return hit;

  const Vec3 radii{scalp.radii.x > 1e-6f ? scalp.radii.x : 1e-6f,
                   scalp.radii.y > 1e-6f ? scalp.radii.y : 1e-6f,
                   scalp.radii.z > 1e-6f ? scalp.radii.z : 1e-6f};
  const Vec3 origin{(ray.origin.x - scalp.center.x) / radii.x,
                    (ray.origin.y - scalp.center.y) / radii.y,
                    (ray.origin.z - scalp.center.z) / radii.z};
  const Vec3 direction{ray.direction.x / radii.x, ray.direction.y / radii.y,
                       ray.direction.z / radii.z};

  const float a = Dot(direction, direction);
  if (a <= 1e-20f) return hit;
  const float b = 2.0f * Dot(origin, direction);
  const float c = Dot(origin, origin) - 1.0f;
  const float discriminant = b * b - 4.0f * a * c;
  if (discriminant < 0.0f) return hit;

  const float root = std::sqrt(discriminant);
  const float candidates[2] = {(-b - root) / (2.0f * a),
                               (-b + root) / (2.0f * a)};
  for (const float t : candidates) {
    if (t < 0.0f) continue;
    const Vec3 local = origin + direction * t;
    // The dome stops at the cut, so the far intersection is a legitimate hit
    // when the near one lands on the removed part of the ellipsoid.
    if (local.y < scalp.cutHeight) continue;
    hit.hit = true;
    hit.t = t;
    hit.position = LocalDirectionToWorld(scalp, local);
    hit.normal = EllipsoidNormal(scalp, local);
    hit.uv = UVForLocalDirection(scalp, local);
    return hit;
  }
  return hit;
}

Vec3 SnapToScalp(const ScalpMesh& scalp, const Vec3& point, Vec3* outNormal,
                 Vec2* outUV) {
  if (!scalp.valid) {
    if (outNormal) *outNormal = Vec3{0.0f, 1.0f, 0.0f};
    if (outUV) *outUV = Vec2{0.0f, 0.0f};
    return point;
  }
  Vec3 local = Normalized(WorldToLocalDirection(scalp, point));
  if (LengthSquared(local) <= 0.0f) local = Vec3{0.0f, 1.0f, 0.0f};
  if (local.y < scalp.cutHeight) {
    // Slide down to the rim rather than wrapping onto the removed underside.
    const float ringRadius =
        std::sqrt(std::max(0.0f, 1.0f - scalp.cutHeight * scalp.cutHeight));
    const float planar = std::sqrt(local.x * local.x + local.z * local.z);
    const float scale = planar > 1e-6f ? ringRadius / planar : 0.0f;
    local = Vec3{local.x * scale, scalp.cutHeight, local.z * scale};
    if (planar <= 1e-6f) local = Vec3{ringRadius, scalp.cutHeight, 0.0f};
  }
  if (outNormal) *outNormal = EllipsoidNormal(scalp, local);
  if (outUV) *outUV = UVForLocalDirection(scalp, local);
  return LocalDirectionToWorld(scalp, local);
}

Vec3 ScalpPointFromUV(const ScalpMesh& scalp, const Vec2& uv, Vec3* outNormal) {
  const Vec3 local = LocalDirectionForUV(scalp, uv);
  if (outNormal) *outNormal = EllipsoidNormal(scalp, local);
  return LocalDirectionToWorld(scalp, local);
}

void ScalpFrame(const Vec3& normal, Vec3* outEast, Vec3* outNorth) {
  const Vec3 unit = Normalized(normal);
  Vec3 east = Cross(Vec3{0.0f, 1.0f, 0.0f}, unit);
  if (LengthSquared(east) <= 1e-12f) {
    // Exactly at a pole the azimuthal frame is undefined; any fixed basis is
    // as good as another and keeps the transport well defined.
    east = Vec3{1.0f, 0.0f, 0.0f};
  }
  east = Normalized(east);
  if (outEast) *outEast = east;
  if (outNorth) *outNorth = Cross(unit, east);
}

// ── polyline helpers ─────────────────────────────────────────────────────────

std::vector<Vec3> ResampleByArcLength(const std::vector<Vec3>& points,
                                      int count) {
  std::vector<Vec3> result;
  if (count <= 0 || points.empty()) return result;
  if (points.size() == 1 || count == 1) {
    result.assign(static_cast<std::size_t>(count), points.front());
    return result;
  }

  std::vector<float> cumulative(points.size(), 0.0f);
  for (std::size_t i = 1; i < points.size(); ++i) {
    cumulative[i] = cumulative[i - 1] + Distance(points[i - 1], points[i]);
  }
  const float total = cumulative.back();
  result.reserve(static_cast<std::size_t>(count));
  if (total <= 1e-8f) {
    result.assign(static_cast<std::size_t>(count), points.front());
    result.back() = points.back();
    return result;
  }

  std::size_t cursor = 0;
  for (int i = 0; i < count; ++i) {
    const float target = total * static_cast<float>(i) /
                         static_cast<float>(count - 1);
    while (cursor + 2 < points.size() && cumulative[cursor + 1] < target) {
      ++cursor;
    }
    const float segmentLength = cumulative[cursor + 1] - cumulative[cursor];
    const float fraction =
        segmentLength > 1e-8f ? (target - cumulative[cursor]) / segmentLength
                              : 0.0f;
    result.push_back(
        Lerp(points[cursor], points[cursor + 1], Saturate(fraction)));
  }
  // Preserve the endpoints exactly; accumulated float error must never detach
  // a root from the scalp.
  result.front() = points.front();
  result.back() = points.back();
  return result;
}

void SmoothPolyline(std::vector<Vec3>& points, float strength, int iterations,
                    bool pinFirst) {
  const float amount = Saturate(strength);
  if (points.size() < 3 || amount <= 0.0f || iterations <= 0) return;
  const std::size_t first = pinFirst ? 1u : 0u;
  for (int iteration = 0; iteration < iterations; ++iteration) {
    std::vector<Vec3> source = points;
    for (std::size_t i = first; i < points.size(); ++i) {
      if (i == 0 || i + 1 >= source.size()) continue;
      const Vec3 average = (source[i - 1] + source[i + 1]) * 0.5f;
      points[i] = Lerp(source[i], average, amount);
    }
  }
}

// ── guide sets ───────────────────────────────────────────────────────────────

GuideSet SeedGuides(const ScalpMesh& scalp, const SeedGuideParameters& params) {
  GuideSet guides;
  if (!scalp.valid) return guides;

  const int rings = ClampInt(params.rings, 1, 32);
  const int perRing = ClampInt(params.perRing, 1, 128);
  const int pointCount = ClampInt(params.pointCount, 2, 64);
  const float length = std::max(0.02f, params.length);
  const float step = length / static_cast<float>(pointCount - 1);

  guides.curves.reserve(static_cast<std::size_t>(rings * perRing));
  for (int ring = 0; ring < rings; ++ring) {
    // Keep the outermost ring clear of the rim so seeded roots never sit on
    // the silhouette edge.
    const float v = (static_cast<float>(ring) + 0.65f) /
                    (static_cast<float>(rings) + 0.35f) * 0.86f;
    for (int index = 0; index < perRing; ++index) {
      const std::uint32_t guideSeed = HashCombine(
          params.seed, static_cast<std::uint32_t>(ring * 1013 + index));
      const float stagger = (ring % 2 == 0) ? 0.0f : 0.5f;
      const float u = (static_cast<float>(index) + stagger) /
                      static_cast<float>(perRing);

      Vec3 normal;
      const Vec3 root = ScalpPointFromUV(scalp, Vec2{u, v}, &normal);

      GuideCurve curve;
      curve.rootNormal = normal;
      curve.rootUV = Vec2{u, v};
      curve.points.reserve(static_cast<std::size_t>(pointCount));
      curve.points.push_back(root);

      // Fall away from the crown as the curve grows: mostly downward, with a
      // slight backward bias and a retained share of the root normal so hair
      // on the front of the head falls forward-and-down while hair on the back
      // falls back-and-down. A single global sweep direction would drive the
      // front guides straight through the scalp.
      const Vec3 sweepTarget = Normalized(
          normal * 0.45f + Vec3{0.0f, -1.0f, 0.0f} +
          Vec3{RandomSignedFloat(HashCombine(guideSeed, 5u)) * 0.18f, 0.0f,
               -0.25f + RandomSignedFloat(HashCombine(guideSeed, 9u)) * 0.18f});
      Vec3 position = root;
      for (int point = 1; point < pointCount; ++point) {
        const float t = static_cast<float>(point) /
                        static_cast<float>(pointCount - 1);
        const float bend = Saturate(params.sweep) * t * t;
        Vec3 direction = Normalized(Lerp(normal, sweepTarget, bend));
        const Vec3 wobble =
            RandomSignedVec3(HashCombine(guideSeed, static_cast<std::uint32_t>(
                                                        point * 131u))) *
            0.09f;
        direction = Normalized(direction + wobble * t);
        position += direction * step;
        // Seeded guides must not tunnel into the dome they grow from.
        Vec3 local = Vec3{(position.x - scalp.center.x) / scalp.radii.x,
                          (position.y - scalp.center.y) / scalp.radii.y,
                          (position.z - scalp.center.z) / scalp.radii.z};
        const float radius = Length(local);
        if (radius < 1.0f && radius > 1e-6f) {
          local *= 1.0f / radius;
          position = Vec3{scalp.center.x + local.x * scalp.radii.x,
                          scalp.center.y + local.y * scalp.radii.y,
                          scalp.center.z + local.z * scalp.radii.z};
        }
        curve.points.push_back(position);
      }
      guides.curves.push_back(std::move(curve));
    }
  }
  guides.valid = true;
  return guides;
}

GuideCurve MakeGuideFromStroke(const ScalpMesh& scalp,
                               const std::vector<Vec3>& stroke, int pointCount,
                               float smoothing, bool snapRoot) {
  GuideCurve curve;
  if (stroke.size() < 2) return curve;

  std::vector<Vec3> working = stroke;
  if (snapRoot) {
    Vec3 normal;
    Vec2 uv;
    const Vec3 snapped = SnapToScalp(scalp, working.front(), &normal, &uv);
    working.front() = snapped;
    curve.rootNormal = normal;
    curve.rootUV = uv;
  } else {
    Vec3 normal;
    Vec2 uv;
    SnapToScalp(scalp, working.front(), &normal, &uv);
    curve.rootNormal = normal;
    curve.rootUV = uv;
  }

  const int iterations = 1 + static_cast<int>(Saturate(smoothing) * 5.0f);
  SmoothPolyline(working, smoothing, iterations, /*pinFirst=*/true);
  curve.points = ResampleByArcLength(working, ClampInt(pointCount, 2, 64));
  return curve;
}

void ResampleGuideSet(GuideSet& guides, int pointCount) {
  const int count = ClampInt(pointCount, 2, 64);
  for (GuideCurve& curve : guides.curves) {
    if (curve.points.size() < 2) continue;
    curve.points = ResampleByArcLength(curve.points, count);
  }
}

void SmoothGuideSet(GuideSet& guides, float strength) {
  const float amount = Saturate(strength);
  if (amount <= 0.0f) return;
  for (GuideCurve& curve : guides.curves) {
    SmoothPolyline(curve.points, amount, 2, /*pinFirst=*/true);
  }
}

void SnapGuideRoots(const ScalpMesh& scalp, GuideSet& guides) {
  if (!scalp.valid) return;
  for (GuideCurve& curve : guides.curves) {
    if (curve.points.empty()) continue;
    Vec3 normal;
    Vec2 uv;
    const Vec3 snapped = SnapToScalp(scalp, curve.points.front(), &normal, &uv);
    const Vec3 delta = snapped - curve.points.front();
    // Translate the whole curve so re-attaching a root never kinks the shape
    // the user drew or sculpted.
    for (Vec3& point : curve.points) point += delta;
    curve.rootNormal = normal;
    curve.rootUV = uv;
  }
}

GuideCurve EvaluateStoredCurve(const ScalpMesh& scalp, const GuideCurve& stored,
                               int pointCount, float smoothing,
                               bool snapRoot) {
  GuideCurve curve = stored;
  const int count = ClampInt(pointCount, 3, 48);
  if (curve.points.size() >= 2 &&
      static_cast<int>(curve.points.size()) != count) {
    curve.points = ResampleByArcLength(curve.points, count);
  }
  SmoothPolyline(curve.points, smoothing, 2, /*pinFirst=*/true);
  if (snapRoot && scalp.valid && !curve.points.empty()) {
    Vec3 normal;
    Vec2 uv;
    const Vec3 snapped = SnapToScalp(scalp, curve.points.front(), &normal, &uv);
    const Vec3 delta = snapped - curve.points.front();
    for (Vec3& point : curve.points) point += delta;
    curve.rootNormal = normal;
    curve.rootUV = uv;
  }
  return curve;
}

GuideSet EvaluateStoredGuides(const ScalpMesh& scalp, const GuideSet& stored,
                              int pointCount, float smoothing, bool snapRoot) {
  GuideSet result;
  result.curves.reserve(stored.curves.size());
  // Curves are never dropped, even degenerate ones: the Edit Points tool picks
  // by index into this output and writes back into the store at the same
  // index, so the two must stay in lockstep.
  for (const GuideCurve& curve : stored.curves) {
    result.curves.push_back(
        EvaluateStoredCurve(scalp, curve, pointCount, smoothing, snapRoot));
  }
  result.valid = stored.valid;
  return result;
}

// ── sculpt deltas ────────────────────────────────────────────────────────────

void ConformDeltas(SculptDeltas& deltas, const GuideSet& target) {
  deltas.resize(target.curves.size());
  for (std::size_t i = 0; i < target.curves.size(); ++i) {
    const std::size_t needed = target.curves[i].points.size();
    if (deltas[i].size() == needed) continue;
    if (deltas[i].empty()) {
      deltas[i].assign(needed, Vec3{});
      continue;
    }
    deltas[i] = ResampleByIndex(deltas[i], static_cast<int>(needed));
  }
}

GuideSet ApplySculptDeltas(const GuideSet& input, const SculptDeltas& deltas) {
  GuideSet result = input;
  for (std::size_t i = 0; i < result.curves.size() && i < deltas.size(); ++i) {
    GuideCurve& curve = result.curves[i];
    const std::vector<Vec3>& curveDeltas = deltas[i];
    // Index 0 is the root and is intentionally skipped: sculpting bends hair,
    // it does not detach it from the scalp.
    for (std::size_t j = 1; j < curve.points.size() && j < curveDeltas.size();
         ++j) {
      curve.points[j] += curveDeltas[j];
    }
  }
  return result;
}

void ApplyCombBrush(const GuideSet& base, const BrushState& brush,
                    const Vec3& motion, SculptDeltas& deltas) {
  if (!base.valid || base.curves.empty()) return;
  ConformDeltas(deltas, base);

  const float radius = std::max(1e-4f, brush.radius);
  const float falloffExponent = std::max(0.2f, brush.falloff);
  const float strength = Saturate(brush.strength);
  const float smoothing = Saturate(brush.smoothing);
  if (strength <= 0.0f) return;

  const GuideSet current = ApplySculptDeltas(base, deltas);
  for (std::size_t i = 0; i < current.curves.size(); ++i) {
    const std::vector<Vec3>& points = current.curves[i].points;
    std::vector<Vec3>& curveDeltas = deltas[i];
    for (std::size_t j = 1; j < points.size(); ++j) {
      const float distance = Distance(points[j], brush.center);
      if (distance >= radius) continue;
      const float weight =
          std::pow(1.0f - distance / radius, falloffExponent) * strength;
      if (weight <= 1e-5f) continue;

      Vec3 neighbourTarget;
      if (j + 1 < points.size()) {
        neighbourTarget = (points[j - 1] + points[j + 1]) * 0.5f;
      } else if (j >= 2) {
        // The tip has no forward neighbour; extrapolate the previous segment
        // so smoothing straightens the end instead of pulling it inward.
        neighbourTarget = points[j - 1] + (points[j - 1] - points[j - 2]);
      } else {
        neighbourTarget = points[j];
      }
      const Vec3 smoothPull = (neighbourTarget - points[j]) * smoothing;
      curveDeltas[j] += (motion + smoothPull) * weight;
    }
  }
}

}  // namespace noodles::demo::hair
