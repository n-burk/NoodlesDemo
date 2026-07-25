// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT
#pragma once

// Draws the groom into whatever framebuffer is currently bound, immediately
// before GraphEditor composites its transparent graph on top. It owns only GL
// objects: all geometry comes from HairRenderGeometry, and all state comes
// from HairScene.
//
// The same source builds against desktop OpenGL 3.3 core and OpenGL ES 3.0.

#include "HairRenderGeometry.h"
#include "HairScene.h"

#include <cstdint>
#include <string>

namespace noodles::demo::hair {

class HairGLRenderer {
 public:
  HairGLRenderer();
  ~HairGLRenderer();

  HairGLRenderer(const HairGLRenderer&) = delete;
  HairGLRenderer& operator=(const HairGLRenderer&) = delete;

  // All three require the platform's GL context to be current.
  bool initialize();
  void shutdown();
  bool ready() const { return ready_; }

  // Renders one frame into the bound framebuffer. `widthPx`/`heightPx` are
  // physical pixels; `contentScale` converts view points to pixels and keeps
  // gizmo point sizes constant on Retina displays.
  void render(HairScene& scene, int widthPx, int heightPx, float contentScale);

  const std::string& lastError() const { return lastError_; }

  // Number of strand triangles issued by the last render, for diagnostics and
  // the render tests.
  std::size_t lastTriangleCount() const { return lastTriangleCount_; }

 private:
  struct Program;

  bool buildPrograms();
  void uploadSolid(const HairSolidGeometry& geometry);
  void uploadClumpMap(const ClumpMapImage& image);
  void uploadOverlay(const HairOverlayGeometry& geometry);
  void releaseBuffers();

  bool ready_ = false;
  std::string lastError_;
  std::size_t lastTriangleCount_ = 0;

  // Solid geometry is re-uploaded only when the evaluation generation changes,
  // so orbiting the camera never touches these buffers.
  std::uint64_t uploadedEvaluation_ = 0;
  bool hasUpload_ = false;

  unsigned meshProgram_ = 0;
  unsigned ribbonProgram_ = 0;
  unsigned overlayProgram_ = 0;

  unsigned meshVao_ = 0;
  unsigned meshVbo_ = 0;
  unsigned meshIbo_ = 0;
  unsigned meshWireIbo_ = 0;
  unsigned clumpMapTexture_ = 0;
  bool hasClumpMap_ = false;
  unsigned ribbonVao_ = 0;
  unsigned ribbonVbo_ = 0;
  unsigned ribbonIbo_ = 0;
  unsigned lineVao_ = 0;
  unsigned lineVbo_ = 0;
  unsigned pointVao_ = 0;
  unsigned pointVbo_ = 0;

  std::size_t meshIndexCount_ = 0;
  std::size_t meshWireIndexCount_ = 0;
  std::size_t ribbonIndexCount_ = 0;
  std::size_t lineVertexCount_ = 0;
  std::size_t pointVertexCount_ = 0;

  std::size_t lineCapacityBytes_ = 0;
  std::size_t pointCapacityBytes_ = 0;
};

}  // namespace noodles::demo::hair
