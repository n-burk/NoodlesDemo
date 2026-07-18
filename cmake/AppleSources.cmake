# Stable Objective-C++ source manifests plus shared compiler requirements.
set(_NOODLES_APPLE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

set(NOODLES_APPLE_UIKIT_PUBLIC_HEADERS
  "${_NOODLES_APPLE_ROOT}/include/NoodlesApple/UIKit/NoodlesAppleGraphView.h"
  "${_NOODLES_APPLE_ROOT}/include/NoodlesApple/UIKit/NoodlesApplePencilForwarding.h")
set(NOODLES_APPLE_UIKIT_SOURCES
  "${_NOODLES_APPLE_ROOT}/src/UIKit/NoodlesAppleGraphView.mm")

set(NOODLES_APPLE_APPKIT_PUBLIC_HEADERS
  "${_NOODLES_APPLE_ROOT}/include/NoodlesApple/AppKit/NoodlesAppleGraphView.h")
set(NOODLES_APPLE_APPKIT_SOURCES
  "${_NOODLES_APPLE_ROOT}/src/AppKit/NoodlesAppleGraphView.mm")

unset(_NOODLES_APPLE_ROOT)

function(noodles_apple_configure_objcxx target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "Unknown NoodlesApple Objective-C++ target: ${target}")
  endif()
  target_compile_features("${target}" PUBLIC cxx_std_17)
  target_compile_options("${target}" PRIVATE
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
endfunction()
