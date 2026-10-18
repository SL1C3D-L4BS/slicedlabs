# sl_fabric: the canonical project model of Volume 04 (WO-037, SL-ARCH-001, ADR-0010, ADR-0011). Later work orders only
# add files: sources are globbed from src/ and gen/src/ (schema-generated code, WO-045), headers live in include/ and
# gen/include/. Nobody edits this file again.
file(GLOB _sl_fabric_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gen/src/*.cpp")
add_library(sl_fabric STATIC ${_sl_fabric_sources})
add_library(sl::fabric ALIAS sl_fabric)
# gen/include holds schema-generated headers (WO-045); it may not exist yet, which compilers accept.
target_include_directories(sl_fabric PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/gen/include>")
target_link_libraries(sl_fabric PUBLIC sl::core)
# ADR-0016: the F5 mutation ledger is compiled in Debug and in every sanitizer build.
if(SL_SANITIZE)
  target_compile_definitions(sl_fabric PUBLIC SL_FABRIC_F5_INSTRUMENT=1)
else()
  target_compile_definitions(sl_fabric PUBLIC "$<$<CONFIG:Debug>:SL_FABRIC_F5_INSTRUMENT=1>")
endif()
sl_target(sl_fabric)
sl_provenance(sl_fabric)

add_subdirectory(testing)
add_subdirectory(tools)
