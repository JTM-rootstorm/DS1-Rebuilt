cmake_minimum_required(VERSION 3.25)
project(ds1_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# SwARc's runtime and HLE modules are shared libraries: rebuilding one
# does not relink the game.
set(CMAKE_LINK_DEPENDS_NO_SHARED ON)

# SwARc: the lifter, x87 model and runtime (a sibling checkout).
set(SWARC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../SwARc"
    CACHE PATH "SwARc source tree")
# Graphics go through SwARc's SDL_GPU backend (Vulkan, Metal on macOS).
# dxvk-native (d3d9 only, SDL3 WSI) is opt-in and not built by default:
# configure with -DDXVK_NATIVE_DIR=<its install prefix> (as the Dungeon
# Siege project builds it, ../tools_ext/dxvk-native) to add the d3d9
# reference backend (DS1_GFX=d3d9).
set(DXVK_NATIVE_DIR "" CACHE PATH "dxvk-native install prefix (opt-in d3d9 backend)")

enable_testing()
add_subdirectory("${SWARC_DIR}" swarc)

# The game: runtime + HLE + lifted code (generated/, produced by
# recomp_lift; see CLAUDE.md for the command).
recomp_add_lifted(ds1_lifted "${CMAKE_CURRENT_SOURCE_DIR}/generated")
if(TARGET ds1_lifted)
  add_executable(ds1 src/main.cpp src/fov.cpp src/window.cpp src/resolutions.cpp src/ui_scale.cpp src/cursor.cpp src/crt.cpp $<TARGET_OBJECTS:ds1_lifted>)
  target_link_libraries(ds1 PRIVATE recomp_win32 recomp_mss32 recomp_bink
                        recomp_d3d7 recomp_dbghelp)
  set_target_properties(ds1 PROPERTIES ENABLE_EXPORTS ON)  # symbol names in fault backtraces
  if(DXVK_NATIVE_DIR)
    set_target_properties(ds1 PROPERTIES BUILD_RPATH "${DXVK_NATIVE_DIR}/lib64")
  endif()
endif()

# Oracle tests over every instruction form in the EXE. Configure with
#   -DDS1_GAME_EXE=".../Dungeon Siege/DungeonSiege.exe"
set(DS1_GAME_EXE "" CACHE FILEPATH "DungeonSiege.exe for EXE-derived tests")
recomp_add_lift_tests(EXE "${DS1_GAME_EXE}"
  SYMBOLS "${CMAKE_CURRENT_SOURCE_DIR}/symbols"
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/lifter.manifest")

# The native CRT functions (src/crt.cpp) against the originals, lifted at
# build time (the calls they make outside the CRT are stubs,
# tests/crt_stubs.manifest): tests/crt_test.cpp.
if(EXISTS "${DS1_GAME_EXE}")
  set(_dir "${CMAKE_CURRENT_BINARY_DIR}/crt_oracle")
  set(_gen "${_dir}/gen/gen_000.cpp" "${_dir}/gen/table.cpp")
  set(_manifest "${_dir}/crt_test.manifest")
  add_custom_command(OUTPUT ${_gen}
    COMMAND ${CMAKE_COMMAND} -E cat "${CMAKE_CURRENT_SOURCE_DIR}/lifter.manifest"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/crt_stubs.manifest" > "${_manifest}"
    COMMAND recomp_lift --exe "${DS1_GAME_EXE}"
            --symbols "${CMAKE_CURRENT_SOURCE_DIR}/symbols"
            --manifest "${_manifest}"
            --only 0040C8D0,0040C930,0040D130,00631C10,0040CC70,0040C7E0,0040C7F0,0040DA70,00632090,00410580,0040D7E0,0040D6A0,00631BE0,0040D760,00410610,00410650,00409DAE,00632024,0071EB34,00631E9E,00632049,00631D84,00411C30,0040E0D0,0040E140,00637090,0040D820,00631810,0040C12D,0040F54D,00411680,00411750,006322F0,00631420,00631554,00631688,006316B6,006316DE,00631706,00631734,0040C0B8,0040C19C,0040F5BC,0040E9F9,0040EC1D,0040E23A
            --insns-per-file 1000000 --out "${_dir}/gen"
    DEPENDS recomp_lift "${DS1_GAME_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/lifter.manifest"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/crt_stubs.manifest"
    COMMENT "Lifting the CRT functions for crt_oracle_test")
  add_executable(crt_oracle_test tests/crt_test.cpp tests/crt_native.cpp ${_gen})
  target_include_directories(crt_oracle_test PRIVATE "${SWARC_DIR}/src")
  target_link_libraries(crt_oracle_test PRIVATE recomp_x87 recomp_pe)
  set_source_files_properties(${_gen} PROPERTIES COMPILE_OPTIONS
    "-O2;-g0;-fno-strict-aliasing;-fnon-call-exceptions")
  add_test(NAME crt_oracle COMMAND crt_oracle_test "${DS1_GAME_EXE}")
endif()
