#pragma once

#include <noodles/apple/GraphDocument.h>

#include <cstdint>
#include <vector>

namespace noodles::apple::examples {

// Platform-neutral output used by both runnable demos. Pixels are tightly
// packed, top-to-bottom RGBA8. Keeping the example processor independent of
// AppKit/UIKit makes the graph-to-image contract directly unit testable.
struct ContrivedRgbaImage {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> pixels;

  bool empty() const {
    return width <= 0 || height <= 0 || pixels.empty();
  }
};

// Render a small deterministic landscape-like image from the current graph
// state. The graph's connections decide which stages participate; the exposed
// property values drive noise, grading, mask and display behavior.
ContrivedRgbaImage RenderContrivedImage(const GraphSnapshot& snapshot,
                                        int width = 768,
                                        int height = 512);

// Stable checksum intended for example-contract tests and diagnostics.
std::uint64_t ContrivedImageChecksum(const ContrivedRgbaImage& image);

}  // namespace noodles::apple::examples
