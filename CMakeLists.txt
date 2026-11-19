cmake_minimum_required(VERSION 3.18)
project(teddymod LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Dobby (cloned into ./Dobby by the workflow, or add it as a git submodule)

set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
add_subdirectory(Dobby)

add_library(teddymod SHARED src/main.cpp)
target_include_directories(teddymod PRIVATE Dobby/include)
target_link_libraries(teddymod dobby_static log dl)
set_target_properties(teddymod PROPERTIES CXX_VISIBILITY_PRESET hidden)
