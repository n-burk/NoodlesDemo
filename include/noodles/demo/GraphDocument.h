#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace noodles::demo {

// Plain, persistence-agnostic data consumed by GraphEditor. A GraphDocument may
// be backed by the reference in-memory model or an application's own storage.
enum class GraphPropertyKind { Attribute, Relationship };
enum class GraphPropertyDirection { Input, Output };
enum class GraphPortKind { Connection, Relationship };

struct GraphPort {
  std::string name;
  GraphPortKind kind = GraphPortKind::Connection;
};

struct GraphProperty {
  std::string name;
  GraphPropertyKind kind = GraphPropertyKind::Attribute;
  // A display token such as "float", "bool", or "rel".
  std::string type;
  bool connected = false;

  bool hasValue = false;
  bool isScrubable = false;
  double numericValue = 0.0;
  std::string displayValue;

  // Attribute rows default to input-side presentation for compatibility with
  // existing document adapters. Output attributes are rendered as typed,
  // output-only rows on the node's right edge. Relationships remain output
  // rows regardless of this value.
  GraphPropertyDirection direction = GraphPropertyDirection::Input;

  // Exact, untruncated value for token/string/asset attributes. displayValue
  // remains presentation-only and may be shortened to keep nodes compact.
  std::string stringValue;
};

struct GraphNode {
  std::string id;
  std::string name;
  std::string schemaTypeName;
  std::vector<GraphPort> ports;
  std::vector<GraphProperty> properties;
  bool hasPosition = false;
  double posX = 0.0;
  double posY = 0.0;
  // False is permitted for a target-only placeholder. Mutations that require
  // a real backing object reject placeholders.
  bool present = true;
};

struct GraphEdge {
  std::string sourceNodeId;
  std::string sourcePort;
  std::string targetNodeId;
  // Empty for a relationship to a whole node.
  std::string targetPort;
  bool isRelationship = false;
};

struct GraphSnapshot {
  std::vector<GraphNode> nodes;
  std::vector<GraphEdge> edges;
};

struct GraphDocumentChange {
  enum class Kind { AttributeValue, NodePosition, Structure };

  Kind kind = Kind::Structure;
  std::string nodeId;
  std::string propertyName;
};

class GraphDocument {
 public:
  using Observer = std::function<void(const GraphDocumentChange&)>;
  using ObserverToken = std::uint64_t;

  virtual ~GraphDocument() = default;

  // Capture the editor-visible graph at `displayFrame`. Implementations should
  // return stable node/property ordering so refreshes do not visually churn.
  virtual GraphSnapshot snapshot(double displayFrame) const = 0;
  virtual bool containsNode(const std::string& nodeId) const = 0;

  // Relationship edges are owned by (sourceNodeId, relationshipName) and point
  // to a whole node. Attribute connections are authored on the input endpoint
  // and point to the output endpoint.
  virtual bool authorRelationship(const std::string& sourceNodeId,
                                  const std::string& relationshipName,
                                  const std::string& targetNodeId) = 0;
  virtual bool removeRelationship(const std::string& sourceNodeId,
                                  const std::string& relationshipName,
                                  const std::string& targetNodeId) = 0;
  virtual bool authorConnection(const std::string& inputNodeId,
                                const std::string& inputPort,
                                const std::string& outputNodeId,
                                const std::string& outputPort) = 0;
  virtual bool removeConnection(const std::string& inputNodeId,
                                const std::string& inputPort,
                                const std::string& outputNodeId,
                                const std::string& outputPort) = 0;

  virtual bool setNodePosition(const std::string& nodeId, double x,
                               double y) = 0;
  virtual bool clearNodePosition(const std::string& nodeId) = 0;
  virtual bool setAttributeValue(const std::string& nodeId,
                                 const std::string& attributeName, double value,
                                 double displayFrame) = 0;
  virtual bool setStringAttributeValue(const std::string& nodeId,
                                       const std::string& attributeName,
                                       const std::string& value,
                                       double displayFrame) {
    (void)nodeId;
    (void)attributeName;
    (void)value;
    (void)displayFrame;
    return false;
  }

  // Observers may run on the thread that changed the backing model. GraphEditor
  // only queues their data and applies it from processPendingDocumentChanges().
  virtual ObserverToken addObserver(Observer observer) = 0;
  virtual void removeObserver(ObserverToken token) = 0;
};

}  // namespace noodles::demo
