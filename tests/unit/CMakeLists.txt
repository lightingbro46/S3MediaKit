find_package(GTest 1.10 REQUIRED)
include(GoogleTest)

function(add_s3_unit_test target)
  add_executable(${target} ${ARGN})
  target_compile_options(${target} PRIVATE ${COMPILE_OPTIONS_DEFAULT})
  target_compile_definitions(${target} PRIVATE ${MK_COMPILE_DEFINITIONS})
  target_link_libraries(${target}
    PRIVATE
      GTest::gtest
      GTest::gtest_main
      S3MediaKit::MediaKit)
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/manager")

  # Ubuntu packages GoogleMock separately on some releases. Tests that need
  # mocks may use it when available without making the base suite depend on it.
  if(TARGET GTest::gmock)
    target_link_libraries(${target} PRIVATE GTest::gmock)
  endif()

  gtest_discover_tests(${target}
    TEST_PREFIX "unit."
    PROPERTIES
      LABELS "unit"
      TIMEOUT 15)
endfunction()

add_s3_unit_test(unit_common_str_util
  common/test_str_util.cpp
  "${PROJECT_SOURCE_DIR}/manager/Common/StrUtil.cpp")
