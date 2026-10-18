# sl_sandbox: the smallest executable linking sl_core; the smoke target and the first ctest test (WO-014).
add_executable(sl_sandbox main.cpp)
target_link_libraries(sl_sandbox PRIVATE sl::core)
sl_target(sl_sandbox)
sl_provenance(sl_sandbox)

add_test(NAME sandbox_self_check COMMAND sl_sandbox --self-check)
