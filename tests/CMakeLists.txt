# Four test tiers (WO-026, SL-EP-051, SL-GOV-009). Helpers: cmake/SlTesting.cmake. See docs/testing/README.md.
add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(system)
add_subdirectory(e2e)

# Golden-file test (WO-027): sl_golden.hpp finds tests/golden through SL_GOLDEN_DIR; the extra label lets `ctest -L golden` pick it.
# Registered here, not in unit/CMakeLists.txt, which WO-029 owns this wave.
sl_add_test(NAME golden_smoke TIER unit SOURCES unit/golden_smoke.cpp LIBS sl::core)
target_compile_definitions(golden_smoke PRIVATE SL_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
set_property(TEST golden_smoke APPEND PROPERTY LABELS golden)

sl_testing_finalize()
