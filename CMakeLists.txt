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
find_package(PkgConfig REQUIRED)
# ffmpeg (LGPL, linked dynamically): MP3 for Miles streams, Bink movies.
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat libavcodec libavutil libswscale)
set(DS1_LLVM_ROOT "/usr/lib/llvm/22" CACHE PATH
    "LLVM install with include/llvm/Demangle and lib64/libLLVMDemangle.a")

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
  # Every x87 instruction of the game runs through these: always optimise
  # (an -O0 debug build made float-heavy scenes several times slower).
  # No stack protector: its canary check sat in every hot x87 helper.
  target_compile_options(ds1_x87 PRIVATE -O2 -fno-stack-protector)
  target_compile_options(softfloat PRIVATE -O2)
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
  # -fnon-call-exceptions: guest access violations are raised from the
  # SIGSEGV handler and may unwind (GuestResume) out of the faulting frame.
  target_compile_options(ds1_lifted PRIVATE -O2 -g0 -fno-strict-aliasing
                         -fnon-call-exceptions)

  add_executable(ds1
    src/main.cpp
    src/runtime/dispatch.cpp
    src/runtime/process.cpp
    src/runtime/watchdog.cpp
    src/runtime/guest_alloc.cpp
    src/runtime/hle/hle.cpp
    src/runtime/hle/kernel32.cpp
    src/runtime/hle/kernel32_file.cpp
    src/runtime/hle/kernel32_sync.cpp
    src/runtime/hle/advapi32.cpp
    src/runtime/hle/user32_input.cpp
    src/runtime/hle/user32_window.cpp
    src/runtime/hle/gdi32.cpp
    src/runtime/hle/kernel32_seh.cpp
    src/runtime/hle/kernel32_misc.cpp
    src/runtime/hle/shell_net.cpp
    src/runtime/hle/dbghelp.cpp
    src/runtime/hle/mss32.cpp
    src/runtime/audio/source.cpp
    src/runtime/audio/mixer.cpp
    src/runtime/hle/binkw32.cpp
    src/runtime/hle/com.cpp
    src/runtime/hle/ddraw.cpp
    src/runtime/hle/d3d7.cpp
    src/runtime/gfx/format.cpp
    src/runtime/gfx/d3d9_device.cpp
    $<TARGET_OBJECTS:ds1_lifted>)
  target_include_directories(ds1 PRIVATE src)
  target_link_libraries(ds1 PRIVATE ds1_image ds1_x87 SDL3::SDL3
                        Threads::Threads PkgConfig::FFMPEG)
  # LLVM's Microsoft demangler backs dbghelp!UnDecorateSymbolName (FuBi).
  target_include_directories(ds1 PRIVATE "${DS1_LLVM_ROOT}/include")
  target_link_libraries(ds1 PRIVATE "${DS1_LLVM_ROOT}/lib64/libLLVMDemangle.a")
  # Per-pixel and per-sample host paths: optimised in every build type.
  set_source_files_properties(src/runtime/audio/mixer.cpp
    src/runtime/audio/source.cpp src/runtime/gfx/format.cpp
    PROPERTIES COMPILE_OPTIONS "-O2")
  # Only the d3d9 backend sees dxvk's Windows/D3D9 headers.
  set_source_files_properties(src/runtime/gfx/d3d9_device.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${DXVK_NATIVE_DIR}/include/dxvk"
    COMPILE_DEFINITIONS "DXVK_WSI_SDL3")
  target_link_libraries(ds1 PRIVATE
    "${DXVK_NATIVE_DIR}/lib64/libdxvk_d3d9.so")
  set_target_properties(ds1 PROPERTIES BUILD_RPATH "${DXVK_NATIVE_DIR}/lib64"
    ENABLE_EXPORTS ON)  # symbol names in fault backtraces
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
