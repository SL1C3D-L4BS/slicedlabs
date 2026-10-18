# sl_core: static library (WO-014), plus the generated build_info.hpp. WO-030 adds diagnostics: log.cpp (logger and
# sinks), error.cpp (error descriptions) and assert.cpp (assertion reports, optional std::stacktrace).
add_library(sl_core STATIC src/version.cpp src/log.cpp src/error.cpp src/assert.cpp)
add_library(sl::core ALIAS sl_core)
target_include_directories(sl_core PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(sl_core PRIVATE sl_build_info)
sl_target(sl_core)
sl_provenance(sl_core)

# std::stacktrace in assertion reports (WO-030). Optional (cmake/features: `stacktrace` is not required): on only when
# __cpp_lib_stacktrace is defined AND a program using it links, first without a library (MSVC), then with stdc++exp
# (libstdc++). SL_STACKTRACE=OFF turns it off; a toolchain without it builds the same reports without frames.
option(SL_STACKTRACE "Capture std::stacktrace in assertion reports when the toolchain provides it" ON)
set(SL_STACKTRACE_USED "off")
if(SL_STACKTRACE)
  include(CheckCXXSourceCompiles)
  set(_sl_stacktrace_probe [=[
#include <version>
#if !defined(__cpp_lib_stacktrace)
#error "no __cpp_lib_stacktrace"
#endif
#include <stacktrace>
int main() { return std::stacktrace::current().size() > 100000 ? 1 : 0; }
]=])
  block(SCOPE_FOR VARIABLES)
    set(CMAKE_CXX_STANDARD ${SL_CXX_STANDARD})
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_REQUIRED_QUIET ON)
    check_cxx_source_compiles("${_sl_stacktrace_probe}" SL_HAS_STACKTRACE_NOLIB)
    if(NOT SL_HAS_STACKTRACE_NOLIB)
      set(CMAKE_REQUIRED_LIBRARIES stdc++exp)
      check_cxx_source_compiles("${_sl_stacktrace_probe}" SL_HAS_STACKTRACE_STDCXXEXP)
    endif()
  endblock()
  if(SL_HAS_STACKTRACE_NOLIB)
    set(SL_STACKTRACE_USED "on")
  elseif(SL_HAS_STACKTRACE_STDCXXEXP)
    set(SL_STACKTRACE_USED "on (stdc++exp)")
    target_link_libraries(sl_core PRIVATE stdc++exp)
  endif()
  if(NOT SL_STACKTRACE_USED STREQUAL "off")
    target_compile_definitions(sl_core PRIVATE SL_HAS_STACKTRACE=1)
  endif()
endif()
message(STATUS "sliced: std::stacktrace in assertion reports: ${SL_STACKTRACE_USED}")
