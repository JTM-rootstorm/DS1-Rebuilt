cmake_minimum_required(VERSION 3.25)
project(ds1_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# dxvk-native (d3d9 only, SDL3 WSI), built from ../tools_ext/dxvk.
set(DXVK_NATIVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools_ext/dxvk-native"
    CACHE PATH "dxvk-native install prefix")

find_package(SDL3 REQUIRED CONFIG)

# The d3d9 backend is the Linux bring-up renderer only; macOS will use the
# SDL_GPU (Metal) backend (see ARCHITECTURE.md).
option(DS1_DXVK_BACKEND "Build the dxvk-native d3d9 backend" ${LINUX})

if(DS1_DXVK_BACKEND)
add_library(dxvk_d3d9 SHARED IMPORTED)
set_target_properties(dxvk_d3d9 PROPERTIES
  IMPORTED_LOCATION "${DXVK_NATIVE_DIR}/lib64/libdxvk_d3d9.so"
  INTERFACE_INCLUDE_DIRECTORIES "${DXVK_NATIVE_DIR}/include/dxvk"
  INTERFACE_COMPILE_DEFINITIONS "DXVK_WSI_SDL3")
endif()

# Host runtime: guest address space, PE image mapping, import thunks.
# Win32/DirectX HLE and lifted code will hang off this.
add_library(ds1_runtime STATIC
  src/runtime/guest_memory.cpp
  src/runtime/pe_image.cpp)
target_include_directories(ds1_runtime PUBLIC src)

# Maps DungeonSiege.exe into the guest arena and reports what the HLE
# layer must provide.
add_executable(ds1_loader_probe src/tools/loader_probe.cpp)
target_link_libraries(ds1_loader_probe PRIVATE ds1_runtime)

# Proves dxvk-native + SDL3 work on this machine.
if(DS1_DXVK_BACKEND)
add_executable(d3d9_smoke src/smoke/d3d9_smoke.cpp)
target_link_libraries(d3d9_smoke PRIVATE dxvk_d3d9 SDL3::SDL3)
set_target_properties(d3d9_smoke PROPERTIES
  BUILD_RPATH "${DXVK_NATIVE_DIR}/lib64")
endif()
