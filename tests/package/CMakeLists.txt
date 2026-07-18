cmake_minimum_required(VERSION 3.24)

project(NoodlesApplePackageConsumer LANGUAGES CXX)

find_package(NoodlesApple 0.1 CONFIG REQUIRED)

add_executable(NoodlesApplePackageConsumer main.cpp)
target_compile_features(NoodlesApplePackageConsumer PRIVATE cxx_std_17)
target_link_libraries(NoodlesApplePackageConsumer PRIVATE NoodlesApple::Core)

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  enable_language(OBJCXX)
  add_executable(NoodlesAppleAppKitPackageConsumer appkit_main.mm)
  target_compile_features(NoodlesAppleAppKitPackageConsumer PRIVATE cxx_std_17)
  target_compile_options(NoodlesAppleAppKitPackageConsumer PRIVATE -fobjc-arc)
  target_link_libraries(NoodlesAppleAppKitPackageConsumer
    PRIVATE NoodlesApple::AppKit)
endif()
