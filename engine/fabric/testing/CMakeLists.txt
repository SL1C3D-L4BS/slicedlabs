# sl_fabric_testing: helpers shared by the fabric tests (WO-037). Later work orders add testing/*.cpp; testing_base.cpp
# keeps the library non-empty. Nobody edits this file again.
file(GLOB _sl_fabric_testing_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_library(sl_fabric_testing STATIC ${_sl_fabric_testing_sources})
add_library(sl::fabric_testing ALIAS sl_fabric_testing)
# testing/include holds helper headers of later work orders; it may not exist yet.
target_include_directories(sl_fabric_testing PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(sl_fabric_testing PUBLIC sl::fabric)
sl_target(sl_fabric_testing)
