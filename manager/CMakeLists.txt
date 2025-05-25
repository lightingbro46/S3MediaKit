# protocpp
find_package(Protobuf REQUIRED)

set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(GENERATED_PROTO_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR})

file(GLOB PROTO_FILES "${PROTO_DIR}/*.proto")

protobuf_generate(
  LANGUAGE cpp
  BASE_DIR ${PROTO_DIR}
  IMPORT_DIRS ${PROTO_DIR}
  PROTOS ${PROTO_FILES}
  OUT_VAR GENERATED_PROTO_SRCS
)

add_library(protocpp STATIC ${GENERATED_PROTO_SRCS})
target_link_libraries(protocpp
  PUBLIC
    ${Protobuf_LIBRARIES}
)

target_compile_options(protocpp
  PRIVATE ${COMPILE_OPTIONS_DEFAULT})
target_include_directories(protocpp
  PRIVATE
    "$<BUILD_INTERFACE:${GENERATED_PROTO_INCLUDE_DIR}>"
  PUBLIC
    "$<BUILD_INTERFACE:${GENERATED_PROTO_INCLUDE_DIR}>")

##############################################################################
# manager
file(GLOB MANAGER_SRC_LIST 
  ${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp 
  ${CMAKE_CURRENT_SOURCE_DIR}/*/*.h 
  ${CMAKE_CURRENT_SOURCE_DIR}/*/*.c)

add_library(s3managerkit STATIC ${MANAGER_SRC_LIST})

add_library(S3MediaKit::ManagerKit ALIAS s3managerkit)

target_link_libraries(s3managerkit
  PUBLIC
    S3MediaKit::MediaKit
    S3MediaKit::ToolKit
    protocpp)

target_compile_definitions(s3managerkit 
  PUBLIC ${COMPILE_DEFINITIONS})
target_compile_options(s3managerkit
  PRIVATE ${COMPILE_OPTIONS_DEFAULT})
  
target_include_directories(s3managerkit
  PRIVATE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<BUILD_INTERFACE:${GENERATED_PROTO_INCLUDE_DIR}>")

update_cached_list(MK_LINK_LIBRARIES S3MediaKit::ManagerKit)