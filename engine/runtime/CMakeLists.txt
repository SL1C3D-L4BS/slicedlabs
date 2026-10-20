# sl_runtime: the native runtime of Volume 05 (WO-076, SL-ARCH-003 ARCH3-R001/R002/R012, ADR-0022). Written once: later
# work orders only add files. Sources are globbed from src/, public headers live in include/sl/runtime/. Links only the
# standard library, sl::core and sl::fabric. Nobody edits this file again.
file(GLOB _sl_runtime_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(sl_runtime STATIC ${_sl_runtime_sources})
add_library(sl::runtime ALIAS sl_runtime)
target_include_directories(sl_runtime PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(sl_runtime PUBLIC sl::core sl::fabric)
# ARCH3-R012 (ADR-0026): no floating-point contraction in the library or in the systems consumers instantiate from its
# headers.
target_compile_options(sl_runtime
  INTERFACE $<$<CXX_COMPILER_ID:GNU,Clang>:-ffp-contract=off>
  PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-ffp-contract=off>)
# ARCH3-R020 (ADR-0022): failpoints RFP0-RFP10 are compiled in Debug and in every sanitizer build, out of Release.
# Always defined (0 or 1) so -Wundef holds.
if(SL_SANITIZE)
  target_compile_definitions(sl_runtime PUBLIC SL_RUNTIME_FAULTS=1)
else()
  target_compile_definitions(sl_runtime PUBLIC "SL_RUNTIME_FAULTS=$<IF:$<CONFIG:Debug>,1,0>")
endif()
sl_target(sl_runtime)
sl_provenance(sl_runtime)

add_subdirectory(testing)
add_subdirectory(tools)
