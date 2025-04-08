file(GLOB SRT_SRC_LIST *.cpp *.h *.hpp)

add_library(srt STATIC ${SRT_SRC_LIST})
add_library(S3MediaKit::SRT ALIAS srt)
target_link_libraries(srt
  PUBLIC
    S3MediaKit::MediaKit
    S3MediaKit::ToolKit)
target_compile_options(srt
  PRIVATE ${COMPILE_OPTIONS_DEFAULT})
target_include_directories(srt
  PRIVATE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>")

update_cached_list(MK_COMPILE_DEFINITIONS ENABLE_SRT)
update_cached_list(MK_LINK_LIBRARIES S3MediaKit::SRT)

message(STATUS "srt function is enabled")
