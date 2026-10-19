# Fabric unit tests (WO-037). Every tests/unit/fabric/<stem>.cpp ending in _test.cpp becomes the doctest executable and
# ctest test fabric_<stem> (tier unit) with the extra labels fabric and fabric_<area>, where <area> is <stem> up to its
# first '_' (id_text_test -> fabric_id): `just fabric-test id` runs that label. Later work orders only add files.
set(_sl_fabric_io_tests manifest_locator_test workspace_lock_test vfs_faulty_test)
file(GLOB _sl_fabric_unit_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
foreach(src IN LISTS _sl_fabric_unit_tests)
  get_filename_component(stem "${src}" NAME_WE)
  string(REGEX REPLACE "_.*$" "" area "${stem}")
  sl_add_test(NAME fabric_${stem} TIER unit SOURCES "${src}" LIBS sl::fabric sl::fabric_testing)
  target_compile_definitions(fabric_${stem} PRIVATE
    SL_FABRIC_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/fabric")
  set_property(TEST fabric_${stem} APPEND PROPERTY LABELS fabric fabric_${area})
  if(SL_SANITIZE)
    set_property(TEST fabric_${stem} APPEND PROPERTY ENVIRONMENT "SL_FABRIC_SWEEP_SCALE=0.1")
  endif()
  # Tests that fsync through vfs_posix are bound by the disk, not the CPU: on btrfs the first fsync after a full
  # compile step waits for the transaction commit of every fresh object file (observed 10.7 to 15.3 s for tests that
  # take 0.01 to 0.09 s alone, WO-063 acceptance runs 3 to 6). They keep the unit tier but get the I/O timeout.
  if(stem IN_LIST _sl_fabric_io_tests)
    set_property(TEST fabric_${stem} PROPERTY TIMEOUT 60)
    set_property(TEST fabric_${stem} APPEND PROPERTY LABELS fabric_io)
  endif()
endforeach()
