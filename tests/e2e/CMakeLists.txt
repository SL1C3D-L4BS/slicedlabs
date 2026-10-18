# E2E tier: 300 s default timeout. sandbox_build copies the tree to build/e2e, configures it with the debug preset
# (CMake and the local toolchain only, no network), builds sl_sandbox and runs it.
sl_add_script_test(NAME sandbox_build TIER e2e SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/sandbox_build.cmake"
  ARGS "-DSL_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DSL_E2E_DIR=${CMAKE_BINARY_DIR}/e2e")
