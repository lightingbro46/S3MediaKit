# Find out if srtp is installed
find_package(SRTP QUIET)
if(NOT SRTP_FOUND OR NOT ENABLE_OPENSSL)
  set(ENABLE_WEBRTC OFF)
  message(WARNING "srtp not found, WebRTC related functions failed to open")
  return()
endif()


message(STATUS "found library: ${SRTP_LIBRARIES}")
include_directories(SYSTEM ${SRTP_INCLUDE_DIRS})

set(LINK_LIBRARIES ${SRTP_LIBRARIES})

set(COMPILE_DEFINITIONS)
set(INCLUDE_DIRECTORIES)

if(ENABLE_SCTP)
find_package(SCTP QUIET)
if(SCTP_FOUND)
  message(STATUS "found library: ${SCTP_INCLUDE_DIRS} ${SCTP_LIBRARIES}")
  include_directories(SYSTEM ${SCTP_INCLUDE_DIRS})
  # TODO: Avoid exposing SCTP outside of WebRTC modules
  list(APPEND COMPILE_DEFINITIONS ENABLE_SCTP)
  list(APPEND INCLUDE_DIRECTORIES ${SCTP_INCLUDE_DIRS})
  list(APPEND LINK_LIBRARIES ${SCTP_LIBRARIES})
  message(STATUS "WebRTC datachannel function is turned on")
endif()
endif()

file(GLOB WEBRTC_SRC_LIST
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
add_library(webrtc STATIC ${WEBRTC_SRC_LIST})
add_library(S3MediaKit::WebRTC ALIAS webrtc)
target_compile_definitions(webrtc
  # ENABLE_SCTP temporarily needs to be exposed
  PUBLIC ${COMPILE_DEFINITIONS})
target_compile_options(webrtc
  PRIVATE ${COMPILE_OPTIONS_DEFAULT})
target_link_libraries(webrtc
  PRIVATE
    S3MediaKit::MediaKit
    S3MediaKit::ToolKit
    ${LINK_LIBRARIES})
target_include_directories(webrtc
  PRIVATE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
    ${INCLUDE_DIRECTORIES})

message(STATUS "WebRTC function is enabled")

update_cached_list(MK_COMPILE_DEFINITIONS ENABLE_WEBRTC)
update_cached_list(MK_LINK_LIBRARIES S3MediaKit::WebRTC ${LINK_LIBRARIES})
