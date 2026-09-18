if(NOT ENABLE_TIER_STORAGE OR NOT ENABLE_AWS_SDK)
  return()
endif()

find_package(GTest 1.10 REQUIRED)
include(GoogleTest)

add_executable(integration_tier_storage_minio
  test_tier_object_storage.cpp
  test_object_storage_metrics.cpp)
target_compile_options(integration_tier_storage_minio PRIVATE ${COMPILE_OPTIONS_DEFAULT})
target_compile_definitions(integration_tier_storage_minio PRIVATE ${MK_COMPILE_DEFINITIONS})
target_link_libraries(integration_tier_storage_minio
  PRIVATE
    GTest::gtest
    GTest::gtest_main
    S3MediaKit::ManagerKit
    S3MediaKit::ext-codec)
target_include_directories(integration_tier_storage_minio PRIVATE
  "${PROJECT_SOURCE_DIR}/manager")

gtest_discover_tests(integration_tier_storage_minio
  TEST_PREFIX "integration."
  PROPERTIES
    LABELS "integration;tier-storage"
    TIMEOUT 90)
add_test(NAME integration.tier_storage_minio_all
  COMMAND integration_tier_storage_minio)
set_tests_properties(integration.tier_storage_minio_all PROPERTIES
  LABELS "integration;tier-storage"
  TIMEOUT 90)

add_executable(integration_tier_storage_manager
  test_tier_storage_manager.cpp
  tier_storage_test_stubs.cpp)
target_compile_options(integration_tier_storage_manager PRIVATE ${COMPILE_OPTIONS_DEFAULT})
target_compile_definitions(integration_tier_storage_manager PRIVATE
  ${MK_COMPILE_DEFINITIONS}
  TIER_TEST_MIGRATION_DIR="${PROJECT_SOURCE_DIR}/migration/mserver_updates"
  TIER_TEST_ESC_MIGRATION_DIR="${PROJECT_SOURCE_DIR}/migration/updates")
target_link_libraries(integration_tier_storage_manager
  PRIVATE
    GTest::gtest
    -Wl,--start-group
    S3MediaKit::MediaKit
    S3MediaKit::ManagerKit
    S3MediaKit::ext-codec
    -Wl,--end-group)
target_include_directories(integration_tier_storage_manager PRIVATE
  "${PROJECT_SOURCE_DIR}/manager"
  "${PROJECT_SOURCE_DIR}/server")

foreach(manager_case
    MapsAppDirectoryBindMountToLogicalPoolRoot
    RegistersHotSegmentIdempotently
    RunsCycleAndMovesHotSegmentToNas
    MovesOnlyStartOwnedSegmentsAcrossOverlappingBoundary
    ReusesExistingNasTargetWhenSourceIsMissing
    BacksOffMissingSegmentJob
    DropsMissingSegmentAfterMaxAttempts
    RunsCycleAndMovesHotSegmentToMinio
    RestoresSegmentFromMinio
    ReusesActiveRestoreForDuplicateRequest
    RestoresAllCameraStreamsWhenStreamIdIsEmpty
    RestoreStatusPreservesTimeBlockTotalBytes
    ReusesEquivalentRestoreWindowJobsAndExtendsTtl
    MarksExpiredAndDeletesAfterApproval
    ProtectsRetentionUntilGuardsExpire
    DefersDeletionWhileTransferIsActive
    KeepsWorkPendingWhenTimefileIsUnreadable
    PressureDeletesOldestAcrossPoolAndStopsAtTarget
    AgeRetentionSkipsBlockedRangesAndSplitsAtCutoff
    PressureDeletionSplitsAroundProtectedVideo
    AgeRetentionSplitsAroundProtectedVideo
    ReconcilesRangesWhoseFilesAreMissing
    BackfillSkipsBlocksWithoutFiles
    RebuildClearsDirtyAfterSwapAndKeepsConcurrentBlocks
    AlignsMoveThresholdToSplitGranularity
    EnforcesRestoreConcurrencyLimit
    SearchEngineReflectsTierMetadata
    LoadsAndRefreshesSavedCameraRecordRoot
    PolicyLifecycleChangesCameraStorage
    ResolvesExistingEmptyAndSymlinkPlaybackFiles
    RejectsUnreadablePlaybackFile
    RequiresRestoreWhenObjectCacheIsMissing
    ValidatesRestoreWindowCacheByStreamCoverageAndTtl
    CleansOnlyExpiredJobScopedRestoreCache
    TracksPoolHealthAndBlocksPlaybackWhenOffline
    DefersTierMoveUntilDestinationPoolRecovers
    CancelsPendingJobAfterPolicyReassignment
    CancelsPendingJobAfterRetentionIncrease
    GuardsRangeAndOrphanDeletionUntilPoolRecovers
    BackfillsAtomicallyAndIdempotently
    BackfillsAcrossLocalMidnightIntoSeparateRanges
    BoundsAndCompactsTierMetadata
    SkipsOrphanBlockAndAdvancesCursor
    BackfillShapesRangesAroundExistingOnes
    CompletesEmptyBackfillWhenCameraHasNoTimeFile
    RecoversRunningWorkAcrossFreshProcesses
    DeletesStableStaleTemporaryMp4FromHotPools
    PreservesUnsafeTemporaryMp4Candidates
    DeletesCompletedMp4OnlyWhenMatchingTimeBlockIsMissing
    ReportsTemporaryMp4InDryRunWithoutDeleting
    UsesSmallerHotOrCameraRetentionForCleanup
    DoesNotDeleteTemporaryMp4ImmediatelyAfterRestart
    CleansRangeMetadataBeforeCompletingJob)

  add_test(
    NAME "integration.tier-manager.${manager_case}"
    COMMAND integration_tier_storage_manager
      "--gtest_filter=TierStorageManagerIntegrationTest.${manager_case}")
  set_tests_properties("integration.tier-manager.${manager_case}" PROPERTIES
    LABELS "integration;tier-storage;tier-manager"
    SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
    TIMEOUT 60)
endforeach()
