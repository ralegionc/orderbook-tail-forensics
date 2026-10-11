cmake_minimum_required(VERSION 3.24)
project(lob_tails LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MINGW)
  add_link_options(-static)  # executables run without the MinGW DLLs on PATH
endif()

include(FetchContent)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)  # only used through mi_stl_allocator
FetchContent_Declare(mimalloc
  GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
  GIT_TAG v2.1.7
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(mimalloc)

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE include)
target_compile_options(lob INTERFACE -Wall -Wextra -march=native)

add_executable(itch_extract tools/itch_extract.cpp)
target_link_libraries(itch_extract PRIVATE lob)

add_executable(hiccup tools/hiccup.cpp)
target_link_libraries(hiccup PRIVATE lob)

add_executable(replay_bench bench/replay_bench.cpp)
target_link_libraries(replay_bench PRIVATE lob mimalloc-static)

add_executable(test_books tests/test_books.cpp)
target_link_libraries(test_books PRIVATE lob)

enable_testing()
add_test(NAME test_books COMMAND test_books)
