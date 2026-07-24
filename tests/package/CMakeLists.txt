cmake_minimum_required(VERSION 3.24)

project(NoodlesDemoPackageConsumer LANGUAGES CXX)

find_package(NoodlesDemo 0.1 CONFIG REQUIRED)

add_executable(NoodlesDemoPackageConsumer main.cpp)
target_compile_features(NoodlesDemoPackageConsumer PRIVATE cxx_std_17)
target_link_libraries(NoodlesDemoPackageConsumer PRIVATE NoodlesDemo::Core)

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  enable_language(OBJCXX)
  add_executable(NoodlesDemoAppKitPackageConsumer appkit_main.mm)
  target_compile_features(NoodlesDemoAppKitPackageConsumer PRIVATE cxx_std_17)
  target_compile_options(NoodlesDemoAppKitPackageConsumer PRIVATE -fobjc-arc)
  target_link_libraries(NoodlesDemoAppKitPackageConsumer
    PRIVATE NoodlesDemo::AppKit)
endif()
