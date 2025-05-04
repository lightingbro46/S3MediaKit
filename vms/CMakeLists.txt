file(GLOB VMS_SRC_LIST *.cpp *.h *.hpp)

add_library(s3vmskit STATIC ${VMS_SRC_LIST})
add_library(S3MediaKit::VmsKit ALIAS s3vmskit)
target_link_libraries(s3vmskit
  PUBLIC
    S3MediaKit::MediaKit
    S3MediaKit::ToolKit)
target_compile_options(s3vmskit
  PRIVATE ${COMPILE_OPTIONS_DEFAULT})
target_include_directories(s3vmskit
  PRIVATE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>")

update_cached_list(MK_COMPILE_DEFINITIONS ENABLE_VMS)
update_cached_list(MK_LINK_LIBRARIES S3MediaKit::VmsKit)

message(STATUS "vmskit function is enabled")