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
add_subdirectory(S3ToolKit)
# Add library alias
add_library(S3MediaKit::ToolKit ALIAS S3ToolKit)
# Add dependencies
update_cached_list(MK_LINK_LIBRARIES S3MediaKit::ToolKit)

###################################################################

# ONVIF
add_subdirectory(onvif/core)
update_cached_list(MK_LINK_LIBRARIES onvifcore)

###################################################################

# jwt-cpp
file(GLOB JWTCPP_SRC_LIST
  ${CMAKE_CURRENT_SOURCE_DIR}/jwt-cpp/include/jwt-cpp/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/jwt-cpp/include/picojson/*.h
)

add_library(jwtcpp INTERFACE)
target_include_directories(jwtcpp
  INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/jwt-cpp/include>"
)
update_cached_list(MK_LINK_LIBRARIES jwtcpp)