#include <noodles/apple/GraphEditor.h>
#include <noodles/apple/InMemoryGraphDocument.h>

#include <memory>

int main() {
  noodles::apple::GraphNode node;
  node.id = "/Package/Node";
  node.name = "Installed package";
  node.hasPosition = true;
  node.posX = 20.0;
  node.posY = 30.0;

  noodles::apple::GraphSnapshot snapshot;
  snapshot.nodes.push_back(std::move(node));
  auto document =
      std::make_shared<noodles::apple::InMemoryGraphDocument>(std::move(snapshot));

  noodles::apple::GraphEditor editor;
  editor.setDocument(std::move(document));
  return editor.nodeCount() == 1 ? 0 : 1;
}
