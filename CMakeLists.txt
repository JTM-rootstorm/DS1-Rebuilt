cmake_minimum_required(VERSION 3.25)
project(ds1_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# dxvk-native (d3d9 only, SDL3 WSI), built from ../tools_ext/dxvk.
set(DXVK_NATIVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools_ext/dxvk-native"
    CACHE PATH "dxvk-native install prefix")

find_package(SDL3 REQUIRED CONFIG)
find_package(Threads REQUIRED)

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

# Guest address space + PE mapping. Shared by the runtime and the lifter.
add_library(ds1_image STATIC
  src/runtime/guest_memory.cpp
  src/runtime/pe_image.cpp)
target_include_directories(ds1_image PUBLIC src)

# --- x87 model (SoftFloat 3e) -----------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/softfloat.cmake" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/x87.cpp")
  include(cmake/softfloat.cmake)  # defines target `softfloat`
  add_library(ds1_x87 STATIC src/runtime/x87.cpp)
  target_include_directories(ds1_x87 PUBLIC src)
  target_link_libraries(ds1_x87 PRIVATE softfloat)
endif()
# ----------------------------------------------------------------------

# Lifter (build-time tool): x86-32 machine code -> C++.
add_subdirectory(tools/lifter)

# The game: runtime + HLE + lifted code (generated/, produced by ds1_lift;
# see CLAUDE.md for the command).
file(GLOB DS1_GENERATED CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp")
if(DS1_GENERATED AND TARGET ds1_x87)
  # Lifted code is always optimised: at -O0 it would be unusably slow and
  # debug info for 100+ MB of generated source is not useful.
  add_library(ds1_lifted OBJECT ${DS1_GENERATED})
  target_include_directories(ds1_lifted PRIVATE src)
  target_compile_options(ds1_lifted PRIVATE -O2 -g0 -fno-strict-aliasing)

  add_executable(ds1
    src/main.cpp
    src/runtime/dispatch.cpp
    src/runtime/process.cpp
    src/runtime/guest_alloc.cpp
    src/runtime/hle/hle.cpp
    src/runtime/hle/kernel32.cpp
    src/runtime/hle/kernel32_file.cpp
    src/runtime/hle/kernel32_sync.cpp
    src/runtime/hle/advapi32.cpp
    src/runtime/hle/user32_input.cpp
    $<TARGET_OBJECTS:ds1_lifted>)
  target_include_directories(ds1 PRIVATE src)
  target_link_libraries(ds1 PRIVATE ds1_image ds1_x87 Threads::Threads)
endif()

# Maps DungeonSiege.exe into the guest arena and reports what the HLE
# layer must provide.
add_executable(ds1_loader_probe src/tools/loader_probe.cpp)
target_link_libraries(ds1_loader_probe PRIVATE ds1_image)

# Proves dxvk-native + SDL3 work on this machine.
if(DS1_DXVK_BACKEND)
  add_executable(d3d9_smoke src/smoke/d3d9_smoke.cpp)
  target_link_libraries(d3d9_smoke PRIVATE dxvk_d3d9 SDL3::SDL3)
  set_target_properties(d3d9_smoke PROPERTIES
    BUILD_RPATH "${DXVK_NATIVE_DIR}/lib64")
endif()

enable_testing()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()
