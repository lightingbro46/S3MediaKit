/**
 * WebApiStorage.h
 *
 * Bodies shared by the two storage-tiering endpoint families:
 *
 *   /media/mserver/storage/*   behind CHECK_AUTH_TOKEN() + CHECK_USER_PERMISSION  (WebApiStorage.cpp)
 *   /media/api/storage/*       behind CHECK_SECRET()                              (WebApi.cpp)
 *
 * Only the body is shared. Each family keeps its own guards and its own registration, because the
 * two have different trust boundaries: merging them into an HTTP forwarder would move that
 * boundary. The functions below are plain functions, so a route added to one family cannot silently
 * diverge from the other — which is what used to happen (one family dispatched to the API worker,
 * the other still ran on the request poller).
 *
 * Contract of every function here:
 *   - runs on the API worker pool (blocking: SQLite, filesystem, object storage);
 *   - fills `val` (the `data` field and, on error, the fields the response must keep);
 *   - throws ApiResponseException for business errors, like RETURN_API_RESPONSE(code, msg).
 *
 * Both families answer the same payload; where they used to differ, the `/media/mserver/storage/*`
 * payload is the one kept.
 */

#ifndef S3MEDIAKIT_WEBAPI_STORAGE_H
#define S3MEDIAKIT_WEBAPI_STORAGE_H

#include "json/json.h"
#include "WebApi.h"

#ifdef ENABLE_TIER_STORAGE

namespace managerkit {

// --- Section 1: storage pools -------------------------------------------------------------
void storagePoolList(const ArgsMap &allArgs, Json::Value &val);
void storagePoolDetail(const ArgsMap &allArgs, Json::Value &val);
void storagePoolCreate(const ArgsJson &allArgs, Json::Value &val);
void storagePoolUpdate(const ArgsJson &allArgs, Json::Value &val);
void storagePoolDelete(const ArgsMap &allArgs, Json::Value &val);
void storagePoolEncryptChangePassword(const ArgsMap &allArgs, Json::Value &val);
void storagePoolTestConnection(const ArgsJson &allArgs, Json::Value &val);
// Constant answer (no blocking work): both families call it on the request poller.
void storagePoolOptions(Json::Value &val);

// --- Section 2: storage policies ------------------------------------------------------------
void storagePolicyList(const ArgsMap &allArgs, Json::Value &val);
void storagePolicyDetail(const ArgsMap &allArgs, Json::Value &val);
void storagePolicyCreate(const ArgsJson &allArgs, Json::Value &val);
void storagePolicyUpdate(const ArgsJson &allArgs, Json::Value &val);
void storagePolicyDelete(const ArgsMap &allArgs, Json::Value &val);

// --- Section 3: policy assignment (camera level) ---------------------------------------------
void storagePolicyAssignCamera(const ArgsMap &allArgs, Json::Value &val);
void storagePolicyAssignCameras(const ArgsJson &allArgs, Json::Value &val);
void storagePolicyRemoveCamera(const ArgsMap &allArgs, Json::Value &val);
void storagePolicyRemoveCameras(const ArgsJson &allArgs, Json::Value &val);

// --- Section 4: camera storage ----------------------------------------------------------------
void storageCameraEffectivePolicyList(const ArgsMap &allArgs, Json::Value &val);
void storageCameraTimeline(const ArgsMap &allArgs, Json::Value &val);
void storageCameraSummary(const ArgsMap &allArgs, Json::Value &val);

// --- Sections 5-6: restore / tiering jobs -----------------------------------------------------
void storageRestoreJobList(const ArgsMap &allArgs, Json::Value &val);
void storageTieringJobList(const ArgsMap &allArgs, Json::Value &val);

// --- Section 7: dashboard ---------------------------------------------------------------------
void storageDashboardSummary(Json::Value &val);
void storageDashboardDetail(Json::Value &val);

// --- Sections 8-10: alerts, expired segments, protected videos --------------------------------
void storageAlertList(const ArgsMap &allArgs, Json::Value &val);
void storageExpiredSegmentList(const ArgsMap &allArgs, Json::Value &val);
void storageProtectedList(const ArgsMap &allArgs, Json::Value &val);

// --- Mount points -----------------------------------------------------------------------------
void storageMountpointAvailable(const ArgsMap &allArgs, Json::Value &val);

} // namespace managerkit

#endif // ENABLE_TIER_STORAGE

#endif // S3MEDIAKIT_WEBAPI_STORAGE_H
