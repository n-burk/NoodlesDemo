#pragma once

#include <noodles/demo/GraphDocument.h>

#include <memory>
#include <string>

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

// The four demo documents selectable from the apps' graph switcher, modeled
// on everyday compositing workflows. GrainComp is the original six-node
// contract graph (a grain-overlay finishing comp); every variant renders
// through the shared image processor.
enum class DemoGraphVariant {
  // Film-grain finish: generated grain is graded and merged over the plate
  // through an ellipse matte — the contract graph both apps ship first.
  GrainComp = 0,
  // Retro CRT television: chromatic aberration, mixed-in static, jitter,
  // scanlines, and a vignette. The static level and color fringing are
  // animated, so the frame slider sweeps a clean signal into a dying one.
  CrtTv,
  // Psychedelic kaleidoscope: the plate is folded into mirrored wedges,
  // swirled, posterized, and tinted. The fold rotation and swirl twist are
  // animated, so the frame slider spins the mandala.
  Kaleidoscope,
  // Many nodes, links, and animated operations to probe editor throughput:
  // the render chain plus op chains merging into the composite, all placed
  // by the editor's deterministic layered auto-layout.
  StressTest,
};

inline constexpr int kDemoGraphVariantCount = 4;

// Short, user-facing name for the switcher control ("Composite", ...).
const char* DemoGraphVariantTitle(DemoGraphVariant variant);

// Build one variant's document. Hosts that switch graphs on a live editor use
// this with GraphEditor::setDocument and keep their own per-variant cache.
std::shared_ptr<InMemoryGraphDocument> CreateDemoGraphDocument(
    DemoGraphVariant variant);

DemoGraphFixture CreateDemoGraphFixture();
DemoGraphFixture CreateDemoGraphFixture(DemoGraphVariant variant);

// A fresh processor node for the demos' Add Node control: one image input, a
// scrubable gain, one image output. Callers mint the unique id/name pair.
GraphNode MakeDemoProcessorNode(std::string id, std::string name);

// Node types the demos can add at runtime. Every kind has a real exec
// function in the shared image processor, so a newly added node changes the
// rendered output as soon as it is wired between an image source and Display.
enum class DemoOpKind {
  Grade = 0,  // out = mix(in, in * gain, mix)
  Invert,     // out = mix(in, 1 - in, mix)
  Pixelate,   // samples its input on a quantized uv grid
  Wave,       // samples its input through a sine uv distortion
  Mix,        // out = mix(input, blend, mix)
  Noise,      // procedural generator, no input
  Blur,       // 3×3 box average of its input at ±radius
  Threshold,  // luma bright-pass: keeps highlights, crushes the rest
  Tint,       // per-channel gain (red/green/blue)
  Chroma,     // chromatic aberration: R/B sampled at radial offsets
  Scanlines,  // CRT-style horizontal line attenuation
  Vignette,   // radial edge darkening
  Kaleido,    // polar mirror fold into rotated wedges
  Swirl,      // rotational uv distortion around the center
  Posterize,  // per-channel quantization to N levels
  Halftone,   // comic-print color dots on a cell grid
};

inline constexpr int kDemoOpKindCount = 16;

// Menu title for the demos' Add Node control ("Grade", "Invert", ...).
const char* DemoOpKindTitle(DemoOpKind kind);

// Build one addable node of `kind`. Callers mint the unique id/name pair;
// defaults are chosen so the node visibly changes the image once connected.
GraphNode MakeDemoOpNode(DemoOpKind kind, std::string id, std::string name);

} // namespace noodles::demo::examples
