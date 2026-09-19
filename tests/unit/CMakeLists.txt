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
add_s3_unit_test(unit_http http/test_http_const.cpp http/test_http_cookie.cpp http/test_http_cookie_manager.cpp http/test_hls_viewer_session.cpp)
target_link_libraries(unit_http PRIVATE S3MediaKit::ext-codec)
add_s3_unit_test(unit_rtcp rtcp/test_rtcp_fci.cpp)
target_sources(unit_rtcp PRIVATE rtcp/test_rtcp_packets.cpp rtcp/test_rtcp_context.cpp)
add_s3_unit_test(unit_rtmp rtmp/test_amf.cpp rtmp/test_rtmp_utils.cpp rtmp/test_flv_splitter.cpp rtmp/test_rtmp_protocol.cpp rtmp/test_rtmp_demuxer.cpp)
target_link_libraries(unit_rtmp PRIVATE S3MediaKit::ext-codec)
add_test(NAME unit.rtmp_all COMMAND unit_rtmp)
set_tests_properties(unit.rtmp_all PROPERTIES LABELS unit TIMEOUT 15)
add_s3_unit_test(unit_hls http/test_hls_parser.cpp)
target_sources(unit_hls PRIVATE record/test_hls_maker.cpp record/test_hls_media_source.cpp record/test_recorder.cpp record/test_mp4_io.cpp record/test_mpeg_muxer.cpp)
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
target_sources(unit_storage PRIVATE storage/test_storage_models.cpp manager/test_motion_search.cpp)
target_sources(unit_storage PRIVATE manager/test_record_policy.cpp manager/test_device_source.cpp manager/test_resource_monitor.cpp manager/test_time_file.cpp)
target_link_libraries(unit_storage PRIVATE S3MediaKit::ManagerKit S3MediaKit::ext-codec)
add_test(NAME unit.storage_all COMMAND unit_storage)

add_s3_unit_test(unit_extract_job storage/test_extract_job.cpp)
target_link_libraries(unit_extract_job PRIVATE S3MediaKit::ManagerKit)
add_test(NAME unit.extract_job COMMAND unit_extract_job)
set_tests_properties(unit.storage_all PROPERTIES LABELS unit TIMEOUT 15)

if(ENABLE_SYNC_CHANGES)
  # Uses a temporary on-disk ESC database built from the real migrations.
  # CameraStatistic pulls in camera/stream code that references server/ symbols;
  # reuse the tier-storage stubs for those.
  add_s3_unit_test(unit_sync_apply
    manager/test_sync_apply.cpp
    manager/test_camera_statistic_sync.cpp
    "${PROJECT_SOURCE_DIR}/tests/integration/tier_storage/tier_storage_test_stubs.cpp")
  target_link_libraries(unit_sync_apply PRIVATE
    -Wl,--start-group
    S3MediaKit::MediaKit
    S3MediaKit::ManagerKit
    S3MediaKit::ext-codec
    -Wl,--end-group)
  target_include_directories(unit_sync_apply PRIVATE "${PROJECT_SOURCE_DIR}/server")
  target_compile_definitions(unit_sync_apply PRIVATE
    S3_ESC_MIGRATION_DIR="${PROJECT_SOURCE_DIR}/migration/updates"
    S3_MSERVER_MIGRATION_DIR="${PROJECT_SOURCE_DIR}/migration/mserver_updates")
  add_test(NAME unit.sync_apply_all COMMAND unit_sync_apply)
  set_tests_properties(unit.sync_apply_all PROPERTIES LABELS unit TIMEOUT 30)
endif()

# API worker dispatcher (server/ApiWorkDispatcher.cpp), compiled directly like the storage API test.
add_s3_unit_test(unit_api_work_dispatcher
  server/test_api_work_dispatcher.cpp
  "${PROJECT_SOURCE_DIR}/server/ApiWorkDispatcher.cpp"
  "${PROJECT_SOURCE_DIR}/server/WebApiErrCode.cpp")
target_link_libraries(unit_api_work_dispatcher PRIVATE
  -Wl,--start-group
  S3MediaKit::MediaKit
  S3MediaKit::ManagerKit
  S3MediaKit::ext-codec
  -Wl,--end-group)
target_include_directories(unit_api_work_dispatcher PRIVATE "${PROJECT_SOURCE_DIR}/server")
add_test(NAME unit.api_work_dispatcher_all COMMAND unit_api_work_dispatcher)
set_tests_properties(unit.api_work_dispatcher_all PROPERTIES LABELS unit TIMEOUT 30)

# Shared blocking thread pools (manager/Common/BlockingExecutors).
add_s3_unit_test(unit_blocking_executors manager/test_blocking_executors.cpp manager/test_serial_task_queue.cpp)
target_link_libraries(unit_blocking_executors PRIVATE S3MediaKit::ManagerKit)
add_test(NAME unit.blocking_executors_all COMMAND unit_blocking_executors)
set_tests_properties(unit.blocking_executors_all PROPERTIES LABELS unit TIMEOUT 30)

# SubnetScan with an injected probe; server/ symbols come from the tier-storage stubs.
add_s3_unit_test(unit_subnet_scan
  manager/test_subnet_scan.cpp
  "${PROJECT_SOURCE_DIR}/tests/integration/tier_storage/tier_storage_test_stubs.cpp")
target_link_libraries(unit_subnet_scan PRIVATE
  -Wl,--start-group
  S3MediaKit::MediaKit
  S3MediaKit::ManagerKit
  S3MediaKit::ext-codec
  -Wl,--end-group)
target_include_directories(unit_subnet_scan PRIVATE "${PROJECT_SOURCE_DIR}/server")
add_test(NAME unit.subnet_scan_all COMMAND unit_subnet_scan)
set_tests_properties(unit.subnet_scan_all PROPERTIES LABELS unit TIMEOUT 30)

# FFmpegExtractor start/cancel (server/FFmpegSource.cpp compiled directly); remaining server/
# symbols come from the tier-storage stubs.
add_s3_unit_test(unit_extract_start
  server/test_extract_start.cpp
  "${PROJECT_SOURCE_DIR}/server/FFmpegSource.cpp"
  "${PROJECT_SOURCE_DIR}/server/Process.cpp"
  "${PROJECT_SOURCE_DIR}/server/System.cpp"
  "${PROJECT_SOURCE_DIR}/server/WebApiErrCode.cpp"
  "${PROJECT_SOURCE_DIR}/tests/integration/tier_storage/tier_storage_test_stubs.cpp")
target_link_libraries(unit_extract_start PRIVATE
  -Wl,--start-group
  S3MediaKit::MediaKit
  S3MediaKit::ManagerKit
  S3MediaKit::ext-codec
  -Wl,--end-group)
target_include_directories(unit_extract_start PRIVATE "${PROJECT_SOURCE_DIR}/server")
target_compile_definitions(unit_extract_start PRIVATE
  S3_MSERVER_MIGRATION_DIR="${PROJECT_SOURCE_DIR}/migration/mserver_updates")
add_test(NAME unit.extract_start_all COMMAND unit_extract_start)
set_tests_properties(unit.extract_start_all PROPERTIES LABELS unit TIMEOUT 30)

if(ENABLE_TIER_STORAGE)
  add_s3_unit_test(unit_storage_pool_api
    server/test_storage_pool_api.cpp
    "${PROJECT_SOURCE_DIR}/server/WebApiErrCode.cpp")
  target_link_libraries(unit_storage_pool_api PRIVATE
    S3MediaKit::ManagerKit
    S3MediaKit::ext-codec)
  target_include_directories(unit_storage_pool_api PRIVATE
    "${PROJECT_SOURCE_DIR}/server")
  add_test(NAME unit.storage_pool_api_all COMMAND unit_storage_pool_api)
  set_tests_properties(unit.storage_pool_api_all PROPERTIES LABELS unit TIMEOUT 15)

  # Tier read-then-write transactions against a temporary mserver DB built from the real migrations.
  add_s3_unit_test(unit_tier_txn_lock storage/test_tier_txn_lock.cpp)
  target_link_libraries(unit_tier_txn_lock PRIVATE
    S3MediaKit::ManagerKit
    S3MediaKit::ext-codec)
  target_compile_definitions(unit_tier_txn_lock PRIVATE
    S3_MSERVER_MIGRATION_DIR="${PROJECT_SOURCE_DIR}/migration/mserver_updates")
  add_test(NAME unit.tier_txn_lock_all COMMAND unit_tier_txn_lock)
  set_tests_properties(unit.tier_txn_lock_all PROPERTIES LABELS unit TIMEOUT 30)
endif()

if(ENABLE_SERVER_LIB)
  add_executable(unit_extract_preview
    server/test_extract_preview.cpp
    server/test_ffmpeg_overlay_filter.cpp)
  target_compile_options(unit_extract_preview PRIVATE ${COMPILE_OPTIONS_DEFAULT})
  target_compile_definitions(unit_extract_preview PRIVATE ${MK_COMPILE_DEFINITIONS})
  target_link_libraries(unit_extract_preview
    PRIVATE
      GTest::gtest
      GTest::gtest_main)
  if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_libraries(unit_extract_preview
      PRIVATE
        -Wl,--start-group
        MediaServer
        ${MK_LINK_LIBRARIES}
        -Wl,--end-group)
  else()
    target_link_libraries(unit_extract_preview
      PRIVATE
        MediaServer
        ${MK_LINK_LIBRARIES})
  endif()
  target_include_directories(unit_extract_preview PRIVATE
    "${PROJECT_SOURCE_DIR}/manager"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/3rdpart/S3ToolKit/src"
    "${PROJECT_SOURCE_DIR}/3rdpart/jsoncpp/include"
    "${PROJECT_SOURCE_DIR}/server"
    "${CMAKE_BINARY_DIR}/manager"
    "${PROJECT_SOURCE_DIR}/3rdpart/onvif/core/include"
    "${PROJECT_SOURCE_DIR}/3rdpart/onvif/core/include/gsoap")
  gtest_discover_tests(unit_extract_preview
    TEST_PREFIX "unit."
    PROPERTIES
      LABELS "unit"
      TIMEOUT 15)
  add_test(NAME unit.extract_preview_all COMMAND unit_extract_preview)
  set_tests_properties(unit.extract_preview_all PROPERTIES LABELS unit TIMEOUT 15)
endif()

add_s3_unit_test(unit_hls_reader_monitor manager/test_hls_reader_monitor.cpp)
target_link_libraries(unit_hls_reader_monitor PRIVATE S3MediaKit::ManagerKit)
