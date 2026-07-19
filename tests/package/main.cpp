#include <noodles/demo/GraphEditor.h>
#include <noodles/demo/InMemoryGraphDocument.h>

#include <memory>

int main() {
  noodles::demo::GraphNode node;
  node.id = "/Package/Node";
  node.name = "Installed package";
  node.hasPosition = true;
  node.posX = 20.0;
  node.posY = 30.0;

  noodles::demo::GraphSnapshot snapshot;
  snapshot.nodes.push_back(std::move(node));
  auto document =
      std::make_shared<noodles::demo::InMemoryGraphDocument>(std::move(snapshot));

  noodles::demo::GraphEditor editor;
  editor.setDocument(std::move(document));
  return editor.nodeCount() == 1 ? 0 : 1;
}
