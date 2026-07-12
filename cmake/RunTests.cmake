if(NOT DEFINED BUILD_DIR)
  set(BUILD_DIR "${CMAKE_SOURCE_DIR}/build-test")
endif()

file(MAKE_DIRECTORY "${BUILD_DIR}/test-results")
execute_process(
  COMMAND "${CMAKE_CTEST_COMMAND}"
          --test-dir "${BUILD_DIR}"
          --output-on-failure
          --timeout 60
          --output-junit "${BUILD_DIR}/test-results/unit.xml"
  RESULT_VARIABLE test_result)

if(NOT test_result EQUAL 0)
  message(FATAL_ERROR "Unit tests failed with exit code ${test_result}")
endif()
