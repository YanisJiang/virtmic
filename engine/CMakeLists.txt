add_library(virtmic_engine
  src/engine_policy.cpp
  src/shm_writer.cpp
  src/wasapi_capture.cpp
)
target_include_directories(virtmic_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(virtmic_engine PUBLIC virtmic_shared)

if(WIN32)
  target_link_libraries(virtmic_engine PUBLIC kernel32 ole32 uuid)
endif()

if(VIRTMIC_BUILD_TESTS)
  add_executable(engine_tests
    tests/test_engine_policy.cpp
    tests/test_shm_writer.cpp
    tests/test_wasapi_capture_smoke.cpp
    tests/test_resample_window.cpp
  )
  target_link_libraries(engine_tests PRIVATE virtmic_engine GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(engine_tests)
endif()
