add_executable(phantombt-tests
  main.cpp
  unit/test_smoke.cpp
)
target_link_libraries(phantombt-tests PRIVATE phantom_core)
target_compile_definitions(phantombt-tests PRIVATE PB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
