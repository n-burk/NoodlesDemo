if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND TARGET NoodlesDemoUIKit)
  add_library(NoodlesDemoUIKitCompileSmoke OBJECT UIKitShellCompileSmoke.mm)
  target_compile_options(NoodlesDemoUIKitCompileSmoke PRIVATE -fobjc-arc)
  target_link_libraries(NoodlesDemoUIKitCompileSmoke PRIVATE
    NoodlesDemo::UIKit)
endif()

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND
   TARGET NoodlesDemoAppKit)
  add_executable(NoodlesDemoAppKitShellSmoke
    AppKitShellSmokeTests.mm
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Examples/AppleDemoImageLoader.mm")
  target_compile_options(NoodlesDemoAppKitShellSmoke PRIVATE -fobjc-arc)
  target_link_libraries(NoodlesDemoAppKitShellSmoke PRIVATE
    NoodlesDemo::AppKit "-framework ImageIO")
  add_test(NAME noodles_demo_appkit_shell_smoke
           COMMAND NoodlesDemoAppKitShellSmoke)
endif()
