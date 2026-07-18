if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND TARGET NoodlesAppleUIKit)
  add_library(NoodlesAppleUIKitCompileSmoke OBJECT UIKitShellCompileSmoke.mm)
  target_compile_options(NoodlesAppleUIKitCompileSmoke PRIVATE -fobjc-arc)
  target_link_libraries(NoodlesAppleUIKitCompileSmoke PRIVATE
    NoodlesApple::UIKit)
endif()

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND
   TARGET NoodlesAppleAppKit)
  add_executable(NoodlesAppleAppKitShellSmoke AppKitShellSmokeTests.mm)
  target_compile_options(NoodlesAppleAppKitShellSmoke PRIVATE -fobjc-arc)
  target_link_libraries(NoodlesAppleAppKitShellSmoke PRIVATE
    NoodlesApple::AppKit)
  add_test(NAME noodles_apple_appkit_shell_smoke
           COMMAND NoodlesAppleAppKitShellSmoke)
endif()
