# Build the AWS SDK (s3 + core and the aws-c-* / aws-crt-cpp / s2n runtime) from the
# 3rdpart/aws-sdk-cpp submodule as shared libraries with RUNPATH $ORIGIN.
#
# Same reuse scheme as BundledFFmpeg.cmake: the install prefix release/<os>/3rdpart/aws-sdk is
# shared by every build directory; a matching build id skips the (long) SDK build.
#
# Outputs: AWS_SDK_INSTALL (prefix) and the s3_aws_sdk target to depend on.

include(ExternalProject)

set(S3_DEPS_PREFIX_ROOT "${CMAKE_SOURCE_DIR}/release/${SYSTEM_NAME_LOWER}/3rdpart"
    CACHE PATH "Install prefix root of the bundled third-party shared libraries")
set(S3_DEPS_JOBS 2 CACHE STRING "Parallel jobs used to build bundled third-party libraries")

set(_aws_src "${CMAKE_SOURCE_DIR}/3rdpart/aws-sdk-cpp")
set(AWS_SDK_INSTALL "${S3_DEPS_PREFIX_ROOT}/aws-sdk")

if(NOT EXISTS "${_aws_src}/crt/aws-crt-cpp/crt/aws-c-common/CMakeLists.txt")
  message(FATAL_ERROR
    "3rdpart/aws-sdk-cpp is checked out without its crt submodules.\n"
    "Run: git submodule update --init --recursive --depth 1 3rdpart/aws-sdk-cpp")
endif()

# Matches the SDK previously built by hand into 3rdpart/aws-sdk/bin: curl HTTP client and the
# system OpenSSL (USE_OPENSSL) instead of a bundled aws-lc.
set(_aws_cache_args
  "-DCMAKE_BUILD_TYPE:STRING=Release"
  "-DCMAKE_INSTALL_PREFIX:PATH=${AWS_SDK_INSTALL}"
  "-DCMAKE_INSTALL_LIBDIR:STRING=lib"
  "-DCMAKE_INSTALL_RPATH:STRING=$ORIGIN"
  "-DBUILD_ONLY:STRING=s3"
  "-DBUILD_SHARED_LIBS:BOOL=ON"
  "-DUSE_OPENSSL:BOOL=ON"
  "-DENABLE_TESTING:BOOL=OFF"
  "-DAUTORUN_UNIT_TESTS:BOOL=OFF")

execute_process(COMMAND git -C ${_aws_src} rev-parse HEAD
  OUTPUT_VARIABLE _aws_sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
# Compiler by id/version, not path (see BundledFFmpeg.cmake).
string(SHA1 _aws_build_id
  "${_aws_sha};${_aws_cache_args};${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")

set(_aws_id_file "${AWS_SDK_INSTALL}/.s3-build-id")
set(_aws_prebuilt_id "")
if(EXISTS "${_aws_id_file}")
  file(READ "${_aws_id_file}" _aws_prebuilt_id)
  string(STRIP "${_aws_prebuilt_id}" _aws_prebuilt_id)
endif()

if(_aws_prebuilt_id STREQUAL _aws_build_id)
  message(STATUS "AWS SDK bundled: reusing ${AWS_SDK_INSTALL}")
  add_custom_target(s3_aws_sdk)
else()
  message(STATUS "AWS SDK bundled: will build into ${AWS_SDK_INSTALL}")
  file(WRITE "${CMAKE_BINARY_DIR}/3rdpart/aws-sdk-build-id" "${_aws_build_id}")
  ExternalProject_Add(s3_aws_sdk
    SOURCE_DIR       ${_aws_src}
    BINARY_DIR       ${CMAKE_BINARY_DIR}/3rdpart/aws-sdk
    STAMP_DIR        ${CMAKE_BINARY_DIR}/3rdpart/aws-sdk-stamp
    CMAKE_CACHE_ARGS ${_aws_cache_args}
                     "-DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}"
                     "-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}"
    BUILD_COMMAND    ${CMAKE_COMMAND} --build <BINARY_DIR> -j${S3_DEPS_JOBS}
    INSTALL_COMMAND  ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
    COMMAND          ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/3rdpart/aws-sdk-build-id ${_aws_id_file}
    BUILD_BYPRODUCTS ${AWS_SDK_INSTALL}/lib/libaws-cpp-sdk-core.so ${AWS_SDK_INSTALL}/lib/libaws-cpp-sdk-s3.so)
endif()

file(MAKE_DIRECTORY "${AWS_SDK_INSTALL}/include")
