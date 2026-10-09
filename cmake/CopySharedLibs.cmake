# cmake -DSRC=<dir> -DDST=<dir> [-DPATTERN=lib*.so*] -P CopySharedLibs.cmake
# Copies the shared libraries of SRC into DST at build time (the set is only known once the
# bundled library is built), keeping versioned symlinks as symlinks.
if(NOT PATTERN)
  set(PATTERN "lib*.so*")
endif()
file(GLOB _libs "${SRC}/${PATTERN}")
if(NOT _libs)
  message(FATAL_ERROR "No ${PATTERN} in ${SRC}")
endif()
file(MAKE_DIRECTORY "${DST}")
file(COPY ${_libs} DESTINATION "${DST}")
