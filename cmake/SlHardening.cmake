# Release hardening and the gcc analyzer (WO-025, SL-EP-050). Included by SlStandards.cmake after _sl_add exists;
# sl_target() applies SL_HARDEN_COMPILE, SL_HARDEN_LINK and SL_ANALYZER_FLAGS to every first-party target.
#
#   SL_HARDEN     ON adds the OpenSSF compiler options set (GNU front end, clang and gcc alike). Every flag is REQUIRED:
#                 a hardening flag the compiler lacks stops the configure, it is never dropped quietly. The ci-release
#                 and ci-gcc-release presets turn it on; scripts/check-hardening reads the binaries to prove it.
#   SL_ANALYZER   ON adds gcc's -fanalyzer (GNU only; the `analyze` recipe uses it in build/gcc-analyzer).
#   SL_PROOFS     ON builds the planted-defect programs of tests/proofs (root CMakeLists.txt); `just sanitize-proof`.
#
# What the set is for: _FORTIFY_SOURCE=3 and -fstrict-flex-arrays=3 turn libc and trailing-array overruns into aborts,
# -fsanitize=bounds with -fsanitize-trap=bounds makes an out-of-bounds array index a trap (SIGILL) with no runtime
# library, -fstack-protector-strong and -fstack-clash-protection guard the stack, -ftrivial-auto-var-init=zero zeroes
# uninitialised locals, -fno-delete-null-pointer-checks -fno-strict-overflow -fno-strict-aliasing remove three
# optimisations that turn latent bugs into exploitable ones, -fcf-protection=full emits IBT and SHSTK notes (x86),
# -fPIE with -pie gives ASLR, and the linker half is RELRO, immediate binding, a non-executable stack and no dlopen.
option(SL_HARDEN "Release hardening: the OpenSSF compiler and linker options set (GNU front end)" OFF)
option(SL_ANALYZER "gcc -fanalyzer on every first-party target (gcc only)" OFF)

set(SL_HARDEN_COMPILE "")
set(SL_HARDEN_LINK "")
set(SL_ANALYZER_FLAGS "")

if(SL_HARDEN)
  if(_sl_msvc_frontend)
    message(FATAL_ERROR "sliced: SL_HARDEN is defined for the GNU compiler front end (clang, gcc), not ${CMAKE_CXX_COMPILER_ID}")
  endif()
  if(SL_SANITIZE)
    message(FATAL_ERROR "sliced: SL_HARDEN and SL_SANITIZE=${SL_SANITIZE} are exclusive: hardening traps on bounds, "
      "a sanitizer reports them")
  endif()
  set(_sl_harden_flags -O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fPIE
    -ftrivial-auto-var-init=zero -fstrict-flex-arrays=3 -fno-delete-null-pointer-checks -fno-strict-overflow
    -fno-strict-aliasing -fsanitize=bounds -fsanitize-trap=bounds)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
    list(APPEND _sl_harden_flags -fcf-protection=full)
  endif()
  _sl_add(SL_HARDEN_COMPILE REQUIRED ${_sl_harden_flags})

  include(CheckLinkerFlag)
  set(_sl_harden_link -pie -Wl,-z,relro,-z,now,-z,noexecstack,-z,nodlopen)
  check_linker_flag(CXX "${_sl_harden_link}" SL_HAS_HARDEN_LINK)
  if(NOT SL_HAS_HARDEN_LINK)
    message(FATAL_ERROR "sliced: the linker (${SL_LINKER_USED}) rejects the hardening link options ${_sl_harden_link}")
  endif()
  set(SL_HARDEN_LINK ${_sl_harden_link})
  message(STATUS "sliced: hardening on (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
endif()

if(SL_ANALYZER)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "sliced: SL_ANALYZER needs gcc, got ${CMAKE_CXX_COMPILER_ID}")
  endif()
  _sl_add(SL_ANALYZER_FLAGS REQUIRED -fanalyzer)
endif()
