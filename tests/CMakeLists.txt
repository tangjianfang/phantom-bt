add_executable(phantombt-tests
  main.cpp
  unit/test_smoke.cpp
  unit/test_bytes.cpp
  unit/test_log.cpp
  unit/test_net.cpp
  unit/test_json.cpp
  unit/test_usbip_protocol.cpp
  unit/test_descriptors.cpp
  unit/test_control.cpp
)
target_link_libraries(phantombt-tests PRIVATE phantom_core)
target_compile_definitions(phantombt-tests PRIVATE PB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
