# Fabric developer tools (WO-037): one executable per tools/*.cpp, named by its file stem, linked to sl::fabric. Later
# work orders only add files. Nobody edits this file again.
file(GLOB _sl_fabric_tools CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
foreach(src IN LISTS _sl_fabric_tools)
  get_filename_component(stem "${src}" NAME_WE)
  add_executable(${stem} "${src}")
  target_link_libraries(${stem} PRIVATE sl::fabric)
  sl_target(${stem})
endforeach()
