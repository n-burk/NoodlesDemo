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
- The iPad and macOS demos use the same in-memory demo graph fixture and
  the same renderer configuration as the shipping integration.

## Build and test

See `BUILDING.md`. CMake is the canonical build and install graph. Generated
Xcode projects and build directories are never source.

Run `ctest --test-dir build --output-on-failure` after core or adapter changes.
When touching UIKit or OpenGL ES, also compile the generic iOS device target.
When touching AppKit/OpenGL, run the macOS demo smoke test and pixel tests.
