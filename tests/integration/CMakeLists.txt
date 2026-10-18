# Integration tier: doctest tests that link several engine libraries in one process, 60 s default timeout.
sl_add_test(NAME sandbox_selfcheck TIER integration SOURCES sandbox_selfcheck.cpp LIBS sl::core)
