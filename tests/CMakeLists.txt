add_executable(NoodlesDemoCoreTests
  Core/GraphEditorTests.cpp)
target_compile_features(NoodlesDemoCoreTests PRIVATE cxx_std_17)
target_link_libraries(NoodlesDemoCoreTests PRIVATE NoodlesDemo::Core)

add_test(NAME noodles_demo_core COMMAND NoodlesDemoCoreTests)

# Compile and exercise the exact in-memory fixture linked into both runnable
# demos. This guards against a demo silently presenting a narrower interaction
# surface than the shipping USD-backed app while the generic editor tests stay
# green.
add_executable(NoodlesDemoContractTests
  Core/DemoContractTests.cpp
  ../Examples/DemoGraphFixture.cpp
  ../Examples/DemoImageProcessor.cpp)
target_compile_features(NoodlesDemoContractTests PRIVATE cxx_std_17)
target_include_directories(NoodlesDemoContractTests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../Examples")
target_link_libraries(NoodlesDemoContractTests PRIVATE NoodlesDemo::Core)
add_test(NAME noodles_demo_contract
         COMMAND NoodlesDemoContractTests)

if(TARGET NoodlesDemoUSD)
  add_executable(NoodlesDemoUsdDocumentTests
    Core/UsdGraphDocumentTests.cpp)
  target_compile_features(NoodlesDemoUsdDocumentTests PRIVATE cxx_std_17)
  target_link_libraries(NoodlesDemoUsdDocumentTests PRIVATE NoodlesDemo::USD)
  add_test(NAME noodles_demo_usd_document
           COMMAND NoodlesDemoUsdDocumentTests)
endif()

if(APPLE)
  add_subdirectory(Apple)
endif()

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  if(TARGET noodles::noodles)
    set(_noodles_demo_test_noodles_target noodles::noodles)
  elseif(TARGET noodles)
    set(_noodles_demo_test_noodles_target noodles)
  else()
    message(FATAL_ERROR
      "NoodlesDemo render tests require the Noodles CMake target")
  endif()

  get_target_property(_noodles_demo_test_asset_dir
    "${_noodles_demo_test_noodles_target}" NOODLES_ASSET_DIR)
  if(NOT _noodles_demo_test_asset_dir OR
     _noodles_demo_test_asset_dir MATCHES "-NOTFOUND$" OR
     NOT EXISTS "${_noodles_demo_test_asset_dir}/fonts/Poppins-Regular.png" OR
     NOT EXISTS "${_noodles_demo_test_asset_dir}/fonts/Poppins-Regular.json")
    message(FATAL_ERROR
      "Noodles target must expose a valid NOODLES_ASSET_DIR for render tests")
  endif()

  add_executable(NoodlesDemoRenderTests
    Render/GraphEditorRenderTests.cpp
    ../Examples/DemoGraphFixture.cpp)
  target_include_directories(NoodlesDemoRenderTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../Examples")
  target_compile_features(NoodlesDemoRenderTests PRIVATE cxx_std_17)
  target_compile_definitions(NoodlesDemoRenderTests PRIVATE
    GL_SILENCE_DEPRECATION
    "NOODLES_DEMO_TEST_ASSET_DIR=\"${_noodles_demo_test_asset_dir}\"")
  target_link_libraries(NoodlesDemoRenderTests PRIVATE
    NoodlesDemo::Core
    "-framework OpenGL")

  add_test(NAME noodles_demo_render COMMAND NoodlesDemoRenderTests)
  set_tests_properties(noodles_demo_render PROPERTIES LABELS "render;macos")

  unset(_noodles_demo_test_noodles_target)
  unset(_noodles_demo_test_asset_dir)
endif()
