# Stable source manifest for parent builds that embed NoodlesDemo instead of
# consuming its exported CMake package. Paths are absolute so this fragment can
# be included from any source directory.
set(_NOODLES_DEMO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

set(NOODLES_DEMO_CORE_PUBLIC_HEADERS
  "${_NOODLES_DEMO_ROOT}/include/noodles/demo/GraphDocument.h"
  "${_NOODLES_DEMO_ROOT}/include/noodles/demo/GraphEditor.h"
  "${_NOODLES_DEMO_ROOT}/include/noodles/demo/InMemoryGraphDocument.h")

set(NOODLES_DEMO_CORE_SOURCES
  "${_NOODLES_DEMO_ROOT}/src/Core/GraphEditor.cpp"
  "${_NOODLES_DEMO_ROOT}/src/Core/InMemoryGraphDocument.cpp")

set(NOODLES_DEMO_USD_PUBLIC_HEADERS
  "${_NOODLES_DEMO_ROOT}/include/noodles/demo/UsdGraphDocument.h")

set(NOODLES_DEMO_USD_SOURCES
  "${_NOODLES_DEMO_ROOT}/src/USD/UsdGraphDocument.cpp")

set(NOODLES_DEMO_CORE_TEST_SOURCES
  "${_NOODLES_DEMO_ROOT}/tests/Core/GraphEditorTests.cpp")

set(NOODLES_DEMO_USD_TEST_SOURCES
  "${_NOODLES_DEMO_ROOT}/tests/Core/UsdGraphDocumentTests.cpp")

unset(_NOODLES_DEMO_ROOT)
