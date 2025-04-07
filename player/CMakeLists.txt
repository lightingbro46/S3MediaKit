set(LINK_LIBRARIES ${MK_LINK_LIBRARIES})

find_package(PkgConfig QUIET)
# Find out if SDL2 is installed
if(PKG_CONFIG_FOUND)
  pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
  if(SDL2_FOUND)
    list(APPEND LINK_LIBRARIES PkgConfig::SDL2)
    message(STATUS "found library: ${SDL2_LIBRARIES}")
  endif()
endif()

if(NOT SDL2_FOUND)
  find_package(SDL2 QUIET)
  if(SDL2_FOUND)
    include_directories(SYSTEM ${SDL2_INCLUDE_DIR})
    list(APPEND LINK_LIBRARIES ${SDL2_LIBRARY})
    message(STATUS "found library: ${SDL2_LIBRARY}")
  endif()
endif()

set(PLAYER_NAME "test_player")

# If ffmpeg/libavcodec ffmpeg/libavcodec SDL is installed, the player is compiled
if(NOT SDL2_FOUND)
  message(WARNING "${PLAYER_NAME} disabled, please install sdl2 ffmpeg/libavcodec ffmpeg/libavutil ffmpeg/libswresample")
  return()
endif()

message(STATUS "${PLAYER_NAME} enabled")

aux_source_directory(. SRC_LIST)
add_executable(${PLAYER_NAME} ${SRC_LIST})
target_compile_definitions(${PLAYER_NAME}
  PRIVATE ${MK_COMPILE_DEFINITIONS})
target_compile_options(${PLAYER_NAME}
  PRIVATE ${COMPILE_OPTIONS_DEFAULT})

# TODO: Unified parameters?
if(MSVC)
  set_target_properties(${PLAYER_NAME} PROPERTIES LINK_FLAGS "/SAFESEH:NO /SUBSYSTEM:WINDOWS")
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  target_link_libraries(${PLAYER_NAME} -Wl,--start-group ${LINK_LIBRARIES} -Wl,--end-group)
else()
  target_link_libraries(${PLAYER_NAME} ${LINK_LIBRARIES})
endif()
