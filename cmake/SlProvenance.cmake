# Build provenance records (WO-029, F3 seed of SL-GOV-006, SL-EP-055). One record per built artifact:
#   build/<preset>/provenance/<target>.prov.json   schema sl-provenance/1 (schemas/sl-provenance.schema.json)
#   {target, artifact{path,sha256}, inputs[{path,sha256}], toolchain, parameters, environment_hash, built_at}
#
#   sl_provenance(<target>)   call after the target's sources, include directories, link libraries and compile
#                             options are final. Adds a POST_BUILD step that runs this same file as a
#                             `cmake -P` script (no Python at build time): it hashes the artifact and every input
#                             (the target's sources plus the headers of its own and its dependencies' include
#                             directories that live in the repository; build-tree files are derived, not inputs)
#                             and writes the record. The record is rewritten whenever the target is relinked.
#
# environment_hash is the sha256 of the canonical JSON {"parameters":{...},"toolchain":{...}} (keys sorted, no
# spaces); built_at is the one non-deterministic field and is not part of it. tools/provenance/check.py
# recomputes the hash and every file hash (provenance-check).
include_guard(GLOBAL)

# ----------------------------------------------------------------------------------------------------------------
# script mode: cmake -D... -P cmake/SlProvenance.cmake
# ----------------------------------------------------------------------------------------------------------------
function(_sl_prov_json out value)
  string(REPLACE "\\" "\\\\" v "${value}")
  string(REPLACE "\"" "\\\"" v "${v}")
  set(${out} "\"${v}\"" PARENT_SCOPE)
endfunction()

function(_sl_prov_write)
  foreach(var IN ITEMS TARGET TARGET_FILE SOURCE_DIR BINARY_DIR SOURCES INCLUDE_DIRS COMPILER COMPILER_VERSION
      CMAKE_VER LINKER LOCK_SHA256 PRESET BUILD_TYPE STD SANITIZE WERROR FLAGS_SHA256 OUT)
    if(NOT DEFINED SLP_${var})
      message(FATAL_ERROR "sliced: provenance: -DSLP_${var}=... is missing")
    endif()
  endforeach()
  if(NOT EXISTS "${SLP_TARGET_FILE}")
    message(FATAL_ERROR "sliced: provenance: artifact ${SLP_TARGET_FILE} does not exist")
  endif()

  # inputs: sources, then every header found in the include directories; repo-relative, sorted, unique
  set(files "")
  string(REPLACE "|" ";" sources "${SLP_SOURCES}")
  string(REPLACE "|" ";" incdirs "${SLP_INCLUDE_DIRS}")
  foreach(f IN LISTS sources)
    list(APPEND files "${f}")
  endforeach()
  foreach(d IN LISTS incdirs)
    file(GLOB_RECURSE found LIST_DIRECTORIES false "${d}/*.h" "${d}/*.hh" "${d}/*.hpp" "${d}/*.hxx" "${d}/*.inl")
    list(APPEND files ${found})
  endforeach()
  set(rels "")
  foreach(f IN LISTS files)
    file(RELATIVE_PATH rel "${SLP_SOURCE_DIR}" "${f}")
    list(APPEND rels "${rel}")
  endforeach()
  list(REMOVE_DUPLICATES rels)
  list(SORT rels)
  set(input_items "")
  foreach(rel IN LISTS rels)
    if(NOT EXISTS "${SLP_SOURCE_DIR}/${rel}")
      message(FATAL_ERROR "sliced: provenance: input ${rel} of ${SLP_TARGET} does not exist")
    endif()
    file(SHA256 "${SLP_SOURCE_DIR}/${rel}" h)
    _sl_prov_json(p "${rel}")
    list(APPEND input_items "    {\"path\": ${p}, \"sha256\": \"${h}\"}")
  endforeach()
  list(JOIN input_items ",\n" inputs_json)

  file(SHA256 "${SLP_TARGET_FILE}" artifact_sha)
  file(RELATIVE_PATH artifact_rel "${SLP_BINARY_DIR}" "${SLP_TARGET_FILE}")

  # canonical environment JSON: keys sorted, no spaces (python: json.dumps(sort_keys=True, separators=(",", ":")))
  foreach(var IN ITEMS CMAKE_VER COMPILER COMPILER_VERSION LINKER LOCK_SHA256 BUILD_TYPE FLAGS_SHA256 PRESET SANITIZE)
    _sl_prov_json(J_${var} "${SLP_${var}}")
  endforeach()
  set(tc "{\"cmake\":${J_CMAKE_VER},\"compiler\":${J_COMPILER},\"compiler_version\":${J_COMPILER_VERSION},\"linker\":${J_LINKER},\"lock_sha256\":${J_LOCK_SHA256}}")
  set(pa "{\"build_type\":${J_BUILD_TYPE},\"flags_sha256\":${J_FLAGS_SHA256},\"preset\":${J_PRESET},\"sanitize\":${J_SANITIZE},\"std\":${SLP_STD},\"warnings_as_errors\":${SLP_WERROR}}")
  string(SHA256 env_hash "{\"parameters\":${pa},\"toolchain\":${tc}}")

  string(TIMESTAMP built_at "%Y-%m-%dT%H:%M:%SZ" UTC)
  _sl_prov_json(J_TARGET "${SLP_TARGET}")
  _sl_prov_json(J_ARTIFACT "${artifact_rel}")
  set(json "{
  \"schema\": \"sl-provenance/1\",
  \"target\": ${J_TARGET},
  \"artifact\": {\"path\": ${J_ARTIFACT}, \"sha256\": \"${artifact_sha}\"},
  \"inputs\": [
${inputs_json}
  ],
  \"toolchain\": {
    \"compiler\": ${J_COMPILER}, \"compiler_version\": ${J_COMPILER_VERSION}, \"cmake\": ${J_CMAKE_VER},
    \"linker\": ${J_LINKER}, \"lock_sha256\": ${J_LOCK_SHA256}
  },
  \"parameters\": {
    \"preset\": ${J_PRESET}, \"build_type\": ${J_BUILD_TYPE}, \"std\": ${SLP_STD}, \"sanitize\": ${J_SANITIZE},
    \"warnings_as_errors\": ${SLP_WERROR}, \"flags_sha256\": ${J_FLAGS_SHA256}
  },
  \"environment_hash\": \"${env_hash}\",
  \"built_at\": \"${built_at}\"
}
")
  get_filename_component(out_dir "${SLP_OUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${out_dir}")
  file(WRITE "${SLP_OUT}.tmp" "${json}")
  file(RENAME "${SLP_OUT}.tmp" "${SLP_OUT}")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  _sl_prov_write()
  return()
endif()

# ----------------------------------------------------------------------------------------------------------------
# configure mode
# ----------------------------------------------------------------------------------------------------------------

# true when path is inside the source tree and outside the build tree
function(_sl_prov_in_repo out path)
  set(${out} FALSE PARENT_SCOPE)
  string(FIND "${path}" "${CMAKE_BINARY_DIR}/" in_build)
  string(FIND "${path}" "${CMAKE_SOURCE_DIR}/" in_src)
  if(in_build EQUAL 0 OR NOT in_src EQUAL 0)
    return()
  endif()
  set(${out} TRUE PARENT_SCOPE)
endfunction()

# strip generator expressions that only decorate a path; leave "" for anything still conditional
function(_sl_prov_plain out item)
  string(REGEX REPLACE "\\$<BUILD_INTERFACE:([^>]*)>" "\\1" v "${item}")
  string(REGEX REPLACE "\\$<LINK_ONLY:([^>]*)>" "\\1" v "${v}")
  if(v MATCHES "\\$<")
    set(v "")
  endif()
  set(${out} "${v}" PARENT_SCOPE)
endfunction()

# the include directories of <tgt> and of every target reachable through its link libraries (repository paths only)
function(_sl_prov_include_dirs out tgt)
  set(todo "${tgt}")
  set(seen "")
  set(dirs "")
  while(todo)
    list(POP_FRONT todo t)
    get_target_property(aliased ${t} ALIASED_TARGET)
    if(aliased)
      set(t "${aliased}")
    endif()
    if(t IN_LIST seen)
      continue()
    endif()
    list(APPEND seen "${t}")
    set(props INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES INTERFACE_LINK_LIBRARIES)
    if(t STREQUAL tgt)
      list(APPEND props INCLUDE_DIRECTORIES LINK_LIBRARIES)
    endif()
    foreach(prop IN LISTS props)
      get_target_property(values ${t} ${prop})
      if(NOT values)
        continue()
      endif()
      foreach(item IN LISTS values)
        _sl_prov_plain(item "${item}")
        if(item STREQUAL "")
          continue()
        endif()
        if(prop MATCHES "LINK_LIBRARIES$")
          if(TARGET ${item})
            list(APPEND todo "${item}")
          endif()
        else()
          _sl_prov_in_repo(ok "${item}")
          if(ok)
            list(APPEND dirs "${item}")
          endif()
        endif()
      endforeach()
    endforeach()
  endwhile()
  list(REMOVE_DUPLICATES dirs)
  set(${out} "${dirs}" PARENT_SCOPE)
endfunction()

function(sl_provenance tgt)
  if(NOT TARGET ${tgt})
    message(FATAL_ERROR "sliced: sl_provenance(${tgt}): no such target")
  endif()
  get_target_property(type ${tgt} TYPE)
  if(NOT type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|EXECUTABLE)$")
    message(FATAL_ERROR "sliced: sl_provenance(${tgt}): only built artifacts carry provenance (target type ${type})")
  endif()

  # sources (absolute, repository files only: generated sources live in the build tree)
  get_target_property(src_dir ${tgt} SOURCE_DIR)
  get_target_property(raw_sources ${tgt} SOURCES)
  set(sources "")
  foreach(s IN LISTS raw_sources)
    if(s MATCHES "\\$<")
      continue()
    endif()
    cmake_path(ABSOLUTE_PATH s BASE_DIRECTORY "${src_dir}" NORMALIZE OUTPUT_VARIABLE abs)
    _sl_prov_in_repo(ok "${abs}")
    if(ok)
      list(APPEND sources "${abs}")
    endif()
  endforeach()
  _sl_prov_include_dirs(incdirs ${tgt})

  # parameters and toolchain (the same values SlBuildInfo records)
  set(build_type "${CMAKE_BUILD_TYPE}")
  if(NOT build_type)
    set(build_type "multi-config")
  endif()
  string(TOUPPER "${CMAKE_BUILD_TYPE}" cfg)
  set(sanitize "${SL_SANITIZE}")
  if(NOT sanitize)
    set(sanitize "none")
  endif()
  set(preset "${SL_PRESET}")
  if(NOT preset)
    set(preset "none")
  endif()
  if(SL_WARNINGS_AS_ERRORS)
    set(werror true)
  else()
    set(werror false)
  endif()
  get_property(lock_sha GLOBAL PROPERTY SL_DEPS_LOCK_SHA256)
  if(NOT lock_sha)
    set(lock_sha "none")
  endif()
  get_target_property(copts ${tgt} COMPILE_OPTIONS)
  get_target_property(cdefs ${tgt} COMPILE_DEFINITIONS)
  string(SHA256 flags_sha "cxx_flags=${CMAKE_CXX_FLAGS}\ncxx_flags_config=${CMAKE_CXX_FLAGS_${cfg}}\ncompile_options=${copts}\ncompile_definitions=${cdefs}\n")

  set(out "${CMAKE_BINARY_DIR}/provenance/${tgt}.prov.json")
  list(JOIN sources "|" sources_arg)
  list(JOIN incdirs "|" incdirs_arg)
  add_custom_command(TARGET ${tgt} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
      "-DSLP_TARGET=${tgt}" "-DSLP_TARGET_FILE=$<TARGET_FILE:${tgt}>"
      "-DSLP_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DSLP_BINARY_DIR=${CMAKE_BINARY_DIR}"
      "-DSLP_SOURCES=${sources_arg}" "-DSLP_INCLUDE_DIRS=${incdirs_arg}"
      "-DSLP_COMPILER=${CMAKE_CXX_COMPILER_ID}" "-DSLP_COMPILER_VERSION=${CMAKE_CXX_COMPILER_VERSION}"
      "-DSLP_CMAKE_VER=${CMAKE_VERSION}" "-DSLP_LINKER=${SL_LINKER_USED}" "-DSLP_LOCK_SHA256=${lock_sha}"
      "-DSLP_PRESET=${preset}" "-DSLP_BUILD_TYPE=${build_type}" "-DSLP_STD=${SL_CXX_STANDARD}"
      "-DSLP_SANITIZE=${sanitize}" "-DSLP_WERROR=${werror}" "-DSLP_FLAGS_SHA256=${flags_sha}"
      "-DSLP_OUT=${out}"
      -P "${CMAKE_SOURCE_DIR}/cmake/SlProvenance.cmake"
    COMMENT "provenance: ${tgt}"
    VERBATIM)
endfunction()
