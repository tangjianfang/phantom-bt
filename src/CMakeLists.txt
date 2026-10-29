add_library(phantom_core STATIC
  core/bytes.cpp
  core/log.cpp
  core/net.cpp
  core/json.cpp
)
target_link_libraries(phantom_core PUBLIC ws2_32)
target_include_directories(phantom_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
