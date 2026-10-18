# A stand-in for vcpkg's scripts/buildsystems/vcpkg.cmake (WO-019 fixture; the file name ends in vcpkg.cmake on purpose).
# It does what the sl_deps vcpkg provider checks for: defines VCPKG_TARGET_TRIPLET and puts an "installed tree" on
# CMAKE_PREFIX_PATH. The tree here is this directory: share/doctest/doctestConfig.cmake (version 2.5.3).
# Nothing is downloaded or installed.
set(VCPKG_TARGET_TRIPLET "x64-linux" CACHE STRING "")
get_filename_component(_sl_fake_vcpkg_root "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
list(PREPEND CMAKE_PREFIX_PATH "${_sl_fake_vcpkg_root}")
