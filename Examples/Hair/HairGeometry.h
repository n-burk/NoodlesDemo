// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT
#pragma once

// Procedural scalp construction plus the guide-curve operations the /Hair
// nodes evaluate: arc-length resampling, smoothing, root snapping, comb
// sculpting, and clumping. Everything here is deterministic and free of
// platform, GL, and graph types so it can be tested directly.

#include "HairTypes.h"

#include <cstdint>
#include <vector>

namespace noodles::demo::hair {

// ── scalp ────────────────────────────────────────────────────────────────────

struct ScalpParameters {
  float radius = 1.0f;     // x/z radius in world units
  float height = 1.18f;    // y radius as a multiple of `radius`
  float roundness = 0.62f; // 0 = shallow cap, 1 = dome cut below the equator
  int segments = 56;       // azimuthal divisions
  int rings = 28;          // polar divisions
  Vec3 center{0.0f, 0.86f, 0.0f};
};

ScalpMesh BuildScalpMesh(const ScalpParameters& parameters);

struct ScalpHit {
  bool hit = false;
  Vec3 position;
  Vec3 normal{0.0f, 1.0f, 0.0f};
  Vec2 uv;
  float t = 0.0f;
};

// Analytic ellipsoid intersection restricted to the dome region, so picking is
// independent of the tessellation level.
ScalpHit RayHitScalp(const ScalpMesh& scalp, const Ray& ray);

// Nearest point on the scalp surface along the radial direction from the
// scalp center. Used to keep guide roots attached while points are dragged.
Vec3 SnapToScalp(const ScalpMesh& scalp, const Vec3& point, Vec3* outNormal,
                 Vec2* outUV);

// Surface point for a normalized (azimuth, polar) coordinate.
Vec3 ScalpPointFromUV(const ScalpMesh& scalp, const Vec2& uv, Vec3* outNormal);

// A stable orthonormal frame at a scalp normal: east, north, normal. Guides
// are transported between roots through this frame so interpolated hair
// follows the surface instead of sliding across it.
void ScalpFrame(const Vec3& normal, Vec3* outEast, Vec3* outNorth);

// ── polyline helpers ─────────────────────────────────────────────────────────

// Uniform arc-length resample to exactly `count` points. The first and last
// points are preserved exactly, which is what keeps roots attached.
std::vector<Vec3> ResampleByArcLength(const std::vector<Vec3>& points,
                                      int count);

// Laplacian smoothing. `strength` in [0, 1]; index 0 is never moved when
// `pinFirst` is set, so smoothing cannot detach a root.
void SmoothPolyline(std::vector<Vec3>& points, float strength, int iterations,
                    bool pinFirst);

// ── guide sets ───────────────────────────────────────────────────────────────

struct SeedGuideParameters {
  int rings = 6;
  int perRing = 12;
  float length = 0.55f;
  int pointCount = 12;
  float sweep = 0.72f;  // how far the seeded guides fall away from the normal
  std::uint32_t seed = 91;
};

// The deterministic guide set the demo opens with: rings of guides laid over
// the dome, swept back and slightly curled so the launch state is a styled
// groom rather than a spike ball.
GuideSet SeedGuides(const ScalpMesh& scalp, const SeedGuideParameters& params);

// Build one guide from a raw drawn stroke. The first sample is treated as the
// root; `snapRoot` re-attaches it to the scalp.
GuideCurve MakeGuideFromStroke(const ScalpMesh& scalp,
                               const std::vector<Vec3>& stroke, int pointCount,
                               float smoothing, bool snapRoot);

void ResampleGuideSet(GuideSet& guides, int pointCount);
void SmoothGuideSet(GuideSet& guides, float strength);
void SnapGuideRoots(const ScalpMesh& scalp, GuideSet& guides);

// The exact stages the CreateGuides node applies to one stored curve.
//
// Resampling runs only when the stored count differs from the requested one,
// so a curve the user just dragged is not silently redistributed under the
// cursor on the next frame. Both the evaluator and the Edit Points drag solver
// call this, which is what keeps "what is drawn" and "what is solved for"
// from drifting apart.
GuideCurve EvaluateStoredCurve(const ScalpMesh& scalp, const GuideCurve& stored,
                               int pointCount, float smoothing, bool snapRoot);

GuideSet EvaluateStoredGuides(const ScalpMesh& scalp, const GuideSet& stored,
                              int pointCount, float smoothing, bool snapRoot);

// ── sculpt deltas (GuideSculpt) ──────────────────────────────────────────────

// Per-guide, per-point displacement authored by the comb brush. Stored
// separately from the guides so an upstream change (a redraw, a different
// point count) re-applies the same sculpt instead of discarding it.
using SculptDeltas = std::vector<std::vector<Vec3>>;

// Reshape `deltas` so every curve matches the corresponding curve in `target`,
// resampling existing displacements by normalized index.
void ConformDeltas(SculptDeltas& deltas, const GuideSet& target);

GuideSet ApplySculptDeltas(const GuideSet& input, const SculptDeltas& deltas);

// One comb-brush step. `motion` is the world-space push for this step; points
// inside the brush move by a falloff-weighted mix of that push and a
// smoothing pull toward their neighbours. Index 0 of every curve is a root and
// is never moved.
void ApplyCombBrush(const GuideSet& base, const BrushState& brush,
                    const Vec3& motion, SculptDeltas& deltas);

}  // namespace noodles::demo::hair
