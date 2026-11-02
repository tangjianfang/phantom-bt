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
  unit/test_aes_smp.cpp
  integration/test_ble_e2e.cpp
  unit/test_pipe_gui.cpp
  unit/test_btsnoop.cpp
  unit/test_coverage_extra.cpp
  unit/test_env_check.cpp
  unit/test_test_runner.cpp
  unit/test_bt_radio.cpp
  unit/test_global_settings.cpp
  unit/test_ble_cov.cpp
  unit/test_bt_hci_cov.cpp
  unit/test_core_cov.cpp
  unit/test_hid_cov.cpp
  unit/test_infra_cov.cpp
  unit/test_sim_control_cov.cpp
  unit/test_usb_usbip_cov.cpp
  unit/test_cli_cov.cpp
  unit/test_entry_cov.cpp
  unit/test_arch_contracts.cpp
)
target_link_libraries(phantombt-tests PRIVATE phantom_core)
target_compile_definitions(phantombt-tests PRIVATE PB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Linux/macOS 宿主：只编译可在无 Windows 头的情况下链接的逻辑，跑架构契约单测。
# 不代替 MSVC 上的 phantombt-tests，也不覆盖 usbip-win2 / BTHUSB 真机。
if(NOT WIN32)
  add_executable(phantombt-host-tests
    host_main.cpp
    unit/test_arch_contracts.cpp
    unit/test_controller.cpp
    unit/test_aes_smp.cpp
    unit/test_bt_adapter.cpp
    unit/test_hci_packets.cpp
    integration/test_ble_e2e.cpp
    host/posix_shims.cpp
    ../src/core/bytes.cpp
    ../src/core/log.cpp
    ../src/core/json.cpp
    ../src/usbip/protocol.cpp
    ../src/usb/descriptors.cpp
    ../src/usb/control.cpp
    ../src/hid/report_descriptor.cpp
    ../src/hid/profiles.cpp
    ../src/hid/hid_model.cpp
    ../src/sim/device_registry.cpp
    ../src/sim/simulator.cpp
    ../src/sim/btsnoop.cpp
    ../src/control/commands.cpp
    ../src/hci/controller.cpp
    ../src/hci/le_air.cpp
    ../src/hci/link.cpp
    ../src/ble/ble_device.cpp
    ../src/ble/gatt_db.cpp
    ../src/ble/att.cpp
    ../src/ble/services.cpp
    ../src/ble/hogp.cpp
    ../src/ble/l2cap.cpp
    ../src/ble/aes.cpp
    ../src/ble/smp.cpp
    ../src/ble/device_stack.cpp
    ../src/bluetooth/adapter_model.cpp
  )
  target_include_directories(phantombt-host-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(phantombt-host-tests PRIVATE PB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  target_link_libraries(phantombt-host-tests PRIVATE pthread)
  add_test(NAME phantombt-host-tests COMMAND phantombt-host-tests)
endif()
