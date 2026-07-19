#pragma once

#include <noodles/demo/GraphDocument.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace noodles::demo {

// Platform hooks only. Persistence/model semantics remain in GraphDocument;
// UIKit/AppKit shells only schedule rendering, selection UI, and edit
// envelopes.
struct GraphEditDelegate {
  std::function<void()> beginEdit;
  std::function<void()> endEdit;
  std::function<void(const std::string&)> status;
  std::function<void()> requestRender;
  std::function<void(const std::string&)> selectionChanged;
  // Fired exactly once for a successful editor-authored topology mutation
  // (connect, reconnect, disconnect, add-to-canvas, or remove-from-canvas).
  // Unlike graphStructureChanged, this is synchronous and occurs inside the
  // beginEdit/endEdit envelope so product hosts can invalidate their runtime
  // without inferring topology from link/node counts.
  std::function<void()> topologyEdited;
  // External document topology changed underneath the editor. The host should
  // schedule a refresh on its serialized render thread.
  std::function<void()> graphStructureChanged;
  // Fired synchronously for every authored scrub/toggle. `live` is true while
  // the gesture is moving and false at its committed end.
  std::function<void(const std::string& nodeId,
                     const std::string& attributeName, bool live)>
      attributeEdited;
  // A no-move middle-row tap on a display-only attribute. This requests host
  // UI (for example an asset picker); it is not a document edit and therefore
  // does not open the beginEdit/endEdit envelope. Kept last so adding this hook
  // does not disturb positional aggregate initializers for older delegates.
  std::function<void(const std::string& nodeId,
                     const std::string& attributeName)>
      attributeActivated;
};

struct GraphPinInfo {
  std::string name;
  bool isOutput = false;
  bool isRelationship = false;
  double centerX = 0.0;
  double centerY = 0.0;
  std::string typeText;
  bool hasValue = false;
  bool isScrubable = false;
  double value = 0.0;
};

struct GraphLinkInfo {
  std::string sourceNodeId;
  std::string sourcePort;
  std::string targetNodeId;
  std::string targetPort;
  bool isRelationship = false;
  double startX = 0.0;
  double startY = 0.0;
  double endX = 0.0;
  double endY = 0.0;
};

// Shared C++ graph editor. It owns the Noodles model/render lifecycle and all
// interaction state, while a GraphDocument supplies snapshots and mutations.
// Not internally synchronized: call editor methods from one serialized thread.
class GraphEditor {
 public:
  GraphEditor();
  ~GraphEditor();

  GraphEditor(const GraphEditor&) = delete;
  GraphEditor& operator=(const GraphEditor&) = delete;
  GraphEditor(GraphEditor&&) = delete;
  GraphEditor& operator=(GraphEditor&&) = delete;

  void setDelegate(GraphEditDelegate delegate);
  void setDocument(std::shared_ptr<GraphDocument> document);
  std::shared_ptr<GraphDocument> document() const;
  void refresh();

  // GL lifecycle. Each call requires the platform's GL context to be current.
  // assetsPath is the Noodles shader root; atlas and metrics are the MSDF font
  // files. The same source builds against desktop OpenGL and OpenGL ES 3.
  void initializeGL(const std::string& assetsPath,
                    const std::string& fontAtlasPath,
                    const std::string& fontMetricsPath);
  void resize(int widthPx, int heightPx, float contentScale);
  void renderFrame();
  void cleanupGL();

  void setClearColor(float r, float g, float b, float a);
  void setOverlayOpacity(float opacity);
  void setValueScrubEnabled(bool enabled);
  void setDisplayFrame(double frame);

  // Apply queued external value/position changes. Structural changes are
  // surfaced through GraphEditDelegate::graphStructureChanged so the shell can
  // schedule refresh() on its serialized render thread.
  void processPendingDocumentChanges();
  void processPendingStageChanges() { processPendingDocumentChanges(); }

  // Coordinates are view points (top-left origin), not physical pixels.
  void pointerDown(double x, double y);
  void pointerMove(double x, double y);
  void pointerUp(double x, double y);
  void pinchBegin();
  void pinchUpdate(double scale, double anchorX, double anchorY);
  void pinchEnd();

  void setViewportPan(double panX, double panY);
  double panX() const;
  double panY() const;
  void setZoom(double zoom);
  double zoom() const;
  // Center and, when needed, zoom out so every laid-out node fits inside the
  // current viewport. Never magnifies above 1:1. Returns false until resize()
  // has supplied a viewport or when the document has no visible nodes.
  bool frameAll(double paddingPoints = 32.0);

  std::string selectedNodeId() const;
  std::size_t nodeCount() const;
  std::size_t linkCount() const;
  bool nodePosition(const std::string& id, double* x, double* y) const;
  bool nodeSize(const std::string& id, double* w, double* h) const;
  std::vector<GraphPinInfo> nodePins(const std::string& id) const;
  std::vector<GraphLinkInfo> links() const;
  // Returns the transient noodle shown during connection authoring/reconnect.
  // This is useful to hosts that expose drag state through accessibility and
  // keeps preview geometry testable without a GL readback.
  bool activeLinkPreview(GraphLinkInfo* preview) const;

  bool minimapVisible() const;
  bool minimapRect(double* x, double* y, double* w, double* h) const;
  bool minimapPointToWorld(double sx, double sy, double* wx, double* wy) const;
  void viewToGraph(double vx, double vy, double* gx, double* gy) const;
  bool hitsGraphElementAt(double x, double y) const;

  // Remove only the graph representation (position + touching edges), never
  // the backing object itself.
  bool longPressAt(double x, double y);
  bool addNodeAt(const std::string& nodeId, double viewX, double viewY);
  bool addPrimNodeAt(const std::string& nodeId, double viewX, double viewY) {
    return addNodeAt(nodeId, viewX, viewY);
  }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Compatibility spelling for adopters of the earlier host API.
using NoodlesGraphView = GraphEditor;

}  // namespace noodles::demo
