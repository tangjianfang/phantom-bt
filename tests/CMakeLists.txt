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
  unit/test_report_descriptor.cpp
  unit/test_profiles.cpp
  unit/test_hid_model.cpp
  unit/test_usbip_server.cpp
  unit/test_commands.cpp
  unit/test_hci_packets.cpp
  unit/test_controller.cpp
  unit/test_bt_adapter.cpp
  unit/test_le_air.cpp
  unit/test_ble_discovery.cpp
  unit/test_link.cpp
  unit/test_att_gatt.cpp
)
target_link_libraries(phantombt-tests PRIVATE phantom_core)
target_compile_definitions(phantombt-tests PRIVATE PB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
