set(COMPILE_DEFINITIONS)
set(INCLUDE_DIRECTORIES)

file(GLOB EXT_SRC_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)

add_library(ext-codec STATIC ${EXT_SRC_LIST})
add_library(S3MediaKit::ext-codec ALIAS ext-codec)

target_compile_options(ext-codec PRIVATE ${COMPILE_OPTIONS_DEFAULT})
target_compile_definitions(ext-codec PUBLIC ${COMPILE_DEFINITIONS})

target_link_libraries(ext-codec PRIVATE S3MediaKit::MediaKit S3MediaKit::ToolKit ${LINK_LIBRARIES})

target_include_directories(ext-codec
        PRIVATE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
        PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
        ${INCLUDE_DIRECTORIES})

update_cached_list(MK_LINK_LIBRARIES S3MediaKit::ext-codec ${LINK_LIBRARIES})
