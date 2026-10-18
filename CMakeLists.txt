# Sliced Labs C++23 foundation (WO-014, ADR-0005). Policy lives in cmake/: SlStandards (sl_target), SlWarnings,
# SlBuildInfo, SlFeatures (WO-015), SlVulkan (WO-016) and SlDeps (WO-018).
cmake_minimum_required(VERSION 3.28...4.4)
project(SlicedLabs VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# ADR-0005: module scanning stays off until `import std` is verified on both compilers.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(SlStandards)
include(SlWarnings)

include(SlFeatures)
sl_features_check()
include(SlVulkan)
sl_vulkan_detect()
include(SlDeps)
sl_deps_init()

include(SlBuildInfo)
include(CTest)

add_subdirectory(engine/core)
add_subdirectory(examples/sandbox)
add_subdirectory(tests/unit)
