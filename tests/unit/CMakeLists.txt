# sl_tests: doctest unit tests (WO-018). doctest comes from sl_require() (vendored, system, cpm or vcpkg by policy).
sl_require(doctest)

add_executable(sl_tests smoke.cpp)
target_link_libraries(sl_tests PRIVATE sl::doctest sl::core)
sl_target(sl_tests)

add_test(NAME sl_tests COMMAND sl_tests)
