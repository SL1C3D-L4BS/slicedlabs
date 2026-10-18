# The warning table as a PURE function: no CMAKE_* variable is read, so cmake/tests/warnings_table.cmake can
# call it with mocked compiler ids. sl_target() (SlStandards) applies the list for the real compiler.
#
#   sl_warning_flags(<out> <compiler_id> <frontend_variant>)
#     compiler_id        GNU, Clang, AppleClang, MSVC (CMAKE_CXX_COMPILER_ID)
#     frontend_variant   GNU or MSVC (CMAKE_CXX_COMPILER_FRONTEND_VARIANT; clang-cl is Clang + MSVC)
function(sl_warning_flags out compiler_id frontend_variant)
  set(flags "")
  if(frontend_variant STREQUAL "MSVC" OR compiler_id STREQUAL "MSVC")
    set(flags /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc)
  elseif(compiler_id MATCHES "^(GNU|Clang|AppleClang)$")
    set(flags -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast
      -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2
      -Wimplicit-fallthrough -Wundef -Wextra-semi -Wcast-qual)
    if(compiler_id MATCHES "Clang")
      list(APPEND flags -Wthread-safety -Wcomma -Wloop-analysis -Wunreachable-code-aggressive)
    else()
      list(APPEND flags -Wduplicated-cond -Wlogical-op -Wbidi-chars=any)
    endif()
  endif()
  set(${out} "${flags}" PARENT_SCOPE)
endfunction()
