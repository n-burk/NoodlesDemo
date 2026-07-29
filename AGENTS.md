# NoodlesDemo agent guide

NoodlesDemo is an independently buildable Apple host and editor-controller
for Meta's `noodles` C++ node-graph renderer. Keep the reusable package free of
Inkwell application globals and UI notifications.

## Boundaries

- `NoodlesDemoCore` depends only on `noodles::noodles` and the C++ standard
  library. Its public document model must not include OpenUSD or Apple headers.
- `NoodlesDemoUSD` is optional. OpenUSD collection and authoring live there,
  never in the core.
- `NoodlesDemoUIKit` and `NoodlesDemoAppKit` translate platform events and
  own GL contexts. They must drive the same `GraphEditor` implementation.
- Product-specific cache invalidation, undo, notifications, and Pencil canvas
  forwarding policy remain in the consuming application adapter.
- `NoodlesDemoHair` (`Examples/Hair`) is an example consumer of Core, not part
  of it. It must contain no GL, Apple, or OpenUSD types so
  `tests/Hair/HairGroomTests.cpp` links the same code the apps run; GL lives
  only in `Examples/Hair/HairGLRenderer.*`.
- The demos ship two renderer configurations over the same `GraphEditor`: the
  in-memory image fixture (`Examples/DemoGraphFixture.cpp`), and the Hair Groom
  scene, which draws into the graph view's own drawable and therefore requires
  `editor->setOverlayBlendsWithBackground(true)`. Do not change that switch's
  default; `tests/Render/HairRenderTests.cpp` pins both directions.

## Build and test

See `BUILDING.md`. CMake is the canonical build and install graph. Generated
Xcode projects and build directories are never source.

Run `ctest --test-dir build --output-on-failure` after core or adapter changes.
`noodles_demo_core`, `noodles_demo_contract` and `noodles_demo_hair` run
everywhere; `noodles_demo_render`, `noodles_demo_hair_render` and
`noodles_demo_appkit_shell_smoke` are registered only on a macOS host, and
`noodles_demo_usd_document` only when the USD adapter is enabled. The hair
suites exist on the ctest path only — never in the Xcode demo targets.

When touching UIKit or OpenGL ES, also compile the generic iOS device target.
When touching AppKit/OpenGL, run the macOS demo smoke test and pixel tests.
