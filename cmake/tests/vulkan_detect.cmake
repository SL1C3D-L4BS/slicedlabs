# Script-mode test of the pure helpers in cmake/SlVulkan.cmake (WO-016):
#   cmake -P cmake/tests/vulkan_detect.cmake
# No Vulkan, no device, no tools needed.
cmake_minimum_required(VERSION 3.28)
get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${_root}/cmake/SlVulkan.cmake")

set(_cases 0)

function(_expect label want got)
  math(EXPR n "${_cases} + 1")
  set(_cases ${n} PARENT_SCOPE)
  if(got STREQUAL want)
    message(STATUS "ok    ${label}")
  else()
    message(SEND_ERROR "FAIL  ${label}\n  want: '${want}'\n  got:  '${got}'")
  endif()
endfunction()

# version parsing
_sl_vk_parse_version(v "v2026.18.2")
_expect("parse v2026.18.2" "2026.18.2" "${v}")
_sl_vk_parse_version(v "slangc version 2026.18 (build x)")
_expect("parse 2026.18" "2026.18" "${v}")
_sl_vk_parse_version(v "1.4.363")
_expect("parse 1.4.363" "1.4.363" "${v}")
_sl_vk_parse_version(v "no digits here")
_expect("parse none" "" "${v}")
_sl_vk_parse_version(v "")
_expect("parse empty" "" "${v}")

# version ordering as the detect step uses it (VERSION_GREATER_EQUAL, numeric per component)
if("2026.18.2" VERSION_GREATER_EQUAL "2026.18")
  _expect("2026.18.2 >= 2026.18" "yes" "yes")
else()
  _expect("2026.18.2 >= 2026.18" "yes" "no")
endif()
if("2026.9" VERSION_GREATER_EQUAL "2026.18")
  _expect("2026.9 < 2026.18" "no" "yes")
else()
  _expect("2026.9 < 2026.18" "no" "no")
endif()

# layer manifest api_version, from a fixture string (the shape of the real manifest, with escapes and arrays)
set(_manifest [=[{
  "file_format_version": "1.2.0",
  "layer": {
    "name": "VK_LAYER_KHRONOS_validation",
    "library_path": "libVkLayer_khronos_validation.so",
    "api_version": "1.4.363",
    "introduction": "Line one.\n\nLine two; with a semicolon and [brackets].",
    "platforms": ["WINDOWS", "LINUX"]
  }
}]=])
_sl_vk_layer_api_version(v "${_manifest}")
_expect("layer api_version" "1.4.363" "${v}")
_sl_vk_layer_api_version(v [=[{"layer": {"name": "x"}}]=])
_expect("layer api_version absent" "" "${v}")
_sl_vk_layer_api_version(v "not json at all")
_expect("layer api_version malformed" "" "${v}")
_sl_vk_layer_api_version(v "")
_expect("layer api_version empty" "" "${v}")

# VK_HEADER_VERSION from header text
_sl_vk_header_version_from_text(v "// x\n#define VK_HEADER_VERSION 363\n#define VK_HEADER_VERSION_COMPLETE VK_MAKE_API_VERSION(0, 1, 4, VK_HEADER_VERSION)\n")
_expect("header version" "363" "${v}")
_sl_vk_header_version_from_text(v "#define VK_HEADER_VERSION_COMPLETE 1")
_expect("header version absent" "" "${v}")

# JSON string literal
_sl_vk_json_str(v "a\"b\\c")
_expect("json str escapes" "\"a\\\"b\\\\c\"" "${v}")
_sl_vk_json_str(v "")
_expect("json str empty is null" "null" "${v}")

if(_cases LESS 14)
  message(SEND_ERROR "FAIL  expected at least 14 cases, ran ${_cases}")
endif()
message(STATUS "vulkan_detect: ${_cases} cases")
