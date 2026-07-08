set(COMPILE_DEFINITIONS)
set(INCLUDE_DIRECTORIES)

file(GLOB_RECURSE PLUGIN_SRC_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)

add_library(ext-plugin STATIC ${PLUGIN_SRC_LIST})
add_library(S3MediaKit::ext-plugin ALIAS ext-plugin)

target_compile_options(ext-plugin PRIVATE ${COMPILE_OPTIONS_DEFAULT})
target_compile_definitions(ext-plugin PUBLIC ${COMPILE_DEFINITIONS})

target_link_libraries(ext-plugin PRIVATE S3MediaKit::ManagerKit ${LINK_LIBRARIES})

target_include_directories(ext-plugin 
  PRIVATE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
  PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
  ${INCLUDE_DIRECTORIES})

update_cached_list(MK_COMPILE_DEFINITIONS ENABLE_PLUGIN)
update_cached_list(MK_LINK_LIBRARIES S3MediaKit::ext-plugin ${LINK_LIBRARIES})

message(STATUS "plugin function is enabled")