// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT
//
// Offscreen GL coverage for the hair demo's viewport, and for the one Core
// change it depends on.
//
// The groom is drawn into the same framebuffer the editor then composites the
// graph into. That only works because GraphEditor::setOverlayBlendsWithBackground
// switches the overlay quad from an unblended overwrite to premultiplied
// source-over. These tests rasterize real geometry through the real shaders and
// then assert exactly that: with blending on the groom survives underneath the
// graph, and with it off the graph erases it. A CPU-only test cannot see the
// difference, because the difference is one GL blend state.
//
// macOS-only (CGL), mirroring GraphEditorRenderTests.

#include "HairGLRenderer.h"
#include "HairGraph.h"
#include "HairScene.h"

#include <noodles/demo/GraphEditor.h>
#include <noodles/demo/InMemoryGraphDocument.h>

#include <OpenGL/OpenGL.h>  // CGL
#include <OpenGL/gl3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace nd = noodles::demo;
namespace hair = noodles::demo::hair;

#ifndef NOODLES_DEMO_TEST_ASSET_DIR
#define NOODLES_DEMO_TEST_ASSET_DIR "."
#endif

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                   #condition);                                               \
      return false;                                                           \
    }                                                                         \
  } while (false)

namespace {

constexpr int kWidth = 480;
constexpr int kHeight = 360;

struct Rgba {
  std::uint8_t r, g, b, a;
};

bool MakeContext(CGLContextObj* outContext) {
  const CGLPixelFormatAttribute accelerated[] = {
      kCGLPFAOpenGLProfile,
      (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
      kCGLPFAAccelerated,
      kCGLPFAColorSize,
      (CGLPixelFormatAttribute)24,
      kCGLPFAAlphaSize,
      (CGLPixelFormatAttribute)8,
      kCGLPFADepthSize,
      (CGLPixelFormatAttribute)24,
      (CGLPixelFormatAttribute)0};
  const CGLPixelFormatAttribute software[] = {
      kCGLPFAOpenGLProfile,
      (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
      kCGLPFAColorSize,
      (CGLPixelFormatAttribute)24,
      kCGLPFAAlphaSize,
      (CGLPixelFormatAttribute)8,
      kCGLPFADepthSize,
      (CGLPixelFormatAttribute)24,
      (CGLPixelFormatAttribute)0};

  CGLPixelFormatObj format = nullptr;
  GLint count = 0;
  if (CGLChoosePixelFormat(accelerated, &format, &count) != kCGLNoError ||
      !format) {
    if (CGLChoosePixelFormat(software, &format, &count) != kCGLNoError ||
        !format) {
      return false;
    }
  }
  CGLContextObj context = nullptr;
  const CGLError error = CGLCreateContext(format, nullptr, &context);
  CGLDestroyPixelFormat(format);
  if (error != kCGLNoError || !context) return false;
  CGLSetCurrentContext(context);
  *outContext = context;
  return true;
}

// A private RGBA8 + depth framebuffer standing in for the platform drawable.
// Deliberately not framebuffer 0: on iOS the "default" framebuffer is the
// CAEAGLLayer's FBO, and both the editor and the hair renderer have to respect
// whatever is bound.
struct OffscreenTarget {
  GLuint framebuffer = 0;
  GLuint color = 0;
  GLuint depth = 0;

  bool create() {
    glGenFramebuffers(1, &framebuffer);
    glGenTextures(1, &color);
    glGenRenderbuffers(1, &depth);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kWidth, kHeight, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kWidth,
                          kHeight);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depth);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  }

  void destroy() {
    if (depth) glDeleteRenderbuffers(1, &depth);
    if (color) glDeleteTextures(1, &color);
    if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
    framebuffer = color = depth = 0;
  }
};

std::vector<Rgba> ReadPixels() {
  std::vector<Rgba> pixels(static_cast<std::size_t>(kWidth * kHeight));
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE,
               pixels.data());
  return pixels;
}

int DrainGlErrors(const char* stage) {
  int count = 0;
  for (GLenum error = glGetError(); error != GL_NO_ERROR;
       error = glGetError()) {
    std::printf("  [GL ERROR] after %s: 0x%04X\n", stage, error);
    if (++count > 32) break;
  }
  return count;
}

// The renderer clears to this; anything else is groom.
bool IsViewportClear(const Rgba& pixel) {
  return pixel.r <= 20 && pixel.g <= 22 && pixel.b <= 28;
}

bool IsBlack(const Rgba& pixel) {
  return pixel.r <= 3 && pixel.g <= 3 && pixel.b <= 3;
}

int CountWhere(const std::vector<Rgba>& pixels, bool (*predicate)(const Rgba&)) {
  int count = 0;
  for (const Rgba& pixel : pixels) {
    if (predicate(pixel)) ++count;
  }
  return count;
}

std::string AssetPath(const char* suffix) {
  return std::string(NOODLES_DEMO_TEST_ASSET_DIR) + suffix;
}

struct Harness {
  CGLContextObj context = nullptr;
  OffscreenTarget target;
  std::shared_ptr<nd::InMemoryGraphDocument> document;
  std::unique_ptr<hair::HairScene> scene;
  std::unique_ptr<hair::HairGLRenderer> renderer;

  bool setUp() {
    if (!MakeContext(&context)) return false;
    if (!target.create()) return false;
    document = hair::CreateHairGraphDocument();
    scene = std::make_unique<hair::HairScene>(document);
    scene->setViewport(static_cast<float>(kWidth), static_cast<float>(kHeight));
    scene->frameGroom();
    renderer = std::make_unique<hair::HairGLRenderer>();
    return true;
  }

  void tearDown() {
    if (renderer) renderer->shutdown();
    renderer.reset();
    scene.reset();
    document.reset();
    target.destroy();
    if (context) {
      CGLSetCurrentContext(nullptr);
      CGLDestroyContext(context);
      context = nullptr;
    }
  }

  void renderGroom() {
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    renderer->render(*scene, kWidth, kHeight, 1.0f);
  }
};

// ── tests ───────────────────────────────────────────────────────────────────

bool TestGroomRasterizes() {
  Harness harness;
  if (!harness.setUp()) {
    std::printf("  [skip] no usable GL context\n");
    return true;
  }
  bool ok = [&]() -> bool {
    CHECK(harness.renderer->initialize());
    CHECK(harness.renderer->lastError().empty());
    harness.renderGroom();
    CHECK(DrainGlErrors("hair render") == 0);

    // A real groom was drawn: strand triangles were issued, and a substantial
    // share of the frame is no longer the clear color.
    CHECK(harness.renderer->lastTriangleCount() > 0);
    const std::vector<Rgba> pixels = ReadPixels();
    const int clear = CountWhere(pixels, IsViewportClear);
    const int painted = kWidth * kHeight - clear;
    std::printf("  [hair] painted=%d of %d, triangles=%zu\n", painted,
                kWidth * kHeight, harness.renderer->lastTriangleCount());
    CHECK(painted > (kWidth * kHeight) / 20);

    // Every pixel is opaque: the viewport owns its alpha, which is what lets
    // the graph blend over it without the window compositor showing through.
    for (const Rgba& pixel : pixels) CHECK(pixel.a == 255);
    return true;
  }();
  harness.tearDown();
  return ok;
}

bool TestHiddenHairAndScalpRemoveGeometry() {
  Harness harness;
  if (!harness.setUp()) {
    std::printf("  [skip] no usable GL context\n");
    return true;
  }
  bool ok = [&]() -> bool {
    CHECK(harness.renderer->initialize());
    harness.renderGroom();
    const int paintedWithHair =
        (kWidth * kHeight) - CountWhere(ReadPixels(), IsViewportClear);

    // The display switches are real: turning hair and the scalp off removes
    // their geometry from the frame.
    harness.document->setAttributeValue(hair::ids::kOutput,
                                        hair::props::kShowHair, 0.0, 0.0);
    harness.document->setAttributeValue(hair::ids::kScalp,
                                        hair::props::kScalpVisible, 0.0, 0.0);
    harness.document->setAttributeValue(hair::ids::kOutput,
                                        hair::props::kShowGuides, 0.0, 0.0);
    harness.document->setAttributeValue(hair::ids::kOutput,
                                        hair::props::kShowGrid, 0.0, 0.0);
    harness.document->setAttributeValue(
        hair::ids::kCreateGuides, hair::props::kGuidesGizmos, 0.0, 0.0);
    harness.document->setAttributeValue(hair::ids::kClump,
                                        hair::props::kClumpGizmo, 0.0, 0.0);
    // The clump map ships off; setting it again here would be a no-op.
    harness.scene->onAttributeEdited(hair::ids::kOutput,
                                     hair::props::kShowHair);
    harness.renderGroom();
    CHECK(DrainGlErrors("hidden render") == 0);

    const int paintedWithout =
        (kWidth * kHeight) - CountWhere(ReadPixels(), IsViewportClear);
    std::printf("  [hidden] painted %d -> %d\n", paintedWithHair,
                paintedWithout);
    CHECK(paintedWithHair > 0);
    CHECK(paintedWithout == 0);
    CHECK(harness.renderer->lastTriangleCount() == 0);
    return true;
  }();
  harness.tearDown();
  return ok;
}

// The decisive test for the Core change.
bool TestGraphCompositePreservesTheGroom() {
  Harness harness;
  if (!harness.setUp()) {
    std::printf("  [skip] no usable GL context\n");
    return true;
  }
  bool ok = [&]() -> bool {
    CHECK(harness.renderer->initialize());

    // An EMPTY document, deliberately: with no nodes there is no graph content
    // and no minimap, so the overlay pass contributes nothing of its own and
    // what remains is purely the blend-versus-overwrite question.
    auto editor = std::make_shared<nd::GraphEditor>();
    editor->setClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    editor->setOverlayOpacity(0.5f);
    editor->setDocument(std::make_shared<nd::InMemoryGraphDocument>());
    editor->initializeGL(AssetPath(""), AssetPath("/fonts/Poppins-Regular.png"),
                         AssetPath("/fonts/Poppins-Regular.json"));
    editor->resize(kWidth, kHeight, 1.0f);
    CHECK(editor->nodeCount() == 0);
    CHECK(!editor->minimapVisible());
    CHECK(DrainGlErrors("editor init") == 0);

    // Baseline: the groom alone.
    harness.renderGroom();
    const std::vector<Rgba> groomOnly = ReadPixels();
    const int groomPainted =
        (kWidth * kHeight) - CountWhere(groomOnly, IsViewportClear);
    CHECK(groomPainted > 0);

    // Default (overwrite) compositing: the overlay quad owns every pixel, so
    // an empty graph wipes the groom to transparent black. This is correct for
    // the shipping shells, where the graph sits on its own surface above a
    // separate canvas view — and is exactly why the demo cannot use it.
    CHECK(!editor->overlayBlendsWithBackground());
    glBindFramebuffer(GL_FRAMEBUFFER, harness.target.framebuffer);
    editor->renderFrame();
    CHECK(DrainGlErrors("overwrite composite") == 0);
    const std::vector<Rgba> overwritten = ReadPixels();
    const int blackAfterOverwrite = CountWhere(overwritten, IsBlack);
    std::printf("  [overwrite] black=%d of %d\n", blackAfterOverwrite,
                kWidth * kHeight);
    CHECK(blackAfterOverwrite == kWidth * kHeight);

    // Blended compositing: the groom survives underneath.
    editor->setOverlayBlendsWithBackground(true);
    CHECK(editor->overlayBlendsWithBackground());
    harness.renderGroom();
    glBindFramebuffer(GL_FRAMEBUFFER, harness.target.framebuffer);
    editor->renderFrame();
    CHECK(DrainGlErrors("blended composite") == 0);
    const std::vector<Rgba> blended = ReadPixels();

    int preserved = 0;
    int compared = 0;
    for (std::size_t i = 0; i < groomOnly.size(); ++i) {
      if (IsViewportClear(groomOnly[i])) continue;
      ++compared;
      const int dr = static_cast<int>(blended[i].r) - groomOnly[i].r;
      const int dg = static_cast<int>(blended[i].g) - groomOnly[i].g;
      const int db = static_cast<int>(blended[i].b) - groomOnly[i].b;
      if (dr * dr + dg * dg + db * db <= 12) ++preserved;
    }
    std::printf("  [blended] preserved %d of %d groom pixels\n", preserved,
                compared);
    CHECK(compared > 0);
    CHECK(preserved == compared);

    editor->cleanupGL();
    return true;
  }();
  harness.tearDown();
  return ok;
}

bool TestGraphDrawsOverTheGroom() {
  Harness harness;
  if (!harness.setUp()) {
    std::printf("  [skip] no usable GL context\n");
    return true;
  }
  bool ok = [&]() -> bool {
    CHECK(harness.renderer->initialize());

    auto editor = std::make_shared<nd::GraphEditor>();
    editor->setClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    editor->setOverlayOpacity(1.0f);
    editor->setOverlayBlendsWithBackground(true);
    editor->setDocument(harness.document);
    editor->initializeGL(AssetPath(""), AssetPath("/fonts/Poppins-Regular.png"),
                         AssetPath("/fonts/Poppins-Regular.json"));
    editor->resize(kWidth, kHeight, 1.0f);
    CHECK(editor->frameAll(8.0));

    harness.renderGroom();
    const std::vector<Rgba> groomOnly = ReadPixels();

    glBindFramebuffer(GL_FRAMEBUFFER, harness.target.framebuffer);
    editor->renderFrame();
    CHECK(DrainGlErrors("graph over groom") == 0);
    const std::vector<Rgba> composited = ReadPixels();

    // With the graph in view, a real share of the frame changed: the nodes are
    // genuinely painted on top of the groom rather than beside it.
    int changed = 0;
    for (std::size_t i = 0; i < groomOnly.size(); ++i) {
      if (groomOnly[i].r != composited[i].r ||
          groomOnly[i].g != composited[i].g ||
          groomOnly[i].b != composited[i].b) {
        ++changed;
      }
    }
    std::printf("  [over] %d of %d pixels changed\n", changed,
                kWidth * kHeight);
    CHECK(changed > (kWidth * kHeight) / 50);

    editor->cleanupGL();
    return true;
  }();
  harness.tearDown();
  return ok;
}

// Camera motion must not touch the strand buffers; only a groom change may.
bool TestCameraOrbitDoesNotReuploadHair() {
  Harness harness;
  if (!harness.setUp()) {
    std::printf("  [skip] no usable GL context\n");
    return true;
  }
  bool ok = [&]() -> bool {
    CHECK(harness.renderer->initialize());
    harness.renderGroom();
    const std::uint64_t afterFirst = harness.scene->generations().evaluation;

    for (int step = 0; step < 12; ++step) {
      harness.scene->orbitBy(9.0f, 3.0f);
      harness.renderGroom();
      CHECK(harness.scene->generations().evaluation == afterFirst);
      CHECK(!harness.scene->recomputedOnLastQuery());
    }
    CHECK(DrainGlErrors("orbit renders") == 0);

    harness.document->setAttributeValue(hair::ids::kGenerateHair,
                                        hair::props::kHairDensity, 700.0, 0.0);
    harness.scene->onAttributeEdited(hair::ids::kGenerateHair,
                                     hair::props::kHairDensity);
    harness.renderGroom();
    CHECK(harness.scene->generations().evaluation > afterFirst);
    CHECK(harness.scene->result().hair.strands.size() == 700);
    return true;
  }();
  harness.tearDown();
  return ok;
}

// The clump map has to reach the scalp as genuinely different colours, not as
// a brightness ramp of one hue.
bool TestClumpMapPaintsPastelRegionsOnTheScalp() {
  Harness harness;
  if (!harness.setUp()) {
    std::printf("  [skip] no usable GL context\n");
    return true;
  }
  bool ok = [&]() -> bool {
    CHECK(harness.renderer->initialize());
    // Hide the hair so the scalp is unobstructed, and turn the map on.
    harness.document->setAttributeValue(hair::ids::kOutput,
                                        hair::props::kShowHair, 0.0, 0.0);
    harness.document->setAttributeValue(hair::ids::kOutput,
                                        hair::props::kShowGuides, 0.0, 0.0);
    harness.document->setAttributeValue(
        hair::ids::kCreateGuides, hair::props::kGuidesGizmos, 0.0, 0.0);
    harness.document->setAttributeValue(hair::ids::kClump,
                                        hair::props::kClumpGizmo, 0.0, 0.0);
    harness.scene->onAttributeEdited(hair::ids::kOutput,
                                     hair::props::kShowHair);

    harness.renderGroom();
    const std::vector<Rgba> plain = ReadPixels();

    harness.document->setAttributeValue(hair::ids::kClump,
                                        hair::props::kClumpShowMap, 1.0, 0.0);
    harness.scene->onAttributeEdited(hair::ids::kClump,
                                     hair::props::kClumpShowMap);
    CHECK(harness.scene->result().display.showClumpMap);
    CHECK(!harness.scene->result().clumpMapImage.empty());
    harness.renderGroom();
    CHECK(DrainGlErrors("clump map render") == 0);
    const std::vector<Rgba> mapped = ReadPixels();

    // Count distinct hues on the scalp. Quantizing the hue angle ignores the
    // shading gradient, so this counts regions rather than brightness steps.
    bool hues[24] = {false};
    int scalpPixels = 0;
    int changed = 0;
    for (std::size_t i = 0; i < mapped.size(); ++i) {
      if (IsViewportClear(mapped[i])) continue;
      ++scalpPixels;
      if (mapped[i].r != plain[i].r || mapped[i].g != plain[i].g ||
          mapped[i].b != plain[i].b) {
        ++changed;
      }
      const int maxChannel =
          std::max<int>(mapped[i].r, std::max<int>(mapped[i].g, mapped[i].b));
      const int minChannel =
          std::min<int>(mapped[i].r, std::min<int>(mapped[i].g, mapped[i].b));
      const int chroma = maxChannel - minChannel;
      if (maxChannel < 40 || chroma < 12) continue;  // ink or near-grey
      float hue = 0.0f;
      if (maxChannel == mapped[i].r) {
        hue = static_cast<float>(mapped[i].g - mapped[i].b) /
              static_cast<float>(chroma);
      } else if (maxChannel == mapped[i].g) {
        hue = 2.0f + static_cast<float>(mapped[i].b - mapped[i].r) /
                         static_cast<float>(chroma);
      } else {
        hue = 4.0f + static_cast<float>(mapped[i].r - mapped[i].g) /
                         static_cast<float>(chroma);
      }
      if (hue < 0.0f) hue += 6.0f;
      const int bucket = std::min(23, static_cast<int>(hue * 4.0f));
      hues[bucket] = true;
    }
    int distinctHues = 0;
    for (const bool present : hues) {
      if (present) ++distinctHues;
    }
    std::printf("  [clumpmap] scalp=%d changed=%d distinctHues=%d\n",
                scalpPixels, changed, distinctHues);
    CHECK(scalpPixels > 0);
    CHECK(changed > scalpPixels / 2);
    // Many well-separated hues, which a single-hue brightness ramp cannot
    // produce.
    CHECK(distinctHues >= 6);
    return true;
  }();
  harness.tearDown();
  return ok;
}

struct TestCase {
  const char* name;
  bool (*run)();
};

}  // namespace

int main() {
  const TestCase tests[] = {
      {"groom_rasterizes", TestGroomRasterizes},
      {"hidden_hair_and_scalp_remove_geometry",
       TestHiddenHairAndScalpRemoveGeometry},
      {"graph_composite_preserves_groom",
       TestGraphCompositePreservesTheGroom},
      {"graph_draws_over_groom", TestGraphDrawsOverTheGroom},
      {"camera_orbit_does_not_reupload_hair",
       TestCameraOrbitDoesNotReuploadHair},
      {"clump_map_paints_pastel_regions",
       TestClumpMapPaintsPastelRegionsOnTheScalp},
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
