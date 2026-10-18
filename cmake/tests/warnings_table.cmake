# Table test for sl_warning_flags (cmake/SlWarnings.cmake), run in script mode:
#   cmake -P cmake/tests/warnings_table.cmake
# Optional narrowing: -DSL_TEST_COMPILER_ID=GNU|Clang|MSVC -DSL_TEST_FRONTEND=GNU|MSVC (both together) runs
# one case. The clang-cl probe (informational, exit 0 either way) asks the real clang-cl, when installed, to
# accept the MSVC flag set on an empty translation unit under build/.
#   -DSL_TEST_SKIP_PROBE=1 skips the probe.
cmake_minimum_required(VERSION 3.28)
get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${_root}/cmake/SlWarnings.cmake")

set(_common -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast -Wnon-virtual-dtor
  -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wundef
  -Wextra-semi -Wcast-qual)
set(_clang_only -Wthread-safety -Wcomma -Wloop-analysis -Wunreachable-code-aggressive)
set(_gnu_only -Wduplicated-cond -Wlogical-op -Wbidi-chars=any)
set(_msvc /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc)

set(_failures 0)
set(_cases 0)

# _expect(<label> <id> <frontend> <expected list>): exact list equality (order is part of the contract)
function(_expect label id frontend)
  sl_warning_flags(got "${id}" "${frontend}")
  set(want ${ARGN})
  math(EXPR n "${_cases} + 1")
  set(_cases ${n} PARENT_SCOPE)
  if(got STREQUAL want)
    message(STATUS "ok    ${label}")
  else()
    message(SEND_ERROR "FAIL  ${label}\n  want: ${want}\n  got:  ${got}")
    set(_failures 1 PARENT_SCOPE)
  endif()
endfunction()

# _absent(<label> <id> <frontend> <flag>...): none of the flags may appear
function(_absent label id frontend)
  sl_warning_flags(got "${id}" "${frontend}")
  math(EXPR n "${_cases} + 1")
  set(_cases ${n} PARENT_SCOPE)
  foreach(f IN LISTS ARGN)
    if(f IN_LIST got)
      message(SEND_ERROR "FAIL  ${label}: ${f} must not be in the ${id}/${frontend} list")
      set(_failures 1 PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(STATUS "ok    ${label}")
endfunction()

set(_all_cases
  "GNU|GNU|gcc"
  "Clang|GNU|clang"
  "AppleClang|GNU|appleclang"
  "MSVC|MSVC|cl"
  "Clang|MSVC|clang-cl")
if(DEFINED SL_TEST_COMPILER_ID AND DEFINED SL_TEST_FRONTEND)
  set(_all_cases "${SL_TEST_COMPILER_ID}|${SL_TEST_FRONTEND}|selected")
endif()

foreach(case IN LISTS _all_cases)
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 id)
  list(GET parts 1 fe)
  list(GET parts 2 name)
  if(fe STREQUAL "MSVC")
    _expect("${name} (${id}/${fe}) is the MSVC set" "${id}" "${fe}" ${_msvc})
    _absent("${name} (${id}/${fe}) has no GNU flag" "${id}" "${fe}" -Wall -Wextra ${_clang_only} ${_gnu_only})
  elseif(id STREQUAL "GNU")
    _expect("${name} (${id}/${fe}) is common + gcc-only" "${id}" "${fe}" ${_common} ${_gnu_only})
    _absent("${name} (${id}/${fe}) has no clang-only flag" "${id}" "${fe}" ${_clang_only} /W4)
  elseif(id MATCHES "Clang")
    _expect("${name} (${id}/${fe}) is common + clang-only" "${id}" "${fe}" ${_common} ${_clang_only})
    _absent("${name} (${id}/${fe}) has no gcc-only flag" "${id}" "${fe}" ${_gnu_only} /W4)
  else()
    message(SEND_ERROR "FAIL  unknown SL_TEST_COMPILER_ID '${id}'")
    set(_failures 1)
  endif()
endforeach()

# An unknown compiler yields an empty list (sl_target then adds nothing, never a wrong flag).
if(NOT (DEFINED SL_TEST_COMPILER_ID AND DEFINED SL_TEST_FRONTEND))
  sl_warning_flags(got "Intel" "GNU")
  if(got STREQUAL "")
    message(STATUS "ok    unknown compiler yields no flags")
  else()
    message(SEND_ERROR "FAIL  unknown compiler yielded: ${got}")
    set(_failures 1)
  endif()
  math(EXPR _cases "${_cases} + 1")
endif()

# --- clang-cl flag-acceptance probe (informational) ---
if(NOT SL_TEST_SKIP_PROBE AND NOT (DEFINED SL_TEST_COMPILER_ID AND DEFINED SL_TEST_FRONTEND))
  find_program(_clangcl NAMES clang-cl)
  if(_clangcl)
    set(_dir "${_root}/build/warnings-table")
    file(MAKE_DIRECTORY "${_dir}")
    file(WRITE "${_dir}/empty.cpp" "")
    execute_process(COMMAND "${_clangcl}" /c ${_msvc} "/Fo${_dir}/empty.obj" "${_dir}/empty.cpp"
      RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    string(STRIP "${_out}${_err}" _diag)
    if(_rc EQUAL 0 AND _diag STREQUAL "")
      message(STATUS "clangcl-probe: ${_clangcl} accepts '${_msvc}' on an empty TU (exit 0, no diagnostics)")
    elseif(_rc EQUAL 0)
      message(STATUS "clangcl-probe: ${_clangcl} accepts '${_msvc}' on an empty TU (exit 0) with diagnostics:\n${_diag}")
    else()
      message(STATUS "clangcl-probe: ${_clangcl} rejected the MSVC flag set (exit ${_rc}); informational\n${_out}${_err}")
    endif()
  else()
    message(STATUS "clangcl-probe: clang-cl not installed; skipped (informational)")
  endif()
endif()

if(_failures)
  message(FATAL_ERROR "warnings-table: FAIL")
endif()
message(STATUS "warnings-table: ok (${_cases} checks)")
