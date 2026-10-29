add_library(phantom_core STATIC
  core/bytes.cpp
)
target_include_directories(phantom_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
