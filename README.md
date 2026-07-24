# NoodlesDemo

NoodlesDemo is a reusable C++17/Objective-C++ host for interactive
[`noodles`](https://github.com/facebookexperimental/noodles) node graphs on
iPadOS and macOS. It supplies the application-facing pieces that the renderer
deliberately does not own: an editable document seam, interaction controller,
Apple GL views, resource plumbing, and runnable example applications.

The core is host-agnostic and depends only on `noodles::noodles`. OpenUSD is an
optional adapter, not part of the core API. The Inkwell iPad application uses
that adapter while the public demos use an in-memory demo graph, so both
exercise the same layout, rendering, hit-testing, value editing, connection,
folding, minimap, and gesture implementation.

## Targets

| CMake target | Purpose |
| --- | --- |
| `NoodlesDemo::Core` | Graph DTO/document API plus the shared editor/controller |
| `NoodlesDemo::USD` | Optional source-embedded OpenUSD collector and mutation adapter |
| `NoodlesDemo::UIKit` | `CAEAGLLayer`/OpenGL ES 3 iPadOS view |
| `NoodlesDemo::AppKit` | `NSOpenGLView` macOS view |

The UIKit and AppKit views render the same `GraphEditor`. Platform code only
creates the GL context, translates native events, schedules frames, and can
optionally forward stylus misses to a background canvas. Without such a target,
Pencil behaves like touch across the graph surface.

## Runnable demo surface

The runnable macOS and iPadOS products are both named NoodlesDemo. A switcher
in the control bar selects one of four example documents, each rendered by the
same shared image processor:

- **Grain Comp** — the six-node contract graph shown below as the everyday
  finishing comp it implements: generated film grain is graded and merged
  over the plate through an ellipse matte, with an animated grain grade;
- **CRT TV** — the classic retro-television effect stack: chromatic color
  fringing, mixed-in broadcast static, sync jitter, scanlines, and a tube
  vignette; the static level and fringing are animated, so the frame slider
  sweeps a clean broadcast into a dying signal;
- **Kaleidoscope** — the plate folded into mirrored wedges, swirled around
  the center, posterized into pop-art bands, and tinted; the fold rotation
  and swirl twist are animated so the frame slider spins the mandala, and a
  user-picked source image becomes their own kaleidoscope;
- **Stress Test** — 100+ nodes and noodles placed by the editor's layered
  auto-layout: eight chains of real image ops (grade, invert, wave, pixelate)
  merge through a mix tree into the composite, so every node executes per
  pixel and scrubbing any op visibly changes the render while probing
  editing, navigation, and rendering throughput at scale.

Each graph keeps its edits while the app runs; switching back restores them.
The Add Node control offers a palette of executable op types — Grade, Invert,
Pixelate, Wave, Mix, Noise, Blur, Threshold, Tint, Chroma, Scanlines,
Vignette, Kaleido, Swirl, Posterize, and Halftone — placed by the editor's
incremental auto-layout; each has a real exec function in the shared image
processor, so a new node changes the render as soon as it is wired between an
image source and Display (Halftone plus Posterize, for example, builds a
comic-print look from any graph).
The default Composite document renders the same six-node, topology-driven
image pipeline as before, with every node initially visible:

[![Watch the NoodlesDemo graph editor demo](media/demo-poster.jpg)](media/demo.mp4)

[Watch the 39-second NoodlesDemo video](media/demo.mp4).

```text
Noise.output:image        -> Grade.input:image
Source Image.output:image -> Composite.background:image
Grade.output:image        -> Composite.foreground:image
Composite.mask:relationship -> Ellipse Mask
Composite.output:image    -> Display.surface:image
```

A shared C++ example processor evaluates the current graph snapshot from the
Display input, so live value edits, Boolean toggles, connection edits, and frame
interpolation visibly change the output rather than changing the graph UI alone.
Image data noodles leave explicit right-side `output` rows whose displayed type
is `image`, rather than appearing to originate from a node's scalar controls.
The Source Image node also exposes an `asset`-typed `path` row: tapping its
middle opens the native image browser, decodes the selected file off the main
thread, and replaces the built-in landscape input.
The apps also expose controls for the shipping overlay defaults:

- overlay opacity from 0.15 through 1.0, initially 0.5;
- display-frame scrubbing from frame 0 through 24, including an interpolated
  animated value;
- a visible `noise:*` property group whose header can be folded independently.

The graph itself demonstrates selection, movement, whole-node and property-group
folding, scalar scrubbing, Boolean toggles, connection authoring and editing,
minimap navigation, and fit-to-view layout. The iPad demo treats Pencil and
touch alike for graph editing; the macOS app exercises mouse, scroll-pan, and
trackpad magnification.

## Supported and tested behavior

The public Core and render suites additionally cover deterministic fresh and
incremental layout, dynamic zoom limits, relationship and data reconnect /
disconnect / cancellation, long-press graph removal without deleting the
backing object, external document observation, current-frame authoring,
touch-sized hit regions, minimap navigation, transparent compositing, and
cached text following node drags. UIKit and AppKit compile/shell checks pin the
native embedding APIs; runnable apps remain the platform-interaction smoke test.

## Quick start

Install a current iOS-capable Noodles package, then build the macOS demo:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/noodles/install
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For local source development, pass
`-DNOODLES_DEMO_NOODLES_SOURCE_DIR=/path/to/noodles`. Installed consumers use
`find_package(NoodlesDemo CONFIG REQUIRED)` and link the namespaced targets.

See [BUILDING.md](BUILDING.md) for iPadOS, install, and external-consumer
commands. Maintainers should follow the coordinated dependency sequence in
[RELEASING.md](RELEASING.md).

## Distribution

NoodlesDemo is MIT licensed. Noodles is a separate MIT-licensed dependency;
its font and third-party notices ship with the Noodles package. A source
distribution must include this repository's `LICENSE` and declare a compatible
Noodles package dependency. No Inkwell product assets or private fixtures are
part of this package.
