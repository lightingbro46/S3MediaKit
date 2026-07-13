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

add_s3_unit_test(unit_parser common/test_parser.cpp)
add_s3_unit_test(unit_stamp common/test_stamp.cpp)
add_s3_unit_test(unit_str_coding common/test_str_coding.cpp)
add_s3_unit_test(unit_http http/test_http_const.cpp http/test_http_cookie.cpp http/test_http_cookie_manager.cpp)
add_s3_unit_test(unit_rtcp rtcp/test_rtcp_fci.cpp)
target_sources(unit_rtcp PRIVATE rtcp/test_rtcp_packets.cpp rtcp/test_rtcp_context.cpp)
add_s3_unit_test(unit_rtmp rtmp/test_amf.cpp rtmp/test_rtmp_utils.cpp rtmp/test_flv_splitter.cpp rtmp/test_rtmp_protocol.cpp rtmp/test_rtmp_demuxer.cpp)
target_link_libraries(unit_rtmp PRIVATE S3MediaKit::ext-codec)
add_test(NAME unit.rtmp_all COMMAND unit_rtmp)
set_tests_properties(unit.rtmp_all PROPERTIES LABELS unit TIMEOUT 15)
add_s3_unit_test(unit_hls http/test_hls_parser.cpp)
target_sources(unit_hls PRIVATE record/test_hls_maker.cpp record/test_recorder.cpp record/test_mp4_io.cpp record/test_mpeg_muxer.cpp)
target_link_libraries(unit_hls PRIVATE S3MediaKit::ext-codec)
add_test(NAME unit.hls_all COMMAND unit_hls)
set_tests_properties(unit.hls_all PROPERTIES LABELS unit TIMEOUT 15)
add_s3_unit_test(unit_http_body http/test_http_body.cpp)
add_s3_unit_test(unit_http_splitters http/test_http_splitters.cpp http/test_websocket_splitter.cpp)
add_s3_unit_test(unit_xml onvif/test_pugixml.cpp onvif/test_soap_object.cpp)
add_s3_unit_test(unit_codec codec/test_h26x.cpp codec/test_audio_codec.cpp)
target_link_libraries(unit_codec PRIVATE S3MediaKit::ext-codec)
# Static codec registration logs during --gtest_list_tests, which prevents
# CMake 3.22's discovery parser from seeing the cases. Register this binary as
# one CTest test; GoogleTest still reports every assertion and case in output.
add_test(NAME unit.codec_all COMMAND unit_codec)
set_tests_properties(unit.codec_all PROPERTIES LABELS unit TIMEOUT 15)
add_s3_unit_test(unit_frame extension/test_frame.cpp rtmp/test_rtmp_packet.cpp)
target_sources(unit_frame PRIVATE extension/test_factory.cpp common/test_media_sink.cpp common/test_media_source.cpp common/test_packet_cache.cpp)
target_link_libraries(unit_frame PRIVATE S3MediaKit::ext-codec)
add_test(NAME unit.frame_all COMMAND unit_frame)
set_tests_properties(unit.frame_all PROPERTIES LABELS unit TIMEOUT 15)
add_s3_unit_test(unit_rtsp rtsp/test_rtsp.cpp)
target_sources(unit_rtsp PRIVATE rtsp/test_rtp_receiver.cpp)
target_link_libraries(unit_rtsp PRIVATE S3MediaKit::ext-codec)
add_test(NAME unit.rtsp_all COMMAND unit_rtsp)
set_tests_properties(unit.rtsp_all PROPERTIES LABELS unit TIMEOUT 15)
add_s3_unit_test(unit_storage storage/test_db_schema.cpp storage/test_storage_entities.cpp)
target_sources(unit_storage PRIVATE storage/test_storage_models.cpp)
target_sources(unit_storage PRIVATE manager/test_record_policy.cpp manager/test_device_source.cpp manager/test_resource_monitor.cpp manager/test_time_file.cpp)
target_link_libraries(unit_storage PRIVATE S3MediaKit::ManagerKit S3MediaKit::ext-codec)
add_test(NAME unit.storage_all COMMAND unit_storage)
set_tests_properties(unit.storage_all PROPERTIES LABELS unit TIMEOUT 15)
