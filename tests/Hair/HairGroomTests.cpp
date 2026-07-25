// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT
//
// CPU and integration coverage for the hair grooming demo: scalp geometry,
// deterministic strand generation, root attachment, the four node-owned tools,
// topology-sensitive evaluation, gizmo visibility, camera rays, render
// geometry, and the dirty-generation contract that keeps camera motion from
// regenerating hair.

#include "HairClumpMap.h"
#include "HairEvaluator.h"
#include "HairGeneration.h"
#include "HairGeometry.h"
#include "HairGraph.h"
#include "HairRenderGeometry.h"
#include "HairScene.h"
#include "HairTools.h"

#include <noodles/demo/GraphDocument.h>
#include <noodles/demo/InMemoryGraphDocument.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace nd = noodles::demo;
namespace hair = noodles::demo::hair;

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                   #condition);                                               \
      return false;                                                           \
    }                                                                         \
  } while (false)

namespace {

using hair::Vec2;
using hair::Vec3;

// Distance of a point from the scalp's ellipsoid surface, in normalized units:
// 1.0 means exactly on the surface.
float EllipsoidRadius(const hair::ScalpMesh& scalp, const Vec3& point) {
  const Vec3 local{(point.x - scalp.center.x) / scalp.radii.x,
                   (point.y - scalp.center.y) / scalp.radii.y,
                   (point.z - scalp.center.z) / scalp.radii.z};
  return hair::Length(local);
}

std::shared_ptr<nd::InMemoryGraphDocument> MakeDocument() {
  return hair::CreateHairGraphDocument();
}

bool SetNumber(const std::shared_ptr<nd::InMemoryGraphDocument>& document,
               const char* nodeId, const char* property, double value) {
  return document->setAttributeValue(nodeId, property, value, 0.0);
}

// The view-point position of a world point under the scene's current camera.
bool ViewOf(hair::HairScene& scene, const Vec3& world, Vec2* out) {
  return hair::ProjectToView(scene.camera(), world, out);
}

std::size_t TotalGuidePoints(const hair::GuideSet& guides) {
  return guides.pointCount();
}

// ── scalp geometry ──────────────────────────────────────────────────────────

bool TestScalpMeshIsWellFormed() {
  hair::ScalpParameters parameters;
  const hair::ScalpMesh scalp = hair::BuildScalpMesh(parameters);
  CHECK(scalp.valid);
  CHECK(!scalp.empty());
  CHECK(scalp.indices.size() % 3 == 0);

  for (const std::uint32_t index : scalp.indices) {
    CHECK(index < scalp.vertices.size());
  }
  // Every vertex sits on the ellipsoid, and its normal points outward.
  for (const hair::ScalpVertex& vertex : scalp.vertices) {
    CHECK(std::fabs(EllipsoidRadius(scalp, vertex.position) - 1.0f) < 1e-3f);
    const Vec3 outward = vertex.position - scalp.center;
    CHECK(hair::Dot(vertex.normal, outward) > 0.0f);
    CHECK(std::fabs(hair::Length(vertex.normal) - 1.0f) < 1e-3f);
  }

  // Every triangle is wound counter-clockwise seen from outside. The renderer
  // culls back faces, so an inverted winding would quietly show the dome's
  // interior — the far half of the map, mirrored — rather than its surface.
  int wound = 0;
  for (std::size_t i = 0; i + 2 < scalp.indices.size(); i += 3) {
    const Vec3& a = scalp.vertices[scalp.indices[i]].position;
    const Vec3& b = scalp.vertices[scalp.indices[i + 1]].position;
    const Vec3& c = scalp.vertices[scalp.indices[i + 2]].position;
    const Vec3 geometric = hair::Cross(b - a, c - a);
    // The rings adjacent to the pole collapse to zero area; skip those.
    if (hair::LengthSquared(geometric) <= 1e-16f) continue;
    CHECK(hair::Dot(geometric, scalp.vertices[scalp.indices[i]].normal) > 0.0f);
    ++wound;
  }
  CHECK(wound > 0);

  // Roundness controls how far down the dome is carried.
  hair::ScalpParameters shallow = parameters;
  shallow.roundness = 0.0f;
  hair::ScalpParameters deep = parameters;
  deep.roundness = 1.0f;
  CHECK(hair::BuildScalpMesh(shallow).cutHeight >
        hair::BuildScalpMesh(deep).cutHeight);
  return true;
}

bool TestScalpRayHits() {
  const hair::ScalpMesh scalp = hair::BuildScalpMesh(hair::ScalpParameters{});

  // Straight down onto the crown.
  const hair::Ray down{Vec3{0.0f, 6.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}};
  const hair::ScalpHit crown = hair::RayHitScalp(scalp, down);
  CHECK(crown.hit);
  CHECK(std::fabs(EllipsoidRadius(scalp, crown.position) - 1.0f) < 1e-3f);
  CHECK(crown.normal.y > 0.9f);
  CHECK(crown.uv.y < 0.05f);

  // Pointing away from the dome misses.
  const hair::Ray away{Vec3{0.0f, 6.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}};
  CHECK(!hair::RayHitScalp(scalp, away).hit);

  // A ray far to the side misses.
  const hair::Ray beside{Vec3{9.0f, 6.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}};
  CHECK(!hair::RayHitScalp(scalp, beside).hit);

  // Snapping projects onto the surface and never below the cut.
  const Vec3 snapped =
      hair::SnapToScalp(scalp, Vec3{0.4f, -3.0f, 0.2f}, nullptr, nullptr);
  CHECK(std::fabs(EllipsoidRadius(scalp, snapped) - 1.0f) < 1e-3f);
  const float localY = (snapped.y - scalp.center.y) / scalp.radii.y;
  CHECK(localY >= scalp.cutHeight - 1e-4f);
  return true;
}

bool TestPolylineResampleAndSmooth() {
  std::vector<Vec3> line;
  for (int i = 0; i < 5; ++i) {
    line.push_back(Vec3{static_cast<float>(i), 0.0f, 0.0f});
  }
  const std::vector<Vec3> resampled = hair::ResampleByArcLength(line, 9);
  CHECK(resampled.size() == 9);
  // Endpoints are preserved exactly; that is what keeps a root attached.
  CHECK(hair::Distance(resampled.front(), line.front()) < 1e-6f);
  CHECK(hair::Distance(resampled.back(), line.back()) < 1e-6f);
  for (std::size_t i = 1; i < resampled.size(); ++i) {
    CHECK(std::fabs(hair::Distance(resampled[i - 1], resampled[i]) - 0.5f) <
          1e-4f);
  }

  // Smoothing with a pinned first point never moves index 0.
  std::vector<Vec3> zigzag = {Vec3{0, 0, 0}, Vec3{1, 1, 0}, Vec3{2, -1, 0},
                              Vec3{3, 1, 0}, Vec3{4, 0, 0}};
  const Vec3 root = zigzag.front();
  const float before = std::fabs(zigzag[1].y - zigzag[2].y);
  hair::SmoothPolyline(zigzag, 0.5f, 3, /*pinFirst=*/true);
  CHECK(hair::Distance(zigzag.front(), root) < 1e-6f);
  CHECK(std::fabs(zigzag[1].y - zigzag[2].y) < before);
  return true;
}

// ── seeding and root attachment ─────────────────────────────────────────────

bool TestSeededGuidesAreRootedOnScalp() {
  const hair::ScalpMesh scalp = hair::BuildScalpMesh(hair::ScalpParameters{});
  hair::SeedGuideParameters parameters;
  parameters.pointCount = 14;
  parameters.length = 0.62f;
  const hair::GuideSet guides = hair::SeedGuides(scalp, parameters);

  CHECK(guides.valid);
  CHECK(guides.curves.size() ==
        static_cast<std::size_t>(parameters.rings * parameters.perRing));
  for (const hair::GuideCurve& curve : guides.curves) {
    CHECK(curve.points.size() == static_cast<std::size_t>(parameters.pointCount));
    CHECK(std::fabs(EllipsoidRadius(scalp, curve.root()) - 1.0f) < 1e-3f);
    // Each step is a fixed arc length, so the total is the requested length.
    CHECK(std::fabs(curve.length() - parameters.length) < 1e-3f);
    // A seeded guide leaves the scalp rather than burrowing into it.
    CHECK(EllipsoidRadius(scalp, curve.tip()) > 1.0f);
  }

  // Seeding is deterministic.
  const hair::GuideSet again = hair::SeedGuides(scalp, parameters);
  CHECK(again.curves.size() == guides.curves.size());
  for (std::size_t i = 0; i < guides.curves.size(); ++i) {
    for (std::size_t j = 0; j < guides.curves[i].points.size(); ++j) {
      CHECK(hair::Distance(guides.curves[i].points[j],
                           again.curves[i].points[j]) < 1e-7f);
    }
  }
  return true;
}

// ── generation ──────────────────────────────────────────────────────────────

bool TestGenerationIsDeterministicAndBounded() {
  const hair::ScalpMesh scalp = hair::BuildScalpMesh(hair::ScalpParameters{});
  hair::SeedGuideParameters seed;
  const hair::GuideSet guides = hair::SeedGuides(scalp, seed);

  hair::HairGenerationParameters parameters;
  const hair::StrandSet a = hair::GenerateHair(scalp, guides, parameters);
  const hair::StrandSet b = hair::GenerateHair(scalp, guides, parameters);
  CHECK(a.valid);
  CHECK(a.strands.size() == static_cast<std::size_t>(parameters.density));
  CHECK(a.strands.size() >= static_cast<std::size_t>(hair::kMinStrandCount));
  CHECK(a.strands.size() <= static_cast<std::size_t>(hair::kMaxStrandCount));
  CHECK(a.strands.size() == b.strands.size());
  for (std::size_t i = 0; i < a.strands.size(); ++i) {
    CHECK(a.strands[i].points.size() == b.strands[i].points.size());
    for (std::size_t j = 0; j < a.strands[i].points.size(); ++j) {
      CHECK(hair::Distance(a.strands[i].points[j], b.strands[i].points[j]) <
            1e-7f);
    }
  }

  // Segment count is honored, and every strand has the same resolution.
  for (const hair::Strand& strand : a.strands) {
    CHECK(strand.points.size() ==
          static_cast<std::size_t>(parameters.segments + 1));
    CHECK(strand.points.size() >=
          static_cast<std::size_t>(hair::kMinStrandSegments + 1));
    CHECK(strand.points.size() <=
          static_cast<std::size_t>(hair::kMaxStrandSegments + 1));
  }

  // A different seed produces different hair from the same guides.
  hair::HairGenerationParameters reseeded = parameters;
  reseeded.seed = 4242u;
  const hair::StrandSet c = hair::GenerateHair(scalp, guides, reseeded);
  bool anyDifferent = false;
  for (std::size_t i = 0; i < a.strands.size() && !anyDifferent; ++i) {
    if (hair::Distance(a.strands[i].points.back(),
                       c.strands[i].points.back()) > 1e-4f) {
      anyDifferent = true;
    }
  }
  CHECK(anyDifferent);
  return true;
}

bool TestGenerationClampsOutOfRangeControls() {
  hair::HairGenerationParameters wild;
  wild.density = 1000000;
  wild.segments = 2;
  wild.width = 900.0f;
  const hair::HairGenerationParameters clamped =
      hair::ClampGenerationParameters(wild);
  CHECK(clamped.density == hair::kMaxStrandCount);
  CHECK(clamped.segments == hair::kMinStrandSegments);
  CHECK(clamped.width <= 0.06f);

  wild.density = -50;
  wild.segments = 900;
  const hair::HairGenerationParameters low =
      hair::ClampGenerationParameters(wild);
  CHECK(low.density == hair::kMinStrandCount);
  CHECK(low.segments == hair::kMaxStrandSegments);
  return true;
}

bool TestGeneratedRootsStayOnTheScalp() {
  const hair::ScalpMesh scalp = hair::BuildScalpMesh(hair::ScalpParameters{});
  const hair::GuideSet guides =
      hair::SeedGuides(scalp, hair::SeedGuideParameters{});
  hair::HairGenerationParameters parameters;
  parameters.density = hair::kMinStrandCount;
  const hair::StrandSet hairSet = hair::GenerateHair(scalp, guides, parameters);
  CHECK(hairSet.valid);

  for (const hair::Strand& strand : hairSet.strands) {
    CHECK(!strand.points.empty());
    // Root preservation is exact by construction, not approximate.
    CHECK(std::fabs(EllipsoidRadius(scalp, strand.points.front()) - 1.0f) <
          1e-4f);
    // And the strand actually grows away from the surface.
    CHECK(hair::Distance(strand.points.front(), strand.points.back()) > 0.05f);
  }
  return true;
}

bool TestMissingGenerationInputsProduceNothing() {
  const hair::ScalpMesh scalp = hair::BuildScalpMesh(hair::ScalpParameters{});
  const hair::GuideSet guides =
      hair::SeedGuides(scalp, hair::SeedGuideParameters{});

  const hair::StrandSet noScalp =
      hair::GenerateHair(hair::ScalpMesh{}, guides, {});
  CHECK(!noScalp.valid);
  CHECK(noScalp.strands.empty());

  const hair::StrandSet noGuides =
      hair::GenerateHair(scalp, hair::GuideSet{}, {});
  CHECK(!noGuides.valid);
  CHECK(noGuides.strands.empty());
  return true;
}

// ── clump and comb ──────────────────────────────────────────────────────────

bool TestClumpRegionsGatherCurvesAndPreserveRoots() {
  const hair::ScalpMesh scalp = hair::BuildScalpMesh(hair::ScalpParameters{});
  const hair::GuideSet guides =
      hair::SeedGuides(scalp, hair::SeedGuideParameters{});

  const std::vector<hair::ClumpSite> sites =
      hair::BuildClumpSites(scalp, 12, 5u);
  CHECK(sites.size() == 12);
  for (const hair::ClumpSite& site : sites) {
    CHECK(std::fabs(EllipsoidRadius(scalp, site.position) - 1.0f) < 1e-3f);
  }
  // Sites are deterministic.
  const std::vector<hair::ClumpSite> again =
      hair::BuildClumpSites(scalp, 12, 5u);
  for (std::size_t i = 0; i < sites.size(); ++i) {
    CHECK(hair::Distance(sites[i].position, again[i].position) < 1e-7f);
  }

  hair::ClumpState state;
  state.strength = 1.0f;
  state.tipBias = 2.0f;
  state.radius = 0.01f;  // no gizmo attraction: pure region clumping
  state.noise = 0.0f;
  const hair::ClumpPaint full = hair::MakeClumpPaint(1.0f);
  const hair::GuideSet clumped =
      hair::ApplyClumpToGuides(guides, sites, full, scalp, state, 7u);
  CHECK(clumped.valid);
  CHECK(clumped.curves.size() == guides.curves.size());

  // Roots never move, and influence rises toward the tips.
  bool anyTipMoved = false;
  for (std::size_t i = 0; i < guides.curves.size(); ++i) {
    const std::vector<Vec3>& before = guides.curves[i].points;
    const std::vector<Vec3>& after = clumped.curves[i].points;
    CHECK(hair::Distance(before.front(), after.front()) < 1e-6f);
    const std::size_t middle = before.size() / 2;
    CHECK(hair::Distance(before.back(), after.back()) >=
          hair::Distance(before[middle], after[middle]) - 1e-5f);
    if (hair::Distance(before.back(), after.back()) > 1e-4f) anyTipMoved = true;
  }
  CHECK(anyTipMoved);

  // Curves in one region converge: the spread of their tips shrinks.
  hair::ClumpCurves probe;
  for (const hair::GuideCurve& curve : guides.curves) {
    probe.points.push_back(curve.points);
    probe.rootUV.push_back(curve.rootUV);
  }
  const std::vector<int> regions = hair::AssignClumpRegions(probe, sites);
  CHECK(regions.size() == guides.curves.size());
  int chosen = -1;
  for (int region = 0; region < 12 && chosen < 0; ++region) {
    int members = 0;
    for (const int assigned : regions) {
      if (assigned == region) ++members;
    }
    if (members >= 3) chosen = region;
  }
  CHECK(chosen >= 0);

  const auto tipSpread = [&](const hair::GuideSet& set) {
    Vec3 mean;
    int count = 0;
    for (std::size_t i = 0; i < set.curves.size(); ++i) {
      if (regions[i] != chosen) continue;
      mean += set.curves[i].tip();
      ++count;
    }
    mean *= 1.0f / static_cast<float>(count);
    float spread = 0.0f;
    for (std::size_t i = 0; i < set.curves.size(); ++i) {
      if (regions[i] != chosen) continue;
      spread += hair::Distance(set.curves[i].tip(), mean);
    }
    return spread / static_cast<float>(count);
  };
  CHECK(tipSpread(clumped) < tipSpread(guides) * 0.6f);

  // Zero strength is a pass-through; an invalid input stays invalid.
  hair::ClumpState off = state;
  off.strength = 0.0f;
  const hair::GuideSet untouched =
      hair::ApplyClumpToGuides(guides, sites, full, scalp, off, 7u);
  for (std::size_t i = 0; i < guides.curves.size(); ++i) {
    CHECK(hair::Distance(guides.curves[i].tip(), untouched.curves[i].tip()) <
          1e-6f);
  }
  CHECK(!hair::ApplyClumpToGuides(hair::GuideSet{}, sites, full, scalp, state,
                                  7u)
             .valid);
  return true;
}

bool TestPaintedWeightGatesClumping() {
  const hair::ScalpMesh scalp = hair::BuildScalpMesh(hair::ScalpParameters{});
  const hair::GuideSet guides =
      hair::SeedGuides(scalp, hair::SeedGuideParameters{});
  const std::vector<hair::ClumpSite> sites =
      hair::BuildClumpSites(scalp, 12, 5u);

  hair::ClumpState state;
  state.strength = 1.0f;
  state.radius = 0.01f;
  state.noise = 0.0f;

  // A fully erased map disables clumping entirely, which is the whole point of
  // a paintable weight.
  const hair::ClumpPaint erased = hair::MakeClumpPaint(0.0f);
  const hair::GuideSet unclumped =
      hair::ApplyClumpToGuides(guides, sites, erased, scalp, state, 7u);
  for (std::size_t i = 0; i < guides.curves.size(); ++i) {
    CHECK(hair::Distance(guides.curves[i].tip(), unclumped.curves[i].tip()) <
          1e-5f);
  }

  // Sampling and painting.
  hair::ClumpPaint paint = hair::MakeClumpPaint(1.0f);
  CHECK(!paint.empty());
  CHECK(paint.width == hair::kClumpPaintWidth);
  CHECK(std::fabs(hair::SampleClumpWeight(paint, Vec2{0.3f, 0.4f}) - 1.0f) <
        1e-5f);
  // u wraps, because it is an azimuth.
  CHECK(std::fabs(hair::SampleClumpWeight(paint, Vec2{1.25f, 0.4f}) -
                  hair::SampleClumpWeight(paint, Vec2{0.25f, 0.4f})) < 1e-5f);

  Vec3 crownNormal;
  const Vec3 crown = hair::ScalpPointFromUV(scalp, Vec2{0.0f, 0.0f},
                                            &crownNormal);
  // The brush has a smooth falloff, so a stroke builds up rather than stamping
  // a hard-edged disc; a drag is several applications.
  const float before = hair::SampleClumpWeight(paint, Vec2{0.0f, 0.0f});
  // Wide enough to reach the innermost ring of seeded guide roots, which is
  // what makes the clumping assertion below meaningful.
  constexpr float kBrushRadius = 0.9f;
  const int changed =
      hair::PaintClumpWeight(scalp, paint, crown, kBrushRadius, 0.0f, 1.0f);
  CHECK(changed > 0);
  const float afterOne = hair::SampleClumpWeight(paint, Vec2{0.0f, 0.0f});
  CHECK(afterOne < before);
  for (int stroke = 0; stroke < 3; ++stroke) {
    hair::PaintClumpWeight(scalp, paint, crown, kBrushRadius, 0.0f, 1.0f);
  }
  // Erased at the crown, untouched at the rim on the far side.
  CHECK(hair::SampleClumpWeight(paint, Vec2{0.0f, 0.0f}) < 0.2f);
  CHECK(hair::SampleClumpWeight(paint, Vec2{0.5f, 0.95f}) > 0.9f);
  // Painting is bounded: erasing can never drive a weight below zero.
  CHECK(hair::SampleClumpWeight(paint, Vec2{0.0f, 0.0f}) >= 0.0f);

  // The partially painted map clumps less than a full one.
  const hair::GuideSet fullClump = hair::ApplyClumpToGuides(
      guides, sites, hair::MakeClumpPaint(1.0f), scalp, state, 7u);
  const hair::GuideSet paintedClump =
      hair::ApplyClumpToGuides(guides, sites, paint, scalp, state, 7u);
  float fullShift = 0.0f;
  float paintedShift = 0.0f;
  for (std::size_t i = 0; i < guides.curves.size(); ++i) {
    fullShift +=
        hair::Distance(guides.curves[i].tip(), fullClump.curves[i].tip());
    paintedShift +=
        hair::Distance(guides.curves[i].tip(), paintedClump.curves[i].tip());
  }
  // Guides rooted under the erased patch clump less; the rest are unaffected.
  CHECK(paintedShift < fullShift);
  CHECK(paintedShift > 0.0f);
  return true;
}

bool TestClumpMapImageDescribesRegionsAndWeights() {
  const hair::ScalpMesh scalp = hair::BuildScalpMesh(hair::ScalpParameters{});
  const std::vector<hair::ClumpSite> sites =
      hair::BuildClumpSites(scalp, 16, 5u);
  hair::ClumpPaint paint = hair::MakeClumpPaint(1.0f);
  Vec3 normal;
  const Vec3 crown = hair::ScalpPointFromUV(scalp, Vec2{0.0f, 0.0f}, &normal);
  for (int stroke = 0; stroke < 4; ++stroke) {
    hair::PaintClumpWeight(scalp, paint, crown, 0.4f, 0.0f, 1.0f);
  }

  const hair::ClumpMapImage image =
      hair::BuildClumpMapImage(scalp, sites, paint);
  CHECK(!image.empty());
  CHECK(image.width == paint.width);
  CHECK(image.height == paint.height);
  CHECK(image.pixels.size() ==
        static_cast<std::size_t>(image.width * image.height * 4));

  const auto ink = static_cast<std::uint8_t>(
      hair::Saturate(hair::kClumpBoundaryInk) * 255.0f + 0.5f);
  int boundary = 0;
  int erasedTexels = 0;
  for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
    if (image.pixels[i] == ink && image.pixels[i + 1] == ink) ++boundary;
    if (image.pixels[i + 3] < 64) ++erasedTexels;
  }
  // Voronoi cells produce visible boundaries, and the painted hole is present
  // in the alpha channel.
  CHECK(boundary > 0);
  CHECK(boundary < image.width * image.height / 2);
  CHECK(erasedTexels > 0);

  // Every region gets its own pastel: distinct from its neighbours, and
  // genuinely pastel (unsaturated and bright) rather than a brightness ramp of
  // one hue.
  std::vector<Vec3> colors;
  for (int region = 0; region < 16; ++region) {
    const Vec3 color = hair::PastelRegionColor(region);
    const float maxChannel =
        std::max(color.x, std::max(color.y, color.z));
    const float minChannel =
        std::min(color.x, std::min(color.y, color.z));
    CHECK(maxChannel > 0.75f);
    CHECK(maxChannel - minChannel < 0.55f);
    for (const Vec3& other : colors) {
      CHECK(hair::Distance(color, other) > 0.02f);
    }
    colors.push_back(color);
  }
  // Consecutive regions are far apart on the wheel, not adjacent shades.
  CHECK(hair::Distance(hair::PastelRegionColor(0),
                       hair::PastelRegionColor(1)) > 0.2f);
  return true;
}

bool TestCombBrushMovesOnlyNonRootPointsInRadius() {
  const hair::ScalpMesh scalp = hair::BuildScalpMesh(hair::ScalpParameters{});
  const hair::GuideSet guides =
      hair::SeedGuides(scalp, hair::SeedGuideParameters{});

  hair::BrushState brush;
  brush.center = guides.curves.front().tip();
  brush.radius = 0.25f;
  brush.strength = 1.0f;
  brush.falloff = 1.0f;
  brush.smoothing = 0.0f;
  brush.active = true;

  hair::SculptDeltas deltas;
  const Vec3 motion{0.05f, 0.0f, 0.0f};
  hair::ApplyCombBrush(guides, brush, motion, deltas);
  CHECK(deltas.size() == guides.curves.size());

  const hair::GuideSet sculpted = hair::ApplySculptDeltas(guides, deltas);
  bool anyMoved = false;
  for (std::size_t i = 0; i < guides.curves.size(); ++i) {
    // A root is never combed.
    CHECK(hair::Distance(guides.curves[i].root(), sculpted.curves[i].root()) <
          1e-6f);
    for (std::size_t j = 1; j < guides.curves[i].points.size(); ++j) {
      const float shift = hair::Distance(guides.curves[i].points[j],
                                         sculpted.curves[i].points[j]);
      if (shift > 1e-5f) anyMoved = true;
      // Nothing outside the brush radius may move.
      if (hair::Distance(guides.curves[i].points[j], brush.center) >=
          brush.radius) {
        CHECK(shift < 1e-5f);
      }
    }
  }
  CHECK(anyMoved);

  // Deltas survive an upstream point-count change by resampling.
  hair::GuideSet finer = guides;
  hair::ResampleGuideSet(finer, 22);
  hair::SculptDeltas conformed = deltas;
  hair::ConformDeltas(conformed, finer);
  CHECK(conformed.size() == finer.curves.size());
  for (std::size_t i = 0; i < conformed.size(); ++i) {
    CHECK(conformed[i].size() == finer.curves[i].points.size());
  }
  return true;
}

// ── camera and rays ─────────────────────────────────────────────────────────

bool TestCameraRaysAndProjection() {
  hair::Camera camera;
  camera.viewportWidth = 800.0f;
  camera.viewportHeight = 600.0f;

  // The ray through the viewport center points at the orbit target.
  const hair::Ray center = hair::CameraRayThroughPoint(camera, 400.0f, 300.0f);
  const Vec3 forward = hair::CameraForward(camera);
  CHECK(hair::Dot(center.direction, forward) > 0.9999f);
  CHECK(hair::Distance(center.origin, hair::CameraPosition(camera)) < 1e-5f);

  // Project/unproject round-trip: a point along a ray projects back to the
  // view coordinate the ray came from.
  const float samples[4][2] = {
      {400.0f, 300.0f}, {120.0f, 90.0f}, {700.0f, 520.0f}, {400.0f, 80.0f}};
  for (const auto& sample : samples) {
    const hair::Ray ray =
        hair::CameraRayThroughPoint(camera, sample[0], sample[1]);
    const Vec3 world = hair::PointOnRay(ray, 3.0f);
    Vec2 projected;
    CHECK(hair::ProjectToView(camera, world, &projected));
    CHECK(std::fabs(projected.x - sample[0]) < 0.05f);
    CHECK(std::fabs(projected.y - sample[1]) < 0.05f);
  }

  // A point behind the eye does not project.
  const Vec3 behind =
      hair::CameraPosition(camera) - hair::CameraForward(camera) * 2.0f;
  Vec2 unused;
  CHECK(!hair::ProjectToView(camera, behind, &unused));

  // Axis constraint: the pointer ray resolves onto the requested world axis,
  // and the solution actually tracks the pointer rather than mirroring it.
  const Vec3 anchor{0.2f, 1.4f, 0.1f};
  Vec2 anchorView;
  CHECK(hair::ProjectToView(camera, anchor, &anchorView));
  Vec3 onAxis;
  CHECK(hair::PointerOnAxis(camera, anchor, Vec3{1.0f, 0.0f, 0.0f},
                            anchorView.x + 40.0f, anchorView.y, &onAxis));
  CHECK(std::fabs(onAxis.y - anchor.y) < 1e-4f);
  CHECK(std::fabs(onAxis.z - anchor.z) < 1e-4f);
  CHECK(std::fabs(onAxis.x - anchor.x) > 1e-3f);
  // The result must land back under the pointer: solving the closest point
  // between two lines is sign-sensitive, and an inverted solution still
  // produces a plausible-looking point on the correct axis.
  Vec2 roundTrip;
  CHECK(hair::ProjectToView(camera, onAxis, &roundTrip));
  CHECK(std::fabs(roundTrip.x - (anchorView.x + 40.0f)) <
        std::fabs(roundTrip.x - anchorView.x));

  // Dragging along +Y in the world must raise the point, whichever way the
  // camera happens to be facing.
  Vec3 up;
  CHECK(hair::PointerOnAxis(camera, anchor, Vec3{0.0f, 1.0f, 0.0f},
                            anchorView.x, anchorView.y - 60.0f, &up));
  CHECK(up.y > anchor.y);
  Vec3 down;
  CHECK(hair::PointerOnAxis(camera, anchor, Vec3{0.0f, 1.0f, 0.0f},
                            anchorView.x, anchorView.y + 60.0f, &down));
  CHECK(down.y < anchor.y);
  return true;
}

// ── graph, evaluation, topology ─────────────────────────────────────────────

bool TestGraphShapeAndGroups() {
  const nd::GraphSnapshot graph = hair::BuildHairGraphSnapshot();
  CHECK(graph.nodes.size() == 6);
  CHECK(graph.edges.size() == 6);

  const char* required[] = {hair::ids::kScalp,        hair::ids::kCreateGuides,
                            hair::ids::kGuideSculpt,  hair::ids::kClump,
                            hair::ids::kGenerateHair, hair::ids::kOutput};
  for (const char* id : required) CHECK(hair::FindNode(graph, id) != nullptr);

  // The six documented property groups are all present.
  const char* groups[] = {"tool:", "draw:", "brush:",
                          "clump:", "hair:", "display:"};
  for (const char* group : groups) {
    bool found = false;
    for (const nd::GraphNode& node : graph.nodes) {
      for (const nd::GraphProperty& property : node.properties) {
        if (property.name.rfind(group, 0) == 0) found = true;
      }
    }
    CHECK(found);
  }

  // A group header row is named after the bare prefix, so no port may collide
  // with one. This is what forces GenerateHair's output to be `strands`.
  for (const nd::GraphNode& node : graph.nodes) {
    std::vector<std::string> prefixes;
    for (const nd::GraphProperty& property : node.properties) {
      const std::size_t colon = property.name.find(':');
      if (colon != std::string::npos) {
        prefixes.push_back(property.name.substr(0, colon));
      }
    }
    for (const nd::GraphProperty& property : node.properties) {
      if (property.name.find(':') != std::string::npos) continue;
      for (const std::string& prefix : prefixes) {
        CHECK(property.name != prefix);
      }
    }
  }

  // Boolean rows must be scrubable or GraphEditor treats a tap as an
  // activation instead of a toggle.
  for (const nd::GraphNode& node : graph.nodes) {
    for (const nd::GraphProperty& property : node.properties) {
      if (property.type == "bool") {
        CHECK(property.hasValue);
        CHECK(property.isScrubable);
      }
    }
  }

  // Documented wiring, in document-authoring orientation.
  std::string upstream;
  CHECK(hair::ResolveInputSource(graph, hair::ids::kCreateGuides,
                                 hair::ports::kScalpInput, &upstream, nullptr));
  CHECK(upstream == hair::ids::kScalp);
  CHECK(hair::ResolveInputSource(graph, hair::ids::kGenerateHair,
                                 hair::ports::kScalpInput, &upstream, nullptr));
  CHECK(upstream == hair::ids::kScalp);
  CHECK(hair::ResolveInputSource(graph, hair::ids::kGuideSculpt,
                                 hair::ports::kGuidesInput, &upstream,
                                 nullptr));
  CHECK(upstream == hair::ids::kCreateGuides);
  CHECK(hair::ResolveInputSource(graph, hair::ids::kClump,
                                 hair::ports::kCurvesInput, &upstream,
                                 nullptr));
  CHECK(upstream == hair::ids::kGuideSculpt);
  CHECK(hair::ResolveInputSource(graph, hair::ids::kGenerateHair,
                                 hair::ports::kGuidesInput, &upstream,
                                 nullptr));
  CHECK(upstream == hair::ids::kClump);
  CHECK(hair::ResolveInputSource(graph, hair::ids::kOutput,
                                 hair::ports::kStrands, &upstream, nullptr));
  CHECK(upstream == hair::ids::kGenerateHair);
  return true;
}

bool TestSceneEvaluatesAGroomOnLaunch() {
  hair::HairScene scene(MakeDocument());
  const hair::HairEvalResult& eval = scene.result();

  CHECK(eval.valid);
  CHECK(eval.status.empty());
  CHECK(eval.scalp.valid);
  CHECK(eval.guides.valid);
  CHECK(!eval.guides.curves.empty());
  CHECK(eval.guidesAtCreate.valid);
  CHECK(eval.hair.valid);
  CHECK(eval.hair.strands.size() >=
        static_cast<std::size_t>(hair::kMinStrandCount));
  CHECK(eval.hair.strands.size() <=
        static_cast<std::size_t>(hair::kMaxStrandCount));

  // Every stage of the default chain is present and identified.
  CHECK(eval.scalpNodeId == hair::ids::kScalp);
  CHECK(eval.createGuidesNodeId == hair::ids::kCreateGuides);
  CHECK(eval.sculptNodeId == hair::ids::kGuideSculpt);
  CHECK(eval.clumpNodeId == hair::ids::kClump);
  CHECK(eval.generateNodeId == hair::ids::kGenerateHair);
  CHECK(eval.outputNodeId == hair::ids::kOutput);

  // Hair roots are on the scalp on launch.
  for (const hair::Strand& strand : eval.hair.strands) {
    CHECK(std::fabs(EllipsoidRadius(eval.scalp, strand.points.front()) - 1.0f) <
          1e-4f);
  }
  return true;
}

bool TestTopologyDrivesEvaluation() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  CHECK(scene.result().valid);
  const std::size_t clumpedPoints = TotalGuidePoints(scene.result().guides);

  // Severing Output's hair input empties the result and explains why. It must
  // not keep showing the hair it produced a moment ago.
  CHECK(document->removeConnection(hair::ids::kOutput, hair::ports::kStrands,
                                   hair::ids::kGenerateHair,
                                   hair::ports::kStrands));
  scene.onTopologyEdited();
  {
    const hair::HairEvalResult& eval = scene.result();
    CHECK(!eval.valid);
    CHECK(eval.hair.strands.empty());
    CHECK(eval.guides.curves.empty());
    CHECK(!eval.status.empty());
    CHECK(eval.status.find("Output") != std::string::npos);
  }

  // Reconnecting restores it.
  CHECK(document->authorConnection(hair::ids::kOutput, hair::ports::kStrands,
                                   hair::ids::kGenerateHair,
                                   hair::ports::kStrands));
  scene.onTopologyEdited();
  CHECK(scene.result().valid);

  // Dropping the guides input leaves the scalp but no hair.
  CHECK(document->removeConnection(hair::ids::kGenerateHair,
                                   hair::ports::kGuidesInput, hair::ids::kClump,
                                   hair::ports::kClumpedOutput));
  scene.onTopologyEdited();
  {
    const hair::HairEvalResult& eval = scene.result();
    CHECK(!eval.valid);
    CHECK(eval.hair.strands.empty());
    CHECK(eval.scalp.valid);
    CHECK(eval.status.find("guides") != std::string::npos);
  }

  // Rewiring Generate Hair straight to Guide Sculpt bypasses Clump: hair comes
  // back, the clump stage drops out of the chain, and the guides differ from
  // the clumped ones.
  CHECK(document->authorConnection(
      hair::ids::kGenerateHair, hair::ports::kGuidesInput,
      hair::ids::kGuideSculpt, hair::ports::kSculptedOutput));
  scene.onTopologyEdited();
  {
    const hair::HairEvalResult& eval = scene.result();
    CHECK(eval.valid);
    CHECK(eval.clumpNodeId.empty());
    CHECK(!eval.display.showClumpGizmo);
    CHECK(TotalGuidePoints(eval.guides) == clumpedPoints);
    CHECK(eval.sculptNodeId == hair::ids::kGuideSculpt);
  }

  // Dropping Create Guides' scalp input breaks the guide chain entirely.
  CHECK(document->removeConnection(hair::ids::kCreateGuides,
                                   hair::ports::kScalpInput, hair::ids::kScalp,
                                   hair::ports::kMesh));
  scene.onTopologyEdited();
  {
    const hair::HairEvalResult& eval = scene.result();
    CHECK(!eval.valid);
    CHECK(eval.guides.curves.empty());
    CHECK(!eval.status.empty());
  }
  return true;
}

bool TestGraphParametersChangeTheGroom() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  const std::size_t baseStrands = scene.result().hair.strands.size();
  const int baseSegments = scene.result().generation.segments;

  CHECK(SetNumber(document, hair::ids::kGenerateHair,
                  hair::props::kHairDensity, 1400.0));
  scene.onAttributeEdited(hair::ids::kGenerateHair, hair::props::kHairDensity);
  CHECK(scene.result().hair.strands.size() == 1400);
  CHECK(scene.result().hair.strands.size() != baseStrands);

  CHECK(SetNumber(document, hair::ids::kGenerateHair,
                  hair::props::kHairSegments, 22.0));
  scene.onAttributeEdited(hair::ids::kGenerateHair, hair::props::kHairSegments);
  CHECK(scene.result().generation.segments == 22);
  CHECK(scene.result().generation.segments != baseSegments);
  CHECK(scene.result().hair.strands.front().points.size() == 23);

  // Unclamped scrubs are clamped on read, so the renderer's contract holds.
  CHECK(SetNumber(document, hair::ids::kGenerateHair,
                  hair::props::kHairDensity, 90000.0));
  scene.onAttributeEdited(hair::ids::kGenerateHair, hair::props::kHairDensity);
  CHECK(scene.result().generation.density == hair::kMaxStrandCount);
  CHECK(scene.result().hair.strands.size() ==
        static_cast<std::size_t>(hair::kMaxStrandCount));

  // Clump strength is live and visible in the evaluated guides.
  CHECK(SetNumber(document, hair::ids::kGenerateHair,
                  hair::props::kHairDensity, 600.0));
  scene.onAttributeEdited(hair::ids::kGenerateHair, hair::props::kHairDensity);
  const Vec3 tipBefore = scene.result().guides.curves.front().tip();
  CHECK(SetNumber(document, hair::ids::kClump, hair::props::kClumpStrength,
                  1.0));
  CHECK(SetNumber(document, hair::ids::kClump, hair::props::kClumpRadius, 4.0));
  scene.onAttributeEdited(hair::ids::kClump, hair::props::kClumpStrength);
  const Vec3 tipAfter = scene.result().guides.curves.front().tip();
  CHECK(hair::Distance(tipBefore, tipAfter) > 1e-3f);
  return true;
}

// ── dirty generations ───────────────────────────────────────────────────────

bool TestCameraMotionDoesNotRegenerateHair() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);

  scene.result();
  CHECK(scene.recomputedOnLastQuery());
  scene.result();
  CHECK(!scene.recomputedOnLastQuery());

  // Orbiting, trucking, dollying, and resizing all bump only the camera
  // generation; none of them may re-evaluate the groom.
  const std::uint64_t evaluation = scene.generations().evaluation;
  scene.orbitBy(35.0f, -18.0f);
  scene.result();
  CHECK(!scene.recomputedOnLastQuery());
  scene.truckBy(12.0f, 9.0f);
  scene.result();
  CHECK(!scene.recomputedOnLastQuery());
  scene.dollyBy(1.4f);
  scene.result();
  CHECK(!scene.recomputedOnLastQuery());
  scene.setViewport(1200.0f, 800.0f);
  scene.result();
  CHECK(!scene.recomputedOnLastQuery());
  CHECK(scene.generations().evaluation == evaluation);
  CHECK(scene.generations().camera > 1);

  // A parameter edit does re-evaluate.
  CHECK(SetNumber(document, hair::ids::kGenerateHair,
                  hair::props::kHairVariation, 0.6));
  scene.result();
  CHECK(scene.recomputedOnLastQuery());
  CHECK(scene.generations().evaluation > evaluation);
  return true;
}

// ── tools ───────────────────────────────────────────────────────────────────

bool TestToolSwitchesAreExclusive() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  CHECK(scene.activeTool() == hair::HairToolKind::None);

  scene.setActiveTool(hair::HairToolKind::CombBrush);
  CHECK(scene.activeTool() == hair::HairToolKind::CombBrush);
  const nd::GraphSnapshot graph = document->snapshot(0.0);
  CHECK(hair::PropertyBool(graph, hair::ids::kGuideSculpt,
                           hair::props::kToolComb, false));
  CHECK(!hair::PropertyBool(graph, hair::ids::kCreateGuides,
                            hair::props::kToolDraw, true));
  CHECK(!hair::PropertyBool(graph, hair::ids::kCreateGuides,
                            hair::props::kToolEditPoints, true));
  CHECK(!hair::PropertyBool(graph, hair::ids::kClump,
                            hair::props::kToolEditClump, true));

  // Turning a second switch on directly in the document — exactly what a row
  // tap in the editor does — makes the scene turn the previous one off.
  CHECK(SetNumber(document, hair::ids::kClump, hair::props::kToolEditClump,
                  1.0));
  scene.onAttributeEdited(hair::ids::kClump, hair::props::kToolEditClump);
  const nd::GraphSnapshot after = document->snapshot(0.0);
  CHECK(hair::PropertyBool(after, hair::ids::kClump,
                           hair::props::kToolEditClump, false));
  CHECK(!hair::PropertyBool(after, hair::ids::kGuideSculpt,
                            hair::props::kToolComb, true));
  CHECK(scene.activeTool() == hair::HairToolKind::EditClump);

  scene.setActiveTool(hair::HairToolKind::None);
  CHECK(scene.activeTool() == hair::HairToolKind::None);
  return true;
}

bool TestGestureOwnershipIsChosenOnPointerDown() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(800.0f, 600.0f);
  scene.result();

  // With no tool armed, a background press is a camera orbit.
  CHECK(scene.pointerDown(400.0f, 300.0f, /*alternate=*/false));
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::CameraOrbit);
  const float yawBefore = scene.camera().yaw;
  scene.pointerMove(460.0f, 300.0f);
  CHECK(std::fabs(scene.camera().yaw - yawBefore) > 1e-4f);
  scene.pointerUp(460.0f, 300.0f);
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::None);

  // The alternate modifier trucks instead of orbiting.
  const Vec3 targetBefore = scene.camera().target;
  CHECK(scene.pointerDown(400.0f, 300.0f, /*alternate=*/true));
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::CameraTruck);
  scene.pointerMove(430.0f, 330.0f);
  CHECK(hair::Distance(scene.camera().target, targetBefore) > 1e-4f);
  scene.pointerUp(430.0f, 330.0f);

  // Draw Guides claims a press that lands on the scalp...
  scene.setActiveTool(hair::HairToolKind::DrawGuides);
  Vec2 crown;
  CHECK(ViewOf(scene, scene.result().scalp.center + Vec3{0.0f, 1.0f, 0.0f} *
                                                        scene.result()
                                                            .scalp.radii.y,
               &crown));
  CHECK(scene.pointerDown(crown.x, crown.y, false));
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::Tool);
  scene.pointerUp(crown.x, crown.y);

  // ...and declines one that misses it, handing the gesture to the camera and
  // never switching mid-gesture.
  CHECK(scene.pointerDown(6.0f, 594.0f, false));
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::CameraOrbit);
  scene.pointerUp(6.0f, 594.0f);
  return true;
}

bool TestDrawGuidesToolCommitsAStroke() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(800.0f, 600.0f);
  const std::size_t before =
      scene.stores().guides.at(hair::ids::kCreateGuides).curves.size();
  scene.setActiveTool(hair::HairToolKind::DrawGuides);

  const hair::ScalpMesh scalp = scene.result().scalp;
  Vec2 start;
  CHECK(ViewOf(scene, scalp.center + Vec3{0.0f, scalp.radii.y, 0.0f}, &start));
  CHECK(scene.pointerDown(start.x, start.y, false));
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::Tool);

  // Drag away from the root, then check the live preview exists before commit.
  for (int step = 1; step <= 12; ++step) {
    scene.pointerMove(start.x + static_cast<float>(step) * 6.0f,
                      start.y - static_cast<float>(step) * 5.0f);
  }
  CHECK(scene.viewState().strokePreview.points.size() >= 2);

  scene.pointerUp(start.x + 72.0f, start.y - 60.0f);
  const hair::GuideSet& store =
      scene.stores().guides.at(hair::ids::kCreateGuides);
  CHECK(store.curves.size() == before + 1);

  const hair::GuideCurve& drawn = store.curves.back();
  const int pointCount = static_cast<int>(hair::PropertyNumber(
      document->snapshot(0.0), hair::ids::kCreateGuides,
      hair::props::kDrawPoints, 14.0));
  CHECK(drawn.points.size() == static_cast<std::size_t>(pointCount));
  // The drawn root is snapped onto the scalp.
  CHECK(std::fabs(EllipsoidRadius(scalp, drawn.root()) - 1.0f) < 1e-3f);
  // The stroke is clamped to the node's length control.
  const float maxLength = static_cast<float>(hair::PropertyNumber(
      document->snapshot(0.0), hair::ids::kCreateGuides,
      hair::props::kDrawLength, 0.62));
  CHECK(drawn.length() <= maxLength + 1e-3f);

  // The new guide reaches the evaluated output, and the preview is cleared.
  CHECK(scene.result().guidesAtCreate.curves.size() == before + 1);
  CHECK(scene.viewState().strokePreview.points.empty());
  return true;
}

bool TestEditPointsToolMovesAPointToTheCursor() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  scene.setActiveTool(hair::HairToolKind::EditPoints);

  // Pick a tip that is comfortably in front of the camera.
  const hair::GuideSet editable = scene.result().guidesAtCreate;
  CHECK(!editable.curves.empty());
  int chosenCurve = -1;
  Vec2 pickView;
  for (std::size_t i = 0; i < editable.curves.size(); ++i) {
    Vec2 view;
    if (!ViewOf(scene, editable.curves[i].tip(), &view)) continue;
    if (view.x < 120.0f || view.x > 780.0f) continue;
    if (view.y < 120.0f || view.y > 580.0f) continue;
    // Only accept a point that is genuinely the nearest one to that pixel, so
    // the test asserts on the point it thinks it grabbed.
    const hair::GuidePointRef nearest = hair::PickGuidePoint(
        editable, scene.camera(), view.x, view.y,
        hair::kPointPickRadiusPoints, true, nullptr);
    if (nearest.curveIndex != static_cast<int>(i)) continue;
    if (nearest.pointIndex !=
        static_cast<int>(editable.curves[i].points.size() - 1)) {
      continue;
    }
    chosenCurve = static_cast<int>(i);
    pickView = view;
    break;
  }
  CHECK(chosenCurve >= 0);

  CHECK(scene.pointerDown(pickView.x, pickView.y, false));
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::Tool);
  CHECK(scene.viewState().selectedPoint.curveIndex == chosenCurve);

  const Vec2 destination{pickView.x + 55.0f, pickView.y - 40.0f};
  scene.pointerMove(destination.x, destination.y);
  scene.pointerUp(destination.x, destination.y);

  // The evaluated point now projects to where the pointer was released: the
  // drag solves through smoothing rather than trailing behind it.
  const hair::GuideSet after = scene.result().guidesAtCreate;
  const std::vector<Vec3>& points =
      after.curves[static_cast<std::size_t>(chosenCurve)].points;
  Vec2 movedView;
  CHECK(hair::ProjectToView(scene.camera(), points.back(), &movedView));
  CHECK(std::fabs(movedView.x - destination.x) < 2.0f);
  CHECK(std::fabs(movedView.y - destination.y) < 2.0f);

  // Roots stayed on the scalp and other guides were untouched.
  const hair::ScalpMesh scalp = scene.result().scalp;
  for (const hair::GuideCurve& curve : after.curves) {
    CHECK(std::fabs(EllipsoidRadius(scalp, curve.root()) - 1.0f) < 1e-3f);
  }
  for (std::size_t i = 0; i < editable.curves.size(); ++i) {
    if (i == static_cast<std::size_t>(chosenCurve)) continue;
    CHECK(hair::Distance(editable.curves[i].tip(), after.curves[i].tip()) <
          1e-4f);
  }
  return true;
}

bool TestEditPointsAxisHandleConstrainsTheDrag() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  scene.setActiveTool(hair::HairToolKind::EditPoints);

  const hair::GuideSet editable = scene.result().guidesAtCreate;
  CHECK(!editable.curves.empty());

  // Select a point first: handles only exist around the selection. Which point
  // a click selects is decided by screen-space proximity, so the origin has to
  // be read back from the scene rather than assumed.
  Vec2 clickView;
  CHECK(ViewOf(scene, editable.curves.front().tip(), &clickView));
  CHECK(scene.pointerDown(clickView.x, clickView.y, false));
  scene.pointerUp(clickView.x, clickView.y);

  const hair::GuidePointRef selected = scene.viewState().selectedPoint;
  CHECK(selected.valid());
  const Vec3 origin =
      editable.curves[static_cast<std::size_t>(selected.curveIndex)]
          .points[static_cast<std::size_t>(selected.pointIndex)];

  // Grab the Y handle tip.
  const Vec3 handleTip =
      origin + Vec3{0.0f, 1.0f, 0.0f} * hair::kAxisHandleLength;
  Vec2 handleView;
  CHECK(ViewOf(scene, handleTip, &handleView));
  const hair::HairDragAxis picked =
      hair::PickAxisHandle(scene.camera(), origin, hair::kAxisHandleLength,
                           handleView.x, handleView.y,
                           hair::kHandlePickRadiusPoints);
  CHECK(picked == hair::HairDragAxis::AxisY);
  CHECK(scene.pointerDown(handleView.x, handleView.y, false));
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::Tool);
  CHECK(scene.viewState().activeAxis == hair::HairDragAxis::AxisY);

  scene.pointerMove(handleView.x + 30.0f, handleView.y - 45.0f);
  scene.pointerUp(handleView.x + 30.0f, handleView.y - 45.0f);

  const hair::GuideSet after = scene.result().guidesAtCreate;
  const Vec3 moved =
      after.curves[static_cast<std::size_t>(selected.curveIndex)]
          .points[static_cast<std::size_t>(selected.pointIndex)];
  // Constrained to Y: the point rose, and X/Z barely changed.
  CHECK(moved.y > origin.y + 1e-3f);
  CHECK(std::fabs(moved.x - origin.x) < 1e-2f);
  CHECK(std::fabs(moved.z - origin.z) < 1e-2f);
  return true;
}

bool TestCombBrushToolSculptsThroughTheNode() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  CHECK(SetNumber(document, hair::ids::kGuideSculpt,
                  hair::props::kBrushRadius, 0.6));
  CHECK(SetNumber(document, hair::ids::kGuideSculpt,
                  hair::props::kBrushStrength, 1.0));
  scene.onAttributeEdited(hair::ids::kGuideSculpt, hair::props::kBrushRadius);
  scene.setActiveTool(hair::HairToolKind::CombBrush);

  const hair::GuideSet before = scene.result().sculptInputGuides;
  CHECK(before.valid);

  // Start the stroke on a visible guide tip.
  int chosen = -1;
  Vec2 startView;
  for (std::size_t i = 0; i < scene.result().guides.curves.size(); ++i) {
    Vec2 view;
    if (!ViewOf(scene, scene.result().guides.curves[i].tip(), &view)) continue;
    if (view.x < 200.0f || view.x > 700.0f) continue;
    if (view.y < 150.0f || view.y > 550.0f) continue;
    chosen = static_cast<int>(i);
    startView = view;
    break;
  }
  CHECK(chosen >= 0);

  CHECK(scene.pointerDown(startView.x, startView.y, false));
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::Tool);
  CHECK(scene.viewState().brushVisible);
  CHECK(scene.viewState().brushStroking);
  for (int step = 1; step <= 8; ++step) {
    scene.pointerMove(startView.x + static_cast<float>(step) * 7.0f,
                      startView.y);
  }
  scene.pointerUp(startView.x + 56.0f, startView.y);

  const hair::SculptDeltas& deltas =
      scene.stores().sculpt.at(hair::ids::kGuideSculpt);
  CHECK(!deltas.empty());

  const hair::GuideSet after = scene.result().sculptInputGuides;
  const hair::GuideSet sculpted = hair::ApplySculptDeltas(after, deltas);
  bool moved = false;
  for (std::size_t i = 0; i < after.curves.size(); ++i) {
    // Roots are never combed.
    CHECK(hair::Distance(after.curves[i].root(), sculpted.curves[i].root()) <
          1e-6f);
    if (hair::Distance(after.curves[i].tip(), sculpted.curves[i].tip()) >
        1e-4f) {
      moved = true;
    }
  }
  CHECK(moved);

  // The sculpt is visible downstream of the node, in the guides that reach
  // Generate Hair.
  bool downstreamChanged = false;
  for (std::size_t i = 0; i < before.curves.size(); ++i) {
    if (hair::Distance(before.curves[i].tip(),
                       scene.result().guides.curves[i].tip()) > 1e-4f) {
      downstreamChanged = true;
    }
  }
  CHECK(downstreamChanged);
  return true;
}

bool TestEditClumpToolWritesNodeScalars() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  scene.setActiveTool(hair::HairToolKind::EditClump);

  const Vec3 centerBefore = scene.result().clump.center;
  Vec2 gizmoView;
  CHECK(ViewOf(scene, centerBefore, &gizmoView));

  // A press away from the gizmo is declined and orbits instead.
  CHECK(scene.pointerDown(gizmoView.x + 300.0f, gizmoView.y + 200.0f, false));
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::CameraOrbit);
  scene.pointerUp(gizmoView.x + 300.0f, gizmoView.y + 200.0f);

  // Re-project after the orbit moved the camera.
  CHECK(ViewOf(scene, centerBefore, &gizmoView));
  CHECK(scene.pointerDown(gizmoView.x, gizmoView.y, false));
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::Tool);
  scene.pointerMove(gizmoView.x + 40.0f, gizmoView.y - 30.0f);
  scene.pointerUp(gizmoView.x + 40.0f, gizmoView.y - 30.0f);

  // The gizmo has no private state: it authored the node's scalars.
  const nd::GraphSnapshot graph = document->snapshot(0.0);
  const Vec3 authored{
      static_cast<float>(hair::PropertyNumber(graph, hair::ids::kClump,
                                              hair::props::kClumpCenterX, 0.0)),
      static_cast<float>(hair::PropertyNumber(graph, hair::ids::kClump,
                                              hair::props::kClumpCenterY, 0.0)),
      static_cast<float>(hair::PropertyNumber(
          graph, hair::ids::kClump, hair::props::kClumpCenterZ, 0.0))};
  CHECK(hair::Distance(authored, centerBefore) > 1e-3f);
  CHECK(hair::Distance(scene.result().clump.center, authored) < 1e-5f);

  // And the groom followed it.
  Vec2 movedView;
  CHECK(ViewOf(scene, authored, &movedView));
  CHECK(std::fabs(movedView.x - (gizmoView.x + 40.0f)) < 2.0f);
  CHECK(std::fabs(movedView.y - (gizmoView.y - 30.0f)) < 2.0f);
  return true;
}

bool TestToolOnDisconnectedNodeDeclinesTheGesture() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  scene.setActiveTool(hair::HairToolKind::EditClump);
  const Vec3 center = scene.result().clump.center;

  // Unplug Clump from the chain. Its switch is still on, but the tool must not
  // edit geometry that no longer reaches the Output.
  CHECK(document->removeConnection(hair::ids::kGenerateHair,
                                   hair::ports::kGuidesInput, hair::ids::kClump,
                                   hair::ports::kClumpedOutput));
  CHECK(document->authorConnection(
      hair::ids::kGenerateHair, hair::ports::kGuidesInput,
      hair::ids::kGuideSculpt, hair::ports::kSculptedOutput));
  scene.onTopologyEdited();
  CHECK(scene.result().clumpNodeId.empty());

  Vec2 gizmoView;
  CHECK(ViewOf(scene, center, &gizmoView));
  CHECK(scene.pointerDown(gizmoView.x, gizmoView.y, false));
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::CameraOrbit);
  // Unplugged entirely, so there is no Clump stage to name at all.
  CHECK(scene.status().find("no Clump reaches the Output") !=
        std::string::npos);
  scene.pointerUp(gizmoView.x, gizmoView.y);
  return true;
}

// ── gizmos and render geometry ──────────────────────────────────────────────

bool TestDisplaySwitchesRevealGizmosWithoutChangingGeometry() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  scene.setActiveTool(hair::HairToolKind::EditClump);

  const hair::HairViewState shown = scene.viewState();
  CHECK(shown.clumpGizmoVisible);
  const hair::HairOverlayGeometry withGizmo =
      hair::BuildOverlayGeometry(shown);
  const std::size_t guidePoints = TotalGuidePoints(scene.result().guides);
  const std::size_t strandCount = scene.result().hair.strands.size();

  // Hiding the gizmo removes overlay geometry but must not touch the groom.
  CHECK(SetNumber(document, hair::ids::kClump, hair::props::kClumpGizmo, 0.0));
  scene.onAttributeEdited(hair::ids::kClump, hair::props::kClumpGizmo);
  const hair::HairViewState hidden = scene.viewState();
  CHECK(!hidden.clumpGizmoVisible);
  const hair::HairOverlayGeometry withoutGizmo =
      hair::BuildOverlayGeometry(hidden);
  CHECK(withoutGizmo.lineVertices.size() < withGizmo.lineVertices.size());
  CHECK(TotalGuidePoints(scene.result().guides) == guidePoints);
  CHECK(scene.result().hair.strands.size() == strandCount);

  // Guide gizmos and the grid follow their own switches.
  CHECK(SetNumber(document, hair::ids::kCreateGuides,
                  hair::props::kGuidesGizmos, 0.0));
  scene.onAttributeEdited(hair::ids::kCreateGuides,
                          hair::props::kGuidesGizmos);
  CHECK(hair::BuildOverlayGeometry(scene.viewState()).pointCount() == 0);
  CHECK(scene.result().hair.strands.size() == strandCount);

  CHECK(SetNumber(document, hair::ids::kOutput, hair::props::kShowGrid, 0.0));
  scene.onAttributeEdited(hair::ids::kOutput, hair::props::kShowGrid);
  const hair::HairOverlayGeometry noGrid =
      hair::BuildOverlayGeometry(scene.viewState());
  CHECK(noGrid.lineVertices.size() < withoutGizmo.lineVertices.size());

  // Hiding the hair empties the solid pass but leaves the strands evaluated.
  CHECK(SetNumber(document, hair::ids::kOutput, hair::props::kShowHair, 0.0));
  scene.onAttributeEdited(hair::ids::kOutput, hair::props::kShowHair);
  const hair::HairSolidGeometry hiddenHair =
      hair::BuildSolidGeometry(scene.result(), 0.005f);
  CHECK(hiddenHair.ribbonIndices.empty());
  CHECK(scene.result().hair.strands.size() == strandCount);

  // The clump map is off by default; turning its row on reveals the Voronoi
  // region markers.
  CHECK(SetNumber(document, hair::ids::kClump, hair::props::kClumpShowMap,
                  1.0));
  scene.onAttributeEdited(hair::ids::kClump, hair::props::kClumpShowMap);
  CHECK(hair::BuildOverlayGeometry(scene.viewState()).pointCount() > 0);
  CHECK(SetNumber(document, hair::ids::kScalp, hair::props::kScalpVisible,
                  0.0));
  scene.onAttributeEdited(hair::ids::kScalp, hair::props::kScalpVisible);
  CHECK(hair::BuildSolidGeometry(scene.result(), 0.005f).meshIndices.empty());
  CHECK(hair::BuildOverlayGeometry(scene.viewState()).pointCount() == 0);
  CHECK(scene.result().valid);
  return true;
}

bool TestRibbonGeometryIsCameraFacing() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  const hair::HairEvalResult& eval = scene.result();

  const float halfWidth = eval.generation.width;
  const hair::HairSolidGeometry geometry =
      hair::BuildSolidGeometry(eval, halfWidth);

  CHECK(geometry.strandCount == eval.hair.strands.size());
  const std::size_t points = eval.hair.strands.front().points.size();
  CHECK(geometry.pointsPerStrand == points);
  // Two vertices per point, six indices per segment.
  CHECK(geometry.ribbonVertices.size() ==
        geometry.strandCount * points * 2 * hair::kRibbonVertexFloats);
  CHECK(geometry.ribbonIndices.size() ==
        geometry.strandCount * (points - 1) * 6);
  for (const std::uint32_t index : geometry.ribbonIndices) {
    CHECK(index <
          geometry.ribbonVertices.size() / hair::kRibbonVertexFloats);
  }
  CHECK(!geometry.meshIndices.empty());

  // The scalp wireframe only appears when its switch is on.
  CHECK(geometry.meshWireIndices.empty());

  // Every ribbon expands perpendicular to both the view direction and the
  // strand tangent, which is what "camera-facing" means for a ribbon.
  const Vec3 cameraPosition = hair::CameraPosition(scene.camera());
  for (std::size_t vertex = 0;
       vertex + 1 < geometry.ribbonVertices.size() / hair::kRibbonVertexFloats;
       vertex += 257) {
    const float* data =
        &geometry.ribbonVertices[vertex * hair::kRibbonVertexFloats];
    const Vec3 position{data[0], data[1], data[2]};
    const Vec3 tangent{data[3], data[4], data[5]};
    const float side = data[6];
    const float width = data[9];
    const Vec3 expanded = hair::ExpandRibbonVertex(position, tangent,
                                                   cameraPosition, width, side);
    const Vec3 offset = expanded - position;
    CHECK(std::fabs(hair::Length(offset) - width) < 1e-4f);
    CHECK(std::fabs(hair::Dot(offset, hair::Normalized(tangent))) < 1e-4f);
    const Vec3 view = hair::Normalized(cameraPosition - position);
    CHECK(std::fabs(hair::Dot(offset, view)) < 1e-4f);
  }

  // The two sides of a point expand in opposite directions and the ribbon
  // tapers toward the tip.
  const std::size_t stride = hair::kRibbonVertexFloats;
  const Vec3 rootPosition{geometry.ribbonVertices[0],
                          geometry.ribbonVertices[1],
                          geometry.ribbonVertices[2]};
  const Vec3 rootTangent{geometry.ribbonVertices[3], geometry.ribbonVertices[4],
                         geometry.ribbonVertices[5]};
  const float rootWidth = geometry.ribbonVertices[9];
  const Vec3 left = hair::ExpandRibbonVertex(rootPosition, rootTangent,
                                             cameraPosition, rootWidth, -1.0f);
  const Vec3 right = hair::ExpandRibbonVertex(rootPosition, rootTangent,
                                              cameraPosition, rootWidth, 1.0f);
  CHECK(hair::Distance(left, right) > rootWidth);
  const float tipWidth =
      geometry.ribbonVertices[(points - 1) * 2 * stride + 9];
  CHECK(tipWidth < rootWidth);
  return true;
}

bool TestOverlayGeometryCarriesGizmosAndHover() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  scene.setActiveTool(hair::HairToolKind::EditPoints);

  // Hover feedback: moving over a guide point marks it hovered.
  const hair::GuideSet editable = scene.result().guidesAtCreate;
  Vec2 view;
  bool hovered = false;
  for (const hair::GuideCurve& curve : editable.curves) {
    if (!ViewOf(scene, curve.tip(), &view)) continue;
    scene.hoverMove(view.x, view.y);
    if (scene.viewState().hoveredPoint.valid()) {
      hovered = true;
      break;
    }
  }
  CHECK(hovered);

  const hair::HairOverlayGeometry geometry =
      hair::BuildOverlayGeometry(scene.viewState());
  CHECK(geometry.lineVertices.size() % (hair::kLineVertexFloats * 2) == 0);
  CHECK(geometry.pointVertices.size() % hair::kPointVertexFloats == 0);
  CHECK(geometry.lineCount() > 0);
  CHECK(geometry.pointCount() > 0);

  // Selecting a point adds three axis handles: three lines and three tips.
  const std::size_t linesBefore = geometry.lineCount();
  const std::size_t pointsBefore = geometry.pointCount();
  scene.pointerDown(view.x, view.y, false);
  scene.pointerUp(view.x, view.y);
  const hair::HairViewState selected = scene.viewState();
  CHECK(selected.selectedPoint.valid());
  const hair::HairOverlayGeometry withHandles =
      hair::BuildOverlayGeometry(selected);
  CHECK(withHandles.lineCount() == linesBefore + 3);
  CHECK(withHandles.pointCount() == pointsBefore + 3);

  // Hover away from any point clears it.
  scene.hoverMove(4.0f, 4.0f);
  CHECK(!scene.viewState().hoveredPoint.valid());
  return true;
}

bool TestGuideDisplaySwapsToTheEditableSetWhileEditing() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  // Push the clump hard so the editable and final guides visibly differ.
  CHECK(SetNumber(document, hair::ids::kClump, hair::props::kClumpStrength,
                  1.0));
  CHECK(SetNumber(document, hair::ids::kClump, hair::props::kClumpRadius, 5.0));
  scene.onAttributeEdited(hair::ids::kClump, hair::props::kClumpStrength);

  const hair::HairEvalResult& eval = scene.result();
  bool differ = false;
  for (std::size_t i = 0; i < eval.guides.curves.size(); ++i) {
    if (hair::Distance(eval.guides.curves[i].tip(),
                       eval.guidesAtCreate.curves[i].tip()) > 1e-3f) {
      differ = true;
    }
  }
  CHECK(differ);

  // With no tool armed the overlay shows the final guides.
  const hair::HairOverlayGeometry displayed =
      hair::BuildOverlayGeometry(scene.viewState());
  // With Edit Points armed it shows the set the tool actually edits.
  scene.setActiveTool(hair::HairToolKind::EditPoints);
  const hair::HairOverlayGeometry editing =
      hair::BuildOverlayGeometry(scene.viewState());
  CHECK(displayed.lineVertices.size() == editing.lineVertices.size());
  bool anyVertexDiffers = false;
  for (std::size_t i = 0; i < displayed.lineVertices.size(); ++i) {
    if (std::fabs(displayed.lineVertices[i] - editing.lineVertices[i]) >
        1e-4f) {
      anyVertexDiffers = true;
      break;
    }
  }
  CHECK(anyVertexDiffers);
  return true;
}

// ── node palette ────────────────────────────────────────────────────────────

bool TestNodePaletteBuildsCanonicalNodes() {
  const nd::GraphSnapshot fixture = hair::BuildHairGraphSnapshot();
  const struct {
    hair::HairNodeKind kind;
    const char* id;
  } kExpected[] = {
      {hair::HairNodeKind::Scalp, hair::ids::kScalp},
      {hair::HairNodeKind::CreateGuides, hair::ids::kCreateGuides},
      {hair::HairNodeKind::GuideSculpt, hair::ids::kGuideSculpt},
      {hair::HairNodeKind::Clump, hair::ids::kClump},
      {hair::HairNodeKind::GenerateHair, hair::ids::kGenerateHair},
      {hair::HairNodeKind::Output, hair::ids::kOutput},
  };
  CHECK(sizeof(kExpected) / sizeof(kExpected[0]) ==
        static_cast<std::size_t>(hair::kHairNodeKindCount));

  // A node added from the palette is indistinguishable from the one that
  // shipped in the fixture: same schema, same rows, same groups, same ports.
  for (const auto& expected : kExpected) {
    const nd::GraphNode* shipped = hair::FindNode(fixture, expected.id);
    CHECK(shipped != nullptr);
    const nd::GraphNode added =
        hair::MakeHairNode(expected.kind, "/Hair/Added", "Added");
    CHECK(added.schemaTypeName == shipped->schemaTypeName);
    CHECK(added.schemaTypeName == hair::HairNodeKindSchema(expected.kind));
    CHECK(added.properties.size() == shipped->properties.size());
    for (std::size_t i = 0; i < added.properties.size(); ++i) {
      CHECK(added.properties[i].name == shipped->properties[i].name);
      CHECK(added.properties[i].type == shipped->properties[i].type);
      CHECK(added.properties[i].direction == shipped->properties[i].direction);
      CHECK(added.properties[i].isScrubable ==
            shipped->properties[i].isScrubable);
    }
    CHECK(std::string(hair::HairNodeKindTitle(expected.kind)).size() > 0);
  }
  return true;
}

bool TestAddedClumpNodeParticipatesLikeTheOriginal() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  CHECK(scene.result().valid);
  CHECK(scene.result().clumpNodeId == hair::ids::kClump);

  const nd::GraphSnapshot before = document->snapshot(0.0);
  const std::size_t switchesBefore = hair::ToolSwitchesIn(before).size();

  // Add a second Clump from the palette and chain it after the first.
  CHECK(document->createNode(hair::MakeHairNode(hair::HairNodeKind::Clump,
                                                "/Hair/Clump2", "Clump 2")));
  CHECK(document->removeConnection(hair::ids::kGenerateHair,
                                   hair::ports::kGuidesInput, hair::ids::kClump,
                                   hair::ports::kClumpedOutput));
  CHECK(document->authorConnection("/Hair/Clump2", hair::ports::kCurvesInput,
                                   hair::ids::kClump,
                                   hair::ports::kClumpedOutput));
  CHECK(document->authorConnection(hair::ids::kGenerateHair,
                                   hair::ports::kGuidesInput, "/Hair/Clump2",
                                   hair::ports::kClumpedOutput));
  scene.onTopologyEdited();

  // The chain now ends at the new node, and its switches are discovered from
  // the graph rather than from a fixed list of ids.
  const nd::GraphSnapshot after = document->snapshot(0.0);
  // A Clump carries two switches (Edit Clump and Paint Clump), so adding one
  // adds both.
  CHECK(hair::ToolSwitchesIn(after).size() == switchesBefore + 2);
  CHECK(scene.result().valid);
  CHECK(scene.result().clumpNodeId == "/Hair/Clump2");

  // Its parameters drive the groom.
  const Vec3 tipBefore = scene.result().guides.curves.front().tip();
  CHECK(SetNumber(document, "/Hair/Clump2", hair::props::kClumpStrength, 1.0));
  CHECK(SetNumber(document, "/Hair/Clump2", hair::props::kClumpRadius, 5.0));
  scene.onAttributeEdited("/Hair/Clump2", hair::props::kClumpStrength);
  CHECK(hair::Distance(tipBefore, scene.result().guides.curves.front().tip()) >
        1e-3f);

  // Arming Edit Clump picks the node that is actually in the chain, and
  // exclusivity spans the added node too.
  scene.setActiveTool(hair::HairToolKind::EditClump);
  const hair::ActiveToolInfo active =
      hair::ActiveToolIn(document->snapshot(0.0));
  CHECK(active.kind == hair::HairToolKind::EditClump);
  CHECK(active.nodeId == "/Hair/Clump2");
  CHECK(!hair::PropertyBool(document->snapshot(0.0), hair::ids::kClump,
                            hair::props::kToolEditClump, true));

  // Turning the original node's switch on turns the new one off.
  CHECK(SetNumber(document, hair::ids::kClump, hair::props::kToolEditClump,
                  1.0));
  scene.onAttributeEdited(hair::ids::kClump, hair::props::kToolEditClump);
  CHECK(!hair::PropertyBool(document->snapshot(0.0), "/Hair/Clump2",
                            hair::props::kToolEditClump, true));
  CHECK(hair::ActiveToolIn(document->snapshot(0.0)).nodeId ==
        hair::ids::kClump);
  return true;
}

// ── clump on generated curves ───────────────────────────────────────────────

bool TestClumpWorksOnGeneratedCurvesToo() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  CHECK(scene.result().valid);
  CHECK(!scene.result().clumpsGeneratedCurves);

  // Baseline with the Clump in its shipped position, ahead of Generate Hair.
  const std::size_t strandCount = scene.result().hair.strands.size();
  std::vector<Vec3> guideChainTips;
  for (const hair::Strand& strand : scene.result().hair.strands) {
    guideChainTips.push_back(strand.points.back());
  }

  // Move the Clump downstream of Generate Hair: Generate Hair now feeds it,
  // and it feeds the Output. The same modifier clumps the generated curves.
  CHECK(document->removeConnection(hair::ids::kClump,
                                   hair::ports::kCurvesInput,
                                   hair::ids::kGuideSculpt,
                                   hair::ports::kSculptedOutput));
  CHECK(document->removeConnection(hair::ids::kGenerateHair,
                                   hair::ports::kGuidesInput, hair::ids::kClump,
                                   hair::ports::kClumpedOutput));
  CHECK(document->removeConnection(hair::ids::kOutput, hair::ports::kStrands,
                                   hair::ids::kGenerateHair,
                                   hair::ports::kStrands));
  CHECK(document->authorConnection(
      hair::ids::kGenerateHair, hair::ports::kGuidesInput,
      hair::ids::kGuideSculpt, hair::ports::kSculptedOutput));
  CHECK(document->authorConnection(hair::ids::kClump,
                                   hair::ports::kCurvesInput,
                                   hair::ids::kGenerateHair,
                                   hair::ports::kStrands));
  CHECK(document->authorConnection(hair::ids::kOutput, hair::ports::kStrands,
                                   hair::ids::kClump,
                                   hair::ports::kClumpedOutput));
  scene.onTopologyEdited();

  const hair::HairEvalResult& after = scene.result();
  CHECK(after.valid);
  CHECK(after.clumpsGeneratedCurves);
  CHECK(after.clumpNodeId == hair::ids::kClump);
  CHECK(after.generateNodeId == hair::ids::kGenerateHair);
  CHECK(after.hair.strands.size() == strandCount);
  // The regions and the map came along with it.
  CHECK(!after.clumpSites.empty());

  // Clumping generated curves gives a different result from clumping the
  // guides that produced them, and roots still never move.
  bool differs = false;
  for (std::size_t i = 0; i < after.hair.strands.size(); ++i) {
    if (hair::Distance(guideChainTips[i], after.hair.strands[i].points.back()) >
        1e-3f) {
      differs = true;
      break;
    }
  }
  CHECK(differs);
  for (const hair::Strand& strand : after.hair.strands) {
    CHECK(std::fabs(EllipsoidRadius(after.scalp, strand.points.front()) -
                    1.0f) < 1e-4f);
  }

  // Strength still drives it from the node, in this position too.
  const Vec3 tipBefore = after.hair.strands.front().points.back();
  CHECK(SetNumber(document, hair::ids::kClump, hair::props::kClumpStrength,
                  0.0));
  scene.onAttributeEdited(hair::ids::kClump, hair::props::kClumpStrength);
  CHECK(hair::Distance(tipBefore,
                       scene.result().hair.strands.front().points.back()) >
        1e-4f);
  return true;
}

bool TestPaintClumpToolPaintsThroughTheNode() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  CHECK(SetNumber(document, hair::ids::kClump, hair::props::kClumpPaintRadius,
                  0.9));
  CHECK(SetNumber(document, hair::ids::kClump, hair::props::kClumpPaintAmount,
                  1.0));
  CHECK(SetNumber(document, hair::ids::kClump, hair::props::kClumpPaintErase,
                  1.0));
  scene.onAttributeEdited(hair::ids::kClump, hair::props::kClumpPaintRadius);
  scene.setActiveTool(hair::HairToolKind::PaintClump);
  CHECK(scene.activeTool() == hair::HairToolKind::PaintClump);
  CHECK(scene.stores().clumpPaint.empty());

  // A press that misses the scalp is declined and orbits instead.
  CHECK(scene.pointerDown(6.0f, 694.0f, false));
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::CameraOrbit);
  scene.pointerUp(6.0f, 694.0f);

  // A press on the scalp paints.
  const hair::ScalpMesh scalp = scene.result().scalp;
  Vec2 crownView;
  CHECK(ViewOf(scene, scalp.center + Vec3{0.0f, scalp.radii.y, 0.0f},
               &crownView));
  const std::uint64_t paintGeneration = scene.generations().paintStore;
  CHECK(scene.pointerDown(crownView.x, crownView.y, false));
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::Tool);
  CHECK(scene.viewState().paintBrushVisible);
  CHECK(scene.viewState().paintErase);
  for (int step = 0; step < 4; ++step) {
    scene.pointerMove(crownView.x + static_cast<float>(step),
                      crownView.y + static_cast<float>(step));
  }
  scene.pointerUp(crownView.x + 4.0f, crownView.y + 4.0f);

  CHECK(scene.generations().paintStore > paintGeneration);
  const auto stored = scene.stores().clumpPaint.find(hair::ids::kClump);
  CHECK(stored != scene.stores().clumpPaint.end());
  CHECK(!stored->second.empty());
  CHECK(hair::SampleClumpWeight(stored->second, Vec2{0.0f, 0.0f}) < 0.5f);
  // Far from the stroke the map is untouched.
  CHECK(hair::SampleClumpWeight(stored->second, Vec2{0.5f, 0.95f}) > 0.9f);

  // The paint reached the evaluated result and the displayed map.
  CHECK(scene.result().display.showClumpMap);
  CHECK(!scene.result().clumpMapImage.empty());
  CHECK(hair::SampleClumpWeight(scene.result().clumpPaint, Vec2{0.0f, 0.0f}) <
        0.5f);
  return true;
}

// ── explicit clump curves ───────────────────────────────────────────────────

bool TestSuppliedClumpCurvesDriveTheCenters() {
  const hair::ScalpMesh scalp = hair::BuildScalpMesh(hair::ScalpParameters{});
  const hair::GuideSet guides =
      hair::SeedGuides(scalp, hair::SeedGuideParameters{});
  const std::vector<hair::ClumpSite> sites =
      hair::BuildClumpSites(scalp, 8, 5u);
  const hair::ClumpPaint full = hair::MakeClumpPaint(1.0f);

  hair::ClumpState state;
  state.strength = 1.0f;
  state.tipBias = 1.0f;
  state.radius = 0.01f;  // no gizmo attraction
  state.noise = 0.0f;

  // A sparse, deliberately different curve set to clump onto.
  hair::SeedGuideParameters clumpSeed;
  clumpSeed.rings = 2;
  clumpSeed.perRing = 4;
  clumpSeed.length = 1.1f;
  clumpSeed.sweep = 0.1f;  // near-vertical, unlike the swept groom
  clumpSeed.seed = 4242u;
  const hair::GuideSet clumpCurves = hair::SeedGuides(scalp, clumpSeed);
  CHECK(clumpCurves.valid);
  const hair::ClumpCenterCurves centers =
      hair::MakeClumpCenterCurves(clumpCurves);
  CHECK(centers.curves.size() == clumpCurves.curves.size());
  CHECK(centers.roots.size() == centers.curves.size());
  CHECK(!centers.empty());
  CHECK(hair::ClumpCenterCurves{}.empty());

  const hair::GuideSet ownMean =
      hair::ApplyClumpToGuides(guides, sites, full, scalp, state, 7u, nullptr);
  const hair::GuideSet supplied = hair::ApplyClumpToGuides(
      guides, sites, full, scalp, state, 7u, &centers);
  CHECK(supplied.valid);
  CHECK(supplied.curves.size() == guides.curves.size());

  // Clumping onto supplied curves is a different result from clumping onto the
  // members' own mean, and roots still never move.
  bool differs = false;
  for (std::size_t i = 0; i < guides.curves.size(); ++i) {
    CHECK(hair::Distance(guides.curves[i].root(), supplied.curves[i].root()) <
          1e-6f);
    if (hair::Distance(ownMean.curves[i].tip(), supplied.curves[i].tip()) >
        1e-3f) {
      differs = true;
    }
  }
  CHECK(differs);

  // At full strength the tips land on the supplied curve their region matched,
  // which is the whole point of an explicit clump input.
  float worstTipDistance = 0.0f;
  for (const hair::GuideCurve& curve : supplied.curves) {
    float nearest = -1.0f;
    for (const std::vector<Vec3>& center : centers.curves) {
      const float distance = hair::Distance(curve.tip(), center.back());
      if (nearest < 0.0f || distance < nearest) nearest = distance;
    }
    worstTipDistance = std::max(worstTipDistance, nearest);
  }
  CHECK(worstTipDistance < 0.08f);
  return true;
}

bool TestClumpCurvesInputIsTopologyDriven() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  CHECK(scene.result().valid);
  // Unconnected by default: regions clump onto their own members' mean.
  CHECK(!scene.result().clumpCurves.valid);
  const Vec3 tipBefore = scene.result().guides.curves.front().tip();

  // The real workflow: a second, sparser guide set whose only job is to define
  // the clump centers. Wiring the groom's own guides in would be a no-op,
  // because each region would match one of its own members.
  CHECK(document->createNode(hair::MakeHairNode(
      hair::HairNodeKind::CreateGuides, "/Hair/ClumpGuides", "Clump Guides")));
  CHECK(document->authorConnection("/Hair/ClumpGuides",
                                   hair::ports::kScalpInput, hair::ids::kScalp,
                                   hair::ports::kMesh));
  hair::SeedGuideParameters clumpSeed;
  clumpSeed.rings = 2;
  clumpSeed.perRing = 5;
  clumpSeed.length = 1.1f;
  clumpSeed.sweep = 0.1f;  // near-vertical, unlike the swept groom
  clumpSeed.seed = 4242u;
  scene.mutableStores().guides["/Hair/ClumpGuides"] =
      hair::SeedGuides(scene.result().scalp, clumpSeed);
  CHECK(document->authorConnection(hair::ids::kClump, hair::ports::kClumpInput,
                                   "/Hair/ClumpGuides",
                                   hair::ports::kGuidesOutput));
  scene.onTopologyEdited();
  {
    const hair::HairEvalResult& eval = scene.result();
    CHECK(eval.valid);
    CHECK(eval.clumpCurves.valid);
    CHECK(eval.clumpCurves.curves.size() == 10);
    CHECK(hair::Distance(tipBefore, eval.guides.curves.front().tip()) > 1e-3f);
    // The supplied curves are drawn, so the input is visible in the viewport.
    CHECK(hair::BuildOverlayGeometry(scene.viewState()).lineCount() > 0);
  }

  // A connected but broken clump input is reported, not silently ignored, and
  // the message names the node that actually broke rather than the Clump that
  // merely consumed it.
  CHECK(document->removeConnection("/Hair/ClumpGuides",
                                   hair::ports::kScalpInput, hair::ids::kScalp,
                                   hair::ports::kMesh));
  scene.onTopologyEdited();
  CHECK(!scene.result().clumpCurves.valid);
  CHECK(scene.result().status.find("Clump Guides") != std::string::npos);
  // The groom itself still evaluates: only the optional input is broken.
  CHECK(scene.result().valid);

  // Disconnecting restores the mean-of-members behaviour rather than leaving
  // the last supplied curves cached.
  CHECK(document->removeConnection(hair::ids::kClump, hair::ports::kClumpInput,
                                   "/Hair/ClumpGuides",
                                   hair::ports::kGuidesOutput));
  scene.onTopologyEdited();
  CHECK(scene.result().valid);
  CHECK(!scene.result().clumpCurves.valid);
  CHECK(hair::Distance(tipBefore, scene.result().guides.curves.front().tip()) <
        1e-4f);
  return true;
}

// A side input must not steal the groom's stages. Wiring a second Create
// Guides into a Clump's `clumps` input previously hijacked the guide tools,
// because the side branch evaluated last and overwrote the recorded stage.
bool TestSideBranchDoesNotHijackTheGuideTools() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  const std::size_t groomGuides = scene.result().guidesAtCreate.curves.size();
  CHECK(groomGuides > 0);
  CHECK(scene.result().createGuidesNodeId == hair::ids::kCreateGuides);

  CHECK(document->createNode(hair::MakeHairNode(
      hair::HairNodeKind::CreateGuides, "/Hair/ClumpGuides", "Clump Guides")));
  CHECK(document->authorConnection("/Hair/ClumpGuides",
                                   hair::ports::kScalpInput, hair::ids::kScalp,
                                   hair::ports::kMesh));
  hair::SeedGuideParameters clumpSeed;
  clumpSeed.rings = 2;
  clumpSeed.perRing = 5;
  clumpSeed.seed = 4242u;
  scene.mutableStores().guides["/Hair/ClumpGuides"] =
      hair::SeedGuides(scene.result().scalp, clumpSeed);
  CHECK(document->authorConnection(hair::ids::kClump, hair::ports::kClumpInput,
                                   "/Hair/ClumpGuides",
                                   hair::ports::kGuidesOutput));
  scene.onTopologyEdited();

  const hair::HairEvalResult& eval = scene.result();
  CHECK(eval.valid);
  // The clump input is used...
  CHECK(eval.clumpCurves.valid);
  CHECK(eval.clumpCurves.curves.size() == 10);
  // ...but the groom's own stages are still the ones the tools act on.
  CHECK(eval.createGuidesNodeId == hair::ids::kCreateGuides);
  CHECK(eval.guidesAtCreate.curves.size() == groomGuides);
  CHECK(eval.sculptNodeId == hair::ids::kGuideSculpt);
  CHECK(eval.scalpNodeId == hair::ids::kScalp);
  CHECK(eval.clumpNodeId == hair::ids::kClump);

  // Arming from the control panel picks the groom's Create Guides, not the
  // side branch's, and editing a point moves a groom guide.
  scene.setActiveTool(hair::HairToolKind::EditPoints);
  CHECK(hair::ActiveToolIn(document->snapshot(0.0)).nodeId ==
        hair::ids::kCreateGuides);

  // Arming the tool directly on the side-branch node instead is declined, and
  // the message names both the armed node and the one that actually feeds the
  // Output rather than claiming nothing is connected.
  CHECK(SetNumber(document, hair::ids::kCreateGuides,
                  hair::props::kToolEditPoints, 0.0));
  CHECK(SetNumber(document, "/Hair/ClumpGuides", hair::props::kToolEditPoints,
                  1.0));
  scene.onAttributeEdited("/Hair/ClumpGuides", hair::props::kToolEditPoints);
  CHECK(scene.activeTool() == hair::HairToolKind::EditPoints);
  Vec2 view;
  CHECK(ViewOf(scene, eval.guidesAtCreate.curves.front().tip(), &view));
  CHECK(scene.pointerDown(view.x, view.y, false));
  CHECK(scene.gestureOwner() == hair::HairGestureOwner::CameraOrbit);
  CHECK(scene.status().find("Clump Guides") != std::string::npos);
  CHECK(scene.status().find("Create Guides") != std::string::npos);
  scene.pointerUp(view.x, view.y);
  return true;
}

// Chained Clumps: the most downstream one is the stage, and each keeps its own
// paint map.
bool TestChainedClumpsResolveToTheDownstreamStage() {
  auto document = MakeDocument();
  hair::HairScene scene(document);
  scene.setViewport(900.0f, 700.0f);
  CHECK(scene.result().clumpNodeId == hair::ids::kClump);

  CHECK(document->createNode(hair::MakeHairNode(hair::HairNodeKind::Clump,
                                                "/Hair/Clump2", "Clump 2")));
  CHECK(document->removeConnection(hair::ids::kGenerateHair,
                                   hair::ports::kGuidesInput, hair::ids::kClump,
                                   hair::ports::kClumpedOutput));
  CHECK(document->authorConnection("/Hair/Clump2", hair::ports::kCurvesInput,
                                   hair::ids::kClump,
                                   hair::ports::kClumpedOutput));
  CHECK(document->authorConnection(hair::ids::kGenerateHair,
                                   hair::ports::kGuidesInput, "/Hair/Clump2",
                                   hair::ports::kClumpedOutput));
  scene.onTopologyEdited();
  CHECK(scene.result().valid);
  CHECK(scene.result().clumpNodeId == "/Hair/Clump2");

  // Arming Edit Clump picks the downstream one, whose gizmo is the visible
  // one, rather than the first Clump in graph order.
  scene.setActiveTool(hair::HairToolKind::EditClump);
  CHECK(hair::ActiveToolIn(document->snapshot(0.0)).nodeId == "/Hair/Clump2");
  CHECK(!hair::PropertyBool(document->snapshot(0.0), hair::ids::kClump,
                            hair::props::kToolEditClump, true));

  // Paint maps are per node: painting one leaves the other untouched.
  scene.setActiveTool(hair::HairToolKind::PaintClump);
  CHECK(hair::ActiveToolIn(document->snapshot(0.0)).nodeId == "/Hair/Clump2");
  const hair::ScalpMesh scalp = scene.result().scalp;
  Vec2 crown;
  CHECK(ViewOf(scene, scalp.center + Vec3{0.0f, scalp.radii.y, 0.0f}, &crown));
  CHECK(scene.pointerDown(crown.x, crown.y, false));
  scene.pointerUp(crown.x, crown.y);
  CHECK(scene.stores().clumpPaint.count("/Hair/Clump2") == 1);
  CHECK(scene.stores().clumpPaint.count(hair::ids::kClump) == 0);
  return true;
}

struct TestCase {
  const char* name;
  bool (*run)();
};

}  // namespace

int main() {
  const TestCase tests[] = {
      {"scalp_mesh_is_well_formed", TestScalpMeshIsWellFormed},
      {"scalp_ray_hits", TestScalpRayHits},
      {"polyline_resample_and_smooth", TestPolylineResampleAndSmooth},
      {"seeded_guides_rooted_on_scalp", TestSeededGuidesAreRootedOnScalp},
      {"generation_deterministic_and_bounded",
       TestGenerationIsDeterministicAndBounded},
      {"generation_clamps_controls", TestGenerationClampsOutOfRangeControls},
      {"generated_roots_on_scalp", TestGeneratedRootsStayOnTheScalp},
      {"missing_generation_inputs_produce_nothing",
       TestMissingGenerationInputsProduceNothing},
      {"clump_regions_gather_curves",
       TestClumpRegionsGatherCurvesAndPreserveRoots},
      {"painted_weight_gates_clumping", TestPaintedWeightGatesClumping},
      {"clump_map_image_describes_regions",
       TestClumpMapImageDescribesRegionsAndWeights},
      {"comb_brush_moves_non_root_points",
       TestCombBrushMovesOnlyNonRootPointsInRadius},
      {"camera_rays_and_projection", TestCameraRaysAndProjection},
      {"graph_shape_and_groups", TestGraphShapeAndGroups},
      {"scene_evaluates_groom_on_launch", TestSceneEvaluatesAGroomOnLaunch},
      {"topology_drives_evaluation", TestTopologyDrivesEvaluation},
      {"graph_parameters_change_groom", TestGraphParametersChangeTheGroom},
      {"camera_motion_does_not_regenerate",
       TestCameraMotionDoesNotRegenerateHair},
      {"tool_switches_are_exclusive", TestToolSwitchesAreExclusive},
      {"gesture_ownership_on_pointer_down",
       TestGestureOwnershipIsChosenOnPointerDown},
      {"draw_guides_commits_stroke", TestDrawGuidesToolCommitsAStroke},
      {"edit_points_moves_to_cursor",
       TestEditPointsToolMovesAPointToTheCursor},
      {"edit_points_axis_handle", TestEditPointsAxisHandleConstrainsTheDrag},
      {"comb_brush_sculpts_through_node",
       TestCombBrushToolSculptsThroughTheNode},
      {"edit_clump_writes_node_scalars", TestEditClumpToolWritesNodeScalars},
      {"tool_on_disconnected_node_declines",
       TestToolOnDisconnectedNodeDeclinesTheGesture},
      {"display_switches_reveal_gizmos",
       TestDisplaySwitchesRevealGizmosWithoutChangingGeometry},
      {"ribbon_geometry_is_camera_facing", TestRibbonGeometryIsCameraFacing},
      {"overlay_geometry_gizmos_and_hover",
       TestOverlayGeometryCarriesGizmosAndHover},
      {"guide_display_swaps_while_editing",
       TestGuideDisplaySwapsToTheEditableSetWhileEditing},
      {"node_palette_builds_canonical_nodes",
       TestNodePaletteBuildsCanonicalNodes},
      {"added_clump_node_participates",
       TestAddedClumpNodeParticipatesLikeTheOriginal},
      {"clump_works_on_generated_curves", TestClumpWorksOnGeneratedCurvesToo},
      {"paint_clump_tool_paints_through_node",
       TestPaintClumpToolPaintsThroughTheNode},
      {"supplied_clump_curves_drive_centers",
       TestSuppliedClumpCurvesDriveTheCenters},
      {"clump_curves_input_is_topology_driven",
       TestClumpCurvesInputIsTopologyDriven},
      {"side_branch_does_not_hijack_guide_tools",
       TestSideBranchDoesNotHijackTheGuideTools},
      {"chained_clumps_resolve_downstream",
       TestChainedClumpsResolveToTheDownstreamStage},
  };

  bool passed = true;
  for (const TestCase& test : tests) {
    std::printf("[ RUN  ] %s\n", test.name);
    if (test.run()) {
      std::printf("[ PASS ] %s\n", test.name);
    } else {
      std::printf("[ FAIL ] %s\n", test.name);
      passed = false;
    }
  }
  std::printf("%s\n", passed ? "ALL PASSED" : "FAILURES");
  return passed ? 0 : 1;
}
