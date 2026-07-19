#pragma once

#include <memory>

namespace noodles::demo {
class GraphEditor;
class InMemoryGraphDocument;
} // namespace noodles::demo

namespace noodles::demo::examples {

// Shared by both demo applications so iPadOS and macOS exercise precisely the
// same document, layout, values, links, colors and editor configuration.
struct DemoGraphFixture {
  std::shared_ptr<InMemoryGraphDocument> document;
  std::shared_ptr<GraphEditor> editor;
};

DemoGraphFixture CreateDemoGraphFixture();

} // namespace noodles::demo::examples
