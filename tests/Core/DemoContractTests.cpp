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

std::uint64_t RenderHash(const na::GraphSnapshot& snapshot) {
  return na::examples::DemoImageChecksum(
      na::examples::RenderDemoImage(snapshot, 128, 80));
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

bool TestFixtureTopologyAndEditableBoolean() {
  auto fixture = na::examples::CreateDemoGraphFixture();
  CHECK(fixture.document);
  CHECK(fixture.editor);
  CHECK(fixture.editor->nodeCount() == 4);
  CHECK(fixture.editor->linkCount() == 3);
  CHECK(fixture.document->containsNode("/Demo/Source"));

  const na::GraphSnapshot initial = fixture.document->snapshot(12.0);
  CHECK(FindProperty(initial, "/Demo/Noise", "noise:frequency"));
  CHECK(FindProperty(initial, "/Demo/Noise", "noise:amplitude"));
  const na::GraphProperty* noiseOutput =
      FindProperty(initial, "/Demo/Noise", "output");
  CHECK(noiseOutput);
  CHECK(noiseOutput->type == "image");
  CHECK(noiseOutput->direction == na::GraphPropertyDirection::Output);
  CHECK(!FindProperty(initial, "/Demo/Source", "source:level"));

  const auto noisePins = fixture.editor->nodePins("/Demo/Noise");
  const na::GraphPinInfo* output = FindPin(noisePins, "output", true);
  CHECK(output);
  CHECK(output->typeText == "image");
  CHECK(!FindPin(noisePins, "output", false));

  int imageDataLinks = 0;
  for (const na::GraphLinkInfo& link : fixture.editor->links()) {
    if (link.isRelationship) continue;
    CHECK(link.sourcePort == "output");
    ++imageDataLinks;
  }
  CHECK(imageDataLinks == 2);

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

bool TestFixtureHiddenSourceUsesPublicAddNodeSeam() {
  auto fixture = na::examples::CreateDemoGraphFixture();
  CHECK(fixture.editor->nodeCount() == 4);
  CHECK(fixture.editor->addNodeAt("/Demo/Source", 320.0, 240.0));
  CHECK(fixture.editor->nodeCount() == 5);

  const na::GraphSnapshot visible = fixture.document->snapshot(12.0);
  CHECK(FindProperty(visible, "/Demo/Source", "source:level"));
  const na::GraphProperty* sourceOutput =
      FindProperty(visible, "/Demo/Source", "output");
  CHECK(sourceOutput);
  CHECK(sourceOutput->type == "image");
  CHECK(sourceOutput->direction == na::GraphPropertyDirection::Output);
  double x = 0.0;
  double y = 0.0;
  CHECK(fixture.editor->nodePosition("/Demo/Source", &x, &y));
  CHECK(std::isfinite(x) && std::isfinite(y));
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

  auto topologyFixture = na::examples::CreateDemoGraphFixture();
  CHECK(topologyFixture.document->removeConnection(
      "/Demo/Display", "surface", "/Demo/Grade", "output"));
  CHECK(RenderHash(topologyFixture.document->snapshot(12.0)) != baselineHash);

  // Every processor control called out by the fixture contract participates
  // in the output. Mutate snapshots directly here so even the display-only
  // ellipse radius remains covered.
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
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/Mask", "radius", 0.31,
                                   baselineHash));
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/Mask", "feather", 0.43,
                                   baselineHash));
  CHECK(PropertyChangeAltersOutput(baseline, "/Demo/Mask", "invert", 1.0,
                                   baselineHash));
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
      {"/Demo/Display", "surface", "/Demo/Grade", "output", false},
      {"/Demo/Grade", "mask", "/Demo/Mask", "", true},
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

}  // namespace

int main() {
  struct TestCase {
    const char* name;
    bool (*run)();
  };
  const TestCase tests[] = {
      {"fixture_topology_and_editable_boolean",
       TestFixtureTopologyAndEditableBoolean},
      {"fixture_hidden_source_add_node",
       TestFixtureHiddenSourceUsesPublicAddNodeSeam},
      {"fixture_animated_scrub_at_display_frame",
       TestFixtureAnimatedScrubAuthorsAtDisplayFrame},
      {"demo_image_processor_tracks_graph_state",
       TestDemoImageProcessorTracksGraphState},
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
