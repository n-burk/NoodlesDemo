// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT

#include "HairClumpMap.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace noodles::demo::hair {
namespace {

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kGoldenAngle = 2.39996322972865332f;

float WrapUnit(float value) { return value - std::floor(value); }

// Texel center as a scalp UV.
Vec2 TexelUV(int x, int y, int width, int height) {
  return Vec2{(static_cast<float>(x) + 0.5f) / static_cast<float>(width),
              (static_cast<float>(y) + 0.5f) / static_cast<float>(height)};
}

}  // namespace

ClumpPaint MakeClumpPaint(float initialWeight) {
  ClumpPaint paint;
  paint.width = kClumpPaintWidth;
  paint.height = kClumpPaintHeight;
  paint.weights.assign(static_cast<std::size_t>(paint.width) *
                           static_cast<std::size_t>(paint.height),
                       Saturate(initialWeight));
  return paint;
}

float SampleClumpWeight(const ClumpPaint& paint, const Vec2& uv,
                        float fallback) {
  if (paint.empty()) return fallback;

  const float u = WrapUnit(uv.x) * static_cast<float>(paint.width) - 0.5f;
  const float v = Saturate(uv.y) * static_cast<float>(paint.height) - 0.5f;
  const int x0 = static_cast<int>(std::floor(u));
  // The blend fractions come from the UNCLAMPED floor. Deriving fy from the
  // clamped row would make it negative just past the pole and extrapolate
  // outside the texture instead of clamping to its edge.
  const int baseY = static_cast<int>(std::floor(v));
  const float fx = u - static_cast<float>(x0);
  const float fy = v - static_cast<float>(baseY);
  const int y0 = ClampInt(baseY, 0, paint.height - 1);
  const int y1 = ClampInt(baseY + 1, 0, paint.height - 1);

  // u wraps because it is an azimuth; v clamps because it is a polar angle.
  const int wrapX0 = ((x0 % paint.width) + paint.width) % paint.width;
  const int wrapX1 = (wrapX0 + 1) % paint.width;

  const auto at = [&](int x, int y) {
    return paint.weights[static_cast<std::size_t>(y) *
                             static_cast<std::size_t>(paint.width) +
                         static_cast<std::size_t>(x)];
  };
  const float top = Lerp(at(wrapX0, y0), at(wrapX1, y0), fx);
  const float bottom = Lerp(at(wrapX0, y1), at(wrapX1, y1), fx);
  return Saturate(Lerp(top, bottom, fy));
}

int PaintClumpWeight(const ScalpMesh& scalp, ClumpPaint& paint,
                     const Vec3& center, float radius, float target,
                     float amount) {
  if (paint.empty() || !scalp.valid) return 0;
  const float brushRadius = std::max(1e-4f, radius);
  const float strength = Saturate(amount);
  if (strength <= 0.0f) return 0;

  int changed = 0;
  for (int y = 0; y < paint.height; ++y) {
    for (int x = 0; x < paint.width; ++x) {
      const Vec2 uv = TexelUV(x, y, paint.width, paint.height);
      const Vec3 position = ScalpPointFromUV(scalp, uv, nullptr);
      const float distance = Distance(position, center);
      if (distance >= brushRadius) continue;
      // Smooth falloff so repeated strokes build up instead of stamping a
      // hard-edged disc.
      const float falloff = 1.0f - distance / brushRadius;
      const float weight = strength * falloff * falloff;
      float& value = paint.weights[static_cast<std::size_t>(y) *
                                       static_cast<std::size_t>(paint.width) +
                                   static_cast<std::size_t>(x)];
      const float updated = Saturate(Lerp(value, Saturate(target), weight));
      if (std::fabs(updated - value) > 1e-5f) ++changed;
      value = updated;
    }
  }
  return changed;
}

std::vector<ClumpSite> BuildClumpSites(const ScalpMesh& scalp, int regionCount,
                                       std::uint32_t seed) {
  std::vector<ClumpSite> sites;
  if (!scalp.valid) return sites;
  const int count = ClampInt(regionCount, 1, 512);
  sites.reserve(static_cast<std::size_t>(count));

  for (int i = 0; i < count; ++i) {
    const std::uint32_t siteSeed =
        HashCombine(seed, static_cast<std::uint32_t>(i));
    const float spacing = 1.0f / static_cast<float>(count);
    // Area-uniform in normalized height, golden-angle azimuth, sub-cell
    // dither: the same distribution scheme the hair roots use, so regions and
    // roots stay well matched at any count.
    float v = (static_cast<float>(i) + 0.5f) * spacing;
    v = Saturate(v + RandomSignedFloat(HashCombine(siteSeed, 3u)) * spacing *
                         0.7f);
    float u = WrapUnit(std::fmod(static_cast<float>(i) * kGoldenAngle, kTwoPi) /
                           kTwoPi +
                       RandomSignedFloat(HashCombine(siteSeed, 7u)) * 0.02f);

    ClumpSite site;
    site.uv = Vec2{u, v * 0.96f};
    site.position = ScalpPointFromUV(scalp, site.uv, &site.normal);
    sites.push_back(site);
  }
  return sites;
}

int NearestClumpSite(const std::vector<ClumpSite>& sites, const Vec3& point) {
  int best = -1;
  float bestDistance = 0.0f;
  for (std::size_t i = 0; i < sites.size(); ++i) {
    const float distance = LengthSquared(sites[i].position - point);
    if (best < 0 || distance < bestDistance) {
      best = static_cast<int>(i);
      bestDistance = distance;
    }
  }
  return best;
}

std::vector<int> AssignClumpRegions(const ClumpCurves& curves,
                                    const std::vector<ClumpSite>& sites) {
  std::vector<int> regions(curves.points.size(), -1);
  for (std::size_t i = 0; i < curves.points.size(); ++i) {
    if (curves.points[i].empty()) continue;
    regions[i] = NearestClumpSite(sites, curves.points[i].front());
  }
  return regions;
}

ClumpCenterCurves MakeClumpCenterCurves(const GuideSet& guides) {
  ClumpCenterCurves centers;
  for (const GuideCurve& curve : guides.curves) {
    if (curve.points.size() < 2) continue;
    centers.curves.push_back(curve.points);
    centers.roots.push_back(curve.points.front());
  }
  return centers;
}

void ApplyClumpToCurves(ClumpCurves& curves,
                        const std::vector<ClumpSite>& sites,
                        const ClumpPaint& paint, const ScalpMesh& scalp,
                        const ClumpState& state, std::uint32_t seed,
                        const ClumpCenterCurves* centers) {
  (void)scalp;
  const float strength = Saturate(state.strength);
  if (strength <= 0.0f || curves.points.empty() || sites.empty()) return;

  const float tipBias = std::max(0.05f, state.tipBias);
  const float radius = std::max(1e-4f, state.radius);

  const std::vector<int> regions = AssignClumpRegions(curves, sites);

  // Longest member decides the shared resolution of a region's mean curve, so
  // curve sets with mixed point counts (guides at 14, strands at 17) still
  // clump against a well-defined center.
  std::size_t resolution = 0;
  for (const std::vector<Vec3>& points : curves.points) {
    resolution = std::max(resolution, points.size());
  }
  if (resolution < 2) return;

  std::vector<std::vector<Vec3>> means(
      sites.size(), std::vector<Vec3>(resolution, Vec3{}));
  std::vector<int> counts(sites.size(), 0);
  for (std::size_t i = 0; i < curves.points.size(); ++i) {
    const int region = regions[i];
    if (region < 0 || curves.points[i].size() < 2) continue;
    const std::vector<Vec3> resampled =
        ResampleByArcLength(curves.points[i], static_cast<int>(resolution));
    std::vector<Vec3>& mean = means[static_cast<std::size_t>(region)];
    for (std::size_t j = 0; j < resolution; ++j) mean[j] += resampled[j];
    ++counts[static_cast<std::size_t>(region)];
  }

  const bool useSuppliedCenters = centers != nullptr && !centers->empty();
  for (std::size_t region = 0; region < sites.size(); ++region) {
    if (counts[region] <= 0) continue;
    std::vector<Vec3>& mean = means[region];
    if (useSuppliedCenters) {
      // The region adopts the nearest supplied clump curve. Matching on the
      // region's site rather than per curve keeps a whole cell converging on
      // one center, which is what makes it read as a clump.
      int nearest = -1;
      float nearestDistance = 0.0f;
      for (std::size_t i = 0; i < centers->roots.size(); ++i) {
        const float distance =
            LengthSquared(centers->roots[i] - sites[region].position);
        if (nearest < 0 || distance < nearestDistance) {
          nearest = static_cast<int>(i);
          nearestDistance = distance;
        }
      }
      mean = ResampleByArcLength(centers->curves[static_cast<std::size_t>(
                                     nearest)],
                                 static_cast<int>(resolution));
    } else {
      const float inverse = 1.0f / static_cast<float>(counts[region]);
      for (Vec3& point : mean) point *= inverse;
    }

    // The gizmo attracts nearby regions. Its falloff over `radius` is what
    // makes dragging it a legible, local styling action.
    const float attract =
        Saturate(1.0f - Distance(sites[region].position, state.center) /
                            radius);
    const Vec3 jitter =
        RandomSignedVec3(HashCombine(seed, static_cast<std::uint32_t>(region))) *
        (state.noise * radius * 0.35f);
    if (attract <= 0.0f && LengthSquared(jitter) <= 0.0f) continue;

    const Vec3 root = mean.front();
    const Vec3 target = state.center + jitter;
    const float last = static_cast<float>(resolution - 1);
    for (std::size_t j = 1; j < resolution; ++j) {
      const float t = static_cast<float>(j) / last;
      mean[j] = Lerp(mean[j], Lerp(root, target, t), attract * t);
    }
  }

  for (std::size_t i = 0; i < curves.points.size(); ++i) {
    std::vector<Vec3>& points = curves.points[i];
    const int region = regions[i];
    if (region < 0 || points.size() < 2 ||
        counts[static_cast<std::size_t>(region)] <= 0) {
      continue;
    }
    const Vec2 rootUV = i < curves.rootUV.size() ? curves.rootUV[i] : Vec2{};
    const float painted = SampleClumpWeight(paint, rootUV, 1.0f);
    if (painted <= 0.0f) continue;

    const std::vector<Vec3>& mean = means[static_cast<std::size_t>(region)];
    const float last = static_cast<float>(points.size() - 1);
    for (std::size_t j = 1; j < points.size(); ++j) {
      const float t = static_cast<float>(j) / last;
      // Sample the region's mean curve at the same normalized position, so
      // curves of differing lengths still converge sensibly.
      const float meanPosition = t * static_cast<float>(resolution - 1);
      const std::size_t low = static_cast<std::size_t>(meanPosition);
      const std::size_t high = std::min(low + 1, resolution - 1);
      const Vec3 center =
          Lerp(mean[low], mean[high], meanPosition - static_cast<float>(low));

      const float influence =
          Saturate(strength * std::pow(t, tipBias) * painted);
      points[j] = Lerp(points[j], center, influence);
    }
  }
}

GuideSet ApplyClumpToGuides(const GuideSet& input,
                            const std::vector<ClumpSite>& sites,
                            const ClumpPaint& paint, const ScalpMesh& scalp,
                            const ClumpState& state, std::uint32_t seed,
                            const ClumpCenterCurves* centers) {
  GuideSet result;
  if (!input.valid) return result;
  result = input;

  ClumpCurves curves;
  curves.points.reserve(result.curves.size());
  curves.rootUV.reserve(result.curves.size());
  for (const GuideCurve& curve : result.curves) {
    curves.points.push_back(curve.points);
    curves.rootUV.push_back(curve.rootUV);
  }
  ApplyClumpToCurves(curves, sites, paint, scalp, state, seed, centers);
  for (std::size_t i = 0; i < result.curves.size(); ++i) {
    result.curves[i].points = std::move(curves.points[i]);
  }
  return result;
}

StrandSet ApplyClumpToHair(const StrandSet& input,
                           const std::vector<ClumpSite>& sites,
                           const ClumpPaint& paint, const ScalpMesh& scalp,
                           const ClumpState& state, std::uint32_t seed,
                           const ClumpCenterCurves* centers) {
  StrandSet result;
  if (!input.valid) return result;
  result = input;

  ClumpCurves curves;
  curves.points.reserve(result.strands.size());
  curves.rootUV.reserve(result.strands.size());
  for (const Strand& strand : result.strands) {
    curves.points.push_back(strand.points);
    curves.rootUV.push_back(strand.rootUV);
  }
  ApplyClumpToCurves(curves, sites, paint, scalp, state, seed, centers);
  for (std::size_t i = 0; i < result.strands.size(); ++i) {
    result.strands[i].points = std::move(curves.points[i]);
  }
  return result;
}

Vec3 PastelRegionColor(int region) {
  if (region < 0) return Vec3{0.5f, 0.5f, 0.5f};
  // Golden-ratio hue stepping keeps consecutive indices far apart on the wheel,
  // and a small seeded wobble in saturation/value stops same-hue-family cells
  // from reading as one region.
  constexpr float kGoldenRatioConjugate = 0.61803398874989484f;
  const float hue =
      std::fmod(static_cast<float>(region) * kGoldenRatioConjugate, 1.0f);
  const std::uint32_t seed = HashUInt32(static_cast<std::uint32_t>(region) + 1u);
  const float saturation = 0.30f + RandomUnitFloat(seed) * 0.16f;
  const float value = 0.84f + RandomUnitFloat(seed ^ 0x9e3779b9u) * 0.12f;

  // HSV to RGB. Low saturation and high value is what makes it a pastel.
  const float h = hue * 6.0f;
  const float c = value * saturation;
  const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
  const float m = value - c;
  Vec3 rgb;
  if (h < 1.0f) rgb = Vec3{c, x, 0.0f};
  else if (h < 2.0f) rgb = Vec3{x, c, 0.0f};
  else if (h < 3.0f) rgb = Vec3{0.0f, c, x};
  else if (h < 4.0f) rgb = Vec3{0.0f, x, c};
  else if (h < 5.0f) rgb = Vec3{x, 0.0f, c};
  else rgb = Vec3{c, 0.0f, x};
  return Vec3{rgb.x + m, rgb.y + m, rgb.z + m};
}

ClumpMapImage BuildClumpMapImage(const ScalpMesh& scalp,
                                 const std::vector<ClumpSite>& sites,
                                 const ClumpPaint& paint) {
  ClumpMapImage image;
  if (!scalp.valid) return image;
  image.width = paint.empty() ? kClumpPaintWidth : paint.width;
  image.height = paint.empty() ? kClumpPaintHeight : paint.height;
  image.pixels.assign(static_cast<std::size_t>(image.width) *
                          static_cast<std::size_t>(image.height) * 4u,
                      0u);

  std::vector<int> regionOf(static_cast<std::size_t>(image.width) *
                            static_cast<std::size_t>(image.height));
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      const Vec2 uv = TexelUV(x, y, image.width, image.height);
      const Vec3 position = ScalpPointFromUV(scalp, uv, nullptr);
      regionOf[static_cast<std::size_t>(y) *
                   static_cast<std::size_t>(image.width) +
               static_cast<std::size_t>(x)] =
          NearestClumpSite(sites, position);
    }
  }

  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      const std::size_t index = static_cast<std::size_t>(y) *
                                    static_cast<std::size_t>(image.width) +
                                static_cast<std::size_t>(x);
      const int region = regionOf[index];

      // A texel is on a boundary when any 4-neighbour belongs to another
      // region. u wraps; v clamps.
      bool boundary = false;
      const int neighbours[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1},
                                    {x, y + 1}};
      for (const auto& neighbour : neighbours) {
        const int nx = ((neighbour[0] % image.width) + image.width) %
                       image.width;
        const int ny = ClampInt(neighbour[1], 0, image.height - 1);
        if (regionOf[static_cast<std::size_t>(ny) *
                         static_cast<std::size_t>(image.width) +
                     static_cast<std::size_t>(nx)] != region) {
          boundary = true;
          break;
        }
      }

      const Vec2 uv = TexelUV(x, y, image.width, image.height);
      const float weight = SampleClumpWeight(paint, uv, 1.0f);
      const Vec3 colour = boundary || region < 0
                              ? Vec3{kClumpBoundaryInk, kClumpBoundaryInk,
                                     kClumpBoundaryInk + 0.02f}
                              : PastelRegionColor(region);

      std::uint8_t* pixel = &image.pixels[index * 4u];
      pixel[0] = static_cast<std::uint8_t>(Saturate(colour.x) * 255.0f + 0.5f);
      pixel[1] = static_cast<std::uint8_t>(Saturate(colour.y) * 255.0f + 0.5f);
      pixel[2] = static_cast<std::uint8_t>(Saturate(colour.z) * 255.0f + 0.5f);
      pixel[3] = static_cast<std::uint8_t>(Saturate(weight) * 255.0f + 0.5f);
    }
  }
  return image;
}

}  // namespace noodles::demo::hair
