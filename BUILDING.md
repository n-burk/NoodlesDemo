# Building NoodlesApple

## Requirements

- CMake 3.24 or newer
- a C++17 and Objective-C++ capable Apple toolchain
- Noodles 1.1.x, built with the Apple/GLES portability layer
- Xcode with the macOS and iOS SDKs
- OpenUSD only when `NOODLES_APPLE_BUILD_USD_ADAPTER=ON`

Noodles can be supplied as an installed CMake package or as a source checkout:

```sh
cmake -S . -B build \
  -DNOODLES_APPLE_NOODLES_SOURCE_DIR=/path/to/noodles
```

An installed package is preferred for release verification:

```sh
cmake -S /path/to/noodles -B /tmp/noodles-build \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON
cmake --build /tmp/noodles-build --parallel
ctest --test-dir /tmp/noodles-build --output-on-failure
cmake --install /tmp/noodles-build --prefix /tmp/noodles-install

cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/tmp/noodles-install \
  -DNOODLES_APPLE_BUILD_TESTS=ON \
  -DNOODLES_APPLE_BUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## macOS demo

```sh
cmake -S . -B build-macos -G Xcode \
  -DCMAKE_PREFIX_PATH=/tmp/noodles-install \
  -DNOODLES_APPLE_BUILD_EXAMPLES=ON
cmake --build build-macos --config Debug --target NoodlesAppleMacDemo
```

## iPadOS demo

Noodles must be built static for the same device architecture. Configure the
demo with the device SDK; a simulator build uses a separately built simulator
dependency.

```sh
cmake -S . -B build-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DNOODLES_APPLE_NOODLES_SOURCE_DIR=/path/to/noodles \
  -DNOODLES_APPLE_BUILD_EXAMPLES=ON \
  -DNOODLES_APPLE_BUILD_TESTS=OFF
cmake --build build-ios --config Debug --target NoodlesAppleiPadDemo
```

## Optional OpenUSD adapter

The embedding build supplies an OpenUSD interface target and names it with
the required `NOODLES_APPLE_USD_TARGET` setting. NoodlesApple does not discover
product targets implicitly. For example, an embedding application can supply
its own monolithic OpenUSD target:

```cmake
set(NOODLES_APPLE_BUILD_USD_ADAPTER ON)
set(NOODLES_APPLE_USD_TARGET my_openusd_interface)
add_subdirectory(path/to/noodlesApple)
```

`my_openusd_interface` is the embedding application's existing CMake target;
it must publish the required OpenUSD headers and link dependencies.

During the experimental 0.x series, `NoodlesApple::USD` is intentionally a
source-embedding target rather than part of the installed binary package.
OpenUSD's CMake target names and static SDK layouts differ across hosts, so the
embedding application must supply the target and its transitive headers.

## Install and consume

```sh
cmake --install build --prefix /tmp/noodles-apple-install
```

The repository's installed-package consumer verifies both `NoodlesApple::Core`
and, on macOS, `NoodlesApple::AppKit`:

```sh
cmake -S tests/package -B package-consumer \
  -DCMAKE_PREFIX_PATH="/tmp/noodles-install;/tmp/noodles-apple-install"
cmake --build package-consumer --parallel
./package-consumer/NoodlesApplePackageConsumer
./package-consumer/NoodlesAppleAppKitPackageConsumer
```

```cmake
find_package(NoodlesApple 0.1 CONFIG REQUIRED)
target_link_libraries(my_graph_app PRIVATE NoodlesApple::Core)
```
