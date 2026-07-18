// Silence the iOS OpenGLES deprecation warnings used by the GLES renderer.
// Must precede every noodles
// include — the render headers pull render/noodles_gl.h transitively.
#ifndef GLES_SILENCE_DEPRECATION
#  define GLES_SILENCE_DEPRECATION 1
#endif

#include <noodles/apple/GraphEditor.h>

// Noodles layout, hit-testing, and rendering stay private behind GraphEditor.
#include "core/GraphLayout.h"
#include "render/LinkGeometry.h"

#include "core/NodeData.h"
#include "core/RenderConfig.h"
#include "render/GraphNodeRenderer.h"
#include "render/GraphRenderer.h"
#include "render/NodeRenderManager.h"
#include "render/VertexGenerator.h"
#include "render/noodles_gl.h"
#include "spatial/SpatialIndex.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace noodles::apple {
namespace {

template <typename T, typename = void>
struct HasWholePrimRelationshipInset : std::false_type {};

template <typename T>
struct HasWholePrimRelationshipInset<
    T, std::void_t<decltype(T::insetWholePrimRelationshipTargets)>>
    : std::true_type {};

template <typename T>
void PreserveLegacyRelationshipEndpoints(T& config) {
  if constexpr (HasWholePrimRelationshipInset<T>::value) {
    config.insetWholePrimRelationshipTargets = false;
  }
}

// Incremental placement: a node that first appears in a refresh (no persisted
// or held position) and is added to an EXISTING laid-out
// graph is dropped to the right of a connected neighbour (or the graph's right
// edge) and pushed down until it clears every placed node by these margins.
// (A wholly unpositioned graph instead goes through noodles::layoutGraph.)
constexpr double kIncrementalGapX = 120.0;
constexpr double kIncrementalGapY = 60.0;

// Zoom clamp (parity with the current panels' pinch limits).
constexpr double kZoomMin = 0.25;
constexpr double kZoomMax = 4.0;

// ── Touch-ergonomics slop (finger/pencil sizing) ─────────────────────────────
// The default hit tolerances (0.5*linkLineWidth, config.nodePortWidth) are
// desktop-MOUSE numbers: a few WORLD units, which is only a few screen POINTS at
// the default zoom — impossible to land on with a fingertip (~44pt) or even a
// pencil on the noodle overlay. These constants restate the targets as VIEW
// POINTS and convert to world units at hit time (world = viewPt * contentScale /
// zoom, the inverse of computeProjection), so the on-screen target stays a fixed
// finger-friendly size regardless of pan/zoom. Only the miss-everything case
// reaches links (nodes are hit-tested first in pointerDown), so widening these
// cannot steal a node interaction.
//
//   • kLinkTouchSlopViewPts — half-width of the grab corridor around a noodle.
//     ~12pt each side ⇒ ~24pt-wide catch corridor, comfortably fingertip-sized.
//   • kBandTouchSlopViewPts — min width of each row's connect band (the outer
//     strip that starts a link drag). ~14pt so a fingertip reliably lands on it;
//     clamped below to ≤35% of the node width so the scrub/move middle survives
//     on narrow nodes.
//   • kPortTouchSlopViewPts — floor for the port pick RADIUS (drop target /
//     pencil routing). hitRadius() was world-constant, so a zoomed-out port
//     shrank on screen; this pins a ~20pt on-screen floor.
constexpr double kLinkTouchSlopViewPts = 12.0;
constexpr double kBandTouchSlopViewPts = 14.0;
constexpr double kPortTouchSlopViewPts = 20.0;

// Fraction of a node's width that the two connect bands may never jointly exceed
// (0.35 each side ⇒ ≥30% of the row width stays as a scrub/move middle).
constexpr double kBandMaxFracPerSide = 0.35;

// ── relationship target arrow ────────────────────────────────────────────────
// A whole-node relationship noodle terminates at a DOWN-POINTING arrowhead that
// sits on top of the target node: tip on the node's top-center, base (= the
// noodle end) kRelArrowHeight above it. World units, sized against the layout
// scale (nodePinFontSize 36 / nodePortWidth 40) so the arrow reads like a row
// glyph. The arrow doubles as a grab handle for the link's target end.
constexpr double kRelArrowHeight = 36.0;
constexpr double kRelArrowHalfWidth = 18.0;

// Headless font metrics: the FontAtlas defaults (see FontAtlas.h). Used for
// layout before initializeGL() so refresh + hit-testing are fully usable in
// unit tests without a GL context.
noodles::FontMetrics FallbackMetrics() {
  noodles::FontMetrics m;
  m.ascender = 0.8;
  m.descender = -0.2;
  m.lineHeight = 1.2;
  return m;
}

// A metrics-only text-width estimate: proportional to glyph count. Deterministic
// (no GL / font atlas), which is what the layout row-center assertions in the
// host test rely on.
double FallbackTextWidth(const std::string& text, double fontSize) {
  return static_cast<double>(text.size()) * fontSize * 0.5;
}

// The value separator drawn between a pin's type and its value ("float · 0.35").
// UTF-8 middle dot padded with spaces.
constexpr const char* kValueSep = " \xC2\xB7 ";

// %g-style numeric formatting: a captured value and a scrubbed value must read
// the same in the row.
std::string FormatScalar(double v, const std::string& typeToken) {
  if (typeToken == "bool") return v != 0.0 ? "on" : "off";
  char buf[64];
  if (typeToken == "int" || typeToken == "int64" || typeToken == "uint") {
    std::snprintf(buf, sizeof(buf), "%lld",
                  static_cast<long long>(std::llround(v)));
  } else {
    std::snprintf(buf, sizeof(buf), "%g", v);
  }
  return std::string(buf);
}

// The row's TYPE-slot text: "<type>" or "<type> · <value>". The pin NAME is
// never touched (it is the authoring / hit-test identity).
std::string ComposeTypeText(const GraphProperty& prop) {
  if (!prop.hasValue) return prop.type;
  return prop.type + kValueSep + prop.displayValue;
}

// Column-major orthographic projection, matching GraphRenderer::computeProjection
// (which is private): world [pan, pan + size/zoom] → NDC, Y-down. The render
// managers' *FromGraph entry points take this matrix directly.
void ComputeOrtho(double panX, double panY, double zoom, int vpW, int vpH,
                  float* out) {
  const double w = (zoom != 0.0) ? vpW / zoom : vpW;
  const double h = (zoom != 0.0) ? vpH / zoom : vpH;
  const double left = panX, right = panX + w;
  const double top = panY, bottom = panY + h;
  const double nearV = -1000.0, farV = 1000.0;
  for (int i = 0; i < 16; ++i) out[i] = 0.0f;
  out[0] = static_cast<float>(2.0 / (right - left));
  out[5] = static_cast<float>(2.0 / (top - bottom));
  out[10] = static_cast<float>(-2.0 / (farV - nearV));
  out[12] = static_cast<float>(-(right + left) / (right - left));
  out[13] = static_cast<float>(-(top + bottom) / (top - bottom));
  out[14] = static_cast<float>(-(farV + nearV) / (farV - nearV));
  out[15] = 1.0f;
}

}  // namespace

struct GraphEditor::Impl {
  std::shared_ptr<GraphDocument> document;
  GraphEditDelegate delegate;

  noodles::RenderConfig config;
  noodles::GraphModel model;
  noodles::SpatialIndex spatial;
  noodles::GraphRenderer renderer;       // GL-owning (initializeGL/renderFrame)
  noodles::DefaultNodeRenderer nodeRenderer{config};  // node-local geometry

  Impl() { PreserveLegacyRelationshipEndpoints(config); }

  bool glReady = false;
  int viewportW = 0;
  int viewportH = 0;
  float contentScale = 1.0f;

  double zoom = 1.0;
  double panX = 0.0;
  double panY = 0.0;

  std::string selectedNodeId;
  int nextZOrder = 0;
  uint32_t generation = 0;
  uint32_t minimapGen = 0;  // forces the minimap VBO re-upload every frame
  uint32_t relArrowGen = 0;  // forces the arrow VBO re-upload every frame

  // Node-quad / text rebuild gate for the render managers' *FromGraph path:
  // set on any topology / layout / selection / position / value change; a
  // pan/zoom-only frame leaves it false so the cached VBOs are reused.
  bool contentDirty = true;

  // Positions held across refreshes for nodes without a persisted position, so
  // a rebuild does not reshuffle the layout.
  std::unordered_map<std::string, noodles::Vec2d> heldPositions;
  // Subset of held positions created by automatic layout rather than authored
  // by the document/user. The first real-font layout may resize nodes, so these
  // positions can be reflowed once the GL font atlas is available without
  // moving explicitly positioned nodes.
  std::set<std::string> autoPositionedNodeIds;

  // ─── overlay compositing ──────────────────────────────────────────────────
  // Default is a fully TRANSPARENT clear: the editor composites over the canvas.
  std::array<float, 4> clearColor{0.0f, 0.0f, 0.0f, 0.0f};
  float overlayOpacity = 0.5f;
  bool valueScrubEnabled = true;

  // Offscreen pass: the graph renders into `fbo` (RGBA8 color + depth), then a
  // fullscreen quad composites it to the platform's default framebuffer.
  GLuint fbo = 0, fboColorTex = 0, fboDepthRb = 0;
  int fboW = 0, fboH = 0;
  GLuint quadVao = 0;
  std::unique_ptr<noodles::GLSLProgram> compositeProg;

  // ─── value rows (attribute current values, keyed by nodeId → pinName) ──────
  struct ValueRow {
    bool scrubable = false;
    double value = 0.0;
    std::string typeToken;  // "float"/"int"/"bool"/… drives cast + step
    std::string display;    // formatted display text
  };
  std::unordered_map<std::string, std::unordered_map<std::string, ValueRow>>
      valueRows;

  // ─── interaction state ────────────────────────────────────────────────────
  enum class Drag { None, Pan, Node, Link, Scrub, Minimap } drag = Drag::None;
  bool pointerMoved = false;
  double downViewX = 0.0, downViewY = 0.0;
  double lastViewX = 0.0, lastViewY = 0.0;
  double downWorldX = 0.0, downWorldY = 0.0;

  std::string dragNodeId;
  double grabOffsetX = 0.0, grabOffsetY = 0.0;

  // A pointerDown landed on a group-header caret row: a no-move tap toggles the
  // group's fold on pointerUp (a real drag still moves the node).
  std::string foldTapNode, foldTapGroup;

  // Scrub-to-edit a value row.
  std::string scrubNodeId, scrubPin, scrubTypeToken;
  double scrubValue0 = 0.0;   // value at gesture start (absolute delta base)
  double scrubCurrent = 0.0;  // last authored value
  bool scrubBegan = false;    // beginStageEdit fired for this gesture

  // ── link-authoring drag (Drag::Link) ──
  // One state machine drives every edge edit: a NEW connection begun from a port
  // or a row-edge band, and a RECONNECT begun by grabbing ("lifting") an existing
  // link. The ANCHOR endpoint stays put; the FREE end follows the pointer and, on
  // release, must land on a pin/node of the OPPOSITE side (linkAnchorIsOutput
  // decides which). Relationship edges route through AuthorEdge/RemoveEdge (target
  // = whole prim); attribute connections route through AuthorConnection/
  // RemoveConnection (target = an opposite-side pin).
  bool linkDragReconnect = false;   // lifted an existing link (rewire / delete)
  bool linkIsRelationship = false;  // relationship edge vs attribute connection
  std::string linkAnchorNode;       // the endpoint that stays fixed
  std::string linkAnchorPort;
  bool linkAnchorIsOutput = false;  // fixed endpoint's side
  // Exact world-space row edge captured when the gesture starts. Keeping the
  // anchor avoids re-resolving an unconnected input row as a nonexistent
  // output pin (whose fallback is the node's vertical midpoint).
  noodles::Vec2d linkAnchorWorld{0.0, 0.0};
  bool linkFromRelOutPin = false;   // tap on a rel output pin arms it (no move)
  noodles::LinkData linkLifted;     // reconnect: original link (restore on cancel)
  bool dragPreviewActive = false;   // a transient dangling link is in model.links

  // Relationship "tap a port, then tap a target node" arming (unchanged).
  std::string armedNodeId;
  std::string armedPort;

  bool pinching = false;
  // Zoom at pinchBegin: the gesture's `scale` is cumulative-since-begin, so the
  // live zoom is pinchStartZoom * scale — NOT the current zoom * scale, which
  // would compound every update (two 1.5× updates → 2.25×).
  double pinchStartZoom = 1.0;

  // Platform timeline frame (value scrub authors / captures here when animated).
  double displayFrame = 0.0;

  // The document observer may run on any thread. It queues plain changes and
  // the serialized editor thread applies them before rendering.
  GraphDocument::ObserverToken observerToken = 0;
  bool selfEditing = false;
  std::mutex pendingMutex;
  std::atomic<bool> pendingInfoDirty{false};
  std::atomic<bool> pendingStructural{false};
  std::vector<GraphDocumentChange> pendingInfoChanges;

  // Suppress the document's synchronous callbacks while the editor authors.
  struct SelfEditScope {
    Impl& s;
    explicit SelfEditScope(Impl& i) : s(i) { s.selfEditing = true; }
    ~SelfEditScope() { s.selfEditing = false; }
  };

  ~Impl() { unregisterDocumentObserver(); }

  // ── helpers ──
  noodles::FontMetrics metrics() {
    if (glReady) {
      noodles::FontMetrics m;
      m.ascender = renderer.fontAtlas().ascender();
      m.descender = renderer.fontAtlas().descender();
      m.lineHeight = renderer.fontAtlas().lineHeight();
      return m;
    }
    return FallbackMetrics();
  }

  noodles::TextWidthCallback textWidth() {
    if (glReady) {
      noodles::GraphRenderer* r = &renderer;
      return [r](const std::string& text, double fontSize) {
        return r->textManager().calculateTextWidth(text, fontSize);
      };
    }
    return &FallbackTextWidth;
  }

  void layoutAll() {
    noodles::FontMetrics m = metrics();
    noodles::TextWidthCallback cb = textWidth();
    for (auto& [id, node] : model.nodes) {
      model.layoutNode(node, cb, m, &config);
    }
  }

  // Re-run only automatically-owned placement after fallback metrics have
  // been replaced by the real font atlas. This closes the otherwise subtle
  // gap where headless node heights are used for initial stacking and the
  // larger rendered boxes overlap once initializeGL() re-lays them out.
  void reflowAutoPositionsForCurrentMetrics() {
    std::vector<std::string> ids;
    for (const std::string& id : autoPositionedNodeIds) {
      if (model.nodes.find(id) != model.nodes.end()) ids.push_back(id);
    }
    if (ids.empty()) return;
    if (ids.size() == model.nodes.size()) {
      layeredLayout();
      for (const std::string& id : ids) {
        heldPositions[id] = model.nodes.at(id).position;
      }
    } else {
      placeIncrementalNodes(ids);
    }
  }

  // Resolve every link's endpoints from the laid-out pin centers, exactly as the
  // reference graphView._setupLinkEndpoints does (source = output side, target =
  // input side; resolvePortPosition handles dual / relationship pins).
  void rebuildLinkGeometry() {
    for (noodles::LinkData& link : model.links) {
      auto sIt = model.nodes.find(link.sourceNodeId);
      auto tIt = model.nodes.find(link.targetNodeId);
      if (sIt != model.nodes.end()) {
        // A relationship noodle must START at the right-edge center of the ROW
        // it originates from (its own row, or the header row when folded) — not
        // an aggregate/synthetic anchor. Attribute connections resolve normally.
        link.start =
            link.isRelationship
                ? relationshipRowAnchor(sIt->second, link.sourcePort)
                : nodeRenderer.resolvePortPosition(sIt->second, link.sourcePort,
                                                   true);
      }
      if (tIt != model.nodes.end()) {
        if (link.isRelationship && link.targetPort.empty()) {
          // Whole-prim relationship target: the noodle terminates at the
          // TOP-MIDDLE of the arrowhead that sits on the node's top edge
          // (drawRelationshipArrows fills [end, end + kRelArrowHeight] with a
          // down-pointing triangle whose tip touches the node's top-center).
          const noodles::NodeData& n = tIt->second;
          link.end = noodles::Vec2d(n.position[0] + n.size[0] * 0.5,
                                    n.position[1] - kRelArrowHeight);
        } else {
          link.end = nodeRenderer.resolvePortPosition(tIt->second,
                                                      link.targetPort, false);
        }
      }
    }
    model.markLinksChanged();
  }

  void rebuildSpatialIndex() {
    spatial.Clear();
    for (auto& [id, node] : model.nodes) {
      spatial.InsertNode(id, noodles::SpatialIndex::ComputeNodeBounds(node));
    }
    for (int i = 0; i < static_cast<int>(model.links.size()); ++i) {
      spatial.InsertLink(i, noodles::SpatialIndex::ComputeLinkBounds(model.links[i]));
    }
  }

  // View points → graph-space world (inverse of computeProjection):
  //   world = pan + (viewPoint * contentScale) / zoom
  noodles::Vec2d viewToGraph(double vx, double vy) const {
    return noodles::Vec2d(panX + (vx * contentScale) / zoom,
                          panY + (vy * contentScale) / zoom);
  }

  double hitRadius() const {
    // Port pick radius. The layout width (config.nodePortWidth, world units) is
    // the floor; on top of it we enforce a VIEW-point floor (kPortTouchSlopViewPts
    // converted to world) so a zoomed-out port never shrinks below a fingertip on
    // screen. At contentScale 2 / zoom 1 the view floor is 40 world units, i.e.
    // exactly the old max(nodePortWidth,24) behaviour; zooming out now grows the
    // world radius to hold the on-screen size steady.
    return std::max(config.nodePortWidth, viewPtsToWorld(kPortTouchSlopViewPts));
  }

  // A length in VIEW points expressed in graph-space world units at the current
  // pan/zoom (inverse of computeProjection: world = viewPt * contentScale/zoom).
  double viewPtsToWorld(double viewPts) const {
    const double cs = contentScale > 0.0f ? contentScale : 1.0;
    return (viewPts * cs) / std::max(zoom, 1e-6);
  }

  // Curve-grab tolerance for a noodle, in world units: the larger of the
  // desktop line-half-width and a fingertip-sized view-point slop.
  double linkTouchTolerance() const {
    return std::max(0.5 * config.linkLineWidth,
                    viewPtsToWorld(kLinkTouchSlopViewPts));
  }

  // Per-side width (world units) of a node row's connect band: at least the
  // desktop 10% of the node width, at least a fingertip-sized view slop, but
  // never more than kBandMaxFracPerSide of the width so the scrub/move middle
  // always survives (both bands together ≤ 70% of the row).
  double bandTouchWidth(const noodles::NodeData& n) const {
    const double desktop = n.size[0] * 0.10;
    const double slop = viewPtsToWorld(kBandTouchSlopViewPts);
    return std::min(std::max(desktop, slop), n.size[0] * kBandMaxFracPerSide);
  }

  void selectNode(const std::string& id) {
    if (selectedNodeId == id) return;
    if (auto it = model.nodes.find(selectedNodeId); it != model.nodes.end()) {
      it->second.selected = false;
    }
    selectedNodeId = id;
    if (auto it = model.nodes.find(id); it != model.nodes.end()) {
      it->second.selected = true;
      it->second.zOrder = ++nextZOrder;  // bring selection to front
    }
    contentDirty = true;
    if (delegate.selectionChanged) delegate.selectionChanged(id);
  }

  // Topmost node whose box contains the world point, via the spatial index.
  std::string nodeAt(const noodles::Vec2d& world) const {
    std::vector<std::string> nodeIds;
    std::vector<int> linkIdx;
    spatial.QueryPoint(world, nodeIds, linkIdx);
    std::string best;
    int bestZ = -1;
    for (const std::string& id : nodeIds) {
      auto it = model.nodes.find(id);
      if (it == model.nodes.end()) continue;
      if (!noodles::SpatialIndex::ComputeNodeBounds(it->second).Contains(world)) {
        continue;
      }
      if (it->second.zOrder >= bestZ) {
        bestZ = it->second.zOrder;
        best = id;
      }
    }
    return best;
  }

  // True when (nodeId, pin, side) is a relationship pin (vs a plain attribute).
  bool isRelationshipPin(const std::string& nodeId, const std::string& pin,
                         bool isOutput) const {
    auto it = model.nodes.find(nodeId);
    if (it == model.nodes.end()) return false;
    const noodles::NodeData& n = it->second;
    return isOutput ? n.relationshipOutputPins.count(pin) > 0
                    : n.relationshipInputPins.count(pin) > 0;
  }

  // The index of the whole-prim relationship link whose target ARROW contains
  // `world` (with a fingertip slop), or -1. The arrow spans from the noodle end
  // (link.end, the arrow's top-middle) down to the target node's top edge, so
  // this region sits ABOVE the node box and never shadows a row interaction.
  int relationshipArrowAt(const noodles::Vec2d& world) const {
    const double slop = viewPtsToWorld(kPortTouchSlopViewPts * 0.5);
    for (int i = 0; i < static_cast<int>(model.links.size()); ++i) {
      const noodles::LinkData& l = model.links[i];
      if (l.isDangling || !l.isRelationship || !l.targetPort.empty()) continue;
      const double cx = l.end[0];
      const double top = l.end[1];
      // No slop BELOW the tip: the tip touches the target's top edge, and
      // bleeding into the node would steal title-row taps on any node with an
      // inbound relationship arrow.
      if (world[0] >= cx - kRelArrowHalfWidth - slop &&
          world[0] <= cx + kRelArrowHalfWidth + slop && world[1] >= top - slop &&
          world[1] <= top + kRelArrowHeight) {
        return i;
      }
    }
    return -1;
  }

  // The pin whose laid-out row (node-LOCAL center ± half a row) contains localY,
  // searching the input rows then the output rows. Sets *isRel when the row is a
  // relationship pin. Returns "" when localY is not on any row.
  std::string rowPinAt(const noodles::NodeData& n, double localY,
                       bool* isRel, bool* isOutput = nullptr) const {
    if (isRel) *isRel = false;
    if (isOutput) *isOutput = false;
    const std::string in = inputRowAt(n, localY);
    if (!in.empty()) {
      if (isRel) *isRel = n.relationshipInputPins.count(in) > 0;
      return in;
    }
    const double half = std::max(n.layoutPortLineHeight, 1.0) * 0.5;
    const std::size_t count =
        std::min(n.outputPins.size(), n.layoutOutputCenterY.size());
    for (std::size_t i = 0; i < count; ++i) {
      if (std::abs(localY - n.layoutOutputCenterY[i]) <= half) {
        if (isRel) *isRel = n.relationshipOutputPins.count(n.outputPins[i]) > 0;
        if (isOutput) *isOutput = true;
        return n.outputPins[i];
      }
    }
    return std::string();
  }

  // Display-row kind of `pin` on node `n` (0=normal 1=folded-header
  // 2=unfolded-header 3=child), or -1 when the pin is unknown / the side has no
  // authored kinds (which the layout treats as all-normal).
  int rowKindOf(const noodles::NodeData& n, const std::string& pin) const {
    if (pin.empty()) return -1;
    for (std::size_t i = 0;
         i < n.inputPins.size() && i < n.inputRowKinds.size(); ++i) {
      if (n.inputPins[i] == pin) return n.inputRowKinds[i];
    }
    for (std::size_t i = 0;
         i < n.outputPins.size() && i < n.outputRowKinds.size(); ++i) {
      if (n.outputPins[i] == pin) return n.outputRowKinds[i];
    }
    return -1;
  }

  // The model link index under `world` (curve-accurate, via the broad-phase
  // spatial index then LinkGeometry::findLinkUnderCursor), or -1. Skips the
  // transient drag preview link.
  int linkAt(const noodles::Vec2d& world) const {
    const double tol = linkTouchTolerance();
    // Broad-phase over a tol-sized box (not the bare point) so a link whose
    // padded bounds sit just outside the touch point is still a candidate for the
    // precise curve test below; otherwise the fattened tolerance would be capped
    // by the point-only query.
    std::vector<std::string> nodeIds;
    std::vector<int> linkIdx;
    spatial.QueryRegion(touchQueryBox(world, tol), nodeIds, linkIdx);
    if (linkIdx.empty()) return -1;
    std::vector<std::pair<noodles::Vec2d, noodles::Vec2d>> eps;
    std::vector<int> idxMap;
    for (int idx : linkIdx) {
      if (idx < 0 || idx >= static_cast<int>(model.links.size())) continue;
      if (model.links[idx].isDangling) continue;
      eps.emplace_back(model.links[idx].start, model.links[idx].end);
      idxMap.push_back(idx);
    }
    if (eps.empty()) return -1;
    const int hit = noodles::LinkGeometry::findLinkUnderCursor(world, eps, tol);
    return hit >= 0 ? idxMap[hit] : -1;
  }

  // A square broad-phase query region of half-extent `half` (world units) about
  // `world`, so the spatial index returns every item within the touch slop.
  static noodles::Range2d touchQueryBox(const noodles::Vec2d& world,
                                        double half) {
    return noodles::Range2d(
        noodles::Vec2d(world[0] - half, world[1] - half),
        noodles::Vec2d(world[0] + half, world[1] + half));
  }

  // A resolved drop endpoint on the requested side (port-accurate, then a
  // row-band fallback so a drop anywhere on the correct row still connects).
  struct EndpointHit {
    bool found = false;
    std::string nodeId;
    std::string port;
    bool isOutput = false;
  };
  EndpointHit endpointAt(const noodles::Vec2d& world, bool wantOutput) const {
    EndpointHit r;
    const noodles::NodeRenderManager::PortHit port =
        noodles::NodeRenderManager::portAtPoint(model, world, hitRadius(),
                                                nullptr);
    if (port.found && port.isOutput == wantOutput) {
      r.found = true;
      r.nodeId = port.nodeId;
      r.port = port.portName;
      r.isOutput = wantOutput;
      return r;
    }
    const std::string nid = nodeAt(world);
    if (nid.empty()) return r;
    const noodles::NodeData& n = model.nodes.at(nid);
    const double localY = world[1] - n.position[1];
    bool isRel = false;
    bool rowIsOutput = false;
    const std::string pin = rowPinAt(n, localY, &isRel, &rowIsOutput);
    if (pin.empty()) return r;
    // Explicit output rows are output-only. Legacy input rows remain usable
    // from the right edge as data sources for backward compatibility.
    if (!wantOutput && rowIsOutput) return r;
    r.found = true;
    r.nodeId = nid;
    r.port = pin;
    r.isOutput = wantOutput;
    return r;
  }

  // ── link-drag lifecycle ──
  void clearLinkDragState() {
    linkDragReconnect = false;
    linkIsRelationship = false;
    linkAnchorNode.clear();
    linkAnchorPort.clear();
    linkAnchorIsOutput = false;
    linkAnchorWorld = noodles::Vec2d(0.0, 0.0);
    linkFromRelOutPin = false;
    linkLifted = noodles::LinkData();
  }

  // A transient dangling link (empty node ids so rebuildLinkGeometry never
  // clobbers its cursor-driven end) kept LAST in model.links for the drag
  // preview. Colored green (valid drop) / red (invalid).
  void beginDragPreview() {
    noodles::LinkData preview;
    preview.isDangling = true;
    preview.isRelationship = linkIsRelationship;
    // A render may occur between pointerDown and the first pointerMove. Start
    // as a zero-length link at the real anchor instead of flashing at (0,0).
    preview.start = linkAnchorWorld;
    preview.end = linkAnchorWorld;
    model.links.push_back(preview);
    dragPreviewActive = true;
  }
  void updateDragPreview(const noodles::Vec2d& cursor, bool valid) {
    if (!dragPreviewActive || model.links.empty()) return;
    noodles::LinkData& L = model.links.back();
    if (linkAnchorIsOutput) {
      L.start = linkAnchorWorld;
      L.end = cursor;
    } else {
      L.end = linkAnchorWorld;
      L.start = cursor;
    }
    L.isRelationship = linkIsRelationship;
    L.hasColor = true;
    L.color = valid ? std::array<float, 4>{0.3f, 0.9f, 0.3f, 1.0f}
                    : std::array<float, 4>{0.9f, 0.3f, 0.3f, 1.0f};
    model.markLinksChanged();
  }
  // Remove the preview. When a buildModel() follows it rebuilds model.links from
  // scratch, so `pop` can be false; on a cancel path (no rebuild) pass true.
  void clearDragPreview(bool pop) {
    if (dragPreviewActive && pop && !model.links.empty() &&
        model.links.back().isDangling) {
      model.links.pop_back();
    }
    dragPreviewActive = false;
  }

  // Start a NEW connection from a port / row-edge band.
  void beginNewLinkDrag(const std::string& nodeId, const std::string& port,
                        bool isOutput) {
    armedNodeId.clear();
    armedPort.clear();
    selectNode(nodeId);
    drag = Drag::Link;
    linkDragReconnect = false;
    linkAnchorNode = nodeId;
    linkAnchorPort = port;
    linkAnchorIsOutput = isOutput;
    if (auto it = model.nodes.find(nodeId); it != model.nodes.end()) {
      linkAnchorWorld = rowEdgeAnchor(it->second, port, isOutput);
    } else {
      linkAnchorWorld = noodles::Vec2d(0.0, 0.0);
    }
    linkIsRelationship = isRelationshipPin(nodeId, port, isOutput);
    linkFromRelOutPin = linkIsRelationship && isOutput;
    linkLifted = noodles::LinkData();
    beginDragPreview();
    requestRender();
  }

  // Lift an existing link: remove it from the model (visual), anchor the end
  // FARTHER from the grab point, and dangle the nearer end from the pointer.
  void beginLinkReconnect(int linkIndex, const noodles::Vec2d& grab) {
    const noodles::LinkData L = model.links[linkIndex];
    auto dist2 = [](const noodles::Vec2d& a, const noodles::Vec2d& b) {
      const double dx = a[0] - b[0], dy = a[1] - b[1];
      return dx * dx + dy * dy;
    };
    const bool grabbedOutputEnd =
        dist2(grab, L.start) <= dist2(grab, L.end);  // start = output side

    drag = Drag::Link;
    linkDragReconnect = true;
    linkIsRelationship = L.isRelationship;
    linkFromRelOutPin = false;
    linkLifted = L;
    if (grabbedOutputEnd) {
      linkAnchorNode = L.targetNodeId;  // anchor the input side
      linkAnchorPort = L.targetPort;
      linkAnchorIsOutput = false;
      linkAnchorWorld = L.end;
    } else {
      linkAnchorNode = L.sourceNodeId;  // anchor the output side
      linkAnchorPort = L.sourcePort;
      linkAnchorIsOutput = true;
      linkAnchorWorld = L.start;
    }

    model.links.erase(model.links.begin() + linkIndex);
    model.buildConnectionCache();
    rebuildSpatialIndex();  // link indices shifted
    contentDirty = true;
    beginDragPreview();
    requestRender();
  }

  // Restore the lifted link into the model (cancel path — the document was never
  // touched).
  void restoreLiftedLink() {
    model.links.push_back(linkLifted);
    model.buildConnectionCache();
    rebuildLinkGeometry();
    rebuildSpatialIndex();
    contentDirty = true;
  }

  // Perform a batch of document edits under ONE begin/end envelope + self-edit
  // guard (so the reconnect remove+author pair notifies once). `fn` returns
  // whether the document changed.
  bool documentEditBatch(const std::function<bool()>& fn,
                         bool topologyEdit = false) {
    if (!document) return false;
    if (delegate.beginEdit) delegate.beginEdit();
    bool changed;
    { SelfEditScope guard(*this); changed = fn(); }
    if (changed && topologyEdit && delegate.topologyEdited) {
      delegate.topologyEdited();
    }
    if (delegate.endEdit) delegate.endEdit();
    return changed;
  }

  // Remove the lifted link's edge from the document (reconnect delete side).
  bool removeLiftedEdge() {
    if (linkLifted.isRelationship) {
      return document->removeRelationship(linkLifted.sourceNodeId,
                                          linkLifted.sourcePort,
                                          linkLifted.targetNodeId);
    }
    // Attribute connection lives ON the input side (link target), pointing to
    // the output side (link source).
    return document->removeConnection(linkLifted.targetNodeId,
                                      linkLifted.targetPort,
                                      linkLifted.sourceNodeId,
                                      linkLifted.sourcePort);
  }

  // After an edge remove / re-author, pin both former endpoints by persisting
  // their current canvas positions. A positioned node remains visible after
  // disconnecting its last edge, so it no longer disappears from the
  // canvas (explicit removal is the title-row long-press). Must run inside the
  // surrounding documentEditBatch (same envelope + self-edit guard).
  void persistLiftedEndpointPositions() {
    for (const std::string* id :
         {&linkLifted.sourceNodeId, &linkLifted.targetNodeId}) {
      auto it = model.nodes.find(*id);
      if (it == model.nodes.end()) continue;
      const noodles::Vec2d p = it->second.position;
      document->setNodePosition(*id, p[0], p[1]);
      heldPositions[*id] = p;
      autoPositionedNodeIds.erase(*id);
    }
  }

  // Long-press node removal: sever every edge that touches the node and clear
  // its persisted canvas position, so the document no longer surfaces it. The
  // backing object is NEVER deleted — this is a graph-view removal, not a model
  // delete. One begin/end envelope (one undo step). Neighbours that may have
  // just lost their last edge are pinned in place with an authored position so
  // only the long-pressed node leaves the canvas.
  bool removeNodeFromCanvas(const std::string& id) {
    if (!document) return false;
    std::vector<noodles::LinkData> touching;
    for (const noodles::LinkData& l : model.links) {
      if (l.isDangling) continue;
      if (l.sourceNodeId == id || l.targetNodeId == id) touching.push_back(l);
    }
    const bool changed = documentEditBatch([&] {
      bool c = false;
      for (const noodles::LinkData& l : touching) {
        if (l.isRelationship) {
          c = document->removeRelationship(l.sourceNodeId, l.sourcePort,
                                           l.targetNodeId) ||
              c;
        } else {
          c = document->removeConnection(l.targetNodeId, l.targetPort,
                                         l.sourceNodeId, l.sourcePort) ||
              c;
        }
        const std::string& other =
            l.sourceNodeId == id ? l.targetNodeId : l.sourceNodeId;
        auto it = model.nodes.find(other);
        if (it != model.nodes.end()) {
          document->setNodePosition(other, it->second.position[0],
                                    it->second.position[1]);
          heldPositions[other] = it->second.position;
          autoPositionedNodeIds.erase(other);
        }
      }
      c = document->clearNodePosition(id) || c;
      return c;
    }, /*topologyEdit=*/true);
    heldPositions.erase(id);
    if (selectedNodeId == id) {
      if (auto it = model.nodes.find(id); it != model.nodes.end()) {
        it->second.selected = false;
      }
      selectedNodeId.clear();
      if (delegate.selectionChanged) delegate.selectionChanged(std::string());
    }
    buildModel();
    requestRender();
    return changed;
  }

  void status(const std::string& msg) {
    if (delegate.status) delegate.status(msg);
  }

  void requestRender() {
    if (delegate.requestRender) delegate.requestRender();
  }

  // Move a node's rendered geometry live by steering its accumulated offset in
  // the shared NodeTransformFrame. The node-quad, text, icon and port shaders
  // all add uNodeTransforms[slot] to geometry baked at the node's (frozen) base
  // frame, so this moves EVERY layer of the node in lockstep without any
  // regeneration — the reference "bake once, move via the transform texture"
  // path. Crucially the text path is cached and never re-lays-out on a
  // position-only change (its layout signature omits position), so the offset is
  // the ONLY mechanism that makes glyphs follow a drag. The GraphModel snapshot
  // (used for hit-testing, links and port picking) is updated by the caller.
  void moveNodeLive(const std::string& id, const noodles::Vec2d& livePos) {
    noodles::NodeTransformFrame& tf = renderer.transformFrame();
    if (tf.shaderIndex(id) < 0.0f) {
      // Not yet baked into the frame (e.g. a node dragged before its first
      // render): fall back to a full content rebuild, which assigns it a slot
      // and bakes it at its live position.
      contentDirty = true;
      return;
    }
    const noodles::Vec2d base = tf.basePosition(id);
    const noodles::Vec2d cur = tf.offset(id);
    // translate() is additive; steer the accumulated offset to the absolute
    // (live − base) target so re-dragging a node never drifts.
    tf.translate(id, (livePos[0] - base[0]) - cur[0],
                 (livePos[1] - base[1]) - cur[1]);
  }

  // ── document authoring, always bracketed by the delegate ──
  bool authorEdge(const std::string& src, const std::string& rel,
                  const std::string& tgt) {
    return documentEditBatch(
        [&] { return document->authorRelationship(src, rel, tgt); },
        /*topologyEdit=*/true);
  }

  bool removeEdge(const std::string& src, const std::string& rel,
                  const std::string& tgt) {
    return documentEditBatch(
        [&] { return document->removeRelationship(src, rel, tgt); },
        /*topologyEdit=*/true);
  }

  void authorPosition(const std::string& id, double x, double y) {
    if (!document) return;
    if (delegate.beginEdit) delegate.beginEdit();
    {
      SelfEditScope guard(*this);
      if (document->setNodePosition(id, x, y)) {
        autoPositionedNodeIds.erase(id);
      }
    }
    if (delegate.endEdit) delegate.endEdit();
  }

  // ── value-row layout / scrub ──
  // The input pin whose laid-out row (node-LOCAL center ± half a row) contains
  // localY, or "" when localY is not on any input row.
  std::string inputRowAt(const noodles::NodeData& n, double localY) const {
    const double half = std::max(n.layoutPortLineHeight, 1.0) * 0.5;
    const std::size_t count =
        std::min(n.inputPins.size(), n.layoutInputCenterY.size());
    for (std::size_t i = 0; i < count; ++i) {
      if (std::abs(localY - n.layoutInputCenterY[i]) <= half) {
        return n.inputPins[i];
      }
    }
    return std::string();
  }

  // Re-lay-out a single node (its type-slot text changed) and refresh the
  // dependent spatial bounds + link endpoints. Cheaper than a full buildModel
  // and stable enough for per-move scrubbing.
  void layoutNodeOne(noodles::NodeData& node) {
    model.layoutNode(node, textWidth(), metrics(), &config);
    spatial.UpdateNode(node.id, noodles::SpatialIndex::ComputeNodeBounds(node));
    rebuildLinkGeometry();
    contentDirty = true;
  }

  // Author `newVal` to the scrubbed attribute, bracketing the whole gesture in
  // ONE begin/endStageEdit (begin fires lazily on the first authored change;
  // end fires in pointerUp). Updates the row display + fires the status line.
  void applyScrub(double newVal) {
    if (!document) return;
    if (!scrubBegan) {
      if (delegate.beginEdit) delegate.beginEdit();
      scrubBegan = true;
    }
    bool changed;
    {
      SelfEditScope guard(*this);
      changed = document->setAttributeValue(scrubNodeId, scrubPin, newVal,
                                            displayFrame);
    }
    scrubCurrent = newVal;
    if (changed) {
      if (delegate.attributeEdited) {
        delegate.attributeEdited(scrubNodeId, scrubPin, /*live=*/true);
      }
    }

    auto vn = valueRows.find(scrubNodeId);
    if (vn != valueRows.end()) {
      auto vr = vn->second.find(scrubPin);
      if (vr != vn->second.end()) {
        vr->second.value = newVal;
        vr->second.display = FormatScalar(newVal, vr->second.typeToken);
        auto it = model.nodes.find(scrubNodeId);
        if (it != model.nodes.end()) {
          it->second.originalInputPinTypes[scrubPin] =
              vr->second.typeToken + kValueSep + vr->second.display;
          layoutNodeOne(it->second);
        }
      }
    }
    status(scrubPin + " = " + FormatScalar(newVal, scrubTypeToken));
    requestRender();
  }

  // ── overlay compositing ──
  bool overlayComposite() const {
    // Direct path only when the graph is fully opaque AND undimmed; otherwise
    // route through the FBO so the coverage alpha is corrected for the canvas.
    return !(clearColor[3] >= 0.999f && overlayOpacity >= 0.999f);
  }

  void ensureFbo() {
    if (fbo && fboW == viewportW && fboH == viewportH) return;
    if (!fbo) glGenFramebuffers(1, &fbo);
    if (!fboColorTex) glGenTextures(1, &fboColorTex);
    if (!fboDepthRb) glGenRenderbuffers(1, &fboDepthRb);

    glBindTexture(GL_TEXTURE_2D, fboColorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, viewportW, viewportH, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindRenderbuffer(GL_RENDERBUFFER, fboDepthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, viewportW,
                          viewportH);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           fboColorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, fboDepthRb);
    fboW = viewportW;
    fboH = viewportH;

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
  }

  void buildCompositeProgram() {
#if NOODLES_GLES
    const std::string ver = "#version 300 es\n";
    const std::string prec = "precision highp float;\n";
#else
    const std::string ver = "#version 330 core\n";
    const std::string prec;
#endif
    // Fullscreen triangle from gl_VertexID (no VBO); a bound VAO is still
    // required by GL 3.3 core, so `quadVao` (empty) stays bound at draw.
    const std::string vs = ver +
        "out vec2 vUV;\n"
        "void main(){\n"
        "  float x=(gl_VertexID==1)?3.0:-1.0;\n"
        "  float y=(gl_VertexID==2)?3.0:-1.0;\n"
        "  vUV=vec2(x,y)*0.5+0.5;\n"
        "  gl_Position=vec4(x,y,0.0,1.0);\n"
        "}\n";
    // The FBO holds premultiplied RGB (straight-over onto a transparent clear
    // gives correct premultiplied color) but a SQUARED coverage alpha, because
    // noodles blends GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA into a transparent
    // target (a*a for single coverage). sqrt() reconstructs the coverage (exact
    // for the common single-layer pixel; near-exact where opaque nodes overlap).
    // Multiplying the whole premultiplied texel by uOpacity dims uniformly.
    const std::string fs = ver + prec +
        "in vec2 vUV;\n"
        "uniform sampler2D uTex;\n"
        "uniform float uOpacity;\n"
        "out vec4 frag;\n"
        "void main(){\n"
        "  vec4 t=texture(uTex,vUV);\n"
        "  float a=sqrt(clamp(t.a,0.0,1.0));\n"
        "  frag=vec4(t.rgb,a)*uOpacity;\n"
        "}\n";
    compositeProg = std::make_unique<noodles::GLSLProgram>(
        vs, fs, std::vector<std::string>{"uTex", "uOpacity"});
  }

  void compositeOverlay(GLuint targetFbo) {
    if (!compositeProg) buildCompositeProgram();
    if (!quadVao) glGenVertexArrays(1, &quadVao);

    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    glViewport(0, 0, viewportW, viewportH);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    // The full-viewport quad OWNS every pixel of our dedicated overlay surface,
    // and its output is already premultiplied → overwrite (no blend) is correct
    // and avoids a double-composite before the OS window compositor.
    glDisable(GL_BLEND);

    compositeProg->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fboColorTex);
    glUniform1i(compositeProg->getUniformLocation("uTex"), 0);
    glUniform1f(compositeProg->getUniformLocation("uOpacity"), overlayOpacity);

    glBindVertexArray(quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
    compositeProg->release();
  }

  void releaseCompositeGL() {
    if (fbo) { glDeleteFramebuffers(1, &fbo); fbo = 0; }
    if (fboColorTex) { glDeleteTextures(1, &fboColorTex); fboColorTex = 0; }
    if (fboDepthRb) { glDeleteRenderbuffers(1, &fboDepthRb); fboDepthRb = 0; }
    if (quadVao) { glDeleteVertexArrays(1, &quadVao); quadVao = 0; }
    compositeProg.reset();
    fboW = fboH = 0;
  }

  // ── port highlights (item 1) ──
  // The interaction state the port-highlight producer reads. The host has no
  // pointer HOVER (touch/stylus), so hover stays unset; an in-progress link
  // drag lights its source relationship port. Every CONNECTED port still draws
  // its circle from this state regardless of hover — that is what makes the
  // port indicators visible at all.
  noodles::PortHighlightState portHighlightState() const {
    noodles::PortHighlightState st;
    if (drag == Drag::Link && linkAnchorIsOutput) {
      st.draggingLink = true;
      st.dragSourceNodeId = linkAnchorNode;
      st.dragSourcePort = linkAnchorPort;
      st.dragSourceIsOutput = true;
    }
    return st;
  }

  // ── hit routing (item 5, hitsGraphElementAt) ──
  bool hitsGraphElementAt(double vx, double vy) const {
    // The minimap owns its corner while visible (pencil/stylus routes to it).
    if (minimapContains(computeMinimap(), vx, vy)) return true;
    const noodles::Vec2d world = viewToGraph(vx, vy);
    if (relationshipArrowAt(world) >= 0) return true;
    if (!nodeAt(world).empty()) return true;
    if (noodles::NodeRenderManager::portAtPoint(model, world, hitRadius(),
                                                nullptr)
            .found) {
      return true;
    }
    const double tol = linkTouchTolerance();
    std::vector<std::string> nodeIds;
    std::vector<int> linkIdx;
    spatial.QueryRegion(touchQueryBox(world, tol), nodeIds, linkIdx);
    if (!linkIdx.empty()) {
      std::vector<std::pair<noodles::Vec2d, noodles::Vec2d>> eps;
      eps.reserve(linkIdx.size());
      for (int idx : linkIdx) {
        if (idx >= 0 && idx < static_cast<int>(model.links.size())) {
          eps.emplace_back(model.links[idx].start, model.links[idx].end);
        }
      }
      if (noodles::LinkGeometry::findLinkUnderCursor(world, eps, tol) >= 0) {
        return true;
      }
    }
    return false;
  }

  // ── autolayout (item 3) ──
  // Nudge one candidate down until its (margin-padded) box clears every already
  // PLACED node (pending/unplaced nodes are ignored — they still sit at the
  // placeholder origin).
  noodles::Vec2d resolveNoOverlap(const std::string& id,
                                  const noodles::Vec2d& start,
                                  const std::set<std::string>& pending) const {
    const noodles::NodeData& n = model.nodes.at(id);
    noodles::Vec2d p = start;
    bool moved = true;
    int guard = 0;
    while (moved && guard++ < 10000) {
      moved = false;
      for (const auto& [oid, on] : model.nodes) {
        if (oid == id || pending.count(oid)) continue;
        const bool overlapX =
            p[0] < on.position[0] + on.size[0] + kIncrementalGapX &&
            on.position[0] < p[0] + n.size[0] + kIncrementalGapX;
        const bool overlapY =
            p[1] < on.position[1] + on.size[1] + kIncrementalGapY &&
            on.position[1] < p[1] + n.size[1] + kIncrementalGapY;
        if (overlapX && overlapY) {
          p[1] = on.position[1] + on.size[1] + kIncrementalGapY;
          moved = true;
        }
      }
    }
    return p;
  }

  // Place each newly-appeared node relative to the existing (already-positioned)
  // graph without moving any placed node: anchor to a connected placed neighbour
  // (right of it), else to the graph's right edge, then push clear of overlaps.
  void placeIncrementalNodes(const std::vector<std::string>& ids) {
    std::set<std::string> pending(ids.begin(), ids.end());
    for (const std::string& id : ids) {
      auto it = model.nodes.find(id);
      if (it == model.nodes.end()) {
        pending.erase(id);
        continue;
      }
      noodles::NodeData& n = it->second;

      // A connected, already-placed neighbour (smallest id = deterministic).
      std::string anchorId;
      for (const noodles::LinkData& link : model.links) {
        std::string other;
        if (link.sourceNodeId == id) other = link.targetNodeId;
        else if (link.targetNodeId == id) other = link.sourceNodeId;
        else continue;
        if (other.empty() || pending.count(other)) continue;
        if (model.nodes.find(other) == model.nodes.end()) continue;
        if (anchorId.empty() || other < anchorId) anchorId = other;
      }

      noodles::Vec2d target;
      if (!anchorId.empty()) {
        const noodles::NodeData& a = model.nodes.at(anchorId);
        target = noodles::Vec2d(a.position[0] + a.size[0] + kIncrementalGapX,
                                a.position[1]);
      } else {
        double maxRight = 0.0, minTop = 0.0;
        bool any = false;
        for (const auto& [oid, on] : model.nodes) {
          if (pending.count(oid)) continue;
          const double right = on.position[0] + on.size[0];
          maxRight = any ? std::max(maxRight, right) : right;
          minTop = any ? std::min(minTop, on.position[1]) : on.position[1];
          any = true;
        }
        target = any ? noodles::Vec2d(maxRight + kIncrementalGapX, minTop)
                     : noodles::Vec2d(0.0, 0.0);
      }

      n.position = resolveNoOverlap(id, target, pending);
      heldPositions[id] = n.position;
      pending.erase(id);  // now placed; a later pending node may anchor to it
    }
  }

  // ── layered auto-layout (item 1) ──
  // Host-side layered (Sugiyama-lite) layout over ALL links (data-flow AND
  // relationship), used when a wholly-unpositioned graph is first shown. The
  // noodles::layoutGraph deliberately excludes relationship links from
  // every structural phase, so a relationship-connected graph — the common shape
  // in relationship-heavy models — collapses to a single column stacked in Y.
  // This pass ranks
  // over EVERY link by source→target direction (link source = output side,
  // target = input side): X = longest-path rank from the roots (sources left,
  // consumers right); within a rank, siblings are ordered by their predecessors'
  // barycenter and stacked vertically; separate connected components are offset
  // in Y so they never overlap. Results are HELD only — never authored to the
  // document (mirrors layoutGraph's contract).
  void layeredLayout() {
    if (model.nodes.empty()) return;
    constexpr double gapX = 120.0;
    constexpr double gapY = 60.0;
    constexpr double componentGapY = 200.0;

    // Directed adjacency (deduped) + undirected neighbours, over real links.
    std::unordered_map<std::string, std::vector<std::string>> outAdj, inAdj, undir;
    std::set<std::pair<std::string, std::string>> seenEdge;
    for (const noodles::LinkData& link : model.links) {
      const std::string& s = link.sourceNodeId;
      const std::string& t = link.targetNodeId;
      if (s.empty() || t.empty() || s == t) continue;
      if (model.nodes.find(s) == model.nodes.end()) continue;
      if (model.nodes.find(t) == model.nodes.end()) continue;
      undir[s].push_back(t);
      undir[t].push_back(s);
      if (seenEdge.insert({s, t}).second) {
        outAdj[s].push_back(t);
        inAdj[t].push_back(s);
      }
    }
    static const std::vector<std::string> kEmpty;
    const auto adjOf =
        [](const std::unordered_map<std::string, std::vector<std::string>>& m,
           const std::string& id) -> const std::vector<std::string>& {
      auto it = m.find(id);
      return it == m.end() ? kEmpty : it->second;
    };

    std::vector<std::string> ids;
    ids.reserve(model.nodes.size());
    for (const auto& [id, n] : model.nodes) ids.push_back(id);
    std::sort(ids.begin(), ids.end());

    std::set<std::string> visited;
    double componentY = 0.0;
    for (const std::string& start : ids) {
      if (visited.count(start)) continue;

      // Connected component over the undirected link graph (deterministic).
      std::vector<std::string> comp;
      std::queue<std::string> bfs;
      bfs.push(start);
      visited.insert(start);
      while (!bfs.empty()) {
        const std::string cur = bfs.front();
        bfs.pop();
        comp.push_back(cur);
        std::vector<std::string> nb = adjOf(undir, cur);
        std::sort(nb.begin(), nb.end());
        for (const std::string& o : nb) {
          if (!visited.count(o)) {
            visited.insert(o);
            bfs.push(o);
          }
        }
      }
      std::sort(comp.begin(), comp.end());
      const std::set<std::string> inComp(comp.begin(), comp.end());

      // Longest-path rank via Kahn's algorithm (cyclic nodes fall back to 0).
      std::unordered_map<std::string, int> deg;
      for (const std::string& id2 : comp) deg[id2] = 0;
      for (const std::string& id2 : comp)
        for (const std::string& o : adjOf(outAdj, id2))
          if (inComp.count(o)) deg[o]++;
      std::vector<std::string> seeds;
      for (const std::string& id2 : comp)
        if (deg[id2] == 0) seeds.push_back(id2);
      std::sort(seeds.begin(), seeds.end());
      std::unordered_map<std::string, int> rank;
      std::queue<std::string> q;
      for (const std::string& s : seeds) {
        rank[s] = 0;
        q.push(s);
      }
      while (!q.empty()) {
        const std::string cur = q.front();
        q.pop();
        std::vector<std::string> down = adjOf(outAdj, cur);
        std::sort(down.begin(), down.end());
        for (const std::string& nb : down) {
          if (!inComp.count(nb)) continue;
          const int existing = rank.count(nb) ? rank[nb] : 0;
          rank[nb] = std::max(existing, rank[cur] + 1);
          if (--deg[nb] == 0) q.push(nb);
        }
      }
      for (const std::string& id2 : comp)
        if (!rank.count(id2)) rank[id2] = 0;

      std::map<int, std::vector<std::string>> byRank;
      for (const std::string& id2 : comp) byRank[rank[id2]].push_back(id2);

      // Order within each rank by predecessor barycenter (nodes with no placed
      // parent sort last, stable by id); rank 0 sorts by id.
      std::unordered_map<std::string, double> orderIndex;
      const int firstRank = byRank.empty() ? 0 : byRank.begin()->first;
      for (auto& [r, nodesInRank] : byRank) {
        if (r == firstRank) {
          std::sort(nodesInRank.begin(), nodesInRank.end());
        } else {
          const auto bary = [&](const std::string& id3) {
            double sum = 0.0;
            int c = 0;
            for (const std::string& p : adjOf(inAdj, id3)) {
              auto oi = orderIndex.find(p);
              if (oi != orderIndex.end()) {
                sum += oi->second;
                ++c;
              }
            }
            return c > 0 ? sum / c : 1e18;
          };
          std::stable_sort(nodesInRank.begin(), nodesInRank.end(),
                           [&](const std::string& a, const std::string& b) {
                             const double ba = bary(a), bb = bary(b);
                             if (ba != bb) return ba < bb;
                             return a < b;
                           });
        }
        for (int i = 0; i < static_cast<int>(nodesInRank.size()); ++i)
          orderIndex[nodesInRank[i]] = i;
      }

      // Column X per rank (widest node in the rank + gap).
      std::map<int, double> colX;
      double x = 0.0;
      for (auto& [r, nodesInRank] : byRank) {
        colX[r] = x;
        double maxW = 0.0;
        for (const std::string& id2 : nodesInRank)
          maxW = std::max(maxW, model.nodes.at(id2).size[0]);
        x += maxW + gapX;
      }

      // Stack each rank vertically; capture the component bbox for the Y offset.
      double compMinY = 1e18, compMaxY = -1e18;
      for (auto& [r, nodesInRank] : byRank) {
        double y = 0.0;
        for (const std::string& id2 : nodesInRank) {
          noodles::NodeData& n = model.nodes.at(id2);
          n.position = noodles::Vec2d(colX[r], y);
          compMinY = std::min(compMinY, y);
          compMaxY = std::max(compMaxY, y + n.size[1]);
          y += n.size[1] + gapY;
        }
      }
      const double dy = componentY - (compMinY < 1e17 ? compMinY : 0.0);
      for (const std::string& id2 : comp) {
        noodles::NodeData& n = model.nodes.at(id2);
        n.position = noodles::Vec2d(n.position[0], n.position[1] + dy);
      }
      componentY = (compMaxY > -1e17 ? compMaxY + dy : componentY) + componentGapY;
    }
  }

  // ── content bounds / dynamic zoom floor (item 2) ──
  // Graph-space bounding box over every laid-out node. False for an empty graph.
  bool contentBounds(double* minX, double* minY, double* maxX,
                     double* maxY) const {
    bool any = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (const auto& [id, n] : model.nodes) {
      const double nx0 = n.position[0], ny0 = n.position[1];
      const double nx1 = nx0 + n.size[0], ny1 = ny0 + n.size[1];
      if (!any) {
        x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
        any = true;
      } else {
        x0 = std::min(x0, nx0); y0 = std::min(y0, ny0);
        x1 = std::max(x1, nx1); y1 = std::max(y1, ny1);
      }
    }
    if (!any) return false;
    *minX = x0; *minY = y0; *maxX = x1; *maxY = y1;
    return true;
  }

  // The dynamic zoom-out floor: allow zooming out until the whole laid-out graph
  // is ~4× smaller than the viewport (min(0.25, fitZoom*0.25)). Empty graph or
  // no viewport falls back to the static 0.25 floor. The upper bound stays
  // kZoomMax. (EXTRACTION.md §3.)
  double minZoom() const {
    double x0, y0, x1, y1;
    if (viewportW <= 0 || viewportH <= 0 ||
        !contentBounds(&x0, &y0, &x1, &y1)) {
      return kZoomMin;
    }
    const double bw = x1 - x0, bh = y1 - y0;
    if (bw <= 0.0 || bh <= 0.0) return kZoomMin;
    const double fitZoom =
        std::min(static_cast<double>(viewportW) / bw,
                 static_cast<double>(viewportH) / bh);
    return std::min(kZoomMin, fitZoom * 0.25);
  }

  // Exact edge position for a displayed row. Legacy input rows may start an
  // output drag from the right edge even before they have an output mirror.
  // Resolve the requested side first, then reuse the
  // opposite side's row Y, including folded-child → header routing. Only the X
  // is forced to the requested edge.
  noodles::Vec2d rowEdgeAnchor(const noodles::NodeData& n,
                               const std::string& port,
                               bool isOutput) const {
    noodles::Vec2d anchor;
    const auto resolveVisible = [&](bool side, const std::string& candidate,
                                    noodles::Vec2d* result) {
      const auto& pins = side ? n.outputPins : n.inputPins;
      if (std::find(pins.begin(), pins.end(), candidate) == pins.end()) {
        return false;
      }
      *result = nodeRenderer.resolvePortPosition(n, candidate, side);
      return true;
    };
    const auto resolveSide = [&](bool side, noodles::Vec2d* result) {
      if (resolveVisible(side, port, result)) return true;
      const auto& folded = side ? n.foldedOutputPinMap : n.foldedInputPinMap;
      if (auto it = folded.find(port); it != folded.end()) {
        return resolveVisible(side, it->second, result);
      }
      return false;
    };

    if (!resolveSide(isOutput, &anchor) &&
        !resolveSide(!isOutput, &anchor)) {
      anchor = nodeRenderer.resolvePortPosition(n, port, isOutput);
    }
    anchor[0] = isOutput ? n.position[0] + n.size[0] : n.position[0];
    return anchor;
  }

  // Permanent relationship noodles use the same visible-row resolver as live
  // drags, always on the output/right edge.
  noodles::Vec2d relationshipRowAnchor(const noodles::NodeData& n,
                                       const std::string& port) const {
    return rowEdgeAnchor(n, port, /*isOutput=*/true);
  }

  // ── minimap (item 4) ──
  // The minimap frame + fit transform + viewport rectangle, all in VIEW POINTS
  // (top-left origin, Y-down) so input/hit-testing and the pure mapping math
  // share one space; the renderer scales by contentScale to pixels. Computed
  // on demand from the live pan/zoom/viewport (no cached GL state).
  struct MinimapLayout {
    bool visible = false;
    double x = 0, y = 0, w = 0, h = 0;  // frame rect (view points)
    double pad = 0;
    double scale = 1.0;        // world → minimap-inner (points per world unit)
    double offX = 0, offY = 0;  // inner content origin (points)
    double gMinX = 0, gMinY = 0;  // padded graph-space min mapped to (offX,offY)
    double vpX0 = 0, vpY0 = 0, vpX1 = 0, vpY1 = 0;  // viewport rect (points)
  };

  MinimapLayout computeMinimap() const {
    MinimapLayout m;
    double bx0, by0, bx1, by1;
    if (viewportW <= 0 || viewportH <= 0 ||
        !contentBounds(&bx0, &by0, &bx1, &by1)) {
      return m;
    }
    const double cs = contentScale > 0.0f ? contentScale : 1.0;
    const double Wp = viewportW / cs;  // viewport size in points
    const double Hp = viewportH / cs;
    const double vw = zoom != 0.0 ? viewportW / zoom : viewportW;  // world span
    const double vh = zoom != 0.0 ? viewportH / zoom : viewportH;
    const double vminX = panX, vminY = panY;
    const double vmaxX = panX + vw, vmaxY = panY + vh;

    // Visible only when the content extends beyond the viewport on any side.
    const double eps = 1.0;
    const bool exceeds = bx0 < vminX - eps || by0 < vminY - eps ||
                         bx1 > vmaxX + eps || by1 > vmaxY + eps;
    if (!exceeds) return m;

    constexpr double mmW = 180.0, mmH = 120.0, margin = 12.0, pad = 6.0;
    if (Wp < mmW + margin * 2 || Hp < mmH + margin * 2) return m;  // too small

    m.visible = true;
    m.w = mmW; m.h = mmH; m.pad = pad;
    m.x = Wp - mmW - margin;   // bottom-right corner
    m.y = Hp - mmH - margin;

    const double gpadX = std::max((bx1 - bx0) * 0.1, 1.0);
    const double gpadY = std::max((by1 - by0) * 0.1, 1.0);
    m.gMinX = bx0 - gpadX;
    m.gMinY = by0 - gpadY;
    const double gW = (bx1 - bx0) + gpadX * 2;
    const double gH = (by1 - by0) + gpadY * 2;
    const double innerW = mmW - pad * 2, innerH = mmH - pad * 2;
    m.scale = std::min(innerW / gW, innerH / gH);
    const double drawnW = gW * m.scale, drawnH = gH * m.scale;
    m.offX = m.x + pad + (innerW - drawnW) * 0.5;
    m.offY = m.y + pad + (innerH - drawnH) * 0.5;

    m.vpX0 = m.offX + (vminX - m.gMinX) * m.scale;
    m.vpY0 = m.offY + (vminY - m.gMinY) * m.scale;
    m.vpX1 = m.offX + (vmaxX - m.gMinX) * m.scale;
    m.vpY1 = m.offY + (vmaxY - m.gMinY) * m.scale;
    return m;
  }

  static bool minimapContains(const MinimapLayout& m, double sx, double sy) {
    return m.visible && sx >= m.x && sx <= m.x + m.w && sy >= m.y &&
           sy <= m.y + m.h;
  }

  // Inverse of the fit transform: a point inside the minimap → graph-space world.
  static void minimapToWorld(const MinimapLayout& m, double sx, double sy,
                             double* wx, double* wy) {
    *wx = m.gMinX + (m.scale != 0.0 ? (sx - m.offX) / m.scale : 0.0);
    *wy = m.gMinY + (m.scale != 0.0 ? (sy - m.offY) / m.scale : 0.0);
  }

  // Center the viewport on the world point under a minimap cursor point.
  void minimapPanTo(const MinimapLayout& m, double sx, double sy) {
    double wx = 0, wy = 0;
    minimapToWorld(m, sx, sy, &wx, &wy);
    const double vw = zoom != 0.0 ? viewportW / zoom : viewportW;
    const double vh = zoom != 0.0 ? viewportH / zoom : viewportH;
    panX = wx - vw * 0.5;
    panY = wy - vh * 0.5;
    requestRender();
  }

  // Draw the minimap LAST (into whatever framebuffer renderGraph bound), so it
  // rides the same overlay-opacity composite. Node rects + a viewport rectangle
  // over a translucent frame, via the Noodles node-quad shader path
  // (NodeRenderManager::buildPortQuadVertices + renderNodes) — the least-
  // invasive primitive the lib exposes for filled rects. Depth is cleared first
  // so the minimap always wins the LEQUAL test regardless of node depths.
  void drawMinimap() {
    const MinimapLayout m = computeMinimap();
    if (!m.visible) return;
    const double cs = contentScale > 0.0f ? contentScale : 1.0;

    float proj[16];
    ComputeOrtho(0.0, 0.0, 1.0, viewportW, viewportH, proj);  // pixels → NDC
    constexpr float depth = 0.0f;

    std::vector<noodles::NodeVertex> verts;
    const auto addRect = [&](double px, double py, double pw, double ph,
                             const std::array<float, 4>& col) {
      if (pw <= 0.0 || ph <= 0.0) return;
      auto q = noodles::NodeRenderManager::buildPortQuadVertices(
          static_cast<float>(px * cs), static_cast<float>(py * cs),
          static_cast<float>(pw * cs), static_cast<float>(ph * cs), depth, col);
      verts.insert(verts.end(), q.begin(), q.end());
    };

    addRect(m.x, m.y, m.w, m.h, {0.08f, 0.08f, 0.10f, 0.72f});  // frame
    for (const auto& [id, n] : model.nodes) {
      const double nx = m.offX + (n.position[0] - m.gMinX) * m.scale;
      const double ny = m.offY + (n.position[1] - m.gMinY) * m.scale;
      const double nw = std::max(n.size[0] * m.scale, 1.5);
      const double nh = std::max(n.size[1] * m.scale, 1.5);
      const std::array<float, 4> col =
          (id == selectedNodeId)
              ? std::array<float, 4>{1.0f, 0.85f, 0.2f, 0.95f}
              : std::array<float, 4>{0.62f, 0.66f, 0.74f, 0.95f};
      addRect(nx, ny, nw, nh, col);
    }
    // Viewport rectangle as four thin edges (interior stays see-through).
    const double vx0 = std::max(m.vpX0, m.x + m.pad);
    const double vy0 = std::max(m.vpY0, m.y + m.pad);
    const double vx1 = std::min(m.vpX1, m.x + m.w - m.pad);
    const double vy1 = std::min(m.vpY1, m.y + m.h - m.pad);
    const double t = 2.0;
    const std::array<float, 4> vpc{0.95f, 0.4f, 0.4f, 0.95f};
    if (vx1 > vx0 && vy1 > vy0) {
      addRect(vx0, vy0, vx1 - vx0, t, vpc);
      addRect(vx0, vy1 - t, vx1 - vx0, t, vpc);
      addRect(vx0, vy0, t, vy1 - vy0, vpc);
      addRect(vx1 - t, vy0, t, vy1 - vy0, vpc);
    }

    glClear(GL_DEPTH_BUFFER_BIT);
    renderer.nodeManager().renderNodes(
        verts, /*strokeColor=*/0xFFFFFFFEu, /*generation=*/++minimapGen,
        /*selectionChanged=*/true, proj, /*cornerRadius=*/0.0f,
        std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
  }

  // ── relationship target arrows ──
  // One down-pointing arrowhead per whole-prim relationship link, tip on the
  // target node's top-center, base at the noodle end (link.end sits at the
  // arrow's top-middle — see rebuildLinkGeometry). Drawn like the relationship
  // port glyphs (outer ring triangle + inset fill) so the marker reads as a
  // grabbable endpoint; relationshipArrowAt makes it one. Rebuilt every frame
  // (a handful of triangles), like the minimap.
  void drawRelationshipArrows(const float* proj) {
    std::vector<noodles::NodeVertex> verts;
    for (const noodles::LinkData& l : model.links) {
      if (l.isDangling || !l.isRelationship || !l.targetPort.empty()) continue;
      auto tIt = model.nodes.find(l.targetNodeId);
      if (tIt == model.nodes.end()) continue;
      const noodles::NodeData& n = tIt->second;
      const float depth =
          static_cast<float>(noodles::RenderConfig::contentDepth(n.zOrder));
      const float cx = static_cast<float>(l.end[0]);
      const float baseY = static_cast<float>(l.end[1]);  // arrow top = noodle end
      const float tipY = static_cast<float>(l.end[1] + kRelArrowHeight);
      const float hw = static_cast<float>(kRelArrowHalfWidth);
      auto outer = noodles::NodeRenderManager::buildTriangleEndpointVertices(
          cx, tipY, cx - hw, baseY, cx + hw, baseY, depth,
          config.portConnectedRingColor);
      verts.insert(verts.end(), outer.begin(), outer.end());
      // Inset fill (same centroid-shrink the relationship port glyph uses).
      const float icx = cx;
      const float icy = (tipY + baseY + baseY) / 3.0f;
      constexpr float insetRatio = 0.35f;
      const auto inset = [&](float x, float y) {
        return std::pair<float, float>(x + (icx - x) * insetRatio,
                                       y + (icy - y) * insetRatio);
      };
      const auto [ix0, iy0] = inset(cx, tipY);
      const auto [ix1, iy1] = inset(cx - hw, baseY);
      const auto [ix2, iy2] = inset(cx + hw, baseY);
      auto inner = noodles::NodeRenderManager::buildTriangleEndpointVertices(
          ix0, iy0, ix1, iy1, ix2, iy2, depth, config.portConnectedFillColor);
      verts.insert(verts.end(), inner.begin(), inner.end());
    }
    if (verts.empty()) return;
    renderer.nodeManager().renderNodes(
        verts, /*strokeColor=*/0xFFFFFFFEu, /*generation=*/++relArrowGen,
        /*selectionChanged=*/true, proj, /*cornerRadius=*/0.0f,
        std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
  }

  // ── external document reflection ──
  void unregisterDocumentObserver() {
    if (document && observerToken != 0) document->removeObserver(observerToken);
    observerToken = 0;
  }

  void registerDocumentObserver(
      const std::shared_ptr<GraphDocument>& newDocument) {
    unregisterDocumentObserver();
    document = newDocument;
    if (document) {
      observerToken = document->addObserver(
          [this](const GraphDocumentChange& change) {
            onDocumentChanged(change);
          });
    }
    {
      std::lock_guard<std::mutex> lock(pendingMutex);
      pendingInfoChanges.clear();
    }
    pendingInfoDirty = false;
    pendingStructural = false;
  }

  // Runs on the mutating thread and only queues plain change data.
  void onDocumentChanged(const GraphDocumentChange& change) {
    if (selfEditing) return;
    if (change.kind == GraphDocumentChange::Kind::Structure) {
      pendingStructural = true;
      if (delegate.graphStructureChanged) delegate.graphStructureChanged();
    } else {
      {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingInfoChanges.push_back(change);
      }
      pendingInfoDirty = true;
    }
    requestRender();
  }

  // Apply a captured external position while keeping the Noodles transform
  // frame, hit geometry, and links in lockstep.
  void applyExternalPosition(const GraphNode& sourceNode) {
    if (!sourceNode.hasPosition) return;
    const std::string& nodeId = sourceNode.id;
    auto nit = model.nodes.find(nodeId);
    if (nit == model.nodes.end()) return;
    const noodles::Vec2d p(sourceNode.posX, sourceNode.posY);
    nit->second.position = p;
    heldPositions[nodeId] = p;
    autoPositionedNodeIds.erase(nodeId);
    spatial.UpdateNode(nodeId,
                       noodles::SpatialIndex::ComputeNodeBounds(nit->second));
    rebuildLinkGeometry();
    moveNodeLive(nodeId, p);
    contentDirty = true;
  }

  void processPendingDocumentChanges() {
    if (!pendingInfoDirty.exchange(false)) return;
    std::vector<GraphDocumentChange> changes;
    {
      std::lock_guard<std::mutex> lock(pendingMutex);
      changes.swap(pendingInfoChanges);
    }
    if (!document || changes.empty()) return;

    std::set<std::string> posEdits;
    std::set<std::pair<std::string, std::string>> valueEdits;
    for (const GraphDocumentChange& change : changes) {
      if (model.nodes.find(change.nodeId) == model.nodes.end()) continue;
      if (change.kind == GraphDocumentChange::Kind::NodePosition) {
        posEdits.insert(change.nodeId);
      } else if (change.kind == GraphDocumentChange::Kind::AttributeValue) {
        valueEdits.insert({change.nodeId, change.propertyName});
      }
    }

    const GraphSnapshot captured = document->snapshot(displayFrame);
    std::unordered_map<std::string, const GraphNode*> byId;
    for (const GraphNode& node : captured.nodes) byId[node.id] = &node;

    bool changed = false;
    for (const std::string& nodeId : posEdits) {
      auto source = byId.find(nodeId);
      if (source == byId.end()) continue;
      applyExternalPosition(*source->second);
      changed = true;
    }

    if (!valueEdits.empty()) {
      for (const auto& [nodeId, propertyName] : valueEdits) {
        auto sit = byId.find(nodeId);
        if (sit == byId.end()) continue;
        auto nit = model.nodes.find(nodeId);
        if (nit == model.nodes.end()) continue;
        for (const GraphProperty& property : sit->second->properties) {
          if (property.name != propertyName ||
              property.kind != GraphPropertyKind::Attribute) {
            continue;
          }
          auto& pinTypes = property.direction == GraphPropertyDirection::Output
                               ? nit->second.originalOutputPinTypes
                               : nit->second.originalInputPinTypes;
          pinTypes[propertyName] = ComposeTypeText(property);
          if (property.hasValue) {
            valueRows[nodeId][propertyName] = {
                property.isScrubable, property.numericValue, property.type,
                property.displayValue};
          } else {
            auto vn = valueRows.find(nodeId);
            if (vn != valueRows.end()) vn->second.erase(propertyName);
          }
          layoutNodeOne(nit->second);
          changed = true;
          break;
        }
      }
    }
    if (changed) requestRender();
  }

  // A timeline-frame change only changes captured attribute values. Refresh
  // those rows in place so title/group fold state, selection, z-order and held
  // layout survive playback instead of being reset by a full buildModel().
  void refreshValuesForDisplayFrame() {
    if (!document) return;
    const GraphSnapshot captured = document->snapshot(displayFrame);
    bool changed = false;
    for (const GraphNode& source : captured.nodes) {
      auto nodeIt = model.nodes.find(source.id);
      if (nodeIt == model.nodes.end()) continue;
      noodles::NodeData& node = nodeIt->second;
      bool nodeChanged = false;
      for (const GraphProperty& property : source.properties) {
        if (property.kind != GraphPropertyKind::Attribute) continue;
        auto& pinTypes = property.direction == GraphPropertyDirection::Output
                             ? node.originalOutputPinTypes
                             : node.originalInputPinTypes;
        auto typeIt = pinTypes.find(property.name);
        if (typeIt == pinTypes.end()) continue;
        const std::string typeText = ComposeTypeText(property);
        if (typeIt->second != typeText) {
          typeIt->second = typeText;
          nodeChanged = true;
        }
        if (property.hasValue) {
          const ValueRow next{property.isScrubable, property.numericValue,
                              property.type, property.displayValue};
          auto& rows = valueRows[source.id];
          auto row = rows.find(property.name);
          if (row == rows.end() || row->second.scrubable != next.scrubable ||
              row->second.value != next.value ||
              row->second.typeToken != next.typeToken ||
              row->second.display != next.display) {
            rows[property.name] = next;
            nodeChanged = true;
          }
        } else {
          auto rows = valueRows.find(source.id);
          if (rows != valueRows.end() && rows->second.erase(property.name) > 0) {
            nodeChanged = true;
          }
        }
      }
      if (nodeChanged) {
        model.layoutNode(node, textWidth(), metrics(), &config);
        changed = true;
      }
    }
    if (!changed) return;
    rebuildLinkGeometry();
    rebuildSpatialIndex();
    contentDirty = true;
    requestRender();
  }

  // The graph draw body (clear + node quads + renderer). Renders into whatever
  // framebuffer is currently bound (the FBO in the composite path, otherwise the
  // platform default framebuffer directly).
  void renderGraph();

  void buildModel();
};

// Rebuild the GraphModel from a plain snapshot, mapping document properties
// onto noodles pin content the way the reference nodeFactory/pinUtils do:
// input attributes → input-side pins, explicit output attributes and
// relationships → output-side pins; attribute connections become out→in data
// links and relationships become whole-node links.
void GraphEditor::Impl::buildModel() {
  const std::string keepSelection = selectedNodeId;
  model.clear();
  spatial.Clear();
  valueRows.clear();
  nextZOrder = 0;

  // Time-aware value capture: animated rows reflect the current display frame.
  const GraphSnapshot graph = document ? document->snapshot(displayFrame)
                                       : GraphSnapshot{};

  // Nodes with no authored AND no held position — placed by autolayout below.
  std::vector<std::string> unpositioned;
  std::set<std::string> currentNodeIds;
  for (const GraphNode& src : graph.nodes) {
    currentNodeIds.insert(src.id);
    noodles::NodeData nd;
    nd.id = src.id;
    nd.name = src.name;
    nd.type = src.schemaTypeName;  // pin-type fallback / graffi subtitle
    nd.schemaTypeName = src.schemaTypeName;
    nd.zOrder = nextZOrder++;

    for (const GraphProperty& prop : src.properties) {
      if (prop.kind == GraphPropertyKind::Relationship) {
        nd.originalOutputPins.push_back(prop.name);
        nd.originalOutputPinTypes[prop.name] = prop.type;  // "rel"
        nd.relationshipOutputPins.insert(prop.name);
        nd.orderedPinEntries.emplace_back("output", prop.name);
      } else {  // attribute: explicit output rows stay output-only
        const bool isOutput =
            prop.direction == GraphPropertyDirection::Output;
        auto& pins = isOutput ? nd.originalOutputPins : nd.originalInputPins;
        auto& pinTypes = isOutput ? nd.originalOutputPinTypes
                                  : nd.originalInputPinTypes;
        pins.push_back(prop.name);
        // TYPE slot carries "<type> · <value>"; the pin NAME stays pure.
        pinTypes[prop.name] = ComposeTypeText(prop);
        nd.orderedPinEntries.emplace_back(isOutput ? "output" : "input",
                                          prop.name);
        if (prop.hasValue) {
          valueRows[src.id][prop.name] = {prop.isScrubable, prop.numericValue,
                                          prop.type, prop.displayValue};
        }
      }
    }

    // Position: authored → previously-held → autolayout (resolved after sizing).
    // Only authored/held positions are pinned into heldPositions now; a node
    // needing layout is recorded and placed below so it never reshuffles a node
    // that already has a home.
    noodles::Vec2d pos;
    bool positioned = true;
    if (src.hasPosition) {
      pos = noodles::Vec2d(src.posX, src.posY);
      autoPositionedNodeIds.erase(src.id);
    } else if (auto it = heldPositions.find(src.id); it != heldPositions.end()) {
      pos = it->second;
    } else {
      positioned = false;
      pos = noodles::Vec2d(0.0, 0.0);  // placeholder until autolayout runs
    }
    nd.position = pos;
    if (positioned) heldPositions[src.id] = pos;
    else unpositioned.push_back(src.id);

    if (src.id == keepSelection) nd.selected = true;
    model.nodes.emplace(src.id, std::move(nd));
  }
  for (auto it = autoPositionedNodeIds.begin();
       it != autoPositionedNodeIds.end();) {
    if (currentNodeIds.count(*it) == 0) {
      it = autoPositionedNodeIds.erase(it);
    } else {
      ++it;
    }
  }

  // Edges → noodles links. An attribute connection C.in ← B.out is a data
  // link whose OUTPUT (source) side is B.out and INPUT (target) side is C.in;
  // B.out therefore needs an output-side (dual) pin. A relationship is a
  // whole-node link drawn from the relationship's output pin.
  for (const GraphEdge& e : graph.edges) {
    noodles::LinkData link;
    if (e.isRelationship) {
      link.sourceNodeId = e.sourceNodeId;
      link.sourcePort = e.sourcePort;
      link.targetNodeId = e.targetNodeId;
      link.targetPort = std::string();
      link.isRelationship = true;
    } else {
      // Flip to data-flow direction: target of the collected edge is the output.
      link.sourceNodeId = e.targetNodeId;
      link.sourcePort = e.targetPort;
      link.targetNodeId = e.sourceNodeId;
      link.targetPort = e.sourcePort;
      link.targetPropertyName = e.sourcePort;
      link.isRelationship = false;
      // A legacy input-side attribute used as a data source mirrors an output
      // port on its right edge. An explicit output-direction attribute already
      // owns a typed output row and must not get a duplicate dual pin.
      if (auto it = model.nodes.find(link.sourceNodeId);
          it != model.nodes.end() && !link.sourcePort.empty()) {
        const auto& explicitOutputs = it->second.originalOutputPins;
        if (std::find(explicitOutputs.begin(), explicitOutputs.end(),
                      link.sourcePort) == explicitOutputs.end()) {
          it->second.dualPinNames.insert(link.sourcePort);
        }
      }
    }
    model.links.push_back(std::move(link));
  }

  model.buildConnectionCache();
  layoutAll();  // node sizes + per-pin centers (required before any layout)

  // Autolayout the nodes that have no home yet. A wholly unpositioned graph gets
  // a full deterministic layered pass (layeredLayout, edge-aware over BOTH data
  // and relationship links, non-overlapping, left→right by rank); a few new
  // nodes added to an already-placed graph are instead dropped near their
  // neighbours WITHOUT moving the placed nodes. In both cases the results are
  // HELD only — autolayout never authors to the document.
  if (!unpositioned.empty()) {
    autoPositionedNodeIds.insert(unpositioned.begin(), unpositioned.end());
    if (unpositioned.size() == model.nodes.size()) {
      layeredLayout();
      for (auto& [id, node] : model.nodes) heldPositions[id] = node.position;
    } else {
      placeIncrementalNodes(unpositioned);
    }
  }

  rebuildLinkGeometry();
  rebuildSpatialIndex();

  // Reconcile selection with the rebuilt model.
  if (model.nodes.find(keepSelection) == model.nodes.end()) {
    selectedNodeId.clear();
  } else {
    selectedNodeId = keepSelection;
  }
  contentDirty = true;
}

// ─── public shell ────────────────────────────────────────────────────────────

GraphEditor::GraphEditor() : impl_(std::make_unique<Impl>()) {}
GraphEditor::~GraphEditor() {
  if (impl_->glReady) {
    impl_->renderer.cleanup();
    impl_->releaseCompositeGL();
  }
}

void GraphEditor::setDelegate(GraphEditDelegate delegate) {
  impl_->delegate = std::move(delegate);
}

void GraphEditor::setDocument(std::shared_ptr<GraphDocument> document) {
  impl_->registerDocumentObserver(document);
  impl_->buildModel();
  impl_->requestRender();
}

std::shared_ptr<GraphDocument> GraphEditor::document() const {
  return impl_->document;
}

void GraphEditor::refresh() {
  impl_->buildModel();
  impl_->requestRender();
}

void GraphEditor::initializeGL(const std::string& assetsPath,
                                    const std::string& fontAtlasPath,
                                    const std::string& fontMetricsPath) {
  impl_->renderer.initialize(assetsPath, fontAtlasPath, fontMetricsPath);
  impl_->glReady = true;
  // Re-lay-out with the real font-atlas metrics now available.
  impl_->layoutAll();
  impl_->reflowAutoPositionsForCurrentMetrics();
  impl_->rebuildLinkGeometry();
  impl_->rebuildSpatialIndex();
  impl_->contentDirty = true;
}

void GraphEditor::resize(int widthPx, int heightPx, float contentScale) {
  impl_->viewportW = widthPx;
  impl_->viewportH = heightPx;
  impl_->contentScale = contentScale > 0.0f ? contentScale : 1.0f;
}

void GraphEditor::Impl::renderGraph() {
  Impl& s = *this;
  glViewport(0, 0, s.viewportW, s.viewportH);
  glClearColor(s.clearColor[0], s.clearColor[1], s.clearColor[2],
               s.clearColor[3]);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_TRUE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // Canonical C++-owned snapshot render path (reference _paintGLInner): drive the
  // render managers directly from the held GraphModel — node quads + text share
  // ONE NodeTransformFrame (wired by GraphRenderer::initialize), so a move that
  // regenerates one regenerates the other in lockstep, and a pan/zoom-only frame
  // re-uploads only the projection and redraws the cached VBOs (contentDirty
  // stays false). NOT GraphRenderer::render(RenderParams) — that path needs
  // caller-built vertices and omits the port highlights.
  float proj[16];
  ComputeOrtho(s.panX, s.panY, s.zoom, s.viewportW, s.viewportH, proj);
  const float zoomF = static_cast<float>(s.zoom);
  const float panXF = static_cast<float>(s.panX);
  const float panYF = static_cast<float>(s.panY);
  const float vpWf = static_cast<float>(s.viewportW);
  const float vpHf = static_cast<float>(s.viewportH);
  const float cornerRadius = static_cast<float>(s.config.nodeCornerRadius);
  const std::array<float, 4> innerStroke{1.0f, 1.0f, 1.0f, 0.5f};
  const bool contentChanged = s.contentDirty;

  noodles::GraphRenderer& r = s.renderer;

  // Links: four passes for correct layering — unselected then selected, each
  // split regular / relationship (distinct cacheKeys keep their instance VBOs
  // separate). Dimming: 1.0 unselected, 0.3 selected (reference values).
  for (bool drawSelected : {false, true}) {
    const float dim = drawSelected ? 0.3f : 1.0f;
    for (bool relOnly : {false, true}) {
      r.linkManager().renderLinksFromGraph(
          s.model, proj, s.config, zoomF, panXF, panYF, vpWf, vpHf, dim,
          drawSelected, relOnly,
          /*baseColor=*/nullptr, /*selectedColor=*/nullptr,
          /*hoveredColor=*/nullptr, /*highlightedColor=*/nullptr,
          /*cacheKey=*/relOnly ? 1 : 0);
    }
  }

  // Node background quads from the snapshot; moves ride the shared transform
  // frame, so only a content change regenerates geometry.
  r.nodeManager().renderNodesFromGraph(s.model, s.config, r.fontAtlas(), proj,
                                       cornerRadius, innerStroke, contentChanged);

  // Port circles + relationship-authoring triangles, AFTER the node quads and
  // BEFORE text (reference _renderPortHighlightsCpp order). This is the pass
  // that draws the port indicators at all. The producer bakes each node's live
  // drag offset per-vertex, so ports must be rebuilt while a node drag is in
  // progress even though the quads themselves only ride the transform texture.
  r.nodeManager().renderPortHighlightsFromGraph(
      s.model, s.config, s.portHighlightState(), proj, cornerRadius,
      /*geometryChanged=*/contentChanged || s.drag == Drag::Node);

  // Relationship target arrows (down-pointing, tip on the target's top edge).
  s.drawRelationshipArrows(proj);

  // Drag-preview noodle: renderLinksFromGraph deliberately skips dangling
  // links, so the transient preview goes through the per-call vector path with
  // its validity color (green = valid drop, red = invalid). Drawn after the
  // node quads so the free end stays visible under the finger/stylus even over
  // a node body.
  if (s.dragPreviewActive && !s.model.links.empty() &&
      s.model.links.back().isDangling) {
    const noodles::LinkData& pv = s.model.links.back();
    const float previewColor[3] = {pv.color[0], pv.color[1], pv.color[2]};
    r.linkManager().renderLinks(
        std::vector<noodles::LinkData>{pv}, proj, s.config, zoomF, panXF,
        panYF, vpWf, vpHf, /*dimming=*/1.0f, /*drawSelected=*/false,
        pv.hasColor ? previewColor : nullptr, /*selectedColor=*/nullptr,
        /*hoveredColor=*/nullptr, /*highlightedColor=*/nullptr,
        /*cacheKey=*/1000);
  }

  // Node text, sharing the same transform frame → glyphs track a dragged node.
  r.textManager().renderNodeTextFull(s.model.nodes, proj, zoomF, contentChanged,
                                     s.config, s.panX, s.panY,
                                     static_cast<double>(s.viewportW),
                                     static_cast<double>(s.viewportH),
                                     /*cullOffscreen=*/true);

  // Minimap overlay, drawn LAST so it sits above the graph inside the same
  // composite; only appears when the content extends beyond the viewport.
  s.drawMinimap();

  s.contentDirty = false;
}

void GraphEditor::renderFrame() {
  Impl& s = *impl_;
  if (!s.glReady) return;
  if (s.viewportW <= 0 || s.viewportH <= 0) return;

  // Fold in any external document edits queued by its observer
  // before drawing this frame (runs on the render thread, as required).
  s.processPendingDocumentChanges();

  // The platform's "default" framebuffer is NOT 0 on iOS (it is the CAEAGLLayer
  // FBO). Query and restore it — never hardcode 0.
  GLint prevFbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

  if (!s.overlayComposite()) {
    // Opaque + undimmed: render straight to the bound framebuffer (no offscreen
    // pass — identical to the standalone/host path).
    s.renderGraph();
    return;
  }

  s.ensureFbo();
  glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
  s.renderGraph();
  // Composite the offscreen graph onto the platform default framebuffer.
  s.compositeOverlay(static_cast<GLuint>(prevFbo));
}

void GraphEditor::cleanupGL() {
  if (impl_->glReady) {
    impl_->renderer.cleanup();
    impl_->releaseCompositeGL();
    impl_->glReady = false;
  }
}

void GraphEditor::setClearColor(float r, float g, float b, float a) {
  impl_->clearColor = {r, g, b, a};
}

void GraphEditor::setOverlayOpacity(float opacity) {
  impl_->overlayOpacity = std::clamp(opacity, 0.0f, 1.0f);
}

void GraphEditor::setValueScrubEnabled(bool enabled) {
  impl_->valueScrubEnabled = enabled;
}

void GraphEditor::setDisplayFrame(double frame) {
  if (impl_->displayFrame == frame) return;
  impl_->displayFrame = frame;
  impl_->refreshValuesForDisplayFrame();
}

void GraphEditor::processPendingDocumentChanges() {
  impl_->processPendingDocumentChanges();
}

// ─── input ───────────────────────────────────────────────────────────────────

void GraphEditor::pointerDown(double x, double y) {
  Impl& s = *impl_;
  s.pointerMoved = false;
  s.downViewX = s.lastViewX = x;
  s.downViewY = s.lastViewY = y;
  s.foldTapNode.clear();
  s.foldTapGroup.clear();

  // 0) The minimap claims the pointer before ANY other hit-testing while it is
  //    visible: a press inside its frame begins a pan-drag and immediately
  //    centers the viewport on the mapped world point.
  {
    const Impl::MinimapLayout mm = s.computeMinimap();
    if (Impl::minimapContains(mm, x, y)) {
      s.drag = Impl::Drag::Minimap;
      s.minimapPanTo(mm, x, y);
      return;
    }
  }

  const noodles::Vec2d world = s.viewToGraph(x, y);
  s.downWorldX = world[0];
  s.downWorldY = world[1];

  // 0.5) A relationship target ARROW under the cursor: lift that link so the
  //      target end follows the pointer (retarget on a node, disconnect in
  //      empty space). Checked before the node hit so the arrow — whose tip
  //      touches the node's top edge — wins over the node body.
  {
    const int ai = s.relationshipArrowAt(world);
    if (ai >= 0) {
      s.beginLinkReconnect(ai, world);
      return;
    }
  }

  // 1) A node under the cursor: if a port is armed, author the edge into it;
  //    otherwise select and pick a drag zone. Priority within a row:
  //    row-edge band (start a connection) → scrubable middle (scrub) → move.
  //    Port grabs go through the row-edge bands (a pin sits on its row's edge),
  //    which avoids the large port pick-radius stealing an adjacent row's port.
  const std::string hit = s.nodeAt(world);
  if (!hit.empty()) {
    if (!s.armedNodeId.empty() && hit != s.armedNodeId) {
      if (s.authorEdge(s.armedNodeId, s.armedPort, hit)) {
        s.armedNodeId.clear();
        s.armedPort.clear();
        s.buildModel();
        s.requestRender();
        return;
      }
      s.armedNodeId.clear();
      s.armedPort.clear();
    }
    s.selectNode(hit);  // tap-select works from title and every row
    const noodles::NodeData& n = s.model.nodes.at(hit);
    const double localX = world[0] - n.position[0];
    const double localY = world[1] - n.position[1];

    if (localY > n.layoutTitleHeight) {
      bool rowIsRel = false;
      bool rowIsOutput = false;
      const std::string rowPin =
          s.rowPinAt(n, localY, &rowIsRel, &rowIsOutput);
      const int rowKind = s.rowKindOf(n, rowPin);
      if (!rowPin.empty() && (rowKind == 1 || rowKind == 2)) {
        // Group-header caret row: arm a fold toggle (fires on a no-move
        // pointerUp); a real drag still moves the node. Header rows never
        // scrub and never start a link drag (their pin is a group NAME, not an
        // authorable property).
        s.foldTapNode = hit;
        s.foldTapGroup = rowPin;
      } else if (!rowPin.empty()) {
        const double edge = s.bandTouchWidth(n);
        // Outer connect bands take precedence over scrub: right = output side,
        // left = input side for input rows. Explicit output rows are output-only.
        // The band is fingertip-sized (bandTouchWidth), clamped so the
        // scrub/move middle always survives on narrow nodes.
        if (localX > n.size[0] - edge) {
          s.beginNewLinkDrag(hit, rowPin, /*isOutput=*/true);
          return;
        }
        if (localX < edge && !rowIsOutput) {
          s.beginNewLinkDrag(hit, rowPin, /*isOutput=*/false);
          return;
        }
        // Middle band: a scrubable value row scrubs.
        if (s.valueScrubEnabled) {
          auto vn = s.valueRows.find(hit);
          if (vn != s.valueRows.end()) {
            auto vr = vn->second.find(rowPin);
            if (vr != vn->second.end() && vr->second.scrubable) {
              s.drag = Impl::Drag::Scrub;
              s.scrubNodeId = hit;
              s.scrubPin = rowPin;
              s.scrubTypeToken = vr->second.typeToken;
              s.scrubValue0 = vr->second.value;
              s.scrubCurrent = vr->second.value;
              s.scrubBegan = false;
              s.requestRender();
              return;
            }
          }
        }
      }
    }

    // Node move: from the title bar, or as the fallback on a non-scrubable row
    // middle.
    s.drag = Impl::Drag::Node;
    s.dragNodeId = hit;
    s.grabOffsetX = world[0] - n.position[0];
    s.grabOffsetY = world[1] - n.position[1];
    s.requestRender();
    return;
  }

  // 2) A link (noodle) under the cursor: lift it for reconnect / disconnect.
  const int li = s.linkAt(world);
  if (li >= 0) {
    s.beginLinkReconnect(li, world);
    return;
  }

  // 3) Empty space: pan (and clear any armed port / selection intent).
  s.armedNodeId.clear();
  s.armedPort.clear();
  s.drag = Impl::Drag::Pan;
}

void GraphEditor::pointerMove(double x, double y) {
  Impl& s = *impl_;
  const double dxView = x - s.lastViewX;
  const double dyView = y - s.lastViewY;
  s.lastViewX = x;
  s.lastViewY = y;
  if (std::abs(x - s.downViewX) > 2.0 || std::abs(y - s.downViewY) > 2.0) {
    s.pointerMoved = true;
  }
  const noodles::Vec2d world = s.viewToGraph(x, y);

  switch (s.drag) {
    case Impl::Drag::Minimap: {
      // Follow the finger inside the minimap → recenter the viewport (recompute
      // the layout each move so the mapping tracks the pan it just applied).
      const Impl::MinimapLayout mm = s.computeMinimap();
      if (mm.visible) s.minimapPanTo(mm, x, y);
      break;
    }
    case Impl::Drag::Pan: {
      // Content follows the finger: world pan moves opposite the view delta.
      s.panX -= (dxView * s.contentScale) / s.zoom;
      s.panY -= (dyView * s.contentScale) / s.zoom;
      s.requestRender();
      break;
    }
    case Impl::Drag::Node: {
      auto it = s.model.nodes.find(s.dragNodeId);
      if (it != s.model.nodes.end()) {
        it->second.position =
            noodles::Vec2d(world[0] - s.grabOffsetX, world[1] - s.grabOffsetY);
        s.heldPositions[s.dragNodeId] = it->second.position;
        s.spatial.UpdateNode(
            s.dragNodeId,
            noodles::SpatialIndex::ComputeNodeBounds(it->second));
        s.rebuildLinkGeometry();
        // Move the baked geometry via the shared transform frame rather than
        // regenerating it: the node-quad, text, icon and port shaders all add
        // uNodeTransforms[slot], so quads AND text (which is cached and never
        // re-laid-out on a position-only change) follow the drag in lockstep.
        s.moveNodeLive(s.dragNodeId, it->second.position);
        s.requestRender();
      }
      break;
    }
    case Impl::Drag::Scrub: {
      // Absolute mapping from the gesture-start value, so re-crossing the origin
      // returns to value0 (no incremental drift).
      const double dxWorld = world[0] - s.downWorldX;
      const std::string& tk = s.scrubTypeToken;
      double newVal = s.scrubValue0;
      if (tk == "bool") {
        const double thr = 20.0;  // world units before the bool flips
        if (dxWorld > thr) newVal = 1.0;
        else if (dxWorld < -thr) newVal = 0.0;
      } else if (tk == "int" || tk == "int64" || tk == "uint") {
        // Quantized to whole steps (~50 world units per unit).
        newVal = std::round(s.scrubValue0 + dxWorld * 0.02);
      } else {
        const double step = std::max(std::abs(s.scrubValue0), 1.0) * 0.005;
        newVal = s.scrubValue0 + dxWorld * step;
      }
      if (newVal != s.scrubCurrent) s.applyScrub(newVal);  // throttled by change
      break;
    }
    case Impl::Drag::Link: {
      // Drag the free end of the dangling preview to the cursor, colored by
      // whether an opposite-side target sits under it.
      const bool wantOutput = !s.linkAnchorIsOutput;
      bool valid = false;
      if (s.linkIsRelationship) {
        const std::string tgt = s.nodeAt(world);
        valid = !tgt.empty() && tgt != s.linkAnchorNode;
      } else {
        const auto e = s.endpointAt(world, wantOutput);
        valid = e.found && e.nodeId != s.linkAnchorNode;
      }
      s.updateDragPreview(world, valid);
      s.requestRender();
      break;
    }
    case Impl::Drag::None:
      break;
  }
}

void GraphEditor::pointerUp(double x, double y) {
  Impl& s = *impl_;
  const noodles::Vec2d world = s.viewToGraph(x, y);
  const Impl::Drag drag = s.drag;
  s.drag = Impl::Drag::None;

  if (drag == Impl::Drag::Node) {
    if (auto it = s.model.nodes.find(s.dragNodeId); it != s.model.nodes.end()) {
      noodles::NodeData& n = it->second;
      if (s.pointerMoved) {
        // A real drag commits the new position, which also keeps the node
        // visible if it later has no edges.
        const noodles::Vec2d p = n.position;
        s.authorPosition(s.dragNodeId, p[0], p[1]);
      } else if (!s.foldTapGroup.empty() && s.foldTapNode == s.dragNodeId) {
        // Tap on a group-header caret row: toggle the group's fold and
        // re-lay-out (layoutNode re-derives the display pins from foldState).
        bool& folded = n.foldState[s.foldTapGroup];
        folded = !folded;
        s.layoutNodeOne(n);
        s.rebuildSpatialIndex();  // link bounds shift with the folded rows
        s.requestRender();
      } else {
        // Tap on the title CARET zone (the triangle at the title's left edge)
        // toggles whole-node collapse; the rest of the title stays a plain
        // select / move handle.
        const bool onTitle =
            n.titleCollapsed || s.grabOffsetY <= n.layoutTitleHeight;
        const double caretZone =
            std::max(n.layoutTitleHeight * 1.6, s.viewPtsToWorld(28.0));
        if (onTitle && s.grabOffsetX <= caretZone) {
          n.titleCollapsed = !n.titleCollapsed;
          s.layoutNodeOne(n);
          s.rebuildSpatialIndex();
          s.requestRender();
        }
      }
    }
    s.dragNodeId.clear();
    s.foldTapNode.clear();
    s.foldTapGroup.clear();
    return;
  }

  if (drag == Impl::Drag::Scrub) {
    // A tap (no move) on a bool row toggles it; on any other row it is just a
    // select (already done on pointerDown).
    if (!s.pointerMoved && s.scrubTypeToken == "bool") {
      s.applyScrub(s.scrubValue0 != 0.0 ? 0.0 : 1.0);
    }
    // Close the ONE undo/notify envelope opened lazily by applyScrub. The
    // final (committed) value is re-broadcast with live=false so subscribers
    // that defer heavy reconciliation to gesture end have their anchor.
    if (s.scrubBegan) {
      if (s.delegate.attributeEdited) {
        s.delegate.attributeEdited(s.scrubNodeId, s.scrubPin, /*live=*/false);
      }
      if (s.delegate.endEdit) s.delegate.endEdit();
      s.scrubBegan = false;
    }
    s.scrubNodeId.clear();
    s.scrubPin.clear();
    return;
  }

  if (drag == Impl::Drag::Link) {
    const bool moved = s.pointerMoved;

    // A no-move tap on a relationship output pin arms it for a target-node tap.
    if (!s.linkDragReconnect && s.linkFromRelOutPin && !moved) {
      s.armedNodeId = s.linkAnchorNode;
      s.armedPort = s.linkAnchorPort;
      s.status("port armed — tap a target node");
      s.clearDragPreview(/*pop=*/true);
      s.clearLinkDragState();
      return;
    }
    // A no-move tap that lifted a link is not an edit: restore it.
    if (s.linkDragReconnect && !moved) {
      s.clearDragPreview(/*pop=*/true);
      s.restoreLiftedLink();
      s.requestRender();
      s.clearLinkDragState();
      return;
    }

    const bool wantOutput = !s.linkAnchorIsOutput;

    // Resolve the drop target + build the author closure for the edge kind.
    bool valid = false;
    std::function<bool()> authorFn;
    if (s.linkIsRelationship) {
      // A relationship may target any whole node. Normalize to the
      // (owner, relationship name, target node) triple; the output side owns it.
      const std::string tgtNode = s.nodeAt(world);
      const std::string relName =
          s.linkDragReconnect ? s.linkLifted.sourcePort : s.linkAnchorPort;
      const std::string ownerNode =
          s.linkAnchorIsOutput ? s.linkAnchorNode : tgtNode;
      const std::string targetNode =
          s.linkAnchorIsOutput ? tgtNode : s.linkAnchorNode;
      valid = !tgtNode.empty() && !ownerNode.empty() && !targetNode.empty() &&
              !relName.empty() && ownerNode != targetNode;
      authorFn = [&s, ownerNode, relName, targetNode] {
        return s.document->authorRelationship(ownerNode, relName, targetNode);
      };
    } else {
      // Attribute connection target is an opposite-side pin (port or row). The
      // OUTPUT side owns the data source; the connection is authored ON the
      // INPUT side pointing to the OUTPUT side (mirrors CollectGraph).
      const auto t = s.endpointAt(world, wantOutput);
      valid = t.found && t.nodeId != s.linkAnchorNode;
      std::string inNode, inPort, outNode, outPort;
      if (s.linkAnchorIsOutput) {
        outNode = s.linkAnchorNode;
        outPort = s.linkAnchorPort;
        inNode = t.nodeId;
        inPort = t.port;
      } else {
        inNode = s.linkAnchorNode;
        inPort = s.linkAnchorPort;
        outNode = t.nodeId;
        outPort = t.port;
      }
      authorFn = [&s, inNode, inPort, outNode, outPort] {
        return s.document->authorConnection(inNode, inPort, outNode, outPort);
      };
    }

    // Distinguish an empty-space release (→ disconnect a lifted link) from an
    // invalid-target release (→ cancel): "over something" = a node or a port.
    const bool overSomething =
        !s.nodeAt(world).empty() ||
        noodles::NodeRenderManager::portAtPoint(s.model, world, s.hitRadius(),
                                                nullptr)
            .found;

    bool changed = false;
    bool restore = false;
    if (valid) {
      // Re-author (reconnect removes the old edge in the same envelope). The
      // old endpoints are pinned with authored positions so a node whose LAST
      // edge just moved away stays on the canvas.
      changed = s.documentEditBatch([&] {
        bool c = false;
        if (s.linkDragReconnect) c = s.removeLiftedEdge() || c;
        c = authorFn() || c;
        if (c && s.linkDragReconnect) s.persistLiftedEndpointPositions();
        return c;
      }, /*topologyEdit=*/true);
      restore = !changed && s.linkDragReconnect;
    } else if (s.linkDragReconnect && !overSomething) {
      // Disconnect: the edge is removed but BOTH former endpoints stay on the
      // canvas (pinned via authored positions); explicit node removal is the
      // title-row long-press.
      changed = s.documentEditBatch([&] {
        const bool c = s.removeLiftedEdge();
        if (c) s.persistLiftedEndpointPositions();
        return c;
      }, /*topologyEdit=*/true);
      restore = !changed;
    } else {
      restore = s.linkDragReconnect;  // new drag to nowhere / invalid target
    }

    s.clearDragPreview(/*pop=*/!changed);
    if (restore) s.restoreLiftedLink();
    if (changed) s.buildModel();
    s.requestRender();
    s.clearLinkDragState();
    return;
  }

  // A background tap (no drag) clears selection.
  if (drag == Impl::Drag::Pan && !s.pointerMoved) {
    if (!s.selectedNodeId.empty()) {
      if (auto it = s.model.nodes.find(s.selectedNodeId);
          it != s.model.nodes.end()) {
        it->second.selected = false;
      }
      s.selectedNodeId.clear();
      if (s.delegate.selectionChanged) s.delegate.selectionChanged(std::string());
      s.requestRender();
    }
  }
}

void GraphEditor::pinchBegin() {
  impl_->pinching = true;
  // Snapshot the zoom so cumulative-since-begin `scale` maps to an absolute
  // zoom (pinchStartZoom * scale) rather than compounding on the live zoom.
  impl_->pinchStartZoom = impl_->zoom;
}

void GraphEditor::pinchUpdate(double scale, double anchorX, double anchorY) {
  Impl& s = *impl_;
  // `scale` is cumulative since pinchBegin, so the target zoom is anchored to
  // the gesture-start zoom — NOT s.zoom (which already includes this gesture's
  // earlier updates and would compound: two 1.5× updates would give 2.25×).
  const double newZoom =
      std::clamp(s.pinchStartZoom * scale, s.minZoom(), kZoomMax);
  // Keep the world point under the anchor fixed across the zoom change.
  const double ax = anchorX * s.contentScale;
  const double ay = anchorY * s.contentScale;
  const double worldX = s.panX + ax / s.zoom;
  const double worldY = s.panY + ay / s.zoom;
  s.zoom = newZoom;
  s.panX = worldX - ax / newZoom;
  s.panY = worldY - ay / newZoom;
  s.requestRender();
}

void GraphEditor::pinchEnd() { impl_->pinching = false; }

void GraphEditor::setViewportPan(double panX, double panY) {
  impl_->panX = panX;
  impl_->panY = panY;
}
double GraphEditor::panX() const { return impl_->panX; }
double GraphEditor::panY() const { return impl_->panY; }

void GraphEditor::setZoom(double zoom) {
  impl_->zoom = std::clamp(zoom, impl_->minZoom(), kZoomMax);
}
double GraphEditor::zoom() const { return impl_->zoom; }

bool GraphEditor::frameAll(double paddingPoints) {
  Impl& s = *impl_;
  double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
  if (s.viewportW <= 0 || s.viewportH <= 0 ||
      !s.contentBounds(&minX, &minY, &maxX, &maxY)) {
    return false;
  }

  const double width = maxX - minX;
  const double height = maxY - minY;
  if (width <= 0.0 || height <= 0.0) return false;

  const double scale = s.contentScale > 0.0f ? s.contentScale : 1.0;
  const double paddingPx = std::max(0.0, paddingPoints) * scale;
  const double availableWidth = s.viewportW - paddingPx * 2.0;
  const double availableHeight = s.viewportH - paddingPx * 2.0;
  if (availableWidth <= 0.0 || availableHeight <= 0.0) return false;

  const double fitZoom =
      std::min({availableWidth / width, availableHeight / height, 1.0});
  s.zoom = std::clamp(fitZoom, s.minZoom(), kZoomMax);
  s.panX = (minX + maxX) * 0.5 - s.viewportW / (2.0 * s.zoom);
  s.panY = (minY + maxY) * 0.5 - s.viewportH / (2.0 * s.zoom);
  s.requestRender();
  return true;
}

std::string GraphEditor::selectedNodeId() const {
  return impl_->selectedNodeId;
}

std::size_t GraphEditor::nodeCount() const {
  return impl_->model.nodes.size();
}

std::size_t GraphEditor::linkCount() const {
  return impl_->model.links.size();
}

bool GraphEditor::nodePosition(const std::string& id, double* x,
                                    double* y) const {
  auto it = impl_->model.nodes.find(id);
  if (it == impl_->model.nodes.end()) return false;
  const noodles::Vec2d p = it->second.position;
  if (x) *x = p[0];
  if (y) *y = p[1];
  return true;
}

bool GraphEditor::nodeSize(const std::string& id, double* w,
                                double* h) const {
  auto it = impl_->model.nodes.find(id);
  if (it == impl_->model.nodes.end()) return false;
  if (w) *w = it->second.size[0];
  if (h) *h = it->second.size[1];
  return true;
}

std::vector<GraphPinInfo> GraphEditor::nodePins(const std::string& id) const {
  std::vector<GraphPinInfo> out;
  auto it = impl_->model.nodes.find(id);
  if (it == impl_->model.nodes.end()) return out;
  const noodles::NodeData& n = it->second;

  const auto vn = impl_->valueRows.find(id);

  for (int i = 0; i < static_cast<int>(n.inputPins.size()); ++i) {
    const std::string& name = n.inputPins[i];
    noodles::Vec2d c = impl_->nodeRenderer.resolvePortPosition(n, name, false);
    GraphPinInfo info;
    info.name = name;
    info.isOutput = false;
    info.isRelationship = n.relationshipInputPins.count(name) > 0;
    info.centerX = c[0];
    info.centerY = c[1];
    if (auto tt = n.originalInputPinTypes.find(name);
        tt != n.originalInputPinTypes.end()) {
      info.typeText = tt->second;
    }
    if (vn != impl_->valueRows.end()) {
      if (auto vr = vn->second.find(name); vr != vn->second.end()) {
        info.hasValue = true;
        info.isScrubable = vr->second.scrubable;
        info.value = vr->second.value;
      }
    }
    out.push_back(std::move(info));
  }
  for (int i = 0; i < static_cast<int>(n.outputPins.size()); ++i) {
    const std::string& name = n.outputPins[i];
    noodles::Vec2d c = impl_->nodeRenderer.resolvePortPosition(n, name, true);
    GraphPinInfo info;
    info.name = name;
    info.isOutput = true;
    info.isRelationship = n.relationshipOutputPins.count(name) > 0;
    info.centerX = c[0];
    info.centerY = c[1];
    if (auto tt = n.originalOutputPinTypes.find(name);
        tt != n.originalOutputPinTypes.end()) {
      info.typeText = tt->second;
    }
    if (vn != impl_->valueRows.end()) {
      if (auto vr = vn->second.find(name); vr != vn->second.end()) {
        info.hasValue = true;
        info.isScrubable = vr->second.scrubable;
        info.value = vr->second.value;
      }
    }
    out.push_back(std::move(info));
  }
  // Dual pins (attribute connection sources) expose an extra output-side port.
  for (const std::string& dual : n.dualPinNames) {
    noodles::Vec2d c = impl_->nodeRenderer.resolvePortPosition(n, dual, true);
    GraphPinInfo info;
    info.name = dual;
    info.isOutput = true;
    info.centerX = c[0];
    info.centerY = c[1];
    out.push_back(std::move(info));
  }
  return out;
}

std::vector<GraphLinkInfo> GraphEditor::links() const {
  std::vector<GraphLinkInfo> out;
  out.reserve(impl_->model.links.size());
  for (const noodles::LinkData& l : impl_->model.links) {
    if (l.isDangling) continue;  // skip the transient drag-preview link
    GraphLinkInfo info;
    info.sourceNodeId = l.sourceNodeId;
    info.sourcePort = l.sourcePort;
    info.targetNodeId = l.targetNodeId;
    info.targetPort = l.targetPort;
    info.isRelationship = l.isRelationship;
    info.startX = l.start[0];
    info.startY = l.start[1];
    info.endX = l.end[0];
    info.endY = l.end[1];
    out.push_back(std::move(info));
  }
  return out;
}

bool GraphEditor::activeLinkPreview(GraphLinkInfo* preview) const {
  if (!preview || !impl_->dragPreviewActive || impl_->model.links.empty()) {
    return false;
  }
  const noodles::LinkData& link = impl_->model.links.back();
  if (!link.isDangling) return false;
  preview->sourceNodeId = impl_->linkAnchorIsOutput
                              ? impl_->linkAnchorNode
                              : std::string();
  preview->sourcePort = impl_->linkAnchorIsOutput
                            ? impl_->linkAnchorPort
                            : std::string();
  preview->targetNodeId = impl_->linkAnchorIsOutput
                              ? std::string()
                              : impl_->linkAnchorNode;
  preview->targetPort = impl_->linkAnchorIsOutput
                            ? std::string()
                            : impl_->linkAnchorPort;
  preview->isRelationship = link.isRelationship;
  preview->startX = link.start[0];
  preview->startY = link.start[1];
  preview->endX = link.end[0];
  preview->endY = link.end[1];
  return true;
}

bool GraphEditor::minimapVisible() const {
  return impl_->computeMinimap().visible;
}

bool GraphEditor::minimapRect(double* x, double* y, double* w,
                                   double* h) const {
  const Impl::MinimapLayout m = impl_->computeMinimap();
  if (!m.visible) return false;
  if (x) *x = m.x;
  if (y) *y = m.y;
  if (w) *w = m.w;
  if (h) *h = m.h;
  return true;
}

bool GraphEditor::minimapPointToWorld(double sx, double sy, double* wx,
                                           double* wy) const {
  const Impl::MinimapLayout m = impl_->computeMinimap();
  if (!m.visible) return false;
  Impl::minimapToWorld(m, sx, sy, wx, wy);
  return true;
}

void GraphEditor::viewToGraph(double vx, double vy, double* gx,
                                   double* gy) const {
  const noodles::Vec2d w = impl_->viewToGraph(vx, vy);
  if (gx) *gx = w[0];
  if (gy) *gy = w[1];
}

bool GraphEditor::hitsGraphElementAt(double x, double y) const {
  return impl_->hitsGraphElementAt(x, y);
}

bool GraphEditor::longPressAt(double x, double y) {
  Impl& s = *impl_;
  // A long-press only fires from a stationary press. A press that already
  // moved (drag) or landed on a link/scrub/minimap interaction is not a node
  // removal. pointerMoved is only meaningful while a pointer stream is active
  // (the finger long-press recognizer fires without a pointerDown, leaving
  // stale state from the previous gesture).
  if (s.drag != Impl::Drag::None && s.pointerMoved) return false;
  if (s.drag == Impl::Drag::Link || s.drag == Impl::Drag::Scrub ||
      s.drag == Impl::Drag::Minimap) {
    return false;
  }
  const noodles::Vec2d world = s.viewToGraph(x, y);
  const std::string hit = s.nodeAt(world);
  if (hit.empty()) return false;
  const noodles::NodeData& n = s.model.nodes.at(hit);
  const double localY = world[1] - n.position[1];
  if (!n.titleCollapsed && localY > n.layoutTitleHeight) return false;
  const std::string name = n.name.empty() ? hit : n.name;
  // Cancel the in-flight press state (a pencil press already ran pointerDown,
  // arming a node move); the trailing pointerUp then falls through Drag::None.
  s.drag = Impl::Drag::None;
  s.dragNodeId.clear();
  s.foldTapNode.clear();
  s.foldTapGroup.clear();
  if (s.removeNodeFromCanvas(hit)) {
    s.status("removed " + name + " from graph");
    return true;
  }
  return false;
}

bool GraphEditor::addNodeAt(const std::string& nodeId, double viewX,
                            double viewY) {
  Impl& s = *impl_;
  if (!s.document || nodeId.empty() || !s.document->containsNode(nodeId)) {
    return false;
  }
  const noodles::Vec2d world = s.viewToGraph(viewX, viewY);
  // Center an already-displayed node's box under the drop point; a brand-new
  // node uses the default box size until its first layout.
  double w = 200.0, h = 100.0;
  if (auto it = s.model.nodes.find(nodeId); it != s.model.nodes.end()) {
    w = it->second.size[0];
    h = it->second.size[1];
  }
  const double px = world[0] - w * 0.5;
  const double py = world[1] - h * 0.5;
  const bool changed = s.documentEditBatch(
      [&] { return s.document->setNodePosition(nodeId, px, py); },
      /*topologyEdit=*/true);
  s.autoPositionedNodeIds.erase(nodeId);
  s.heldPositions[nodeId] = noodles::Vec2d(px, py);
  s.buildModel();
  s.selectNode(nodeId);
  s.requestRender();
  return changed;
}

}  // namespace noodles::apple
