#include "DemoGraphFixture.h"

#include <noodles/demo/GraphDocument.h>
#include <noodles/demo/GraphEditor.h>
#include <noodles/demo/InMemoryGraphDocument.h>

#include <string>
#include <utility>
#include <vector>

namespace noodles::demo::examples {
namespace {

GraphProperty Number(std::string name, std::string type, double value,
                     bool scrubable = true) {
  GraphProperty property;
  property.name = std::move(name);
  property.type = std::move(type);
  property.hasValue = true;
  property.isScrubable = scrubable;
  property.numericValue = value;
  property.displayValue = std::to_string(value);
  return property;
}

GraphProperty Toggle(std::string name, bool value) {
  // UsdGraphDocument exposes scalar Bool attributes as editable, and
  // GraphEditor maps a no-move row tap to an atomic toggle. Keep the public
  // fixture on that same contract so the demos exercise shipping behavior
  // instead of presenting a visually identical but read-only Boolean row.
  GraphProperty property =
      Number(std::move(name), "bool", value ? 1.0 : 0.0, true);
  property.displayValue = value ? "true" : "false";
  return property;
}

GraphProperty Asset(std::string name, std::string value,
                    std::string displayValue) {
  GraphProperty property;
  property.name = std::move(name);
  property.type = "asset";
  property.hasValue = true;
  property.stringValue = std::move(value);
  property.displayValue = std::move(displayValue);
  return property;
}

GraphProperty Relationship(std::string name) {
  GraphProperty property;
  property.name = std::move(name);
  property.kind = GraphPropertyKind::Relationship;
  property.type = "rel";
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

GraphNode Node(std::string id, std::string name, std::string type,
               std::vector<GraphProperty> properties) {
  GraphNode node;
  node.id = std::move(id);
  node.name = std::move(name);
  node.schemaTypeName = std::move(type);
  node.properties = std::move(properties);
  return node;
}

void SetNumber(GraphNode& node, const std::string& propertyName,
               double value) {
  for (GraphProperty& property : node.properties) {
    if (property.name == propertyName) {
      property.numericValue = value;
      property.displayValue = std::to_string(value);
      return;
    }
  }
}

// The contract graph both demos ship first, framed as the everyday finishing
// comp it implements: generated film grain is graded and merged over the
// plate through an ellipse matte. Topology, ids, and values are pinned by
// the demo contract tests; only the presentation names say what it is.
GraphSnapshot BuildGrainCompSnapshot() {
  GraphSnapshot snapshot;
  snapshot.nodes.push_back(
      Node("/Demo/SourceImage", "Source Image", "Generator",
           {Asset("path", "", "Built-in Landscape"),
            Number("brightness", "float", 1.0), Number("bias", "float", 0.0),
            Toggle("enabled", true),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Noise", "Film Grain", "Generator",
           {Number("noise:frequency", "float", 2.5),
            Number("noise:amplitude", "float", 0.75), Toggle("enabled", true),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Grade", "Grain Grade", "Processor",
           {Port("input", "image", GraphPropertyDirection::Input),
            Number("gain", "float", 1.2), Number("mix", "float", 0.8),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Composite", "Grain Merge", "Processor",
           {Port("background", "image", GraphPropertyDirection::Input),
            Port("foreground", "image", GraphPropertyDirection::Input),
            Relationship("mask"), Number("opacity", "float", 1.0),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Display", "Display", "Output",
           {Port("surface", "image", GraphPropertyDirection::Input),
            Number("exposure", "float", 0.0), Toggle("visible", true)}));
  snapshot.nodes.push_back(
      Node("/Demo/Mask", "Grain Matte", "Shape",
           {Number("radius", "float", 0.65), Number("feather", "float", 0.12),
            Toggle("invert", false)}));

  // Connection edges use document-authoring orientation: the input property is
  // the source endpoint and points at the upstream output property.
  snapshot.edges.push_back(
      {"/Demo/Grade", "input", "/Demo/Noise", "output", false});
  snapshot.edges.push_back(
      {"/Demo/Composite", "background", "/Demo/SourceImage", "output", false});
  snapshot.edges.push_back(
      {"/Demo/Composite", "foreground", "/Demo/Grade", "output", false});
  snapshot.edges.push_back(
      {"/Demo/Composite", "mask", "/Demo/Mask", "", true});
  snapshot.edges.push_back(
      {"/Demo/Display", "surface", "/Demo/Composite", "output", false});
  return snapshot;
}

// Retro CRT television, the classic effect stack from shader land: color
// fringing, mixed-in broadcast static, horizontal jitter, scanlines, and a
// vignette. The static level and fringing are animated, so the frame slider
// sweeps a clean broadcast into a dying signal.
GraphSnapshot BuildCrtTvSnapshot() {
  GraphSnapshot snapshot;
  snapshot.nodes.push_back(
      Node("/Demo/SourceImage", "Source Image", "Generator",
           {Asset("path", "", "Built-in Landscape"),
            Number("brightness", "float", 1.0), Number("bias", "float", 0.0),
            Toggle("enabled", true),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Chroma", "Color Fringe", "Chroma",
           {Port("input", "image", GraphPropertyDirection::Input),
            Number("shift", "float", 0.006),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Static", "Broadcast Static", "Generator",
           {Number("noise:frequency", "float", 18.0),
            Number("noise:amplitude", "float", 1.2), Toggle("enabled", true),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Mix", "Static Mix", "Mix",
           {Port("input", "image", GraphPropertyDirection::Input),
            Port("blend", "image", GraphPropertyDirection::Input),
            Number("mix", "float", 0.08),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Jitter", "Sync Jitter", "Wave",
           {Port("input", "image", GraphPropertyDirection::Input),
            Number("amplitude", "float", 0.003),
            Number("frequency", "float", 48.0),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Scanlines", "Scanlines", "Scanlines",
           {Port("input", "image", GraphPropertyDirection::Input),
            Number("lines", "float", 200.0),
            Number("strength", "float", 0.4),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Vignette", "Tube Vignette", "Vignette",
           {Port("input", "image", GraphPropertyDirection::Input),
            Number("strength", "float", 0.55),
            Number("radius", "float", 0.32),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Display", "Display", "Output",
           {Port("surface", "image", GraphPropertyDirection::Input),
            Number("exposure", "float", 0.0), Toggle("visible", true)}));

  snapshot.edges.push_back(
      {"/Demo/Chroma", "input", "/Demo/SourceImage", "output", false});
  snapshot.edges.push_back(
      {"/Demo/Mix", "input", "/Demo/Chroma", "output", false});
  snapshot.edges.push_back(
      {"/Demo/Mix", "blend", "/Demo/Static", "output", false});
  snapshot.edges.push_back(
      {"/Demo/Jitter", "input", "/Demo/Mix", "output", false});
  snapshot.edges.push_back(
      {"/Demo/Scanlines", "input", "/Demo/Jitter", "output", false});
  snapshot.edges.push_back(
      {"/Demo/Vignette", "input", "/Demo/Scanlines", "output", false});
  snapshot.edges.push_back(
      {"/Demo/Display", "surface", "/Demo/Vignette", "output", false});
  return snapshot;
}

// Psychedelic kaleidoscope: the plate is folded into mirrored wedges,
// swirled around the center, posterized into flat pop-art bands, and given
// a cast. The fold rotation and swirl twist are animated, so the frame
// slider spins and winds the mandala — and a user-picked source image turns
// straight into their own kaleidoscope.
GraphSnapshot BuildKaleidoscopeSnapshot() {
  GraphSnapshot snapshot;
  snapshot.nodes.push_back(
      Node("/Demo/SourceImage", "Source Image", "Generator",
           {Asset("path", "", "Built-in Landscape"),
            Number("brightness", "float", 1.0), Number("bias", "float", 0.0),
            Toggle("enabled", true),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Kaleido", "Mirror Fold", "Kaleido",
           {Port("input", "image", GraphPropertyDirection::Input),
            Number("segments", "float", 6.0),
            Number("rotation", "float", 0.0),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Swirl", "Center Swirl", "Swirl",
           {Port("input", "image", GraphPropertyDirection::Input),
            Number("twist", "float", 0.9),
            Number("radius", "float", 0.85),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Posterize", "Pop Bands", "Posterize",
           {Port("input", "image", GraphPropertyDirection::Input),
            Number("levels", "float", 6.0),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Tint", "Neon Cast", "Tint",
           {Port("input", "image", GraphPropertyDirection::Input),
            Number("red", "float", 1.12), Number("green", "float", 0.92),
            Number("blue", "float", 1.1),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Display", "Display", "Output",
           {Port("surface", "image", GraphPropertyDirection::Input),
            Number("exposure", "float", 0.0), Toggle("visible", true)}));

  snapshot.edges.push_back(
      {"/Demo/Kaleido", "input", "/Demo/SourceImage", "output", false});
  snapshot.edges.push_back(
      {"/Demo/Swirl", "input", "/Demo/Kaleido", "output", false});
  snapshot.edges.push_back(
      {"/Demo/Posterize", "input", "/Demo/Swirl", "output", false});
  snapshot.edges.push_back(
      {"/Demo/Tint", "input", "/Demo/Posterize", "output", false});
  snapshot.edges.push_back(
      {"/Demo/Display", "surface", "/Demo/Tint", "output", false});
  return snapshot;
}

constexpr int kStressChains = 8;
constexpr int kStressChainLength = 12;

std::string StressNodeId(int chain, int index) {
  return "/Stress/Chain" + std::to_string(chain) + "/Op" +
         std::to_string(index);
}

// The op kind at position `index` of a stress chain (index >= 1). Cycling
// kinds keeps every exec function under load, not just the grade path.
DemoOpKind StressOpKind(int index) {
  switch (index % 4) {
    case 1: return DemoOpKind::Grade;
    case 2: return DemoOpKind::Invert;
    case 3: return DemoOpKind::Wave;
    default: return DemoOpKind::Pixelate;
  }
}

GraphSnapshot BuildStressTestSnapshot() {
  // The classic render chain keeps the output image meaningful, then eight
  // twelve-node chains of real image ops converge through a Mix collector
  // tree into the Composite's foreground: every one of the ~109 nodes is
  // reachable from Display, so each op executes per pixel and scrubbing any
  // of them visibly changes the render. No node stores a position — every
  // node is connected, so the editor's deterministic layered auto-layout
  // places the whole wall using real measured node sizes, which is exactly
  // the at-scale layout path this variant exists to probe.
  GraphSnapshot snapshot;
  snapshot.nodes.push_back(
      Node("/Demo/SourceImage", "Source Image", "Generator",
           {Asset("path", "", "Built-in Landscape"),
            Number("brightness", "float", 1.0), Number("bias", "float", 0.0),
            Toggle("enabled", true),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Noise", "Noise", "Generator",
           {Number("noise:frequency", "float", 4.5),
            Number("noise:amplitude", "float", 0.9), Toggle("enabled", true),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Grade", "Color Grade", "Processor",
           {Port("input", "image", GraphPropertyDirection::Input),
            Number("gain", "float", 1.2), Number("mix", "float", 0.8),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Mask", "Ellipse Mask", "Shape",
           {Number("radius", "float", 0.65), Number("feather", "float", 0.22),
            Toggle("invert", false)}));
  snapshot.nodes.push_back(
      Node("/Demo/Composite", "Composite", "Processor",
           {Port("background", "image", GraphPropertyDirection::Input),
            Port("foreground", "image", GraphPropertyDirection::Input),
            Relationship("mask"), Number("opacity", "float", 1.0),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Display", "Display", "Output",
           {Port("surface", "image", GraphPropertyDirection::Input),
            Number("exposure", "float", 0.0), Toggle("visible", true)}));

  snapshot.edges.push_back(
      {"/Demo/Grade", "input", "/Demo/Noise", "output", false});
  snapshot.edges.push_back(
      {"/Demo/Composite", "background", "/Demo/SourceImage", "output", false});
  // The Composite's foreground is the merged output of every stress chain, so
  // the whole wall of ops contributes to the visible ellipse.
  snapshot.edges.push_back(
      {"/Demo/Composite", "foreground", "/Stress/MergeFinal", "output",
       false});
  snapshot.edges.push_back(
      {"/Demo/Composite", "mask", "/Demo/Mask", "", true});
  snapshot.edges.push_back(
      {"/Demo/Display", "surface", "/Demo/Composite", "output", false});

  for (int chain = 0; chain < kStressChains; ++chain) {
    for (int index = 0; index < kStressChainLength; ++index) {
      const std::string id = StressNodeId(chain, index);
      if (index == 0 && chain % 2 == 0) {
        // Even chains start from a self-contained signal generator.
        snapshot.nodes.push_back(Node(
            id, "Signal " + std::to_string(chain), "Generator",
            {Number("frequency", "float", 1.0 + chain),
             Number("amplitude", "float", 0.8),
             Port("output", "image", GraphPropertyDirection::Output)}));
        continue;
      }
      const DemoOpKind kind =
          index == 0 ? DemoOpKind::Grade : StressOpKind(index);
      const std::string label = std::string(DemoOpKindTitle(kind)) + " " +
                                std::to_string(chain) + "." +
                                std::to_string(index);
      GraphNode node = MakeDemoOpNode(kind, id, label);
      // Vary the op parameters so the merged result has visible structure and
      // stacked repeats of one kind do not cancel out.
      switch (kind) {
        case DemoOpKind::Grade:
          SetNumber(node, "gain", 0.92 + 0.03 * index);
          SetNumber(node, "mix", 0.85);
          break;
        case DemoOpKind::Invert:
          SetNumber(node, "mix", 0.12);
          break;
        case DemoOpKind::Wave:
          SetNumber(node, "amplitude", 0.012);
          SetNumber(node, "frequency", 2.0 + chain * 0.5);
          break;
        case DemoOpKind::Pixelate:
          SetNumber(node, "size", 0.012 + 0.004 * chain);
          break;
        case DemoOpKind::Mix:
        case DemoOpKind::Noise:
        case DemoOpKind::Blur:
        case DemoOpKind::Threshold:
        case DemoOpKind::Tint:
        case DemoOpKind::Chroma:
        case DemoOpKind::Scanlines:
        case DemoOpKind::Vignette:
        case DemoOpKind::Kaleido:
        case DemoOpKind::Swirl:
        case DemoOpKind::Posterize:
        case DemoOpKind::Halftone:
          break;
      }
      snapshot.nodes.push_back(std::move(node));
      if (index == 0) {
        // Odd chains fan out from the shared color grade.
        snapshot.edges.push_back(
            {id, "input", "/Demo/Grade", "output", false});
      }
    }
    for (int index = 1; index < kStressChainLength; ++index) {
      snapshot.edges.push_back({StressNodeId(chain, index), "input",
                                StressNodeId(chain, index - 1), "output",
                                false});
    }
  }

  // Merge the eight chain tails pairwise into one image: four first-level
  // mixes, two second-level mixes, then the final mix the Composite consumes.
  for (int pair = 0; pair < 4; ++pair) {
    const std::string id = "/Stress/MergeA" + std::to_string(pair);
    snapshot.nodes.push_back(
        MakeDemoOpNode(DemoOpKind::Mix, id,
                       "Merge " + std::to_string(pair * 2) + "+" +
                           std::to_string(pair * 2 + 1)));
    snapshot.edges.push_back(
        {id, "input", StressNodeId(pair * 2, kStressChainLength - 1),
         "output", false});
    snapshot.edges.push_back(
        {id, "blend", StressNodeId(pair * 2 + 1, kStressChainLength - 1),
         "output", false});
  }
  for (int pair = 0; pair < 2; ++pair) {
    const std::string id = "/Stress/MergeB" + std::to_string(pair);
    snapshot.nodes.push_back(
        MakeDemoOpNode(DemoOpKind::Mix, id,
                       pair == 0 ? "Merge 0-3" : "Merge 4-7"));
    snapshot.edges.push_back(
        {id, "input", "/Stress/MergeA" + std::to_string(pair * 2), "output",
         false});
    snapshot.edges.push_back(
        {id, "blend", "/Stress/MergeA" + std::to_string(pair * 2 + 1),
         "output", false});
  }
  snapshot.nodes.push_back(
      MakeDemoOpNode(DemoOpKind::Mix, "/Stress/MergeFinal", "Merge All"));
  snapshot.edges.push_back(
      {"/Stress/MergeFinal", "input", "/Stress/MergeB0", "output", false});
  snapshot.edges.push_back(
      {"/Stress/MergeFinal", "blend", "/Stress/MergeB1", "output", false});
  return snapshot;
}

}  // namespace

const char* DemoGraphVariantTitle(DemoGraphVariant variant) {
  switch (variant) {
    case DemoGraphVariant::GrainComp:
      return "Grain Comp";
    case DemoGraphVariant::CrtTv:
      return "CRT TV";
    case DemoGraphVariant::Kaleidoscope:
      return "Kaleidoscope";
    case DemoGraphVariant::StressTest:
      return "Stress Test";
  }
  return "Grain Comp";
}

std::shared_ptr<InMemoryGraphDocument> CreateDemoGraphDocument(
    DemoGraphVariant variant) {
  GraphSnapshot snapshot;
  switch (variant) {
    case DemoGraphVariant::GrainComp:
      snapshot = BuildGrainCompSnapshot();
      break;
    case DemoGraphVariant::CrtTv:
      snapshot = BuildCrtTvSnapshot();
      break;
    case DemoGraphVariant::Kaleidoscope:
      snapshot = BuildKaleidoscopeSnapshot();
      break;
    case DemoGraphVariant::StressTest:
      snapshot = BuildStressTestSnapshot();
      break;
  }

  auto document = std::make_shared<InMemoryGraphDocument>(std::move(snapshot));
  switch (variant) {
    case DemoGraphVariant::GrainComp:
      // The grain comp's six nodes are all connected and deliberately have
      // no stored positions. Both demos therefore exercise the same
      // deterministic fresh-layout path a host sees when it first opens a
      // graph; subsequent drags persist normally. The animated gain row
      // proves that both demos author at displayFrame rather than
      // accidentally replacing its default value.
      document->setTimeSample("/Demo/Grade", "gain", 0.0, 0.8);
      document->setTimeSample("/Demo/Grade", "gain", 24.0, 1.4);
      break;
    case DemoGraphVariant::CrtTv:
      // A clean broadcast decays across the frame range: static ramps up and
      // the color fringing spreads.
      document->setTimeSample("/Demo/Mix", "mix", 0.0, 0.03);
      document->setTimeSample("/Demo/Mix", "mix", 24.0, 0.35);
      document->setTimeSample("/Demo/Chroma", "shift", 0.0, 0.002);
      document->setTimeSample("/Demo/Chroma", "shift", 24.0, 0.02);
      break;
    case DemoGraphVariant::Kaleidoscope:
      // The mandala spins and winds tighter across the frame range.
      document->setTimeSample("/Demo/Kaleido", "rotation", 0.0, 0.0);
      document->setTimeSample("/Demo/Kaleido", "rotation", 24.0, 3.1416);
      document->setTimeSample("/Demo/Swirl", "twist", 0.0, 0.3);
      document->setTimeSample("/Demo/Swirl", "twist", 24.0, 2.2);
      break;
    case DemoGraphVariant::StressTest:
      document->setTimeSample("/Demo/Grade", "gain", 0.0, 0.8);
      document->setTimeSample("/Demo/Grade", "gain", 24.0, 1.4);
      // Animate every chain's mid-chain grade op so frame scrubbing
      // re-evaluates many time-sampled attributes across the large graph and
      // visibly sweeps the merged foreground.
      for (int chain = 0; chain < kStressChains; ++chain) {
        const std::string id = StressNodeId(chain, 5);
        document->setTimeSample(id, "gain", 0.0, 0.55);
        document->setTimeSample(id, "gain", 24.0, 1.45);
      }
      break;
  }
  return document;
}

DemoGraphFixture CreateDemoGraphFixture(DemoGraphVariant variant) {
  auto document = CreateDemoGraphDocument(variant);
  auto editor = std::make_shared<GraphEditor>();
  editor->setClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  editor->setOverlayOpacity(0.5f);
  editor->setValueScrubEnabled(true);
  // Tighter than the reference fade band so peripheral noodles visibly fade
  // while panning dense graphs such as the stress wall.
  editor->setLinkFadeRange(0.6, 0.3);
  editor->setDisplayFrame(12.0);
  editor->setDocument(document);
  return {std::move(document), std::move(editor)};
}

DemoGraphFixture CreateDemoGraphFixture() {
  return CreateDemoGraphFixture(DemoGraphVariant::GrainComp);
}

GraphNode MakeDemoProcessorNode(std::string id, std::string name) {
  return Node(std::move(id), std::move(name), "Processor",
              {Port("input", "image", GraphPropertyDirection::Input),
               Number("gain", "float", 1.0),
               Port("output", "image", GraphPropertyDirection::Output)});
}

const char* DemoOpKindTitle(DemoOpKind kind) {
  switch (kind) {
    case DemoOpKind::Grade:
      return "Grade";
    case DemoOpKind::Invert:
      return "Invert";
    case DemoOpKind::Pixelate:
      return "Pixelate";
    case DemoOpKind::Wave:
      return "Wave";
    case DemoOpKind::Mix:
      return "Mix";
    case DemoOpKind::Noise:
      return "Noise";
    case DemoOpKind::Blur:
      return "Blur";
    case DemoOpKind::Threshold:
      return "Threshold";
    case DemoOpKind::Tint:
      return "Tint";
    case DemoOpKind::Chroma:
      return "Chroma";
    case DemoOpKind::Scanlines:
      return "Scanlines";
    case DemoOpKind::Vignette:
      return "Vignette";
    case DemoOpKind::Kaleido:
      return "Kaleido";
    case DemoOpKind::Swirl:
      return "Swirl";
    case DemoOpKind::Posterize:
      return "Posterize";
    case DemoOpKind::Halftone:
      return "Halftone";
  }
  return "Grade";
}

GraphNode MakeDemoOpNode(DemoOpKind kind, std::string id, std::string name) {
  switch (kind) {
    case DemoOpKind::Grade:
      return Node(std::move(id), std::move(name), "Processor",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Number("gain", "float", 1.35),
                   Number("mix", "float", 1.0),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Invert:
      return Node(std::move(id), std::move(name), "Invert",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Number("mix", "float", 1.0),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Pixelate:
      return Node(std::move(id), std::move(name), "Pixelate",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Number("size", "float", 0.04),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Wave:
      return Node(std::move(id), std::move(name), "Wave",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Number("amplitude", "float", 0.03),
                   Number("frequency", "float", 6.0),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Mix:
      return Node(std::move(id), std::move(name), "Mix",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Port("blend", "image", GraphPropertyDirection::Input),
                   Number("mix", "float", 0.5),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Noise:
      return Node(std::move(id), std::move(name), "Generator",
                  {Number("noise:frequency", "float", 3.5),
                   Number("noise:amplitude", "float", 1.0),
                   Toggle("enabled", true),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Blur:
      return Node(std::move(id), std::move(name), "Blur",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Number("radius", "float", 0.02),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Threshold:
      return Node(std::move(id), std::move(name), "Threshold",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Number("threshold", "float", 0.7),
                   Number("softness", "float", 0.1),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Tint:
      // Warm by default so a freshly wired tint visibly changes the image.
      return Node(std::move(id), std::move(name), "Tint",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Number("red", "float", 1.1), Number("green", "float", 0.95),
                   Number("blue", "float", 0.8),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Chroma:
      return Node(std::move(id), std::move(name), "Chroma",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Number("shift", "float", 0.015),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Scanlines:
      return Node(std::move(id), std::move(name), "Scanlines",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Number("lines", "float", 180.0),
                   Number("strength", "float", 0.35),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Vignette:
      return Node(std::move(id), std::move(name), "Vignette",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Number("strength", "float", 0.5),
                   Number("radius", "float", 0.35),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Kaleido:
      return Node(std::move(id), std::move(name), "Kaleido",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Number("segments", "float", 6.0),
                   Number("rotation", "float", 0.0),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Swirl:
      return Node(std::move(id), std::move(name), "Swirl",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Number("twist", "float", 1.2),
                   Number("radius", "float", 0.7),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Posterize:
      return Node(std::move(id), std::move(name), "Posterize",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Number("levels", "float", 5.0),
                   Port("output", "image", GraphPropertyDirection::Output)});
    case DemoOpKind::Halftone:
      return Node(std::move(id), std::move(name), "Halftone",
                  {Port("input", "image", GraphPropertyDirection::Input),
                   Number("cells", "float", 60.0),
                   Number("scale", "float", 1.0),
                   Port("output", "image", GraphPropertyDirection::Output)});
  }
  return MakeDemoProcessorNode(std::move(id), std::move(name));
}

}  // namespace noodles::demo::examples
