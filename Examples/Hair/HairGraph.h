// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT
#pragma once

// The /Hair node graph: ids, schema types, port and property names, and the
// document that wires them together. Only topology plus scalar/Boolean
// controls live here — the typed scalp, guide, and strand data is owned by
// HairScene and keyed by node id.

#include <noodles/demo/GraphDocument.h>

#include <memory>
#include <string>
#include <vector>

namespace noodles::demo {
class InMemoryGraphDocument;
}  // namespace noodles::demo

namespace noodles::demo::hair {

// Node ids. Path-like for readability; the editor treats them as opaque keys.
namespace ids {
inline constexpr const char* kScalp = "/Hair/Scalp";
inline constexpr const char* kCreateGuides = "/Hair/CreateGuides";
inline constexpr const char* kGuideSculpt = "/Hair/GuideSculpt";
inline constexpr const char* kClump = "/Hair/Clump";
inline constexpr const char* kGenerateHair = "/Hair/GenerateHair";
inline constexpr const char* kOutput = "/Hair/Output";
}  // namespace ids

// Schema type names. Evaluation dispatches on these rather than on node ids so
// a renamed or duplicated node still evaluates as its kind.
namespace schema {
inline constexpr const char* kScalp = "HairScalp";
inline constexpr const char* kCreateGuides = "HairCreateGuides";
inline constexpr const char* kGuideSculpt = "HairGuideSculpt";
inline constexpr const char* kClump = "HairClump";
inline constexpr const char* kGenerateHair = "HairGenerate";
inline constexpr const char* kOutput = "HairOutput";
}  // namespace schema

// Port names.
//
// Port names deliberately never collide with a property group prefix. Noodles
// derives a foldable group header whose pin name is the bare prefix, so a node
// carrying `hair:*` rows must not also expose a port literally named `hair` —
// the header row would shadow the port for row-kind lookups and the port would
// start toggling the fold instead of authoring a link. Hence `strands`.
namespace ports {
inline constexpr const char* kMesh = "mesh";          // Scalp output
inline constexpr const char* kScalpInput = "scalp";   // consumers' scalp input
inline constexpr const char* kGuidesInput = "guides"; // consumers' guide input
inline constexpr const char* kGuidesOutput = "guides";    // CreateGuides
inline constexpr const char* kSculptedOutput = "sculpted";  // GuideSculpt
// Clump accepts either guides or generated curves, so its port is typed
// `curves` rather than committing to one of them.
inline constexpr const char* kCurvesInput = "curves";       // Clump
// Optional explicit clump centers. Leave it unconnected and each region
// clumps onto the mean of its own members instead.
inline constexpr const char* kClumpInput = "clumps";        // Clump
inline constexpr const char* kClumpedOutput = "clumped";    // Clump
inline constexpr const char* kStrands = "strands";  // GenerateHair -> Output
}  // namespace ports

// Property names, grouped with the `tool:`, `draw:`, `brush:`, `clump:`,
// `hair:`, and `display:` prefixes the editor renders as foldable headers.
namespace props {
// Scalp
inline constexpr const char* kScalpRadius = "radius";
inline constexpr const char* kScalpHeight = "height";
inline constexpr const char* kScalpRoundness = "roundness";
inline constexpr const char* kScalpVisible = "display:visible";
inline constexpr const char* kScalpWireframe = "display:wireframe";

// CreateGuides
inline constexpr const char* kToolDraw = "tool:draw";
inline constexpr const char* kToolEditPoints = "tool:editPoints";
inline constexpr const char* kDrawSnapRoot = "draw:snapRoot";
inline constexpr const char* kDrawSpacing = "draw:spacing";
inline constexpr const char* kDrawSmoothing = "draw:smoothing";
inline constexpr const char* kDrawPoints = "draw:points";
inline constexpr const char* kDrawLength = "draw:length";
inline constexpr const char* kGuidesGizmos = "display:gizmos";

// GuideSculpt
inline constexpr const char* kToolComb = "tool:comb";
inline constexpr const char* kBrushRadius = "brush:radius";
inline constexpr const char* kBrushStrength = "brush:strength";
inline constexpr const char* kBrushFalloff = "brush:falloff";
inline constexpr const char* kBrushSmooth = "brush:smooth";
inline constexpr const char* kBrushGizmo = "display:brush";

// Clump
inline constexpr const char* kToolEditClump = "tool:editClump";
inline constexpr const char* kToolPaintClump = "tool:paintClump";
inline constexpr const char* kClumpStrength = "clump:strength";
inline constexpr const char* kClumpTipBias = "clump:tipBias";
inline constexpr const char* kClumpRadius = "clump:radius";
inline constexpr const char* kClumpNoise = "clump:noise";
inline constexpr const char* kClumpCenterX = "clump:centerX";
inline constexpr const char* kClumpCenterY = "clump:centerY";
inline constexpr const char* kClumpCenterZ = "clump:centerZ";
inline constexpr const char* kClumpRegions = "clump:regions";
inline constexpr const char* kClumpRegionSeed = "clump:regionSeed";
inline constexpr const char* kClumpPaintRadius = "clump:paintRadius";
inline constexpr const char* kClumpPaintAmount = "clump:paintAmount";
inline constexpr const char* kClumpPaintErase = "clump:paintErase";
inline constexpr const char* kClumpGizmo = "display:gizmo";
inline constexpr const char* kClumpShowMap = "display:clumpMap";

// GenerateHair
inline constexpr const char* kHairDensity = "hair:density";
inline constexpr const char* kHairSegments = "hair:segments";
inline constexpr const char* kHairWidth = "hair:width";
inline constexpr const char* kHairSpread = "hair:spread";
inline constexpr const char* kHairVariation = "hair:variation";
inline constexpr const char* kHairSeed = "hair:seed";

// Output
inline constexpr const char* kShowHair = "display:hair";
inline constexpr const char* kShowGuides = "display:guides";
inline constexpr const char* kShowGrid = "display:grid";
inline constexpr const char* kMaterialTint = "display:tint";
inline constexpr const char* kMaterialShine = "display:shine";
inline constexpr const char* kMaterialAmbient = "display:ambient";
}  // namespace props

// ── node palette ─────────────────────────────────────────────────────────────

// The /Hair node types, which is also the palette the demos' Add Node control
// offers while the groom is up.
enum class HairNodeKind {
  Scalp = 0,
  CreateGuides,
  GuideSculpt,
  Clump,
  GenerateHair,
  Output,
};

inline constexpr int kHairNodeKindCount = 6;

const char* HairNodeKindTitle(HairNodeKind kind);
const char* HairNodeKindSchema(HairNodeKind kind);

// Build one node of `kind`. The canonical graph is assembled from exactly
// these, so a node added at runtime is indistinguishable from one that shipped
// in the fixture — same rows, same groups, same ports.
GraphNode MakeHairNode(HairNodeKind kind, std::string id, std::string name);

// ── tool switches ────────────────────────────────────────────────────────────

// One mutating tool switch found in the graph. Exactly one may be on at a
// time; HairScene enforces that when a switch is toggled, which is what keeps
// the node switches authoritative.
struct ToolSwitch {
  std::string nodeId;
  std::string propertyName;
};

// Every tool switch present in `snapshot`, in node order then row order.
//
// Derived from the snapshot rather than from a fixed list of node ids, so a
// second Clump or Guide Sculpt added at runtime arms its tool exactly like the
// original does.
std::vector<ToolSwitch> ToolSwitchesIn(const GraphSnapshot& snapshot);

// ── snapshot helpers ─────────────────────────────────────────────────────────

const GraphNode* FindNode(const GraphSnapshot& snapshot,
                          const std::string& nodeId);
const GraphProperty* FindProperty(const GraphSnapshot& snapshot,
                                  const std::string& nodeId,
                                  const std::string& propertyName);

double PropertyNumber(const GraphSnapshot& snapshot, const std::string& nodeId,
                      const std::string& propertyName, double fallback);
bool PropertyBool(const GraphSnapshot& snapshot, const std::string& nodeId,
                  const std::string& propertyName, bool fallback);

// Resolve what actually drives `inputPort` on `nodeId`.
//
// GraphEdge uses document-authoring orientation: the edge's source endpoint is
// the INPUT being authored and its target endpoint is the upstream OUTPUT. All
// evaluation traversal goes through this one helper so that inversion is
// stated once instead of at every call site.
bool ResolveInputSource(const GraphSnapshot& snapshot,
                        const std::string& nodeId,
                        const std::string& inputPort,
                        std::string* outUpstreamNodeId,
                        std::string* outUpstreamPort);

// ── document ─────────────────────────────────────────────────────────────────

// Build the six-node groom graph, fully connected:
//   Scalp -> CreateGuides -> GuideSculpt -> Clump -> GenerateHair -> Output
//   Scalp -> GenerateHair
// No node stores a position, so the editor's deterministic layered auto-layout
// places the chain exactly as it does for the other demo graphs.
std::shared_ptr<InMemoryGraphDocument> CreateHairGraphDocument();

GraphSnapshot BuildHairGraphSnapshot();

}  // namespace noodles::demo::hair
