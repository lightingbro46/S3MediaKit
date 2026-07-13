if(NOT DEFINED BUILD_DIR)
  set(BUILD_DIR "${CMAKE_SOURCE_DIR}/build-test")
endif()
if(NOT DEFINED COVERAGE_THRESHOLD)
  set(COVERAGE_THRESHOLD 40)
endif()

find_program(GCOVR_EXECUTABLE gcovr REQUIRED)
file(MAKE_DIRECTORY "${BUILD_DIR}/coverage-html")

set(common_args
  --root "${CMAKE_SOURCE_DIR}"
  --filter "${CMAKE_SOURCE_DIR}/src/"
  --filter "${CMAKE_SOURCE_DIR}/manager/"
  --filter "${CMAKE_SOURCE_DIR}/server/"
  --exclude "${CMAKE_SOURCE_DIR}/3rdpart/"
  --exclude "${CMAKE_SOURCE_DIR}/tests/"
  --exclude ".*\\.pb\\.(cc|h)$"
  --exclude-unreachable-branches)

execute_process(
  COMMAND "${GCOVR_EXECUTABLE}" ${common_args}
          --xml-pretty
          --output "${BUILD_DIR}/coverage.xml"
          --print-summary
          --fail-under-line "${COVERAGE_THRESHOLD}"
          "${BUILD_DIR}"
  RESULT_VARIABLE coverage_result)

execute_process(
  COMMAND "${GCOVR_EXECUTABLE}" ${common_args}
          --html
          --output "${BUILD_DIR}/coverage-html/index.html"
          "${BUILD_DIR}"
  RESULT_VARIABLE html_result)

if(NOT coverage_result EQUAL 0 OR NOT html_result EQUAL 0)
  message(FATAL_ERROR
    "Line coverage is below ${COVERAGE_THRESHOLD}% or report generation failed")
endif()
