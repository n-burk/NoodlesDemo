# Releasing NoodlesDemo

NoodlesDemo 0.1.x and an Apple/GLES-capable Noodles 1.1.x release are a
coordinated pair. Publish and tag Noodles first. The NoodlesDemo workflow reads
`NOODLES_REPOSITORY` and `NOODLES_REF` from manual-dispatch inputs or repository
Actions variables. Its `facebookexperimental/noodles@main` fallback is only a
real development locator while no compatible 1.1.x release is published; it is
not a release pin. Before a NoodlesDemo release, configure CI with an immutable
tag or commit for the compatible Noodles 1.1.x dependency.

## Release gate

1. Commit the reviewed Noodles portability, package, shader, and test changes.
2. Run its macOS static/shared tests, installed-package consumer, and arm64 iOS
   archive workflow; then publish an immutable Noodles 1.1.x tag.
3. Set `NOODLES_REPOSITORY` and `NOODLES_REF` to that published dependency and
   run NoodlesDemo CI from a clean clone. It must pass the
   `noodles_demo_core`, `noodles_demo_contract`, `noodles_demo_hair`,
   `noodles_demo_render`, `noodles_demo_hair_render` and AppKit shell suites,
   both installed-package consumers, and both iPad demo builds (simulator SDK
   and device SDK).
4. Launch the macOS and iPad demos, confirm they open on Hair Groom, and
   exercise each of the five node-owned tools (Draw Guides, Edit Points, Comb
   Brush, Edit Clump, Paint Clump) plus the tool side panel; confirm the groom
   stays visible beneath the composited graph. Then switch to the image
   documents and confirm the same demo graph supports selection, movement,
   folding, value editing, connections, pan/zoom, and the minimap, and that the
   generated image remains visible beneath the overlay and changes live for
   scalar scrubs, Boolean toggles, display-frame changes, and connection edits.
   On iPad, also confirm Pencil pass-through begins only on empty graph space
   and remains sticky for the touch lifetime.
5. Confirm each demo bundle contains `noodles-assets/` plus both projects'
   notices, then tag the selected NoodlesDemo 0.1.x release.

The optional OpenUSD adapter remains source-embedded during the 0.x line; it is
not part of the installed binary package until supported OpenUSD distributions
share a stable exported CMake dependency target.
