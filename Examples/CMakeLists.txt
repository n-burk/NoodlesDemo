set(NOODLES_APPLE_ASSET_DIR "" CACHE PATH
    "Fallback Noodles asset root for the runnable Apple demos")

if(TARGET noodles::noodles)
  set(_noodles_example_target noodles::noodles)
elseif(TARGET noodles)
  set(_noodles_example_target noodles)
else()
  message(FATAL_ERROR "NoodlesApple demos require a Noodles target")
endif()

get_target_property(_noodles_target_asset_dir
                    ${_noodles_example_target} NOODLES_ASSET_DIR)
if(_noodles_target_asset_dir AND
   NOT _noodles_target_asset_dir MATCHES "-NOTFOUND$")
  set(_noodles_example_asset_dir "${_noodles_target_asset_dir}")
elseif(NOODLES_ASSET_DIR)
  set(_noodles_example_asset_dir "${NOODLES_ASSET_DIR}")
elseif(NOODLES_APPLE_ASSET_DIR)
  set(_noodles_example_asset_dir "${NOODLES_APPLE_ASSET_DIR}")
else()
  message(FATAL_ERROR
    "Noodles did not publish NOODLES_ASSET_DIR; set NOODLES_APPLE_ASSET_DIR")
endif()

if(NOT EXISTS "${_noodles_example_asset_dir}/shaders" OR
   NOT EXISTS "${_noodles_example_asset_dir}/fonts")
  message(FATAL_ERROR
    "Noodles demo asset root needs shaders/ and fonts/: "
    "${_noodles_example_asset_dir}")
endif()

add_library(NoodlesAppleExampleFixture STATIC
  ContrivedGraphFixture.cpp
  ContrivedImageProcessor.cpp)
target_compile_features(NoodlesAppleExampleFixture PUBLIC cxx_std_17)
target_include_directories(NoodlesAppleExampleFixture PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(NoodlesAppleExampleFixture PUBLIC NoodlesApple::Core)

# Declare assets as bundle resources instead of copying into a guessed bundle
# layout. CMake then chooses `Contents/Resources` on macOS and the bundle root
# on iOS, and Xcode's resource phase sees the same files as Ninja/Make builds.
function(noodles_apple_add_demo_resources target)
  file(GLOB _noodles_demo_shaders CONFIGURE_DEPENDS
       "${_noodles_example_asset_dir}/shaders/*.glsl")
  file(GLOB _noodles_demo_fonts CONFIGURE_DEPENDS
       "${_noodles_example_asset_dir}/fonts/*")
  set_source_files_properties(${_noodles_demo_shaders} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources/noodles-assets/shaders")
  set_source_files_properties(${_noodles_demo_fonts} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources/noodles-assets/fonts")

  set(_noodles_demo_notices
    "${_noodles_example_asset_dir}/../LICENSE.txt"
    "${_noodles_example_asset_dir}/../THIRD_PARTY_LICENSES.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_NOTICES.md")
  foreach(_notice IN LISTS _noodles_demo_notices)
    if(NOT EXISTS "${_notice}")
      message(FATAL_ERROR "Required demo notice is missing: ${_notice}")
    endif()
  endforeach()
  set_source_files_properties(${_noodles_demo_notices} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources/Notices")

  target_sources(${target} PRIVATE
    ${_noodles_demo_shaders}
    ${_noodles_demo_fonts}
    ${_noodles_demo_notices})
  unset(_noodles_demo_fonts)
  unset(_noodles_demo_notices)
  unset(_noodles_demo_shaders)
endfunction()

# Source builds normally use static Noodles on iOS, but an installed package or
# a macOS developer build may provide a shared target. Keep the runnable demos
# self-contained in that case instead of relying on a build-tree rpath.
function(noodles_apple_bundle_noodles_runtime target)
  get_target_property(_noodles_library_type
                      ${_noodles_example_target} TYPE)
  if(_noodles_library_type STREQUAL "SHARED_LIBRARY")
    set_target_properties(${target} PROPERTIES
      BUILD_WITH_INSTALL_RPATH YES
      INSTALL_RPATH "@executable_path/../Frameworks;@executable_path/Frameworks"
      INSTALL_RPATH_USE_LINK_PATH NO)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory
              "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              "$<TARGET_FILE:${_noodles_example_target}>"
              "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks/$<TARGET_SONAME_FILE_NAME:${_noodles_example_target}>"
      COMMENT "Bundling the shared Noodles runtime for ${target}"
      VERBATIM)
  endif()
endfunction()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND TARGET NoodlesAppleUIKit)
  add_executable(NoodlesAppleiPadDemo MACOSX_BUNDLE
    iPad/main.mm
    iPad/NoodlesAppleiPadDemoViewController.mm
    iPad/NoodlesAppleiPadDemoViewController.h)
  target_compile_options(NoodlesAppleiPadDemo PRIVATE
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
  target_link_libraries(NoodlesAppleiPadDemo PRIVATE
    NoodlesApple::UIKit NoodlesAppleExampleFixture
    "-framework Foundation" "-framework CoreGraphics")
  set_target_properties(NoodlesAppleiPadDemo PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "NoodlesApple iPad Demo"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.noodlesapple.demo.ipad"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/iPad/Info.plist.in"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.noodlesapple.demo.ipad"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "2")
  noodles_apple_add_demo_resources(NoodlesAppleiPadDemo)
  noodles_apple_bundle_noodles_runtime(NoodlesAppleiPadDemo)
endif()

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND
   TARGET NoodlesAppleAppKit)
  add_executable(NoodlesAppleMacDemo MACOSX_BUNDLE
    macOS/main.mm
    macOS/NoodlesAppleMacDemoViewController.mm
    macOS/NoodlesAppleMacDemoViewController.h)
  target_compile_options(NoodlesAppleMacDemo PRIVATE
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
  target_link_libraries(NoodlesAppleMacDemo PRIVATE
    NoodlesApple::AppKit NoodlesAppleExampleFixture)
  set_target_properties(NoodlesAppleMacDemo PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "NoodlesApple Mac Demo"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.noodlesapple.demo.macos"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES")
  noodles_apple_add_demo_resources(NoodlesAppleMacDemo)
  noodles_apple_bundle_noodles_runtime(NoodlesAppleMacDemo)
endif()

unset(_noodles_example_asset_dir)
unset(_noodles_example_target)
unset(_noodles_target_asset_dir)
