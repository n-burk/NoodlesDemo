// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT
#pragma once

// XGen-style clumping: Voronoi regions over the scalp decide which curves clump
// together, and a paintable weight map in scalp UV space decides how strongly
// each root clumps at all.
//
// The clump operates on any set of rooted curves, so the same code clumps guide
// curves before generation and generated strands after it.

#include "HairGeometry.h"
#include "HairTypes.h"

#include <cstdint>
#include <vector>

namespace noodles::demo::hair {

// One Voronoi site: a clump region's seed on the scalp.
struct ClumpSite {
  Vec2 uv;
  Vec3 position;
  Vec3 normal;
};

// The paintable weight texture, in scalp UV space (u = azimuth and wraps,
// v = polar from the crown). Kept separate from the sites because painting must
// survive a change to the region count or the scalp shape.
struct ClumpPaint {
  int width = 0;
  int height = 0;
  std::vector<float> weights;  // row-major, [0, 1]

  bool empty() const {
    return width <= 0 || height <= 0 ||
           weights.size() != static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height);
  }
};

inline constexpr int kClumpPaintWidth = 192;
inline constexpr int kClumpPaintHeight = 96;

// A fully painted map: every root clumps at full strength until the user erases
// some of it. Starting empty would make the Clump node look broken.
ClumpPaint MakeClumpPaint(float initialWeight = 1.0f);

// Bilinear sample with u wrapping. Returns `fallback` for an empty map, so an
// unpainted graph still clumps.
float SampleClumpWeight(const ClumpPaint& paint, const Vec2& uv,
                        float fallback = 1.0f);

// Paint a 3D-round brush: texels whose scalp position lies within `radius` of
// `center` move toward `target` by `amount` × falloff. Painting in world space
// rather than UV space keeps the brush circular near the pole, where UV space
// is badly distorted. Returns the number of texels changed.
int PaintClumpWeight(const ScalpMesh& scalp, ClumpPaint& paint,
                     const Vec3& center, float radius, float target,
                     float amount);

// Deterministic Voronoi sites spread over the dome.
std::vector<ClumpSite> BuildClumpSites(const ScalpMesh& scalp, int regionCount,
                                       std::uint32_t seed);

// Index of the nearest site to a world point, or -1 when there are none.
// Distance is measured in world space, so the partition is a true Voronoi
// diagram on the surface rather than one distorted by the UV parameterization.
int NearestClumpSite(const std::vector<ClumpSite>& sites, const Vec3& point);

// A curve set reduced to what clumping needs: point lists plus each root's
// scalp UV. Guides and generated strands both project onto this.
struct ClumpCurves {
  std::vector<std::vector<Vec3>> points;
  std::vector<Vec2> rootUV;
};

// Explicit clump centers, supplied through the Clump node's `clumps` input.
// When present, a region converges onto the nearest supplied curve instead of
// onto the mean of its own members — the difference between "these strands
// gather among themselves" and "these strands gather onto that curve".
struct ClumpCenterCurves {
  std::vector<std::vector<Vec3>> curves;
  std::vector<Vec3> roots;  // matched against each region's site

  bool empty() const { return curves.empty(); }
};

// Apply clumping in place.
//
// Curves converge onto their own region's mean curve, with influence rising
// toward the tips by `tipBias`, scaled by the painted weight at each root, and
// jittered per region by `noise`. The clump gizmo additionally attracts each
// region's mean curve, falling off with the region site's distance from the
// gizmo over `radius` — which is what keeps dragging it a visible, global
// styling action rather than a no-op. Roots never move.
void ApplyClumpToCurves(ClumpCurves& curves,
                        const std::vector<ClumpSite>& sites,
                        const ClumpPaint& paint, const ScalpMesh& scalp,
                        const ClumpState& state, std::uint32_t seed,
                        const ClumpCenterCurves* centers = nullptr);

// Region index per curve, for diagnostics and tests.
std::vector<int> AssignClumpRegions(const ClumpCurves& curves,
                                    const std::vector<ClumpSite>& sites);

// The two curve kinds the Clump node accepts. Both project onto ClumpCurves
// and back, which is what lets one modifier sit either before Generate Hair
// (clumping guides) or after it (clumping the generated curves).
GuideSet ApplyClumpToGuides(const GuideSet& input,
                            const std::vector<ClumpSite>& sites,
                            const ClumpPaint& paint, const ScalpMesh& scalp,
                            const ClumpState& state, std::uint32_t seed,
                            const ClumpCenterCurves* centers = nullptr);

StrandSet ApplyClumpToHair(const StrandSet& input,
                           const std::vector<ClumpSite>& sites,
                           const ClumpPaint& paint, const ScalpMesh& scalp,
                           const ClumpState& state, std::uint32_t seed,
                           const ClumpCenterCurves* centers = nullptr);

// Project a guide set onto the center-curve form.
ClumpCenterCurves MakeClumpCenterCurves(const GuideSet& guides);

// ── display ─────────────────────────────────────────────────────────────────

// A distinct pastel per Voronoi region. Hues are spaced by the golden ratio so
// that consecutive region indices — which Voronoi construction tends to place
// near each other — never land on neighbouring hues.
Vec3 PastelRegionColor(int region);

// The ink colour baked into cell-boundary texels. A fixed value rather than a
// darkened tint of each region, so boundaries are exactly identifiable in the
// built image.
inline constexpr float kClumpBoundaryInk = 0.07f;

// An RGBA8 texture of the clump map for the scalp shader:
//   RGB = the region's pastel colour, replaced by the boundary ink on a cell
//         edge
//   A   = painted clump weight
// Built on the CPU so the renderer stays a thin uploader and the contents are
// directly testable.
struct ClumpMapImage {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> pixels;  // RGBA8, row-major

  bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
};

ClumpMapImage BuildClumpMapImage(const ScalpMesh& scalp,
                                 const std::vector<ClumpSite>& sites,
                                 const ClumpPaint& paint);

}  // namespace noodles::demo::hair
