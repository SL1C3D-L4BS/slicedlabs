# Runtime executables (WO-076, ARCH3-R001): one executable per tools/sl_*.cpp, named by its file stem, linked to
# sl::runtime. When tools/demo/*.cpp exist they form the gate scene library sl_runtime_demo_scene (links sl::runtime only),
# which sl_runtime_demo links. Nothing here links the runtime or fabric test libraries (runtime-lint R-RT-LINK). Later work
# orders only add files. Nobody edits this file again.
file(GLOB _sl_runtime_demo_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/demo/*.cpp")
if(_sl_runtime_demo_sources)
  add_library(sl_runtime_demo_scene STATIC ${_sl_runtime_demo_sources})
  target_link_libraries(sl_runtime_demo_scene PUBLIC sl::runtime)
  sl_target(sl_runtime_demo_scene)
endif()
file(GLOB _sl_runtime_tools CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sl_*.cpp")
foreach(src IN LISTS _sl_runtime_tools)
  get_filename_component(stem "${src}" NAME_WE)
  add_executable(${stem} "${src}")
  target_link_libraries(${stem} PRIVATE sl::runtime)
  if(stem STREQUAL "sl_runtime_demo" AND TARGET sl_runtime_demo_scene)
    target_link_libraries(${stem} PRIVATE sl_runtime_demo_scene)
  endif()
  sl_target(${stem})
endforeach()
