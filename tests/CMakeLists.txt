add_executable(NoodlesAppleCoreTests
  Core/GraphEditorTests.cpp)
target_compile_features(NoodlesAppleCoreTests PRIVATE cxx_std_17)
target_link_libraries(NoodlesAppleCoreTests PRIVATE NoodlesApple::Core)

add_test(NAME noodles_apple_core COMMAND NoodlesAppleCoreTests)

# Compile and exercise the exact in-memory fixture linked into both runnable
# demos. This guards against a demo silently presenting a narrower interaction
# surface than the shipping USD-backed app while the generic editor tests stay
# green.
add_executable(NoodlesAppleDemoContractTests
  Core/DemoContractTests.cpp
  ../Examples/DemoGraphFixture.cpp
  ../Examples/DemoImageProcessor.cpp)
target_compile_features(NoodlesAppleDemoContractTests PRIVATE cxx_std_17)
target_include_directories(NoodlesAppleDemoContractTests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../Examples")
target_link_libraries(NoodlesAppleDemoContractTests PRIVATE NoodlesApple::Core)
add_test(NAME noodles_apple_demo_contract
         COMMAND NoodlesAppleDemoContractTests)

if(TARGET NoodlesAppleUSD)
  add_executable(NoodlesAppleUsdDocumentTests
    Core/UsdGraphDocumentTests.cpp)
  target_compile_features(NoodlesAppleUsdDocumentTests PRIVATE cxx_std_17)
  target_link_libraries(NoodlesAppleUsdDocumentTests PRIVATE NoodlesApple::USD)
  add_test(NAME noodles_apple_usd_document
           COMMAND NoodlesAppleUsdDocumentTests)
endif()

if(APPLE)
  add_subdirectory(Apple)
endif()

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  if(TARGET noodles::noodles)
    set(_noodles_apple_test_noodles_target noodles::noodles)
  elseif(TARGET noodles)
    set(_noodles_apple_test_noodles_target noodles)
  else()
    message(FATAL_ERROR
      "NoodlesApple render tests require the Noodles CMake target")
  endif()

  get_target_property(_noodles_apple_test_asset_dir
    "${_noodles_apple_test_noodles_target}" NOODLES_ASSET_DIR)
  if(NOT _noodles_apple_test_asset_dir OR
     _noodles_apple_test_asset_dir MATCHES "-NOTFOUND$" OR
     NOT EXISTS "${_noodles_apple_test_asset_dir}/fonts/Poppins-Regular.png" OR
     NOT EXISTS "${_noodles_apple_test_asset_dir}/fonts/Poppins-Regular.json")
    message(FATAL_ERROR
      "Noodles target must expose a valid NOODLES_ASSET_DIR for render tests")
  endif()

  add_executable(NoodlesAppleRenderTests
    Render/GraphEditorRenderTests.cpp
    ../Examples/DemoGraphFixture.cpp)
  target_include_directories(NoodlesAppleRenderTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../Examples")
  target_compile_features(NoodlesAppleRenderTests PRIVATE cxx_std_17)
  target_compile_definitions(NoodlesAppleRenderTests PRIVATE
    GL_SILENCE_DEPRECATION
    "NOODLES_APPLE_TEST_ASSET_DIR=\"${_noodles_apple_test_asset_dir}\"")
  target_link_libraries(NoodlesAppleRenderTests PRIVATE
    NoodlesApple::Core
    "-framework OpenGL")

  add_test(NAME noodles_apple_render COMMAND NoodlesAppleRenderTests)
  set_tests_properties(noodles_apple_render PROPERTIES LABELS "render;macos")

  unset(_noodles_apple_test_noodles_target)
  unset(_noodles_apple_test_asset_dir)
endif()
