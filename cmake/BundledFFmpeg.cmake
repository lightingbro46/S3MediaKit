# Build FFmpeg from the 3rdpart/FFmpeg submodule as shared libraries.
#
# The libraries carry the "_s3" build suffix (libavcodec_s3.so.61 ...) so they can never be
# confused with the distro FFmpeg: a wrong lookup fails with "not found" instead of silently
# loading /usr/lib/libavcodec.so.<n>. Every .so gets RUNPATH $ORIGIN because DT_RUNPATH is not
# inherited: libavfilter must find libavformat_s3 by itself.
#
# The install prefix release/<os>/3rdpart/ffmpeg is shared by every build directory and build
# type. A build id (submodule commits + configure line + compiler) is stored in the prefix; when
# it matches, the prefix is only imported and FFmpeg is not rebuilt.
#
# Outputs: ffmpeg::avutil/avcodec/avformat/avfilter/swscale/swresample imported targets,
# FFMPEG_BUNDLED_PREFIX, and the runtime .so copied to EXECUTABLE_OUTPUT_PATH. ffmpeg/ffprobe stay
# in <prefix>/bin: "./ffmpeg/" next to MediaServer is the default ffmpeg log directory
# (FFmpegSource looks for <exe dir>/../3rdpart/ffmpeg/bin first).

include(ExternalProject)

set(S3_DEPS_PREFIX_ROOT "${CMAKE_SOURCE_DIR}/release/${SYSTEM_NAME_LOWER}/3rdpart"
    CACHE PATH "Install prefix root of the bundled third-party shared libraries")
set(S3_DEPS_JOBS 2 CACHE STRING "Parallel jobs used to build bundled third-party libraries")

set(_ff_src "${CMAKE_SOURCE_DIR}/3rdpart/FFmpeg")
set(_nv_src "${CMAKE_SOURCE_DIR}/3rdpart/nv-codec-headers")
set(FFMPEG_BUNDLED_PREFIX "${S3_DEPS_PREFIX_ROOT}/ffmpeg")

if(NOT EXISTS "${_ff_src}/configure" OR NOT EXISTS "${_nv_src}/Makefile")
  message(FATAL_ERROR
    "ENABLE_BUNDLED_FFMPEG is ON but the FFmpeg submodules are not checked out.\n"
    "Run: git submodule update --init --depth 1 3rdpart/FFmpeg 3rdpart/nv-codec-headers\n"
    "or configure with -DENABLE_BUNDLED_FFMPEG=OFF to use the system FFmpeg.")
endif()

# library name -> soname major, read from the checked-out source so the submodule tag alone
# selects the FFmpeg version (the major is in version_major.h, or version.h for 4.4 and avutil)
file(STRINGS "${_ff_src}/RELEASE" _ff_release LIMIT_COUNT 1)
set(_ff_libs "")
foreach(_name avutil avcodec avformat avfilter swscale swresample)
  string(TOUPPER "${_name}" _upper)
  set(_line "")
  foreach(_hdr version_major.h version.h)
    if(NOT _line AND EXISTS "${_ff_src}/lib${_name}/${_hdr}")
      file(STRINGS "${_ff_src}/lib${_name}/${_hdr}" _line REGEX "#define LIB${_upper}_VERSION_MAJOR +[0-9]+")
    endif()
  endforeach()
  if(NOT _line)
    message(FATAL_ERROR "Cannot read LIB${_upper}_VERSION_MAJOR from ${_ff_src}/lib${_name}")
  endif()
  string(REGEX REPLACE ".*_VERSION_MAJOR +([0-9]+).*" "\\1" _major "${_line}")
  list(APPEND _ff_libs "${_name}:${_major}")
endforeach()

# Both 4.4 and 7.x are supported; the submodule tag picks one.
# 7.x: drawtext also needs harfbuzz; qsv goes through oneVPL (libvpl), since 7.x takes libmfx or
# libvpl, not both, and the Media SDK behind libmfx does not cover Intel Gen12+.
# 4.4: qsv through libmfx, and there is no --enable-libharfbuzz.
string(REGEX MATCH "^[0-9]+" _ff_release_major "${_ff_release}")
if(_ff_release_major LESS 7)
  set(_ff_harfbuzz "")
  set(_ff_qsv --enable-libmfx)
else()
  set(_ff_harfbuzz --enable-libharfbuzz)
  set(_ff_qsv --enable-libvpl)
endif()

# Only what the code uses: libx264/libx265/aac/mjpeg encoders, nvenc/nvdec/cuvid, vaapi and qsv
# for HwCodec, drawtext (freetype) and the SVG watermark read by movie= (librsvg) for overlays,
# https (openssl; GPLv3 via version3).
# --disable-autodetect keeps dev machines and the Docker image on the same feature set.
set(_ff_configure_args
  "--prefix=${FFMPEG_BUNDLED_PREFIX}"
  --build-suffix=_s3
  --enable-shared --disable-static --enable-pic
  --enable-gpl --enable-version3
  --disable-autodetect --enable-zlib --enable-iconv
  --enable-libx264 --enable-libx265 --enable-libfreetype ${_ff_harfbuzz} --enable-librsvg --enable-openssl
  --enable-ffnvcodec --enable-nvenc --enable-nvdec --enable-cuvid
  --enable-vaapi ${_ff_qsv}
  --disable-avdevice --disable-postproc --disable-ffplay --disable-doc)
# Written verbatim into a shell script; configure evaluates each value once more. Executables:
# config.mak gets \$$ORIGIN, make turns $$ into $, the recipe shell turns \$ into $. Libraries are
# linked from a $(eval)'d rule in ffbuild/library.mak, which expands twice: \$$$$ORIGIN.
set(_ff_rpath_args
  "--extra-ldsoflags='-Wl,-rpath,\\\\\\$\\$\\$\\$ORIGIN'"
  "--extra-ldexeflags='-Wl,-rpath,\\\\\\$\\$ORIGIN:\\\\\\$\\$ORIGIN/../lib'")

find_package(Git QUIET)
set(_ff_commits "")
foreach(_dir ${_ff_src} ${_nv_src})
  execute_process(COMMAND ${GIT_EXECUTABLE} -C ${_dir} rev-parse HEAD
    OUTPUT_VARIABLE _sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  list(APPEND _ff_commits "${_sha}")
endforeach()
# The compiler counts by id/version, not path: build dirs configured with /usr/bin/cc and
# /usr/bin/gcc must share the prefix.
string(SHA1 _ff_build_id
  "${_ff_commits};${_ff_configure_args};${_ff_rpath_args};${CMAKE_C_COMPILER_ID}-${CMAKE_C_COMPILER_VERSION}")

set(_ff_id_file "${FFMPEG_BUNDLED_PREFIX}/.s3-build-id")
set(_ff_prebuilt_id "")
if(EXISTS "${_ff_id_file}")
  file(READ "${_ff_id_file}" _ff_prebuilt_id)
  string(STRIP "${_ff_prebuilt_id}" _ff_prebuilt_id)
endif()

set(_ff_sonames "")
foreach(_entry ${_ff_libs})
  string(REPLACE ":" ";" _pair "${_entry}")
  list(GET _pair 0 _name)
  list(GET _pair 1 _major)
  list(APPEND _ff_sonames "${FFMPEG_BUNDLED_PREFIX}/lib/lib${_name}_s3.so.${_major}")
endforeach()
set(_ff_programs "${FFMPEG_BUNDLED_PREFIX}/bin/ffmpeg" "${FFMPEG_BUNDLED_PREFIX}/bin/ffprobe")

if(_ff_prebuilt_id STREQUAL _ff_build_id)
  message(STATUS "FFmpeg n${_ff_release} bundled: reusing ${FFMPEG_BUNDLED_PREFIX}")
  add_custom_target(s3_ffmpeg)
else()
  message(STATUS "FFmpeg n${_ff_release} bundled: will build into ${FFMPEG_BUNDLED_PREFIX}")
  set(_ff_bin "${CMAKE_BINARY_DIR}/3rdpart/ffmpeg")
  string(REPLACE ";" " " _ff_args_str "--cc=${CMAKE_C_COMPILER};${_ff_configure_args};${_ff_rpath_args}")
  file(WRITE "${CMAKE_BINARY_DIR}/3rdpart/ffmpeg-configure-${_ff_build_id}.sh"
    "#!/bin/sh\nset -e\n"
    # only runs for a new build id: objects and .d files of another FFmpeg version break make, and
    # headers/libraries of the other version (4.4 <-> 7.x) must not stay in the shared prefix
    "rm -rf '${_ff_bin}' && mkdir -p '${_ff_bin}' && cd '${_ff_bin}'\n"
    "cd '${FFMPEG_BUNDLED_PREFIX}' 2>/dev/null && rm -rf .s3-build-id include/libav* include/libsw* "
    "lib/libav* lib/libsw* lib/pkgconfig/libav* lib/pkgconfig/libsw* bin/ffmpeg bin/ffprobe share/ffmpeg; "
    "cd '${_ff_bin}'\n"
    "export PKG_CONFIG_PATH='${FFMPEG_BUNDLED_PREFIX}/lib/pkgconfig'\${PKG_CONFIG_PATH:+:\$PKG_CONFIG_PATH}\n"
    "exec '${_ff_src}/configure' ${_ff_args_str}\n")
  file(WRITE "${CMAKE_BINARY_DIR}/3rdpart/ffmpeg-build-id" "${_ff_build_id}")

  ExternalProject_Add(s3_nvcodec_headers
    SOURCE_DIR        ${_nv_src}
    BINARY_DIR        ${_nv_src}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND     ""
    INSTALL_COMMAND   make PREFIX=${FFMPEG_BUNDLED_PREFIX} install
    # per build id: a new id means the prefix may be new or wiped, so ffnvcodec.pc goes in again
    STAMP_DIR         ${CMAKE_BINARY_DIR}/3rdpart/nv-codec-headers-stamp-${_ff_build_id})

  ExternalProject_Add(s3_ffmpeg
    DEPENDS           s3_nvcodec_headers
    SOURCE_DIR        ${_ff_src}
    BINARY_DIR        ${_ff_bin}
    STAMP_DIR         ${CMAKE_BINARY_DIR}/3rdpart/ffmpeg-stamp
    CONFIGURE_COMMAND sh ${CMAKE_BINARY_DIR}/3rdpart/ffmpeg-configure-${_ff_build_id}.sh
    BUILD_COMMAND     make -j${S3_DEPS_JOBS}
    # install -p keeps header mtimes, so a rebuilt FFmpeg does not recompile every source using it
    INSTALL_COMMAND   make install "INSTALL=install -p"
    COMMAND           ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/3rdpart/ffmpeg-build-id ${_ff_id_file}
    BUILD_BYPRODUCTS  ${_ff_sonames} ${_ff_programs})
endif()

file(MAKE_DIRECTORY "${FFMPEG_BUNDLED_PREFIX}/include")
foreach(_entry ${_ff_libs})
  string(REPLACE ":" ";" _pair "${_entry}")
  list(GET _pair 0 _name)
  list(GET _pair 1 _major)
  add_library(ffmpeg::${_name} SHARED IMPORTED GLOBAL)
  set_target_properties(ffmpeg::${_name} PROPERTIES
    IMPORTED_LOCATION             "${FFMPEG_BUNDLED_PREFIX}/lib/lib${_name}_s3.so.${_major}"
    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_BUNDLED_PREFIX}/include")
  # Followed transitively by every target linking the imported library, so nothing compiles
  # against FFmpeg headers before they are installed.
  add_dependencies(ffmpeg::${_name} s3_ffmpeg)
endforeach()

# MediaServer and the tests find these next to themselves through RUNPATH $ORIGIN.
add_custom_target(s3_ffmpeg_runtime ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${EXECUTABLE_OUTPUT_PATH}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ff_sonames} ${EXECUTABLE_OUTPUT_PATH}/
  COMMENT "Copying bundled FFmpeg runtime -> ${EXECUTABLE_OUTPUT_PATH}/")
add_dependencies(s3_ffmpeg_runtime s3_ffmpeg)
