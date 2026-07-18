#include "ContrivedGraphFixture.h"

#include <noodles/apple/GraphDocument.h>
#include <noodles/apple/GraphEditor.h>
#include <noodles/apple/InMemoryGraphDocument.h>

#include <string>
#include <utility>
#include <vector>

namespace noodles::apple::examples {
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

GraphProperty Relationship(std::string name) {
  GraphProperty property;
  property.name = std::move(name);
  property.kind = GraphPropertyKind::Relationship;
  property.type = "rel";
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

} // namespace

ContrivedGraphFixture CreateContrivedGraphFixture() {
  GraphSnapshot snapshot;
  snapshot.nodes.push_back(
      Node("/Demo/Noise", "Noise", "Generator",
           {Number("noise:frequency", "float", 2.5),
            Number("noise:amplitude", "float", 0.75),
            Toggle("enabled", true)}));
  snapshot.nodes.push_back(Node(
      "/Demo/Grade", "Color Grade", "Processor",
      {Number("input", "color3f", 0.25, false), Number("gain", "float", 1.2),
       Number("mix", "float", 0.8), Relationship("mask")}));
  snapshot.nodes.push_back(
      Node("/Demo/Display", "Display", "Output",
           {Number("surface", "color3f", 0.5, false),
            Number("exposure", "float", 0.0), Toggle("visible", true)}));
  snapshot.nodes.push_back(
      Node("/Demo/Mask", "Ellipse Mask", "Shape",
           {Number("radius", "float2", 0.65, false),
            Number("feather", "float", 0.12),
            Toggle("invert", false)}));
  // Kept deliberately unpositioned and unconnected: GraphDocument visibility
  // rules hide it until a demo's Add Source control calls GraphEditor::addNodeAt.
  snapshot.nodes.push_back(
      Node("/Demo/Source", "Source", "Generator",
           {Number("source:level", "float", 1.0),
            Number("source:bias", "float", 0.0), Toggle("enabled", true)}));

  // Connection edges use document-authoring orientation: the input property is
  // the source endpoint and points at the upstream output property.
  snapshot.edges.push_back(
      {"/Demo/Grade", "input", "/Demo/Noise", "noise:amplitude", false});
  snapshot.edges.push_back(
      {"/Demo/Display", "surface", "/Demo/Grade", "mix", false});
  snapshot.edges.push_back({"/Demo/Grade", "mask", "/Demo/Mask", "", true});

  auto document = std::make_shared<InMemoryGraphDocument>(std::move(snapshot));
  // The four initially visible nodes are connected and deliberately have no
  // stored positions. Both demos therefore exercise the same deterministic
  // fresh-layout path a host sees when it first opens a graph; subsequent drags
  // persist normally. /Demo/Source is the hidden add-node fixture described
  // above.
  // The animated gain row proves that both demos author at displayFrame rather
  // than accidentally replacing its default value.
  document->setTimeSample("/Demo/Grade", "gain", 0.0, 0.8);
  document->setTimeSample("/Demo/Grade", "gain", 24.0, 1.4);

  auto editor = std::make_shared<GraphEditor>();
  editor->setClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  editor->setOverlayOpacity(0.5f);
  editor->setValueScrubEnabled(true);
  editor->setDisplayFrame(12.0);
  editor->setDocument(document);
  return {std::move(document), std::move(editor)};
}

} // namespace noodles::apple::examples
