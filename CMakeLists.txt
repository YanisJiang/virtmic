cmake_minimum_required(VERSION 3.20)
project(virtmic LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(VIRTMIC_BUILD_TESTS "Build unit tests" ON)
if(VIRTMIC_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
add_subdirectory(shared)
add_subdirectory(engine)
