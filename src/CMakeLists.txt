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
)
target_link_libraries(phantom_core PUBLIC ws2_32)
target_include_directories(phantom_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
