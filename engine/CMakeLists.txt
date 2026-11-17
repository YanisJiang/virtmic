add_library(virtmic_engine
  src/engine_policy.cpp
  src/shm_writer.cpp
  src/wasapi_capture.cpp
  src/engine.cpp
  src/control_server.cpp
  src/driver_client.cpp
  src/infer_client.cpp
)
target_include_directories(virtmic_engine
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_SOURCE_DIR}/driver/include
)
target_link_libraries(virtmic_engine PUBLIC virtmic_shared)

if(WIN32)
  target_link_libraries(virtmic_engine PUBLIC kernel32 ole32 uuid)
endif()

add_executable(virtmic-engine src/main_cli.cpp)
target_link_libraries(virtmic-engine PRIVATE virtmic_engine)

if(WIN32)
  add_executable(virtmic-feeder ${CMAKE_SOURCE_DIR}/driver/feeder/main.cpp)
  target_include_directories(virtmic-feeder PRIVATE
    ${CMAKE_SOURCE_DIR}/shared/include
    ${CMAKE_SOURCE_DIR}/driver/include
  )
  target_link_libraries(virtmic-feeder PRIVATE kernel32)
endif()

add_executable(virtmic_ctl ${CMAKE_SOURCE_DIR}/tools/virtmic_ctl.cpp)
target_link_libraries(virtmic_ctl PRIVATE virtmic_shared)
if(WIN32)
  target_link_libraries(virtmic_ctl PRIVATE kernel32)
endif()

add_executable(virtmic-shm-probe ${CMAKE_SOURCE_DIR}/tools/shm_probe.cpp)
target_link_libraries(virtmic-shm-probe PRIVATE virtmic_shared)
if(WIN32)
  target_link_libraries(virtmic-shm-probe PRIVATE kernel32)
endif()

add_executable(virtmic-engine-energy-diag ${CMAKE_SOURCE_DIR}/tools/engine_energy_diag.cpp)
target_link_libraries(virtmic-engine-energy-diag PRIVATE virtmic_engine)
if(WIN32)
  target_link_libraries(virtmic-engine-energy-diag PRIVATE kernel32)
endif()

add_executable(virtmic-wasapi-energy-diag ${CMAKE_SOURCE_DIR}/tools/wasapi_energy_diag.cpp)
target_link_libraries(virtmic-wasapi-energy-diag PRIVATE virtmic_engine)
if(WIN32)
  target_link_libraries(virtmic-wasapi-energy-diag PRIVATE kernel32)
endif()

add_executable(virtmic-ctl-start-diag ${CMAKE_SOURCE_DIR}/tools/ctl_start_diag.cpp)
target_link_libraries(virtmic-ctl-start-diag PRIVATE virtmic_engine)
if(WIN32)
  target_link_libraries(virtmic-ctl-start-diag PRIVATE kernel32)
endif()

if(VIRTMIC_BUILD_TESTS)
  add_executable(engine_tests
    tests/test_engine_policy.cpp
    tests/test_shm_writer.cpp
    tests/test_wasapi_capture_smoke.cpp
    tests/test_resample_window.cpp
    tests/test_engine_loop.cpp
    tests/test_infer_client.cpp
  )
  target_link_libraries(engine_tests PRIVATE virtmic_engine GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(engine_tests)
endif()
