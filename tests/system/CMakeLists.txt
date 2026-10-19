# System tier: run a built binary as a separate process, 120 s default timeout.
# --self-check prints nothing, so it passes on exit 0; the plain run is matched against its banner.
sl_add_system_test(NAME sandbox_self_check_run COMMAND sl_sandbox ARGS --self-check)
sl_add_system_test(NAME sandbox_banner COMMAND sl_sandbox
  PASS_REGEX "sl_sandbox[ \n]+compiler +[^\n]+\n +standard +C\\+\\+(23|26)\n")
# WO-062: the Volume 04 end-to-end demo (create, impact, undo, commit, reopen, FP4 crash, recover,
# F2/F5 counters) in a work directory of the build tree; the binary empties it on each run.
sl_add_system_test(NAME fabric_demo COMMAND sl_fabric_cli
  ARGS demo ${CMAKE_CURRENT_BINARY_DIR}/fabric-demo
  PASS_REGEX "fabric-demo: ok")
