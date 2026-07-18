cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NoodlesApple VERSION 0.1.0 LANGUAGES CXX)
  set(NOODLES_APPLE_IS_TOP_LEVEL ON)
else()
  set(NOODLES_APPLE_IS_TOP_LEVEL OFF)
endif()
set(NOODLES_APPLE_VERSION 0.1.0)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(NOODLES_APPLE_BUILD_TESTS "Build NoodlesApple tests" ${NOODLES_APPLE_IS_TOP_LEVEL})
option(NOODLES_APPLE_BUILD_EXAMPLES "Build the iPadOS/macOS demos" ${NOODLES_APPLE_IS_TOP_LEVEL})
option(NOODLES_APPLE_BUILD_USD_ADAPTER "Build the optional OpenUSD adapter" OFF)
option(NOODLES_APPLE_BUILD_UIKIT "Build the UIKit host on iOS" ON)
option(NOODLES_APPLE_BUILD_APPKIT "Build the AppKit host on macOS" ON)
option(NOODLES_APPLE_ENABLE_INSTALL
  "Generate NoodlesApple install/package rules" ${NOODLES_APPLE_IS_TOP_LEVEL})
set(NOODLES_APPLE_NOODLES_SOURCE_DIR "" CACHE PATH
    "Optional source checkout providing the noodles or noodles::noodles target")
set(NOODLES_APPLE_USD_TARGET "" CACHE STRING
    "Existing OpenUSD interface target used by the optional USD adapter")

include(cmake/CoreSources.cmake)
include(cmake/AppleSources.cmake)

if(NOT TARGET noodles::noodles AND NOT TARGET noodles)
  if(NOODLES_APPLE_NOODLES_SOURCE_DIR)
    if(NOT EXISTS "${NOODLES_APPLE_NOODLES_SOURCE_DIR}/CMakeLists.txt")
      message(FATAL_ERROR
        "NOODLES_APPLE_NOODLES_SOURCE_DIR does not contain CMakeLists.txt: "
        "${NOODLES_APPLE_NOODLES_SOURCE_DIR}")
    endif()
    if(DEFINED BUILD_TESTING)
      set(_noodles_apple_saved_build_testing "${BUILD_TESTING}")
      set(_noodles_apple_had_build_testing ON)
    else()
      set(_noodles_apple_had_build_testing OFF)
    endif()
    set(BUILD_TESTING OFF)
    # A bare iOS dylib is not an embeddable framework and would leave demo/app
    # bundles with an unresolved @rpath/libnoodles dependency. Source-based
    # iOS builds therefore compile Noodles statically, matching the installed
    # package guidance and the shipping Inkwell link graph.
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
      if(DEFINED BUILD_SHARED_LIBS)
        set(_noodles_apple_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
        set(_noodles_apple_had_build_shared_libs ON)
      else()
        set(_noodles_apple_had_build_shared_libs OFF)
      endif()
      set(BUILD_SHARED_LIBS OFF)
    endif()
    add_subdirectory("${NOODLES_APPLE_NOODLES_SOURCE_DIR}"
                     "${CMAKE_CURRENT_BINARY_DIR}/_deps/noodles"
                     EXCLUDE_FROM_ALL)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
      if(_noodles_apple_had_build_shared_libs)
        set(BUILD_SHARED_LIBS "${_noodles_apple_saved_build_shared_libs}")
      else()
        unset(BUILD_SHARED_LIBS)
      endif()
    endif()
    if(_noodles_apple_had_build_testing)
      set(BUILD_TESTING "${_noodles_apple_saved_build_testing}")
    else()
      unset(BUILD_TESTING)
    endif()
  else()
    find_package(noodles 1.1...<1.2 CONFIG REQUIRED)
  endif()
endif()

if(TARGET noodles::noodles)
  set(_noodles_target noodles::noodles)
elseif(TARGET noodles)
  set(_noodles_target noodles)
else()
  message(FATAL_ERROR "NoodlesApple requires noodles::noodles (or noodles)")
endif()

# Source-embedded dependencies bypass find_package's version-file check, so
# enforce the same 1.1.x contract on their target metadata. The public Noodles
# package publishes VERSION; Inkwell's deliberately small shipping checkout
# publishes NOODLES_VERSION.
get_target_property(_noodles_version "${_noodles_target}" NOODLES_VERSION)
if(NOT _noodles_version OR _noodles_version MATCHES "-NOTFOUND$")
  get_target_property(_noodles_version "${_noodles_target}" VERSION)
endif()
if((NOT _noodles_version OR _noodles_version MATCHES "-NOTFOUND$") AND
   DEFINED noodles_VERSION)
  set(_noodles_version "${noodles_VERSION}")
endif()
if(NOT _noodles_version OR _noodles_version MATCHES "-NOTFOUND$")
  message(FATAL_ERROR
    "NoodlesApple requires Noodles 1.1.x, but ${_noodles_target} does not "
    "publish VERSION or NOODLES_VERSION metadata")
endif()
if(_noodles_version VERSION_LESS "1.1.0" OR
   _noodles_version VERSION_GREATER_EQUAL "1.2.0")
  message(FATAL_ERROR
    "NoodlesApple requires Noodles 1.1.x; ${_noodles_target} reports "
    "${_noodles_version}")
endif()

get_target_property(_noodles_asset_dir "${_noodles_target}" NOODLES_ASSET_DIR)
if(NOT _noodles_asset_dir OR _noodles_asset_dir MATCHES "-NOTFOUND$")
  set(_noodles_asset_dir "")
endif()
set(NOODLES_APPLE_ASSET_DIR "${_noodles_asset_dir}" CACHE PATH
    "Noodles shader/font asset root used by NoodlesApple hosts and demos")

if(NOT NOODLES_APPLE_CORE_SOURCES)
  message(FATAL_ERROR "NoodlesApple core sources are missing")
endif()

add_library(NoodlesAppleCore STATIC
  ${NOODLES_APPLE_CORE_PUBLIC_HEADERS}
  ${NOODLES_APPLE_CORE_SOURCES})
add_library(NoodlesApple::Core ALIAS NoodlesAppleCore)
set_target_properties(NoodlesAppleCore PROPERTIES
  EXPORT_NAME Core
  POSITION_INDEPENDENT_CODE ON
  NOODLES_APPLE_ASSET_DIR "${NOODLES_APPLE_ASSET_DIR}")
target_compile_features(NoodlesAppleCore PUBLIC cxx_std_17)
target_include_directories(NoodlesAppleCore
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(NoodlesAppleCore PRIVATE ${_noodles_target})

set(_install_targets NoodlesAppleCore)

if(NOODLES_APPLE_BUILD_USD_ADAPTER)
  if(NOT NOODLES_APPLE_USD_TARGET)
    message(FATAL_ERROR
      "NOODLES_APPLE_BUILD_USD_ADAPTER requires an explicit "
      "NOODLES_APPLE_USD_TARGET")
  endif()
  if(NOT TARGET "${NOODLES_APPLE_USD_TARGET}")
    message(FATAL_ERROR "OpenUSD target does not exist: ${NOODLES_APPLE_USD_TARGET}")
  endif()
  add_library(NoodlesAppleUSD STATIC
    ${NOODLES_APPLE_USD_PUBLIC_HEADERS}
    ${NOODLES_APPLE_USD_SOURCES})
  add_library(NoodlesApple::USD ALIAS NoodlesAppleUSD)
  set_target_properties(NoodlesAppleUSD PROPERTIES
    EXPORT_NAME USD
    POSITION_INDEPENDENT_CODE ON)
  target_compile_features(NoodlesAppleUSD PUBLIC cxx_std_17)
  target_include_directories(NoodlesAppleUSD
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(NoodlesAppleUSD
    PUBLIC NoodlesAppleCore
    PUBLIC "${NOODLES_APPLE_USD_TARGET}")
endif()

if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOODLES_APPLE_BUILD_UIKIT)
  enable_language(OBJCXX)
  if(NOODLES_APPLE_UIKIT_SOURCES)
    add_library(NoodlesAppleUIKit STATIC
      ${NOODLES_APPLE_UIKIT_PUBLIC_HEADERS}
      ${NOODLES_APPLE_UIKIT_SOURCES})
    add_library(NoodlesApple::UIKit ALIAS NoodlesAppleUIKit)
    set_target_properties(NoodlesAppleUIKit PROPERTIES EXPORT_NAME UIKit)
    target_include_directories(NoodlesAppleUIKit
      PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    noodles_apple_configure_objcxx(NoodlesAppleUIKit)
    target_link_libraries(NoodlesAppleUIKit PUBLIC NoodlesAppleCore
      "-framework Foundation" "-framework CoreGraphics" "-framework UIKit"
      "-framework OpenGLES" "-framework QuartzCore")
    list(APPEND _install_targets NoodlesAppleUIKit)
  endif()
endif()

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOODLES_APPLE_BUILD_APPKIT)
  enable_language(OBJCXX)
  if(NOODLES_APPLE_APPKIT_SOURCES)
    add_library(NoodlesAppleAppKit STATIC
      ${NOODLES_APPLE_APPKIT_PUBLIC_HEADERS}
      ${NOODLES_APPLE_APPKIT_SOURCES})
    add_library(NoodlesApple::AppKit ALIAS NoodlesAppleAppKit)
    set_target_properties(NoodlesAppleAppKit PROPERTIES EXPORT_NAME AppKit)
    target_include_directories(NoodlesAppleAppKit
      PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    noodles_apple_configure_objcxx(NoodlesAppleAppKit)
    target_link_libraries(NoodlesAppleAppKit PUBLIC NoodlesAppleCore
      "-framework Foundation" "-framework CoreGraphics" "-framework AppKit"
      "-framework OpenGL" "-framework QuartzCore")
    list(APPEND _install_targets NoodlesAppleAppKit)
  endif()
endif()

if(NOODLES_APPLE_BUILD_TESTS)
  include(CTest)
  enable_testing()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
  endif()
endif()

if(NOODLES_APPLE_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Examples/CMakeLists.txt")
  add_subdirectory(Examples)
endif()

if(NOODLES_APPLE_ENABLE_INSTALL)
  install(TARGETS ${_install_targets}
    EXPORT NoodlesAppleTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  # The optional USD adapter is a source-embedding target in 0.x because
  # OpenUSD does not expose one portable package target across supported SDKs.
  # Avoid installing a header whose corresponding library cannot be exported.
  install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    PATTERN "UsdGraphDocument.h" EXCLUDE)
  install(FILES
    LICENSE README.md BUILDING.md DESIGN.md RELEASING.md
    THIRD_PARTY_NOTICES.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")
  install(EXPORT NoodlesAppleTargets
    FILE NoodlesAppleTargets.cmake
    NAMESPACE NoodlesApple::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/NoodlesApple")

  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/NoodlesAppleConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/NoodlesAppleConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/NoodlesApple")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/NoodlesAppleConfigVersion.cmake"
    VERSION ${NOODLES_APPLE_VERSION}
    COMPATIBILITY SameMinorVersion)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/NoodlesAppleConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/NoodlesAppleConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/NoodlesApple")
endif()

unset(_install_targets)
unset(_noodles_target)
unset(_noodles_version)
unset(_noodles_asset_dir)
unset(_noodles_apple_had_build_testing)
unset(_noodles_apple_saved_build_testing)
unset(_noodles_apple_had_build_shared_libs)
unset(_noodles_apple_saved_build_shared_libs)
