# Build librdkafka from the 3rdpart/librdkafka submodule as a static library (C API only is used).
#
# Same reuse scheme as BundledFFmpeg.cmake / BundledAwsSdk.cmake: the install prefix
# release/<os>/3rdpart/librdkafka is shared by every build directory; a matching build id skips
# the build. Static, so nothing has to be copied next to MediaServer; the runtime dependencies are
# system libraries only (OpenSSL, zstd, zlib).
#
# Outputs: the rdkafka::rdkafka imported target and RDKAFKA_INSTALL (prefix).

include(ExternalProject)

set(S3_DEPS_PREFIX_ROOT "${CMAKE_SOURCE_DIR}/release/${SYSTEM_NAME_LOWER}/3rdpart"
    CACHE PATH "Install prefix root of the bundled third-party shared libraries")
set(S3_DEPS_JOBS 2 CACHE STRING "Parallel jobs used to build bundled third-party libraries")

set(_rdk_src "${CMAKE_SOURCE_DIR}/3rdpart/librdkafka")
set(RDKAFKA_INSTALL "${S3_DEPS_PREFIX_ROOT}/librdkafka")

if(NOT EXISTS "${_rdk_src}/CMakeLists.txt")
  message(FATAL_ERROR
    "ENABLE_KAFKA is ON but 3rdpart/librdkafka is not checked out.\n"
    "Run: git submodule update --init --depth 1 3rdpart/librdkafka\n"
    "or configure with -DENABLE_BUNDLED_RDKAFKA=OFF (system librdkafka) or -DENABLE_KAFKA=OFF.")
endif()

find_package(OpenSSL REQUIRED)
find_library(S3_ZSTD_LIBRARY NAMES zstd)
if(NOT S3_ZSTD_LIBRARY)
  message(FATAL_ERROR "ENABLE_KAFKA needs libzstd (apt install libzstd-dev)")
endif()

# TLS and SASL PLAIN/SCRAM over the system OpenSSL, zstd + the lz4 bundled in librdkafka.
# WITH_SASL=ON is what enables SCRAM. librdkafka probes libsasl2 (Cyrus/GSSAPI) only when
# pkg-config is found; a pkg-config that fails "--version" counts as not found (FindPkgConfig
# still loads, which FindOpenSSL needs), so dev machines with libsasl2-dev and the Docker image
# build the same library.
# No curl (OIDC), no plugin dlopen, no examples/tests.
set(_rdk_cache_args
  "-DCMAKE_BUILD_TYPE:STRING=Release"
  "-DCMAKE_INSTALL_PREFIX:PATH=${RDKAFKA_INSTALL}"
  "-DCMAKE_INSTALL_LIBDIR:STRING=lib"
  "-DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON"
  "-DRDKAFKA_BUILD_STATIC:BOOL=ON"
  "-DRDKAFKA_BUILD_EXAMPLES:BOOL=OFF"
  "-DRDKAFKA_BUILD_TESTS:BOOL=OFF"
  "-DWITH_SSL:BOOL=ON"
  "-DWITH_SASL:BOOL=ON"
  "-DWITH_ZSTD:BOOL=ON"
  "-DWITH_ZLIB:BOOL=ON"
  "-DWITH_CURL:BOOL=OFF"
  "-DWITH_PLUGINS:BOOL=OFF"
  "-DENABLE_LZ4_EXT:BOOL=OFF"
  "-DPKG_CONFIG_EXECUTABLE:FILEPATH=/bin/false")

execute_process(COMMAND git -C ${_rdk_src} rev-parse HEAD
  OUTPUT_VARIABLE _rdk_sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
# Compiler by id/version, not path (see BundledFFmpeg.cmake).
string(SHA1 _rdk_build_id
  "${_rdk_sha};${_rdk_cache_args};${CMAKE_C_COMPILER_ID}-${CMAKE_C_COMPILER_VERSION}")

set(_rdk_id_file "${RDKAFKA_INSTALL}/.s3-build-id")
set(_rdk_prebuilt_id "")
if(EXISTS "${_rdk_id_file}")
  file(READ "${_rdk_id_file}" _rdk_prebuilt_id)
  string(STRIP "${_rdk_prebuilt_id}" _rdk_prebuilt_id)
endif()

set(_rdk_lib "${RDKAFKA_INSTALL}/lib/librdkafka.a")
if(_rdk_prebuilt_id STREQUAL _rdk_build_id)
  message(STATUS "librdkafka bundled: reusing ${RDKAFKA_INSTALL}")
  add_custom_target(s3_rdkafka)
else()
  message(STATUS "librdkafka bundled: will build into ${RDKAFKA_INSTALL}")
  file(WRITE "${CMAKE_BINARY_DIR}/3rdpart/rdkafka-build-id" "${_rdk_build_id}")
  ExternalProject_Add(s3_rdkafka
    SOURCE_DIR       ${_rdk_src}
    BINARY_DIR       ${CMAKE_BINARY_DIR}/3rdpart/librdkafka
    STAMP_DIR        ${CMAKE_BINARY_DIR}/3rdpart/librdkafka-stamp
    CMAKE_CACHE_ARGS ${_rdk_cache_args}
                     "-DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}"
                     "-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}"
    BUILD_COMMAND    ${CMAKE_COMMAND} --build <BINARY_DIR> -j${S3_DEPS_JOBS}
    INSTALL_COMMAND  ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
    COMMAND          ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/3rdpart/rdkafka-build-id ${_rdk_id_file}
    BUILD_BYPRODUCTS ${_rdk_lib})
endif()

file(MAKE_DIRECTORY "${RDKAFKA_INSTALL}/include")
add_library(rdkafka::rdkafka STATIC IMPORTED GLOBAL)
set_target_properties(rdkafka::rdkafka PROPERTIES
  IMPORTED_LOCATION                 "${_rdk_lib}"
  IMPORTED_LINK_INTERFACE_LANGUAGES C
  INTERFACE_INCLUDE_DIRECTORIES     "${RDKAFKA_INSTALL}/include"
  INTERFACE_COMPILE_DEFINITIONS     LIBRDKAFKA_STATICLIB
  INTERFACE_LINK_LIBRARIES
    "OpenSSL::SSL;OpenSSL::Crypto;${S3_ZSTD_LIBRARY};z;m;${CMAKE_DL_LIBS};pthread;rt")
add_dependencies(rdkafka::rdkafka s3_rdkafka)
