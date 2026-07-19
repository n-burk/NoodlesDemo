#pragma once

#include <noodles/apple/GraphDocument.h>

#include <memory>
#include <vector>

namespace noodles::apple {

// Reference GraphDocument implementation used by the public demos and unit
// tests. It follows the package visibility/authoring conventions: nodes appear
// when positioned or connected, relationship
// edges target whole nodes, and attribute connections are owned by their input.
class InMemoryGraphDocument final : public GraphDocument {
 public:
  InMemoryGraphDocument();
  explicit InMemoryGraphDocument(GraphSnapshot initial);
  ~InMemoryGraphDocument() override;

  InMemoryGraphDocument(const InMemoryGraphDocument&) = delete;
  InMemoryGraphDocument& operator=(const InMemoryGraphDocument&) = delete;

  void replace(GraphSnapshot snapshot);
  bool addNode(GraphNode node);
  bool addProperty(const std::string& nodeId, GraphProperty property);

  // Add/replace a numeric sample. At least one sample marks the attribute as
  // animated: editor writes then land at displayFrame; otherwise they update
  // the default value.
  bool setTimeSample(const std::string& nodeId,
                     const std::string& attributeName, double frame,
                     double value);
  std::vector<double> timeSamples(const std::string& nodeId,
                                  const std::string& attributeName) const;

  GraphSnapshot snapshot(double displayFrame) const override;
  bool containsNode(const std::string& nodeId) const override;
  bool authorRelationship(const std::string& sourceNodeId,
                          const std::string& relationshipName,
                          const std::string& targetNodeId) override;
  bool removeRelationship(const std::string& sourceNodeId,
                          const std::string& relationshipName,
                          const std::string& targetNodeId) override;
  bool authorConnection(const std::string& inputNodeId,
                        const std::string& inputPort,
                        const std::string& outputNodeId,
                        const std::string& outputPort) override;
  bool removeConnection(const std::string& inputNodeId,
                        const std::string& inputPort,
                        const std::string& outputNodeId,
                        const std::string& outputPort) override;
  bool setNodePosition(const std::string& nodeId, double x, double y) override;
  bool clearNodePosition(const std::string& nodeId) override;
  bool setAttributeValue(const std::string& nodeId,
                         const std::string& attributeName, double value,
                         double displayFrame) override;
  bool setStringAttributeValue(const std::string& nodeId,
                               const std::string& attributeName,
                               const std::string& value,
                               double displayFrame) override;
  ObserverToken addObserver(Observer observer) override;
  void removeObserver(ObserverToken token) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace noodles::apple
