cmake_minimum_required(VERSION 3.16)
project(minisql_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(minisql
  src/main.cpp
  src/minidb.cpp
  src/table.cpp
  src/database.cpp
  src/sqlexpr.cpp
)

target_include_directories(minisql PRIVATE include)

if (MSVC)
  target_compile_options(minisql PRIVATE /W4)
else()
  target_compile_options(minisql PRIVATE -Wall -Wextra -pedantic)
endif()
