# e2e: configure, build and run examples/sandbox from a copy of the tree, offline, with the debug preset.
#   cmake -DSL_SOURCE_DIR=<repo> -DSL_E2E_DIR=<repo>/build/e2e -P tests/e2e/sandbox_build.cmake
# The copy lives in <SL_E2E_DIR>/tree; the original tree is never modified. SL_E2E_DIR must be under a build/ directory.
cmake_minimum_required(VERSION 3.28)

foreach(var SL_SOURCE_DIR SL_E2E_DIR)
  if(NOT ${var})
    message(FATAL_ERROR "e2e: -D${var}=... is required")
  endif()
endforeach()
if(NOT SL_E2E_DIR MATCHES "/build/")
  message(FATAL_ERROR "e2e: SL_E2E_DIR '${SL_E2E_DIR}' is not under a build/ directory; refusing to delete it")
endif()

function(_e2e_run description)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "e2e: ${description} failed (exit ${rc}): ${ARGN}\n${out}")
  endif()
  set(E2E_OUTPUT "${out}" PARENT_SCOPE)
  message(STATUS "e2e: ${description}: ok")
endfunction()

set(tree "${SL_E2E_DIR}/tree")
file(REMOVE_RECURSE "${tree}")
file(MAKE_DIRECTORY "${tree}")
foreach(item CMakeLists.txt CMakePresets.json cmake deps third_party engine examples tests)
  if(EXISTS "${SL_SOURCE_DIR}/${item}")
    file(COPY "${SL_SOURCE_DIR}/${item}" DESTINATION "${tree}")
  endif()
endforeach()

_e2e_run("configure (debug preset)" "${CMAKE_COMMAND}" --preset debug -S "${tree}")
_e2e_run("build sl_sandbox" "${CMAKE_COMMAND}" --build "${tree}/build/debug" --target sl_sandbox)

file(GLOB_RECURSE sandbox LIST_DIRECTORIES false "${tree}/build/debug/sl_sandbox")
if(NOT sandbox)
  message(FATAL_ERROR "e2e: sl_sandbox was not built under ${tree}/build/debug")
endif()
list(GET sandbox 0 sandbox)

_e2e_run("sl_sandbox --self-check" "${sandbox}" --self-check)
_e2e_run("sl_sandbox banner" "${sandbox}")
if(NOT E2E_OUTPUT MATCHES "preset +debug")
  message(FATAL_ERROR "e2e: the copy was not built with the debug preset:\n${E2E_OUTPUT}")
endif()
message(STATUS "e2e: sandbox_build ok")
