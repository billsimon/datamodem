cmake_minimum_required(VERSION 3.16)
project(datamodem VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(DATAMODEM_HARDENING "Compile and link with the usual hardening flags" ON)
option(DATAMODEM_LTO "Link-time optimisation in release builds" ON)
option(DATAMODEM_WERROR "Treat warnings as errors" OFF)
# Two problems, one slice. The V.22bis in the commonly packaged libspandsp
# trains and then carries no data; third_party/spandsp-v22bis holds the two
# source files from a later upstream snapshot where it works. On top of that
# snapshot sits one patch: upstream comes back from a carrier drop at the rate
# it originally offered rather than the one the two ends settled on, which
# over RTP turns a routine jitter buffer underrun into an endless retrain.
# Building these in overrides the library's. The patch is still needed however
# new the installed libspandsp is.
option(DATAMODEM_VENDOR_V22BIS "Build V.22bis from the vendored, patched spandsp sources" ON)
# spandsp's V.42 detection miscounts the gap between the two ADP characters
# and rejects a far end using the top of the range V.42 permits.
# third_party/spandsp-v42 holds the one-line fix.
option(DATAMODEM_VENDOR_V42 "Build V.42 from the vendored, patched spandsp source" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SPANDSP REQUIRED IMPORTED_TARGET spandsp)
pkg_check_modules(PJPROJECT REQUIRED IMPORTED_TARGET libpjproject)

# spandsp.h includes <tiffio.h> unconditionally, for the T.4/T.30 half of the
# library that nothing here touches. We need the header on the include path,
# but not the library on the link line - libspandsp already carries that.
pkg_check_modules(TIFF REQUIRED libtiff-4)

find_package(Threads REQUIRED)
find_package(OpenSSL)

# libpjproject.pc lists only the pj* libraries; the codecs and SRTP that
# pjproject bundles sit beside them as static archives and still have to be
# linked in explicitly.
set(PJ_BUNDLED_LIBS "")
foreach(_pj_extra g7221codec gsmcodec ilbccodec resample speex srtp webrtc yuv)
  foreach(_pj_dir ${PJPROJECT_LIBRARY_DIRS} ${PJPROJECT_STATIC_LIBRARY_DIRS})
    file(GLOB _pj_found "${_pj_dir}/lib${_pj_extra}*.a")
    if(_pj_found)
      list(APPEND PJ_BUNDLED_LIBS ${_pj_found})
      break()
    endif()
  endforeach()
endforeach()
if(PJ_BUNDLED_LIBS)
  message(STATUS "pjproject bundled libraries: ${PJ_BUNDLED_LIBS}")
endif()

# Stamp what went into this binary, so `datamodem version` can say so.
# SOURCE_DATE_EPOCH is honoured for reproducible builds.
if(DEFINED ENV{SOURCE_DATE_EPOCH})
  set(DATAMODEM_BUILD_DATE "epoch:$ENV{SOURCE_DATE_EPOCH}")
else()
  string(TIMESTAMP DATAMODEM_BUILD_DATE "%Y-%m-%dT%H:%M:%SZ" UTC)
endif()
configure_file(include/datamodem/build_info.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/datamodem/build_info.h" @ONLY)

add_executable(datamodem
  src/main.c
  src/config.c
  src/log.c
  src/modem.c
  src/ring.c
  src/session.c
  src/sip.c
  src/term.c
  src/tty.c
  src/util.c
  src/v32.c
  src/v34.c
  src/v34_codec.c
  src/v34_dsp.c
  src/v34_info.c
)

target_include_directories(datamodem PRIVATE
  include
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
  ${TIFF_INCLUDE_DIRS}
)

# A fuzzer for the terminal emulator, under the sanitizers. Not built by
# default: cmake --build build --target term-fuzz && build/term-fuzz [seed]
add_executable(term-fuzz EXCLUDE_FROM_ALL
  tests/term-fuzz.c
  src/term.c
  src/tty.c
  src/log.c
  src/util.c
)
target_include_directories(term-fuzz PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(term-fuzz PRIVATE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined)
target_link_options(term-fuzz PRIVATE -fsanitize=address,undefined)
target_link_libraries(term-fuzz PRIVATE Threads::Threads)
if(NOT APPLE)
  target_link_libraries(term-fuzz PRIVATE util)
endif()

if(DATAMODEM_VENDOR_V22BIS)
  set(V22BIS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spandsp-v22bis")
  target_sources(datamodem PRIVATE
    "${V22BIS_DIR}/v22bis_rx.c"
    "${V22BIS_DIR}/v22bis_tx.c"
  )
  # These are somebody else's sources, held verbatim so they can be diffed
  # against upstream. Compile them on their own terms rather than ours: our
  # warning set would bury any real diagnostic in noise, and patching them to
  # silence it would defeat the point of keeping them verbatim.
  set_source_files_properties(
    "${V22BIS_DIR}/v22bis_rx.c" "${V22BIS_DIR}/v22bis_tx.c"
    PROPERTIES
      COMPILE_OPTIONS "-w"
      INCLUDE_DIRECTORIES "${V22BIS_DIR};${V22BIS_DIR}/generated"
      COMPILE_DEFINITIONS "HAVE_CONFIG_H"
  )
  target_compile_definitions(datamodem PRIVATE DATAMODEM_VENDORED_V22BIS=1)
  message(STATUS "V.22bis: using the vendored spandsp sources "
                 "(the installed libspandsp's does not carry data)")
else()
  message(STATUS "V.22bis: using the installed libspandsp "
                 "(check it actually carries data - many builds do not)")
endif()

if(DATAMODEM_VENDOR_V42)
  set(V42_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spandsp-v42")
  target_sources(datamodem PRIVATE "${V42_DIR}/v42.c")
  set_source_files_properties("${V42_DIR}/v42.c" PROPERTIES
      COMPILE_OPTIONS "-w"
      INCLUDE_DIRECTORIES "${V42_DIR}"
      COMPILE_DEFINITIONS "HAVE_CONFIG_H")
  target_compile_definitions(datamodem PRIVATE DATAMODEM_VENDORED_V42=1)
  message(STATUS "V.42: using the vendored spandsp source "
                 "(the installed one miscounts the ADP gap)")
else()
  message(STATUS "V.42: using the installed libspandsp "
                 "(detection will fail against a peer using a 16-bit ADP gap)")
endif()
target_link_libraries(datamodem PRIVATE
  PkgConfig::PJPROJECT
  ${PJ_BUNDLED_LIBS}
  PkgConfig::SPANDSP
  Threads::Threads
)
if(NOT APPLE)
  # v32.c is our own signal processing, not spandsp's; glibc keeps libm apart.
  target_link_libraries(datamodem PRIVATE m)
endif()

if(OPENSSL_FOUND)
  target_link_libraries(datamodem PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

target_compile_options(datamodem PRIVATE
  -Wall -Wextra -Wno-unused-parameter -Wshadow
  $<$<BOOL:${DATAMODEM_WERROR}>:-Werror>
)

# -O2 rather than -O3: the signal processing is spandsp's, and -O2 is the
# setting it is actually tested with.
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

if(DATAMODEM_HARDENING)
  target_compile_options(datamodem PRIVATE
    -fstack-protector-strong
    -fno-strict-aliasing          # spandsp and pjsip both type-pun
    $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
  )
  if(NOT APPLE)
    # PIE plus full RELRO; the macOS toolchain does both by default.
    target_compile_options(datamodem PRIVATE -fPIE)
    target_link_options(datamodem PRIVATE -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
  endif()
endif()

if(DATAMODEM_LTO AND CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_why)
  if(_ipo_ok)
    set_property(TARGET datamodem PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "link-time optimisation enabled")
  else()
    message(STATUS "link-time optimisation unavailable: ${_ipo_why}")
  endif()
endif()

if(APPLE)
  # pjproject's audio device layer pulls these in even though we run headless.
  target_link_libraries(datamodem PRIVATE
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework CoreFoundation"
    "-framework CoreServices"
    "-framework AudioUnit"
  )
endif()

# The parts of V.34 that have to agree with the far end bit for bit -
# framing, shell mapping, the encoder and decoder, INFO and MP - tested on
# their own. `ctest` or build/v34test.
enable_testing()
add_executable(v34test tests/v34_test.c src/v34_codec.c src/v34_info.c src/v34_dsp.c src/log.c src/util.c)
target_include_directories(v34test PRIVATE src include)
target_compile_options(v34test PRIVATE -Wall -Wextra -Wno-unused-parameter -Wshadow)
target_link_libraries(v34test PRIVATE Threads::Threads)
if(NOT APPLE)
  target_link_libraries(v34test PRIVATE m)
endif()
add_test(NAME v34 COMMAND v34test)

# Caller ID parsing and the other small shared pieces of util.c.
add_executable(utiltest tests/util_test.c src/util.c)
target_include_directories(utiltest PRIVATE src include)
target_compile_options(utiltest PRIVATE -Wall -Wextra -Wno-unused-parameter -Wshadow)
add_test(NAME util COMMAND utiltest)
install(TARGETS datamodem RUNTIME DESTINATION bin)
install(FILES examples/datamodem.conf DESTINATION share/datamodem/examples)
install(PROGRAMS scripts/loopback-test.sh scripts/escape-test.sh scripts/pty-test.py scripts/exec-test.sh
        DESTINATION share/datamodem/scripts)
