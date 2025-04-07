file(GLOB MediaServer_SRC_LIST ./*.cpp ./*.h)

set(COMPILE_DEFINITIONS ${MK_COMPILE_DEFINITIONS})

if(ENABLE_SERVER_LIB)
  list(APPEND COMPILE_DEFINITIONS DISABLE_MAIN)
  add_library(MediaServer STATIC ${MediaServer_SRC_LIST})
  target_compile_definitions(MediaServer
    PRIVATE ${COMPILE_DEFINITIONS})
  target_compile_options(MediaServer
    PRIVATE ${COMPILE_OPTIONS_DEFAULT})
  target_link_libraries(MediaServer
    PRIVATE ${MK_LINK_LIBRARIES})
  update_cached_list(MK_LINK_LIBRARIES MediaServer)
  return()
endif()

# IOS does not compile executable programs, only dependency libraries
if(IOS)
  add_library(MediaServer STATIC ${MediaServer_SRC_LIST})
else()
  add_executable(MediaServer ${MediaServer_SRC_LIST})
endif()

target_compile_definitions(MediaServer
  PRIVATE ${COMPILE_DEFINITIONS})
target_compile_options(MediaServer
  PRIVATE ${COMPILE_OPTIONS_DEFAULT})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  target_link_libraries(MediaServer -Wl,--start-group ${MK_LINK_LIBRARIES} -Wl,--end-group)
else()
  target_link_libraries(MediaServer ${MK_LINK_LIBRARIES})
endif()

if(MSVC)
  set(RESOURCE_FILE "${CMAKE_SOURCE_DIR}/resource.rc")
  set_source_files_properties(${RESOURCE_FILE} PROPERTIES LANGUAGE RC)  
  target_sources(MediaServer PRIVATE ${RESOURCE_FILE})
else()
  # Android, IOS, macOS ...
  # CLion, GCC ...
endif()

install(TARGETS MediaServer DESTINATION ${INSTALL_PATH_RUNTIME})
