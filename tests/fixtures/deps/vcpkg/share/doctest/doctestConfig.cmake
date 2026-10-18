# Fake vcpkg port config for doctest (WO-019 fixture): doctest::doctest points at the repository's vendored header.
get_filename_component(_sl_fake_doctest_root "${CMAKE_CURRENT_LIST_DIR}/../../../../../../third_party/doctest" ABSOLUTE)
if(NOT TARGET doctest::doctest)
  add_library(doctest::doctest INTERFACE IMPORTED)
  set_target_properties(doctest::doctest PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_sl_fake_doctest_root}")
endif()
