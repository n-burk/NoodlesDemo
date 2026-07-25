// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT
#pragma once

// CPU vertex-stream construction for the 3D viewport. No GL types appear here,
// so strand ribbons, gizmos, and handles are all testable without a context.
//
// The split is deliberate: solid geometry (scalp + hair ribbons) is expensive
// and is keyed to the evaluation generation, while overlay geometry (grid,
// guides, points, handles, brush, clump gizmo, stroke preview) is small enough
// to rebuild every frame. Camera motion touches neither, because strand
// ribbons are turned to face the viewer in the vertex shader rather than on
// the CPU.

#include "HairScene.h"
#include "HairTypes.h"

#include <cstdint>
#include <vector>

namespace noodles::demo::hair {

// Floats per vertex in each stream.
inline constexpr int kRibbonVertexFloats = 10;  // pos3 tangent3 side1 t1 shade1 width1
inline constexpr int kMeshVertexFloats = 8;     // pos3 normal3 uv2
inline constexpr int kLineVertexFloats = 6;     // pos3 color3
inline constexpr int kPointVertexFloats = 7;    // pos3 color3 size1

struct HairSolidGeometry {
  std::vector<float> ribbonVertices;
  std::vector<std::uint32_t> ribbonIndices;
  std::vector<float> meshVertices;
  std::vector<std::uint32_t> meshIndices;
  std::vector<std::uint32_t> meshWireIndices;

  std::size_t strandCount = 0;
  std::size_t pointsPerStrand = 0;

  bool empty() const { return ribbonIndices.empty() && meshIndices.empty(); }
};

struct HairOverlayGeometry {
  std::vector<float> lineVertices;   // GL_LINES pairs
  std::vector<float> pointVertices;  // GL_POINTS

  std::size_t lineCount() const {
    return lineVertices.size() / (kLineVertexFloats * 2);
  }
  std::size_t pointCount() const {
    return pointVertices.size() / kPointVertexFloats;
  }
};

// The exact expansion the ribbon vertex shader performs, mirrored on the CPU
// so tests can assert that a ribbon really does face the camera.
Vec3 ExpandRibbonVertex(const Vec3& position, const Vec3& tangent,
                        const Vec3& cameraPosition, float halfWidth,
                        float side);

// Scalp triangles plus one camera-facing ribbon per strand. `strandHalfWidth`
// is the clamped width from the Generate Hair node.
HairSolidGeometry BuildSolidGeometry(const HairEvalResult& eval,
                                     float strandHalfWidth);

// Grid, guide curves, guide/root points, the selected point's X/Y/Z handles,
// the comb brush ring, the clump gizmo, the live stroke preview, and hover
// feedback. Which of these appear is decided by the display switches on the
// nodes that are actually in the evaluated chain.
HairOverlayGeometry BuildOverlayGeometry(const HairViewState& state);

}  // namespace noodles::demo::hair
