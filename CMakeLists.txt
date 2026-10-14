cmake_minimum_required(VERSION 3.25)
project(ds1_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# x86recomp: the lifter, x87 model and runtime (a sibling checkout).
set(X86RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../x86recomp"
    CACHE PATH "x86recomp source tree")
# Graphics go through x86recomp's SDL_GPU backend (Vulkan, Metal on macOS).
# dxvk-native (d3d9 only, SDL3 WSI), built from ../tools_ext/dxvk, is
# optional: it adds the d3d9 backend (DS1_GFX=d3d9), the reference image.
set(_dxvk_default "")
if(NOT APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tools_ext/dxvk-native")
  set(_dxvk_default "${CMAKE_CURRENT_SOURCE_DIR}/../tools_ext/dxvk-native")
endif()
set(DXVK_NATIVE_DIR "${_dxvk_default}" CACHE PATH "dxvk-native install prefix")

enable_testing()
add_subdirectory("${X86RECOMP_DIR}" x86recomp)

# The game: runtime + HLE + lifted code (generated/, produced by
# recomp_lift; see CLAUDE.md for the command).
recomp_add_lifted(ds1_lifted "${CMAKE_CURRENT_SOURCE_DIR}/generated")
if(TARGET ds1_lifted)
  add_executable(ds1 src/main.cpp src/fov.cpp src/window.cpp src/resolutions.cpp src/ui_scale.cpp $<TARGET_OBJECTS:ds1_lifted>)
  target_link_libraries(ds1 PRIVATE recomp_runtime recomp_mss32 recomp_bink
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
