#pragma once

#include <memory>

namespace noodles::apple {
class GraphEditor;
class InMemoryGraphDocument;
} // namespace noodles::apple

namespace noodles::apple::examples {

// Shared by both demo applications so iPadOS and macOS exercise precisely the
// same document, layout, values, links, colors and editor configuration.
struct ContrivedGraphFixture {
  std::shared_ptr<InMemoryGraphDocument> document;
  std::shared_ptr<GraphEditor> editor;
};

ContrivedGraphFixture CreateContrivedGraphFixture();

} // namespace noodles::apple::examples
