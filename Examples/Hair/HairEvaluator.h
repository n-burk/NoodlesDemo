// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT
#pragma once

// Evaluation of the /Hair graph, driven from /Hair/Output backwards through
// the connections that actually exist. Nothing here consults node ids to
// decide the pipeline order: rewiring the graph in the editor rewires the
// groom, and a severed input produces an empty result plus a status message
// rather than the previous frame's geometry.

#include "HairClumpMap.h"
#include "HairGeneration.h"
#include "HairGeometry.h"
#include "HairGraph.h"
#include "HairTypes.h"

#include <map>
#include <string>

namespace noodles::demo::hair {

// Typed, node-owned groom data. Deliberately outside GraphProperty: the graph
// carries topology and scalar/Boolean controls, these carry geometry.
struct HairStores {
  // Authored guide curves per CreateGuides node: the seeded set plus anything
  // the Draw and Edit Points tools have added or moved.
  std::map<std::string, GuideSet> guides;
  // Comb displacements per GuideSculpt node, layered over whatever that node's
  // input evaluates to.
  std::map<std::string, SculptDeltas> sculpt;
  // Painted clump weights per Clump node, in scalp UV space. Kept separately
  // from the Voronoi sites so painting survives a change to the region count.
  std::map<std::string, ClumpPaint> clumpPaint;
};

// Material and visibility controls read from the evaluated Output node.
struct HairDisplaySettings {
  bool showHair = true;
  bool showGuides = true;
  bool showGrid = true;
  bool showScalp = true;
  bool scalpWireframe = false;
  bool showGuideGizmos = true;
  bool showBrushGizmo = true;
  bool showClumpGizmo = true;
  bool showClumpMap = false;
  float tint = 0.08f;
  float shine = 0.55f;
  float ambient = 0.30f;
};

// The result of one evaluation. Each stage is independently valid: losing the
// scalp input on Generate Hair empties the hair without pretending the guides
// vanished too, and `status` names the first break found.
struct HairEvalResult {
  ScalpMesh scalp;
  GuideSet guides;  // the guides that actually reach Generate Hair
  // The guides at the CreateGuides node's own output. This is the set the Edit
  // Points tool picks and drags, because its indices map one-to-one onto that
  // node's store; the displayed `guides` above have been through sculpt and
  // clump and no longer do.
  GuideSet guidesAtCreate;
  // The guides feeding the GuideSculpt node, which is the space its comb
  // deltas are indexed in.
  GuideSet sculptInputGuides;
  StrandSet hair;
  // The clamped parameters generation actually ran with, so the renderer and
  // the tests read the same numbers the synthesis used rather than the raw,
  // unclamped values sitting on the node.
  HairGenerationParameters generation;
  HairDisplaySettings display;

  bool valid = false;  // hair was produced
  std::string status;

  // Node ids of the stages that are genuinely part of the evaluated chain.
  // Gizmos are drawn only for these, so unplugging Clump removes its gizmo
  // along with its effect.
  std::string outputNodeId;
  std::string generateNodeId;
  std::string clumpNodeId;
  std::string sculptNodeId;
  std::string createGuidesNodeId;
  std::string scalpNodeId;

  ClumpState clump;
  BrushState brush;

  // The evaluated Clump node's region layout and painted weights, plus the
  // texture the scalp shader samples. Present only when a Clump is genuinely
  // in the chain.
  std::vector<ClumpSite> clumpSites;
  ClumpPaint clumpPaint;
  ClumpMapImage clumpMapImage;
  // Curves supplied through the Clump node's `clumps` input, if any. Regions
  // converge onto these instead of onto the mean of their own members.
  GuideSet clumpCurves;
  // True when the evaluated Clump sits after Generate Hair, so it clumped the
  // generated curves rather than the guides.
  bool clumpsGeneratedCurves = false;
};

// ── parameter readers ────────────────────────────────────────────────────────

ScalpParameters ReadScalpParameters(const GraphSnapshot& snapshot,
                                    const std::string& nodeId);
HairGenerationParameters ReadGenerationParameters(const GraphSnapshot& snapshot,
                                                  const std::string& nodeId);
ClumpState ReadClumpState(const GraphSnapshot& snapshot,
                          const std::string& nodeId);
BrushState ReadBrushState(const GraphSnapshot& snapshot,
                          const std::string& nodeId);

// The first node with the given schema type, in snapshot order.
const GraphNode* FindNodeBySchema(const GraphSnapshot& snapshot,
                                  const std::string& schemaTypeName);

// ── evaluation ───────────────────────────────────────────────────────────────

HairEvalResult EvaluateHairGraph(const GraphSnapshot& snapshot,
                                 const HairStores& stores);

// Cache key inputs. Camera state is deliberately not part of this, so orbiting
// never re-evaluates the groom.
HairEvalKey MakeEvalKey(const GraphSnapshot& snapshot,
                        const DirtyGenerations& generations);

}  // namespace noodles::demo::hair
