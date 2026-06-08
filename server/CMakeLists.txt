file(GLOB MediaServer_SRC_LIST ./*.cpp ./*.h)

set(COMPILE_DEFINITIONS ${MK_COMPILE_DEFINITIONS})

if(ENABLE_SERVER_LIB)
  list(APPEND COMPILE_DEFINITIONS DISABLE_MAIN)
  add_library(MediaServer STATIC ${MediaServer_SRC_LIST})
  target_compile_definitions(MediaServer
    PRIVATE ${COMPILE_DEFINITIONS})
  target_compile_options(MediaServer
    PRIVATE ${COMPILE_OPTIONS_DEFAULT})
  target_link_libraries(MediaServer
    PRIVATE ${MK_LINK_LIBRARIES})
  update_cached_list(MK_LINK_LIBRARIES MediaServer)
  return()
endif()

# IOS does not compile executable programs, only dependency libraries
if(IOS)
  add_library(MediaServer STATIC ${MediaServer_SRC_LIST})
else()
  add_executable(MediaServer ${MediaServer_SRC_LIST})
endif()

target_compile_definitions(MediaServer
  PRIVATE ${COMPILE_DEFINITIONS})
target_compile_options(MediaServer
  PRIVATE ${COMPILE_OPTIONS_DEFAULT})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  target_link_libraries(MediaServer -Wl,--start-group ${MK_LINK_LIBRARIES} -Wl,--end-group)
else()
  target_link_libraries(MediaServer ${MK_LINK_LIBRARIES})
endif()

# Set RPATH so MediaServer finds .so files in the same directory as itself
# ($ORIGIN) — works both in the build output dir and after deployment when
# the .so files are copied alongside the binary.
if(UNIX AND NOT APPLE)
  set_target_properties(MediaServer PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH             "$ORIGIN"
    INSTALL_RPATH_USE_LINK_PATH FALSE)
endif()

if(MSVC)
  set(RESOURCE_FILE "${CMAKE_SOURCE_DIR}/resource.rc")
  set_source_files_properties(${RESOURCE_FILE} PROPERTIES LANGUAGE RC)  
  target_sources(MediaServer PRIVATE ${RESOURCE_FILE})
else()
  # Android, IOS, macOS ...
  # CLion, GCC ...
endif()

install(TARGETS MediaServer DESTINATION ${INSTALL_PATH_RUNTIME})

#Additional debugging information is output when the release type is used.
string(TOLOWER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE_LOWER)
if(UNIX AND ENABLE_OBJCOPY)
  if("${CMAKE_BUILD_TYPE_LOWER}" STREQUAL "release")
    find_program(OBJCOPY_FOUND objcopy)
    if (OBJCOPY_FOUND)
      add_custom_command(TARGET MediaServer
        POST_BUILD
        COMMAND objcopy --only-keep-debug ${EXECUTABLE_OUTPUT_PATH}/MediaServer ${EXECUTABLE_OUTPUT_PATH}/MediaServer.debug
        COMMAND objcopy --strip-all ${EXECUTABLE_OUTPUT_PATH}/MediaServer
        COMMAND objcopy --add-gnu-debuglink=${EXECUTABLE_OUTPUT_PATH}/MediaServer.debug ${EXECUTABLE_OUTPUT_PATH}/MediaServer
      )
      install(FILES ${EXECUTABLE_OUTPUT_PATH}/MediaServer.debug DESTINATION ${INSTALL_PATH_RUNTIME})
    else()
      message(STATUS "not found objcopy, generate MediaServer.debug skip")
    endif()
  endif()
endif()

