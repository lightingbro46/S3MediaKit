file(GLOB MediaKit_SRC_LIST
  ${CMAKE_CURRENT_SOURCE_DIR}/*/*.c
  ${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/*/*.h)

if(NOT ENABLE_SRT)
    file(GLOB SRT_SRC_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/Srt/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Srt/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Srt/*.h)
    list(REMOVE_ITEM MediaKit_SRC_LIST ${SRT_SRC_LIST})
endif()

if(USE_SOLUTION_FOLDERS AND (NOT GROUP_BY_EXPLORER))
  # Group files in the IDE, separate source files and header files
  set_file_group("${CMAKE_CURRENT_SOURCE_DIR}" ${MediaKit_SRC_LIST})
endif()


# Add library
add_library(zlmediakit STATIC ${MediaKit_SRC_LIST})
add_library(S3MediaKit::MediaKit ALIAS zlmediakit)

set(LINK_LIBRARIES ${MK_LINK_LIBRARIES})
list(REMOVE_ITEM LINK_LIBRARIES S3MediaKit::MediaKit)
set(COMPILE_DEFINITIONS ${MK_COMPILE_DEFINITIONS})

target_compile_definitions(zlmediakit
  PUBLIC ${COMPILE_DEFINITIONS})
target_compile_options(zlmediakit
  PRIVATE ${COMPILE_OPTIONS_DEFAULT})
target_link_libraries(zlmediakit
  PUBLIC ${LINK_LIBRARIES})
target_include_directories(zlmediakit
  PRIVATE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")

update_cached_list(MK_LINK_LIBRARIES S3MediaKit::MediaKit)

# Not in use
if(ENABLE_CXX_API)
  # Keep directory structure
  if(ENABLE_SRT)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
      DESTINATION ${INSTALL_PATH_INCLUDE}/S3MediaKit
      FILES_MATCHING
        PATTERN "*.h")
  else()
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
      DESTINATION ${INSTALL_PATH_INCLUDE}/S3MediaKit
      FILES_MATCHING 
        REGEX ".*/Srt.*" EXCLUDE
        PATTERN "*.h")
  endif()
  install(TARGETS zlmediakit
    DESTINATION ${INSTALL_PATH_LIB})
endif ()
