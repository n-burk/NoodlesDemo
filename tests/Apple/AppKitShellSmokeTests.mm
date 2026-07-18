#import <AppKit/AppKit.h>
#import <NoodlesApple/AppKit/NoodlesAppleGraphView.h>

#include <noodles/apple/GraphDocument.h>
#include <noodles/apple/GraphEditor.h>
#include <noodles/apple/InMemoryGraphDocument.h>

#include <cstdio>
#include <memory>

namespace na = noodles::apple;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,             \
                   #condition);                                                \
      return 1;                                                                \
    }                                                                          \
  } while (false)

int main() {
  @autoreleasepool {
    na::GraphNode node;
    node.id = "/Smoke/Node";
    node.name = "Smoke Node";
    node.schemaTypeName = "Test";
    node.hasPosition = true;
    node.posX = 40.0;
    node.posY = 50.0;
    na::GraphProperty value;
    value.name = "value";
    value.type = "float";
    value.hasValue = true;
    value.isScrubable = true;
    value.numericValue = 0.5;
    value.displayValue = "0.5";
    node.properties.push_back(value);

    na::GraphSnapshot snapshot;
    snapshot.nodes.push_back(node);
    auto document =
        std::make_shared<na::InMemoryGraphDocument>(std::move(snapshot));
    auto editor = std::make_shared<na::GraphEditor>();
    editor->setDocument(document);

    NoodlesAppleGraphView *view =
        [[NoodlesAppleGraphView alloc] initWithFrame:NSMakeRect(0, 0, 640, 480)
                                              editor:editor
                                          assetsPath:@"/tmp/noodles-assets"];
    CHECK(view != nil);
    CHECK([view graphEditor] == editor);
    CHECK([view.assetsPath isEqualToString:@"/tmp/noodles-assets"]);
    CHECK(editor->nodeCount() == 1);

    [view setOverlayOpacity:0.5f];
    [view setClearColorRed:0.0f green:0.0f blue:0.0f alpha:1.0f];
    [view setValueScrubEnabled:YES];
    [view setDisplayFrame:12.0];
    [view reloadGraph];
    (void)[view frameAllWithPadding:24.0];
    CHECK(editor->nodeCount() == 1);
    CHECK([[view selectedNodeId] isEqualToString:@""]);
  }
  std::puts("NoodlesApple AppKit shell smoke ok");
  return 0;
}
