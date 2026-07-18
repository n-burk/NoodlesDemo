# Stable source manifest for parent builds that embed NoodlesApple instead of
# consuming its exported CMake package. Paths are absolute so this fragment can
# be included from any source directory.
set(_NOODLES_APPLE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

set(NOODLES_APPLE_CORE_PUBLIC_HEADERS
  "${_NOODLES_APPLE_ROOT}/include/noodles/apple/GraphDocument.h"
  "${_NOODLES_APPLE_ROOT}/include/noodles/apple/GraphEditor.h"
  "${_NOODLES_APPLE_ROOT}/include/noodles/apple/InMemoryGraphDocument.h")

set(NOODLES_APPLE_CORE_SOURCES
  "${_NOODLES_APPLE_ROOT}/src/Core/GraphEditor.cpp"
  "${_NOODLES_APPLE_ROOT}/src/Core/InMemoryGraphDocument.cpp")

set(NOODLES_APPLE_USD_PUBLIC_HEADERS
  "${_NOODLES_APPLE_ROOT}/include/noodles/apple/UsdGraphDocument.h")

set(NOODLES_APPLE_USD_SOURCES
  "${_NOODLES_APPLE_ROOT}/src/USD/UsdGraphDocument.cpp")

set(NOODLES_APPLE_CORE_TEST_SOURCES
  "${_NOODLES_APPLE_ROOT}/tests/Core/GraphEditorTests.cpp")

set(NOODLES_APPLE_USD_TEST_SOURCES
  "${_NOODLES_APPLE_ROOT}/tests/Core/UsdGraphDocumentTests.cpp")

unset(_NOODLES_APPLE_ROOT)
