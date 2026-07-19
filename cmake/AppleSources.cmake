# Stable Objective-C++ source manifests plus shared compiler requirements.
set(_NOODLES_DEMO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

set(NOODLES_DEMO_UIKIT_PUBLIC_HEADERS
  "${_NOODLES_DEMO_ROOT}/include/NoodlesDemo/UIKit/NoodlesDemoGraphView.h"
  "${_NOODLES_DEMO_ROOT}/include/NoodlesDemo/UIKit/NoodlesDemoPencilForwarding.h")
set(NOODLES_DEMO_UIKIT_SOURCES
  "${_NOODLES_DEMO_ROOT}/src/UIKit/NoodlesDemoGraphView.mm")

set(NOODLES_DEMO_APPKIT_PUBLIC_HEADERS
  "${_NOODLES_DEMO_ROOT}/include/NoodlesDemo/AppKit/NoodlesDemoGraphView.h")
set(NOODLES_DEMO_APPKIT_SOURCES
  "${_NOODLES_DEMO_ROOT}/src/AppKit/NoodlesDemoGraphView.mm")

unset(_NOODLES_DEMO_ROOT)

function(noodles_demo_configure_objcxx target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "Unknown NoodlesDemo Objective-C++ target: ${target}")
  endif()
  target_compile_features("${target}" PUBLIC cxx_std_17)
  target_compile_options("${target}" PRIVATE
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
endfunction()
