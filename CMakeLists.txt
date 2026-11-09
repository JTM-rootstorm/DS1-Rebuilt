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
  add_executable(ds1 src/main.cpp src/fov.cpp src/world_frustum.cpp src/window.cpp src/resolutions.cpp src/ui_scale.cpp src/cursor.cpp $<TARGET_OBJECTS:ds1_lifted>)
  target_link_libraries(ds1 PRIVATE recomp_win32 recomp_mss32 recomp_bink
                        recomp_d3d7 recomp_dbghelp recomp_msvc_crt)
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

# SwARc's native CRT functions (lifter.manifest's `native` lines) against
# the EXE's originals, lifted at build time (the calls they make outside
# the CRT are stubs: tests/crt_stubs.manifest, tests/crt_stubs.cpp).
recomp_add_msvc_crt_oracle(EXE "${DS1_GAME_EXE}"
  SYMBOLS "${CMAKE_CURRENT_SOURCE_DIR}/symbols"
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/lifter.manifest"
  STUBS "${CMAKE_CURRENT_SOURCE_DIR}/tests/crt_stubs.manifest"
  SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/crt_stubs.cpp"
  # _CIfmod's dispatch table (0x791E30): the fmod core and its special cases.
  ENTRIES 6326E4 635084 6350BD 6350BF 6350E7 635126)
