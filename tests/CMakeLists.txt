aux_source_directory(. TEST_SRC_LIST)

find_package(PCAP QUIET)

foreach(TEST_SRC ${TEST_SRC_LIST})
  get_filename_component(TEST_EXE_NAME ${TEST_SRC} NAME_WE)

  if(NOT PCAP_FOUND)
    # message(WARNING "PCAP not found")
    if("${TEST_EXE_NAME}" MATCHES "test_rtp_pcap")
      continue()
    endif()
  endif()
  
  if(NOT TARGET S3MediaKit::WebRTC)
    # Temporarily filter out test modules that depend on WebRTC
    if("${TEST_EXE_NAME}" MATCHES "test_rtcp_nack")
      continue()
    endif()
  endif()

  message(STATUS "add test: ${TEST_EXE_NAME}")
  add_executable(${TEST_EXE_NAME} ${TEST_SRC})
  target_compile_options(${TEST_EXE_NAME}
    PRIVATE ${COMPILE_OPTIONS_DEFAULT})

  target_compile_definitions(${TEST_EXE_NAME}
    PRIVATE ${MK_COMPILE_DEFINITIONS})

  if(USE_SOLUTION_FOLDERS)
    SET_PROPERTY(TARGET ${TEST_EXE_NAME} PROPERTY FOLDER "test")
  endif()

  if(CMAKE_SYSTEM_NAME MATCHES "Linux")

    target_link_libraries(${TEST_EXE_NAME} -Wl,--start-group ${MK_LINK_LIBRARIES} -Wl,--end-group)
  else()
    target_link_libraries(${TEST_EXE_NAME} ${MK_LINK_LIBRARIES})
  endif()
endforeach()

if(TARGET test_rtp_pcap)
  target_include_directories(test_rtp_pcap SYSTEM PRIVATE ${PCAP_INCLUDE_DIRS})
  target_link_libraries(test_rtp_pcap  ${PCAP_LIBRARIES})
endif()
