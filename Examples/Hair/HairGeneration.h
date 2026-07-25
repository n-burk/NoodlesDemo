// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT
#pragma once

// Deterministic strand synthesis. Given a scalp and a guide set, the same
// parameters always produce byte-identical strands on every platform, which is
// what lets the demo's generation tests assert exact structure.

#include "HairTypes.h"

#include <cstdint>

namespace noodles::demo::hair {

// Ranges the demo renders within. The graph's scalar rows are unclamped by
// design (GraphEditor deliberately does not clamp a scrub), so generation
// clamps on read and these bounds are the contract the renderer relies on.
inline constexpr int kMinStrandCount = 500;
inline constexpr int kMaxStrandCount = 1500;
inline constexpr int kMinStrandSegments = 12;
inline constexpr int kMaxStrandSegments = 24;

struct HairGenerationParameters {
  int density = 900;      // strand count, clamped to [500, 1500]
  int segments = 16;      // segments per strand, clamped to [12, 24]
  float width = 0.0055f;  // ribbon half-width at the root, in world units
  float spread = 0.35f;   // root scatter and guide blending radius
  float variation = 0.28f;
  std::uint32_t seed = 1337;
};

// Clamped copies of the raw graph values, exposed so tests and the renderer
// agree on exactly what was used.
HairGenerationParameters ClampGenerationParameters(
    const HairGenerationParameters& raw);

// Deterministically distribute `count` roots over the scalp dome. Roots are
// area-uniform over the spherical cap and jittered by `spread`, so raising the
// density refines the same distribution instead of reshuffling it.
void DistributeRoots(const ScalpMesh& scalp, int count, float spread,
                     std::uint32_t seed, std::vector<Vec3>* outPositions,
                     std::vector<Vec3>* outNormals,
                     std::vector<Vec2>* outUVs = nullptr);

// Build the rendered hair. Returns an invalid, empty set when either input is
// missing or empty; it never falls back to previously generated strands.
//
// Each strand's root is placed on the scalp, the nearest guides are blended in
// the roots' local surface frames so hair follows the dome, and seeded
// variation is applied with a t-weighted profile so strand[0] is exactly the
// root position.
StrandSet GenerateHair(const ScalpMesh& scalp, const GuideSet& guides,
                       const HairGenerationParameters& parameters);

}  // namespace noodles::demo::hair
