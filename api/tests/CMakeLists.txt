aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} TEST_SRC_LIST)

foreach(TEST_SRC ${TEST_SRC_LIST})
  get_filename_component(TEST_EXE_NAME ${TEST_SRC} NAME_WE)

  if(NOT ENABLE_FFMPEG)
    # Filter out test modules that depend on FFmpeg
    if("${TEST_EXE_NAME}" MATCHES "player_opencv")
      continue()
    endif()
  endif()

  message(STATUS "add c api tester: ${TEST_EXE_NAME}")
  set(exe_name api_tester_${TEST_EXE_NAME})
  add_executable(${exe_name} ${TEST_SRC})
  if(USE_SOLUTION_FOLDERS)
    set_property(TARGET ${exe_name} PROPERTY FOLDER "api_test")
  endif()

  target_link_libraries(${exe_name} mk_api)
  target_compile_options(${exe_name} PRIVATE ${COMPILE_OPTIONS_DEFAULT})
endforeach()
