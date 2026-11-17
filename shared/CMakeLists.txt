add_library(virtmic_shared INTERFACE)
target_include_directories(virtmic_shared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(VIRTMIC_BUILD_TESTS)
  add_executable(shared_tests
    tests/test_pcm_constants.cpp
    tests/test_ring_buffer.cpp
  )
  target_link_libraries(shared_tests PRIVATE virtmic_shared GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(shared_tests)
endif()
