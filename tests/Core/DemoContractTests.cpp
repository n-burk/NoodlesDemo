#include "DemoGraphFixture.h"
#include "DemoImageProcessor.h"

#include <noodles/apple/GraphDocument.h>
#include <noodles/apple/GraphEditor.h>
#include <noodles/apple/InMemoryGraphDocument.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace na = noodles::apple;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                   #condition);                                                \
      return false;                                                            \
    }                                                                          \
  } while (false)

namespace {

const na::GraphPinInfo* FindPin(const std::vector<na::GraphPinInfo>& pins,
                                const std::string& name,
                                bool isOutput = false) {
  for (const na::GraphPinInfo& pin : pins) {
    if (pin.name == name && pin.isOutput == isOutput) return &pin;
  }
  return nullptr;
}

const na::GraphProperty* FindProperty(const na::GraphSnapshot& snapshot,
                                      const std::string& nodeId,
                                      const std::string& propertyName) {
  for (const na::GraphNode& node : snapshot.nodes) {
    if (node.id != nodeId) continue;
    for (const na::GraphProperty& property : node.properties) {
      if (property.name == propertyName) return &property;
    }
  }
  return nullptr;
}

const na::GraphNode* FindNode(const na::GraphSnapshot& snapshot,
                              const std::string& nodeId) {
  for (const na::GraphNode& node : snapshot.nodes) {
    if (node.id == nodeId) return &node;
  }
  return nullptr;
}

na::GraphProperty* FindProperty(na::GraphSnapshot& snapshot,
                                const std::string& nodeId,
                                const std::string& propertyName) {
  for (na::GraphNode& node : snapshot.nodes) {
    if (node.id != nodeId) continue;
    for (na::GraphProperty& property : node.properties) {
      if (property.name == propertyName) return &property;
    }
  }
  return nullptr;
}

std::uint64_t RenderHash(
    const na::GraphSnapshot& snapshot,
    const na::examples::DemoRgbaImage* sourceImage = nullptr) {
  return na::examples::DemoImageChecksum(
      na::examples::RenderDemoImage(snapshot, 128, 80, sourceImage));
}

bool PropertyChangeAltersOutput(const na::GraphSnapshot& baseline,
                                const std::string& nodeId,
                                const std::string& propertyName,
                                double value,
                                std::uint64_t baselineHash) {
  na::GraphSnapshot changed = baseline;
  na::GraphProperty* property =
      FindProperty(changed, nodeId, propertyName);
  if (!property) return false;
  property->hasValue = true;
  property->numericValue = value;
  return RenderHash(changed) != baselineHash;
}

bool HasEdge(const na::GraphSnapshot& snapshot,
             const std::string& inputNodeId,
             const std::string& inputPort,
             const std::string& outputNodeId,
             const std::string& outputPort) {
  for (const na::GraphEdge& edge : snapshot.edges) {
    if (!edge.isRelationship && edge.sourceNodeId == inputNodeId &&
        edge.sourcePort == inputPort && edge.targetNodeId == outputNodeId &&
        edge.targetPort == outputPort) {
      return true;
    }
  }
  return false;
}

bool HasRelationship(const na::GraphSnapshot& snapshot,
                     const std::string& sourceNodeId,
                     const std::string& relationshipName,
                     const std::string& targetNodeId) {
  for (const na::GraphEdge& edge : snapshot.edges) {
    if (edge.isRelationship && edge.sourceNodeId == sourceNodeId &&
        edge.sourcePort == relationshipName &&
        edge.targetNodeId == targetNodeId && edge.targetPort.empty()) {
      return true;
    }
  }
  return false;
}

bool RemoveEdge(na::GraphSnapshot* snapshot,
                const std::string& inputNodeId,
                const std::string& inputPort,
                const std::string& outputNodeId,
                const std::string& outputPort) {
  if (!snapshot) return false;
  for (auto edge = snapshot->edges.begin(); edge != snapshot->edges.end();
       ++edge) {
    if (!edge->isRelationship && edge->sourceNodeId == inputNodeId &&
        edge->sourcePort == inputPort && edge->targetNodeId == outputNodeId &&
        edge->targetPort == outputPort) {
      snapshot->edges.erase(edge);
      return true;
    }
  }
  return false;
}

bool RemoveRelationshipEdge(na::GraphSnapshot* snapshot,
                            const std::string& sourceNodeId,
                            const std::string& relationshipName,
                            const std::string& targetNodeId) {
  if (!snapshot) return false;
  for (auto edge = snapshot->edges.begin(); edge != snapshot->edges.end();
       ++edge) {
    if (edge->isRelationship && edge->sourceNodeId == sourceNodeId &&
        edge->sourcePort == relationshipName &&
        edge->targetNodeId == targetNodeId && edge->targetPort.empty()) {
      snapshot->edges.erase(edge);
      return true;
    }
  }
  return false;
}

bool HasTypedPort(const na::GraphSnapshot& snapshot,
                  const std::string& nodeId,
                  const std::string& propertyName,
                  const std::string& type,
                  na::GraphPropertyDirection direction) {
  const na::GraphProperty* property =
      FindProperty(snapshot, nodeId, propertyName);
  return property && property->kind == na::GraphPropertyKind::Attribute &&
         property->type == type && property->direction == direction;
}

bool TestFixtureTopologyAndEditableControls() {
  auto fixture = na::examples::CreateDemoGraphFixture();
  CHECK(fixture.document);
  CHECK(fixture.editor);
  CHECK(fixture.editor->nodeCount() == 6);
  CHECK(fixture.editor->linkCount() == 5);

  const na::GraphSnapshot initial = fixture.document->snapshot(12.0);
  CHECK(initial.nodes.size() == 6);
  CHECK(initial.edges.size() == 5);
  CHECK(FindProperty(initial, "/Demo/Noise", "noise:frequency"));
  CHECK(FindProperty(initial, "/Demo/Noise", "noise:amplitude"));
  CHECK(!FindProperty(initial, "/Demo/Noise", "input"));
  const na::GraphNode* noiseNode = FindNode(initial, "/Demo/Noise");
  CHECK(noiseNode);
  CHECK(noiseNode->schemaTypeName == "Generator");

  const na::GraphProperty* sourcePath =
      FindProperty(initial, "/Demo/SourceImage", "path");
  CHECK(sourcePath);
  CHECK(sourcePath->kind == na::GraphPropertyKind::Attribute);
  CHECK(sourcePath->type == "asset");
  CHECK(sourcePath->hasValue);
  CHECK(!sourcePath->isScrubable);
  CHECK(sourcePath->stringValue.empty());
  CHECK(sourcePath->displayValue == "Built-in Landscape");

  const auto sourcePins = fixture.editor->nodePins("/Demo/SourceImage");
  const na::GraphPinInfo* pathPin = FindPin(sourcePins, "path");
  CHECK(pathPin);
  CHECK(pathPin->hasValue);
  CHECK(!pathPin->isScrubable);
  CHECK(pathPin->typeText.find("asset") != std::string::npos);
  CHECK(pathPin->typeText.find("Built-in Landscape") != std::string::npos);

  // Every image-producing stage exposes a typed, output-only row. Composite's
  // mask remains a whole-node relationship to the Ellipse Mask shape.
  const struct {
    const char* nodeId;
    const char* propertyName;
    const char* type;
    na::GraphPropertyDirection direction;
  } typedPorts[] = {
      {"/Demo/SourceImage", "output", "image",
       na::GraphPropertyDirection::Output},
      {"/Demo/Noise", "output", "image",
       na::GraphPropertyDirection::Output},
      {"/Demo/Grade", "input", "image",
       na::GraphPropertyDirection::Input},
      {"/Demo/Grade", "output", "image",
       na::GraphPropertyDirection::Output},
      {"/Demo/Composite", "background", "image",
       na::GraphPropertyDirection::Input},
      {"/Demo/Composite", "foreground", "image",
       na::GraphPropertyDirection::Input},
      {"/Demo/Composite", "output", "image",
       na::GraphPropertyDirection::Output},
      {"/Demo/Display", "surface", "image",
       na::GraphPropertyDirection::Input},
  };
  for (const auto& port : typedPorts) {
    CHECK(HasTypedPort(initial, port.nodeId, port.propertyName, port.type,
                       port.direction));
  }

  const na::GraphProperty* maskRelationship =
      FindProperty(initial, "/Demo/Composite", "mask");
  CHECK(maskRelationship);
  CHECK(maskRelationship->kind == na::GraphPropertyKind::Relationship);
  CHECK(maskRelationship->type == "rel");
  CHECK(HasRelationship(initial, "/Demo/Composite", "mask", "/Demo/Mask"));

  const struct {
    const char* inputNodeId;
    const char* inputPort;
    const char* outputNodeId;
    const char* outputPort;
  } expectedEdges[] = {
      {"/Demo/Grade", "input", "/Demo/Noise", "output"},
      {"/Demo/Composite", "background", "/Demo/SourceImage", "output"},
      {"/Demo/Composite", "foreground", "/Demo/Grade", "output"},
      {"/Demo/Display", "surface", "/Demo/Composite", "output"},
  };
  for (const auto& edge : expectedEdges) {
    CHECK(HasEdge(initial, edge.inputNodeId, edge.inputPort,
                  edge.outputNodeId, edge.outputPort));
  }

  const auto noisePins = fixture.editor->nodePins("/Demo/Noise");
  const na::GraphPinInfo* output = FindPin(noisePins, "output", true);
  CHECK(output);
  CHECK(output->typeText == "image");
  CHECK(!FindPin(noisePins, "output", false));

  int dataLinks = 0;
  int relationshipLinks = 0;
  for (const na::GraphLinkInfo& link : fixture.editor->links()) {
    if (link.isRelationship) {
      CHECK(link.sourceNodeId == "/Demo/Composite");
      CHECK(link.sourcePort == "mask");
      CHECK(link.targetNodeId == "/Demo/Mask");
      CHECK(link.targetPort.empty());
      ++relationshipLinks;
    } else {
      CHECK(link.sourcePort == "output");
      ++dataLinks;
    }
  }
  CHECK(dataLinks == 4);
  CHECK(relationshipLinks == 1);

  const na::GraphProperty* radius =
      FindProperty(initial, "/Demo/Mask", "radius");
  CHECK(radius);
  CHECK(radius->type == "float");
  CHECK(radius->hasValue);
  CHECK(radius->isScrubable);

  const auto pins = fixture.editor->nodePins("/Demo/Display");
  const na::GraphPinInfo* visible = FindPin(pins, "visible");
  CHECK(visible);
  CHECK(visible->hasValue);
  CHECK(visible->isScrubable);
  CHECK(visible->value == 1.0);

  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
  CHECK(fixture.editor->nodePosition("/Demo/Display", &x, &y));
  CHECK(fixture.editor->nodeSize("/Demo/Display", &width, &height));
  (void)height;

  // A stationary tap in the value band is the shipping Boolean-toggle
  // gesture. Use the actual fixture/editor pair linked into both demo apps.
  const double rowX = x + width * 0.5;
  fixture.editor->pointerDown(rowX, visible->centerY);
  fixture.editor->pointerUp(rowX, visible->centerY);

  const na::GraphSnapshot authoredSnapshot = fixture.document->snapshot(12.0);
  const na::GraphProperty* authored =
      FindProperty(authoredSnapshot, "/Demo/Display", "visible");
  CHECK(authored);
  CHECK(authored->numericValue == 0.0);
  CHECK(authored->displayValue == "off");
  return true;
}

bool TestFixtureAssetPathRoundTripsLosslessly() {
  auto fixture = na::examples::CreateDemoGraphFixture();
  const std::string exactPath =
      "/Volumes/Demo Library/plates/source image with spaces and #symbols "
      "0001.exr";

  CHECK(fixture.document->setStringAttributeValue(
      "/Demo/SourceImage", "path", exactPath, 12.0));
  const na::GraphSnapshot authored = fixture.document->snapshot(12.0);
  const na::GraphProperty* path =
      FindProperty(authored, "/Demo/SourceImage", "path");
  CHECK(path);
  CHECK(path->type == "asset");
  CHECK(path->hasValue);
  CHECK(!path->isScrubable);
  CHECK(path->stringValue == exactPath);
  CHECK(!path->displayValue.empty());
  CHECK(path->displayValue != exactPath);

  // Re-authoring the same exact value is a no-op; clearing it is also exact
  // even though the row presents a friendly placeholder.
  CHECK(!fixture.document->setStringAttributeValue(
      "/Demo/SourceImage", "path", exactPath, 12.0));
  CHECK(fixture.document->setStringAttributeValue(
      "/Demo/SourceImage", "path", "", 12.0));
  const na::GraphSnapshot cleared = fixture.document->snapshot(12.0);
  path = FindProperty(cleared, "/Demo/SourceImage", "path");
  CHECK(path);
  CHECK(path->stringValue.empty());
  CHECK(path->displayValue == "(none)");
  return true;
}

bool TestFixtureRadiusScrubChangesOutput() {
  auto fixture = na::examples::CreateDemoGraphFixture();
  const na::GraphSnapshot baseline = fixture.document->snapshot(12.0);
  const std::uint64_t baselineHash = RenderHash(baseline);
  const na::GraphProperty* before =
      FindProperty(baseline, "/Demo/Mask", "radius");
  CHECK(before);

  const auto pins = fixture.editor->nodePins("/Demo/Mask");
  const na::GraphPinInfo* radius = FindPin(pins, "radius");
  CHECK(radius);
  CHECK(radius->isScrubable);
  CHECK(radius->typeText.find("float") != std::string::npos);

  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
  CHECK(fixture.editor->nodePosition("/Demo/Mask", &x, &y));
  CHECK(fixture.editor->nodeSize("/Demo/Mask", &width, &height));
  (void)y;
  (void)height;

  const double rowX = x + width * 0.5;
  fixture.editor->pointerDown(rowX, radius->centerY);
  fixture.editor->pointerMove(rowX + 60.0, radius->centerY);
  fixture.editor->pointerUp(rowX + 60.0, radius->centerY);

  const na::GraphSnapshot edited = fixture.document->snapshot(12.0);
  const na::GraphProperty* after =
      FindProperty(edited, "/Demo/Mask", "radius");
  CHECK(after);
  CHECK(after->numericValue > before->numericValue);
  CHECK(RenderHash(edited) != baselineHash);
  return true;
}

bool TestFixtureAnimatedScrubAuthorsAtDisplayFrame() {
  auto fixture = na::examples::CreateDemoGraphFixture();
  const auto pins = fixture.editor->nodePins("/Demo/Grade");
  const na::GraphPinInfo* gain = FindPin(pins, "gain");
  CHECK(gain);
  CHECK(gain->isScrubable);
  CHECK(std::abs(gain->value - 1.1) < 1e-5);

  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
  CHECK(fixture.editor->nodePosition("/Demo/Grade", &x, &y));
  CHECK(fixture.editor->nodeSize("/Demo/Grade", &width, &height));
  (void)y;
  (void)height;

  const double rowX = x + width * 0.5;
  fixture.editor->pointerDown(rowX, gain->centerY);
  fixture.editor->pointerMove(rowX + 60.0, gain->centerY);
  fixture.editor->pointerUp(rowX + 60.0, gain->centerY);

  const std::vector<double> samples =
      fixture.document->timeSamples("/Demo/Grade", "gain");
  CHECK(samples.size() == 3);
  CHECK(samples[0] == 0.0);
  CHECK(samples[1] == 12.0);
  CHECK(samples[2] == 24.0);

  const na::GraphSnapshot startSnapshot = fixture.document->snapshot(0.0);
  const na::GraphSnapshot editSnapshot = fixture.document->snapshot(12.0);
  const na::GraphSnapshot endSnapshot = fixture.document->snapshot(24.0);
  const na::GraphProperty* atStart =
      FindProperty(startSnapshot, "/Demo/Grade", "gain");
  const na::GraphProperty* atEdit =
      FindProperty(editSnapshot, "/Demo/Grade", "gain");
  const na::GraphProperty* atEnd =
      FindProperty(endSnapshot, "/Demo/Grade", "gain");
  CHECK(atStart && atEdit && atEnd);
  CHECK(std::abs(atStart->numericValue - 0.8) < 1e-5);
  CHECK(atEdit->numericValue > 0.8);
  CHECK(std::abs(atEnd->numericValue - 1.4) < 1e-5);
  return true;
}

bool TestDemoImageProcessorTracksGraphState() {
  auto fixture = na::examples::CreateDemoGraphFixture();
  const na::GraphSnapshot baseline = fixture.document->snapshot(12.0);
  const na::examples::DemoRgbaImage first =
      na::examples::RenderDemoImage(baseline, 128, 80);
  const na::examples::DemoRgbaImage second =
      na::examples::RenderDemoImage(baseline, 128, 80);
  CHECK(!first.empty());
  CHECK(first.width == 128);
  CHECK(first.height == 80);
  CHECK(first.pixels.size() == 128U * 80U * 4U);
  CHECK(first.pixels == second.pixels);
  for (std::size_t index = 3; index < first.pixels.size(); index += 4) {
    CHECK(first.pixels[index] == 255);
  }

  const std::uint64_t baselineHash =
      na::examples::DemoImageChecksum(first);
  CHECK(baselineHash == RenderHash(baseline));

  // The fixture's animated gain is linearly evaluated by GraphDocument. The
  // processor consumes that evaluated snapshot, so frame scrubbing produces
  // real intermediate imagery rather than only updating graph text.
  const std::uint64_t startHash =
      RenderHash(fixture.document->snapshot(0.0));
  const std::uint64_t endHash =
      RenderHash(fixture.document->snapshot(24.0));
  CHECK(startHash != baselineHash);
  CHECK(endHash != baselineHash);
  CHECK(startHash != endHash);

  // Exercise the same authoring paths used by the native callbacks, not only
  // synthetic snapshots: scalar authoring, an atomic Boolean edit, and a
  // topology mutation must each change the next rendered snapshot.
  auto valueFixture = na::examples::CreateDemoGraphFixture();
  CHECK(valueFixture.document->setAttributeValue(
      "/Demo/Noise", "noise:frequency", 6.75, 12.0));
  CHECK(RenderHash(valueFixture.document->snapshot(12.0)) != baselineHash);

  auto toggleFixture = na::examples::CreateDemoGraphFixture();
  CHECK(toggleFixture.document->setAttributeValue(
      "/Demo/Noise", "enabled", 0.0, 12.0));
  CHECK(RenderHash(toggleFixture.document->snapshot(12.0)) != baselineHash);

  auto radiusFixture = na::examples::CreateDemoGraphFixture();
  CHECK(radiusFixture.document->setAttributeValue(
      "/Demo/Mask", "radius", 0.31, 12.0));
  CHECK(RenderHash(radiusFixture.document->snapshot(12.0)) != baselineHash);

  // Multiple authored sources on one input are ordered. The newest connection
  // controls Display.surface, so connecting Noise.output directly must produce
  // exactly the same image as a graph whose only display source is Noise.
  auto lastAuthoredFixture = na::examples::CreateDemoGraphFixture();
  CHECK(lastAuthoredFixture.document->authorConnection(
      "/Demo/Display", "surface", "/Demo/Noise", "output"));
  const std::uint64_t lastAuthoredHash =
      RenderHash(lastAuthoredFixture.document->snapshot(12.0));
  CHECK(lastAuthoredHash != baselineHash);

  auto directNoiseFixture = na::examples::CreateDemoGraphFixture();
  CHECK(directNoiseFixture.document->removeConnection(
      "/Demo/Display", "surface", "/Demo/Composite", "output"));
  CHECK(directNoiseFixture.document->authorConnection(
      "/Demo/Display", "surface", "/Demo/Noise", "output"));
  CHECK(RenderHash(directNoiseFixture.document->snapshot(12.0)) ==
        lastAuthoredHash);

  // Every processor control called out by the fixture contract participates
  // in the output. Radius uses the real document-authoring path above; these
  // snapshot mutations keep independent coverage for the remaining controls.
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/SourceImage",
                                   "brightness", 0.55, baselineHash));
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/SourceImage", "bias",
                                   0.18, baselineHash));
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/SourceImage", "enabled",
                                   0.0, baselineHash));
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/Noise",
                                   "noise:frequency", 6.75, baselineHash));
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/Noise",
                                   "noise:amplitude", 0.17, baselineHash));
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/Noise", "enabled", 0.0,
                                   baselineHash));
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/Grade", "gain", 1.85,
                                   baselineHash));
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/Grade", "mix", 0.18,
                                   baselineHash));
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/Mask", "feather", 0.43,
                                   baselineHash));
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/Mask", "invert", 1.0,
                                   baselineHash));
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/Composite", "opacity",
                                   0.25, baselineHash));
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/Display", "exposure",
                                   0.75, baselineHash));
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/Display", "visible", 0.0,
                                   baselineHash));

  // Topology is semantic too: each connection selects a processor stage.
  const struct {
    const char* sourceNode;
    const char* sourcePort;
    const char* targetNode;
    const char* targetPort;
    bool relationship;
  } edgeCases[] = {
      {"/Demo/Grade", "input", "/Demo/Noise", "output", false},
      {"/Demo/Composite", "background", "/Demo/SourceImage", "output",
       false},
      {"/Demo/Composite", "foreground", "/Demo/Grade", "output", false},
      {"/Demo/Composite", "mask", "/Demo/Mask", "", true},
      {"/Demo/Display", "surface", "/Demo/Composite", "output", false},
  };
  for (const auto& edgeCase : edgeCases) {
    na::GraphSnapshot disconnected = baseline;
    bool removed = false;
    for (auto edge = disconnected.edges.begin();
         edge != disconnected.edges.end(); ++edge) {
      if (edge->sourceNodeId == edgeCase.sourceNode &&
          edge->sourcePort == edgeCase.sourcePort &&
          edge->targetNodeId == edgeCase.targetNode &&
          edge->targetPort == edgeCase.targetPort &&
          edge->isRelationship == edgeCase.relationship) {
        disconnected.edges.erase(edge);
        removed = true;
        break;
      }
    }
    CHECK(removed);
    CHECK(RenderHash(disconnected) != baselineHash);
  }
  return true;
}

bool TestCompositeFallbackSemantics() {
  auto fixture = na::examples::CreateDemoGraphFixture();
  const na::GraphSnapshot baseline = fixture.document->snapshot(12.0);

  na::GraphSnapshot directSource = baseline;
  directSource.edges.push_back(
      {"/Demo/Display", "surface", "/Demo/SourceImage", "output", false});
  na::GraphSnapshot directGrade = baseline;
  directGrade.edges.push_back(
      {"/Demo/Display", "surface", "/Demo/Grade", "output", false});

  na::GraphSnapshot backgroundOnly = baseline;
  CHECK(RemoveEdge(&backgroundOnly, "/Demo/Composite", "foreground",
                   "/Demo/Grade", "output"));

  // Compare against the exact Source Image output, not merely against a
  // non-black checksum. A disconnected foreground must not invalidate or
  // darken a valid Composite background.
  CHECK(RenderHash(backgroundOnly) == RenderHash(directSource));
  CHECK(RenderHash(backgroundOnly) != RenderHash(baseline));

  na::GraphSnapshot foregroundOnly = baseline;
  CHECK(RemoveEdge(&foregroundOnly, "/Demo/Composite", "background",
                   "/Demo/SourceImage", "output"));
  CHECK(RenderHash(foregroundOnly) == RenderHash(directGrade));

  // A missing mask behaves as full white when both image inputs exist.
  na::GraphSnapshot unmasked = baseline;
  CHECK(RemoveRelationshipEdge(&unmasked, "/Demo/Composite", "mask",
                               "/Demo/Mask"));
  CHECK(RenderHash(unmasked) == RenderHash(directGrade));
  na::GraphProperty* opacity =
      FindProperty(unmasked, "/Demo/Composite", "opacity");
  CHECK(opacity);
  opacity->numericValue = 0.0;
  CHECK(RenderHash(unmasked) == RenderHash(directSource));

  // With neither image input, Composite is invalid and Display uses the same
  // deterministic no-signal result as a disconnected surface.
  na::GraphSnapshot noCompositeImages = baseline;
  CHECK(RemoveEdge(&noCompositeImages, "/Demo/Composite", "background",
                   "/Demo/SourceImage", "output"));
  CHECK(RemoveEdge(&noCompositeImages, "/Demo/Composite", "foreground",
                   "/Demo/Grade", "output"));
  na::GraphSnapshot noDisplayInput = baseline;
  CHECK(RemoveEdge(&noDisplayInput, "/Demo/Display", "surface",
                   "/Demo/Composite", "output"));
  CHECK(RenderHash(noCompositeImages) == RenderHash(noDisplayInput));
  return true;
}

bool TestExternalSourceImageChangesOutput() {
  auto fixture = na::examples::CreateDemoGraphFixture();
  const na::GraphSnapshot snapshot = fixture.document->snapshot(12.0);
  const std::uint64_t builtInHash = RenderHash(snapshot);

  na::examples::DemoRgbaImage external;
  external.width = 3;
  external.height = 2;
  external.pixels = {
      255, 24,  32,  255, 250, 220, 40,  255, 20,  230, 220, 255,
      30,  50,  240, 255, 220, 40,  210, 255, 30,  210, 55,  255,
  };

  const na::examples::DemoRgbaImage first =
      na::examples::RenderDemoImage(snapshot, 128, 80, &external);
  const na::examples::DemoRgbaImage second =
      na::examples::RenderDemoImage(snapshot, 128, 80, &external);
  CHECK(!first.empty());
  CHECK(first.pixels == second.pixels);
  CHECK(na::examples::DemoImageChecksum(first) ==
        RenderHash(snapshot, &external));
  CHECK(na::examples::DemoImageChecksum(first) != builtInHash);
  return true;
}

bool TestMissingInputAndCycleAreDeterministic() {
  auto fixture = na::examples::CreateDemoGraphFixture();
  const na::GraphSnapshot baseline = fixture.document->snapshot(12.0);
  const std::uint64_t baselineHash = RenderHash(baseline);

  // With Display disconnected there is no image root, so the evaluator emits
  // the deterministic no-signal image.
  na::GraphSnapshot missingInput = baseline;
  CHECK(RemoveEdge(&missingInput, "/Demo/Display", "surface",
                   "/Demo/Composite", "output"));
  const na::examples::DemoRgbaImage missingFirst =
      na::examples::RenderDemoImage(missingInput, 128, 80);
  const na::examples::DemoRgbaImage missingSecond =
      na::examples::RenderDemoImage(missingInput, 128, 80);
  CHECK(!missingFirst.empty());
  CHECK(missingFirst.pixels == missingSecond.pixels);
  CHECK(na::examples::DemoImageChecksum(missingFirst) != baselineHash);

  // The most recently authored source wins. Feeding Composite.output back into
  // Grade.input creates Grade -> Composite -> Grade. The cycle must terminate
  // safely while Composite preserves its independent Source Image background.
  na::GraphSnapshot cyclic = baseline;
  na::GraphEdge cycleEdge;
  cycleEdge.sourceNodeId = "/Demo/Grade";
  cycleEdge.sourcePort = "input";
  cycleEdge.targetNodeId = "/Demo/Composite";
  cycleEdge.targetPort = "output";
  cyclic.edges.push_back(cycleEdge);
  const na::examples::DemoRgbaImage cycleFirst =
      na::examples::RenderDemoImage(cyclic, 128, 80);
  const na::examples::DemoRgbaImage cycleSecond =
      na::examples::RenderDemoImage(cyclic, 128, 80);
  CHECK(!cycleFirst.empty());
  CHECK(cycleFirst.pixels == cycleSecond.pixels);
  CHECK(na::examples::DemoImageChecksum(cycleFirst) != baselineHash);
  na::GraphSnapshot backgroundOnly = baseline;
  CHECK(RemoveEdge(&backgroundOnly, "/Demo/Composite", "foreground",
                   "/Demo/Grade", "output"));
  CHECK(cycleFirst.pixels ==
        na::examples::RenderDemoImage(backgroundOnly, 128, 80).pixels);
  return true;
}

bool TestDirectDisplayIgnoresDisconnectedDownstreamControls() {
  struct ControlChange {
    const char* nodeId;
    const char* propertyName;
    double value;
  };

  // With Noise connected directly to Display, Grade, Mask, and Composite are
  // disconnected consumers and cannot influence the image.
  auto noiseFixture = na::examples::CreateDemoGraphFixture();
  CHECK(noiseFixture.document->removeConnection(
      "/Demo/Display", "surface", "/Demo/Composite", "output"));
  CHECK(noiseFixture.document->authorConnection(
      "/Demo/Display", "surface", "/Demo/Noise", "output"));
  const std::uint64_t directNoiseHash =
      RenderHash(noiseFixture.document->snapshot(12.0));
  const ControlChange noiseDownstream[] = {
      {"/Demo/SourceImage", "brightness", 0.4},
      {"/Demo/SourceImage", "bias", 0.3},
      {"/Demo/SourceImage", "enabled", 0.0},
      {"/Demo/Grade", "gain", 2.4},
      {"/Demo/Grade", "mix", 0.05},
      {"/Demo/Mask", "radius", 0.18},
      {"/Demo/Mask", "feather", 0.52},
      {"/Demo/Mask", "invert", 1.0},
      {"/Demo/Composite", "opacity", 0.08},
  };
  for (const ControlChange& change : noiseDownstream) {
    CHECK(noiseFixture.document->setAttributeValue(
        change.nodeId, change.propertyName, change.value, 12.0));
    CHECK(RenderHash(noiseFixture.document->snapshot(12.0)) ==
          directNoiseHash);
  }

  // With Source Image connected directly, Noise and every later processing
  // stage are likewise irrelevant to the visible result.
  auto sourceFixture = na::examples::CreateDemoGraphFixture();
  CHECK(sourceFixture.document->removeConnection(
      "/Demo/Display", "surface", "/Demo/Composite", "output"));
  CHECK(sourceFixture.document->authorConnection(
      "/Demo/Display", "surface", "/Demo/SourceImage", "output"));
  const std::uint64_t directSourceHash =
      RenderHash(sourceFixture.document->snapshot(12.0));
  const ControlChange sourceDownstream[] = {
      {"/Demo/Noise", "noise:frequency", 9.25},
      {"/Demo/Noise", "noise:amplitude", 0.05},
      {"/Demo/Noise", "enabled", 0.0},
      {"/Demo/Grade", "gain", 2.6},
      {"/Demo/Grade", "mix", 0.12},
      {"/Demo/Mask", "radius", 0.22},
      {"/Demo/Mask", "invert", 1.0},
      {"/Demo/Composite", "opacity", 0.15},
  };
  for (const ControlChange& change : sourceDownstream) {
    CHECK(sourceFixture.document->setAttributeValue(
        change.nodeId, change.propertyName, change.value, 12.0));
    CHECK(RenderHash(sourceFixture.document->snapshot(12.0)) ==
          directSourceHash);
  }
  return true;
}

}  // namespace

int main() {
  struct TestCase {
    const char* name;
    bool (*run)();
  };
  const TestCase tests[] = {
      {"fixture_topology_and_editable_controls",
       TestFixtureTopologyAndEditableControls},
      {"fixture_asset_path_round_trip",
       TestFixtureAssetPathRoundTripsLosslessly},
      {"fixture_radius_scrub_changes_output",
       TestFixtureRadiusScrubChangesOutput},
      {"fixture_animated_scrub_at_display_frame",
       TestFixtureAnimatedScrubAuthorsAtDisplayFrame},
      {"demo_image_processor_tracks_graph_state",
       TestDemoImageProcessorTracksGraphState},
      {"composite_fallback_semantics", TestCompositeFallbackSemantics},
      {"external_source_image_changes_output",
       TestExternalSourceImageChangesOutput},
      {"missing_input_and_cycle_are_deterministic",
       TestMissingInputAndCycleAreDeterministic},
      {"direct_display_ignores_downstream_controls",
       TestDirectDisplayIgnoresDisconnectedDownstreamControls},
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
