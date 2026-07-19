// noodles_graph_view_test.cpp — the shared graph-view host (NoodlesGraphView),
// exercised HEADLESS (no GL context). Pins the model build (traversal parity
// with usd_graph::GraphJson), the property→pin mapping, link topology, position
// resolution (authored / grid), laid-out pin geometry, pointer hit-selection,
// and the pin-drag edge-authoring state machine through a stub delegate.
//
// NO GL calls here — setDocument + hit-model must be usable without a
// context (initializeGL is never called).

#include <noodles/demo/GraphEditor.h>
#include <noodles/demo/InMemoryGraphDocument.h>



#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ig = noodles::demo;

#define INK_CHECK(cond)                                                       \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      return false;                                                           \
    }                                                                         \
  } while (0)

namespace {

using Document = std::shared_ptr<ig::InMemoryGraphDocument>;

ig::GraphNode Node(std::string id, bool positioned = false, double x = 0.0,
                   double y = 0.0) {
  ig::GraphNode node;
  node.id = std::move(id);
  const std::size_t slash = node.id.rfind('/');
  node.name = slash == std::string::npos ? node.id : node.id.substr(slash + 1);
  node.hasPosition = positioned;
  node.posX = x;
  node.posY = y;
  return node;
}

ig::GraphProperty Numeric(std::string name, std::string type = "float",
                          double value = 0.0) {
  ig::GraphProperty property;
  property.name = std::move(name);
  property.kind = ig::GraphPropertyKind::Attribute;
  property.type = std::move(type);
  property.hasValue = true;
  property.isScrubable = true;
  property.numericValue = value;
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%g", value);
  property.displayValue = property.type == "bool"
                              ? (value == 0.0 ? "off" : "on")
                              : std::string(buffer);
  return property;
}

ig::GraphProperty Display(std::string name, std::string type,
                          std::string value) {
  ig::GraphProperty property;
  property.name = std::move(name);
  property.kind = ig::GraphPropertyKind::Attribute;
  property.type = std::move(type);
  property.hasValue = true;
  property.displayValue = std::move(value);
  return property;
}

ig::GraphProperty Port(std::string name, std::string type,
                       ig::GraphPropertyDirection direction) {
  ig::GraphProperty property;
  property.name = std::move(name);
  property.kind = ig::GraphPropertyKind::Attribute;
  property.type = std::move(type);
  property.direction = direction;
  return property;
}

ig::GraphProperty Relationship(std::string name) {
  ig::GraphProperty property;
  property.name = std::move(name);
  property.kind = ig::GraphPropertyKind::Relationship;
  property.type = "rel";
  return property;
}

Document NewDocument(std::initializer_list<ig::GraphNode> nodes) {
  auto document = std::make_shared<ig::InMemoryGraphDocument>();
  for (ig::GraphNode node : nodes) document->addNode(std::move(node));
  return document;
}

bool HasEdge(const Document& document, const std::string& source,
             const std::string& sourcePort, const std::string& target,
             const std::string& targetPort, bool relationship) {
  for (const ig::GraphEdge& edge : document->snapshot(0.0).edges) {
    if (edge.sourceNodeId == source && edge.sourcePort == sourcePort &&
        edge.targetNodeId == target && edge.targetPort == targetPort &&
        edge.isRelationship == relationship) {
      return true;
    }
  }
  return false;
}

double NumericValue(const Document& document, const std::string& nodeId,
                    const std::string& propertyName, double frame = 0.0) {
  for (const ig::GraphNode& node : document->snapshot(frame).nodes) {
    if (node.id != nodeId) continue;
    for (const ig::GraphProperty& property : node.properties) {
      if (property.name == propertyName) return property.numericValue;
    }
  }
  return 0.0;
}

// A: --(custom:link rel)--> B, with an authored ui:nodegraph:node:pos.
// C.in --(attr connection)--> B.out. Typeless prims keep the property set (and
// therefore the pin layout) deterministic across OpenUSD versions.
Document MakeStage() {
  auto document = NewDocument(
      {Node("/Root/A", true, 500.0, 300.0), Node("/Root/B"),
       Node("/Root/C")});
  document->addProperty("/Root/A", Relationship("custom:link"));
  document->addProperty("/Root/A", Display("style", "token", "solid"));
  document->addProperty("/Root/B", Numeric("out"));
  document->addProperty("/Root/C", Numeric("in", "float", 0.35));
  document->authorRelationship("/Root/A", "custom:link", "/Root/B");
  document->authorConnection("/Root/C", "in", "/Root/B", "out");
  return document;
}

// Attribute connection D.in ← S.out, plus a re-target node T (surfaced by a
// relationship so it becomes a graph node; its unconnected "in" row is a clean
// reconnect target). Positions are left to autolayout (authoring
// ui:nodegraph:node:pos would add a very wide value row and overlap the nodes).
Document MakeLinkStage() {
  auto document = NewDocument({Node("/L/S"), Node("/L/D"), Node("/L/T")});
  document->addProperty("/L/S", Numeric("out", "float", 1.0));
  document->addProperty("/L/D", Numeric("in"));
  document->addProperty("/L/T", Numeric("in"));
  document->addProperty("/L/T", Relationship("anchor"));
  document->authorConnection("/L/D", "in", "/L/S", "out");
  document->authorRelationship("/L/T", "anchor", "/L/D");
  return document;
}

// A --(custom:link rel)--> B, with a re-target node C (surfaced by its own
// relationship). Positions are AUTHORED with A directly above B: a relationship
// noodle anchors at its target's top-center, so placing the source above the
// target keeps the noodle's target end (just above B's top edge) in open space
// where it can be grabbed for a reconnect. (The layered auto-layout — which flows
// connected prims left→right — is exercised by the fresh / fan layout tests; this
// test pins the reconnect edit, so it fixes the geometry rather than depending on
// layout.) C sits to the side as the reconnect target.
Document MakeRelStage() {
  auto document = NewDocument({Node("/R/A", true, 0.0, 0.0),
                               Node("/R/B", true, 0.0, 600.0),
                               Node("/R/C", true, 900.0, 0.0)});
  document->addProperty("/R/A", Relationship("custom:link"));
  document->addProperty("/R/C", Relationship("anchor"));
  document->authorRelationship("/R/A", "custom:link", "/R/B");
  // C points at A (not B) so the A→B link is unambiguous near its B end.
  document->authorRelationship("/R/C", "anchor", "/R/A");
  return document;
}

bool PtOverNode(const ig::NoodlesGraphView& v,
                const std::vector<std::string>& ids, double x, double y) {
  for (const std::string& id : ids) {
    double px = 0, py = 0, pw = 0, ph = 0;
    if (!v.nodePosition(id, &px, &py) || !v.nodeSize(id, &pw, &ph)) continue;
    if (x >= px && x <= px + pw && y >= py && y <= py + ph) return true;
  }
  return false;
}

// A grab point ON the link running from p0 toward p1 that hitsGraphElementAt
// reports and that is NOT over any node (so pointerDown there grabs the link).
// Scans t from the p0 side inward, with perpendicular offsets to catch the
// bowed noodle curve; the returned point is therefore near the p0 END of the
// link, in the inter-node gap. Callers order (p0,p1) to grab the desired end.
bool FindLinkGrab(const ig::NoodlesGraphView& v,
                  const std::vector<std::string>& ids, double x0, double y0,
                  double x1, double y1, double* gx, double* gy) {
  const double dx = x1 - x0, dy = y1 - y0;
  const double len = std::sqrt(dx * dx + dy * dy);
  const double nx = len > 0 ? -dy / len : 0.0;
  const double ny = len > 0 ? dx / len : 0.0;
  for (int i = 2; i <= 8; ++i) {
    const double t = i / 10.0;
    const double bx = x0 + dx * t, by = y0 + dy * t;
    for (double off : {0.0, 6.0, -6.0, 12.0, -12.0, 24.0, -24.0, 40.0, -40.0}) {
      const double x = bx + nx * off, y = by + ny * off;
      if (PtOverNode(v, ids, x, y)) continue;
      if (v.hitsGraphElementAt(x, y)) {
        *gx = x;
        *gy = y;
        return true;
      }
    }
  }
  return false;
}

// The graph-space center of a node box (a safe drop target for a whole-node /
// relationship release).
void NodeCenter(const ig::NoodlesGraphView& v, const std::string& id, double* cx,
                double* cy) {
  double x = 0, y = 0, w = 0, h = 0;
  v.nodePosition(id, &x, &y);
  v.nodeSize(id, &w, &h);
  *cx = x + w * 0.5;
  *cy = y + h * 0.5;
}

bool HasPin(const std::vector<ig::GraphPinInfo>& pins, const std::string& name,
            bool isOutput) {
  for (const auto& p : pins) {
    if (p.name == name && p.isOutput == isOutput) return true;
  }
  return false;
}

const ig::GraphPinInfo* FindPin(const std::vector<ig::GraphPinInfo>& pins,
                                const std::string& name, bool isOutput) {
  for (const auto& p : pins) {
    if (p.name == name && p.isOutput == isOutput) return &p;
  }
  return nullptr;
}

// True when two nodes' laid-out boxes do not overlap (autolayout invariant).
bool NodesDisjoint(const ig::NoodlesGraphView& v, const std::string& a,
                   const std::string& b) {
  double ax, ay, aw, ah, bx, by, bw, bh;
  if (!v.nodePosition(a, &ax, &ay) || !v.nodeSize(a, &aw, &ah)) return false;
  if (!v.nodePosition(b, &bx, &by) || !v.nodeSize(b, &bw, &bh)) return false;
  const bool overlapX = ax < bx + bw && bx < ax + aw;
  const bool overlapY = ay < by + bh && by < ay + ah;
  return !(overlapX && overlapY);
}

// Centre-to-centre distance between two laid-out nodes.
double NodeCenterDistance(const ig::NoodlesGraphView& v, const std::string& a,
                          const std::string& b) {
  double ax, ay, aw, ah, bx, by, bw, bh;
  v.nodePosition(a, &ax, &ay);
  v.nodeSize(a, &aw, &ah);
  v.nodePosition(b, &bx, &by);
  v.nodeSize(b, &bw, &bh);
  const double dx = (ax + aw * 0.5) - (bx + bw * 0.5);
  const double dy = (ay + ah * 0.5) - (by + bh * 0.5);
  return std::sqrt(dx * dx + dy * dy);
}

bool TestModelTopologyAndPins() {
  ig::NoodlesGraphView view;
  view.setDocument(MakeStage());

  // Same node set as GraphJson: A (rel source), B (rel + connection target),
  // C (connection source).
  INK_CHECK(view.nodeCount() == 3);
  // One relationship link (A→B) + one data link (B.out→C.in).
  INK_CHECK(view.linkCount() == 2);

  // Relationship → output pin; attribute → input pin.
  auto aPins = view.nodePins("/Root/A");
  const ig::GraphPinInfo* aRel = FindPin(aPins, "custom:link", /*out=*/true);
  INK_CHECK(aRel != nullptr);
  INK_CHECK(aRel->isRelationship);

  auto cPins = view.nodePins("/Root/C");
  INK_CHECK(HasPin(cPins, "in", /*out=*/false));

  // B.out is the data-flow SOURCE of the connection, so it carries an
  // input-side attribute pin AND a mirrored output (dual) pin.
  auto bPins = view.nodePins("/Root/B");
  INK_CHECK(HasPin(bPins, "out", /*out=*/false));
  INK_CHECK(HasPin(bPins, "out", /*out=*/true));
  std::printf("  model topology + pin mapping ok\n");
  return true;
}

bool TestExplicitOutputDirectionAndType() {
  auto document = NewDocument(
      {Node("/Ports/Source", true, 100.0, 100.0),
       Node("/Ports/Sink", true, 700.0, 100.0)});
  document->addProperty(
      "/Ports/Source",
      Port("output", "image", ig::GraphPropertyDirection::Output));
  document->addProperty(
      "/Ports/Sink",
      Port("input", "image", ig::GraphPropertyDirection::Input));

  ig::NoodlesGraphView view;
  view.setDocument(document);
  const auto sourcePins = view.nodePins("/Ports/Source");
  const ig::GraphPinInfo* output =
      FindPin(sourcePins, "output", /*isOutput=*/true);
  INK_CHECK(output != nullptr);
  INK_CHECK(output->typeText == "image");
  INK_CHECK(FindPin(sourcePins, "output", /*isOutput=*/false) == nullptr);

  double sourceX = 0.0, sourceWidth = 0.0;
  INK_CHECK(view.nodePosition("/Ports/Source", &sourceX, nullptr));
  INK_CHECK(view.nodeSize("/Ports/Source", &sourceWidth, nullptr));
  ig::GraphLinkInfo preview;
  view.pointerDown(sourceX + sourceWidth * 0.03, output->centerY);
  INK_CHECK(!view.activeLinkPreview(&preview));
  view.pointerUp(sourceX + sourceWidth * 0.03, output->centerY);

  const auto sinkPins = view.nodePins("/Ports/Sink");
  const ig::GraphPinInfo* input =
      FindPin(sinkPins, "input", /*isOutput=*/false);
  INK_CHECK(input != nullptr);
  INK_CHECK(input->typeText == "image");
  INK_CHECK(FindPin(sinkPins, "input", /*isOutput=*/true) == nullptr);

  INK_CHECK(document->authorConnection("/Ports/Sink", "input",
                                       "/Ports/Source", "output"));
  view.refresh();
  INK_CHECK(view.linkCount() == 1);
  const auto links = view.links();
  INK_CHECK(links.size() == 1);
  INK_CHECK(links[0].sourceNodeId == "/Ports/Source");
  INK_CHECK(links[0].sourcePort == "output");
  INK_CHECK(std::abs(links[0].startX - output->centerX) < 1e-6);
  INK_CHECK(std::abs(links[0].startY - output->centerY) < 1e-6);
  std::printf("  explicit output row carries its image type and link source ok\n");
  return true;
}

bool TestUnconnectedRowDragPreviewUsesRowEdge() {
  auto document =
      NewDocument({Node("/Preview/Source", true, 120.0, 90.0)});
  document->addProperty("/Preview/Source", Numeric("top"));
  document->addProperty("/Preview/Source", Numeric("middle"));
  document->addProperty("/Preview/Source", Numeric("bottom"));

  ig::NoodlesGraphView view;
  view.setDocument(document);
  const auto pins = view.nodePins("/Preview/Source");
  const ig::GraphPinInfo* row = FindPin(pins, "top", /*isOutput=*/false);
  INK_CHECK(row != nullptr);

  double x = 0.0, y = 0.0, width = 0.0, height = 0.0;
  INK_CHECK(view.nodePosition("/Preview/Source", &x, &y));
  INK_CHECK(view.nodeSize("/Preview/Source", &width, &height));
  INK_CHECK(std::abs(row->centerY - (y + height * 0.5)) > 1.0);
  const double rightBandX = x + width * 0.97;

  // The preview is initialized on pointerDown, before any move event, and the
  // fixed source stays on the grabbed row's right edge for the whole drag.
  view.pointerDown(rightBandX, row->centerY);
  ig::GraphLinkInfo preview;
  INK_CHECK(view.activeLinkPreview(&preview));
  INK_CHECK(std::abs(preview.startX - (x + width)) < 1e-6);
  INK_CHECK(std::abs(preview.startY - row->centerY) < 1e-6);
  INK_CHECK(std::abs(preview.endX - preview.startX) < 1e-6);
  INK_CHECK(std::abs(preview.endY - preview.startY) < 1e-6);

  view.pointerMove(rightBandX + 160.0, row->centerY + 75.0);
  INK_CHECK(view.activeLinkPreview(&preview));
  INK_CHECK(std::abs(preview.startX - (x + width)) < 1e-6);
  INK_CHECK(std::abs(preview.startY - row->centerY) < 1e-6);
  INK_CHECK(std::abs(preview.startY - (y + height * 0.5)) > 1.0);

  view.pointerUp(rightBandX + 160.0, row->centerY + 75.0);
  INK_CHECK(!view.activeLinkPreview(&preview));
  std::printf("  unconnected-row drag preview stays on right row edge ok\n");
  return true;
}

bool TestPositionsAndLayout() {
  ig::NoodlesGraphView view;
  view.setDocument(MakeStage());

  // A carries an authored position; it wins over the grid.
  double ax = 0, ay = 0;
  INK_CHECK(view.nodePosition("/Root/A", &ax, &ay));
  INK_CHECK(ax == 500.0 && ay == 300.0);

  // B and C have no authored/held position, so autolayout places them (near
  // their connected neighbours) instead of the old fixed grid. Assert the
  // invariants layout guarantees: both are positioned and no two node boxes
  // overlap A or each other.
  double bx = 0, by = 0, cx = 0, cy = 0;
  INK_CHECK(view.nodePosition("/Root/B", &bx, &by));
  INK_CHECK(view.nodePosition("/Root/C", &cx, &cy));
  INK_CHECK(NodesDisjoint(view, "/Root/A", "/Root/B"));
  INK_CHECK(NodesDisjoint(view, "/Root/A", "/Root/C"));
  INK_CHECK(NodesDisjoint(view, "/Root/B", "/Root/C"));

  // Layout: input pins sit on the left edge, output pins on the right edge, and
  // every pin row center falls below the title bar and inside the node box.
  double cw = 0, ch = 0;
  INK_CHECK(view.nodeSize("/Root/C", &cw, &ch));
  INK_CHECK(cw > 0.0 && ch > 0.0);
  auto cPins = view.nodePins("/Root/C");
  const ig::GraphPinInfo* cIn = FindPin(cPins, "in", /*out=*/false);
  INK_CHECK(cIn != nullptr);
  INK_CHECK(cIn->centerX == cx);                 // left edge
  INK_CHECK(cIn->centerY > cy && cIn->centerY < cy + ch);

  auto aPins = view.nodePins("/Root/A");
  double aw = 0, ah = 0;
  INK_CHECK(view.nodeSize("/Root/A", &aw, &ah));
  const ig::GraphPinInfo* aOut = FindPin(aPins, "custom:link", /*out=*/true);
  INK_CHECK(aOut != nullptr);
  INK_CHECK(std::abs(aOut->centerX - (ax + aw)) < 1e-6);  // right edge
  INK_CHECK(aOut->centerY > ay && aOut->centerY < ay + ah);
  std::printf("  positions + layout row centers ok\n");
  return true;
}

bool TestPointerSelectsNode() {
  ig::NoodlesGraphView view;
  std::string lastSelected;
  int selectionCalls = 0;
  ig::GraphEditDelegate del;
  del.selectionChanged = [&](const std::string& id) {
    lastSelected = id;
    ++selectionCalls;
  };
  view.setDelegate(del);
  view.setDocument(MakeStage());

  // Tap the center of C (at default pan/zoom/scale, view point == graph point).
  double cx = 0, cy = 0, cw = 0, ch = 0;
  view.nodePosition("/Root/C", &cx, &cy);
  view.nodeSize("/Root/C", &cw, &ch);
  const double px = cx + cw * 0.5;
  const double py = cy + ch * 0.5;
  view.pointerDown(px, py);
  view.pointerUp(px, py);

  INK_CHECK(view.selectedNodeId() == "/Root/C");
  INK_CHECK(selectionCalls >= 1 && lastSelected == "/Root/C");
  std::printf("  pointer hit selects node ok\n");
  return true;
}

bool TestPinDragAuthorsEdge() {
  std::shared_ptr<ig::InMemoryGraphDocument> stage = MakeStage();
  ig::NoodlesGraphView view;

  int beginCount = 0, endCount = 0;
  int topologyCount = 0;
  ig::GraphEditDelegate del;
  del.beginEdit = [&]() { ++beginCount; };
  del.endEdit = [&]() { ++endCount; };
  del.topologyEdited = [&]() { ++topologyCount; };
  view.setDelegate(del);
  view.setDocument(stage);

  // Grab A's output relationship port and drag onto node C.
  auto aPins = view.nodePins("/Root/A");
  const ig::GraphPinInfo* aOut = FindPin(aPins, "custom:link", /*out=*/true);
  INK_CHECK(aOut != nullptr);
  double cx = 0, cy = 0, cw = 0, ch = 0;
  view.nodePosition("/Root/C", &cx, &cy);
  view.nodeSize("/Root/C", &cw, &ch);

  view.pointerDown(aOut->centerX, aOut->centerY);
  view.pointerMove(cx + cw * 0.5, cy + ch * 0.5);
  view.pointerUp(cx + cw * 0.5, cy + ch * 0.5);

  // The document gained A --custom:link--> C (plus the original → B).
  INK_CHECK(HasEdge(stage, "/Root/A", "custom:link", "/Root/B", "", true));
  INK_CHECK(HasEdge(stage, "/Root/A", "custom:link", "/Root/C", "", true));
  INK_CHECK(beginCount >= 1 && endCount >= 1);
  INK_CHECK(topologyCount == 1);

  // The rebuilt model reflects the new edge.
  INK_CHECK(view.linkCount() == 3);
  std::printf("  pin-drag authors relationship edge ok\n");
  return true;
}

// Value rows: a scrubable float shows "<type> · <value>" in its type slot and is
// flagged scrubable; a token attribute is display-only.
bool TestValueRowCapture() {
  ig::NoodlesGraphView view;
  view.setDocument(MakeStage());

  auto cPins = view.nodePins("/Root/C");
  const ig::GraphPinInfo* cIn = FindPin(cPins, "in", /*out=*/false);
  INK_CHECK(cIn != nullptr);
  INK_CHECK(cIn->hasValue);
  INK_CHECK(cIn->isScrubable);
  INK_CHECK(cIn->typeText.find("float") != std::string::npos);
  INK_CHECK(cIn->typeText.find("0.35") != std::string::npos);

  auto aPins = view.nodePins("/Root/A");
  const ig::GraphPinInfo* aStyle = FindPin(aPins, "style", /*out=*/false);
  INK_CHECK(aStyle != nullptr);
  INK_CHECK(aStyle->hasValue);
  INK_CHECK(!aStyle->isScrubable);           // token → display-only
  INK_CHECK(aStyle->typeText.find("solid") != std::string::npos);
  std::printf("  value-row capture + type composition ok\n");
  return true;
}

// A display-only asset row has three distinct gestures: a stationary middle
// tap activates host UI, a middle drag moves the node, and the edge band starts
// connection authoring. Only the first one may fire attributeActivated.
bool TestDisplayOnlyRowActivationPriority() {
  auto document =
      NewDocument({Node("/Asset/Source", true, 120.0, 90.0)});
  ig::GraphProperty path =
      Display("path", "asset", "picked/source image.exr");
  path.stringValue = "/Volumes/Demo/picked/source image.exr";
  document->addProperty("/Asset/Source", std::move(path));

  ig::NoodlesGraphView view;
  int beginCount = 0;
  int endCount = 0;
  int activationCount = 0;
  int attributeEditCount = 0;
  int topologyCount = 0;
  std::string activatedNode;
  std::string activatedAttribute;
  ig::GraphEditDelegate delegate;
  delegate.beginEdit = [&] { ++beginCount; };
  delegate.endEdit = [&] { ++endCount; };
  delegate.attributeActivated =
      [&](const std::string& nodeId, const std::string& attributeName) {
        ++activationCount;
        activatedNode = nodeId;
        activatedAttribute = attributeName;
      };
  delegate.attributeEdited =
      [&](const std::string&, const std::string&, bool) {
        ++attributeEditCount;
      };
  delegate.topologyEdited = [&] { ++topologyCount; };
  view.setDelegate(delegate);
  view.setDocument(document);

  auto pins = view.nodePins("/Asset/Source");
  const ig::GraphPinInfo* pathPin =
      FindPin(pins, "path", /*isOutput=*/false);
  INK_CHECK(pathPin != nullptr);
  INK_CHECK(pathPin->hasValue);
  INK_CHECK(!pathPin->isScrubable);
  INK_CHECK(pathPin->typeText.find("asset") != std::string::npos);

  double x = 0.0, y = 0.0, width = 0.0, height = 0.0;
  INK_CHECK(view.nodePosition("/Asset/Source", &x, &y));
  INK_CHECK(view.nodeSize("/Asset/Source", &width, &height));
  (void)y;
  (void)height;
  const double middleX = x + width * 0.5;

  // A stationary middle tap activates exactly once and is not a document edit.
  view.pointerDown(middleX, pathPin->centerY);
  view.pointerUp(middleX, pathPin->centerY);
  INK_CHECK(activationCount == 1);
  INK_CHECK(activatedNode == "/Asset/Source");
  INK_CHECK(activatedAttribute == "path");
  INK_CHECK(beginCount == 0 && endCount == 0);
  INK_CHECK(attributeEditCount == 0);
  INK_CHECK(topologyCount == 0);

  // A real middle drag is a node move and must suppress activation.
  view.pointerDown(middleX, pathPin->centerY);
  view.pointerMove(middleX + 60.0, pathPin->centerY + 35.0);
  view.pointerUp(middleX + 60.0, pathPin->centerY + 35.0);
  double movedX = 0.0, movedY = 0.0, movedWidth = 0.0;
  INK_CHECK(view.nodePosition("/Asset/Source", &movedX, &movedY));
  INK_CHECK(view.nodeSize("/Asset/Source", &movedWidth, nullptr));
  INK_CHECK(movedX != x || movedY != y);
  INK_CHECK(activationCount == 1);
  INK_CHECK(beginCount == 1 && endCount == 1);
  INK_CHECK(attributeEditCount == 0);
  INK_CHECK(topologyCount == 0);

  // The moved row's right edge remains a link gesture, never an activation.
  pins = view.nodePins("/Asset/Source");
  pathPin = FindPin(pins, "path", /*isOutput=*/false);
  INK_CHECK(pathPin != nullptr);
  const double rightBandX = movedX + movedWidth * 0.97;
  view.pointerDown(rightBandX, pathPin->centerY);
  ig::GraphLinkInfo preview;
  INK_CHECK(view.activeLinkPreview(&preview));
  view.pointerUp(rightBandX, pathPin->centerY);
  INK_CHECK(!view.activeLinkPreview(&preview));
  INK_CHECK(activationCount == 1);
  INK_CHECK(beginCount == 1 && endCount == 1);
  INK_CHECK(attributeEditCount == 0);
  INK_CHECK(topologyCount == 0);

  std::printf("  display-only row activation / drag / edge priority ok\n");
  return true;
}

// Scrub state machine: pressing a scrubable row's middle + dragging authors a
// changed value to the document inside exactly one begin/end edit envelope.
// Semantic edits are surfaced directly through the platform-neutral delegate.
struct EditNoticeProbe {
  int liveCount = 0;
  int committedCount = 0;
  std::string lastPrim, lastAttr;
  void OnEdited(const std::string& nodeId, const std::string& attribute,
                bool live) {
    if (live) ++liveCount; else ++committedCount;
    lastPrim = nodeId;
    lastAttr = attribute;
  }
};

bool TestScrubAuthorsValue() {
  std::shared_ptr<ig::InMemoryGraphDocument> stage = MakeStage();
  ig::NoodlesGraphView view;
  int beginCount = 0, endCount = 0;
  int topologyCount = 0;
  EditNoticeProbe probe;
  ig::GraphEditDelegate del;
  del.beginEdit = [&]() { ++beginCount; };
  del.endEdit = [&]() { ++endCount; };
  del.topologyEdited = [&]() { ++topologyCount; };
  del.attributeEdited = [&](const std::string& nodeId,
                            const std::string& attribute, bool live) {
    probe.OnEdited(nodeId, attribute, live);
  };
  view.setDelegate(del);
  view.setDocument(stage);

  auto cPins = view.nodePins("/Root/C");
  const ig::GraphPinInfo* cIn = FindPin(cPins, "in", /*out=*/false);
  INK_CHECK(cIn != nullptr);
  double cx = 0, cy = 0, cw = 0, ch = 0;
  view.nodePosition("/Root/C", &cx, &cy);
  view.nodeSize("/Root/C", &cw, &ch);

  const double midX = cx + cw * 0.5;   // middle band, not the pin edge
  const double rowY = cIn->centerY;
  view.pointerDown(midX, rowY);
  view.pointerMove(midX + 60.0, rowY);
  view.pointerUp(midX + 60.0, rowY);

  const double authored = NumericValue(stage, "/Root/C", "in");
  INK_CHECK(authored > 0.35);                     // dragging right raised it
  INK_CHECK(beginCount == 1 && endCount == 1);    // ONE undo/notify envelope
  INK_CHECK(topologyCount == 0);                  // value edit, not topology

  // Direct delegate delivery reached the platform probe.
  INK_CHECK(probe.liveCount >= 1);                // per applied change
  INK_CHECK(probe.committedCount == 1);           // once at gesture end
  INK_CHECK(probe.lastPrim == "/Root/C" && probe.lastAttr == "in");

  std::printf("  scrub authors changed value, single envelope + notices ok\n");
  return true;
}

// Drag zones: a drag starting on a row does NOT move the node; a drag starting
// on the title bar does.
bool TestTitleOnlyNodeDrag() {
  ig::NoodlesGraphView view;
  int topologyCount = 0;
  ig::GraphEditDelegate del;
  del.topologyEdited = [&] { ++topologyCount; };
  view.setDelegate(del);
  view.setDocument(MakeStage());

  auto cPins = view.nodePins("/Root/C");
  const ig::GraphPinInfo* cIn = FindPin(cPins, "in", /*out=*/false);
  INK_CHECK(cIn != nullptr);
  double cx0 = 0, cy0 = 0, cw = 0, ch = 0;
  view.nodePosition("/Root/C", &cx0, &cy0);
  view.nodeSize("/Root/C", &cw, &ch);
  const double midX = cx0 + cw * 0.5;

  // Drag on the scrubable row → node stays put (it scrubs instead).
  view.pointerDown(midX, cIn->centerY);
  view.pointerMove(midX + 40.0, cIn->centerY + 40.0);
  view.pointerUp(midX + 40.0, cIn->centerY + 40.0);
  double cx1 = 0, cy1 = 0;
  view.nodePosition("/Root/C", &cx1, &cy1);
  INK_CHECK(cx1 == cx0 && cy1 == cy0);

  // Drag on the title bar → node moves.
  view.pointerDown(midX, cy0 + 4.0);   // just below the top edge = title bar
  view.pointerMove(midX + 50.0, cy0 + 54.0);
  view.pointerUp(midX + 50.0, cy0 + 54.0);
  double cx2 = 0, cy2 = 0;
  view.nodePosition("/Root/C", &cx2, &cy2);
  INK_CHECK(cx2 != cx0 || cy2 != cy0);
  INK_CHECK(topologyCount == 0);  // position edits never invalidate topology
  std::printf("  title-only node drag ok\n");
  return true;
}

// A data-flow chain A.out → B.in, B.out → C.in with NO authored positions, so
// the whole graph goes through the deterministic layoutGraph pass. Attribute
// CONNECTIONS (not relationships) drive noodles' layout, so A/B/C land in
// consecutive layers.
Document MakeChainStage() {
  auto document = NewDocument({Node("/G/A"), Node("/G/B"), Node("/G/C")});
  document->addProperty("/G/A", Numeric("out", "float", 1.0));
  document->addProperty("/G/B", Numeric("in"));
  document->addProperty("/G/B", Numeric("out", "float", 1.0));
  document->addProperty("/G/C", Numeric("in"));
  document->authorConnection("/G/B", "in", "/G/A", "out");
  document->authorConnection("/G/C", "in", "/G/B", "out");
  return document;
}

// Item 2: cumulative pinch scale must map to an absolute zoom, not compound;
// the graph point under the anchor stays fixed.
bool TestPinchZoomNotCompounding() {
  ig::NoodlesGraphView view;
  view.setDocument(MakeStage());
  view.setZoom(1.0);
  view.setViewportPan(0.0, 0.0);

  const double ax = 100.0, ay = 80.0;
  double gx0 = 0, gy0 = 0;
  view.viewToGraph(ax, ay, &gx0, &gy0);

  view.pinchBegin();
  view.pinchUpdate(1.5, ax, ay);
  view.pinchUpdate(1.5, ax, ay);  // still cumulative 1.5, not 1.5*1.5
  view.pinchEnd();

  INK_CHECK(std::abs(view.zoom() - 1.5) < 1e-9);  // NOT 2.25

  double gx1 = 0, gy1 = 0;
  view.viewToGraph(ax, ay, &gx1, &gy1);
  INK_CHECK(std::abs(gx1 - gx0) < 1e-6 && std::abs(gy1 - gy0) < 1e-6);
  std::printf("  pinch zoom absolute (no compounding), anchor fixed ok\n");
  return true;
}

bool TestFrameAllFitsViewport() {
  ig::GraphEditor editor;
  editor.setDocument(MakeChainStage());
  INK_CHECK(!editor.frameAll(20.0));  // no viewport yet

  constexpr double scale = 2.0;
  constexpr double widthPoints = 320.0;
  constexpr double heightPoints = 200.0;
  editor.resize(static_cast<int>(widthPoints * scale),
                static_cast<int>(heightPoints * scale), scale);
  INK_CHECK(editor.frameAll(20.0));
  INK_CHECK(editor.zoom() <= 1.0);

  for (const char* id : {"/G/A", "/G/B", "/G/C"}) {
    double x = 0.0, y = 0.0, width = 0.0, height = 0.0;
    INK_CHECK(editor.nodePosition(id, &x, &y));
    INK_CHECK(editor.nodeSize(id, &width, &height));
    const double left = (x - editor.panX()) * editor.zoom() / scale;
    const double top = (y - editor.panY()) * editor.zoom() / scale;
    const double right = left + width * editor.zoom() / scale;
    const double bottom = top + height * editor.zoom() / scale;
    INK_CHECK(left >= 20.0 - 1e-6 && top >= 20.0 - 1e-6);
    INK_CHECK(right <= widthPoints - 20.0 + 1e-6);
    INK_CHECK(bottom <= heightPoints - 20.0 + 1e-6);
  }

  ig::GraphEditor empty;
  empty.resize(640, 400, 2.0f);
  INK_CHECK(!empty.frameAll());
  std::printf("  frame-all fits content in point-space padding ok\n");
  return true;
}

// Item 3a: a wholly unpositioned graph is laid out with no overlaps, and a
// node's directly-connected neighbours are nearer than a non-adjacent node.
bool TestAutolayoutFreshNoOverlap() {
  ig::NoodlesGraphView view;
  view.setDocument(MakeChainStage());
  INK_CHECK(view.nodeCount() == 3);

  INK_CHECK(NodesDisjoint(view, "/G/A", "/G/B"));
  INK_CHECK(NodesDisjoint(view, "/G/A", "/G/C"));
  INK_CHECK(NodesDisjoint(view, "/G/B", "/G/C"));

  const double dAB = NodeCenterDistance(view, "/G/A", "/G/B");
  const double dBC = NodeCenterDistance(view, "/G/B", "/G/C");
  const double dAC = NodeCenterDistance(view, "/G/A", "/G/C");
  INK_CHECK(dAB < dAC);  // adjacent (connected) nearer than the chain endpoints
  INK_CHECK(dBC < dAC);

  // Layered invariant: the chain A→B→C flows strictly left→right by rank.
  double axp = 0, ayp = 0, bxp = 0, byp = 0, cxp = 0, cyp = 0;
  view.nodePosition("/G/A", &axp, &ayp);
  view.nodePosition("/G/B", &bxp, &byp);
  view.nodePosition("/G/C", &cxp, &cyp);
  INK_CHECK(axp < bxp && bxp < cxp);
  std::printf("  fresh autolayout: left→right rank, no overlap, connected nearer ok\n");
  return true;
}

// Item 3 (layout): a fan A→B, A→C puts B and C in the SAME rank (shared X) and
// stacks them vertically without overlap. Uses attribute connections A.out→B.in
// and A.out→C.in so the whole graph goes through the fresh layered pass.
Document MakeFanStage() {
  auto document = NewDocument({Node("/F/A"), Node("/F/B"), Node("/F/C")});
  document->addProperty("/F/A", Numeric("out", "float", 1.0));
  document->addProperty("/F/B", Numeric("in"));
  document->addProperty("/F/C", Numeric("in"));
  document->authorConnection("/F/B", "in", "/F/A", "out");
  document->authorConnection("/F/C", "in", "/F/A", "out");
  return document;
}

bool TestAutolayoutFanSiblings() {
  ig::NoodlesGraphView view;
  view.setDocument(MakeFanStage());
  INK_CHECK(view.nodeCount() == 3);

  double axp = 0, ayp = 0, bxp = 0, byp = 0, cxp = 0, cyp = 0;
  INK_CHECK(view.nodePosition("/F/A", &axp, &ayp));
  INK_CHECK(view.nodePosition("/F/B", &bxp, &byp));
  INK_CHECK(view.nodePosition("/F/C", &cxp, &cyp));

  // A is the source (left of the siblings).
  INK_CHECK(axp < bxp && axp < cxp);
  // B and C share the same rank → (approximately) the same X.
  INK_CHECK(std::abs(bxp - cxp) < 1.0);
  // Siblings are stacked vertically and do not overlap.
  INK_CHECK(std::abs(byp - cyp) > 1.0);
  INK_CHECK(NodesDisjoint(view, "/F/B", "/F/C"));
  INK_CHECK(NodesDisjoint(view, "/F/A", "/F/B"));
  INK_CHECK(NodesDisjoint(view, "/F/A", "/F/C"));
  std::printf("  fan autolayout: siblings share rank, stacked, disjoint ok\n");
  return true;
}

// Item 3 (anchor): a relationship noodle A.custom:link→B must START at the
// right-edge center of A's "custom:link" ROW (the pin's own resolved center),
// not an aggregate/synthetic anchor.
bool TestRelationshipNoodleSourceAnchor() {
  ig::NoodlesGraphView view;
  view.setDocument(MakeStage());

  auto aPins = view.nodePins("/Root/A");
  const ig::GraphPinInfo* aRel = FindPin(aPins, "custom:link", /*out=*/true);
  INK_CHECK(aRel != nullptr && aRel->isRelationship);

  double ax = 0, ay = 0, aw = 0, ah = 0;
  view.nodePosition("/Root/A", &ax, &ay);
  view.nodeSize("/Root/A", &aw, &ah);

  bool found = false;
  for (const ig::GraphLinkInfo& l : view.links()) {
    if (l.isRelationship && l.sourceNodeId == "/Root/A" &&
        l.sourcePort == "custom:link" && l.targetNodeId == "/Root/B") {
      found = true;
      // The noodle must START at the node's RIGHT EDGE (the output-side port
      // circle X), asserted against the geometry directly — NOT just "== the pin
      // anchor" (which would tautologically pass even if both drifted to the row
      // middle, the on-device bug).
      INK_CHECK(std::abs(l.startX - (ax + aw)) < 1e-6);      // right edge
      // Explicitly NOT the horizontal middle of the row / node.
      INK_CHECK(std::abs(l.startX - (ax + aw * 0.5)) > aw * 0.25);
      // Y sits ON the relationship row (its port-circle Y), inside the node box.
      INK_CHECK(std::abs(l.startY - aRel->centerY) < 1e-6);
      INK_CHECK(l.startY > ay && l.startY < ay + ah);
      // The port circle (aRel->centerX) draws at that same right edge.
      INK_CHECK(std::abs(aRel->centerX - (ax + aw)) < 1e-6);
    }
  }
  INK_CHECK(found);
  std::printf("  relationship noodle starts at its row's right-edge center ok\n");
  return true;
}

// The rig/mover real-world shape: several namespaced relationships on ONE node
// (e.g. mover:target, mover:weightRef) group under a "mover" namespace header,
// so each relationship pin is a grouped child row rather than a top-level pin.
// The child's noodle must still START at the node's RIGHT EDGE, not the header's
// aggregate center or the child row's middle.
bool TestRelationshipNamespacedGroupAnchor() {
  auto stage = NewDocument({Node("/Rig/M"), Node("/Rig/S1"),
                            Node("/Rig/S2")});
  // Two members of the "mover" namespace → a grouped header with child rows.
  stage->addProperty("/Rig/M", Relationship("mover:target"));
  stage->addProperty("/Rig/M", Relationship("mover:weightRef"));
  stage->authorRelationship("/Rig/M", "mover:target", "/Rig/S1");
  stage->authorRelationship("/Rig/M", "mover:weightRef", "/Rig/S2");

  ig::NoodlesGraphView view;
  view.setDocument(stage);

  double mx = 0, my = 0, mw = 0, mh = 0;
  INK_CHECK(view.nodePosition("/Rig/M", &mx, &my));
  INK_CHECK(view.nodeSize("/Rig/M", &mw, &mh));

  int checked = 0;
  for (const ig::GraphLinkInfo& l : view.links()) {
    if (!l.isRelationship || l.sourceNodeId != "/Rig/M") continue;
    ++checked;
    INK_CHECK(std::abs(l.startX - (mx + mw)) < 1e-6);            // right edge
    INK_CHECK(std::abs(l.startX - (mx + mw * 0.5)) > mw * 0.25);  // not middle
    INK_CHECK(l.startY > my && l.startY <= my + mh);             // on a row
  }
  INK_CHECK(checked >= 1);
  std::printf("  namespaced-group relationship noodles start at right edge ok\n");
  return true;
}

// Item 2: the zoom-out floor is dynamic — a graph much larger than the viewport
// can zoom out past the static 0.25 clamp; an empty graph keeps the 0.25 floor.
bool TestDynamicZoomFloor() {
  ig::NoodlesGraphView view;
  view.setDocument(MakeChainStage());
  view.resize(200, 200, 1.0f);  // small viewport vs a wide laid-out chain
  view.setZoom(0.001);
  INK_CHECK(view.zoom() < 0.25);  // dynamic floor allowed zooming past 0.25
  INK_CHECK(view.zoom() > 0.0);

  ig::NoodlesGraphView empty;  // no graph, no viewport → static 0.25 floor
  empty.setZoom(0.001);
  INK_CHECK(std::abs(empty.zoom() - 0.25) < 1e-9);
  std::printf("  dynamic zoom-out floor ok\n");
  return true;
}

// Item 4: when the content exceeds the viewport the minimap is visible; a press
// inside it recenters the viewport on the mapped world point (drag→pan inverse).
bool TestMinimapDragPan() {
  ig::NoodlesGraphView view;
  view.setDocument(MakeChainStage());
  view.resize(400, 300, 1.0f);   // points == pixels (contentScale 1)
  view.setViewportPan(0.0, 0.0);
  view.setZoom(2.0);             // zoom in so the wide chain exceeds the viewport
  INK_CHECK(view.minimapVisible());

  double mx = 0, my = 0, mw = 0, mh = 0;
  INK_CHECK(view.minimapRect(&mx, &my, &mw, &mh));
  INK_CHECK(mw > 0.0 && mh > 0.0);

  const double cx = mx + mw * 0.5, cy = my + mh * 0.5;  // minimap center
  double wx = 0, wy = 0;
  INK_CHECK(view.minimapPointToWorld(cx, cy, &wx, &wy));

  view.pointerDown(cx, cy);  // press recenters immediately
  view.pointerUp(cx, cy);

  const double vw = 400.0 / view.zoom(), vh = 300.0 / view.zoom();
  const double centerX = view.panX() + vw * 0.5;
  const double centerY = view.panY() + vh * 0.5;
  INK_CHECK(std::abs(centerX - wx) < 1e-6 && std::abs(centerY - wy) < 1e-6);
  std::printf("  minimap visible + drag→pan recenters on mapped point ok\n");
  return true;
}

// Item 3b: a node appearing in a later refresh is placed near its neighbour
// without moving any node that already has a position.
bool TestAutolayoutIncrementalPreserves() {
  std::shared_ptr<ig::InMemoryGraphDocument> stage = MakeChainStage();
  ig::NoodlesGraphView view;
  view.setDocument(stage);

  double ax = 0, ay = 0, bx = 0, by = 0, cx = 0, cy = 0;
  view.nodePosition("/G/A", &ax, &ay);
  view.nodePosition("/G/B", &bx, &by);
  view.nodePosition("/G/C", &cx, &cy);

  // Add D connected to C (D.in ← C.out), then refresh the same view.
  stage->addProperty("/G/C", Numeric("out", "float", 1.0));
  stage->addNode(Node("/G/D"));
  stage->addProperty("/G/D", Numeric("in"));
  stage->authorConnection("/G/D", "in", "/G/C", "out");
  view.setDocument(stage);
  INK_CHECK(view.nodeCount() == 4);

  // Placed nodes are untouched.
  double ax2 = 0, ay2 = 0, bx2 = 0, by2 = 0, cx2 = 0, cy2 = 0;
  view.nodePosition("/G/A", &ax2, &ay2);
  view.nodePosition("/G/B", &bx2, &by2);
  view.nodePosition("/G/C", &cx2, &cy2);
  INK_CHECK(ax2 == ax && ay2 == ay);
  INK_CHECK(bx2 == bx && by2 == by);
  INK_CHECK(cx2 == cx && cy2 == cy);

  // D placed, disjoint, and nearer its neighbour C than the far end A.
  double dx = 0, dy = 0;
  INK_CHECK(view.nodePosition("/G/D", &dx, &dy));
  INK_CHECK(NodesDisjoint(view, "/G/D", "/G/A"));
  INK_CHECK(NodesDisjoint(view, "/G/D", "/G/B"));
  INK_CHECK(NodesDisjoint(view, "/G/D", "/G/C"));
  INK_CHECK(NodeCenterDistance(view, "/G/D", "/G/C") <
            NodeCenterDistance(view, "/G/D", "/G/A"));
  std::printf("  incremental autolayout preserves placed nodes ok\n");
  return true;
}

// Item 4: scrubbing an ANIMATED attribute authors at the display frame, leaving
// the existing samples intact and not authoring a default.
bool TestScrubAuthorsAtDisplayFrame() {
  auto stage = NewDocument({Node("/Root/A"), Node("/Root/C")});
  stage->addProperty("/Root/A", Relationship("custom:link"));
  stage->addProperty("/Root/C", Numeric("in"));
  stage->authorRelationship("/Root/A", "custom:link", "/Root/C");
  stage->setTimeSample("/Root/C", "in", 1.0, 0.1);
  stage->setTimeSample("/Root/C", "in", 10.0, 0.9);

  ig::NoodlesGraphView view;
  view.setDisplayFrame(5.0);
  view.setDocument(stage);

  auto cPins = view.nodePins("/Root/C");
  const ig::GraphPinInfo* cIn = FindPin(cPins, "in", /*out=*/false);
  INK_CHECK(cIn != nullptr && cIn->isScrubable);

  double cx = 0, cy = 0, cw = 0, ch = 0;
  view.nodePosition("/Root/C", &cx, &cy);
  view.nodeSize("/Root/C", &cw, &ch);
  const double midX = cx + cw * 0.5;
  view.pointerDown(midX, cIn->centerY);
  view.pointerMove(midX + 80.0, cIn->centerY);
  view.pointerUp(midX + 80.0, cIn->centerY);

  // A new sample authored at frame 5; frames 1 and 10 preserved; no default.
  const std::vector<double> times = stage->timeSamples("/Root/C", "in");
  INK_CHECK(times.size() == 3);
  bool has1 = false, has5 = false, has10 = false;
  for (double t : times) {
    if (t == 1.0) has1 = true;
    if (t == 5.0) has5 = true;
    if (t == 10.0) has10 = true;
  }
  INK_CHECK(has1 && has5 && has10);
  std::printf("  scrub authors at display frame (animated) ok\n");
  return true;
}

// Item 5: an external stage edit to a displayed value row / node position is
// reflected after processPendingStageChanges(); a structural edit fires the
// graphStructureChanged delegate instead of rebuilding inline.
bool TestExternalEditReflects() {
  std::shared_ptr<ig::InMemoryGraphDocument> stage = MakeStage();
  // Give B an authored position before observing it.
  stage->setNodePosition("/Root/B", 10.0, 20.0);

  ig::NoodlesGraphView view;
  int structuralCbs = 0;
  ig::GraphEditDelegate del;
  del.graphStructureChanged = [&] { ++structuralCbs; };
  view.setDelegate(del);
  view.setDocument(stage);

  // 1) External value change on a displayed scrubable row.
  INK_CHECK(stage->setAttributeValue("/Root/C", "in", 0.77, 0.0));
  view.processPendingStageChanges();
  {
    auto cPins = view.nodePins("/Root/C");
    const ig::GraphPinInfo* cIn = FindPin(cPins, "in", /*out=*/false);
    INK_CHECK(cIn != nullptr);
    INK_CHECK(cIn->typeText.find("0.77") != std::string::npos);
    INK_CHECK(cIn->value > 0.76 && cIn->value < 0.78);
  }

  // 2) External position change on B (Set only — the attr already exists).
  INK_CHECK(stage->setNodePosition("/Root/B", 1234.0, 567.0));
  view.processPendingStageChanges();
  double bx = 0, by = 0;
  INK_CHECK(view.nodePosition("/Root/B", &bx, &by));
  INK_CHECK(bx == 1234.0 && by == 567.0);

  // 3) External STRUCTURAL change → the platform is notified to refresh.
  INK_CHECK(stage->authorRelationship("/Root/A", "custom:link2", "/Root/C"));
  INK_CHECK(structuralCbs >= 1);
  std::printf("  external value + position reflect; structural notified ok\n");
  return true;
}

// hitsGraphElementAt: true over a node, false over empty space.
bool TestHitsGraphElement() {
  ig::NoodlesGraphView view;
  view.setDocument(MakeStage());
  double cx = 0, cy = 0, cw = 0, ch = 0;
  view.nodePosition("/Root/C", &cx, &cy);
  view.nodeSize("/Root/C", &cw, &ch);
  INK_CHECK(view.hitsGraphElementAt(cx + cw * 0.5, cy + ch * 0.5));
  INK_CHECK(!view.hitsGraphElementAt(cx - 100000.0, cy - 100000.0));
  std::printf("  hitsGraphElementAt node hit + empty miss ok\n");
  return true;
}

// (e) hitsGraphElementAt is true ON a link (curve-accurate hit-test), false far
// away.
bool TestLinkMidpointHits() {
  ig::NoodlesGraphView view;
  view.setDocument(MakeLinkStage());
  const std::vector<std::string> ids{"/L/S", "/L/D", "/L/T"};

  auto sPins = view.nodePins("/L/S");
  auto dPins = view.nodePins("/L/D");
  const ig::GraphPinInfo* sOut = FindPin(sPins, "out", /*out=*/true);
  const ig::GraphPinInfo* dIn = FindPin(dPins, "in", /*out=*/false);
  INK_CHECK(sOut != nullptr && dIn != nullptr);

  double gx = 0, gy = 0;
  INK_CHECK(FindLinkGrab(view, ids, sOut->centerX, sOut->centerY, dIn->centerX,
                         dIn->centerY, &gx, &gy));
  INK_CHECK(view.hitsGraphElementAt(gx, gy));      // on the link curve
  INK_CHECK(!view.hitsGraphElementAt(gx, gy - 100000.0));  // far away
  std::printf("  hitsGraphElementAt true on a link ok\n");
  return true;
}

// (a-attr) grabbing a data link and releasing in empty space DISCONNECTS it:
// the attribute connection is removed, inside one begin/end envelope.
bool TestDisconnectConnectionToEmpty() {
  std::shared_ptr<ig::InMemoryGraphDocument> stage = MakeLinkStage();
  ig::NoodlesGraphView view;
  int beginCount = 0, endCount = 0;
  int topologyCount = 0;
  bool topologyInsideEnvelope = false;
  ig::GraphEditDelegate del;
  del.beginEdit = [&] { ++beginCount; };
  del.endEdit = [&] { ++endCount; };
  del.topologyEdited = [&] {
    ++topologyCount;
    topologyInsideEnvelope = beginCount == 1 && endCount == 0;
  };
  view.setDelegate(del);
  view.setDocument(stage);
  const std::vector<std::string> ids{"/L/S", "/L/D", "/L/T"};

  auto sPins = view.nodePins("/L/S");
  auto dPins = view.nodePins("/L/D");
  const ig::GraphPinInfo* sOut = FindPin(sPins, "out", /*out=*/true);
  const ig::GraphPinInfo* dIn = FindPin(dPins, "in", /*out=*/false);
  double gx = 0, gy = 0;
  INK_CHECK(FindLinkGrab(view, ids, sOut->centerX, sOut->centerY, dIn->centerX,
                         dIn->centerY, &gx, &gy));

  view.pointerDown(gx, gy);
  view.pointerMove(gx, gy - 50000.0);
  view.pointerUp(gx, gy - 50000.0);  // empty space

  INK_CHECK(!HasEdge(stage, "/L/D", "in", "/L/S", "out", false));
  INK_CHECK(beginCount == 1 && endCount == 1);
  INK_CHECK(topologyCount == 1 && topologyInsideEnvelope);
  std::printf("  drag data link to empty disconnects (1 envelope) ok\n");
  return true;
}

// (a-rel) grabbing a relationship link and releasing in empty space removes the
// relationship target.
bool TestDisconnectRelationshipToEmpty() {
  std::shared_ptr<ig::InMemoryGraphDocument> stage = MakeRelStage();
  ig::NoodlesGraphView view;
  view.setDocument(stage);
  const std::vector<std::string> ids{"/R/A", "/R/B", "/R/C"};

  auto aPins = view.nodePins("/R/A");
  const ig::GraphPinInfo* aRel = FindPin(aPins, "custom:link", /*out=*/true);
  INK_CHECK(aRel != nullptr);
  double bx = 0, by = 0, bw = 0, bh = 0;
  view.nodePosition("/R/B", &bx, &by);
  view.nodeSize("/R/B", &bw, &bh);
  // Relationship link end = B top-center. Grab from the A end (unambiguous).
  double gx = 0, gy = 0;
  INK_CHECK(FindLinkGrab(view, ids, aRel->centerX, aRel->centerY, bx + bw * 0.5,
                         by, &gx, &gy));

  view.pointerDown(gx, gy);
  view.pointerMove(gx, gy - 50000.0);
  view.pointerUp(gx, gy - 50000.0);  // empty space

  INK_CHECK(!HasEdge(stage, "/R/A", "custom:link", "/R/B", "", true));
  std::printf("  drag relationship link to empty disconnects ok\n");
  return true;
}

// (b-attr) grabbing a data link near its input end and dropping on another
// node's input pin RE-AUTHORS it: old connection gone, new one present, one
// begin/end envelope.
bool TestReconnectConnectionToNode() {
  std::shared_ptr<ig::InMemoryGraphDocument> stage = MakeLinkStage();
  ig::NoodlesGraphView view;
  int beginCount = 0, endCount = 0;
  int topologyCount = 0;
  bool topologyInsideEnvelope = false;
  ig::GraphEditDelegate del;
  del.beginEdit = [&] { ++beginCount; };
  del.endEdit = [&] { ++endCount; };
  del.topologyEdited = [&] {
    ++topologyCount;
    topologyInsideEnvelope = beginCount == 1 && endCount == 0;
  };
  view.setDelegate(del);
  view.setDocument(stage);
  const std::vector<std::string> ids{"/L/S", "/L/D", "/L/T"};

  auto sPins = view.nodePins("/L/S");
  auto dPins = view.nodePins("/L/D");
  auto tPins = view.nodePins("/L/T");
  const ig::GraphPinInfo* sOut = FindPin(sPins, "out", /*out=*/true);
  const ig::GraphPinInfo* dIn = FindPin(dPins, "in", /*out=*/false);
  const ig::GraphPinInfo* tIn = FindPin(tPins, "in", /*out=*/false);
  INK_CHECK(sOut && dIn && tIn);

  // Grab near the INPUT (D) end: order (p0=D.in, p1=S.out) so the grab lands
  // near D → the grabbed end is the input side → anchor is the output (S.out).
  double gx = 0, gy = 0;
  INK_CHECK(FindLinkGrab(view, ids, dIn->centerX, dIn->centerY, sOut->centerX,
                         sOut->centerY, &gx, &gy));

  view.pointerDown(gx, gy);
  view.pointerMove(tIn->centerX + 20.0, tIn->centerY);
  view.pointerUp(tIn->centerX, tIn->centerY);

  INK_CHECK(!HasEdge(stage, "/L/D", "in", "/L/S", "out", false));
  INK_CHECK(HasEdge(stage, "/L/T", "in", "/L/S", "out", false));
  INK_CHECK(beginCount == 1 && endCount == 1);  // single envelope for remove+add
  INK_CHECK(topologyCount == 1 && topologyInsideEnvelope);

  // Round-trips: the new edge surfaces with T.in as the input side.
  auto tPins2 = view.nodePins("/L/T");
  INK_CHECK(HasPin(tPins2, "in", /*out=*/false));
  std::printf("  reconnect data link to new node re-authors (1 envelope) ok\n");
  return true;
}

// (b-rel) grabbing a relationship link near its target end and dropping on a new
// node re-targets the relationship.
bool TestReconnectRelationshipToNode() {
  std::shared_ptr<ig::InMemoryGraphDocument> stage = MakeRelStage();
  ig::NoodlesGraphView view;
  int beginCount = 0, endCount = 0;
  int topologyCount = 0;
  ig::GraphEditDelegate del;
  del.beginEdit = [&] { ++beginCount; };
  del.endEdit = [&] { ++endCount; };
  del.topologyEdited = [&] { ++topologyCount; };
  view.setDelegate(del);
  view.setDocument(stage);
  const std::vector<std::string> ids{"/R/A", "/R/B", "/R/C"};

  auto aPins = view.nodePins("/R/A");
  const ig::GraphPinInfo* aRel = FindPin(aPins, "custom:link", /*out=*/true);
  double bx = 0, by = 0, bw = 0, bh = 0;
  view.nodePosition("/R/B", &bx, &by);
  view.nodeSize("/R/B", &bw, &bh);
  // Grab near the TARGET (B) end: order (p0=B top-center, p1=A rel pin).
  double gx = 0, gy = 0;
  INK_CHECK(FindLinkGrab(view, ids, bx + bw * 0.5, by, aRel->centerX,
                         aRel->centerY, &gx, &gy));

  double ccx = 0, ccy = 0;
  NodeCenter(view, "/R/C", &ccx, &ccy);

  view.pointerDown(gx, gy);
  view.pointerMove(ccx, ccy);
  view.pointerUp(ccx, ccy);

  INK_CHECK(!HasEdge(stage, "/R/A", "custom:link", "/R/B", "", true));
  INK_CHECK(HasEdge(stage, "/R/A", "custom:link", "/R/C", "", true));
  INK_CHECK(beginCount == 1 && endCount == 1);
  INK_CHECK(topologyCount == 1);
  std::printf("  reconnect relationship link to new node ok\n");
  return true;
}

// (c) a right-band drag from an attribute row starts a NEW output-side
// connection; dropping on an opposite input pin authors it (round-trips).
bool TestRowEdgeBandAuthorsConnection() {
  auto stage = NewDocument({Node("/E/S"), Node("/E/D")});
  stage->addProperty("/E/S", Numeric("out", "float", 1.0));
  stage->addProperty("/E/D", Numeric("in"));
  // A pre-existing edge so both prims surface as nodes (the collector only emits
  // edge-participating prims). Positions are left to autolayout.
  stage->addProperty("/E/S", Relationship("anchor"));
  stage->authorRelationship("/E/S", "anchor", "/E/D");

  ig::NoodlesGraphView view;
  int beginCount = 0, endCount = 0;
  ig::GraphEditDelegate del;
  del.beginEdit = [&] { ++beginCount; };
  del.endEdit = [&] { ++endCount; };
  view.setDelegate(del);
  view.setDocument(stage);

  // Right band of S's "out" row → start an output-side connection drag.
  auto sPins = view.nodePins("/E/S");
  const ig::GraphPinInfo* sOutRow = FindPin(sPins, "out", /*out=*/false);
  INK_CHECK(sOutRow != nullptr);
  double sx = 0, sy = 0, sw = 0, sh = 0;
  view.nodePosition("/E/S", &sx, &sy);
  view.nodeSize("/E/S", &sw, &sh);
  const double rightBandX = sx + sw * 0.97;  // outer 10% right band

  auto dPins = view.nodePins("/E/D");
  const ig::GraphPinInfo* dIn = FindPin(dPins, "in", /*out=*/false);
  INK_CHECK(dIn != nullptr);

  view.pointerDown(rightBandX, sOutRow->centerY);
  view.pointerMove(dIn->centerX + 20.0, dIn->centerY);
  view.pointerUp(dIn->centerX, dIn->centerY);

  // The connection is authored ON the INPUT side (D.in) pointing to the OUTPUT
  // side (S.out) — exactly how CollectGraph reads it.
  INK_CHECK(HasEdge(stage, "/E/D", "in", "/E/S", "out", false));
  INK_CHECK(beginCount == 1 && endCount == 1);
  std::printf("  row-edge band authors a new connection (round-trip) ok\n");
  return true;
}

// (d) a drag on a scrubable row's MIDDLE still scrubs (bands did not steal it).
bool TestRowMiddleStillScrubs() {
  std::shared_ptr<ig::InMemoryGraphDocument> stage = MakeStage();
  ig::NoodlesGraphView view;
  view.setDocument(stage);

  auto cPins = view.nodePins("/Root/C");
  const ig::GraphPinInfo* cIn = FindPin(cPins, "in", /*out=*/false);
  INK_CHECK(cIn != nullptr && cIn->isScrubable);
  double cx = 0, cy = 0, cw = 0, ch = 0;
  view.nodePosition("/Root/C", &cx, &cy);
  view.nodeSize("/Root/C", &cw, &ch);

  const double midX = cx + cw * 0.5;  // middle band
  view.pointerDown(midX, cIn->centerY);
  view.pointerMove(midX + 60.0, cIn->centerY);
  view.pointerUp(midX + 60.0, cIn->centerY);

  INK_CHECK(NumericValue(stage, "/Root/C", "in") > 0.35);
  INK_CHECK(view.linkCount() == 2);   // topology unchanged
  std::printf("  row middle still scrubs (no band regression) ok\n");
  return true;
}

// ── Touch-ergonomics (finger/pencil sizing) ──────────────────────────────────
// These run at contentScale 2 (iPad) and zoom 1, where world = viewPt * 2. The
// hit tests are distance-to-the-straight-segment between a link's two endpoints
// (LinkGeometry::findLinkUnderCursor) and per-side row bands, both widened from
// desktop-mouse sizes to fingertip sizes expressed in VIEW points. They assert
// that a press OFF the exact target by a fingertip-sized margin still grabs it —
// the on-device failure the precise-coordinate tests above could never catch.

// (e) At contentScale 2 / zoom 1, a press 8 VIEW points off a link's straight
// midpoint still lifts the link (drop-in-empty disconnects it), while a press 30
// VIEW points off misses and pans (connection intact). 8 view pts = 16 world
// units < the ~24-world finger tolerance; the old ~6-world mouse tolerance would
// have missed at 16.
bool TestLinkTouchSlopLiftsOffCurve() {
  const std::vector<std::string> ids{"/L/S", "/L/D", "/L/T"};

  // World point → view point at pan 0, zoom 1, contentScale 2 (view = world/2).
  auto toView = [](double w) { return w * 0.5; };

  // Perpendicular-offset a segment-midpoint press by `offWorld` world units and
  // return whether the link's data connection (D.in ← S.out) survives a
  // drop-in-empty gesture. Removed ⇒ the press grabbed (lifted) the link.
  auto liftedAfterOffset = [&](double offWorld) -> bool {
    std::shared_ptr<ig::InMemoryGraphDocument> stage = MakeLinkStage();
    ig::NoodlesGraphView view;
    view.setDocument(stage);
    view.resize(1024, 768, 2.0f);  // contentScale 2 (iPad)
    view.setZoom(1.0);
    view.setViewportPan(0.0, 0.0);

    auto sPins = view.nodePins("/L/S");
    auto dPins = view.nodePins("/L/D");
    const ig::GraphPinInfo* sOut = FindPin(sPins, "out", /*out=*/true);
    const ig::GraphPinInfo* dIn = FindPin(dPins, "in", /*out=*/false);
    INK_CHECK(sOut && dIn);

    // The link's hit segment runs endpoint-to-endpoint (the two pin centers).
    const double x0 = sOut->centerX, y0 = sOut->centerY;
    const double x1 = dIn->centerX, y1 = dIn->centerY;
    const double dx = x1 - x0, dy = y1 - y0;
    const double len = std::sqrt(dx * dx + dy * dy);
    INK_CHECK(len > 0.0);
    const double nx = -dy / len, ny = dx / len;  // unit perpendicular
    const double bx = x0 + dx * 0.5, by = y0 + dy * 0.5;  // segment midpoint
    INK_CHECK(!PtOverNode(view, ids, bx, by));            // midpoint in the gap
    const double px = bx + nx * offWorld, py = by + ny * offWorld;
    INK_CHECK(!PtOverNode(view, ids, px, py));  // pressed point not over a node

    view.pointerDown(toView(px), toView(py));
    view.pointerMove(toView(px), toView(py) - 50000.0);  // drag to empty
    view.pointerUp(toView(px), toView(py) - 50000.0);

    return !HasEdge(stage, "/L/D", "in", "/L/S", "out", false);
  };

  INK_CHECK(liftedAfterOffset(16.0));   //  8 view pts off → still lifts
  INK_CHECK(!liftedAfterOffset(60.0));  // 30 view pts off → misses (pans)
  std::printf("  link touch-slop lifts 8pt-off press, misses 30pt-off ok\n");
  return true;
}

// (f) A connect-band grab 12 VIEW points inside the right edge of a node that is
// SMALL ON SCREEN (contentScale 2, zoomed out to 0.4 — the real device case: the
// user zooms out to see the whole graph, so each node's on-screen 10% band shrinks
// below a fingertip) still starts an output-side connection drag. Here 12 view pts
// is 60 world units, OUTSIDE the desktop 10% band (~43 world) but inside the
// view-point-floored band (~70 world), so it only connects with the new sizing.
// Dropping on another node's input authors the connection.
bool TestBandTouchSlopNarrowNode() {
  auto stage = NewDocument({Node("/E/S"), Node("/E/D")});
  stage->addProperty("/E/S", Numeric("out"));
  stage->addProperty("/E/D", Numeric("in"));
  // A pre-existing edge so both prims surface as nodes.
  stage->addProperty("/E/S", Relationship("a"));
  stage->authorRelationship("/E/S", "a", "/E/D");

  ig::NoodlesGraphView view;
  int beginCount = 0, endCount = 0;
  ig::GraphEditDelegate del;
  del.beginEdit = [&] { ++beginCount; };
  del.endEdit = [&] { ++endCount; };
  view.setDelegate(del);
  view.setDocument(stage);
  const double kCS = 2.0, kZoom = 0.4;  // iPad contentScale, zoomed out
  view.resize(1024, 768, (float)kCS);
  view.setZoom(kZoom);
  view.setViewportPan(0.0, 0.0);

  // world → view at pan 0: view = world * zoom / contentScale.
  auto toView = [&](double w) { return w * kZoom / kCS; };
  const double worldPerViewPt = kCS / kZoom;  // 5 world units per view point

  double sx = 0, sy = 0, sw = 0, sh = 0;
  view.nodePosition("/E/S", &sx, &sy);
  view.nodeSize("/E/S", &sw, &sh);

  // 12 view points inside the right edge of the output row.
  const double insetWorld = 12.0 * worldPerViewPt;  // 60 world units
  // Meaningful only if this inset is OUTSIDE the desktop 10% band — i.e. the old
  // sizing would have missed it (falling through to node move / scrub).
  INK_CHECK(insetWorld > sw * 0.10);

  auto sPins = view.nodePins("/E/S");
  const ig::GraphPinInfo* sOutRow = FindPin(sPins, "out", /*out=*/false);
  INK_CHECK(sOutRow != nullptr);
  auto dPins = view.nodePins("/E/D");
  const ig::GraphPinInfo* dIn = FindPin(dPins, "in", /*out=*/false);
  INK_CHECK(dIn != nullptr);

  const double grabWorldX = sx + sw - insetWorld;
  view.pointerDown(toView(grabWorldX), toView(sOutRow->centerY));
  view.pointerMove(toView(dIn->centerX) + 2.0, toView(dIn->centerY));
  view.pointerUp(toView(dIn->centerX), toView(dIn->centerY));

  INK_CHECK(HasEdge(stage, "/E/D", "in", "/E/S", "out", false));
  INK_CHECK(beginCount == 1 && endCount == 1);
  std::printf(
      "  band touch-slop starts connection 12pt inside small-on-screen node ok\n");
  return true;
}

// ── node persistence / removal / fold / drop (2026-07 feature sweep) ─────────

// Disconnecting a node's last edge KEEPS both endpoints on the canvas: the
// editor authors ui:nodegraph:node:pos on both former endpoints inside the
// disconnect envelope, and the collector surfaces positioned prims as nodes.
bool TestDisconnectKeepsNodes() {
  std::shared_ptr<ig::InMemoryGraphDocument> stage = MakeRelStage();
  ig::NoodlesGraphView view;
  view.setDocument(stage);
  INK_CHECK(view.nodeCount() == 3);

  auto aPins = view.nodePins("/R/A");
  const ig::GraphPinInfo* aRel = FindPin(aPins, "custom:link", /*out=*/true);
  INK_CHECK(aRel != nullptr);
  double bx = 0, by = 0, bw = 0, bh = 0;
  view.nodePosition("/R/B", &bx, &by);
  view.nodeSize("/R/B", &bw, &bh);
  double gx = 0, gy = 0;
  const std::vector<std::string> ids{"/R/A", "/R/B", "/R/C"};
  INK_CHECK(FindLinkGrab(view, ids, bx + bw * 0.5, by, aRel->centerX,
                         aRel->centerY, &gx, &gy));

  view.pointerDown(gx, gy);
  view.pointerMove(gx, gy - 50000.0);
  view.pointerUp(gx, gy - 50000.0);  // empty space → disconnect

  INK_CHECK(!HasEdge(stage, "/R/A", "custom:link", "/R/B", "", true));

  // …but BOTH former endpoints are still nodes on the canvas.
  INK_CHECK(view.nodeCount() == 3);
  double x = 0, y = 0;
  INK_CHECK(view.nodePosition("/R/A", &x, &y));
  INK_CHECK(view.nodePosition("/R/B", &x, &y));
  std::printf("  disconnect keeps both endpoints on canvas ok\n");
  return true;
}

// The whole-prim relationship noodle END sits at the top-middle of the target
// ARROW: target top-center X, kRelArrowHeight (36 world units) above the
// target's top edge, and the arrow region routes to the graph.
bool TestRelationshipEndAtArrowTop() {
  ig::NoodlesGraphView view;
  view.setDocument(MakeRelStage());
  double bx = 0, by = 0, bw = 0, bh = 0;
  view.nodePosition("/R/B", &bx, &by);
  view.nodeSize("/R/B", &bw, &bh);
  bool found = false;
  for (const ig::GraphLinkInfo& l : view.links()) {
    if (!l.isRelationship || l.sourceNodeId != "/R/A" ||
        l.targetNodeId != "/R/B") {
      continue;
    }
    found = true;
    INK_CHECK(std::abs(l.endX - (bx + bw * 0.5)) < 1e-6);  // top-center X
    INK_CHECK(std::abs(l.endY - (by - 36.0)) < 1e-6);      // arrow top-middle
    // A point inside the arrow (just above the node's top edge) is a graph hit.
    INK_CHECK(view.hitsGraphElementAt(bx + bw * 0.5, by - 10.0));
  }
  INK_CHECK(found);
  std::printf("  relationship end anchors at the target arrow top ok\n");
  return true;
}

// A no-move tap on a group-header caret row folds the group (child rows hide,
// the node shrinks) and a second tap unfolds it.
bool TestHeaderTapTogglesFold() {
  ig::NoodlesGraphView view;
  view.setDocument(MakeRelStage());
  // A's "custom:link" is namespaced → grouped under a "custom" header row.
  auto pins = view.nodePins("/R/A");
  const ig::GraphPinInfo* header = FindPin(pins, "custom", /*out=*/true);
  INK_CHECK(header != nullptr);
  INK_CHECK(FindPin(pins, "custom:link", /*out=*/true) != nullptr);
  double ax = 0, ay = 0, aw0 = 0, ah0 = 0;
  view.nodePosition("/R/A", &ax, &ay);
  view.nodeSize("/R/A", &aw0, &ah0);

  // Tap the header row's middle: header rows never scrub or start a link.
  view.pointerDown(ax + aw0 * 0.5, header->centerY);
  view.pointerUp(ax + aw0 * 0.5, header->centerY);

  INK_CHECK(FindPin(view.nodePins("/R/A"), "custom:link", true) == nullptr);
  double aw1 = 0, ah1 = 0;
  view.nodeSize("/R/A", &aw1, &ah1);
  INK_CHECK(ah1 < ah0);  // the child row is hidden

  // Tap the (re-laid-out) header again → unfolds.
  auto pins2 = view.nodePins("/R/A");
  const ig::GraphPinInfo* header2 = FindPin(pins2, "custom", /*out=*/true);
  INK_CHECK(header2 != nullptr);
  view.pointerDown(ax + aw1 * 0.5, header2->centerY);
  view.pointerUp(ax + aw1 * 0.5, header2->centerY);
  INK_CHECK(FindPin(view.nodePins("/R/A"), "custom:link", true) != nullptr);
  std::printf("  header-row tap toggles group fold ok\n");
  return true;
}

// Long-press on a title row removes the node from the CANVAS: its edges are
// severed and its authored position cleared (one envelope); the prim survives
// on the stage, and its former neighbours stay pinned on the canvas.
bool TestLongPressRemovesNode() {
  std::shared_ptr<ig::InMemoryGraphDocument> stage = MakeRelStage();
  ig::NoodlesGraphView view;
  int beginCount = 0, endCount = 0;
  int topologyCount = 0;
  ig::GraphEditDelegate del;
  del.beginEdit = [&] { ++beginCount; };
  del.endEdit = [&] { ++endCount; };
  del.topologyEdited = [&] { ++topologyCount; };
  view.setDelegate(del);
  view.setDocument(stage);
  INK_CHECK(view.nodeCount() == 3);
  double ax = 0, ay = 0, aw = 0, ah = 0;
  view.nodePosition("/R/A", &ax, &ay);
  view.nodeSize("/R/A", &aw, &ah);

  INK_CHECK(view.longPressAt(ax + aw * 0.5, ay + 4.0));  // title-bar press
  INK_CHECK(view.nodeCount() == 2);
  INK_CHECK(!view.nodePosition("/R/A", nullptr, nullptr));
  INK_CHECK(view.nodePosition("/R/B", nullptr, nullptr));  // neighbours pinned
  INK_CHECK(view.nodePosition("/R/C", nullptr, nullptr));
  INK_CHECK(beginCount == 1 && endCount == 1);  // one undo envelope
  INK_CHECK(topologyCount == 1);

  INK_CHECK(stage->containsNode("/R/A"));  // backing object survives
  INK_CHECK(!HasEdge(stage, "/R/A", "custom:link", "/R/B", "", true));

  // A long-press on a ROW (not the title) is NOT a removal.
  double cx = 0, cy = 0, cw = 0, ch = 0;
  view.nodePosition("/R/C", &cx, &cy);
  view.nodeSize("/R/C", &cw, &ch);
  INK_CHECK(!view.longPressAt(cx + cw * 0.5, cy + ch - 4.0));
  INK_CHECK(view.nodeCount() == 2);
  std::printf("  long-press title removes node from canvas ok\n");
  return true;
}

// Dropping a prim from the outliner pins it onto the canvas as a node at the
// drop point, even with no edges; a fresh view over the same stage keeps it.
bool TestAddPrimNodeAt() {
  std::shared_ptr<ig::InMemoryGraphDocument> stage = MakeRelStage();
  stage->addNode(Node("/R/Loose"));  // no edges, no position
  ig::NoodlesGraphView view;
  int topologyCount = 0;
  ig::GraphEditDelegate del;
  del.topologyEdited = [&] { ++topologyCount; };
  view.setDelegate(del);
  view.setDocument(stage);
  INK_CHECK(view.nodeCount() == 3);  // a loose prim is NOT a node…

  INK_CHECK(view.addPrimNodeAt("/R/Loose", 400.0, 250.0));  // …until dropped
  INK_CHECK(view.nodeCount() == 4);
  INK_CHECK(topologyCount == 1);
  double x = 0, y = 0;
  INK_CHECK(view.nodePosition("/R/Loose", &x, &y));
  // Centered under the drop point using the default box (200×100) — the prim
  // was not yet displayed, so its real laid-out size was unknown at drop time.
  INK_CHECK(x == 300.0 && y == 200.0);
  INK_CHECK(view.selectedNodeId() == "/R/Loose");
  INK_CHECK(!view.addPrimNodeAt("/R/Missing", 0.0, 0.0));  // unknown prim

  // Round-trips: a fresh view over the same stage still shows it.
  ig::NoodlesGraphView view2;
  view2.setDocument(stage);
  INK_CHECK(view2.nodeCount() == 4);
  std::printf("  outliner drop pins prim as node ok\n");
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  struct Case { const char* name; bool (*fn)(); };
  const Case cases[] = {
      {"model_topology_and_pins", TestModelTopologyAndPins},
      {"explicit_output_direction_and_type",
       TestExplicitOutputDirectionAndType},
      {"unconnected_row_preview_anchor",
       TestUnconnectedRowDragPreviewUsesRowEdge},
      {"positions_and_layout", TestPositionsAndLayout},
      {"pointer_selects_node", TestPointerSelectsNode},
      {"pin_drag_authors_edge", TestPinDragAuthorsEdge},
      {"value_row_capture", TestValueRowCapture},
      {"display_only_row_activation_priority",
       TestDisplayOnlyRowActivationPriority},
      {"scrub_authors_value", TestScrubAuthorsValue},
      {"title_only_node_drag", TestTitleOnlyNodeDrag},
      {"pinch_zoom_not_compounding", TestPinchZoomNotCompounding},
      {"frame_all_fits_viewport", TestFrameAllFitsViewport},
      {"autolayout_fresh_no_overlap", TestAutolayoutFreshNoOverlap},
      {"autolayout_fan_siblings", TestAutolayoutFanSiblings},
      {"relationship_noodle_source_anchor", TestRelationshipNoodleSourceAnchor},
      {"relationship_namespaced_group_anchor", TestRelationshipNamespacedGroupAnchor},
      {"dynamic_zoom_floor", TestDynamicZoomFloor},
      {"minimap_drag_pan", TestMinimapDragPan},
      {"autolayout_incremental_preserves", TestAutolayoutIncrementalPreserves},
      {"scrub_authors_at_display_frame", TestScrubAuthorsAtDisplayFrame},
      {"external_edit_reflects", TestExternalEditReflects},
      {"hits_graph_element", TestHitsGraphElement},
      {"link_midpoint_hits", TestLinkMidpointHits},
      {"disconnect_connection_to_empty", TestDisconnectConnectionToEmpty},
      {"disconnect_relationship_to_empty", TestDisconnectRelationshipToEmpty},
      {"reconnect_connection_to_node", TestReconnectConnectionToNode},
      {"reconnect_relationship_to_node", TestReconnectRelationshipToNode},
      {"row_edge_band_authors_connection", TestRowEdgeBandAuthorsConnection},
      {"row_middle_still_scrubs", TestRowMiddleStillScrubs},
      {"link_touch_slop_lifts_off_curve", TestLinkTouchSlopLiftsOffCurve},
      {"band_touch_slop_narrow_node", TestBandTouchSlopNarrowNode},
      {"disconnect_keeps_nodes", TestDisconnectKeepsNodes},
      {"relationship_end_at_arrow_top", TestRelationshipEndAtArrowTop},
      {"header_tap_toggles_fold", TestHeaderTapTogglesFold},
      {"long_press_removes_node", TestLongPressRemovesNode},
      {"add_prim_node_at", TestAddPrimNodeAt},
  };
  for (const Case& tc : cases) {
    std::printf("[ RUN  ] %s\n", tc.name);
    if (tc.fn()) {
      std::printf("[ PASS ] %s\n", tc.name);
    } else {
      std::printf("[ FAIL ] %s\n", tc.name);
      ok = false;
    }
  }
  std::printf("%s\n", ok ? "ALL PASSED" : "FAILURES");
  return ok ? 0 : 1;
}
