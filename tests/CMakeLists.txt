# Dev-only tests; nothing here ships.

# x87_test runs the host CPU's own x87 (inline asm) next to the model from
# the same state and compares the results, so it can only exist on x86-64.
if(TARGET ds1_x87 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_executable(x87_test x87_test.cpp)
  target_link_libraries(x87_test PRIVATE ds1_x87)
  add_test(NAME x87 COMMAND x87_test)
  set_tests_properties(x87 PROPERTIES TIMEOUT 300)
endif()

# lift_semantics_test checks the integer/flag helpers in lifted.h against
# the host CPU the same way.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_executable(lift_semantics_test lift_semantics_test.cpp)
  target_include_directories(lift_semantics_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_compile_options(lift_semantics_test PRIVATE -O1)
  add_test(NAME lift_semantics COMMAND lift_semantics_test)
  set_tests_properties(lift_semantics PROPERTIES TIMEOUT 300)
endif()

# lift_x87_test runs every x87 instruction form found in the game EXE both
# lifted (on the x87 model) and natively, from the same random states. It
# needs the EXE to generate its inputs: configure with
#   -DDS1_GAME_EXE=".../Dungeon Siege/DungeonSiege.exe"
set(DS1_GAME_EXE "" CACHE FILEPATH "DungeonSiege.exe for EXE-derived tests")
if(TARGET ds1_x87 AND TARGET ds1_lift AND EXISTS "${DS1_GAME_EXE}" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(_forms "${CMAKE_CURRENT_BINARY_DIR}/lift_x87_forms.cpp")
  add_custom_command(OUTPUT "${_forms}"
    COMMAND ds1_lift --exe "${DS1_GAME_EXE}"
            --symbols "${PROJECT_SOURCE_DIR}/symbols"
            --manifest "${PROJECT_SOURCE_DIR}/lifter.manifest"
            --x87-test "${_forms}"
    DEPENDS ds1_lift "${PROJECT_SOURCE_DIR}/lifter.manifest"
    COMMENT "Lifting the EXE's x87 instruction forms for lift_x87_test")
  add_executable(lift_x87_test lift_x87_test.cpp "${_forms}")
  target_include_directories(lift_x87_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lift_x87_test PRIVATE ds1_x87 ds1_image)
  target_compile_options(lift_x87_test PRIVATE -O1 -fno-strict-aliasing)
  add_test(NAME lift_x87 COMMAND lift_x87_test)
  set_tests_properties(lift_x87 PROPERTIES TIMEOUT 600)
endif()

# lift_int_test: the same for every integer instruction form in the EXE.
if(TARGET ds1_lift AND EXISTS "${DS1_GAME_EXE}" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(_iforms "${CMAKE_CURRENT_BINARY_DIR}/lift_int_forms.cpp")
  add_custom_command(OUTPUT "${_iforms}"
    COMMAND ds1_lift --exe "${DS1_GAME_EXE}"
            --symbols "${PROJECT_SOURCE_DIR}/symbols"
            --manifest "${PROJECT_SOURCE_DIR}/lifter.manifest"
            --int-test "${_iforms}"
    DEPENDS ds1_lift "${PROJECT_SOURCE_DIR}/lifter.manifest"
    COMMENT "Lifting the EXE's integer instruction forms for lift_int_test")
  add_executable(lift_int_test lift_int_test.cpp "${_iforms}")
  target_include_directories(lift_int_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lift_int_test PRIVATE ds1_x87 ds1_image)
  target_compile_options(lift_int_test PRIVATE -O1 -fno-strict-aliasing)
  add_test(NAME lift_int COMMAND lift_int_test)
  set_tests_properties(lift_int PROPERTIES TIMEOUT 900)
endif()
