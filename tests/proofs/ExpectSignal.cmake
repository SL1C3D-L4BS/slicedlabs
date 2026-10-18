# cmake -DEXE=<program> -DARG=<mode> -DEXPECT=<exit|signal> -P ExpectSignal.cmake   (WO-025)
#   EXPECT=exit    passes only when the program exits 0
#   EXPECT=signal  passes only when it died of SIGILL or SIGTRAP: a bounds trap is `ud2` (Illegal instruction) or an
#                  int3 (Trace/breakpoint trap). A clean exit, a non-zero exit code or any other signal fails.
# execute_process reports a signal death as the signal's text in RESULT_VARIABLE and an exit code as a number.
if(NOT EXE OR NOT ARG OR NOT EXPECT)
  message(FATAL_ERROR "ExpectSignal: need -DEXE=<program> -DARG=<mode> -DEXPECT=exit|signal")
endif()
execute_process(COMMAND "${EXE}" "${ARG}" RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
if(EXPECT STREQUAL "exit")
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "${EXE} ${ARG}: expected exit 0, got '${result}'")
  endif()
elseif(EXPECT STREQUAL "signal")
  if(NOT result MATCHES "Illegal instruction|Trace/breakpoint trap")
    message(FATAL_ERROR "${EXE} ${ARG}: expected a trap (SIGILL or SIGTRAP), got '${result}'. "
      "Is the hardening flag -fsanitize=bounds -fsanitize-trap=bounds still on the command line?")
  endif()
else()
  message(FATAL_ERROR "ExpectSignal: EXPECT must be exit or signal, got '${EXPECT}'")
endif()
