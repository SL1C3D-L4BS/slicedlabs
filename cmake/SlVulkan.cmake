# Vulkan detection (WO-016, SL-EP-045). Nothing here opens a VkInstance or VkDevice (the verify jail has no
# /dev/dri); the facts come from headers, the loader file, the layer manifest and `<tool> -v`.
#   SL_VK_MIN_HEADER   minimum VK_HEADER_VERSION, compile-probed (default 350)
#   SL_VK_LAYER_DIRS   directories searched for VkLayer_khronos_validation.json (the negative test points this
#                      at an empty directory)
#   SL_VK_VALIDATION   validation layer wanted: ON in Debug, OFF otherwise. When it is ON and the manifest is
#                      missing, the configure stops with the pacman line if SL_WARNINGS_AS_ERRORS (ci) is ON
#                      on Linux, and warns otherwise (the effective setting then becomes 0).
#   SL_TOOL_SLANGC / SL_TOOL_SPIRV_VAL   optional tools (find_program cache variables)
# Results: INTERFACE target sl_vulkan_config (alias sl::vulkan_config; links Vulkan::Headers; defines
# SL_VK_VALIDATION=0|1 and SL_VK_HEADER_VERSION=<n>) and build/<preset>/vulkan-info.json.
# SlBuildInfo.cmake reads nothing from this module, so the report is a separate file (seam for a later episode).

set(SL_VK_MIN_HEADER "350" CACHE STRING "Minimum VK_HEADER_VERSION of the installed Vulkan headers")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_sl_vk_default_dirs "/usr/share/vulkan/explicit_layer.d;/etc/vulkan/explicit_layer.d")
else()
  set(_sl_vk_default_dirs "")
endif()
set(SL_VK_LAYER_DIRS "${_sl_vk_default_dirs}" CACHE STRING "Directories searched for VkLayer_khronos_validation.json")
set(SL_VK_SLANGC_MIN "2026.18" CACHE STRING "Minimum slangc version (optional tool)")

# --- pure helpers (script-mode tested in cmake/tests/vulkan_detect.cmake) ---

# _sl_vk_parse_version(<out> <text>): first dotted number in text ("v2026.18.2" -> 2026.18.2), else empty
function(_sl_vk_parse_version out text)
  set(v "")
  if(text MATCHES "([0-9]+(\\.[0-9]+)+)")
    set(v "${CMAKE_MATCH_1}")
  endif()
  set(${out} "${v}" PARENT_SCOPE)
endfunction()

# _sl_vk_layer_api_version(<out> <manifest json text>): layer.api_version, or empty when absent or malformed
function(_sl_vk_layer_api_version out json)
  set(v "")
  string(JSON got ERROR_VARIABLE err GET "${json}" layer api_version)
  if(NOT err AND NOT got MATCHES "NOTFOUND$")
    set(v "${got}")
  endif()
  set(${out} "${v}" PARENT_SCOPE)
endfunction()

# _sl_vk_header_version_from_text(<out> <header text>): the number in `#define VK_HEADER_VERSION <n>`, else empty
function(_sl_vk_header_version_from_text out text)
  set(v "")
  if(text MATCHES "#define[ \t]+VK_HEADER_VERSION[ \t]+([0-9]+)")
    set(v "${CMAKE_MATCH_1}")
  endif()
  set(${out} "${v}" PARENT_SCOPE)
endfunction()

# _sl_vk_json_str(<out> <value>): JSON string literal, or null for an empty value
function(_sl_vk_json_str out value)
  if(value STREQUAL "")
    set(${out} "null" PARENT_SCOPE)
    return()
  endif()
  string(REPLACE "\\" "\\\\" v "${value}")
  string(REPLACE "\"" "\\\"" v "${v}")
  set(${out} "\"${v}\"" PARENT_SCOPE)
endfunction()

function(_sl_vk_write_if_different path content)
  set(old "")
  if(EXISTS "${path}")
    file(READ "${path}" old)
  endif()
  if(NOT old STREQUAL content)
    file(WRITE "${path}" "${content}")
  endif()
endfunction()

# --- detection ---

function(sl_vulkan_detect)
  # 1. headers and loader
  find_package(Vulkan REQUIRED)
  set(fv "${Vulkan_VERSION}")
  if(NOT fv)
    set(fv "n/a")
  endif()
  message(STATUS "sliced: vulkan FindVulkan version ${fv}, include ${Vulkan_INCLUDE_DIR}")

  set(loader_found false)
  if(Vulkan_LIBRARY)
    set(loader_found true)
    message(STATUS "sliced: vulkan loader ${Vulkan_LIBRARY}")
  else()
    message(WARNING "sliced: vulkan loader library not found")
  endif()

  # 2. VK_HEADER_VERSION >= SL_VK_MIN_HEADER, compile-probed; the exact value is read from the header text
  set(probe_src "${CMAKE_BINARY_DIR}/CMakeFiles/sl_vk_header_probe.cpp")
  file(WRITE "${probe_src}"
    "#include <vulkan/vulkan_core.h>\n"
    "#if VK_HEADER_VERSION < ${SL_VK_MIN_HEADER}\n"
    "#error \"VK_HEADER_VERSION is below ${SL_VK_MIN_HEADER}\"\n"
    "#endif\n"
    "int main() { return 0; }\n")
  set(probe_var "SL_VK_HEADERS_GE_${SL_VK_MIN_HEADER}")
  try_compile(${probe_var} SOURCES "${probe_src}" LINK_LIBRARIES Vulkan::Headers
    OUTPUT_VARIABLE probe_out)
  if(NOT ${probe_var})
    message(FATAL_ERROR "sliced: Vulkan headers at ${Vulkan_INCLUDE_DIR} are older than "
      "VK_HEADER_VERSION ${SL_VK_MIN_HEADER} (SL_VK_MIN_HEADER)\n${probe_out}")
  endif()
  set(header_version "")
  if(Vulkan_INCLUDE_DIR AND EXISTS "${Vulkan_INCLUDE_DIR}/vulkan/vulkan_core.h")
    file(READ "${Vulkan_INCLUDE_DIR}/vulkan/vulkan_core.h" core_text)
    _sl_vk_header_version_from_text(header_version "${core_text}")
  endif()
  if(NOT header_version)
    message(FATAL_ERROR "sliced: cannot read VK_HEADER_VERSION from ${Vulkan_INCLUDE_DIR}/vulkan/vulkan_core.h")
  endif()
  message(STATUS "sliced: vulkan headers VK_HEADER_VERSION ${header_version} (min ${SL_VK_MIN_HEADER})")

  # 3. validation layer manifest
  set(manifest "")
  set(layer_api "")
  foreach(dir IN LISTS SL_VK_LAYER_DIRS)
    if(EXISTS "${dir}/VkLayer_khronos_validation.json")
      set(manifest "${dir}/VkLayer_khronos_validation.json")
      break()
    endif()
  endforeach()
  if(manifest)
    file(READ "${manifest}" manifest_text)
    _sl_vk_layer_api_version(layer_api "${manifest_text}")
    message(STATUS "sliced: vulkan validation layer ${manifest} api_version ${layer_api}")
  else()
    message(STATUS "sliced: vulkan validation layer manifest not found in: ${SL_VK_LAYER_DIRS}")
  endif()

  # 4. SL_VK_VALIDATION: ON by default in Debug; the effective value needs the manifest
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(validation_default ON)
  else()
    set(validation_default OFF)
  endif()
  set(SL_VK_VALIDATION "${validation_default}" CACHE BOOL "Enable the Khronos validation layer (default ON in Debug)")
  set(validation_effective 0)
  if(SL_VK_VALIDATION)
    if(manifest)
      set(validation_effective 1)
    elseif(SL_WARNINGS_AS_ERRORS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
      message(FATAL_ERROR "sliced: the Vulkan validation layer is required (SL_VK_VALIDATION=ON, warnings as "
        "errors) but VkLayer_khronos_validation.json was not found in: ${SL_VK_LAYER_DIRS}\n"
        "Install it with:\n"
        "  sudo pacman -S vulkan-validation-layers")
    else()
      message(WARNING "sliced: SL_VK_VALIDATION=ON but VkLayer_khronos_validation.json was not found in "
        "${SL_VK_LAYER_DIRS}; validation is off (sudo pacman -S vulkan-validation-layers)")
    endif()
  endif()
  message(STATUS "sliced: vulkan validation ${validation_effective} (SL_VK_VALIDATION=${SL_VK_VALIDATION})")

  # 5. optional tools
  find_program(SL_TOOL_SLANGC NAMES slangc)
  set(slangc_version "")
  if(SL_TOOL_SLANGC)
    execute_process(COMMAND "${SL_TOOL_SLANGC}" -v OUTPUT_VARIABLE so ERROR_VARIABLE se
      RESULT_VARIABLE src OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
    _sl_vk_parse_version(slangc_version "${so} ${se}")
    if(slangc_version AND slangc_version VERSION_GREATER_EQUAL SL_VK_SLANGC_MIN)
      message(STATUS "sliced: slangc ${slangc_version} at ${SL_TOOL_SLANGC} (min ${SL_VK_SLANGC_MIN})")
    elseif(slangc_version)
      message(STATUS "sliced: slangc ${slangc_version} at ${SL_TOOL_SLANGC} is below ${SL_VK_SLANGC_MIN} (optional)")
    else()
      message(STATUS "sliced: slangc at ${SL_TOOL_SLANGC} reported no version (optional)")
    endif()
  else()
    message(STATUS "sliced: slangc not found (optional)")
  endif()
  find_program(SL_TOOL_SPIRV_VAL NAMES spirv-val)
  if(SL_TOOL_SPIRV_VAL)
    message(STATUS "sliced: spirv-val ${SL_TOOL_SPIRV_VAL}")
  else()
    message(STATUS "sliced: spirv-val not found (optional; sudo pacman -S spirv-tools)")
  endif()

  # 6. the config target (sl_core does not link it yet)
  if(NOT TARGET sl_vulkan_config)
    add_library(sl_vulkan_config INTERFACE)
    add_library(sl::vulkan_config ALIAS sl_vulkan_config)
  endif()
  target_link_libraries(sl_vulkan_config INTERFACE Vulkan::Headers)
  target_compile_definitions(sl_vulkan_config INTERFACE
    SL_VK_VALIDATION=${validation_effective} SL_VK_HEADER_VERSION=${header_version})

  # 7. report: build/<preset>/vulkan-info.json
  set(j "{}")
  string(JSON j SET "${j}" headers_version ${header_version})
  string(JSON j SET "${j}" min_header ${SL_VK_MIN_HEADER})
  _sl_vk_json_str(v "${Vulkan_VERSION}")
  string(JSON j SET "${j}" find_vulkan_version "${v}")
  string(JSON j SET "${j}" loader_found ${loader_found})
  _sl_vk_json_str(v "${Vulkan_LIBRARY}")
  string(JSON j SET "${j}" loader "${v}")
  _sl_vk_json_str(v "${manifest}")
  string(JSON j SET "${j}" layer_manifest "${v}")
  _sl_vk_json_str(v "${layer_api}")
  string(JSON j SET "${j}" layer_api_version "${v}")
  if(validation_effective)
    string(JSON j SET "${j}" validation true)
  else()
    string(JSON j SET "${j}" validation false)
  endif()
  string(JSON j SET "${j}" slangc "{}")
  _sl_vk_json_str(v "${SL_TOOL_SLANGC}")
  string(JSON j SET "${j}" slangc path "${v}")
  _sl_vk_json_str(v "${slangc_version}")
  string(JSON j SET "${j}" slangc version "${v}")
  string(JSON j SET "${j}" spirv_val "{}")
  _sl_vk_json_str(v "${SL_TOOL_SPIRV_VAL}")
  string(JSON j SET "${j}" spirv_val path "${v}")
  _sl_vk_write_if_different("${CMAKE_BINARY_DIR}/vulkan-info.json" "${j}\n")
endfunction()
