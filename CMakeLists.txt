cmake_minimum_required(VERSION 3.25)
project(phantom-bt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive-)
  add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
endif()

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
