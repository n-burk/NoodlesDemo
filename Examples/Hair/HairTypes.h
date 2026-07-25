// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT
#pragma once

// Platform-neutral C++17 groom data. None of these types appear in
// NoodlesDemoCore: the graph carries topology plus scalar/Boolean controls,
// while the typed scalp/guide/strand data lives here and is keyed by node id.

#include "HairMath.h"

#include <cstdint>
#include <string>
#include <vector>

namespace noodles::demo::hair {

// ── scalp ────────────────────────────────────────────────────────────────────

struct ScalpVertex {
  Vec3 position;
  Vec3 normal;
  Vec2 uv;  // x = azimuth 0..1, y = polar 0..1 (0 at the crown)
};

// A procedural dome. The triangle list feeds rendering; the analytic ellipsoid
// parameters feed ray hits and root distribution, so picking never depends on
// the tessellation level.
struct ScalpMesh {
  std::vector<ScalpVertex> vertices;
  std::vector<std::uint32_t> indices;

  Vec3 center{0.0f, 0.0f, 0.0f};
  Vec3 radii{1.0f, 1.0f, 1.0f};
  // Dome cut expressed as a normalized height: only the surface with
  // localY >= cutHeight (in units of radii.y) is part of the scalp.
  float cutHeight = -0.15f;
  bool valid = false;

  bool empty() const { return vertices.empty() || indices.empty(); }
};

// ── guides ───────────────────────────────────────────────────────────────────

// One guide curve. points[0] is the root and is the only point constrained to
// the scalp surface; tools that move points never move a root off the scalp
// while root snapping is enabled.
struct GuideCurve {
  std::vector<Vec3> points;
  Vec3 rootNormal{0.0f, 1.0f, 0.0f};
  Vec2 rootUV;

  bool empty() const { return points.size() < 2; }
  Vec3 root() const {
    return points.empty() ? Vec3{} : points.front();
  }
  Vec3 tip() const { return points.empty() ? Vec3{} : points.back(); }
  float length() const {
    float total = 0.0f;
    for (std::size_t i = 1; i < points.size(); ++i) {
      total += Distance(points[i - 1], points[i]);
    }
    return total;
  }
};

// A guide set is `valid` only when it was produced by a fully connected
// upstream chain. An invalid set is empty and carries no stale geometry.
struct GuideSet {
  std::vector<GuideCurve> curves;
  bool valid = false;

  bool empty() const { return curves.empty(); }
  std::size_t pointCount() const {
    std::size_t total = 0;
    for (const GuideCurve& curve : curves) total += curve.points.size();
    return total;
  }
};

// ── generated hair ───────────────────────────────────────────────────────────

struct Strand {
  std::vector<Vec3> points;
  // Per-strand width multiplier and shading offset, both seeded, so variation
  // survives regeneration without storing a second parallel array.
  float widthScale = 1.0f;
  float shade = 0.5f;
  // The root's scalp UV, carried so a Clump placed after generation can look
  // up the painted clump weight without re-projecting every strand.
  Vec2 rootUV;
};

struct StrandSet {
  std::vector<Strand> strands;
  bool valid = false;

  bool empty() const { return strands.empty(); }
  std::size_t pointCount() const {
    std::size_t total = 0;
    for (const Strand& strand : strands) total += strand.points.size();
    return total;
  }
};

// ── tool state ───────────────────────────────────────────────────────────────

enum class HairToolKind {
  None = 0,
  DrawGuides,
  EditPoints,
  CombBrush,
  EditClump,
  PaintClump,
};

const char* HairToolKindName(HairToolKind kind);

// Live comb-brush state. `active` is true only between pointer down and up, so
// the renderer can show a stroke-time ring separately from the hover ring.
struct BrushState {
  Vec3 center;
  Vec3 lastCenter;
  float radius = 0.35f;
  float strength = 0.5f;
  float falloff = 2.0f;
  float smoothing = 0.35f;
  bool active = false;
  bool valid = false;
};

// The clump target the Edit Clump tool drags. Center and target are authored
// back into the Clump node's clump:centerX/Y/Z scalars, which keeps the node
// switches authoritative and makes the gizmo genuinely node-driven.
struct ClumpState {
  Vec3 center{0.0f, 1.55f, 0.0f};
  float strength = 0.6f;
  float tipBias = 2.0f;
  float radius = 0.7f;
  float noise = 0.15f;
  // Voronoi regions over the scalp: how many clumps the groom is divided into.
  int regionCount = 28;
  std::uint32_t regionSeed = 5;
  // Paint brush, in world units, for the clump-weight map.
  float paintRadius = 0.28f;
  float paintAmount = 0.55f;
  bool paintErase = false;
};

// Which guide point the Edit Points tool is manipulating, and along which axis
// when an axis handle was grabbed instead of the point body.
enum class HairDragAxis { CameraPlane = 0, AxisX, AxisY, AxisZ };

struct GuidePointRef {
  int curveIndex = -1;
  int pointIndex = -1;

  bool valid() const { return curveIndex >= 0 && pointIndex >= 0; }
};

// ── dirty tracking ───────────────────────────────────────────────────────────

// Monotonic generation counters. Camera motion bumps only `camera`, so an
// orbit never invalidates the evaluated groom; the evaluator's cache key is
// built from the other counters plus the graph's topology/parameter hashes.
struct DirtyGenerations {
  std::uint64_t guideStore = 1;   // authored guide points (CreateGuides)
  std::uint64_t sculptStore = 1;  // comb deltas (GuideSculpt)
  std::uint64_t paintStore = 1;   // painted clump weights (Clump)
  std::uint64_t graph = 1;        // topology or scalar/Boolean property change
  std::uint64_t camera = 1;       // view only, never invalidates evaluation
  std::uint64_t evaluation = 1;   // bumped whenever a new result is produced

  void bumpGuideStore() { ++guideStore; }
  void bumpSculptStore() { ++sculptStore; }
  void bumpPaintStore() { ++paintStore; }
  void bumpGraph() { ++graph; }
  void bumpCamera() { ++camera; }
};

// The inputs that decide whether a cached evaluation may be reused. Camera is
// deliberately absent.
struct HairEvalKey {
  std::uint64_t topologyHash = 0;
  std::uint64_t parameterHash = 0;
  std::uint64_t guideStore = 0;
  std::uint64_t sculptStore = 0;
  std::uint64_t paintStore = 0;

  bool operator==(const HairEvalKey& other) const {
    return topologyHash == other.topologyHash &&
           parameterHash == other.parameterHash &&
           guideStore == other.guideStore &&
           sculptStore == other.sculptStore && paintStore == other.paintStore;
  }
  bool operator!=(const HairEvalKey& other) const { return !(*this == other); }
};

}  // namespace noodles::demo::hair
