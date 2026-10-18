# cpm provider (WO-019, ADR-0002). Content-addressed download through the vendored third_party/cpm/CPM.cmake, never at
# configure time unless SL_OFFLINE is OFF and the cache misses. The package is fetched DOWNLOAD_ONLY (we never run its
# own CMakeLists); OUR target sl_dep_<pkg> is built from the downloaded tree (header-only: the tree is a SYSTEM include).
#
# Pins the provider accepts, in this order:
#   url + url_hash    `url` is the lock member when present, else derived from a github.com `git` + `tag`
#                     (<git minus .git>/archive/refs/tags/<tag>.tar.gz); url_hash is the sha256 of the archive.
#                     A file:// url is hashed here first: a mismatch is FATAL (integrity), it never falls through.
#   git + git_commit  GIT_REPOSITORY + GIT_TAG <commit>.
# Neither pinned: UNAVAILABLE (founder runs `just deps-lock-refresh <pkg>`).
#
# Cache: CPM_SOURCE_CACHE = ${SL_CPM_SOURCE_CACHE} or ${CMAKE_BINARY_DIR}/_cpm_cache; the package lives in
# <cache>/<pkg>/<url_hash|git_commit> (CUSTOM_CACHE_KEY), so a hit is a plain directory test. While SL_OFFLINE is ON
# (default) FETCHCONTENT_FULLY_DISCONNECTED is ON and a cache miss for a non-file:// source is UNAVAILABLE naming the
# cache directory. A file:// source is local and may populate the cache while offline.
# Contract: see the header of cmake/SlDeps.cmake. A FATAL result is an integrity failure and never falls through.
function(sl_deps_provider_cpm pkg pjson out_status out_detail out_hash)
  set(${out_hash} "" PARENT_SCOPE)

  # 1. CPM.cmake itself is hash-pinned in the lock (package `cpm`, vendored provider).
  get_property(lockjson GLOBAL PROPERTY SL_DEPS_LOCK_JSON)
  _sl_deps_jget(cpm_dir "${lockjson}" packages cpm providers vendored dir)
  _sl_deps_jget(cpm_want "${lockjson}" packages cpm providers vendored files CPM.cmake)
  if(NOT cpm_dir OR NOT cpm_want)
    set(${out_status} FATAL PARENT_SCOPE)
    set(${out_detail} "the lock has no sha256 for third_party/cpm/CPM.cmake (package cpm, provider vendored)" PARENT_SCOPE)
    return()
  endif()
  set(cpm_file "${CMAKE_SOURCE_DIR}/${cpm_dir}/CPM.cmake")
  if(NOT EXISTS "${cpm_file}")
    set(${out_status} FATAL PARENT_SCOPE)
    set(${out_detail} "${cpm_dir}/CPM.cmake is locked but missing on disk (provider cpm)" PARENT_SCOPE)
    return()
  endif()
  file(SHA256 "${cpm_file}" cpm_have)
  if(NOT cpm_have STREQUAL cpm_want)
    set(${out_status} FATAL PARENT_SCOPE)
    set(${out_detail} "${cpm_dir}/CPM.cmake sha256 ${cpm_have} does not match the lock (${cpm_want}) (provider cpm)" PARENT_SCOPE)
    return()
  endif()

  # 2. the pin
  _sl_deps_jget(url "${pjson}" providers cpm url)
  _sl_deps_jget(url_hash "${pjson}" providers cpm url_hash)
  _sl_deps_jget(git "${pjson}" providers cpm git)
  _sl_deps_jget(tag "${pjson}" providers cpm tag)
  _sl_deps_jget(git_commit "${pjson}" providers cpm git_commit)
  if(NOT url AND git MATCHES "^https://github\\.com/" AND tag)
    string(REGEX REPLACE "\\.git$" "" git_base "${git}")
    set(url "${git_base}/archive/refs/tags/${tag}.tar.gz")
  endif()
  if(url AND url_hash)
    set(mode url)
    set(key "${url_hash}")
    set(source "${url}")
  elseif(git AND git_commit)
    set(mode git)
    set(key "${git_commit}")
    set(source "${git}")
  else()
    set(${out_status} UNAVAILABLE PARENT_SCOPE)
    set(${out_detail} "cpm provider unpinned (url_hash/git_commit null): founder runs just deps-lock-refresh ${pkg}" PARENT_SCOPE)
    return()
  endif()
  set(is_local FALSE)
  if(source MATCHES "^file://")
    set(is_local TRUE)
  endif()

  # 3. a local archive is hashed before CPM sees it: wrong url_hash is an integrity failure, not a fall-through
  if(mode STREQUAL "url" AND is_local)
    string(SUBSTRING "${url}" 7 -1 local_path)
    if(NOT EXISTS "${local_path}")
      set(${out_status} UNAVAILABLE PARENT_SCOPE)
      set(${out_detail} "${url} does not exist" PARENT_SCOPE)
      return()
    endif()
    file(SHA256 "${local_path}" url_have)
    if(NOT url_have STREQUAL url_hash)
      set(${out_status} FATAL PARENT_SCOPE)
      set(${out_detail} "${local_path} sha256 ${url_have} does not match the lock url_hash (${url_hash}) (provider cpm)" PARENT_SCOPE)
      return()
    endif()
  endif()

  # 4. cache location and offline policy
  if(SL_CPM_SOURCE_CACHE)
    set(cache "${SL_CPM_SOURCE_CACHE}")
  else()
    set(cache "${CMAKE_BINARY_DIR}/_cpm_cache")
  endif()
  string(TOLOWER "${pkg}" lower)
  set(entry "${cache}/${lower}/${key}")
  set(hit FALSE)
  if(IS_DIRECTORY "${entry}")
    file(GLOB entry_files "${entry}/*")
    if(entry_files)
      set(hit TRUE)
    endif()
  endif()
  if(SL_OFFLINE AND NOT hit AND NOT is_local)
    set(${out_status} UNAVAILABLE PARENT_SCOPE)
    set(${out_detail} "cache miss for ${pkg} (${key}) in ${cache} while SL_OFFLINE is ON; seed it (founder: just deps-cpm-smoke) or pass -DSL_CPM_SOURCE_CACHE=<seeded dir>" PARENT_SCOPE)
    return()
  endif()

  # 5. CPM.cmake, once
  set(CPM_SOURCE_CACHE "${cache}" CACHE PATH "CPM source cache (sl_deps)" FORCE)
  set(CPM_USE_LOCAL_PACKAGES OFF CACHE BOOL "sl_deps: never find_package() instead of the pinned source" FORCE)
  set(CPM_DONT_UPDATE_MODULE_PATH ON CACHE BOOL "sl_deps: CPM does not touch CMAKE_MODULE_PATH" FORCE)
  if(NOT COMMAND CPMAddPackage)
    include("${cpm_file}")
  endif()
  if(SL_OFFLINE AND NOT (is_local AND NOT hit))
    set(FETCHCONTENT_FULLY_DISCONNECTED ON)
  else()
    set(FETCHCONTENT_FULLY_DISCONNECTED OFF)
  endif()

  if(mode STREQUAL "url")
    CPMAddPackage(NAME "${pkg}" URL "${url}" URL_HASH "SHA256=${url_hash}" CUSTOM_CACHE_KEY "${key}" DOWNLOAD_ONLY YES)
  else()
    CPMAddPackage(NAME "${pkg}" GIT_REPOSITORY "${git}" GIT_TAG "${git_commit}" CUSTOM_CACHE_KEY "${key}" DOWNLOAD_ONLY YES)
  endif()

  set(src "${${pkg}_SOURCE_DIR}")
  if(NOT src OR NOT IS_DIRECTORY "${src}")
    set(${out_status} UNAVAILABLE PARENT_SCOPE)
    set(${out_detail} "CPM did not populate ${pkg} (cache ${cache}, SL_OFFLINE ${SL_OFFLINE})" PARENT_SCOPE)
    return()
  endif()
  _sl_deps_jget(header "${pjson}" providers system header)
  if(header AND NOT EXISTS "${src}/${header}")
    set(${out_status} UNAVAILABLE PARENT_SCOPE)
    set(${out_detail} "the ${mode} source for ${pkg} has no ${header} (looked in ${src})" PARENT_SCOPE)
    return()
  endif()

  add_library(sl_dep_${pkg} INTERFACE IMPORTED GLOBAL)
  target_include_directories(sl_dep_${pkg} SYSTEM INTERFACE "${src}")
  set(${out_status} OK PARENT_SCOPE)
  set(${out_hash} "${key}" PARENT_SCOPE)
endfunction()
