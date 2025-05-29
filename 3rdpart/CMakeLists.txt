# jsoncpp
file(GLOB JSONCPP_SRC_LIST
  ${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp/include/json/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp/src/lib_json/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp/src/lib_json/*.h)
  
add_library(jsoncpp STATIC ${JSONCPP_SRC_LIST})
target_compile_options(jsoncpp
  PRIVATE ${COMPILE_OPTIONS_DEFAULT})
target_include_directories(jsoncpp
  PRIVATE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/jsoncpp/include"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/jsoncpp/include")

update_cached_list(MK_LINK_LIBRARIES jsoncpp)

##############################################################################

# media-server
set(MediaServer_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/media-server")
# TODO: Complement a function to handle various libraries

# Add mov and flv libraries for MP4 recording
if (ENABLE_MP4)
  # MOV
  set(MediaServer_MOV_ROOT ${MediaServer_ROOT}/libmov)
  aux_source_directory(${MediaServer_MOV_ROOT}/include MOV_SRC_LIST)
  aux_source_directory(${MediaServer_MOV_ROOT}/source MOV_SRC_LIST)
  add_library(mov STATIC ${MOV_SRC_LIST})
  add_library(MediaServer::mov ALIAS mov)
  target_compile_options(mov PRIVATE ${COMPILE_OPTIONS_DEFAULT})
  target_include_directories(mov
          PRIVATE
          "$<BUILD_INTERFACE:${MediaServer_MOV_ROOT}/include>"
          PUBLIC
          "$<BUILD_INTERFACE:${MediaServer_MOV_ROOT}/include>")

  # FLV
  set(MediaServer_FLV_ROOT ${MediaServer_ROOT}/libflv)
  aux_source_directory(${MediaServer_FLV_ROOT}/include FLV_SRC_LIST)
  aux_source_directory(${MediaServer_FLV_ROOT}/source FLV_SRC_LIST)
  add_library(flv STATIC ${FLV_SRC_LIST})
  add_library(MediaServer::flv ALIAS flv)
  target_compile_options(flv PRIVATE ${COMPILE_OPTIONS_DEFAULT})
  target_include_directories(flv
          PRIVATE
          "$<BUILD_INTERFACE:${MediaServer_FLV_ROOT}/include>"
          PUBLIC
          "$<BUILD_INTERFACE:${MediaServer_FLV_ROOT}/include>")

  update_cached_list(MK_LINK_LIBRARIES MediaServer::flv MediaServer::mov)

  if (ENABLE_MP4)
    message(STATUS "ENABLE_MP4 defined")
    update_cached_list(MK_COMPILE_DEFINITIONS ENABLE_MP4)
  endif ()
endif ()

# Add mkv libraries for MKV recording
if (ENABLE_MKV)
  # MOV
  set(MediaServer_MKV_ROOT ${MediaServer_ROOT}/libmkv)
  aux_source_directory(${MediaServer_MKV_ROOT}/include MKV_SRC_LIST)
  aux_source_directory(${MediaServer_MKV_ROOT}/src MKV_SRC_LIST)
  add_library(mkv STATIC ${MKV_SRC_LIST})
  add_library(MediaServer::mkv ALIAS mkv)
  target_compile_options(mkv PRIVATE ${COMPILE_OPTIONS_DEFAULT})
  target_include_directories(mkv
          PRIVATE
          "$<BUILD_INTERFACE:${MediaServer_MKV_ROOT}/include>"
          PUBLIC
          "$<BUILD_INTERFACE:${MediaServer_MKV_ROOT}/include>")

  update_cached_list(MK_LINK_LIBRARIES MediaServer::mkv)

  if (ENABLE_MKV)
    message(STATUS "ENABLE_MKV defined")
    update_cached_list(MK_COMPILE_DEFINITIONS ENABLE_MKV)
  endif ()
endif ()

# Add mpeg to support ts generation
if(ENABLE_RTPPROXY OR ENABLE_HLS)
  # mpeg
  set(MediaServer_MPEG_ROOT ${MediaServer_ROOT}/libmpeg)
  aux_source_directory(${MediaServer_MPEG_ROOT}/include MPEG_SRC_LIST)
  aux_source_directory(${MediaServer_MPEG_ROOT}/source  MPEG_SRC_LIST)
  add_library(mpeg STATIC ${MPEG_SRC_LIST})
  add_library(MediaServer::mpeg ALIAS mpeg)
  # media-server library related compiled macros
  # MPEG_H26X_VERIFY -Video stream type recognition
  # MPEG_ZERO_PAYLOAD_LENGTH -Compatible with hik streams
  # MPEG_DAHUA_AAC_FROM_G711 -Compatible with dahua streams
  target_compile_options(mpeg
    PRIVATE ${COMPILE_OPTIONS_DEFAULT} -DMPEG_H26X_VERIFY -DMPEG_ZERO_PAYLOAD_LENGTH -DMPEG_DAHUA_AAC_FROM_G711)
  target_include_directories(mpeg
    PRIVATE
      "$<BUILD_INTERFACE:${MediaServer_MPEG_ROOT}/include>"
    PUBLIC
      "$<BUILD_INTERFACE:${MediaServer_MPEG_ROOT}/include>")

  update_cached_list(MK_LINK_LIBRARIES MediaServer::mpeg)
  if(ENABLE_RTPPROXY)
    message(STATUS "ENABLE_RTPPROXY defined")
    update_cached_list(MK_COMPILE_DEFINITIONS ENABLE_RTPPROXY)
  endif()
  if(ENABLE_HLS)
    message(STATUS "ENABLE_HLS defined")
    update_cached_list(MK_COMPILE_DEFINITIONS ENABLE_HLS)
  endif()
endif()

##############################################################################

# toolkit
# TODO:Rebuild toolkit for direct reference

include(CheckStructHasMember)
include(CheckSymbolExists)

# Check sendmmsg-related dependencies and set the corresponding macros, configure _GNU_SOURCE to enable the GNU extension feature
list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_struct_has_member("struct mmsghdr" msg_hdr sys/socket.h HAVE_MMSG_HDR)
check_symbol_exists(sendmmsg sys/socket.h HAVE_SENDMMSG_API)
check_symbol_exists(recvmmsg sys/socket.h HAVE_RECVMMSG_API)

set(COMPILE_DEFINITIONS)
# ToolKit depends on ENABLE_OPENSSL and ENABLE_MYSQL, ENABLE_SQLITE
list(FIND MK_COMPILE_DEFINITIONS ENABLE_OPENSSL ENABLE_OPENSSL_INDEX)
if(NOT ENABLE_OPENSSL_INDEX EQUAL -1)
  list(APPEND COMPILE_DEFINITIONS ENABLE_OPENSSL)
endif()
list(FIND MK_COMPILE_DEFINITIONS ENABLE_MYSQL   ENABLE_MYSQL_INDEX)
if(NOT ENABLE_MYSQL_INDEX EQUAL -1)
  list(APPEND COMPILE_DEFINITIONS ENABLE_MYSQL)
endif()
list(FIND MK_COMPILE_DEFINITIONS ENABLE_SQLITE   ENABLE_SQLITE_INDEX)
if(NOT ENABLE_SQLITE_INDEX EQUAL -1)
  list(APPEND COMPILE_DEFINITIONS ENABLE_SQLITE)
endif()
if(HAVE_MMSG_HDR)
  list(APPEND COMPILE_DEFINITIONS HAVE_MMSG_HDR)
endif()
if(HAVE_SENDMMSG_API)
  list(APPEND COMPILE_DEFINITIONS HAVE_SENDMMSG_API)
endif()
if(HAVE_RECVMMSG_API)
  list(APPEND COMPILE_DEFINITIONS HAVE_RECVMMSG_API)
endif()

# check the socket buffer size set by the upper cmake project, if it is set, use the setting of the upper cmake project, otherwise set it to 256K
# if the socket buffer size is set to 0, it means that the socket buffer size is not set, and the kernel default value is used(just for linux)
if(DEFINED SOCKET_DEFAULT_BUF_SIZE)
  if (SOCKET_DEFAULT_BUF_SIZE EQUAL 0)
    message(STATUS "Socket default buffer size is not set, use the kernel default value")
  else()
    message(STATUS "Socket default buffer size is set to ${SOCKET_DEFAULT_BUF_SIZE}")
  endif ()
  add_definitions(-DSOCKET_DEFAULT_BUF_SIZE=${SOCKET_DEFAULT_BUF_SIZE})
endif()

set(ToolKit_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/S3ToolKit)
# Collect source code
file(GLOB ToolKit_SRC_LIST
  ${ToolKit_ROOT}/src/*/*.cpp
  ${ToolKit_ROOT}/src/*/*.h
  ${ToolKit_ROOT}/src/*/*.c)
if(IOS)
  list(APPEND ToolKit_SRC_LIST
    ${ToolKit_ROOT}/src/Network/Socket_ios.mm)
endif()

###################################################################
# Use wepoll windows iocp to simulate epoll
if(ENABLE_WEPOLL)
  if(WIN32)
    message(STATUS "Enable wepoll")
    #Add wepoll source files and API parameters compatible files
    list(APPEND ToolKit_SRC_LIST
            ${CMAKE_CURRENT_SOURCE_DIR}/wepoll/wepoll.c
            ${CMAKE_CURRENT_SOURCE_DIR}/wepoll/sys/epoll.cpp)
    #Add wepoll header file directory
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/wepoll)
    #Turn on epoll
    add_definitions(-DHAS_EPOLL)
  endif()
endif()
###################################################################

# Remove win32 adapter code
if(NOT WIN32)
  list(REMOVE_ITEM ToolKit_SRC_LIST ${ToolKit_ROOT}/win32/getopt.c)
else()
  # Prevent Windows.h from including Winsock.h
  list(APPEND COMPILE_DEFINITIONS
    WIN32_LEAN_AND_MEAN MP4V2_NO_STDINT_DEFS
    # Disable warning
    _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

# Add library
add_library(s3toolkit STATIC ${ToolKit_SRC_LIST})
add_library(S3MediaKit::ToolKit ALIAS s3toolkit)
target_compile_definitions(s3toolkit
  PUBLIC ${COMPILE_DEFINITIONS})
target_compile_options(s3toolkit
  PRIVATE ${COMPILE_OPTIONS_DEFAULT})
target_include_directories(s3toolkit
  PRIVATE
    "$<BUILD_INTERFACE:${ToolKit_ROOT}/src>"
  PUBLIC
    "$<BUILD_INTERFACE:${ToolKit_ROOT}>/src")

update_cached_list(MK_LINK_LIBRARIES S3MediaKit::ToolKit)

if(USE_SOLUTION_FOLDERS AND (NOT GROUP_BY_EXPLORER))
  # Group files in the IDE, separate source files and header files
  set_file_group(${ToolKit_ROOT}/src ${ToolKit_SRC_LIST})
endif()

# Not in use
if(ENABLE_CXX_API)
  # Keep directory structure
  install(DIRECTORY ${ToolKit_ROOT}/
    DESTINATION ${INSTALL_PATH_INCLUDE}/ZLToolKit
    REGEX "(.*[.](md|cpp)|win32)$" EXCLUDE)
  install(TARGETS zltoolkit
    DESTINATION ${INSTALL_PATH_LIB})
endif()
