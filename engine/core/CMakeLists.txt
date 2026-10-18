# sl_core: static library, one translation unit plus the generated build_info.hpp (WO-014).
add_library(sl_core STATIC src/version.cpp)
add_library(sl::core ALIAS sl_core)
target_include_directories(sl_core PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(sl_core PRIVATE sl_build_info)
sl_target(sl_core)
