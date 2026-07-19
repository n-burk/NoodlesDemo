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

}  // namespace

DemoGraphFixture CreateDemoGraphFixture() {
  GraphSnapshot snapshot;
  snapshot.nodes.push_back(
      Node("/Demo/SourceImage", "Source Image", "Generator",
           {Asset("path", "", "Built-in Landscape"),
            Number("brightness", "float", 1.0), Number("bias", "float", 0.0),
            Toggle("enabled", true),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Noise", "Noise", "Generator",
           {Number("noise:frequency", "float", 2.5),
            Number("noise:amplitude", "float", 0.75), Toggle("enabled", true),
            Port("output", "image", GraphPropertyDirection::Output)}));
  snapshot.nodes.push_back(
      Node("/Demo/Grade", "Color Grade", "Processor",
           {Port("input", "image", GraphPropertyDirection::Input),
            Number("gain", "float", 1.2), Number("mix", "float", 0.8),
            Port("output", "image", GraphPropertyDirection::Output)}));
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
  snapshot.nodes.push_back(
      Node("/Demo/Mask", "Ellipse Mask", "Shape",
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

  auto document = std::make_shared<InMemoryGraphDocument>(std::move(snapshot));
  // All six nodes are connected and deliberately have no stored positions.
  // Both demos therefore exercise the same deterministic fresh-layout path a
  // host sees when it first opens a graph; subsequent drags persist normally.
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

}  // namespace noodles::demo::examples
