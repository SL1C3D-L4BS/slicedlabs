# Language standard, warnings, sanitizers, linker and compiler cache policy (ADR-0005, .claude/rules/cpp.md).
#   SL_CXX_STANDARD          23 (default) or 26
#   SL_SANITIZE              "", address, undefined or thread (address and thread together are refused)
#   SL_WARNINGS_AS_ERRORS    ON in the ci presets: CMAKE_COMPILE_WARNING_AS_ERROR and CMAKE_LINK_WARNING_AS_ERROR
#   SL_LINKER                auto (mold, else lld), mold, lld or default (CMake 3.29+; ignored below it)
#   SL_CCACHE                AUTO (use ccache when found), ON (required) or OFF
# A flag the policy needs is never dropped quietly: a required flag the compiler lacks stops the configure,
# an optional warning the compiler lacks is named in the configure output.
include(CheckCXXCompilerFlag)

set(SL_CXX_STANDARD "23" CACHE STRING "C++ standard: 23 (baseline) or 26 (opt-in preset)")
set_property(CACHE SL_CXX_STANDARD PROPERTY STRINGS 23 26)
set(SL_SANITIZE "" CACHE STRING "Sanitizer: empty, address, undefined or thread")
set_property(CACHE SL_SANITIZE PROPERTY STRINGS "" address undefined thread)
option(SL_WARNINGS_AS_ERRORS "Treat compile and link warnings as errors (ci presets)" OFF)
set(SL_LINKER "auto" CACHE STRING "Linker: auto, mold, lld or default")
set_property(CACHE SL_LINKER PROPERTY STRINGS auto mold lld default)
set(SL_CCACHE "AUTO" CACHE STRING "ccache launcher: AUTO, ON or OFF")
set_property(CACHE SL_CCACHE PROPERTY STRINGS AUTO ON OFF)
set(SL_PRESET "" CACHE STRING "Configure preset name (set by CMakePresets.json)")

if(NOT SL_CXX_STANDARD MATCHES "^(23|26)$")
  message(FATAL_ERROR "sliced: SL_CXX_STANDARD must be 23 or 26, got '${SL_CXX_STANDARD}'")
endif()
if(SL_SANITIZE MATCHES "address" AND SL_SANITIZE MATCHES "thread")
  message(FATAL_ERROR "sliced: SL_SANITIZE cannot combine address and thread (got '${SL_SANITIZE}')")
endif()
if(NOT SL_SANITIZE MATCHES "^(|address|undefined|thread)$")
  message(FATAL_ERROR "sliced: SL_SANITIZE must be empty, address, undefined or thread, got '${SL_SANITIZE}'")
endif()

set(_sl_msvc_frontend OFF)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(_sl_msvc_frontend ON)
endif()

# Probe a flag once per configure; the result is cached under a name made from the flag.
function(_sl_probe flag out)
  string(MAKE_C_IDENTIFIER "SL_HAS_${flag}" id)
  if(flag MATCHES "^-fsanitize=")
    set(CMAKE_REQUIRED_LINK_OPTIONS "${flag}")
  endif()
  check_cxx_compiler_flag("${flag}" ${id})
  set(${out} ${${id}} PARENT_SCOPE)
endfunction()

# _sl_add(LIST_VAR REQUIRED|OPTIONAL flag...): append the supported flags; a missing REQUIRED flag stops the
# configure, a missing OPTIONAL flag is named.
function(_sl_add var mode)
  set(list ${${var}})
  foreach(flag IN LISTS ARGN)
    _sl_probe("${flag}" ok)
    if(ok)
      list(APPEND list "${flag}")
    elseif(mode STREQUAL "REQUIRED")
      message(FATAL_ERROR "sliced: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} does not support ${flag}, "
        "which the build policy requires")
    else()
      message(STATUS "sliced: ${flag} skipped (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} lacks it)")
    endif()
  endforeach()
  set(${var} "${list}" PARENT_SCOPE)
endfunction()

# --- warnings (table in SlWarnings.cmake, included by the root before the first sl_target call) ---
include(SlWarnings)
sl_warning_flags(_sl_warnings "${CMAKE_CXX_COMPILER_ID}" "${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}")
set(SL_WARNING_FLAGS "")
_sl_add(SL_WARNING_FLAGS OPTIONAL ${_sl_warnings})

# --- sanitizers ---
set(SL_SANITIZE_COMPILE "")
set(SL_SANITIZE_LINK "")
if(SL_SANITIZE)
  if(_sl_msvc_frontend)
    if(NOT SL_SANITIZE STREQUAL "address")
      message(FATAL_ERROR "sliced: SL_SANITIZE=${SL_SANITIZE} is not available with the MSVC front end (address only)")
    endif()
    _sl_add(SL_SANITIZE_COMPILE REQUIRED /fsanitize=address)
  else()
    _sl_add(SL_SANITIZE_COMPILE REQUIRED -fsanitize=${SL_SANITIZE})
    set(SL_SANITIZE_LINK -fsanitize=${SL_SANITIZE})
    list(APPEND SL_SANITIZE_COMPILE -fno-omit-frame-pointer -g)
    if(SL_SANITIZE STREQUAL "undefined")
      _sl_add(SL_SANITIZE_COMPILE REQUIRED -fno-sanitize-recover=all)
    endif()
  endif()
endif()

# --- Debug hardening (GNU front end) ---
set(SL_DEBUG_FLAGS "")
if(NOT _sl_msvc_frontend)
  list(APPEND SL_DEBUG_FLAGS -D_GLIBCXX_ASSERTIONS)
  _sl_add(SL_DEBUG_FLAGS REQUIRED -ftrivial-auto-var-init=pattern)
endif()

# --- warnings as errors: CMake's own switches, never a hand-rolled -Werror ---
if(SL_WARNINGS_AS_ERRORS)
  set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
  set(CMAKE_LINK_WARNING_AS_ERROR ON) # honoured by CMake 4.0+, ignored below
endif()

# --- linker (CMAKE_LINKER_TYPE needs CMake 3.29) ---
set(SL_LINKER_USED "default")
if(NOT SL_LINKER STREQUAL "default" AND NOT _sl_msvc_frontend)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    find_program(SL_MOLD_PROGRAM NAMES mold)
    find_program(SL_LLD_PROGRAM NAMES ld.lld lld)
    if(SL_LINKER MATCHES "^(auto|mold)$" AND SL_MOLD_PROGRAM)
      set(CMAKE_LINKER_TYPE MOLD)
      set(SL_LINKER_USED "mold")
    elseif(SL_LINKER MATCHES "^(auto|lld)$" AND SL_LLD_PROGRAM)
      set(CMAKE_LINKER_TYPE LLD)
      set(SL_LINKER_USED "lld")
    elseif(NOT SL_LINKER STREQUAL "auto")
      message(FATAL_ERROR "sliced: SL_LINKER=${SL_LINKER} requested but the linker was not found")
    endif()
  else()
    message(STATUS "sliced: SL_LINKER needs CMake 3.29 (have ${CMAKE_VERSION}); using the default linker")
  endif()
endif()
message(STATUS "sliced: linker ${SL_LINKER_USED}")

# --- compiler cache ---
string(TOUPPER "${SL_CCACHE}" _sl_ccache_mode)
set(SL_CCACHE_USED "none")
if(NOT _sl_ccache_mode STREQUAL "OFF")
  find_program(SL_CCACHE_PROGRAM NAMES ccache)
  if(SL_CCACHE_PROGRAM)
    # A cache directory that cannot be written (a jail, a read-only home) would fail every compile: probe once.
    set(_sl_probe_src "${CMAKE_BINARY_DIR}/CMakeFiles/sl_ccache_probe.cpp")
    file(WRITE "${_sl_probe_src}" "\n")
    execute_process(COMMAND "${SL_CCACHE_PROGRAM}" "${CMAKE_CXX_COMPILER}" -x c++ -c "${_sl_probe_src}"
      -o "${CMAKE_BINARY_DIR}/CMakeFiles/sl_ccache_probe.o"
      RESULT_VARIABLE _sl_ccache_rc OUTPUT_QUIET ERROR_VARIABLE _sl_ccache_err)
    if(_sl_ccache_rc EQUAL 0)
      set(CMAKE_CXX_COMPILER_LAUNCHER "${SL_CCACHE_PROGRAM}")
      set(SL_CCACHE_USED "ccache")
    elseif(_sl_ccache_mode STREQUAL "ON")
      message(FATAL_ERROR "sliced: SL_CCACHE=ON but ccache is not usable: ${_sl_ccache_err}")
    else()
      string(STRIP "${_sl_ccache_err}" _sl_ccache_err)
      message(STATUS "sliced: ccache found but not usable (${_sl_ccache_err}); compiling without it")
    endif()
  elseif(_sl_ccache_mode STREQUAL "ON")
    message(FATAL_ERROR "sliced: SL_CCACHE=ON but ccache was not found")
  endif()
endif()
message(STATUS "sliced: compiler cache ${SL_CCACHE_USED}")

# sl_target(<tgt>): the one place a target takes the language standard, warnings, sanitizer and Debug policy.
function(sl_target tgt)
  set_target_properties(${tgt} PROPERTIES
    CXX_STANDARD ${SL_CXX_STANDARD} CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_compile_options(${tgt} PRIVATE ${SL_WARNING_FLAGS} ${SL_SANITIZE_COMPILE})
  foreach(flag IN LISTS SL_DEBUG_FLAGS)
    target_compile_options(${tgt} PRIVATE "$<$<CONFIG:Debug>:${flag}>")
  endforeach()
  if(SL_SANITIZE_LINK)
    target_link_options(${tgt} PRIVATE ${SL_SANITIZE_LINK})
  endif()
endfunction()
