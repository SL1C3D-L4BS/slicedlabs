# sl_runtime_testing: helpers shared by the runtime tests (WO-076, ARCH3-R001). The only runtime target that links
# sl::fabric_testing. Later work orders add testing/*.cpp; testing_base.cpp keeps the library non-empty. Tests include
# its headers by path (engine/runtime/testing/<name>.hpp). Nobody edits this file again.
file(GLOB _sl_runtime_testing_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_library(sl_runtime_testing STATIC ${_sl_runtime_testing_sources})
add_library(sl::runtime_testing ALIAS sl_runtime_testing)
target_link_libraries(sl_runtime_testing PUBLIC sl::runtime sl::fabric_testing)
sl_target(sl_runtime_testing)
