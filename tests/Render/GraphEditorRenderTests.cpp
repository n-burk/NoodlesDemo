// GraphEditorRenderTests.cpp — a macOS offscreen GL pixel regression for the
// public NoodlesApple Core renderer. The fixture uses InMemoryGraphDocument, so
// the test has no OpenUSD or product dependency.
//
// It creates a windowless desktop GL 3.2 core context via CGL, binds a private
// RGBA8+depth FBO as the "default" framebuffer (mirroring iOS, where the default
// framebuffer is the CAEAGLLayer FBO, not 0 — this exercises renderFrame's
// GL_FRAMEBUFFER_BINDING save/restore), renders, and classifies every pixel as
// background / link-ish / node-ish. Two passes: opacity 1.0 (direct path) and
// opacity 0.5 (offscreen-FBO composite path).
//
// macOS-only (CGL). The title-drag, minimap, and relationship-exit cases gate
// behavior; opaque and composite passes gate that node interiors paint.

#include "DemoGraphFixture.h"

#include <noodles/apple/GraphEditor.h>
#include <noodles/apple/InMemoryGraphDocument.h>

#include <OpenGL/OpenGL.h>  // CGL
#include <OpenGL/gl3.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ig = noodles::apple;

#ifndef NOODLES_APPLE_TEST_ASSET_DIR
#  define NOODLES_APPLE_TEST_ASSET_DIR "."
#endif

namespace {

constexpr int kW = 512;
constexpr int kH = 512;
constexpr double kZoom = 0.4;  // fit the large laid-out nodes with background

// Drain and print the GL error queue with a stage label; returns the count.
int drainGlErrors(const char* stage) {
  int n = 0;
  for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError()) {
    std::printf("  [GL ERROR] after %s: 0x%04X\n", stage, e);
    ++n;
    if (n > 32) break;  // runaway guard
  }
  if (n == 0) std::printf("  [gl] %s: no errors\n", stage);
  return n;
}

ig::GraphNode Node(std::string id, double x, double y) {
  ig::GraphNode node;
  node.id = std::move(id);
  const std::size_t slash = node.id.rfind('/');
  node.name = slash == std::string::npos ? node.id : node.id.substr(slash + 1);
  node.hasPosition = true;
  node.posX = x;
  node.posY = y;
  return node;
}

ig::GraphProperty Numeric(std::string name, double value) {
  ig::GraphProperty property;
  property.name = std::move(name);
  property.kind = ig::GraphPropertyKind::Attribute;
  property.type = "float";
  property.hasValue = true;
  property.isScrubable = true;
  property.numericValue = value;
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%g", value);
  property.displayValue = buffer;
  return property;
}

ig::GraphProperty Relationship(std::string name) {
  ig::GraphProperty property;
  property.name = std::move(name);
  property.kind = ig::GraphPropertyKind::Relationship;
  property.type = "rel";
  return property;
}

// A → B via a relationship. Authored positions and an attribute row on each
// node keep the fixture geometry deterministic.
std::shared_ptr<ig::InMemoryGraphDocument> MakeDocument() {
  auto document = std::make_shared<ig::InMemoryGraphDocument>();
  document->addNode(Node("/Root/A", 60.0, 60.0));
  document->addNode(Node("/Root/B", 300.0, 300.0));
  document->addProperty("/Root/A", Relationship("custom:link"));
  document->addProperty("/Root/A", Numeric("in", 0.5));
  document->addProperty("/Root/B", Numeric("in", 0.5));
  document->authorRelationship("/Root/A", "custom:link", "/Root/B");
  return document;
}

// A whole-prim relationship whose TARGET sits to the LEFT of the SOURCE — the
// case that exposed the device bug: the noodle must still leave the source's
// right-edge port OUTWARD, not dive back across the row toward the node center.
// S is placed to the right, T to the left; both carry an attribute row for real
// height. custom:link has an empty target port, so it renders as the vertical-
// end-tangent (prim-target) curve.
std::shared_ptr<ig::InMemoryGraphDocument> MakeRelLeftDocument() {
  auto document = std::make_shared<ig::InMemoryGraphDocument>();
  document->addNode(Node("/Rel/S", 700.0, 200.0));  // source on the right
  document->addNode(Node("/Rel/T", 60.0, 160.0));   // target on the left
  document->addProperty("/Rel/S", Relationship("custom:link"));
  document->addProperty("/Rel/S", Numeric("in", 0.5));
  document->addProperty("/Rel/T", Numeric("in", 0.5));
  document->authorRelationship("/Rel/S", "custom:link", "/Rel/T");
  return document;
}

struct Rgba {
  uint8_t r, g, b, a;
};

// One frame's worth of readback + classification.
struct FrameStats {
  int background = 0;   // == clear color (within tolerance)
  int nonBackground = 0;
  int nodeInterior = 0;      // sampled pixels inside node A's projected rect
  int nodeInteriorNonBg = 0; // ...of those, actually painted
};

// Create the offscreen GL 3.2 core context. Returns false if no context could be
// made (headless CI with no GL at all) — the harness treats that as a skip.
bool makeContext(CGLContextObj* outCtx) {
  const CGLPixelFormatAttribute accel[] = {
      kCGLPFAOpenGLProfile,
      (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
      kCGLPFAAccelerated,
      kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
      kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
      kCGLPFADepthSize, (CGLPixelFormatAttribute)24,
      (CGLPixelFormatAttribute)0};
  // Software fallback (no kCGLPFAAccelerated) for machines without a usable GPU
  // context (rare, but keeps CI honest rather than silently passing).
  const CGLPixelFormatAttribute soft[] = {
      kCGLPFAOpenGLProfile,
      (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
      kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
      kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
      kCGLPFADepthSize, (CGLPixelFormatAttribute)24,
      (CGLPixelFormatAttribute)0};

  CGLPixelFormatObj pix = nullptr;
  GLint nPix = 0;
  if (CGLChoosePixelFormat(accel, &pix, &nPix) != kCGLNoError || !pix) {
    std::printf("  [ctx] accelerated pixel format unavailable, trying software\n");
    if (CGLChoosePixelFormat(soft, &pix, &nPix) != kCGLNoError || !pix) {
      std::printf("  [ctx] no 3.2 core pixel format available at all\n");
      return false;
    }
  }
  CGLContextObj ctx = nullptr;
  CGLError err = CGLCreateContext(pix, nullptr, &ctx);
  CGLDestroyPixelFormat(pix);
  if (err != kCGLNoError || !ctx) {
    std::printf("  [ctx] CGLCreateContext failed: %d\n", err);
    return false;
  }
  CGLSetCurrentContext(ctx);
  *outCtx = ctx;
  const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  const char* ren = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  std::printf("  [ctx] GL_VERSION=%s\n  [ctx] GL_RENDERER=%s\n",
              ver ? ver : "(null)", ren ? ren : "(null)");
  return true;
}

// Build an RGBA8 + depth FBO to stand in for the platform "default" framebuffer.
GLuint makeDefaultFbo() {
  GLuint fbo = 0, color = 0, depth = 0;
  glGenFramebuffers(1, &fbo);
  glGenTextures(1, &color);
  glGenRenderbuffers(1, &depth);

  glBindTexture(GL_TEXTURE_2D, color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kW, kH, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kW, kH);

  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         color, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, depth);
  GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (st != GL_FRAMEBUFFER_COMPLETE) {
    std::printf("  [fbo] INCOMPLETE default-substitute FBO: 0x%04X\n", st);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  return fbo;
}

// Sample-classify a readback buffer. clear* is the opaque clear color.
FrameStats classify(const std::vector<Rgba>& px, uint8_t cr, uint8_t cg,
                    uint8_t cb, int nodeCol0, int nodeRow0, int nodeCol1,
                    int nodeRow1) {
  FrameStats s;
  auto isClear = [&](const Rgba& p) {
    auto near = [](int x, int y) { return std::abs(x - y) <= 6; };
    return near(p.r, cr) && near(p.g, cg) && near(p.b, cb);
  };
  for (const Rgba& p : px) {
    if (isClear(p)) ++s.background; else ++s.nonBackground;
  }
  // Node-interior sampling window (GL bottom-left origin already applied by
  // caller: rows are in framebuffer space).
  for (int row = nodeRow0; row <= nodeRow1; ++row) {
    if (row < 0 || row >= kH) continue;
    for (int col = nodeCol0; col <= nodeCol1; ++col) {
      if (col < 0 || col >= kW) continue;
      const Rgba& p = px[static_cast<size_t>(row) * kW + col];
      ++s.nodeInterior;
      if (!isClear(p)) ++s.nodeInteriorNonBg;
    }
  }
  return s;
}

// Render one pass and read it back. Returns false on a harness-level failure.
bool runPass(ig::NoodlesGraphView& view, GLuint defaultFbo, float opacity,
             const char* label, uint8_t cr, uint8_t cg, uint8_t cb,
             FrameStats* out) {
  std::printf("\n=== PASS: %s (overlayOpacity=%.2f) ===\n", label, opacity);
  view.setClearColor(cr / 255.0f, cg / 255.0f, cb / 255.0f, 1.0f);  // opaque
  view.setOverlayOpacity(opacity);

  glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
  glViewport(0, 0, kW, kH);
  drainGlErrors("bind default fbo");

  view.renderFrame();
  drainGlErrors("renderFrame");

  // Node A's projected rect. With pan=0 contentScale=1, screen(view) = world*zoom
  // (top-down); glReadPixels is bottom-left, so fb-row = H - view_y.
  double ax = 0, ay = 0, aw = 0, ah = 0;
  view.nodePosition("/Root/A", &ax, &ay);
  view.nodeSize("/Root/A", &aw, &ah);
  // Sample a tight interior window (inset 25%) to stay clear of the anti-aliased
  // rounded-corner border and land squarely on the node body fill.
  const double insetX = aw * 0.25, insetY = ah * 0.25;
  const int col0 = static_cast<int>((ax + insetX) * kZoom);
  const int col1 = static_cast<int>((ax + aw - insetX) * kZoom);
  const int viewYtop = static_cast<int>((ay + insetY) * kZoom);
  const int viewYbot = static_cast<int>((ay + ah - insetY) * kZoom);
  const int row0 = kH - viewYbot;  // flip
  const int row1 = kH - viewYtop;
  std::printf("  node A graph rect: pos=(%.1f,%.1f) size=(%.1f,%.1f)\n", ax, ay,
              aw, ah);
  std::printf("  sample window: cols[%d..%d] fb-rows[%d..%d]\n", col0, col1,
              row0, row1);

  std::vector<Rgba> px(static_cast<size_t>(kW) * kH);
  glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
  glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  drainGlErrors("glReadPixels");

  *out = classify(px, cr, cg, cb, col0, row0, col1, row1);
  std::printf("  pixels: background=%d nonBackground=%d\n", out->background,
              out->nonBackground);
  std::printf("  node-interior samples: total=%d painted(non-bg)=%d\n",
              out->nodeInterior, out->nodeInteriorNonBg);
  if (out->nodeInterior > 0) {
    const double frac = 100.0 * out->nodeInteriorNonBg / out->nodeInterior;
    std::printf("  node-interior painted fraction: %.1f%%\n", frac);
  }
  if (out->nonBackground == 0) {
    std::printf("  >>> NOTHING rendered at all (not even links).\n");
  } else if (out->nodeInteriorNonBg == 0) {
    std::printf("  >>> BUG REPRODUCED: something painted (links) but node "
                "bodies are INVISIBLE.\n");
  } else {
    std::printf("  >>> Nodes render on host (node bodies painted).\n");
  }
  return out->nonBackground > 0 && out->nodeInterior > 0 &&
         out->nodeInteriorNonBg > 0;
}

// A node sub-rectangle projected into framebuffer space (bottom-left origin),
// from the node's LIVE snapshot position/size at pan=0 zoom=kZoom
// contentScale=1. fx/fy are fractions of the node box (0 = min edge, 1 = max).
struct FbRect {
  int col0, col1, row0, row1;
};
FbRect projectNodeSubRect(ig::NoodlesGraphView& view, const char* id, double fx0,
                          double fy0, double fx1, double fy1) {
  double px = 0, py = 0, pw = 0, ph = 0;
  view.nodePosition(id, &px, &py);
  view.nodeSize(id, &pw, &ph);
  FbRect r;
  r.col0 = static_cast<int>((px + pw * fx0) * kZoom);
  r.col1 = static_cast<int>((px + pw * fx1) * kZoom);
  const int viewYtop = static_cast<int>((py + ph * fy0) * kZoom);
  const int viewYbot = static_cast<int>((py + ph * fy1) * kZoom);
  r.row0 = kH - viewYbot;  // GL bottom-left origin: flip
  r.row1 = kH - viewYtop;
  return r;
}

struct RectStats {
  int total = 0;
  int nonBg = 0;
  int textish = 0;  // pure-white glyph pixels (node body never reaches this)
};
RectStats classifyRect(const std::vector<Rgba>& px, const FbRect& r, uint8_t cr,
                       uint8_t cg, uint8_t cb) {
  RectStats s;
  auto isClear = [&](const Rgba& p) {
    auto near = [](int x, int y) { return std::abs(x - y) <= 6; };
    return near(p.r, cr) && near(p.g, cg) && near(p.b, cb);
  };
  for (int row = r.row0; row <= r.row1; ++row) {
    if (row < 0 || row >= kH) continue;
    for (int col = r.col0; col <= r.col1; ++col) {
      if (col < 0 || col >= kW) continue;
      const Rgba& p = px[static_cast<size_t>(row) * kW + col];
      ++s.total;
      if (!isClear(p)) ++s.nonBg;
      // Glyphs are drawn pure white; the node body is HSV(sat 0.35, val≈0.5) so
      // its brightest channel stays well under 200 even when selected — a pixel
      // white in all three channels is text, not body.
      if (p.r > 200 && p.g > 200 && p.b > 200) ++s.textish;
    }
  }
  return s;
}

// Drive a title drag and prove BOTH the node body AND its text move with it:
// the reported bug was quads following the drag while glyphs stayed pinned at
// the node's original position. Runs on the direct (opacity 1.0) path so the
// min>200 glyph test sees full-intensity white. Returns true iff every check
// passes (this case is a real regression gate, unlike the repro probe above).
bool runDragCase(ig::NoodlesGraphView& view, GLuint defaultFbo, uint8_t cr,
                 uint8_t cg, uint8_t cb) {
  std::printf("\n=== DRAG CASE: node text follows a title drag ===\n");
  view.setClearColor(cr / 255.0f, cg / 255.0f, cb / 255.0f, 1.0f);
  view.setOverlayOpacity(1.0f);

  auto readback = [&](std::vector<Rgba>& out) {
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
    glViewport(0, 0, kW, kH);
    view.renderFrame();
    out.assign(static_cast<size_t>(kW) * kH, Rgba{});
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
  };

  // A node's title band (top slice) — B sits far below A on screen and the
  // A→B link never reaches this band, so it is a contamination-free sample of
  // A's title text both before and after the drag. by1 stops at 0.26: the
  // relationship noodle now ends at the top-middle of B's target ARROW
  // (kRelArrowHeight above B's top edge), so its vertical-end-tangent overshoot
  // arc tops out ~36 world units higher than before — at 0.30 the band's bottom
  // row grazed that arc after the drag.
  const double bx0 = 0.05, by0 = 0.02, bx1 = 0.70, by1 = 0.26;

  // 1) Baseline: A at its authored position with body + title text painted.
  std::vector<Rgba> before;
  readback(before);
  const FbRect origBand = projectNodeSubRect(view, "/Root/A", bx0, by0, bx1, by1);
  const RectStats orig = classifyRect(before, origBand, cr, cg, cb);
  std::printf("  original title band cols[%d..%d] rows[%d..%d]: nonBg=%d textish=%d\n",
              origBand.col0, origBand.col1, origBand.row0, origBand.row1,
              orig.nonBg, orig.textish);

  // 2) Grab A by its title (VIEW coords: view = world*kZoom at pan 0) and drag
  //    +300 px in both axes — far enough that old and new rects never overlap.
  double ax = 0, ay = 0;
  view.nodePosition("/Root/A", &ax, &ay);
  const double downVX = (ax + 20.0) * kZoom;
  const double downVY = (ay + 12.0) * kZoom;
  view.pointerDown(downVX, downVY);
  view.pointerMove(downVX + 300.0, downVY + 300.0);
  view.pointerUp(downVX + 300.0, downVY + 300.0);

  // 3) Re-render; A's snapshot now reports the moved position.
  std::vector<Rgba> after;
  readback(after);
  const FbRect newBand = projectNodeSubRect(view, "/Root/A", bx0, by0, bx1, by1);
  const FbRect newBody = projectNodeSubRect(view, "/Root/A", 0.10, 0.10, 0.90, 0.90);
  const RectStats movedText = classifyRect(after, newBand, cr, cg, cb);
  const RectStats movedBody = classifyRect(after, newBody, cr, cg, cb);
  const RectStats vacated = classifyRect(after, origBand, cr, cg, cb);
  std::printf("  moved title band cols[%d..%d] rows[%d..%d]: nonBg=%d textish=%d\n",
              newBand.col0, newBand.col1, newBand.row0, newBand.row1,
              movedText.nonBg, movedText.textish);
  std::printf("  moved body rect: nonBg=%d\n", movedBody.nonBg);
  std::printf("  vacated original band: nonBg=%d textish=%d\n", vacated.nonBg,
              vacated.textish);
  // Diagnostic on residue: list the stray non-background pixels so a failure
  // names WHERE the contamination sits (band-clipping curve vs stale quad).
  if (vacated.nonBg > 0) {
    int listed = 0;
    for (int row = origBand.row0; row <= origBand.row1 && listed < 12; ++row) {
      for (int col = origBand.col0; col <= origBand.col1 && listed < 12; ++col) {
        const Rgba& p = after[static_cast<size_t>(row) * kW + col];
        const auto near = [](int x, int y) { return std::abs(x - y) <= 6; };
        if (!(near(p.r, cr) && near(p.g, cg) && near(p.b, cb))) {
          std::printf("    stray px col=%d row=%d rgba=(%d,%d,%d,%d)\n", col,
                      row, p.r, p.g, p.b, p.a);
          ++listed;
        }
      }
    }
  }

  bool ok = true;
  auto check = [&](bool cond, const char* msg) {
    std::printf("    [%s] %s\n", cond ? "PASS" : "FAIL", msg);
    ok = ok && cond;
  };
  check(orig.textish > 0, "text present in node A before the drag");
  check(movedBody.nonBg > 0, "(i) node body pixels at the new rect");
  check(movedText.textish > 0, "(ii) text pixels inside the new rect");
  check(vacated.nonBg == 0, "(iii) old rect body is gone (background)");
  check(vacated.textish == 0, "(iii) old rect text is gone");
  std::printf("  >>> DRAG CASE %s\n", ok ? "PASSED" : "FAILED");
  return ok;
}

// Item 4 (render): when the content exceeds the viewport the minimap draws in
// the bottom-right corner. Zoom in so the laid-out nodes overflow the viewport,
// render at opacity 1.0 (direct path), and assert the minimap corner has painted
// (non-background) pixels — the node rects + viewport rectangle inside its frame.
bool runMinimapCase(ig::NoodlesGraphView& view, GLuint defaultFbo, uint8_t cr,
                    uint8_t cg, uint8_t cb) {
  std::printf("\n=== MINIMAP CASE: overview appears when content overflows ===\n");
  view.setClearColor(cr / 255.0f, cg / 255.0f, cb / 255.0f, 1.0f);
  view.setOverlayOpacity(1.0f);
  view.setViewportPan(0.0, 0.0);
  view.setZoom(2.0);  // zoom in so the graph overflows the 512² viewport

  const bool visible = view.minimapVisible();
  std::printf("  minimapVisible=%d\n", visible ? 1 : 0);

  glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
  glViewport(0, 0, kW, kH);
  view.renderFrame();

  std::vector<Rgba> px(static_cast<size_t>(kW) * kH);
  glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
  glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

  // The minimap frame sits at the bottom-right in view points; contentScale is
  // 1, so points == pixels. In glReadPixels (bottom-left origin) that maps to a
  // high-column / low-row window. Count painted pixels there.
  auto isClear = [&](const Rgba& p) {
    auto near = [](int x, int y) { return std::abs(x - y) <= 6; };
    return near(p.r, cr) && near(p.g, cg) && near(p.b, cb);
  };
  int painted = 0;
  for (int row = 14; row <= 130; ++row) {
    for (int col = 330; col <= 500; ++col) {
      if (row < 0 || row >= kH || col < 0 || col >= kW) continue;
      if (!isClear(px[static_cast<size_t>(row) * kW + col])) ++painted;
    }
  }
  std::printf("  minimap-corner painted (non-bg) pixels: %d\n", painted);

  bool ok = true;
  auto check = [&](bool cond, const char* msg) {
    std::printf("    [%s] %s\n", cond ? "PASS" : "FAIL", msg);
    ok = ok && cond;
  };
  check(visible, "minimap reports visible when content overflows viewport");
  check(painted > 20, "minimap corner has painted pixels (node rects / viewport)");
  std::printf("  >>> MINIMAP CASE %s\n", ok ? "PASSED" : "FAILED");
  return ok;
}

// Relationship-noodle source exit (device bug): with the source's whole-prim
// relationship target to the LEFT, the drawn curve must leave the source's
// right-edge port OUTWARD (paint just right of the port) and must NOT reappear
// below the row at the node's horizontal middle (which is where the pre-fix
// curve — handle aimed back into the node — ran, making the noodle read as
// "starting at the middle of the row"). Direct path (opacity 1.0) so the noodle
// is full intensity. Gates the process.
bool runRelationshipExitCase(ig::NoodlesGraphView& view, GLuint defaultFbo,
                             uint8_t cr, uint8_t cg, uint8_t cb) {
  std::printf("\n=== REL EXIT CASE: relationship noodle leaves the right-edge port ===\n");
  view.setClearColor(cr / 255.0f, cg / 255.0f, cb / 255.0f, 1.0f);
  view.setOverlayOpacity(1.0f);
  view.setViewportPan(0.0, 0.0);
  // These nodes are wide (relationship group + value rows); zoom out enough that
  // the source's right edge AND the outward handle bump (up to +120 world) stay
  // inside the 512² viewport.
  const double zoom = 0.22;
  view.setZoom(zoom);
  view.setDocument(MakeRelLeftDocument());

  // The source's relationship pin: its port circle is at the node's right edge
  // (centerX), on its row (centerY), both in world space.
  double sx = 0, sy = 0, sw = 0, sh = 0;
  view.nodePosition("/Rel/S", &sx, &sy);
  view.nodeSize("/Rel/S", &sw, &sh);
  double portX = sx + sw, portY = sy + sh * 0.5;  // fallback
  for (const ig::GraphPinInfo& p : view.nodePins("/Rel/S")) {
    if (p.isRelationship && p.isOutput) {
      portX = p.centerX;
      portY = p.centerY;
      break;
    }
  }
  std::printf("  source rect pos=(%.1f,%.1f) size=(%.1f,%.1f) port=(%.1f,%.1f)\n",
              sx, sy, sw, sh, portX, portY);

  glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
  glViewport(0, 0, kW, kH);
  view.renderFrame();
  std::vector<Rgba> px(static_cast<size_t>(kW) * kH);
  glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
  glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

  auto isClear = [&](const Rgba& p) {
    auto near = [](int x, int y) { return std::abs(x - y) <= 6; };
    return near(p.r, cr) && near(p.g, cg) && near(p.b, cb);
  };
  // Count painted (non-background) pixels in a WORLD-space window, projected to
  // the framebuffer (pan 0, contentScale 1: col = worldX*zoom, row = H - worldY*zoom).
  auto paintedInWorldWindow = [&](double wx0, double wy0, double wx1,
                                  double wy1) {
    const int c0 = static_cast<int>(wx0 * zoom), c1 = static_cast<int>(wx1 * zoom);
    const int rTop = kH - static_cast<int>(wy1 * zoom);
    const int rBot = kH - static_cast<int>(wy0 * zoom);
    int painted = 0;
    for (int row = rTop; row <= rBot; ++row) {
      if (row < 0 || row >= kH) continue;
      for (int col = c0; col <= c1; ++col) {
        if (col < 0 || col >= kW) continue;
        if (!isClear(px[static_cast<size_t>(row) * kW + col])) ++painted;
      }
    }
    return painted;
  };

  // (a) Just RIGHT of the right-edge port, on the row, OUTSIDE the node body:
  //     the outward-leaving noodle must paint here.
  const int rightOfPort =
      paintedInWorldWindow(portX + 10.0, portY - 18.0, portX + 70.0, portY + 18.0);
  // (b) At the node's horizontal MIDDLE, BELOW the node (outside the body): the
  //     fixed curve stays right/inside near the source, so nothing paints here.
  const double midX = sx + sw * 0.5;
  const int midBelow =
      paintedInWorldWindow(midX - 30.0, sy + sh + 20.0, midX + 30.0, sy + sh + 80.0);
  std::printf("  painted right-of-port=%d  mid-below-node=%d\n", rightOfPort,
              midBelow);

  bool ok = true;
  auto check = [&](bool cond, const char* msg) {
    std::printf("    [%s] %s\n", cond ? "PASS" : "FAIL", msg);
    ok = ok && cond;
  };
  check(rightOfPort > 0,
        "noodle paints just right of the source's right-edge port (leaves outward)");
  check(midBelow == 0,
        "no noodle below the node at its horizontal middle (not mid-row start)");
  std::printf("  >>> REL EXIT CASE %s\n", ok ? "PASSED" : "FAILED");
  return ok;
}

// The demos attach their document before the platform shell creates a real GL
// font atlas. Fallback text metrics are intentionally approximate, so this
// exact call order used to leave the final two demo nodes overlapping after
// initializeGL enlarged their rows. Gate the public fixture with real assets,
// then verify frameAll keeps every final node inside the requested padding.
bool runDemoRealFontLayoutCase(const std::string& assets,
                               const std::string& fontPng,
                               const std::string& fontJson,
                               GLuint defaultFbo) {
  std::printf("\n=== DEMO LAYOUT CASE: real-font relayout + frame-all ===\n");
  auto fixture = ig::examples::CreateDemoGraphFixture();
  glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
  fixture.editor->initializeGL(assets, fontPng, fontJson);
  fixture.editor->resize(kW, kH, 1.0f);

  const std::vector<std::string> ids{
      "/Demo/Noise", "/Demo/Grade", "/Demo/Mask", "/Demo/Display"};
  struct Rect { double x, y, w, h; };
  std::vector<Rect> rects;
  bool ok = true;
  for (const std::string& id : ids) {
    Rect r{};
    const bool found = fixture.editor->nodePosition(id, &r.x, &r.y) &&
                       fixture.editor->nodeSize(id, &r.w, &r.h);
    std::printf("  %s pos=(%.1f,%.1f) size=(%.1f,%.1f)\n", id.c_str(),
                r.x, r.y, r.w, r.h);
    ok = ok && found && r.w > 0.0 && r.h > 0.0;
    rects.push_back(r);
  }
  for (std::size_t i = 0; i < rects.size(); ++i) {
    for (std::size_t j = i + 1; j < rects.size(); ++j) {
      const Rect& a = rects[i];
      const Rect& b = rects[j];
      const bool overlaps = a.x < b.x + b.w && a.x + a.w > b.x &&
                            a.y < b.y + b.h && a.y + a.h > b.y;
      if (overlaps) {
        std::printf("    [FAIL] overlap: %s and %s\n", ids[i].c_str(),
                    ids[j].c_str());
      }
      ok = ok && !overlaps;
    }
  }

  constexpr double padding = 32.0;
  ok = fixture.editor->frameAll(padding) && ok;
  const double zoom = fixture.editor->zoom();
  const double panX = fixture.editor->panX();
  const double panY = fixture.editor->panY();
  constexpr double tolerance = 0.75;
  for (std::size_t i = 0; i < rects.size(); ++i) {
    const Rect& r = rects[i];
    const double left = (r.x - panX) * zoom;
    const double top = (r.y - panY) * zoom;
    const double right = (r.x + r.w - panX) * zoom;
    const double bottom = (r.y + r.h - panY) * zoom;
    const bool padded = left >= padding - tolerance &&
                        top >= padding - tolerance &&
                        right <= kW - padding + tolerance &&
                        bottom <= kH - padding + tolerance;
    if (!padded) {
      std::printf("    [FAIL] frame padding %s screen=(%.1f,%.1f)-(%.1f,%.1f)\n",
                  ids[i].c_str(), left, top, right, bottom);
    }
    ok = ok && padded;
  }
  fixture.editor->cleanupGL();
  std::printf("  >>> DEMO LAYOUT CASE %s\n", ok ? "PASSED" : "FAILED");
  return ok;
}

}  // namespace

int main() {
  std::printf("[ RUN  ] noodles_graph_view_render (host offscreen GL)\n");

  CGLContextObj ctx = nullptr;
  if (!makeContext(&ctx)) {
    std::printf("[ SKIP ] no offscreen GL context available on this host\n");
    return 0;  // environment skip, not a failure
  }

  GLuint defaultFbo = makeDefaultFbo();
  if (defaultFbo == 0) {
    std::printf("[ FAIL ] could not create default-substitute FBO\n");
    return 1;
  }

  const std::string assets = NOODLES_APPLE_TEST_ASSET_DIR;
  const std::string fontPng = assets + "/fonts/Poppins-Regular.png";
  const std::string fontJson = assets + "/fonts/Poppins-Regular.json";
  std::printf("  assets: %s\n", assets.c_str());

  ig::NoodlesGraphView view;
  view.setDocument(MakeDocument());
  std::printf("  model: nodes=%zu links=%zu\n", view.nodeCount(),
              view.linkCount());

  glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);  // current when GL objects init
  view.initializeGL(assets, fontPng, fontJson);
  drainGlErrors("initializeGL");
  view.resize(kW, kH, 1.0f);
  view.setViewportPan(0.0, 0.0);
  // Zoom out so the (large) laid-out nodes fit with genuine background around
  // them — otherwise a node fills the whole viewport and "links but no nodes"
  // can't be told apart from "everything painted".
  view.setZoom(kZoom);

  bool ok = true;
  FrameStats direct{}, composite{};
  ok &= runPass(view, defaultFbo, 1.0f, "direct (opaque, undimmed)", 20, 20, 24,
                &direct);
  ok &= runPass(view, defaultFbo, 0.5f, "composite (FBO path)", 20, 20, 24,
                &composite);

  std::printf("\n=== SUMMARY ===\n");
  std::printf("  direct    : node-interior painted %d/%d\n",
              direct.nodeInteriorNonBg, direct.nodeInterior);
  std::printf("  composite : node-interior painted %d/%d\n",
              composite.nodeInteriorNonBg, composite.nodeInterior);

  // Regression gate: text must follow a node drag.
  const bool dragOk = runDragCase(view, defaultFbo, 20, 20, 24);
  ok &= dragOk;

  // Minimap overview gate (item 4): visible + painted when content overflows.
  const bool minimapOk = runMinimapCase(view, defaultFbo, 20, 20, 24);
  ok &= minimapOk;

  // Relationship-noodle source-exit gate (device bug): the noodle leaves the
  // source's right-edge port outward, not across the middle of the row.
  const bool relExitOk = runRelationshipExitCase(view, defaultFbo, 20, 20, 24);
  ok &= relExitOk;

  // Public-demo initialization order, real-font layout, and framing gate.
  const bool demoLayoutOk =
      runDemoRealFontLayoutCase(assets, fontPng, fontJson, defaultFbo);
  ok &= demoLayoutOk;

  view.cleanupGL();
  CGLSetCurrentContext(nullptr);
  CGLDestroyContext(ctx);

  std::printf("%s\n", ok ? "[ DONE ] render regression suite complete"
                         : "[ FAIL ] render regression");
  return ok ? 0 : 1;
}
