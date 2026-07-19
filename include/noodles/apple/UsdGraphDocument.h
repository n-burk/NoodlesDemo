#pragma once

#include <noodles/apple/GraphDocument.h>

// Some iOS OpenUSD distributions advertise Python support without shipping
// Python headers. Temporarily select the non-Python C++ declarations without
// changing the including application's preprocessor state.
#ifdef PXR_PYTHON_SUPPORT_ENABLED
#pragma push_macro("PXR_PYTHON_SUPPORT_ENABLED")
#undef PXR_PYTHON_SUPPORT_ENABLED
#define NOODLES_APPLE_RESTORE_PXR_PYTHON_SUPPORT_ENABLED 1
#endif
#include <pxr/pxr.h>
#include <pxr/usd/usd/stage.h>
#ifdef NOODLES_APPLE_RESTORE_PXR_PYTHON_SUPPORT_ENABLED
#pragma pop_macro("PXR_PYTHON_SUPPORT_ENABLED")
#undef NOODLES_APPLE_RESTORE_PXR_PYTHON_SUPPORT_ENABLED
#endif

#include <functional>
#include <memory>
#include <vector>

namespace noodles::apple {

// Optional OpenUSD-backed document. The portable editor never sees pxr types;
// applications that use USD link this adapter target and pass it to
// GraphEditor::setDocument().
class UsdGraphDocument final : public GraphDocument {
 public:
  enum class PropertyFilterUse { NodeRow, EdgeTopology };

  using PostRelationshipMutationHook = std::function<void(
      const pxr::UsdStageRefPtr& stage, const std::string& sourceNodeId,
      const std::string& relationshipName,
      const std::vector<std::string>& targetsBefore,
      const std::vector<std::string>& targetsAfter)>;
  // Called for scalar attributes captured by snapshot(). `scalarEditable` is
  // the adapter's default (true for supported numeric/bool scalar types). A
  // product may return false for attributes its renderer cannot update live.
  using AttributeEditabilityPolicy = std::function<bool(
      const std::string& nodeId, const std::string& schemaTypeName,
      const std::string& attributeName, const std::string& valueType,
      bool scalarEditable)>;
  // Optional visibility seam for product-derived properties. It is evaluated
  // independently for a node row and for relationship/connection topology;
  // the default is to expose every USD property in both uses.
  using PropertyFilter = std::function<bool(
      const std::string& nodeId, const std::string& schemaTypeName,
      const std::string& propertyName, GraphPropertyKind kind,
      PropertyFilterUse use)>;

  explicit UsdGraphDocument(pxr::UsdStageRefPtr stage = {});
  ~UsdGraphDocument() override;

  UsdGraphDocument(const UsdGraphDocument&) = delete;
  UsdGraphDocument& operator=(const UsdGraphDocument&) = delete;

  void setStage(pxr::UsdStageRefPtr stage);
  pxr::UsdStageRefPtr stage() const;
  // Optional product hook for maintaining derived relationships after a
  // successful authored relationship mutation.
  void setPostRelationshipMutationHook(PostRelationshipMutationHook hook);
  void setAttributeEditabilityPolicy(AttributeEditabilityPolicy policy);
  void setPropertyFilter(PropertyFilter filter);

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
