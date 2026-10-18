# Proofs (WO-025): programs that exist to be wrong. Added by the root CMakeLists.txt when SL_PROOFS or SL_HARDEN is ON.
#
# SL_PROOFS=ON  asan_overflow, ubsan_shift and tsan_race, one planted defect each. `just sanitize-proof`
#               (tests/gates/sanitize_proof.sh) builds each under its sanitizer preset and requires the sanitizer's
#               report and a non-zero exit. They are never part of ALL and never registered with ctest: a ctest run
#               would be red by design.
# SL_HARDEN=ON  trap_bounds plus four ctest tests (system tier): the in-bounds modes exit 0, the out-of-bounds modes
#               must die of SIGILL or SIGTRAP (ExpectSignal.cmake). Run by `just hardening` through the ci-release
#               and ci-gcc-release presets.
if(SL_PROOFS)
  foreach(proof IN ITEMS asan_overflow ubsan_shift tsan_race)
    add_executable(${proof} EXCLUDE_FROM_ALL ${proof}.cpp)
    sl_target(${proof})
    if(proof STREQUAL "tsan_race")
      find_package(Threads REQUIRED)
      target_link_libraries(${proof} PRIVATE Threads::Threads)
    endif()
  endforeach()
endif()

if(SL_HARDEN)
  add_executable(trap_bounds trap_bounds.cpp)
  sl_target(trap_bounds)
  foreach(mode IN ITEMS array-in member-in)
    sl_add_script_test(NAME trap_bounds_${mode} TIER system SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/ExpectSignal.cmake"
      ARGS "-DEXE=$<TARGET_FILE:trap_bounds>" "-DARG=${mode}" "-DEXPECT=exit")
  endforeach()
  foreach(mode IN ITEMS array-out member-out)
    sl_add_script_test(NAME trap_bounds_${mode} TIER system SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/ExpectSignal.cmake"
      ARGS "-DEXE=$<TARGET_FILE:trap_bounds>" "-DARG=${mode}" "-DEXPECT=signal")
  endforeach()
endif()
