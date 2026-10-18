# Fabric integration tests (WO-037). Every tests/integration/fabric/<stem>.cpp ending in _test.cpp becomes the doctest
# executable and ctest test fabric_<stem> (tier integration) with the extra labels fabric and fabric_<area>, where
# <area> is <stem> up to its first '_' (tx_recover_test -> fabric_tx). Later work orders only add files.
file(GLOB _sl_fabric_integration_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
foreach(src IN LISTS _sl_fabric_integration_tests)
  get_filename_component(stem "${src}" NAME_WE)
  string(REGEX REPLACE "_.*$" "" area "${stem}")
  sl_add_test(NAME fabric_${stem} TIER integration SOURCES "${src}" LIBS sl::fabric sl::fabric_testing)
  target_compile_definitions(fabric_${stem} PRIVATE
    SL_FABRIC_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/fabric")
  set_property(TEST fabric_${stem} APPEND PROPERTY LABELS fabric fabric_${area})
  if(SL_SANITIZE)
    set_property(TEST fabric_${stem} APPEND PROPERTY ENVIRONMENT "SL_FABRIC_SWEEP_SCALE=0.1")
  endif()
endforeach()
