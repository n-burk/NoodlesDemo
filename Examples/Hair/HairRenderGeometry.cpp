// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT

#include "HairRenderGeometry.h"

#include <cmath>

namespace noodles::demo::hair {
namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

const Vec3 kGridColor{0.20f, 0.23f, 0.30f};
const Vec3 kGridAxisColor{0.34f, 0.38f, 0.48f};
const Vec3 kGuideColor{0.36f, 0.62f, 0.92f};
const Vec3 kGuideTipColor{0.62f, 0.86f, 1.00f};
const Vec3 kGuidePointColor{0.55f, 0.72f, 0.95f};
const Vec3 kRootColor{1.00f, 0.72f, 0.24f};
const Vec3 kSelectedColor{1.00f, 0.95f, 0.35f};
const Vec3 kHoverColor{0.55f, 1.00f, 0.70f};
const Vec3 kBrushColor{0.45f, 1.00f, 0.72f};
const Vec3 kBrushActiveColor{1.00f, 0.55f, 0.35f};
const Vec3 kClumpColor{0.95f, 0.42f, 0.85f};
const Vec3 kClumpHoverColor{1.00f, 0.72f, 0.95f};
const Vec3 kStrokeColor{1.00f, 0.85f, 0.30f};
const Vec3 kPaintColor{0.35f, 0.95f, 1.00f};
const Vec3 kPaintEraseColor{1.00f, 0.45f, 0.42f};
const Vec3 kClumpSiteColor{0.85f, 0.35f, 0.95f};
const Vec3 kAxisColorX{0.95f, 0.32f, 0.32f};
const Vec3 kAxisColorY{0.38f, 0.92f, 0.40f};
const Vec3 kAxisColorZ{0.36f, 0.55f, 0.98f};

void PushLineVertex(std::vector<float>& out, const Vec3& position,
                    const Vec3& color) {
  out.push_back(position.x);
  out.push_back(position.y);
  out.push_back(position.z);
  out.push_back(color.x);
  out.push_back(color.y);
  out.push_back(color.z);
}

void PushLine(std::vector<float>& out, const Vec3& from, const Vec3& to,
              const Vec3& fromColor, const Vec3& toColor) {
  PushLineVertex(out, from, fromColor);
  PushLineVertex(out, to, toColor);
}

void PushPoint(std::vector<float>& out, const Vec3& position, const Vec3& color,
               float size) {
  out.push_back(position.x);
  out.push_back(position.y);
  out.push_back(position.z);
  out.push_back(color.x);
  out.push_back(color.y);
  out.push_back(color.z);
  out.push_back(size);
}

// A wire circle in the plane spanned by `axisA`/`axisB`.
void PushCircle(std::vector<float>& out, const Vec3& center, const Vec3& axisA,
                const Vec3& axisB, float radius, const Vec3& color,
                int segments) {
  for (int i = 0; i < segments; ++i) {
    const float a0 = kTwoPi * static_cast<float>(i) /
                     static_cast<float>(segments);
    const float a1 = kTwoPi * static_cast<float>(i + 1) /
                     static_cast<float>(segments);
    const Vec3 p0 = center + axisA * (std::cos(a0) * radius) +
                    axisB * (std::sin(a0) * radius);
    const Vec3 p1 = center + axisA * (std::cos(a1) * radius) +
                    axisB * (std::sin(a1) * radius);
    PushLine(out, p0, p1, color, color);
  }
}

// A ring that hugs the scalp, so a paint brush reads as being on the surface
// rather than floating in front of it.
void PushScalpCircle(std::vector<float>& out, const ScalpMesh& scalp,
                     const Vec3& center, const Vec3& normal, float radius,
                     const Vec3& color, int segments) {
  Vec3 east;
  Vec3 north;
  ScalpFrame(normal, &east, &north);
  Vec3 previous;
  for (int i = 0; i <= segments; ++i) {
    const float angle = kTwoPi * static_cast<float>(i) /
                        static_cast<float>(segments);
    const Vec3 offset =
        east * (std::cos(angle) * radius) + north * (std::sin(angle) * radius);
    // Lift slightly off the surface so the ring is not z-fighting the scalp.
    const Vec3 onSurface =
        SnapToScalp(scalp, center + offset, nullptr, nullptr) + normal * 0.004f;
    if (i > 0) PushLine(out, previous, onSurface, color, color);
    previous = onSurface;
  }
}

Vec3 AxisColor(HairDragAxis axis) {
  switch (axis) {
    case HairDragAxis::AxisX:
      return kAxisColorX;
    case HairDragAxis::AxisY:
      return kAxisColorY;
    case HairDragAxis::AxisZ:
      return kAxisColorZ;
    case HairDragAxis::CameraPlane:
      break;
  }
  return kSelectedColor;
}

// Tangent at point `index` of a polyline: central difference in the interior,
// one-sided at the ends, so a ribbon never collapses at the root or tip.
Vec3 PolylineTangent(const std::vector<Vec3>& points, std::size_t index) {
  if (points.size() < 2) return Vec3{0.0f, 1.0f, 0.0f};
  Vec3 tangent;
  if (index == 0) {
    tangent = points[1] - points[0];
  } else if (index + 1 >= points.size()) {
    tangent = points[index] - points[index - 1];
  } else {
    tangent = points[index + 1] - points[index - 1];
  }
  const Vec3 unit = Normalized(tangent);
  return LengthSquared(unit) > 0.0f ? unit : Vec3{0.0f, 1.0f, 0.0f};
}

void PushGuideCurve(std::vector<float>& out, const GuideCurve& curve,
                    const Vec3& rootColor, const Vec3& tipColor) {
  if (curve.points.size() < 2) return;
  const float last = static_cast<float>(curve.points.size() - 1);
  for (std::size_t i = 1; i < curve.points.size(); ++i) {
    const float t0 = static_cast<float>(i - 1) / last;
    const float t1 = static_cast<float>(i) / last;
    PushLine(out, curve.points[i - 1], curve.points[i],
             Lerp(rootColor, tipColor, t0), Lerp(rootColor, tipColor, t1));
  }
}

}  // namespace

Vec3 ExpandRibbonVertex(const Vec3& position, const Vec3& tangent,
                        const Vec3& cameraPosition, float halfWidth,
                        float side) {
  const Vec3 view = Normalized(cameraPosition - position);
  Vec3 lateral = Cross(tangent, view);
  if (LengthSquared(lateral) <= 1e-12f) {
    // Strand seen exactly end-on: any perpendicular keeps the quad from
    // degenerating for that one frame.
    lateral = AnyPerpendicular(tangent);
  }
  return position + Normalized(lateral) * (halfWidth * side);
}

HairSolidGeometry BuildSolidGeometry(const HairEvalResult& eval,
                                     float strandHalfWidth) {
  HairSolidGeometry geometry;

  if (eval.scalp.valid && eval.display.showScalp) {
    geometry.meshVertices.reserve(eval.scalp.vertices.size() *
                                  kMeshVertexFloats);
    for (const ScalpVertex& vertex : eval.scalp.vertices) {
      geometry.meshVertices.push_back(vertex.position.x);
      geometry.meshVertices.push_back(vertex.position.y);
      geometry.meshVertices.push_back(vertex.position.z);
      geometry.meshVertices.push_back(vertex.normal.x);
      geometry.meshVertices.push_back(vertex.normal.y);
      geometry.meshVertices.push_back(vertex.normal.z);
      // The scalp's UVs are what let the fragment shader sample the paintable
      // clump-weight map.
      geometry.meshVertices.push_back(vertex.uv.x);
      geometry.meshVertices.push_back(vertex.uv.y);
    }
    geometry.meshIndices = eval.scalp.indices;
    if (eval.display.scalpWireframe) {
      // Two of each triangle's three edges are enough to draw the whole mesh
      // once: the third is the neighbouring triangle's first edge.
      geometry.meshWireIndices.reserve(eval.scalp.indices.size() * 2);
      for (std::size_t i = 0; i + 2 < eval.scalp.indices.size(); i += 3) {
        geometry.meshWireIndices.push_back(eval.scalp.indices[i]);
        geometry.meshWireIndices.push_back(eval.scalp.indices[i + 1]);
        geometry.meshWireIndices.push_back(eval.scalp.indices[i + 1]);
        geometry.meshWireIndices.push_back(eval.scalp.indices[i + 2]);
      }
    }
  }

  if (!eval.hair.valid || !eval.display.showHair) return geometry;

  const float halfWidth = std::max(1e-5f, strandHalfWidth);
  geometry.strandCount = eval.hair.strands.size();
  geometry.pointsPerStrand =
      eval.hair.strands.empty() ? 0 : eval.hair.strands.front().points.size();

  geometry.ribbonVertices.reserve(eval.hair.pointCount() * 2 *
                                  kRibbonVertexFloats);
  geometry.ribbonIndices.reserve(eval.hair.pointCount() * 6);

  std::uint32_t base = 0;
  for (const Strand& strand : eval.hair.strands) {
    const std::size_t count = strand.points.size();
    if (count < 2) continue;
    const float last = static_cast<float>(count - 1);
    for (std::size_t i = 0; i < count; ++i) {
      const float t = static_cast<float>(i) / last;
      const Vec3 tangent = PolylineTangent(strand.points, i);
      // Taper toward the tip; a constant-width ribbon reads as a ribbon, a
      // tapered one reads as hair.
      const float width =
          halfWidth * strand.widthScale * (1.0f - 0.72f * t * t);
      for (int side = 0; side < 2; ++side) {
        const float sideSign = side == 0 ? -1.0f : 1.0f;
        geometry.ribbonVertices.push_back(strand.points[i].x);
        geometry.ribbonVertices.push_back(strand.points[i].y);
        geometry.ribbonVertices.push_back(strand.points[i].z);
        geometry.ribbonVertices.push_back(tangent.x);
        geometry.ribbonVertices.push_back(tangent.y);
        geometry.ribbonVertices.push_back(tangent.z);
        geometry.ribbonVertices.push_back(sideSign);
        geometry.ribbonVertices.push_back(t);
        geometry.ribbonVertices.push_back(strand.shade);
        geometry.ribbonVertices.push_back(width);
      }
    }
    for (std::size_t i = 0; i + 1 < count; ++i) {
      const std::uint32_t v0 = base + static_cast<std::uint32_t>(i * 2);
      const std::uint32_t v1 = v0 + 1;
      const std::uint32_t v2 = v0 + 2;
      const std::uint32_t v3 = v0 + 3;
      geometry.ribbonIndices.push_back(v0);
      geometry.ribbonIndices.push_back(v2);
      geometry.ribbonIndices.push_back(v1);
      geometry.ribbonIndices.push_back(v1);
      geometry.ribbonIndices.push_back(v2);
      geometry.ribbonIndices.push_back(v3);
    }
    base += static_cast<std::uint32_t>(count * 2);
  }
  return geometry;
}

HairOverlayGeometry BuildOverlayGeometry(const HairViewState& state) {
  HairOverlayGeometry geometry;
  if (!state.eval) return geometry;
  const HairEvalResult& eval = *state.eval;

  // ── ground grid ────────────────────────────────────────────────────────────
  if (eval.display.showGrid) {
    constexpr int kGridLines = 12;
    constexpr float kGridExtent = 3.0f;
    const float step = (kGridExtent * 2.0f) / static_cast<float>(kGridLines);
    for (int i = 0; i <= kGridLines; ++i) {
      const float offset = -kGridExtent + step * static_cast<float>(i);
      const bool axis = std::fabs(offset) < 1e-4f;
      const Vec3 color = axis ? kGridAxisColor : kGridColor;
      PushLine(geometry.lineVertices, Vec3{offset, 0.0f, -kGridExtent},
               Vec3{offset, 0.0f, kGridExtent}, color, color);
      PushLine(geometry.lineVertices, Vec3{-kGridExtent, 0.0f, offset},
               Vec3{kGridExtent, 0.0f, offset}, color, color);
    }
  }

  // ── scalp wireframe is part of the solid pass; guides live here ────────────
  const bool editing = state.tool == HairToolKind::EditPoints ||
                       state.tool == HairToolKind::DrawGuides;
  // While a guide-editing tool is armed, the editable set (the Create Guides
  // output) is what the user picks and drags, so that is what gets drawn with
  // handles. Otherwise the final post-clump guides are shown.
  const GuideSet& displayGuides =
      editing && eval.guidesAtCreate.valid ? eval.guidesAtCreate : eval.guides;

  if (eval.display.showGuides && displayGuides.valid) {
    for (const GuideCurve& curve : displayGuides.curves) {
      PushGuideCurve(geometry.lineVertices, curve, kGuideColor, kGuideTipColor);
    }
  }

  if (eval.display.showGuideGizmos && displayGuides.valid) {
    for (std::size_t curveIndex = 0; curveIndex < displayGuides.curves.size();
         ++curveIndex) {
      const std::vector<Vec3>& points =
          displayGuides.curves[curveIndex].points;
      for (std::size_t pointIndex = 0; pointIndex < points.size();
           ++pointIndex) {
        const bool isRoot = pointIndex == 0;
        const bool isSelected =
            state.selectedPoint.curveIndex == static_cast<int>(curveIndex) &&
            state.selectedPoint.pointIndex == static_cast<int>(pointIndex);
        const bool isHovered =
            state.hoveredPoint.curveIndex == static_cast<int>(curveIndex) &&
            state.hoveredPoint.pointIndex == static_cast<int>(pointIndex);
        Vec3 color = isRoot ? kRootColor : kGuidePointColor;
        float size = isRoot ? 7.0f : 5.0f;
        if (isHovered) {
          color = kHoverColor;
          size = 9.0f;
        }
        if (isSelected) {
          color = kSelectedColor;
          size = 11.0f;
        }
        PushPoint(geometry.pointVertices, points[pointIndex], color, size);
      }
    }
  }

  // ── X/Y/Z handles on the selected point ────────────────────────────────────
  if (state.tool == HairToolKind::EditPoints &&
      eval.display.showGuideGizmos && state.selectedPoint.valid()) {
    const std::size_t curveIndex =
        static_cast<std::size_t>(state.selectedPoint.curveIndex);
    if (curveIndex < displayGuides.curves.size()) {
      const std::vector<Vec3>& points = displayGuides.curves[curveIndex].points;
      const std::size_t pointIndex =
          static_cast<std::size_t>(state.selectedPoint.pointIndex);
      if (pointIndex < points.size()) {
        const Vec3 origin = points[pointIndex];
        const HairDragAxis axes[3] = {HairDragAxis::AxisX, HairDragAxis::AxisY,
                                      HairDragAxis::AxisZ};
        for (const HairDragAxis axis : axes) {
          const Vec3 direction = AxisDirection(axis);
          const Vec3 tip = origin + direction * kAxisHandleLength;
          const bool active = state.activeAxis == axis;
          const Vec3 color = active ? kSelectedColor : AxisColor(axis);
          PushLine(geometry.lineVertices, origin, tip, color, color);
          PushPoint(geometry.pointVertices, tip, color, active ? 11.0f : 8.0f);
        }
      }
    }
  }

  // ── comb brush ─────────────────────────────────────────────────────────────
  if (state.brushVisible) {
    const Vec3 color = state.brushStroking ? kBrushActiveColor : kBrushColor;
    const Vec3 right = CameraRight(state.camera);
    const Vec3 up = CameraUp(state.camera);
    PushCircle(geometry.lineVertices, state.brushCenter, right, up,
               state.brushRadius, color, 48);
    PushPoint(geometry.pointVertices, state.brushCenter, color, 7.0f);
  }

  // ── clump gizmo ────────────────────────────────────────────────────────────
  if (state.clumpGizmoVisible) {
    const Vec3 color = state.clumpGizmoHovered ? kClumpHoverColor : kClumpColor;
    const Vec3 center = state.clumpCenter;
    const float ball = 0.06f;
    PushCircle(geometry.lineVertices, center, Vec3{1, 0, 0}, Vec3{0, 1, 0},
               ball, color, 24);
    PushCircle(geometry.lineVertices, center, Vec3{0, 1, 0}, Vec3{0, 0, 1},
               ball, color, 24);
    PushCircle(geometry.lineVertices, center, Vec3{1, 0, 0}, Vec3{0, 0, 1},
               ball, color, 24);
    // The influence radius, drawn facing the viewer so it reads as a falloff
    // rather than a solid.
    PushCircle(geometry.lineVertices, center, CameraRight(state.camera),
               CameraUp(state.camera), eval.clump.radius,
               Lerp(color, Vec3{0.10f, 0.10f, 0.14f}, 0.55f), 56);
    PushPoint(geometry.pointVertices, center, color, 10.0f);

    if (state.tool == HairToolKind::EditClump) {
      const HairDragAxis axes[3] = {HairDragAxis::AxisX, HairDragAxis::AxisY,
                                    HairDragAxis::AxisZ};
      for (const HairDragAxis axis : axes) {
        const Vec3 direction = AxisDirection(axis);
        const Vec3 tip = center + direction * (kAxisHandleLength * 1.6f);
        const Vec3 axisColor =
            state.activeAxis == axis ? kSelectedColor : AxisColor(axis);
        PushLine(geometry.lineVertices, center, tip, axisColor, axisColor);
        PushPoint(geometry.pointVertices, tip, axisColor, 9.0f);
      }
    }
  }

  // ── clump regions and the paint brush ─────────────────────────────────────
  // The map is a texture on the scalp, so hiding the scalp hides its regions
  // too — markers floating where a surface used to be would read as a bug.
  if (eval.display.showClumpMap && eval.display.showScalp &&
      !eval.clumpSites.empty()) {
    // The region layout itself is drawn on the scalp by the shader; these
    // markers show where each Voronoi site sits.
    for (const ClumpSite& site : eval.clumpSites) {
      PushPoint(geometry.pointVertices, site.position + site.normal * 0.01f,
                kClumpSiteColor, 6.0f);
    }
  }

  // Curves supplied through the Clump node's `clumps` input, drawn distinctly
  // from the groom's own guides so it is obvious what the regions gather onto.
  if (eval.display.showClumpGizmo && eval.clumpCurves.valid) {
    for (const GuideCurve& curve : eval.clumpCurves.curves) {
      PushGuideCurve(geometry.lineVertices, curve, kClumpColor,
                     kClumpHoverColor);
      if (!curve.points.empty()) {
        PushPoint(geometry.pointVertices, curve.points.back(), kClumpHoverColor,
                  8.0f);
      }
    }
  }

  if (state.paintBrushVisible && eval.scalp.valid) {
    const Vec3 color = state.paintErase ? kPaintEraseColor : kPaintColor;
    PushScalpCircle(geometry.lineVertices, eval.scalp, state.paintBrushCenter,
                    state.paintBrushNormal, state.paintBrushRadius, color, 56);
    if (state.paintBrushStroking) {
      PushScalpCircle(geometry.lineVertices, eval.scalp,
                      state.paintBrushCenter, state.paintBrushNormal,
                      state.paintBrushRadius * 0.5f, color, 40);
    }
    PushPoint(geometry.pointVertices,
              state.paintBrushCenter + state.paintBrushNormal * 0.01f, color,
              8.0f);
  }

  // ── live stroke preview ────────────────────────────────────────────────────
  if (state.strokePreview.points.size() >= 2) {
    PushGuideCurve(geometry.lineVertices, state.strokePreview, kStrokeColor,
                   kSelectedColor);
    PushPoint(geometry.pointVertices, state.strokePreview.points.front(),
              kRootColor, 9.0f);
  }

  return geometry;
}

}  // namespace noodles::demo::hair
