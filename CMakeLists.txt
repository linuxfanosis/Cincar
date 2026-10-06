cmake_minimum_required(VERSION 3.20)
project(Cincar CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(loader loader/elf.cpp loader/loader.cpp)
target_include_directories(loader PUBLIC loader)

add_library(runtime runtime/runtime.cpp)
target_include_directories(runtime PUBLIC runtime)
target_link_libraries(runtime PUBLIC loader)

add_executable(elfinfo tools/elfinfo.cpp)
target_link_libraries(elfinfo loader)

add_executable(runelf tools/runelf.cpp)
target_link_libraries(runelf runtime)
