// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT

#include "HairGeneration.h"

#include "HairGeometry.h"

#include <algorithm>
#include <cmath>

namespace noodles::demo::hair {
namespace {

constexpr float kTwoPi = 6.28318530717958647692f;
// The golden angle spreads successive roots as far apart as possible, so any
// prefix of the sequence is already well distributed. That is what makes
// raising the density refine the layout instead of reshuffling it.
constexpr float kGoldenAngle = 2.39996322972865332f;
constexpr int kGuideBlendCount = 3;

struct GuideShape {
  Vec3 root;
  // Offsets from the root, in world space.
  std::vector<Vec3> offsets;
};

// Push a generated point back out to the scalp if the blend put it inside.
// World-space interpolation is what preserves the groom's direction, and this
// is the one place it needs help: a strand rooted between two guides can dip
// under the surface on a convex dome.
Vec3 PushOutOfScalp(const ScalpMesh& scalp, const Vec3& point) {
  if (!scalp.valid) return point;
  const Vec3 radii{scalp.radii.x > 1e-6f ? scalp.radii.x : 1e-6f,
                   scalp.radii.y > 1e-6f ? scalp.radii.y : 1e-6f,
                   scalp.radii.z > 1e-6f ? scalp.radii.z : 1e-6f};
  const Vec3 local{(point.x - scalp.center.x) / radii.x,
                   (point.y - scalp.center.y) / radii.y,
                   (point.z - scalp.center.z) / radii.z};
  const float radius = Length(local);
  if (radius >= 1.0f || radius <= 1e-6f) return point;
  const float scale = 1.0f / radius;
  return Vec3{scalp.center.x + local.x * radii.x * scale,
              scalp.center.y + local.y * radii.y * scale,
              scalp.center.z + local.z * radii.z * scale};
}

}  // namespace

HairGenerationParameters ClampGenerationParameters(
    const HairGenerationParameters& raw) {
  HairGenerationParameters clamped = raw;
  clamped.density = ClampInt(raw.density, kMinStrandCount, kMaxStrandCount);
  clamped.segments =
      ClampInt(raw.segments, kMinStrandSegments, kMaxStrandSegments);
  clamped.width = Clamp(raw.width, 0.0004f, 0.06f);
  clamped.spread = Clamp(raw.spread, 0.0f, 1.5f);
  clamped.variation = Clamp(raw.variation, 0.0f, 1.5f);
  return clamped;
}

void DistributeRoots(const ScalpMesh& scalp, int count, float spread,
                     std::uint32_t seed, std::vector<Vec3>* outPositions,
                     std::vector<Vec3>* outNormals,
                     std::vector<Vec2>* outUVs) {
  if (outPositions) outPositions->clear();
  if (outNormals) outNormals->clear();
  if (outUVs) outUVs->clear();
  if (!scalp.valid || count <= 0) return;
  if (outPositions) outPositions->reserve(static_cast<std::size_t>(count));
  if (outNormals) outNormals->reserve(static_cast<std::size_t>(count));
  if (outUVs) outUVs->reserve(static_cast<std::size_t>(count));

  const float jitter = Clamp(spread, 0.0f, 1.5f);
  for (int i = 0; i < count; ++i) {
    const std::uint32_t rootSeed =
        HashCombine(seed, static_cast<std::uint32_t>(i));
    // Sample uniformly in the dome's normalized height. On a sphere, equal
    // height bands carry equal area (Archimedes), so this is area-uniform
    // without any rejection sampling.
    const float spacing = 1.0f / static_cast<float>(count);
    float u = (static_cast<float>(i) + 0.5f) * spacing;
    // Jitter is scaled by the sample spacing, so it stays a sub-cell dither at
    // any density instead of collapsing the distribution at high counts.
    u = Saturate(u + RandomSignedFloat(HashCombine(rootSeed, 3u)) * jitter *
                         spacing * 0.9f);
    float azimuth = std::fmod(static_cast<float>(i) * kGoldenAngle, kTwoPi);
    azimuth += RandomSignedFloat(HashCombine(rootSeed, 7u)) * jitter * 0.12f;

    // Leave a small margin at the rim so no root lands exactly on the cut.
    const float v = Saturate(u) * 0.94f;
    Vec2 uv{azimuth / kTwoPi, v};
    uv.x = uv.x - std::floor(uv.x);

    Vec3 normal;
    const Vec3 position = ScalpPointFromUV(scalp, uv, &normal);
    if (outPositions) outPositions->push_back(position);
    if (outNormals) outNormals->push_back(normal);
    if (outUVs) outUVs->push_back(uv);
  }
}

StrandSet GenerateHair(const ScalpMesh& scalp, const GuideSet& guides,
                       const HairGenerationParameters& rawParameters) {
  StrandSet hair;
  // A missing or empty input yields an explicitly invalid, empty result. The
  // caller reports that as a status message rather than drawing stale hair.
  if (!scalp.valid || !guides.valid || guides.curves.empty()) return hair;

  const HairGenerationParameters parameters =
      ClampGenerationParameters(rawParameters);
  const int pointCount = parameters.segments + 1;

  // Resample every guide to the strand resolution once and store its shape as
  // world-space offsets from its own root.
  //
  // The offsets stay in world space on purpose. Re-expressing them in each
  // target root's surface frame would make every strand sweep away from its
  // own root in the same *local* direction, which on a dome reads as a radial
  // spike ball rather than as hair with a direction. Blending in world space
  // keeps the groom pointing the way the guides do; PushOutOfScalp handles the
  // one artifact that costs.
  std::vector<GuideShape> guideShapes;
  guideShapes.reserve(guides.curves.size());
  for (const GuideCurve& curve : guides.curves) {
    if (curve.points.size() < 2) continue;
    const std::vector<Vec3> resampled =
        ResampleByArcLength(curve.points, pointCount);
    GuideShape shape;
    shape.root = resampled.front();
    shape.offsets.reserve(resampled.size());
    for (const Vec3& point : resampled) {
      shape.offsets.push_back(point - shape.root);
    }
    guideShapes.push_back(std::move(shape));
  }
  if (guideShapes.empty()) return hair;

  std::vector<Vec3> rootPositions;
  std::vector<Vec3> rootNormals;
  std::vector<Vec2> rootUVs;
  DistributeRoots(scalp, parameters.density, parameters.spread, parameters.seed,
                  &rootPositions, &rootNormals, &rootUVs);
  if (rootPositions.empty()) return hair;

  hair.strands.reserve(rootPositions.size());
  for (std::size_t strandIndex = 0; strandIndex < rootPositions.size();
       ++strandIndex) {
    const std::uint32_t strandSeed = HashCombine(
        parameters.seed ^ 0x5bf03635u, static_cast<std::uint32_t>(strandIndex));
    const Vec3 rootPosition = rootPositions[strandIndex];

    // Pick the nearest guides by root distance. A small fixed fan-in keeps the
    // blend local, so a guide edited on one side of the head cannot swing hair
    // on the other side.
    int nearestIndex[kGuideBlendCount] = {-1, -1, -1};
    float nearestDistance[kGuideBlendCount] = {0.0f, 0.0f, 0.0f};
    for (std::size_t guideIndex = 0; guideIndex < guideShapes.size();
         ++guideIndex) {
      const float distance =
          Distance(rootPosition, guideShapes[guideIndex].root);
      for (int slot = 0; slot < kGuideBlendCount; ++slot) {
        if (nearestIndex[slot] < 0 || distance < nearestDistance[slot]) {
          for (int shift = kGuideBlendCount - 1; shift > slot; --shift) {
            nearestIndex[shift] = nearestIndex[shift - 1];
            nearestDistance[shift] = nearestDistance[shift - 1];
          }
          nearestIndex[slot] = static_cast<int>(guideIndex);
          nearestDistance[slot] = distance;
          break;
        }
      }
    }

    float weights[kGuideBlendCount] = {0.0f, 0.0f, 0.0f};
    float weightTotal = 0.0f;
    for (int slot = 0; slot < kGuideBlendCount; ++slot) {
      if (nearestIndex[slot] < 0) continue;
      const float distance = nearestDistance[slot];
      weights[slot] = 1.0f / (distance * distance + 1e-4f);
      weightTotal += weights[slot];
    }
    if (weightTotal <= 0.0f) {
      weights[0] = 1.0f;
      weightTotal = 1.0f;
      if (nearestIndex[0] < 0) nearestIndex[0] = 0;
    }

    // Seeded per-strand variation. Length and width are scaled, and a drift
    // direction bends the strand progressively; all of it is weighted by t so
    // the root stays exactly on the scalp.
    const float variation = parameters.variation;
    const float lengthScale =
        1.0f + RandomSignedFloat(HashCombine(strandSeed, 17u)) * variation *
                   0.45f;
    const Vec3 drift =
        Vec3{RandomSignedFloat(HashCombine(strandSeed, 29u)),
             RandomSignedFloat(HashCombine(strandSeed, 31u)) * 0.4f,
             RandomSignedFloat(HashCombine(strandSeed, 37u))} *
        (variation * 0.06f);

    Strand strand;
    strand.widthScale =
        1.0f + RandomSignedFloat(HashCombine(strandSeed, 41u)) * variation *
                   0.5f;
    strand.shade =
        Saturate(0.5f + RandomSignedFloat(HashCombine(strandSeed, 43u)) *
                            variation * 0.9f);
    strand.rootUV = rootUVs[strandIndex];
    strand.points.reserve(static_cast<std::size_t>(pointCount));

    for (int point = 0; point < pointCount; ++point) {
      const float t =
          static_cast<float>(point) / static_cast<float>(pointCount - 1);
      Vec3 blended;
      for (int slot = 0; slot < kGuideBlendCount; ++slot) {
        const int guideIndex = nearestIndex[slot];
        if (guideIndex < 0) continue;
        const std::vector<Vec3>& offsets =
            guideShapes[static_cast<std::size_t>(guideIndex)].offsets;
        blended +=
            offsets[static_cast<std::size_t>(point)] *
            (weights[slot] / weightTotal);
      }
      blended *= lengthScale;
      // t² keeps the drift off the root entirely and lets it accumulate toward
      // the tip, which is where real strands separate.
      Vec3 position = rootPosition + blended + drift * (t * t);
      if (point > 0) position = PushOutOfScalp(scalp, position);
      strand.points.push_back(position);
    }
    // Exact root preservation: strand[0] is the scalp point by construction,
    // whatever the blend did above.
    strand.points.front() = rootPosition;
    hair.strands.push_back(std::move(strand));
  }

  hair.valid = true;
  return hair;
}

}  // namespace noodles::demo::hair
