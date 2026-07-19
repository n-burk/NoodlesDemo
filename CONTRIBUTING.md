# Contributing

Contributions are welcome through the repository's normal pull-request flow.

Before opening a change:

1. keep `NoodlesDemo::Core` free of OpenUSD, UIKit, AppKit, and product globals;
2. add or update a focused unit test for interaction or document behavior;
3. run the macOS build and CTest suite from `BUILDING.md`;
4. compile the generic iOS device target for UIKit/OpenGL ES changes;
5. preserve the shared demo fixture across the iPadOS and macOS demos.

Changes copied from Noodles itself belong upstream in Noodles. NoodlesDemo
should consume renderer capabilities through a documented API rather than carry
an unlabelled source fork.

By contributing, you agree that your contribution is licensed under the MIT
license in `LICENSE`.
