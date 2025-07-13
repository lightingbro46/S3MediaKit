#include <cmath>
#include <ctime>
#include <sstream>
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Record/Recorder.h"
#include "Local/TimeRecorder.h"
#include "Local/TimeQuery.h"
#include "Storage/MigrationHistory.h"
#include "Manager.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
using namespace managerkit;

namespace managerkit {

namespace Manager {
#define GENERAL_FIELD "manager."
const std::string kMediaServerDomain = GENERAL_FIELD"mediaServerDomain";
const std::string kCertSavePath = GENERAL_FIELD"certSavePath";
const std::string kMaxAllowedDevices = GENERAL_FIELD"maxAllowedDevices";

static onceToken token([]() {
    mINI::Instance()[kMediaServerDomain] = "";
    mINI::Instance()[kCertSavePath] = "./certs";
    mINI::Instance()[kMaxAllowedDevices] = 256;
});
} // namespace Manager

} // namespace managerkit

static void *manager_hook_tag = nullptr;

void installManagerHook () {

#ifdef ENABLE_MP4
    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastRecordMP4, [](BroadcastRecordMP4Args) {
        DebugL << "Record mp4 file " << info.app << " " << info.stream << " " << info.start_time << " " << info.time_len << " " << info.file_path;
        TimeBlock block;
        block.set_app(info.app);
        block.set_stream(info.stream);
        block.set_start_time(info.start_time);
        block.set_time_len(std::round(info.time_len));
        block.set_file_size(info.file_size);
        block.set_file_path(info.file_path);

        TimeRecorder::Instance().inputBlock(block);
    });
#endif // ENABLE_MP4

#ifdef ENABLE_MKV
    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastRecordMKV, [](BroadcastRecordMKVArgs) {
        TraceL << "Record mkv file " << info.app << " " << info.stream << " " << info.start_time << " " << info.time_len << " " << info.file_path;
        TimeBlock block;
        block.set_app(info.app);
        block.set_stream(info.stream);
        block.set_start_time(info.start_time);
        block.set_time_len(std::round(info.time_len));
        block.set_file_size(info.file_size);
        block.set_file_path(info.file_path);

        TimeRecorder::Instance().inputBlock(block);
    });
#endif // ENABLE_MKV

#if defined(ENABLE_MP4) || defined(ENABLE_MKV)
    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastMediaSeeked, [](BroadcastMediaSeekedArgs) {
        auto infos = split(args.stream, "/");
        MediaTuple tuple = { args.vhost, infos[0], infos[1], "" };
        auto query = std::make_shared<TimeQuery>(tuple);
        int64_t offset = query->getOffsetOfDate(stamp);
        invoker(offset);
    });
#endif // defined(ENABLE_MP4) || defined(ENABLE_MKV)

}

void unInstallManagerHook() {
    NoticeCenter::Instance().delListener(&manager_hook_tag);
}

void migrateDatabase() {
    TraceL << "Prepare migrating local media server database";
    auto localDbMigrate = std::make_shared<MigrationHistoryImp>(Database::kMediaServerDb);
    GET_CONFIG(string, mserverUpdateSavePath, Database::kMServerMigrationSavePath)
    localDbMigrate->migrate(mserverUpdateSavePath);

    TraceL << "Prepare migrating edge storage database";
    auto escDbMigrate = std::make_shared<MigrationHistoryImp>(Database::kEdgeStorageControllerDb);
    GET_CONFIG(string, escUpdateSavePath, Database::kESCMigrationSavePath)
    escDbMigrate->migrate(escUpdateSavePath);
}