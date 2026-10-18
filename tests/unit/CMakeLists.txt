# Unit tier: doctest, no process spawning, 10 s default timeout. sl_add_test() (cmake/SlTesting.cmake) resolves doctest
# through sl_require() and generates the main.
# WO-030: log_test, error_test, assert_test, log_fixture (SL_LOG_JSON_PATH: JSON log of the run, `just log-schema-check`).
sl_add_test(NAME sl_tests TIER unit
  SOURCES smoke.cpp build_info_test.cpp log_test.cpp error_test.cpp assert_test.cpp log_fixture.cpp
  LIBS sl::core)
target_compile_definitions(sl_tests PRIVATE SL_EXPECTED_CXX_STANDARD="${SL_CXX_STANDARD}")
sl_provenance(sl_tests)
# Fabric unit tests (WO-037): one executable per tests/unit/fabric/*_test.cpp.
add_subdirectory(fabric)
