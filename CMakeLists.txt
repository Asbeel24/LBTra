cmake_minimum_required(VERSION 3.16)
project(LBTra CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The engine has no dependencies so it can be dropped into the JUCE plugin as-is.
add_library(lbtra_engine STATIC
  engine/Wav.cpp
  engine/Analysis.cpp
  engine/Effects.cpp
  engine/Engine.cpp)
target_include_directories(lbtra_engine PUBLIC engine)
if(MSVC)
  target_compile_options(lbtra_engine PRIVATE /W4 /utf-8)
else()
  target_compile_options(lbtra_engine PRIVATE -Wall -Wextra)
endif()

add_executable(lbtra_render tools/lbtra_render.cpp)
target_link_libraries(lbtra_render PRIVATE lbtra_engine)

add_executable(lbtra_analyze tools/lbtra_analyze.cpp)
target_link_libraries(lbtra_analyze PRIVATE lbtra_engine)
if(MSVC)
  target_compile_options(lbtra_render PRIVATE /utf-8)
  target_compile_options(lbtra_analyze PRIVATE /utf-8)
endif()
