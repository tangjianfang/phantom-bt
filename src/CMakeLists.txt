add_library(phantom_core STATIC
  core/bytes.cpp
  core/log.cpp
  core/net.cpp
  core/json.cpp
  usbip/protocol.cpp
  usb/descriptors.cpp
  usb/control.cpp
  hid/report_descriptor.cpp
  hid/profiles.cpp
  hid/hid_model.cpp
  sim/device_registry.cpp
  usbip/server.cpp
  sim/simulator.cpp
  control/commands.cpp
  hci/packets.h
  hci/controller.cpp
  hci/le_air.cpp
  ble/ble_device.cpp
  hci/link.cpp
  ble/gatt_db.cpp
  ble/att.cpp
  ble/services.cpp
  ble/hogp.cpp
  ble/l2cap.cpp
  ble/aes.cpp
  ble/smp.cpp
  ble/device_stack.cpp
  bluetooth/adapter_model.cpp
)

add_executable(phantombt cli/main.cpp)
target_link_libraries(phantombt PRIVATE phantom_core)

# winrt-probe（可选：依赖 SDK 的 C++/WinRT 头）
set(_pb_cppwinrt "")
foreach(_d "$ENV{WindowsSdkDir}/Include" "C:/Program Files (x86)/Windows Kits/10/Include")
  if(EXISTS "${_d}")
    file(GLOB _vers "${_d}/*")
    foreach(_v ${_vers})
      if(EXISTS "${_v}/cppwinrt/winrt/base.h")
        set(_pb_cppwinrt "${_v}/cppwinrt")
      endif()
    endforeach()
  endif()
endforeach()
if(NOT _pb_cppwinrt STREQUAL "")
  add_executable(winrt-probe probes/main.cpp)
  target_include_directories(winrt-probe PRIVATE ${_pb_cppwinrt})
  target_compile_definitions(winrt-probe PRIVATE _WIN32_WINNT=0x0A00)
  target_link_libraries(winrt-probe PRIVATE windowsapp)
else()
  message(STATUS "winrt-probe: cppwinrt headers not found; target skipped")
endif()
target_link_libraries(phantom_core PUBLIC ws2_32)
target_include_directories(phantom_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
