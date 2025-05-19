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

file(GLOB_RECURSE MANAGER_SRC_LIST *.cpp *.h *.hpp)

add_library(s3managerkit STATIC ${MANAGER_SRC_LIST} ${GENERATED_PROTO_SRCS})

add_library(S3MediaKit::ManagerKit ALIAS s3managerkit)

target_link_libraries(s3managerkit
  PUBLIC
    S3MediaKit::MediaKit
    S3MediaKit::ToolKit
    ${Protobuf_LIBRARIES}
)

target_compile_options(s3managerkit
  PRIVATE ${COMPILE_OPTIONS_DEFAULT})
  
target_include_directories(s3managerkit
  PRIVATE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
    "$<BUILD_INTERFACE:${GENERATED_PROTO_INCLUDE_DIR}>"
)

update_cached_list(MK_COMPILE_DEFINITIONS ENABLE_MANAGER)
update_cached_list(MK_LINK_LIBRARIES S3MediaKit::ManagerKit)

message(STATUS "managerkit function is enabled")