# Configure-time build provenance (F3 field set). Writes, under ${CMAKE_BINARY_DIR} (= build/<preset>):
#   generated/sl/build_info.hpp   constexpr strings for sl_core (target sl_build_info carries the include dir)
#   build-info.json               {inputs, toolchain, parameters, environment_hash, provenance_dir}
#                                 (provenance_dir: where sl_provenance() records land, cmake/SlProvenance.cmake)
# Files are rewritten only when their content changes, so a re-configure does not rebuild.

function(_sl_json_str out value)
  string(REPLACE "\\" "\\\\" v "${value}")
  string(REPLACE "\"" "\\\"" v "${v}")
  set(${out} "\"${v}\"" PARENT_SCOPE)
endfunction()

function(_sl_write_if_different path content)
  set(old "")
  if(EXISTS "${path}")
    file(READ "${path}" old)
  endif()
  if(NOT old STREQUAL content)
    file(WRITE "${path}" "${content}")
  endif()
endfunction()

function(_sl_source_revision out_rev out_describe)
  set(rev "unversioned")
  set(describe "unversioned")
  if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
    find_package(Git QUIET)
    if(GIT_FOUND)
      execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE r OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE rc)
      if(rc EQUAL 0 AND r)
        set(rev "${r}")
      endif()
      execute_process(COMMAND "${GIT_EXECUTABLE}" describe --always --tags --dirty
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE d OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE rc)
      if(rc EQUAL 0 AND d)
        set(describe "${d}")
      endif()
    endif()
  endif()
  set(${out_rev} "${rev}" PARENT_SCOPE)
  set(${out_describe} "${describe}" PARENT_SCOPE)
endfunction()

function(sl_build_info_generate)
  _sl_source_revision(source_rev describe)
  set(preset "${SL_PRESET}")
  if(NOT preset)
    set(preset "none")
  endif()
  set(build_type "${CMAKE_BUILD_TYPE}")
  if(NOT build_type)
    set(build_type "multi-config")
  endif()
  set(sanitize "${SL_SANITIZE}")
  if(NOT sanitize)
    set(sanitize "none")
  endif()
  if(SL_WARNINGS_AS_ERRORS)
    set(werror true)
  else()
    set(werror false)
  endif()

  # header
  set(SL_BI_COMPILER "${CMAKE_CXX_COMPILER_ID}")
  set(SL_BI_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
  set(SL_BI_STD "${SL_CXX_STANDARD}")
  set(SL_BI_BUILD_TYPE "${build_type}")
  set(SL_BI_SANITIZE "${sanitize}")
  set(SL_BI_PRESET "${preset}")
  set(SL_BI_LINKER "${SL_LINKER_USED}")
  set(SL_BI_REVISION "${describe}")
  set(gen_dir "${CMAKE_BINARY_DIR}/generated")
  configure_file("${CMAKE_SOURCE_DIR}/cmake/templates/build_info.hpp.in"
    "${gen_dir}/sl/build_info.hpp" @ONLY)

  # environment hash: sha256 of the sorted toolchain + parameters text
  set(lines
    "toolchain.compiler=${CMAKE_CXX_COMPILER_ID}"
    "toolchain.version=${CMAKE_CXX_COMPILER_VERSION}"
    "toolchain.cmake=${CMAKE_VERSION}"
    "toolchain.generator=${CMAKE_GENERATOR}"
    "toolchain.linker=${SL_LINKER_USED}"
    "parameters.preset=${preset}"
    "parameters.build_type=${build_type}"
    "parameters.sanitize=${sanitize}"
    "parameters.warnings_as_errors=${werror}"
    "parameters.std=${SL_CXX_STANDARD}")
  list(SORT lines)
  list(JOIN lines "\n" env_text)
  string(SHA256 env_hash "${env_text}\n")

  set(j "{}")
  foreach(v IN ITEMS inputs toolchain parameters)
    string(JSON j SET "${j}" ${v} "{}")
  endforeach()
  _sl_json_str(v "${source_rev}")
  string(JSON j SET "${j}" inputs source_rev "${v}")
  get_property(_sl_lock_sha GLOBAL PROPERTY SL_DEPS_LOCK_SHA256)
  if(_sl_lock_sha)
    _sl_json_str(v "${_sl_lock_sha}")
    string(JSON j SET "${j}" inputs lock_sha256 "${v}")
  else()
    string(JSON j SET "${j}" inputs lock_sha256 null)
  endif()
  foreach(kv IN ITEMS
      "compiler|${CMAKE_CXX_COMPILER_ID}" "version|${CMAKE_CXX_COMPILER_VERSION}" "cmake|${CMAKE_VERSION}"
      "generator|${CMAKE_GENERATOR}" "linker|${SL_LINKER_USED}")
    string(REPLACE "|" ";" kv "${kv}")
    list(GET kv 0 k)
    list(GET kv 1 val)
    _sl_json_str(v "${val}")
    string(JSON j SET "${j}" toolchain ${k} "${v}")
  endforeach()
  foreach(kv IN ITEMS
      "preset|${preset}" "build_type|${build_type}" "sanitize|${sanitize}")
    string(REPLACE "|" ";" kv "${kv}")
    list(GET kv 0 k)
    list(GET kv 1 val)
    _sl_json_str(v "${val}")
    string(JSON j SET "${j}" parameters ${k} "${v}")
  endforeach()
  string(JSON j SET "${j}" parameters warnings_as_errors ${werror})
  string(JSON j SET "${j}" parameters std ${SL_CXX_STANDARD})
  string(JSON j SET "${j}" environment_hash "\"${env_hash}\"")
  file(RELATIVE_PATH _sl_prov_rel "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/provenance")
  _sl_json_str(v "${_sl_prov_rel}")
  string(JSON j SET "${j}" provenance_dir "${v}")
  _sl_write_if_different("${CMAKE_BINARY_DIR}/build-info.json" "${j}\n")

  message(STATUS "sliced: build-info.json ${preset} ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
    "env ${env_hash}")
endfunction()

sl_build_info_generate()

add_library(sl_build_info INTERFACE)
target_include_directories(sl_build_info INTERFACE "${CMAKE_BINARY_DIR}/generated")
