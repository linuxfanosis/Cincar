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

add_library(shader shader/translate.cpp shader/eval.cpp)
target_include_directories(shader PUBLIC shader)

add_executable(shadercc tools/shadercc.cpp)
target_link_libraries(shadercc shader)

add_executable(shaderrun tools/shaderrun.cpp)
target_link_libraries(shaderrun shader)

# Optional: runs translated shaders on a real Vulkan device (needs libvulkan-dev).
find_package(Vulkan)
if(Vulkan_FOUND)
  add_executable(vkrun tools/vkrun.cpp)
  target_link_libraries(vkrun shader Vulkan::Vulkan)
else()
  message(STATUS "Vulkan not found: vkrun will not be built")
endif()
