// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT

#include "HairGraph.h"

#include <noodles/demo/InMemoryGraphDocument.h>

#include <cstdio>
#include <utility>

namespace noodles::demo::hair {
namespace {

// Compact fixed-point display so a scrubbed row does not render a 17-digit
// double. GraphEditor rewrites this text itself once a scrub starts; this is
// only the initial presentation.
std::string FormatNumber(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%g", value);
  return std::string(buffer);
}

GraphProperty Number(std::string name, double value) {
  GraphProperty property;
  property.name = std::move(name);
  property.type = "float";
  property.hasValue = true;
  property.isScrubable = true;
  property.numericValue = value;
  property.displayValue = FormatNumber(value);
  return property;
}

// A Boolean row must be scrubable for GraphEditor to map a no-move row tap to
// an atomic toggle; a non-scrubable value row becomes an activation instead.
GraphProperty Toggle(std::string name, bool value) {
  GraphProperty property;
  property.name = std::move(name);
  property.type = "bool";
  property.hasValue = true;
  property.isScrubable = true;
  property.numericValue = value ? 1.0 : 0.0;
  property.displayValue = value ? "on" : "off";
  return property;
}

GraphProperty Port(std::string name, std::string type,
                   GraphPropertyDirection direction) {
  GraphProperty property;
  property.name = std::move(name);
  property.type = std::move(type);
  property.direction = direction;
  return property;
}

GraphProperty Input(std::string name, std::string type) {
  return Port(std::move(name), std::move(type), GraphPropertyDirection::Input);
}

GraphProperty Output(std::string name, std::string type) {
  return Port(std::move(name), std::move(type), GraphPropertyDirection::Output);
}

GraphNode Node(std::string id, std::string name, std::string schemaTypeName,
               std::vector<GraphProperty> properties) {
  GraphNode node;
  node.id = std::move(id);
  node.name = std::move(name);
  node.schemaTypeName = std::move(schemaTypeName);
  node.properties = std::move(properties);
  return node;
}

}  // namespace

const char* HairNodeKindTitle(HairNodeKind kind) {
  switch (kind) {
    case HairNodeKind::Scalp: return "Scalp";
    case HairNodeKind::CreateGuides: return "Create Guides";
    case HairNodeKind::GuideSculpt: return "Guide Sculpt";
    case HairNodeKind::Clump: return "Clump";
    case HairNodeKind::GenerateHair: return "Generate Hair";
    case HairNodeKind::Output: return "Output";
  }
  return "Scalp";
}

const char* HairNodeKindSchema(HairNodeKind kind) {
  switch (kind) {
    case HairNodeKind::Scalp: return schema::kScalp;
    case HairNodeKind::CreateGuides: return schema::kCreateGuides;
    case HairNodeKind::GuideSculpt: return schema::kGuideSculpt;
    case HairNodeKind::Clump: return schema::kClump;
    case HairNodeKind::GenerateHair: return schema::kGenerateHair;
    case HairNodeKind::Output: return schema::kOutput;
  }
  return schema::kScalp;
}

GraphNode MakeHairNode(HairNodeKind kind, std::string id, std::string name) {
  switch (kind) {
    case HairNodeKind::Scalp:
      return Node(std::move(id), std::move(name), schema::kScalp,
                  {Number(props::kScalpRadius, 1.0),
                   Number(props::kScalpHeight, 1.18),
                   Number(props::kScalpRoundness, 0.62),
                   Toggle(props::kScalpVisible, true),
                   Toggle(props::kScalpWireframe, false),
                   Output(ports::kMesh, "mesh")});
    case HairNodeKind::CreateGuides:
      return Node(std::move(id), std::move(name), schema::kCreateGuides,
                  {Input(ports::kScalpInput, "mesh"),
                   Toggle(props::kToolDraw, false),
                   Toggle(props::kToolEditPoints, false),
                   Toggle(props::kDrawSnapRoot, true),
                   Number(props::kDrawSpacing, 0.05),
                   Number(props::kDrawSmoothing, 0.35),
                   Number(props::kDrawPoints, 14.0),
                   Number(props::kDrawLength, 0.62),
                   Toggle(props::kGuidesGizmos, true),
                   Output(ports::kGuidesOutput, "guides")});
    case HairNodeKind::GuideSculpt:
      return Node(std::move(id), std::move(name), schema::kGuideSculpt,
                  {Input(ports::kGuidesInput, "guides"),
                   Toggle(props::kToolComb, false),
                   Number(props::kBrushRadius, 0.30),
                   Number(props::kBrushStrength, 0.55),
                   Number(props::kBrushFalloff, 2.0),
                   Number(props::kBrushSmooth, 0.35),
                   Toggle(props::kBrushGizmo, true),
                   Output(ports::kSculptedOutput, "guides")});
    case HairNodeKind::Clump:
      return Node(std::move(id), std::move(name), schema::kClump,
                  {Input(ports::kCurvesInput, "curves"),
                   Input(ports::kClumpInput, "curves"),
                   Toggle(props::kToolEditClump, false),
                   Toggle(props::kToolPaintClump, false),
                   Number(props::kClumpStrength, 0.55),
                   Number(props::kClumpTipBias, 2.2),
                   Number(props::kClumpRadius, 0.85),
                   Number(props::kClumpNoise, 0.18),
                   Number(props::kClumpRegions, 28.0),
                   Number(props::kClumpRegionSeed, 5.0),
                   Number(props::kClumpPaintRadius, 0.28),
                   Number(props::kClumpPaintAmount, 0.55),
                   Toggle(props::kClumpPaintErase, false),
                   Number(props::kClumpCenterX, 0.0),
                   Number(props::kClumpCenterY, 1.62),
                   Number(props::kClumpCenterZ, -0.34),
                   Toggle(props::kClumpGizmo, true),
                   Toggle(props::kClumpShowMap, false),
                   Output(ports::kClumpedOutput, "curves")});
    case HairNodeKind::GenerateHair:
      return Node(std::move(id), std::move(name), schema::kGenerateHair,
                  {Input(ports::kScalpInput, "mesh"),
                   Input(ports::kGuidesInput, "guides"),
                   Number(props::kHairDensity, 900.0),
                   Number(props::kHairSegments, 16.0),
                   Number(props::kHairWidth, 0.0055),
                   Number(props::kHairSpread, 0.35),
                   Number(props::kHairVariation, 0.28),
                   Number(props::kHairSeed, 1337.0),
                   Output(ports::kStrands, "hair")});
    case HairNodeKind::Output:
      return Node(std::move(id), std::move(name), schema::kOutput,
                  {Input(ports::kStrands, "hair"),
                   Toggle(props::kShowHair, true),
                   Toggle(props::kShowGuides, true),
                   Toggle(props::kShowGrid, true),
                   Number(props::kMaterialTint, 0.08),
                   Number(props::kMaterialShine, 0.55),
                   Number(props::kMaterialAmbient, 0.30)});
  }
  return Node(std::move(id), std::move(name), schema::kScalp, {});
}

std::vector<ToolSwitch> ToolSwitchesIn(const GraphSnapshot& snapshot) {
  // Row order within a node is the authored order, so the result is stable and
  // ActiveTool's "first one wins" tie-break is deterministic.
  static const char* const kSwitchNames[] = {
      props::kToolDraw, props::kToolEditPoints, props::kToolComb,
      props::kToolEditClump, props::kToolPaintClump};

  std::vector<ToolSwitch> switches;
  for (const GraphNode& node : snapshot.nodes) {
    for (const GraphProperty& property : node.properties) {
      for (const char* name : kSwitchNames) {
        if (property.name != name) continue;
        switches.push_back(ToolSwitch{node.id, property.name});
        break;
      }
    }
  }
  return switches;
}

const GraphNode* FindNode(const GraphSnapshot& snapshot,
                          const std::string& nodeId) {
  for (const GraphNode& node : snapshot.nodes) {
    if (node.id == nodeId) return &node;
  }
  return nullptr;
}

const GraphProperty* FindProperty(const GraphSnapshot& snapshot,
                                  const std::string& nodeId,
                                  const std::string& propertyName) {
  const GraphNode* node = FindNode(snapshot, nodeId);
  if (!node) return nullptr;
  for (const GraphProperty& property : node->properties) {
    if (property.name == propertyName) return &property;
  }
  return nullptr;
}

double PropertyNumber(const GraphSnapshot& snapshot, const std::string& nodeId,
                      const std::string& propertyName, double fallback) {
  const GraphProperty* property = FindProperty(snapshot, nodeId, propertyName);
  if (!property || !property->hasValue) return fallback;
  return property->numericValue;
}

bool PropertyBool(const GraphSnapshot& snapshot, const std::string& nodeId,
                  const std::string& propertyName, bool fallback) {
  const GraphProperty* property = FindProperty(snapshot, nodeId, propertyName);
  if (!property || !property->hasValue) return fallback;
  return property->numericValue != 0.0;
}

bool ResolveInputSource(const GraphSnapshot& snapshot,
                        const std::string& nodeId,
                        const std::string& inputPort,
                        std::string* outUpstreamNodeId,
                        std::string* outUpstreamPort) {
  for (const GraphEdge& edge : snapshot.edges) {
    if (edge.isRelationship) continue;
    if (edge.sourceNodeId != nodeId || edge.sourcePort != inputPort) continue;
    if (outUpstreamNodeId) *outUpstreamNodeId = edge.targetNodeId;
    if (outUpstreamPort) *outUpstreamPort = edge.targetPort;
    return true;
  }
  return false;
}

GraphSnapshot BuildHairGraphSnapshot() {
  GraphSnapshot snapshot;
  snapshot.nodes.push_back(
      MakeHairNode(HairNodeKind::Scalp, ids::kScalp, "Scalp"));
  snapshot.nodes.push_back(MakeHairNode(HairNodeKind::CreateGuides,
                                        ids::kCreateGuides, "Create Guides"));
  snapshot.nodes.push_back(MakeHairNode(HairNodeKind::GuideSculpt,
                                        ids::kGuideSculpt, "Guide Sculpt"));
  snapshot.nodes.push_back(
      MakeHairNode(HairNodeKind::Clump, ids::kClump, "Clump"));
  snapshot.nodes.push_back(MakeHairNode(HairNodeKind::GenerateHair,
                                        ids::kGenerateHair, "Generate Hair"));
  snapshot.nodes.push_back(
      MakeHairNode(HairNodeKind::Output, ids::kOutput, "Output"));

  // Document-authoring orientation: {inputNode, inputPort, outputNode,
  // outputPort}. See ResolveInputSource.
  snapshot.edges.push_back(
      {ids::kCreateGuides, ports::kScalpInput, ids::kScalp, ports::kMesh,
       false});
  snapshot.edges.push_back({ids::kGuideSculpt, ports::kGuidesInput,
                            ids::kCreateGuides, ports::kGuidesOutput, false});
  snapshot.edges.push_back({ids::kClump, ports::kCurvesInput,
                            ids::kGuideSculpt, ports::kSculptedOutput, false});
  snapshot.edges.push_back({ids::kGenerateHair, ports::kGuidesInput,
                            ids::kClump, ports::kClumpedOutput, false});
  snapshot.edges.push_back({ids::kGenerateHair, ports::kScalpInput,
                            ids::kScalp, ports::kMesh, false});
  snapshot.edges.push_back({ids::kOutput, ports::kStrands, ids::kGenerateHair,
                            ports::kStrands, false});
  return snapshot;
}

std::shared_ptr<InMemoryGraphDocument> CreateHairGraphDocument() {
  return std::make_shared<InMemoryGraphDocument>(BuildHairGraphSnapshot());
}

}  // namespace noodles::demo::hair
