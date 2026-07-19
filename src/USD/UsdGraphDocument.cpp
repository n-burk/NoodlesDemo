// The iOS OpenUSD archive enables PXR's Python declarations without shipping
// Python headers. This implementation is intentionally C++-only.
#ifdef PXR_PYTHON_SUPPORT_ENABLED
#undef PXR_PYTHON_SUPPORT_ENABLED
#endif

#include <noodles/apple/UsdGraphDocument.h>
#include <pxr/base/gf/half.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/base/tf/weakPtr.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/notice.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/property.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/timeCode.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace noodles::apple {
namespace {

const TfToken& PositionToken() {
  static const TfToken token("ui:nodegraph:node:pos");
  return token;
}

std::string FormatScalar(double value, const std::string& type) {
  if (type == "bool") return value != 0.0 ? "on" : "off";
  char buffer[64];
  if (type == "int" || type == "int64" || type == "uint") {
    std::snprintf(buffer, sizeof(buffer), "%lld",
                  static_cast<long long>(std::llround(value)));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%g", value);
  }
  return buffer;
}

std::string FormatComponent(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%g", value);
  return buffer;
}

std::string TruncateDisplay(const std::string& value,
                            std::size_t maximum = 24) {
  if (value.size() <= maximum) return value;
  std::size_t end = maximum;
  while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) {
    --end;
  }
  return value.substr(0, end) + "\xE2\x80\xA6";
}

void CaptureAttributeValue(const UsdAttribute& attribute,
                           GraphProperty& property, UsdTimeCode time) {
  const SdfValueTypeName valueType = attribute.GetTypeName();
  const auto setNumeric = [&](double value) {
    property.hasValue = true;
    property.isScrubable = true;
    property.numericValue = value;
    property.displayValue = FormatScalar(value, property.type);
  };

  if (valueType == SdfValueTypeNames->Float) {
    float value;
    if (attribute.Get(&value, time)) setNumeric(value);
  } else if (valueType == SdfValueTypeNames->Double) {
    double value;
    if (attribute.Get(&value, time)) setNumeric(value);
  } else if (valueType == SdfValueTypeNames->Half) {
    GfHalf value;
    if (attribute.Get(&value, time)) setNumeric(value);
  } else if (valueType == SdfValueTypeNames->Int) {
    int value;
    if (attribute.Get(&value, time)) setNumeric(value);
  } else if (valueType == SdfValueTypeNames->Int64) {
    std::int64_t value;
    if (attribute.Get(&value, time)) setNumeric(static_cast<double>(value));
  } else if (valueType == SdfValueTypeNames->UInt) {
    unsigned value;
    if (attribute.Get(&value, time)) setNumeric(value);
  } else if (valueType == SdfValueTypeNames->Bool) {
    bool value;
    if (attribute.Get(&value, time)) setNumeric(value ? 1.0 : 0.0);
  } else if (valueType == SdfValueTypeNames->Float2 ||
             valueType == SdfValueTypeNames->Double2) {
    GfVec2d value;
    bool read = false;
    if (valueType == SdfValueTypeNames->Float2) {
      GfVec2f floatValue;
      read = attribute.Get(&floatValue, time);
      value = GfVec2d(floatValue);
    } else {
      read = attribute.Get(&value, time);
    }
    if (read) {
      property.hasValue = true;
      property.displayValue = "(" + FormatComponent(value[0]) + ", " +
                              FormatComponent(value[1]) + ")";
    }
  } else if (valueType == SdfValueTypeNames->Float3 ||
             valueType == SdfValueTypeNames->Double3) {
    GfVec3d value;
    bool read = false;
    if (valueType == SdfValueTypeNames->Float3) {
      GfVec3f floatValue;
      read = attribute.Get(&floatValue, time);
      value = GfVec3d(floatValue);
    } else {
      read = attribute.Get(&value, time);
    }
    if (read) {
      property.hasValue = true;
      property.displayValue = "(" + FormatComponent(value[0]) + ", " +
                              FormatComponent(value[1]) + ", " +
                              FormatComponent(value[2]) + ")";
    }
  } else if (valueType == SdfValueTypeNames->Token) {
    TfToken value;
    if (attribute.Get(&value, time)) {
      property.hasValue = true;
      property.stringValue = value.GetString();
      property.displayValue = TruncateDisplay(property.stringValue);
    }
  } else if (valueType == SdfValueTypeNames->String) {
    std::string value;
    if (attribute.Get(&value, time)) {
      property.hasValue = true;
      property.stringValue = value;
      property.displayValue = TruncateDisplay(property.stringValue);
    }
  } else if (valueType == SdfValueTypeNames->Asset) {
    SdfAssetPath value;
    if (attribute.Get(&value, time)) {
      property.hasValue = true;
      property.stringValue = value.GetAssetPath();
      property.displayValue = TruncateDisplay(property.stringValue);
    }
  }
}

GraphSnapshot CollectGraph(
    const UsdStageRefPtr& stage, UsdTimeCode time,
    const UsdGraphDocument::AttributeEditabilityPolicy& editabilityPolicy,
    const UsdGraphDocument::PropertyFilter& propertyFilter) {
  GraphSnapshot graph;
  if (!stage) return graph;

  struct Accumulator {
    std::vector<GraphPort> ports;
    bool present = false;
  };
  std::map<std::string, Accumulator> accumulators;

  for (const UsdPrim& prim : stage->TraverseAll()) {
    if (prim.IsPseudoRoot()) continue;
    const std::string sourceId = prim.GetPath().GetString();
    const std::string schemaType = prim.GetTypeName().GetString();
    bool isSource = false;

    for (const UsdRelationship& relationship : prim.GetRelationships()) {
      if (!relationship.HasAuthoredTargets()) continue;
      SdfPathVector targets;
      if (!relationship.GetTargets(&targets) || targets.empty()) continue;
      const std::string name = relationship.GetName().GetString();
      if (propertyFilter &&
          !propertyFilter(sourceId, schemaType, name,
                          GraphPropertyKind::Relationship,
                          UsdGraphDocument::PropertyFilterUse::EdgeTopology)) {
        continue;
      }
      accumulators[sourceId].ports.push_back(
          {name, GraphPortKind::Relationship});
      isSource = true;
      for (const SdfPath& target : targets) {
        const std::string targetId = target.GetPrimPath().GetString();
        graph.edges.push_back({sourceId, name, targetId, {}, true});
        accumulators[targetId];
      }
    }

    for (const UsdAttribute& attribute : prim.GetAttributes()) {
      if (!attribute.HasAuthoredConnections()) continue;
      SdfPathVector sources;
      if (!attribute.GetConnections(&sources) || sources.empty()) continue;
      const std::string name = attribute.GetName().GetString();
      if (propertyFilter &&
          !propertyFilter(sourceId, schemaType, name,
                          GraphPropertyKind::Attribute,
                          UsdGraphDocument::PropertyFilterUse::EdgeTopology)) {
        continue;
      }
      accumulators[sourceId].ports.push_back({name, GraphPortKind::Connection});
      isSource = true;
      for (const SdfPath& source : sources) {
        const std::string targetId = source.GetPrimPath().GetString();
        const std::string targetPort =
            source.IsPropertyPath() ? source.GetName() : std::string();
        graph.edges.push_back({sourceId, name, targetId, targetPort, false});
        accumulators[targetId];
      }
    }

    if (isSource) accumulators[sourceId].present = true;
    const UsdAttribute position = prim.GetAttribute(PositionToken());
    if (position && position.HasAuthoredValue()) accumulators[sourceId];
  }

  graph.nodes.reserve(accumulators.size());
  for (auto& [id, accumulator] : accumulators) {
    const UsdPrim prim = stage->GetPrimAtPath(SdfPath(id));
    GraphNode node;
    node.id = prim ? prim.GetPath().GetString() : id;
    node.name = prim ? prim.GetName().GetString() : std::string();
    node.schemaTypeName = prim ? prim.GetTypeName().GetString() : std::string();
    node.ports = std::move(accumulator.ports);
    node.present = static_cast<bool>(prim);

    if (prim) {
      for (const UsdProperty& usdProperty : prim.GetProperties()) {
        GraphProperty property;
        property.name = usdProperty.GetName().GetString();
        if (UsdRelationship relationship = usdProperty.As<UsdRelationship>()) {
          property.kind = GraphPropertyKind::Relationship;
          if (propertyFilter &&
              !propertyFilter(node.id, node.schemaTypeName, property.name,
                              property.kind,
                              UsdGraphDocument::PropertyFilterUse::NodeRow)) {
            continue;
          }
          property.type = "rel";
          property.connected = relationship.HasAuthoredTargets();
        } else if (UsdAttribute attribute = usdProperty.As<UsdAttribute>()) {
          property.kind = GraphPropertyKind::Attribute;
          if (propertyFilter &&
              !propertyFilter(node.id, node.schemaTypeName, property.name,
                              property.kind,
                              UsdGraphDocument::PropertyFilterUse::NodeRow)) {
            continue;
          }
          property.type = attribute.GetTypeName().GetAsToken().GetString();
          property.connected = attribute.HasAuthoredConnections();
          CaptureAttributeValue(attribute, property, time);
          if (editabilityPolicy && property.isScrubable) {
            property.isScrubable =
                editabilityPolicy(node.id, node.schemaTypeName, property.name,
                                  property.type, true);
          }
        } else {
          continue;
        }
        node.properties.push_back(std::move(property));
      }

      const UsdAttribute position = prim.GetAttribute(PositionToken());
      GfVec2f value;
      if (position && position.HasAuthoredValue() && position.Get(&value)) {
        node.hasPosition = true;
        node.posX = value[0];
        node.posY = value[1];
      }
    }
    graph.nodes.push_back(std::move(node));
  }
  return graph;
}

bool IsEditable(const UsdStageRefPtr& stage) {
  if (!stage) return false;
  const SdfLayerHandle layer = stage->GetEditTarget().GetLayer();
  return layer && layer->PermissionToEdit();
}

bool ValidPrimPath(const std::string& value, SdfPath* path) {
  if (value.empty() || !SdfPath::IsValidPathString(value)) return false;
  *path = SdfPath(value);
  return path->IsPrimPath();
}

SdfPath ConnectionTargetPath(const SdfPath& primPath,
                             const std::string& attributeName) {
  return attributeName.empty()
             ? primPath
             : primPath.AppendProperty(TfToken(attributeName));
}

}  // namespace

struct UsdGraphDocument::Impl : public TfWeakBase {
  UsdStageRefPtr stage;
  PostRelationshipMutationHook postRelationshipMutationHook;
  AttributeEditabilityPolicy attributeEditabilityPolicy;
  PropertyFilter propertyFilter;
  TfNotice::Key noticeKey;
  bool listening = false;
  std::mutex observerMutex;
  ObserverToken nextObserver = 1;
  std::unordered_map<ObserverToken, Observer> observers;

  ~Impl() { stopListening(); }

  void stopListening() {
    if (listening) TfNotice::Revoke(noticeKey);
    listening = false;
  }

  void setStage(const UsdStageRefPtr& value) {
    stopListening();
    stage = value;
    if (stage) {
      noticeKey =
          TfNotice::Register(TfCreateWeakPtr(this), &Impl::onObjectsChanged,
                             UsdStageWeakPtr(stage));
      listening = true;
    }
  }

  void notify(const GraphDocumentChange& change) {
    std::vector<Observer> copy;
    {
      std::lock_guard<std::mutex> lock(observerMutex);
      copy.reserve(observers.size());
      for (const auto& [token, observer] : observers) {
        (void)token;
        copy.push_back(observer);
      }
    }
    for (const Observer& observer : copy) {
      if (observer) observer(change);
    }
  }

  void onObjectsChanged(const UsdNotice::ObjectsChanged& notice) {
    bool structural = false;
    for (const SdfPath& path : notice.GetResyncedPaths()) {
      if (path.IsPropertyPath() && path.GetName() == PositionToken()) {
        notify({GraphDocumentChange::Kind::NodePosition,
                path.GetPrimPath().GetString(),
                {}});
      } else {
        structural = true;
      }
    }
    for (const SdfPath& path : notice.GetChangedInfoOnlyPaths()) {
      if (!path.IsPropertyPath()) continue;
      const auto kind = path.GetName() == PositionToken()
                            ? GraphDocumentChange::Kind::NodePosition
                            : GraphDocumentChange::Kind::AttributeValue;
      notify({kind, path.GetPrimPath().GetString(), path.GetName()});
    }
    if (structural) {
      notify({GraphDocumentChange::Kind::Structure, {}, {}});
    }
  }
};

UsdGraphDocument::UsdGraphDocument(UsdStageRefPtr stage)
    : impl_(std::make_unique<Impl>()) {
  impl_->setStage(stage);
}

UsdGraphDocument::~UsdGraphDocument() = default;

void UsdGraphDocument::setStage(UsdStageRefPtr stage) {
  impl_->setStage(stage);
  impl_->notify({GraphDocumentChange::Kind::Structure, {}, {}});
}

UsdStageRefPtr UsdGraphDocument::stage() const { return impl_->stage; }

void UsdGraphDocument::setPostRelationshipMutationHook(
    PostRelationshipMutationHook hook) {
  impl_->postRelationshipMutationHook = std::move(hook);
}

void UsdGraphDocument::setAttributeEditabilityPolicy(
    AttributeEditabilityPolicy policy) {
  impl_->attributeEditabilityPolicy = std::move(policy);
}

void UsdGraphDocument::setPropertyFilter(PropertyFilter filter) {
  impl_->propertyFilter = std::move(filter);
}

GraphSnapshot UsdGraphDocument::snapshot(double displayFrame) const {
  return CollectGraph(impl_->stage, UsdTimeCode(displayFrame),
                      impl_->attributeEditabilityPolicy, impl_->propertyFilter);
}

bool UsdGraphDocument::containsNode(const std::string& nodeId) const {
  SdfPath path;
  return ValidPrimPath(nodeId, &path) && impl_->stage &&
         static_cast<bool>(impl_->stage->GetPrimAtPath(path));
}

bool UsdGraphDocument::authorRelationship(const std::string& sourceNodeId,
                                          const std::string& relationshipName,
                                          const std::string& targetNodeId) {
  SdfPath sourcePath, targetPath;
  if (!ValidPrimPath(sourceNodeId, &sourcePath) ||
      !ValidPrimPath(targetNodeId, &targetPath) || relationshipName.empty() ||
      !SdfPath::IsValidNamespacedIdentifier(relationshipName) ||
      !IsEditable(impl_->stage)) {
    return false;
  }
  const UsdPrim source = impl_->stage->GetPrimAtPath(sourcePath);
  const UsdPrim target = impl_->stage->GetPrimAtPath(targetPath);
  if (!source || !target) return false;

  SdfPathVector before;
  bool added = false;
  {
    SdfChangeBlock block;
    UsdRelationship relationship =
        source.GetRelationship(TfToken(relationshipName));
    if (!relationship) {
      relationship =
          source.CreateRelationship(TfToken(relationshipName), false);
    }
    if (!relationship) return false;
    relationship.GetTargets(&before);
    if (std::find(before.begin(), before.end(), targetPath) != before.end()) {
      return false;
    }
    added = relationship.AddTarget(targetPath);
  }
  if (added && impl_->postRelationshipMutationHook) {
    std::vector<std::string> beforeIds;
    std::vector<std::string> afterIds;
    beforeIds.reserve(before.size());
    afterIds.reserve(before.size() + 1);
    for (const SdfPath& path : before) {
      beforeIds.push_back(path.GetString());
      afterIds.push_back(path.GetString());
    }
    afterIds.push_back(targetPath.GetString());
    impl_->postRelationshipMutationHook(impl_->stage, sourceNodeId,
                                        relationshipName, beforeIds, afterIds);
  }
  return added;
}

bool UsdGraphDocument::removeRelationship(const std::string& sourceNodeId,
                                          const std::string& relationshipName,
                                          const std::string& targetNodeId) {
  SdfPath sourcePath, targetPath;
  if (!ValidPrimPath(sourceNodeId, &sourcePath) ||
      !ValidPrimPath(targetNodeId, &targetPath) || relationshipName.empty() ||
      !SdfPath::IsValidNamespacedIdentifier(relationshipName) ||
      !IsEditable(impl_->stage)) {
    return false;
  }
  const UsdPrim source = impl_->stage->GetPrimAtPath(sourcePath);
  if (!source) return false;
  const UsdRelationship relationship =
      source.GetRelationship(TfToken(relationshipName));
  if (!relationship) return false;
  SdfPathVector before;
  relationship.GetTargets(&before);
  if (std::find(before.begin(), before.end(), targetPath) == before.end()) {
    return false;
  }
  bool removed = false;
  {
    SdfChangeBlock block;
    removed = relationship.RemoveTarget(targetPath);
  }
  if (removed && impl_->postRelationshipMutationHook) {
    std::vector<std::string> beforeIds;
    std::vector<std::string> afterIds;
    beforeIds.reserve(before.size());
    afterIds.reserve(before.size());
    for (const SdfPath& path : before) {
      beforeIds.push_back(path.GetString());
      if (path != targetPath) afterIds.push_back(path.GetString());
    }
    impl_->postRelationshipMutationHook(impl_->stage, sourceNodeId,
                                        relationshipName, beforeIds, afterIds);
  }
  return removed;
}

bool UsdGraphDocument::authorConnection(const std::string& inputNodeId,
                                        const std::string& inputPort,
                                        const std::string& outputNodeId,
                                        const std::string& outputPort) {
  SdfPath inputPath, outputPath;
  if (!ValidPrimPath(inputNodeId, &inputPath) ||
      !ValidPrimPath(outputNodeId, &outputPath) || inputPort.empty() ||
      !SdfPath::IsValidNamespacedIdentifier(inputPort) ||
      (!outputPort.empty() &&
       !SdfPath::IsValidNamespacedIdentifier(outputPort)) ||
      !IsEditable(impl_->stage)) {
    return false;
  }
  const UsdPrim input = impl_->stage->GetPrimAtPath(inputPath);
  const UsdPrim output = impl_->stage->GetPrimAtPath(outputPath);
  if (!input || !output) return false;
  const UsdAttribute attribute = input.GetAttribute(TfToken(inputPort));
  if (!attribute) return false;
  const SdfPath target = ConnectionTargetPath(outputPath, outputPort);
  SdfPathVector current;
  attribute.GetConnections(&current);
  if (std::find(current.begin(), current.end(), target) != current.end()) {
    return false;
  }
  SdfChangeBlock block;
  return attribute.AddConnection(target);
}

bool UsdGraphDocument::removeConnection(const std::string& inputNodeId,
                                        const std::string& inputPort,
                                        const std::string& outputNodeId,
                                        const std::string& outputPort) {
  SdfPath inputPath, outputPath;
  if (!ValidPrimPath(inputNodeId, &inputPath) ||
      !ValidPrimPath(outputNodeId, &outputPath) || inputPort.empty() ||
      !SdfPath::IsValidNamespacedIdentifier(inputPort) ||
      (!outputPort.empty() &&
       !SdfPath::IsValidNamespacedIdentifier(outputPort)) ||
      !IsEditable(impl_->stage)) {
    return false;
  }
  const UsdPrim input = impl_->stage->GetPrimAtPath(inputPath);
  if (!input) return false;
  const UsdAttribute attribute = input.GetAttribute(TfToken(inputPort));
  if (!attribute) return false;
  const SdfPath target = ConnectionTargetPath(outputPath, outputPort);
  SdfPathVector current;
  attribute.GetConnections(&current);
  if (std::find(current.begin(), current.end(), target) == current.end()) {
    return false;
  }
  SdfChangeBlock block;
  return attribute.RemoveConnection(target);
}

bool UsdGraphDocument::setNodePosition(const std::string& nodeId, double x,
                                       double y) {
  SdfPath path;
  if (!ValidPrimPath(nodeId, &path) || !IsEditable(impl_->stage)) return false;
  UsdPrim prim = impl_->stage->GetPrimAtPath(path);
  if (!prim) return false;
  SdfChangeBlock block;
  UsdAttribute position = prim.GetAttribute(PositionToken());
  if (!position) {
    position =
        prim.CreateAttribute(PositionToken(), SdfValueTypeNames->Float2, true);
  }
  return position &&
         position.Set(GfVec2f(static_cast<float>(x), static_cast<float>(y)));
}

bool UsdGraphDocument::clearNodePosition(const std::string& nodeId) {
  SdfPath path;
  if (!ValidPrimPath(nodeId, &path) || !IsEditable(impl_->stage)) return false;
  UsdPrim prim = impl_->stage->GetPrimAtPath(path);
  if (!prim) return false;
  const UsdAttribute position = prim.GetAttribute(PositionToken());
  if (!position || !position.HasAuthoredValue()) return false;
  SdfChangeBlock block;
  return prim.RemoveProperty(PositionToken());
}

bool UsdGraphDocument::setAttributeValue(const std::string& nodeId,
                                         const std::string& attributeName,
                                         double value, double displayFrame) {
  SdfPath path;
  if (!ValidPrimPath(nodeId, &path) || attributeName.empty() ||
      !SdfPath::IsValidNamespacedIdentifier(attributeName) ||
      !IsEditable(impl_->stage)) {
    return false;
  }
  const UsdPrim prim = impl_->stage->GetPrimAtPath(path);
  if (!prim) return false;
  const UsdAttribute attribute = prim.GetAttribute(TfToken(attributeName));
  if (!attribute) return false;
  const SdfValueTypeName type = attribute.GetTypeName();
  const UsdTimeCode time = attribute.GetNumTimeSamples() > 0
                               ? UsdTimeCode(displayFrame)
                               : UsdTimeCode::Default();
  SdfChangeBlock block;
  const auto setIfChanged = [&attribute, time](auto next) {
    decltype(next) old{};
    if (attribute.Get(&old, time) && old == next) return false;
    return attribute.Set(next, time);
  };
  if (type == SdfValueTypeNames->Float) {
    return setIfChanged(static_cast<float>(value));
  }
  if (type == SdfValueTypeNames->Double) return setIfChanged(value);
  if (type == SdfValueTypeNames->Half) {
    return setIfChanged(GfHalf(static_cast<float>(value)));
  }
  if (type == SdfValueTypeNames->Int) {
    return setIfChanged(static_cast<int>(std::llround(value)));
  }
  if (type == SdfValueTypeNames->Int64) {
    return setIfChanged(static_cast<std::int64_t>(std::llround(value)));
  }
  if (type == SdfValueTypeNames->UInt) {
    const long long rounded = std::llround(value);
    return setIfChanged(static_cast<unsigned>(rounded < 0 ? 0 : rounded));
  }
  if (type == SdfValueTypeNames->Bool) return setIfChanged(value >= 0.5);
  return false;
}

bool UsdGraphDocument::setStringAttributeValue(const std::string& nodeId,
                                               const std::string& attributeName,
                                               const std::string& value,
                                               double displayFrame) {
  SdfPath path;
  if (!ValidPrimPath(nodeId, &path) || attributeName.empty() ||
      !SdfPath::IsValidNamespacedIdentifier(attributeName) ||
      !IsEditable(impl_->stage)) {
    return false;
  }
  const UsdPrim prim = impl_->stage->GetPrimAtPath(path);
  if (!prim) return false;
  const UsdAttribute attribute = prim.GetAttribute(TfToken(attributeName));
  if (!attribute) return false;
  const SdfValueTypeName type = attribute.GetTypeName();
  const UsdTimeCode time = attribute.GetNumTimeSamples() > 0
                               ? UsdTimeCode(displayFrame)
                               : UsdTimeCode::Default();
  SdfChangeBlock block;
  if (type == SdfValueTypeNames->String) {
    std::string old;
    if (attribute.Get(&old, time) && old == value) return false;
    return attribute.Set(value, time);
  }
  if (type == SdfValueTypeNames->Token) {
    const TfToken next(value);
    TfToken old;
    if (attribute.Get(&old, time) && old == next) return false;
    return attribute.Set(next, time);
  }
  if (type == SdfValueTypeNames->Asset) {
    SdfAssetPath old;
    if (attribute.Get(&old, time) && old.GetAssetPath() == value) return false;
    return attribute.Set(SdfAssetPath(value), time);
  }
  return false;
}

GraphDocument::ObserverToken UsdGraphDocument::addObserver(Observer observer) {
  if (!observer) return 0;
  std::lock_guard<std::mutex> lock(impl_->observerMutex);
  const ObserverToken token = impl_->nextObserver++;
  impl_->observers.emplace(token, std::move(observer));
  return token;
}

void UsdGraphDocument::removeObserver(ObserverToken token) {
  std::lock_guard<std::mutex> lock(impl_->observerMutex);
  impl_->observers.erase(token);
}

}  // namespace noodles::apple
