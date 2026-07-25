// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT

#include "HairEvaluator.h"

#include <set>

namespace noodles::demo::hair {
namespace {

std::uint32_t HashString(const std::string& text, std::uint32_t seed) {
  std::uint32_t hash = seed;
  for (const char character : text) {
    hash = HashCombine(hash, static_cast<std::uint32_t>(
                                 static_cast<unsigned char>(character)));
  }
  return hash;
}

std::uint32_t HashDouble(double value, std::uint32_t seed) {
  // Quantize before hashing so the key does not churn on the last bit of a
  // value that round-tripped through the document unchanged.
  const long long quantized = static_cast<long long>(value * 1048576.0);
  return HashCombine(
      HashCombine(seed, static_cast<std::uint32_t>(quantized & 0xffffffffll)),
      static_cast<std::uint32_t>((quantized >> 32) & 0xffffffffll));
}

// Walks the graph backwards from Output. `active` is the recursion stack, used
// only as a cycle guard: an invalid graph must produce a status message, never
// an unbounded recursion.
class Evaluator {
 public:
  Evaluator(const GraphSnapshot& snapshot, const HairStores& stores)
      : snapshot_(snapshot), stores_(stores) {}

  HairEvalResult Run() {
    HairEvalResult result;

    const GraphNode* output = FindNodeBySchema(snapshot_, schema::kOutput);
    if (!output) {
      result.status = "No Output node in the graph";
      return result;
    }
    result.outputNodeId = output->id;
    ReadDisplaySettings(output->id, &result.display);

    std::string sourceId;
    if (!ResolveInputSource(snapshot_, output->id, ports::kStrands, &sourceId,
                            nullptr)) {
      result.status = "Output has no hair input — connect Generate Hair";
      FinishDisplay(&result);
      return result;
    }
    const bool produced = EvaluateHair(sourceId, &result, &result.hair);
    result.valid = produced && result.hair.valid;
    if (!result.valid && result.status.empty()) {
      result.status = "Nothing reached the Output";
    }
    FinishDisplay(&result);
    return result;
  }

 private:
  // Display switches and gizmo state for the nodes that are genuinely in the
  // evaluated chain, plus the clump map the scalp shader samples.
  void FinishDisplay(HairEvalResult* result) {
    if (!result->scalpNodeId.empty()) {
      result->display.showScalp = PropertyBool(
          snapshot_, result->scalpNodeId, props::kScalpVisible, true);
      result->display.scalpWireframe = PropertyBool(
          snapshot_, result->scalpNodeId, props::kScalpWireframe, false);
    }
    if (!result->createGuidesNodeId.empty()) {
      result->display.showGuideGizmos = PropertyBool(
          snapshot_, result->createGuidesNodeId, props::kGuidesGizmos, true);
    }
    if (!result->sculptNodeId.empty()) {
      result->brush = ReadBrushState(snapshot_, result->sculptNodeId);
      result->display.showBrushGizmo = PropertyBool(
          snapshot_, result->sculptNodeId, props::kBrushGizmo, true);
    } else {
      result->display.showBrushGizmo = false;
    }
    if (!result->clumpNodeId.empty()) {
      result->clump = ReadClumpState(snapshot_, result->clumpNodeId);
      result->display.showClumpGizmo = PropertyBool(
          snapshot_, result->clumpNodeId, props::kClumpGizmo, true);
      result->display.showClumpMap = PropertyBool(
          snapshot_, result->clumpNodeId, props::kClumpShowMap, false);
      if (result->display.showClumpMap && result->scalp.valid &&
          !result->clumpSites.empty()) {
        result->clumpMapImage = BuildClumpMapImage(
            result->scalp, result->clumpSites, result->clumpPaint);
      }
    } else {
      result->display.showClumpGizmo = false;
      result->display.showClumpMap = false;
    }
  }

  // The hair path. A Clump may sit between Generate Hair and the Output, in
  // which case it clumps the generated curves instead of the guides.
  bool EvaluateHair(const std::string& nodeId, HairEvalResult* result,
                    StrandSet* outHair) {
    const GraphNode* node = FindNode(snapshot_, nodeId);
    if (!node) {
      SetStatus(result, "Missing upstream node " + nodeId);
      return false;
    }
    if (!active_.insert(nodeId).second) {
      SetStatus(result, "Feedback loop at " + node->name);
      return false;
    }
    const bool produced = EvaluateHairInner(*node, result, outHair);
    active_.erase(nodeId);
    if (!produced) *outHair = StrandSet{};
    return produced;
  }

  bool EvaluateHairInner(const GraphNode& node, HairEvalResult* result,
                         StrandSet* outHair) {
    if (node.schemaTypeName == schema::kGenerateHair) {
      result->generateNodeId = node.id;

      // Scalp and guides are resolved independently so a break in one is
      // reported without silently blanking the other.
      std::string scalpSourceId;
      bool haveScalp = false;
      if (!ResolveInputSource(snapshot_, node.id, ports::kScalpInput,
                              &scalpSourceId, nullptr)) {
        SetStatus(result, node.name + " has no scalp input");
      } else {
        haveScalp = EvaluateMesh(scalpSourceId, result, &result->scalp);
      }

      std::string guideSourceId;
      bool haveGuides = false;
      if (!ResolveInputSource(snapshot_, node.id, ports::kGuidesInput,
                              &guideSourceId, nullptr)) {
        SetStatus(result, node.name + " has no guides input");
      } else {
        haveGuides = EvaluateGuides(guideSourceId, result, &result->guides);
      }
      if (!haveScalp || !haveGuides) {
        if (result->status.empty()) {
          result->status = "Hair inputs are incomplete";
        }
        return false;
      }

      result->generation = ReadGenerationParameters(snapshot_, node.id);
      *outHair = GenerateHair(result->scalp, result->guides,
                              result->generation);
      if (!outHair->valid) {
        SetStatus(result, node.name + " produced no strands");
        return false;
      }
      return true;
    }

    if (node.schemaTypeName == schema::kClump) {
      std::string upstreamId;
      if (!ResolveInputSource(snapshot_, node.id, ports::kCurvesInput,
                              &upstreamId, nullptr)) {
        SetStatus(result, node.name + " has no curves input");
        return false;
      }
      StrandSet input;
      if (!EvaluateHair(upstreamId, result, &input)) return false;
      *outHair = ApplyClumpAt(node, result, input);
      result->clumpsGeneratedCurves = true;
      return outHair->valid;
    }

    SetStatus(result, node.name + " does not output hair");
    return false;
  }

  // Shared by both chains: build the node's Voronoi sites, look up its painted
  // weights, and record both so the renderer can draw the map.
  template <typename CurveSetT>
  CurveSetT ApplyClumpAt(const GraphNode& node, HairEvalResult* result,
                         const CurveSetT& input) {
    // Recorded only for a Clump that is genuinely on the chain to the Output.
    const bool isChainStage = !inSideBranch();
    if (isChainStage) result->clumpNodeId = node.id;

    const ClumpState state = ReadClumpState(snapshot_, node.id);
    if (!result->scalp.valid) {
      // Regions live on the scalp surface, so without one there is nothing to
      // partition. Pass the curves through rather than inventing a layout.
      SetStatus(result, node.name + " needs a scalp to place clump regions");
      return input;
    }

    const std::vector<ClumpSite> sites =
        BuildClumpSites(result->scalp, state.regionCount, state.regionSeed);
    ClumpPaint paint;
    const auto stored = stores_.clumpPaint.find(node.id);
    if (stored != stores_.clumpPaint.end()) paint = stored->second;

    // Optional explicit clump centers. Unconnected is the normal case and
    // means "clump onto your own mean"; a connected but broken upstream is a
    // real break and is reported rather than silently ignored.
    ClumpCenterCurves centers;
    GuideSet supplied;
    std::string clumpSourceId;
    if (ResolveInputSource(snapshot_, node.id, ports::kClumpInput,
                           &clumpSourceId, nullptr)) {
      SideBranchScope sideBranch(*this);
      if (EvaluateGuides(clumpSourceId, result, &supplied)) {
        centers = MakeClumpCenterCurves(supplied);
      } else {
        SetStatus(result, node.name + " has a broken clump-curves input");
      }
    }

    if (isChainStage) {
      result->clumpSites = sites;
      result->clumpPaint = paint;
      result->clumpCurves = supplied;
    }

    const std::uint32_t seed = HashString(node.id, 0x9e3779b9u);
    return ApplyClumpImpl(input, sites, paint, result->scalp, state, seed,
                          centers.empty() ? nullptr : &centers);
  }

  static GuideSet ApplyClumpImpl(const GuideSet& input,
                                 const std::vector<ClumpSite>& sites,
                                 const ClumpPaint& paint,
                                 const ScalpMesh& scalp,
                                 const ClumpState& state, std::uint32_t seed,
                                 const ClumpCenterCurves* centers) {
    return ApplyClumpToGuides(input, sites, paint, scalp, state, seed, centers);
  }

  static StrandSet ApplyClumpImpl(const StrandSet& input,
                                  const std::vector<ClumpSite>& sites,
                                  const ClumpPaint& paint,
                                  const ScalpMesh& scalp,
                                  const ClumpState& state, std::uint32_t seed,
                                  const ClumpCenterCurves* centers) {
    return ApplyClumpToHair(input, sites, paint, scalp, state, seed, centers);
  }

  void SetStatus(HairEvalResult* result, const std::string& message) const {
    if (result->status.empty()) result->status = message;
  }

  void ReadDisplaySettings(const std::string& outputId,
                           HairDisplaySettings* display) const {
    display->showHair = PropertyBool(snapshot_, outputId, props::kShowHair,
                                     true);
    display->showGuides =
        PropertyBool(snapshot_, outputId, props::kShowGuides, true);
    display->showGrid =
        PropertyBool(snapshot_, outputId, props::kShowGrid, true);
    display->tint = static_cast<float>(
        Clamp(static_cast<float>(
                  PropertyNumber(snapshot_, outputId, props::kMaterialTint,
                                 0.08)),
              0.0f, 1.0f));
    display->shine = Clamp(
        static_cast<float>(
            PropertyNumber(snapshot_, outputId, props::kMaterialShine, 0.55)),
        0.0f, 2.0f);
    display->ambient = Clamp(
        static_cast<float>(
            PropertyNumber(snapshot_, outputId, props::kMaterialAmbient, 0.30)),
        0.0f, 1.0f);
  }

  bool EvaluateMesh(const std::string& nodeId, HairEvalResult* result,
                    ScalpMesh* outMesh) {
    const GraphNode* node = FindNode(snapshot_, nodeId);
    if (!node) {
      SetStatus(result, "Missing upstream node " + nodeId);
      return false;
    }
    if (node->schemaTypeName != schema::kScalp) {
      SetStatus(result, node->name + " does not output a scalp mesh");
      return false;
    }
    if (!active_.insert(nodeId).second) {
      SetStatus(result, "Feedback loop at " + node->name);
      return false;
    }
    *outMesh = BuildScalpMesh(ReadScalpParameters(snapshot_, nodeId));
    if (!inSideBranch()) result->scalpNodeId = nodeId;
    active_.erase(nodeId);
    return outMesh->valid;
  }

  bool EvaluateGuides(const std::string& nodeId, HairEvalResult* result,
                      GuideSet* outGuides) {
    const GraphNode* node = FindNode(snapshot_, nodeId);
    if (!node) {
      SetStatus(result, "Missing upstream node " + nodeId);
      return false;
    }
    if (!active_.insert(nodeId).second) {
      SetStatus(result, "Feedback loop at " + node->name);
      return false;
    }
    const bool produced = EvaluateGuidesInner(*node, result, outGuides);
    active_.erase(nodeId);
    if (!produced) *outGuides = GuideSet{};
    return produced;
  }

  bool EvaluateGuidesInner(const GraphNode& node, HairEvalResult* result,
                           GuideSet* outGuides) {
    const std::string& schemaName = node.schemaTypeName;

    if (schemaName == schema::kCreateGuides) {
      // Guides only exist relative to a scalp: without one there is nothing to
      // root them to, so this is a hard break rather than a pass-through.
      std::string scalpSourceId;
      if (!ResolveInputSource(snapshot_, node.id, ports::kScalpInput,
                              &scalpSourceId, nullptr)) {
        SetStatus(result, node.name + " has no scalp input");
        return false;
      }
      ScalpMesh scalp;
      if (!EvaluateMesh(scalpSourceId, result, &scalp)) return false;

      const auto stored = stores_.guides.find(node.id);
      if (stored == stores_.guides.end() || !stored->second.valid) {
        SetStatus(result, node.name + " has no guides");
        return false;
      }
      const int pointCount = ClampInt(
          static_cast<int>(std::lround(PropertyNumber(
              snapshot_, node.id, props::kDrawPoints, 14.0))),
          3, 48);
      GuideSet guides = EvaluateStoredGuides(
          scalp, stored->second, pointCount,
          static_cast<float>(PropertyNumber(snapshot_, node.id,
                                            props::kDrawSmoothing, 0.35)),
          PropertyBool(snapshot_, node.id, props::kDrawSnapRoot, true));
      if (guides.curves.empty()) {
        SetStatus(result, node.name + " has no guides");
        return false;
      }
      guides.valid = true;
      if (!inSideBranch()) {
        result->guidesAtCreate = guides;
        result->createGuidesNodeId = node.id;
      }
      *outGuides = std::move(guides);
      return true;
    }

    if (schemaName == schema::kGuideSculpt) {
      std::string upstreamId;
      if (!ResolveInputSource(snapshot_, node.id, ports::kGuidesInput,
                              &upstreamId, nullptr)) {
        SetStatus(result, node.name + " has no guides input");
        return false;
      }
      GuideSet input;
      if (!EvaluateGuides(upstreamId, result, &input)) return false;
      if (!inSideBranch()) result->sculptInputGuides = input;

      const auto stored = stores_.sculpt.find(node.id);
      if (stored != stores_.sculpt.end() && !stored->second.empty()) {
        // Conform on a copy: the store is keyed to whatever point count was
        // current when the stroke was made, and the upstream count can change
        // underneath it at any time.
        SculptDeltas deltas = stored->second;
        ConformDeltas(deltas, input);
        *outGuides = ApplySculptDeltas(input, deltas);
      } else {
        *outGuides = input;
      }
      if (!inSideBranch()) result->sculptNodeId = node.id;
      return true;
    }

    if (schemaName == schema::kClump) {
      std::string upstreamId;
      if (!ResolveInputSource(snapshot_, node.id, ports::kCurvesInput,
                              &upstreamId, nullptr)) {
        SetStatus(result, node.name + " has no curves input");
        return false;
      }
      GuideSet input;
      if (!EvaluateGuides(upstreamId, result, &input)) return false;
      *outGuides = ApplyClumpAt(node, result, input);
      return outGuides->valid;
    }

    SetStatus(result, node.name + " does not output guides");
    return false;
  }

  // A side branch is an input that feeds a stage without being the stage
  // itself — currently only a Clump's `clumps` input. Nodes reached through
  // one must not be recorded as the groom's stages: which Create Guides the
  // guide tools edit is decided by the chain that reaches the Output, not by
  // whichever branch happened to evaluate last.
  class SideBranchScope {
   public:
    explicit SideBranchScope(Evaluator& evaluator) : evaluator_(evaluator) {
      ++evaluator_.sideBranchDepth_;
    }
    ~SideBranchScope() { --evaluator_.sideBranchDepth_; }

   private:
    Evaluator& evaluator_;
  };
  bool inSideBranch() const { return sideBranchDepth_ > 0; }

  const GraphSnapshot& snapshot_;
  const HairStores& stores_;
  std::set<std::string> active_;
  int sideBranchDepth_ = 0;
};

}  // namespace

const GraphNode* FindNodeBySchema(const GraphSnapshot& snapshot,
                                  const std::string& schemaTypeName) {
  for (const GraphNode& node : snapshot.nodes) {
    if (node.schemaTypeName == schemaTypeName) return &node;
  }
  return nullptr;
}

ScalpParameters ReadScalpParameters(const GraphSnapshot& snapshot,
                                    const std::string& nodeId) {
  ScalpParameters parameters;
  parameters.radius = Clamp(
      static_cast<float>(
          PropertyNumber(snapshot, nodeId, props::kScalpRadius, 1.0)),
      0.15f, 4.0f);
  parameters.height = Clamp(
      static_cast<float>(
          PropertyNumber(snapshot, nodeId, props::kScalpHeight, 1.18)),
      0.3f, 3.0f);
  parameters.roundness = Saturate(static_cast<float>(
      PropertyNumber(snapshot, nodeId, props::kScalpRoundness, 0.62)));
  return parameters;
}

HairGenerationParameters ReadGenerationParameters(const GraphSnapshot& snapshot,
                                                  const std::string& nodeId) {
  HairGenerationParameters parameters;
  parameters.density = static_cast<int>(std::lround(
      PropertyNumber(snapshot, nodeId, props::kHairDensity, 900.0)));
  parameters.segments = static_cast<int>(std::lround(
      PropertyNumber(snapshot, nodeId, props::kHairSegments, 16.0)));
  parameters.width = static_cast<float>(
      PropertyNumber(snapshot, nodeId, props::kHairWidth, 0.0055));
  parameters.spread = static_cast<float>(
      PropertyNumber(snapshot, nodeId, props::kHairSpread, 0.35));
  parameters.variation = static_cast<float>(
      PropertyNumber(snapshot, nodeId, props::kHairVariation, 0.28));
  const double seed = PropertyNumber(snapshot, nodeId, props::kHairSeed, 1337.0);
  parameters.seed =
      static_cast<std::uint32_t>(std::llround(std::fabs(seed))) | 1u;
  // GraphEditor deliberately does not clamp a scrub, so the ranges the
  // renderer depends on are enforced here, once, on read.
  return ClampGenerationParameters(parameters);
}

ClumpState ReadClumpState(const GraphSnapshot& snapshot,
                          const std::string& nodeId) {
  ClumpState clump;
  clump.strength = Saturate(static_cast<float>(
      PropertyNumber(snapshot, nodeId, props::kClumpStrength, 0.55)));
  clump.tipBias = Clamp(
      static_cast<float>(
          PropertyNumber(snapshot, nodeId, props::kClumpTipBias, 2.2)),
      0.05f, 12.0f);
  clump.radius = Clamp(
      static_cast<float>(
          PropertyNumber(snapshot, nodeId, props::kClumpRadius, 0.85)),
      0.02f, 8.0f);
  clump.noise = Clamp(
      static_cast<float>(
          PropertyNumber(snapshot, nodeId, props::kClumpNoise, 0.18)),
      0.0f, 3.0f);
  clump.regionCount = ClampInt(
      static_cast<int>(std::lround(
          PropertyNumber(snapshot, nodeId, props::kClumpRegions, 28.0))),
      1, 512);
  clump.regionSeed = static_cast<std::uint32_t>(std::llround(std::fabs(
                         PropertyNumber(snapshot, nodeId,
                                        props::kClumpRegionSeed, 5.0)))) |
                     1u;
  clump.paintRadius = Clamp(
      static_cast<float>(
          PropertyNumber(snapshot, nodeId, props::kClumpPaintRadius, 0.28)),
      0.01f, 4.0f);
  clump.paintAmount = Saturate(static_cast<float>(
      PropertyNumber(snapshot, nodeId, props::kClumpPaintAmount, 0.55)));
  clump.paintErase =
      PropertyBool(snapshot, nodeId, props::kClumpPaintErase, false);
  clump.center = Vec3{
      static_cast<float>(
          PropertyNumber(snapshot, nodeId, props::kClumpCenterX, 0.0)),
      static_cast<float>(
          PropertyNumber(snapshot, nodeId, props::kClumpCenterY, 1.62)),
      static_cast<float>(
          PropertyNumber(snapshot, nodeId, props::kClumpCenterZ, -0.34))};
  return clump;
}

BrushState ReadBrushState(const GraphSnapshot& snapshot,
                          const std::string& nodeId) {
  BrushState brush;
  brush.radius = Clamp(
      static_cast<float>(
          PropertyNumber(snapshot, nodeId, props::kBrushRadius, 0.30)),
      0.01f, 4.0f);
  brush.strength = Saturate(static_cast<float>(
      PropertyNumber(snapshot, nodeId, props::kBrushStrength, 0.55)));
  brush.falloff = Clamp(
      static_cast<float>(
          PropertyNumber(snapshot, nodeId, props::kBrushFalloff, 2.0)),
      0.2f, 12.0f);
  brush.smoothing = Saturate(static_cast<float>(
      PropertyNumber(snapshot, nodeId, props::kBrushSmooth, 0.35)));
  return brush;
}

HairEvalResult EvaluateHairGraph(const GraphSnapshot& snapshot,
                                 const HairStores& stores) {
  Evaluator evaluator(snapshot, stores);
  return evaluator.Run();
}

HairEvalKey MakeEvalKey(const GraphSnapshot& snapshot,
                        const DirtyGenerations& generations) {
  HairEvalKey key;
  std::uint32_t topology = 0x1234567u;
  std::uint32_t parameters = 0x89abcdefu;
  for (const GraphNode& node : snapshot.nodes) {
    topology = HashString(node.id, topology);
    topology = HashString(node.schemaTypeName, topology);
    for (const GraphProperty& property : node.properties) {
      topology = HashString(property.name, topology);
      if (property.hasValue) {
        parameters = HashString(property.name, parameters);
        parameters = HashDouble(property.numericValue, parameters);
      }
    }
  }
  for (const GraphEdge& edge : snapshot.edges) {
    topology = HashString(edge.sourceNodeId, topology);
    topology = HashString(edge.sourcePort, topology);
    topology = HashString(edge.targetNodeId, topology);
    topology = HashString(edge.targetPort, topology);
    topology = HashCombine(topology, edge.isRelationship ? 1u : 0u);
  }
  key.topologyHash = topology;
  key.parameterHash = parameters;
  key.guideStore = generations.guideStore;
  key.sculptStore = generations.sculptStore;
  key.paintStore = generations.paintStore;
  return key;
}

}  // namespace noodles::demo::hair
