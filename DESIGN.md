# NoodlesDemo design

## Purpose

Noodles is a rendering library: it intentionally does not create windows,
translate native input, or define an application's graph persistence. This
project is the reusable implementation layer between Noodles and Apple apps.
It is independently buildable and distributable so the Inkwell integration is
an example consumer, not the owner of the platform implementation.

## Layering

1. `NoodlesDemo::Core` owns the plain graph DTOs, `GraphDocument` mutation and
   observation contract, Noodles model/layout/render state, hit testing, and the
   complete interaction state machine.
2. `NoodlesDemo::UIKit` and `NoodlesDemo::AppKit` own GL contexts, drawable
   lifecycles, native-event translation, and on-demand frame scheduling. Both
   view classes publish an identical background-host hook set
   (`onRenderBackground`, `onTeardownBackgroundGL`,
   `onBackgroundPointerDown/Move/Up`, `onBackgroundHover`,
   `onBackgroundZoom{Begin,Update,End}`, `graphPanLock`) so gesture-ownership
   logic stays in shared C++ rather than being written twice in Objective-C.
3. `NoodlesDemo::USD` adapts an OpenUSD stage to `GraphDocument`. It is
   optional and cannot leak PXR types into Core.
4. Product adapters own undo integration, persisted document policy, cache and
   renderer invalidation, notifications, and routing a stylus miss into the
   product canvas.

The public demos use `InMemoryGraphDocument`; Inkwell uses the USD adapter. Both
feed the same Core and Apple view targets, which makes demo/product behavior
parity an executable property rather than a visual imitation.

Example consumers (`Examples/Hair`, target `NoodlesDemoHair`) may own typed data
outside `GraphDocument`. The graph carries topology plus scalar and Boolean
controls, and Core stays agnostic to what a host stores alongside it.

## Compositing

`GraphEditor` composites its offscreen graph with blending disabled — correct
when the graph owns a transparent surface above a separate canvas view, where
the overlay quad legitimately owns every pixel. A host that draws its own scene
into the *same* drawable calls `setOverlayBlendsWithBackground(true)`, which
forces the offscreen path — so the graph's clear never reaches host pixels — and
blends the result back with premultiplied source-over. Over a transparent-black
target the two paths are arithmetically identical, so existing adopters are
unaffected. Enabling it makes the host responsible for covering the viewport:
the editor no longer guarantees a defined value for pixels the graph does not
touch. `tests/Render/HairRenderTests.cpp` pins both directions with real pixels.

## Compatibility policy

The initial public API is experimental until 1.0. Within the 0.x line, changes
must keep a source-compatible migration alias when practical. The graph fixture
and interaction/render tests define the behavior contract across iPadOS,
macOS, and Inkwell.

## Dependency policy

Noodles 1.1.x is a required, separately versioned CMake package. Source
builds may accept an explicit local checkout; installed consumers never fetch
implicitly. OpenUSD is an optional adapter
dependency and is not required to build Core or the demos. During the 0.x
series the USD adapter is consumed from source because supported OpenUSD SDKs
do not share a stable exported CMake target contract.
