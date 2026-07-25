// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT
#pragma once

// The groom controller: it owns the typed guide/sculpt stores, the camera, the
// evaluation cache, and the four node-owned tools. Everything is platform
// neutral and expressed in view points, so the macOS and iPadOS shells share
// this logic entirely and only translate their native events into it.

#include "HairEvaluator.h"
#include "HairTools.h"

#include <noodles/demo/GraphDocument.h>

#include <memory>
#include <string>

namespace noodles::demo::hair {

// Who owns the current pointer gesture. Chosen once at pointer-down and never
// changed until pointer-up.
enum class HairGestureOwner {
  None = 0,
  Tool,
  CameraOrbit,
  CameraTruck,
};

// Everything the renderer needs for one frame. Held by reference to the
// scene's evaluated result, so building it copies no geometry.
struct HairViewState {
  const HairEvalResult* eval = nullptr;
  Camera camera;
  HairToolKind tool = HairToolKind::None;

  // Live stroke preview while Draw Guides is dragging.
  GuideCurve strokePreview;

  GuidePointRef selectedPoint;
  GuidePointRef hoveredPoint;
  HairDragAxis activeAxis = HairDragAxis::CameraPlane;

  bool brushVisible = false;
  bool brushStroking = false;
  Vec3 brushCenter;
  float brushRadius = 0.3f;

  bool clumpGizmoVisible = false;
  bool clumpGizmoHovered = false;
  Vec3 clumpCenter;

  // Paint Clump brush, shown as a ring on the scalp surface.
  bool paintBrushVisible = false;
  bool paintBrushStroking = false;
  Vec3 paintBrushCenter;
  Vec3 paintBrushNormal{0.0f, 1.0f, 0.0f};
  float paintBrushRadius = 0.28f;
  bool paintErase = false;
};

class HairScene {
 public:
  explicit HairScene(std::shared_ptr<GraphDocument> document);

  const std::shared_ptr<GraphDocument>& document() const { return document_; }

  // ── viewport and camera ────────────────────────────────────────────────────
  void setViewport(float widthPoints, float heightPoints);
  const Camera& camera() const { return camera_; }
  void orbitBy(float deltaXPoints, float deltaYPoints);
  void truckBy(float deltaXPoints, float deltaYPoints);
  void dollyBy(float factor);
  void frameGroom();

  // ── evaluation ─────────────────────────────────────────────────────────────

  // Evaluates only when the graph, the parameters, or a typed store changed.
  // Camera motion never invalidates the cache, so orbiting does not regenerate
  // hair; `recomputedOnLastQuery` reports which happened.
  const HairEvalResult& result();
  bool recomputedOnLastQuery() const { return recomputed_; }
  const DirtyGenerations& generations() const { return generations_; }

  // Call after any editor-authored attribute edit so the next result() picks it
  // up, and so a `tool:*` toggle re-establishes exclusivity.
  void onAttributeEdited(const std::string& nodeId,
                         const std::string& propertyName);
  void onTopologyEdited();

  // ── tools ──────────────────────────────────────────────────────────────────
  HairToolKind activeTool();
  // Writes the switches back to the document: the one for `kind` on, every
  // other mutating tool off. Node switches stay the single source of truth.
  void setActiveTool(HairToolKind kind);

  // ── input, in view points ──────────────────────────────────────────────────

  // Decides ownership for the whole gesture. Returns true when the scene takes
  // it — which is always, once the caller has established that the press did
  // not land on graph content: an unclaimed tool falls through to the camera.
  bool pointerDown(float x, float y, bool alternate);
  void pointerMove(float x, float y);
  void pointerUp(float x, float y);
  void hoverMove(float x, float y);
  HairGestureOwner gestureOwner() const { return gestureOwner_; }

  void pinchBegin();
  void pinchUpdate(float scale);
  void pinchEnd();

  const std::string& status() const { return status_; }
  void setStatus(std::string message) { status_ = std::move(message); }

  HairViewState viewState();

  // Testing seams: the stores are the node-owned typed data.
  const HairStores& stores() const { return stores_; }
  HairStores& mutableStores() { return stores_; }
  void reseedGuides();

 private:
  GraphSnapshot snapshot() const;
  void conformStores(const GraphSnapshot& snapshot);
  bool beginTool(HairToolKind kind, const GraphSnapshot& snapshot,
                 const HairEvalResult& eval, float x, float y);
  void continueTool(float x, float y);
  void finishTool(float x, float y);

  bool beginDraw(const GraphSnapshot& snapshot, const HairEvalResult& eval,
                 float x, float y);
  bool beginEditPoints(const GraphSnapshot& snapshot,
                       const HairEvalResult& eval, float x, float y);
  bool beginComb(const GraphSnapshot& snapshot, const HairEvalResult& eval,
                 float x, float y);
  bool beginEditClump(const GraphSnapshot& snapshot, const HairEvalResult& eval,
                      float x, float y);
  bool beginPaintClump(const GraphSnapshot& snapshot,
                       const HairEvalResult& eval, float x, float y);
  // Ray-cast to the scalp and stamp the clump-weight brush there. Shared by
  // the initial press and every subsequent move.
  bool paintClumpAt(float x, float y);

  void setNumber(const std::string& nodeId, const std::string& propertyName,
                 double value);
  std::string preferredToolNodeId(HairToolKind kind,
                                  const GraphSnapshot& graph);

  std::shared_ptr<GraphDocument> document_;
  HairStores stores_;
  DirtyGenerations generations_;

  HairEvalResult result_;
  HairEvalKey resultKey_;
  bool hasResult_ = false;
  bool recomputed_ = false;

  Camera camera_;
  std::string status_;

  HairGestureOwner gestureOwner_ = HairGestureOwner::None;
  HairToolKind gestureTool_ = HairToolKind::None;
  float lastX_ = 0.0f;
  float lastY_ = 0.0f;

  // Draw Guides
  GuideStroke stroke_;
  std::string strokeNodeId_;

  // Edit Points
  GuidePointRef selectedPoint_;
  GuidePointRef hoveredPoint_;
  GuidePointRef dragPoint_;
  HairDragAxis dragAxis_ = HairDragAxis::CameraPlane;
  Vec3 dragAnchor_;
  Vec3 dragPlaneOffset_;
  float dragAxisOffset_ = 0.0f;
  std::string editNodeId_;

  // Comb Brush
  bool brushStroking_ = false;
  bool brushHasCenter_ = false;
  Vec3 brushCenter_;
  Vec3 brushPlanePoint_;
  Vec3 brushPlaneNormal_{0.0f, 0.0f, 1.0f};
  std::string sculptNodeId_;

  // Edit Clump
  bool clumpHovered_ = false;
  std::string clumpNodeId_;

  // Paint Clump
  bool paintStroking_ = false;
  bool paintHasCenter_ = false;
  Vec3 paintCenter_;
  Vec3 paintNormal_{0.0f, 1.0f, 0.0f};
  std::string paintNodeId_;

  float pinchStartDistance_ = 0.0f;
  bool pinching_ = false;
};

}  // namespace noodles::demo::hair
