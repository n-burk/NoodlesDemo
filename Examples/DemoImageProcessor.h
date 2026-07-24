#pragma once

#include <noodles/demo/GraphDocument.h>

#include <cstdint>
#include <vector>

namespace noodles::demo::examples {

// Platform-neutral output used by both runnable demos. Pixels are tightly
// packed, top-to-bottom straight-alpha RGBA8. RenderDemoImage itself returns
// opaque pixels. Keeping the example processor independent of AppKit/UIKit
// makes the graph-to-image contract directly unit testable.
struct DemoRgbaImage {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> pixels;

  bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
};

// Render an image from the current graph state. When sourceImage is null, the
// Source Image node uses the deterministic built-in landscape. Hosts may pass
// decoded RGBA8 pixels from their native asset picker without moving file I/O
// or platform image codecs into this portable processor.
DemoRgbaImage RenderDemoImage(const GraphSnapshot& snapshot, int width = 768,
                              int height = 512,
                              const DemoRgbaImage* sourceImage = nullptr);

// Stable checksum intended for example-contract tests and diagnostics.
std::uint64_t DemoImageChecksum(const DemoRgbaImage& image);

}  // namespace noodles::demo::examples
