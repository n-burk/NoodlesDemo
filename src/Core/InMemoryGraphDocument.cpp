#include <noodles/demo/InMemoryGraphDocument.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace noodles::demo {
namespace {

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

std::string TruncateDisplay(const std::string& value,
                            std::size_t maximum = 28) {
  if (value.size() <= maximum) return value;
  std::size_t end = maximum;
  while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) {
    --end;
  }
  return value.substr(0, end) + "\xE2\x80\xA6";
}

std::string FormatStringDisplay(const std::string& value,
                                const std::string& type) {
  if (value.empty()) return "(none)";
  if (type == "asset") {
    const std::size_t separator = value.find_last_of("/\\");
    if (separator != std::string::npos && separator + 1 < value.size()) {
      return TruncateDisplay(value.substr(separator + 1));
    }
  }
  return TruncateDisplay(value);
}

double CastScalar(double value, const std::string& type) {
  if (type == "bool") return value >= 0.5 ? 1.0 : 0.0;
  if (type == "uint") {
    const double rounded = std::round(value);
    return std::clamp(
        rounded, 0.0,
        static_cast<double>(std::numeric_limits<unsigned>::max()));
  }
  if (type == "int") {
    return std::clamp(std::round(value),
                      static_cast<double>(std::numeric_limits<int>::min()),
                      static_cast<double>(std::numeric_limits<int>::max()));
  }
  if (type == "int64") return std::round(value);
  if (type == "half" || type == "float") {
    return static_cast<double>(static_cast<float>(value));
  }
  return value;
}

bool LinearlyInterpolates(const std::string& type) {
  return type == "half" || type == "float" || type == "double";
}

bool SameEdge(const GraphEdge& a, const GraphEdge& b) {
  return a.sourceNodeId == b.sourceNodeId && a.sourcePort == b.sourcePort &&
         a.targetNodeId == b.targetNodeId && a.targetPort == b.targetPort &&
         a.isRelationship == b.isRelationship;
}

}  // namespace

struct InMemoryGraphDocument::Impl {
  struct NodeRecord {
    GraphNode node;
    std::unordered_map<std::string, std::map<double, double>> samples;
  };

  mutable std::mutex mutex;
  std::map<std::string, NodeRecord> nodes;
  std::vector<GraphEdge> edges;
  ObserverToken nextObserver = 1;
  std::unordered_map<ObserverToken, Observer> observers;

  static GraphProperty* FindProperty(NodeRecord& record,
                                     const std::string& name) {
    auto found = std::find_if(
        record.node.properties.begin(), record.node.properties.end(),
        [&](const GraphProperty& p) { return p.name == name; });
    return found == record.node.properties.end() ? nullptr : &*found;
  }

  static const GraphProperty* FindProperty(const NodeRecord& record,
                                           const std::string& name) {
    auto found = std::find_if(
        record.node.properties.begin(), record.node.properties.end(),
        [&](const GraphProperty& p) { return p.name == name; });
    return found == record.node.properties.end() ? nullptr : &*found;
  }

  std::vector<Observer> observerCopy() const {
    std::vector<Observer> result;
    result.reserve(observers.size());
    for (const auto& [token, observer] : observers) {
      (void)token;
      result.push_back(observer);
    }
    return result;
  }

  static void Notify(const std::vector<Observer>& observers,
                     const GraphDocumentChange& change) {
    for (const Observer& observer : observers) {
      if (observer) observer(change);
    }
  }
};

InMemoryGraphDocument::InMemoryGraphDocument()
    : impl_(std::make_unique<Impl>()) {}

InMemoryGraphDocument::InMemoryGraphDocument(GraphSnapshot initial)
    : InMemoryGraphDocument() {
  replace(std::move(initial));
}

InMemoryGraphDocument::~InMemoryGraphDocument() = default;

void InMemoryGraphDocument::replace(GraphSnapshot snapshotValue) {
  std::vector<Observer> observers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->nodes.clear();
    for (GraphNode& node : snapshotValue.nodes) {
      if (node.id.empty()) continue;
      impl_->nodes[node.id].node = std::move(node);
    }
    impl_->edges = std::move(snapshotValue.edges);
    observers = impl_->observerCopy();
  }
  Impl::Notify(observers, {GraphDocumentChange::Kind::Structure, {}, {}});
}

bool InMemoryGraphDocument::addNode(GraphNode node) {
  if (node.id.empty()) return false;
  std::vector<Observer> observers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->nodes.count(node.id) != 0) return false;
    impl_->nodes[node.id].node = std::move(node);
    observers = impl_->observerCopy();
  }
  Impl::Notify(observers, {GraphDocumentChange::Kind::Structure, {}, {}});
  return true;
}

bool InMemoryGraphDocument::addProperty(const std::string& nodeId,
                                        GraphProperty property) {
  if (property.name.empty()) return false;
  std::vector<Observer> observers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto node = impl_->nodes.find(nodeId);
    if (node == impl_->nodes.end() || !node->second.node.present ||
        Impl::FindProperty(node->second, property.name)) {
      return false;
    }
    node->second.node.properties.push_back(std::move(property));
    observers = impl_->observerCopy();
  }
  Impl::Notify(observers, {GraphDocumentChange::Kind::Structure, nodeId, {}});
  return true;
}

bool InMemoryGraphDocument::setTimeSample(const std::string& nodeId,
                                          const std::string& attributeName,
                                          double frame, double value) {
  std::vector<Observer> observers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto node = impl_->nodes.find(nodeId);
    if (node == impl_->nodes.end()) return false;
    GraphProperty* property = Impl::FindProperty(node->second, attributeName);
    if (!property || property->kind != GraphPropertyKind::Attribute ||
        !property->isScrubable) {
      return false;
    }
    value = CastScalar(value, property->type);
    auto& samples = node->second.samples[attributeName];
    auto found = samples.find(frame);
    if (found != samples.end() && found->second == value) return false;
    samples[frame] = value;
    observers = impl_->observerCopy();
  }
  Impl::Notify(observers, {GraphDocumentChange::Kind::AttributeValue, nodeId,
                           attributeName});
  return true;
}

std::vector<double> InMemoryGraphDocument::timeSamples(
    const std::string& nodeId, const std::string& attributeName) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<double> result;
  auto node = impl_->nodes.find(nodeId);
  if (node == impl_->nodes.end()) return result;
  auto samples = node->second.samples.find(attributeName);
  if (samples == node->second.samples.end()) return result;
  result.reserve(samples->second.size());
  for (const auto& [frame, value] : samples->second) {
    (void)value;
    result.push_back(frame);
  }
  return result;
}

GraphSnapshot InMemoryGraphDocument::snapshot(double displayFrame) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  GraphSnapshot result;

  std::unordered_map<std::string, bool> visible;
  for (const auto& [id, record] : impl_->nodes) {
    if (record.node.hasPosition) visible[id] = true;
  }
  for (const GraphEdge& edge : impl_->edges) {
    visible[edge.sourceNodeId] = true;
    visible[edge.targetNodeId] = true;
  }

  result.nodes.reserve(visible.size());
  for (const auto& [id, record] : impl_->nodes) {
    if (!visible[id]) continue;
    GraphNode node = record.node;
    node.ports.clear();
    for (GraphProperty& property : node.properties) property.connected = false;

    for (const GraphEdge& edge : impl_->edges) {
      if (edge.sourceNodeId != id) continue;
      const GraphPortKind kind = edge.isRelationship
                                     ? GraphPortKind::Relationship
                                     : GraphPortKind::Connection;
      const auto duplicate = std::find_if(
          node.ports.begin(), node.ports.end(), [&](const GraphPort& port) {
            return port.name == edge.sourcePort && port.kind == kind;
          });
      if (duplicate == node.ports.end()) {
        node.ports.push_back({edge.sourcePort, kind});
      }
      for (GraphProperty& property : node.properties) {
        if (property.name == edge.sourcePort &&
            property.kind == (edge.isRelationship
                                  ? GraphPropertyKind::Relationship
                                  : GraphPropertyKind::Attribute)) {
          property.connected = true;
        }
      }
    }

    for (GraphProperty& property : node.properties) {
      auto samples = record.samples.find(property.name);
      if (samples == record.samples.end() || samples->second.empty()) continue;
      const auto& values = samples->second;
      auto value = values.lower_bound(displayFrame);
      double sampledValue = 0.0;
      if (value == values.end()) {
        sampledValue = std::prev(values.end())->second;
      } else if (value->first == displayFrame || value == values.begin()) {
        sampledValue = value->second;
      } else if (LinearlyInterpolates(property.type)) {
        // Match UsdAttribute::Get's default interpolation for floating-point
        // scalar time samples. Discrete scalars (Bool/integer) remain held.
        const auto previous = std::prev(value);
        const double span = value->first - previous->first;
        const double alpha =
            span > 0.0 ? (displayFrame - previous->first) / span : 0.0;
        sampledValue =
            previous->second + (value->second - previous->second) * alpha;
      } else {
        sampledValue = std::prev(value)->second;
      }
      property.hasValue = true;
      property.numericValue = sampledValue;
      property.displayValue = FormatScalar(sampledValue, property.type);
    }
    result.nodes.push_back(std::move(node));
  }
  result.edges = impl_->edges;
  return result;
}

bool InMemoryGraphDocument::containsNode(const std::string& nodeId) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->nodes.find(nodeId);
  return found != impl_->nodes.end() && found->second.node.present;
}

bool InMemoryGraphDocument::authorRelationship(
    const std::string& sourceNodeId, const std::string& relationshipName,
    const std::string& targetNodeId) {
  if (sourceNodeId.empty() || relationshipName.empty() ||
      targetNodeId.empty() || sourceNodeId == targetNodeId) {
    return false;
  }
  std::vector<Observer> observers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto source = impl_->nodes.find(sourceNodeId);
    auto target = impl_->nodes.find(targetNodeId);
    if (source == impl_->nodes.end() || target == impl_->nodes.end() ||
        !source->second.node.present || !target->second.node.present) {
      return false;
    }
    GraphProperty* property =
        Impl::FindProperty(source->second, relationshipName);
    if (property && property->kind != GraphPropertyKind::Relationship) {
      return false;
    }
    GraphEdge edge{sourceNodeId, relationshipName, targetNodeId, {}, true};
    if (std::any_of(
            impl_->edges.begin(), impl_->edges.end(),
            [&](const GraphEdge& old) { return SameEdge(old, edge); })) {
      return false;
    }
    if (!property) {
      source->second.node.properties.push_back(
          {relationshipName, GraphPropertyKind::Relationship, "rel", true});
    }
    impl_->edges.push_back(std::move(edge));
    observers = impl_->observerCopy();
  }
  Impl::Notify(observers, {GraphDocumentChange::Kind::Structure, {}, {}});
  return true;
}

bool InMemoryGraphDocument::removeRelationship(
    const std::string& sourceNodeId, const std::string& relationshipName,
    const std::string& targetNodeId) {
  std::vector<Observer> observers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    GraphEdge edge{sourceNodeId, relationshipName, targetNodeId, {}, true};
    auto found =
        std::find_if(impl_->edges.begin(), impl_->edges.end(),
                     [&](const GraphEdge& old) { return SameEdge(old, edge); });
    if (found == impl_->edges.end()) return false;
    impl_->edges.erase(found);
    observers = impl_->observerCopy();
  }
  Impl::Notify(observers, {GraphDocumentChange::Kind::Structure, {}, {}});
  return true;
}

bool InMemoryGraphDocument::authorConnection(const std::string& inputNodeId,
                                             const std::string& inputPort,
                                             const std::string& outputNodeId,
                                             const std::string& outputPort) {
  if (inputNodeId.empty() || inputPort.empty() || outputNodeId.empty() ||
      inputNodeId == outputNodeId) {
    return false;
  }
  std::vector<Observer> observers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto input = impl_->nodes.find(inputNodeId);
    auto output = impl_->nodes.find(outputNodeId);
    if (input == impl_->nodes.end() || output == impl_->nodes.end() ||
        !input->second.node.present || !output->second.node.present) {
      return false;
    }
    const GraphProperty* inProperty =
        Impl::FindProperty(input->second, inputPort);
    if (!inProperty || inProperty->kind != GraphPropertyKind::Attribute) {
      return false;
    }
    if (!outputPort.empty()) {
      const GraphProperty* outProperty =
          Impl::FindProperty(output->second, outputPort);
      if (!outProperty || outProperty->kind != GraphPropertyKind::Attribute) {
        return false;
      }
    }
    GraphEdge edge{inputNodeId, inputPort, outputNodeId, outputPort, false};
    if (std::any_of(
            impl_->edges.begin(), impl_->edges.end(),
            [&](const GraphEdge& old) { return SameEdge(old, edge); })) {
      return false;
    }
    impl_->edges.push_back(std::move(edge));
    observers = impl_->observerCopy();
  }
  Impl::Notify(observers, {GraphDocumentChange::Kind::Structure, {}, {}});
  return true;
}

bool InMemoryGraphDocument::removeConnection(const std::string& inputNodeId,
                                             const std::string& inputPort,
                                             const std::string& outputNodeId,
                                             const std::string& outputPort) {
  std::vector<Observer> observers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    GraphEdge edge{inputNodeId, inputPort, outputNodeId, outputPort, false};
    auto found =
        std::find_if(impl_->edges.begin(), impl_->edges.end(),
                     [&](const GraphEdge& old) { return SameEdge(old, edge); });
    if (found == impl_->edges.end()) return false;
    impl_->edges.erase(found);
    observers = impl_->observerCopy();
  }
  Impl::Notify(observers, {GraphDocumentChange::Kind::Structure, {}, {}});
  return true;
}

bool InMemoryGraphDocument::setNodePosition(const std::string& nodeId, double x,
                                            double y) {
  std::vector<Observer> observers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto node = impl_->nodes.find(nodeId);
    if (node == impl_->nodes.end() || !node->second.node.present) return false;
    if (node->second.node.hasPosition && node->second.node.posX == x &&
        node->second.node.posY == y) {
      return false;
    }
    node->second.node.hasPosition = true;
    node->second.node.posX = x;
    node->second.node.posY = y;
    observers = impl_->observerCopy();
  }
  Impl::Notify(observers,
               {GraphDocumentChange::Kind::NodePosition, nodeId, {}});
  return true;
}

bool InMemoryGraphDocument::clearNodePosition(const std::string& nodeId) {
  std::vector<Observer> observers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto node = impl_->nodes.find(nodeId);
    if (node == impl_->nodes.end() || !node->second.node.hasPosition)
      return false;
    node->second.node.hasPosition = false;
    node->second.node.posX = 0.0;
    node->second.node.posY = 0.0;
    observers = impl_->observerCopy();
  }
  Impl::Notify(observers,
               {GraphDocumentChange::Kind::NodePosition, nodeId, {}});
  return true;
}

bool InMemoryGraphDocument::setAttributeValue(const std::string& nodeId,
                                              const std::string& attributeName,
                                              double value,
                                              double displayFrame) {
  std::vector<Observer> observers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto node = impl_->nodes.find(nodeId);
    if (node == impl_->nodes.end()) return false;
    GraphProperty* property = Impl::FindProperty(node->second, attributeName);
    if (!property || property->kind != GraphPropertyKind::Attribute ||
        !property->isScrubable) {
      return false;
    }
    value = CastScalar(value, property->type);
    auto samples = node->second.samples.find(attributeName);
    if (samples != node->second.samples.end() && !samples->second.empty()) {
      auto found = samples->second.find(displayFrame);
      if (found != samples->second.end() && found->second == value)
        return false;
      samples->second[displayFrame] = value;
    } else {
      if (property->hasValue && property->numericValue == value) return false;
      property->hasValue = true;
      property->numericValue = value;
      property->displayValue = FormatScalar(value, property->type);
    }
    observers = impl_->observerCopy();
  }
  Impl::Notify(observers, {GraphDocumentChange::Kind::AttributeValue, nodeId,
                           attributeName});
  return true;
}

bool InMemoryGraphDocument::setStringAttributeValue(
    const std::string& nodeId, const std::string& attributeName,
    const std::string& value, double displayFrame) {
  (void)displayFrame;
  std::vector<Observer> observers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto node = impl_->nodes.find(nodeId);
    if (node == impl_->nodes.end()) return false;
    GraphProperty* property = Impl::FindProperty(node->second, attributeName);
    if (!property || property->kind != GraphPropertyKind::Attribute ||
        (property->type != "asset" && property->type != "string" &&
         property->type != "token")) {
      return false;
    }
    if (property->hasValue && property->stringValue == value) return false;
    property->hasValue = true;
    property->stringValue = value;
    property->displayValue = FormatStringDisplay(value, property->type);
    observers = impl_->observerCopy();
  }
  Impl::Notify(observers, {GraphDocumentChange::Kind::AttributeValue, nodeId,
                           attributeName});
  return true;
}

GraphDocument::ObserverToken InMemoryGraphDocument::addObserver(
    Observer observer) {
  if (!observer) return 0;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const ObserverToken token = impl_->nextObserver++;
  impl_->observers.emplace(token, std::move(observer));
  return token;
}

void InMemoryGraphDocument::removeObserver(ObserverToken token) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->observers.erase(token);
}

}  // namespace noodles::demo
