# Fabric unit tests (WO-037). Every tests/unit/fabric/<stem>.cpp ending in _test.cpp becomes the doctest executable and
# ctest test fabric_<stem> (tier unit) with the extra labels fabric and fabric_<area>, where <area> is <stem> up to its
# first '_' (id_text_test -> fabric_id): `just fabric-test id` runs that label. Later work orders only add files.
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
endforeach()
