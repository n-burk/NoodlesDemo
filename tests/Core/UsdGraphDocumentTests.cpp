#include <noodles/apple/UsdGraphDocument.h>

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>

#include <cstdio>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
namespace na = noodles::apple;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,           \
                   #condition);                                                \
      return false;                                                            \
    }                                                                          \
  } while (false)

namespace {

UsdStageRefPtr MakeStage() {
  UsdStageRefPtr stage = UsdStage::CreateInMemory();
  UsdPrim a = stage->DefinePrim(SdfPath("/Root/A"), TfToken("Xform"));
  UsdPrim b = stage->DefinePrim(SdfPath("/Root/B"), TfToken("Xform"));
  UsdPrim c = stage->DefinePrim(SdfPath("/Root/C"), TfToken("Xform"));
  a.CreateRelationship(TfToken("custom:link"), false)
      .SetTargets({SdfPath("/Root/B")});
  UsdAttribute input =
      c.CreateAttribute(TfToken("in"), SdfValueTypeNames->Float, true);
  input.Set(0.35f);
  b.CreateAttribute(TfToken("out"), SdfValueTypeNames->Float, true);
  input.AddConnection(SdfPath("/Root/B.out"));
  return stage;
}

bool HasEdge(const na::GraphSnapshot& graph, const std::string& source,
             const std::string& sourcePort, const std::string& target,
             const std::string& targetPort, bool relationship) {
  for (const na::GraphEdge& edge : graph.edges) {
    if (edge.sourceNodeId == source && edge.sourcePort == sourcePort &&
        edge.targetNodeId == target && edge.targetPort == targetPort &&
        edge.isRelationship == relationship) {
      return true;
    }
  }
  return false;
}

bool TestSnapshotTopology() {
  na::UsdGraphDocument document(MakeStage());
  const na::GraphSnapshot graph = document.snapshot(0.0);
  CHECK(graph.nodes.size() == 3);
  CHECK(graph.edges.size() == 2);
  CHECK(HasEdge(graph, "/Root/A", "custom:link", "/Root/B", "", true));
  CHECK(HasEdge(graph, "/Root/C", "in", "/Root/B", "out", false));

  bool sawRelationship = false;
  bool sawValue = false;
  for (const na::GraphNode& node : graph.nodes) {
    for (const na::GraphProperty& property : node.properties) {
      if (node.id == "/Root/A" && property.name == "custom:link") {
        sawRelationship =
            property.kind == na::GraphPropertyKind::Relationship &&
            property.connected;
      }
      if (node.id == "/Root/C" && property.name == "in") {
        sawValue = property.kind == na::GraphPropertyKind::Attribute &&
                   property.isScrubable && property.hasValue &&
                   property.numericValue > 0.34 &&
                   property.numericValue < 0.36;
      }
    }
  }
  CHECK(sawRelationship && sawValue);
  return true;
}

bool TestAuthorAndRemoveRelationship() {
  UsdStageRefPtr stage = MakeStage();
  na::UsdGraphDocument document(stage);
  int hookCalls = 0;
  bool hookOk = true;
  std::vector<std::string> lastBefore;
  std::vector<std::string> lastAfter;
  document.setPostRelationshipMutationHook(
      [&](const UsdStageRefPtr& hookStage, const std::string& source,
          const std::string& name, const std::vector<std::string>& before,
          const std::vector<std::string>& after) {
        hookOk = hookOk && hookStage == stage && source == "/Root/A" &&
                 name == "custom:link";
        ++hookCalls;
        lastBefore = before;
        lastAfter = after;
      });

  CHECK(document.authorRelationship("/Root/A", "custom:link", "/Root/C"));
  CHECK(!document.authorRelationship("/Root/A", "custom:link", "/Root/C"));
  CHECK(hookOk && hookCalls == 1 && lastBefore.size() == 1 &&
        lastAfter.size() == 2);
  CHECK(document.removeRelationship("/Root/A", "custom:link", "/Root/C"));
  CHECK(!document.removeRelationship("/Root/A", "custom:link", "/Root/C"));
  CHECK(hookOk && hookCalls == 2 && lastBefore.size() == 2 &&
        lastAfter.size() == 1);
  return true;
}

bool TestDerivedRelationshipHookContract() {
  UsdStageRefPtr stage = UsdStage::CreateInMemory();
  stage->DefinePrim(SdfPath("/Graph/Operator"), TfToken("Xform"));
  stage->DefinePrim(SdfPath("/Graph/Payload"), TfToken("Xform"));
  na::UsdGraphDocument document(stage);

  int calls = 0;
  bool hookOk = true;
  document.setPostRelationshipMutationHook(
      [&](const UsdStageRefPtr&, const std::string& source,
          const std::string& relationship,
          const std::vector<std::string>& before,
          const std::vector<std::string>& after) {
        if (relationship != "graph:binding") return;
        hookOk = hookOk && source == "/Graph/Operator";
        if (calls == 0) {
          hookOk = hookOk && before.empty() &&
                   after == std::vector<std::string>{"/Graph/Payload"};
        } else {
          hookOk = hookOk &&
                   before == std::vector<std::string>{"/Graph/Payload"} &&
                   after.empty();
        }
        ++calls;
      });

  CHECK(document.authorRelationship("/Graph/Operator", "graph:binding",
                                    "/Graph/Payload"));
  CHECK(document.removeRelationship("/Graph/Operator", "graph:binding",
                                    "/Graph/Payload"));
  CHECK(hookOk && calls == 2);
  return true;
}

bool TestPoliciesDefaultAndOverride() {
  na::UsdGraphDocument document(MakeStage());
  // Keep A visible after its only topology property is filtered.
  CHECK(document.setNodePosition("/Root/A", 10.0, 20.0));
  auto findProperty = [](const na::GraphSnapshot& graph,
                         const std::string& nodeId,
                         const std::string& propertyName)
      -> const na::GraphProperty* {
    for (const na::GraphNode& node : graph.nodes) {
      if (node.id != nodeId) continue;
      for (const na::GraphProperty& property : node.properties) {
        if (property.name == propertyName) return &property;
      }
    }
    return nullptr;
  };

  const na::GraphSnapshot defaults = document.snapshot(0.0);
  const na::GraphProperty* defaultInput =
      findProperty(defaults, "/Root/C", "in");
  CHECK(defaultInput && defaultInput->isScrubable);
  CHECK(HasEdge(defaults, "/Root/A", "custom:link", "/Root/B", "", true));

  bool editPolicyCalled = false;
  document.setAttributeEditabilityPolicy(
      [&](const std::string& nodeId, const std::string& schema,
          const std::string& attribute, const std::string& valueType,
          bool scalarEditable) {
        if (nodeId == "/Root/C" && attribute == "in") {
          editPolicyCalled = schema == "Xform" && valueType == "float" &&
                             scalarEditable;
          return false;
        }
        return scalarEditable;
      });
  document.setPropertyFilter(
      [](const std::string& nodeId, const std::string&,
         const std::string& propertyName, na::GraphPropertyKind,
         na::UsdGraphDocument::PropertyFilterUse use) {
        return !(nodeId == "/Root/A" && propertyName == "custom:link" &&
                 use == na::UsdGraphDocument::PropertyFilterUse::EdgeTopology);
      });

  const na::GraphSnapshot overridden = document.snapshot(0.0);
  const na::GraphProperty* overriddenInput =
      findProperty(overridden, "/Root/C", "in");
  CHECK(editPolicyCalled && overriddenInput &&
        !overriddenInput->isScrubable);
  CHECK(!HasEdge(overridden, "/Root/A", "custom:link", "/Root/B", "",
                 true));
  // Filtering topology independently does not hide the row.
  CHECK(findProperty(overridden, "/Root/A", "custom:link") != nullptr);
  return true;
}

bool TestNodePositionRoundTrip() {
  UsdStageRefPtr stage = MakeStage();
  na::UsdGraphDocument document(stage);
  CHECK(document.setNodePosition("/Root/A", 120.0, -35.5));

  GfVec2f authored;
  CHECK(stage->GetPrimAtPath(SdfPath("/Root/A"))
            .GetAttribute(TfToken("ui:nodegraph:node:pos"))
            .Get(&authored));
  CHECK(authored == GfVec2f(120.0f, -35.5f));

  bool sawPosition = false;
  for (const na::GraphNode& node : document.snapshot(0.0).nodes) {
    if (node.id == "/Root/A") {
      sawPosition = node.hasPosition && node.posX == 120.0 &&
                    node.posY == -35.5;
    }
  }
  CHECK(sawPosition);
  CHECK(document.clearNodePosition("/Root/A"));
  return true;
}

bool TestAuthorAndRemoveConnection() {
  UsdStageRefPtr stage = UsdStage::CreateInMemory();
  UsdPrim source = stage->DefinePrim(SdfPath("/Root/S"), TfToken("Xform"));
  UsdPrim destination =
      stage->DefinePrim(SdfPath("/Root/D"), TfToken("Xform"));
  source.CreateAttribute(TfToken("out"), SdfValueTypeNames->Float, true)
      .Set(1.0f);
  destination.CreateAttribute(TfToken("in"), SdfValueTypeNames->Float, true);
  na::UsdGraphDocument document(stage);

  CHECK(document.authorConnection("/Root/D", "in", "/Root/S", "out"));
  CHECK(!document.authorConnection("/Root/D", "in", "/Root/S", "out"));
  CHECK(HasEdge(document.snapshot(0.0), "/Root/D", "in", "/Root/S", "out",
                false));
  CHECK(document.removeConnection("/Root/D", "in", "/Root/S", "out"));
  CHECK(!document.removeConnection("/Root/D", "in", "/Root/S", "out"));
  return true;
}

}  // namespace

int main() {
  struct TestCase {
    const char* name;
    bool (*run)();
  };
  const TestCase tests[] = {
      {"snapshot_topology", TestSnapshotTopology},
      {"author_remove_relationship", TestAuthorAndRemoveRelationship},
      {"derived_relationship_hook", TestDerivedRelationshipHookContract},
      {"policies_default_and_override", TestPoliciesDefaultAndOverride},
      {"node_position_round_trip", TestNodePositionRoundTrip},
      {"author_remove_connection", TestAuthorAndRemoveConnection},
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
  return passed ? 0 : 1;
}
