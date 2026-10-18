# Dependency resolver (WO-018, ADR-0002, SL-GOV-008). One lockfile, four providers, no network at configure.
#
#   sl_deps_init()        read deps/deps.lock.json ONCE (file(READ) + string(JSON)), hash it, choose the provider order,
#                         start build/<preset>/deps-resolved.json. Sets SL_DEPS_LOCK_SHA256 (cache, internal) and the
#                         global properties SL_DEPS_LOCK_JSON, SL_DEPS_LOCK_SHA256, SL_DEPS_PROVIDERS_EFFECTIVE.
#   sl_require(<pkg>)     resolve one locked package; the winner is aliased to the lock's `target` (sl::doctest).
#
#   SL_DEPS_PROVIDERS     cache list, empty = policy default from the lock: provider_order.ci when SL_WARNINGS_AS_ERRORS is
#                         ON (vendored;cpm;system;vcpkg), else provider_order.dev (system;vendored;cpm;vcpkg). The
#                         environment variable of the same name is read when the cache entry is empty.
#   SL_OFFLINE            ON (default): the cpm provider never downloads; a cache miss for a remote source is UNAVAILABLE.
#   SL_CPM_SOURCE_CACHE   cpm provider cache directory; empty = <binary dir>/_cpm_cache.
#   SL_DEPS_LOCK_OVERRIDE test hook (tests/gates/deps_provider_test.sh): another lock file, e.g. one whose cpm url is a
#                         file:// fixture. Warns when used and is refused in ci (SL_WARNINGS_AS_ERRORS=ON).
#
# Provider contract (cmake/deps/provider_<name>.cmake): a function
#     sl_deps_provider_<name>(<pkg> <pkg_lock_json> <out_status> <out_detail> <out_hash>)
# sets <out_status> to OK, UNAVAILABLE or FATAL in the caller. OK: it has created the IMPORTED INTERFACE target
# sl_dep_<pkg>, <out_hash> is the content hash of what it resolved. UNAVAILABLE: <out_detail> is the reason and the next
# provider is tried. FATAL: an integrity failure (hash mismatch, a locked file gone); <out_detail> names the file, and the
# configure stops: a FATAL result never falls through to another provider.
include_guard(GLOBAL)

set(SL_DEPS_PROVIDERS "" CACHE STRING "Provider order for sl_require(): empty = lock policy (ci or dev); else a subset of vendored;system;cpm;vcpkg")
option(SL_OFFLINE "cpm provider: FETCHCONTENT_FULLY_DISCONNECTED; a cache miss for a remote source is UNAVAILABLE (default ON)" ON)
set(SL_CPM_SOURCE_CACHE "" CACHE PATH "cpm provider: CPM_SOURCE_CACHE; empty = <binary dir>/_cpm_cache")
set(SL_DEPS_LOCK_OVERRIDE "" CACHE FILEPATH "TEST HOOK (WO-019): read this lock file instead of deps/deps.lock.json; refused when SL_WARNINGS_AS_ERRORS is ON")

foreach(_sl_prov IN ITEMS vendored system cpm vcpkg)
  include("${CMAKE_CURRENT_LIST_DIR}/deps/provider_${_sl_prov}.cmake")
endforeach()

function(_sl_deps_jstr out value)
  string(REPLACE "\\" "\\\\" v "${value}")
  string(REPLACE "\"" "\\\"" v "${v}")
  set(${out} "\"${v}\"" PARENT_SCOPE)
endfunction()

# _sl_deps_jget(<out> <json> <member...>): the value as a string, or empty when the path is absent or null.
function(_sl_deps_jget out json)
  string(JSON t ERROR_VARIABLE e TYPE "${json}" ${ARGN})
  if(e OR t STREQUAL "NULL")
    set(${out} "" PARENT_SCOPE)
    return()
  endif()
  string(JSON v GET "${json}" ${ARGN})
  set(${out} "${v}" PARENT_SCOPE)
endfunction()

# _sl_deps_jmembers(<out> <json> <member...>): the object's member names as a list (empty for a non-object).
function(_sl_deps_jmembers out json)
  set(names "")
  string(JSON t ERROR_VARIABLE e TYPE "${json}" ${ARGN})
  if(NOT e AND t STREQUAL "OBJECT")
    string(JSON obj GET "${json}" ${ARGN})
    string(JSON n LENGTH "${obj}")
    if(n GREATER 0)
      math(EXPR last "${n} - 1")
      foreach(i RANGE ${last})
        string(JSON name MEMBER "${obj}" ${i})
        list(APPEND names "${name}")
      endforeach()
    endif()
  endif()
  set(${out} "${names}" PARENT_SCOPE)
endfunction()

function(sl_deps_init)
  set(lock "${CMAKE_SOURCE_DIR}/deps/deps.lock.json")
  if(SL_DEPS_LOCK_OVERRIDE)
    if(SL_WARNINGS_AS_ERRORS)
      message(FATAL_ERROR "sliced: deps: SL_DEPS_LOCK_OVERRIDE is a test hook and is refused when SL_WARNINGS_AS_ERRORS is ON (ci)")
    endif()
    set(lock "${SL_DEPS_LOCK_OVERRIDE}")
    message(WARNING "sliced: deps: reading the lock from SL_DEPS_LOCK_OVERRIDE=${lock}, not deps/deps.lock.json (test hook)")
  endif()
  if(NOT EXISTS "${lock}")
    message(FATAL_ERROR "sliced: deps: ${lock} is missing; run `just deps-lock`")
  endif()
  # The lock is read once; a change to it re-runs the configure.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${lock}")
  file(READ "${lock}" json)
  file(SHA256 "${lock}" lock_sha)
  string(JSON schema ERROR_VARIABLE err GET "${json}" schema)
  if(err OR NOT schema STREQUAL "sl-deps-lock/1")
    message(FATAL_ERROR "sliced: deps: ${lock} is not an sl-deps-lock/1 file (${err}${schema})")
  endif()

  set(order "${SL_DEPS_PROVIDERS}")
  if(order STREQUAL "" AND DEFINED ENV{SL_DEPS_PROVIDERS})
    set(order "$ENV{SL_DEPS_PROVIDERS}")
  endif()
  if(order STREQUAL "")
    if(SL_WARNINGS_AS_ERRORS)
      set(mode ci)
    else()
      set(mode dev)
    endif()
    string(JSON n LENGTH "${json}" policy provider_order ${mode})
    math(EXPR last "${n} - 1")
    foreach(i RANGE ${last})
      string(JSON p GET "${json}" policy provider_order ${mode} ${i})
      list(APPEND order "${p}")
    endforeach()
  endif()
  foreach(p IN LISTS order)
    if(NOT COMMAND sl_deps_provider_${p})
      message(FATAL_ERROR "sliced: deps: SL_DEPS_PROVIDERS names '${p}', which is not a provider "
        "(vendored, system, cpm, vcpkg)")
    endif()
  endforeach()

  set_property(GLOBAL PROPERTY SL_DEPS_LOCK_JSON "${json}")
  set_property(GLOBAL PROPERTY SL_DEPS_LOCK_SHA256 "${lock_sha}")
  set_property(GLOBAL PROPERTY SL_DEPS_PROVIDERS_EFFECTIVE "${order}")
  set(SL_DEPS_LOCK_SHA256 "${lock_sha}" CACHE INTERNAL "sha256 of deps/deps.lock.json (set by sl_deps_init)")

  # build/<preset>/deps-resolved.json restarts empty on every configure; sl_require() appends.
  _sl_deps_jstr(js "${lock_sha}")
  set(doc "{\"schema\":\"sl-deps-resolved/1\",\"lock_sha256\":${js},\"providers\":[],\"resolved\":[]}")
  set(i 0)
  foreach(p IN LISTS order)
    string(JSON doc SET "${doc}" providers ${i} "\"${p}\"")
    math(EXPR i "${i} + 1")
  endforeach()
  set_property(GLOBAL PROPERTY SL_DEPS_RESOLVED_JSON "${doc}")
  file(WRITE "${CMAKE_BINARY_DIR}/deps-resolved.json" "${doc}\n")
  message(STATUS "sliced: deps lock ${lock_sha} providers ${order}")
endfunction()

function(_sl_deps_record pkg provider version hash)
  get_property(doc GLOBAL PROPERTY SL_DEPS_RESOLVED_JSON)
  get_property(lock_sha GLOBAL PROPERTY SL_DEPS_LOCK_SHA256)
  set(entry "{}")
  foreach(kv IN ITEMS "package|${pkg}" "provider|${provider}" "hash|${hash}" "lock_sha256|${lock_sha}")
    string(REPLACE "|" ";" kv "${kv}")
    list(GET kv 0 k)
    list(GET kv 1 val)
    _sl_deps_jstr(js "${val}")
    string(JSON entry SET "${entry}" ${k} "${js}")
  endforeach()
  if(version STREQUAL "")
    string(JSON entry SET "${entry}" version null)
  else()
    _sl_deps_jstr(js "${version}")
    string(JSON entry SET "${entry}" version "${js}")
  endif()
  string(JSON n LENGTH "${doc}" resolved)
  string(JSON doc SET "${doc}" resolved ${n} "${entry}")
  set_property(GLOBAL PROPERTY SL_DEPS_RESOLVED_JSON "${doc}")
  file(WRITE "${CMAKE_BINARY_DIR}/deps-resolved.json" "${doc}\n")
endfunction()

# sl_require(<pkg>): walk SL_DEPS_PROVIDERS (intersected with the providers the lock declares for <pkg>) until one
# returns OK. FATAL stops at once; when every provider is UNAVAILABLE the error lists each with its reason.
function(sl_require pkg)
  get_property(json GLOBAL PROPERTY SL_DEPS_LOCK_JSON)
  if(NOT json)
    message(FATAL_ERROR "sliced: deps: sl_require(${pkg}) was called before sl_deps_init()")
  endif()
  get_property(resolved GLOBAL PROPERTY SL_DEP_RESOLVED_${pkg})
  if(resolved)
    return()
  endif()
  string(JSON pjson ERROR_VARIABLE err GET "${json}" packages "${pkg}")
  if(err)
    message(FATAL_ERROR "sliced: deps: '${pkg}' has no entry in deps/deps.lock.json (missing manifest entry: add it to "
      "deps/deps.json, then `just deps-lock`)")
  endif()
  _sl_deps_jmembers(declared "${pjson}" providers)
  get_property(order GLOBAL PROPERTY SL_DEPS_PROVIDERS_EFFECTIVE)

  set(chain "")
  foreach(p IN LISTS order)
    if(p IN_LIST declared)
      list(APPEND chain "${p}")
    endif()
  endforeach()
  if(NOT chain)
    message(FATAL_ERROR "sliced: deps: no provider can resolve '${pkg}'. SL_DEPS_PROVIDERS is '${order}', the lock "
      "declares '${declared}' for it.\nFix (founder): pass -DSL_DEPS_PROVIDERS=<one of ${declared}> or add a provider "
      "in deps/deps.json, then `just deps-lock`.")
  endif()

  set(tried "")
  foreach(p IN LISTS chain)
    set(status "")
    set(detail "")
    set(hash "")
    cmake_language(CALL sl_deps_provider_${p} "${pkg}" "${pjson}" status detail hash)
    if(status STREQUAL "OK")
      _sl_deps_jget(version "${pjson}" version)
      _sl_deps_jget(target "${pjson}" target)
      if(target AND NOT TARGET "${target}")
        add_library("${target}" ALIAS "sl_dep_${pkg}")
      endif()
      set_property(GLOBAL PROPERTY SL_DEP_RESOLVED_${pkg} "${p}")
      _sl_deps_record("${pkg}" "${p}" "${version}" "${hash}")
      message(STATUS "sliced: deps ${pkg} ${version} via ${p} (${hash})")
      return()
    elseif(status STREQUAL "FATAL")
      message(FATAL_ERROR "sliced: deps: integrity failure resolving '${pkg}' via ${p}: ${detail}\n"
        "An integrity failure never falls through to another provider.\n"
        "Fix (founder): restore the file from git, or re-vendor and re-lock with `just deps-vendor ${pkg}`, then "
        "`just deps-check`.")
    elseif(status STREQUAL "UNAVAILABLE")
      string(APPEND tried "\n  - ${p}: ${detail}")
    else()
      message(FATAL_ERROR "sliced: deps: provider ${p} returned '${status}' for '${pkg}' (expected OK, UNAVAILABLE or FATAL)")
    endif()
  endforeach()
  message(FATAL_ERROR "sliced: deps: cannot resolve '${pkg}'. Providers tried, in order:${tried}\n"
    "Fix (founder): install the system package (`sudo pacman -Syu --needed <pkg>`, name in deps/deps.lock.json), or "
    "vendor it offline (`just deps-vendor ${pkg}`), or widen -DSL_DEPS_PROVIDERS; then re-run the configure.")
endfunction()
