#include "RecordingStatistic.h"
#include "Common/StrUtil.h"
#include "Local/StorageTier.h"
#include "Server/ResourceMonitor.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

static Json::Value makeJsonKeyValue(const string &key, const string &value) {
    Json::Value ret;
    ret["name"] = key;
    ret["value"] = value;
    return ret;
}

// BookmarkStats
static Json::Value makeBookmarkStatsJson(BookmarkStats stats) {
    Json::Value ret;
    ret["recordAverageSizeB"] = stats.recordAverageSizeB;
    ret["recordCount"] = stats.recordCount;
    return ret;
}

static BookmarkStats getBookmarkStats(const Json::Value &data) {
    BookmarkStats stats;
    stats.recordAverageSizeB = data["recordAverageSizeB"].asUInt64();
    stats.recordCount = data["recordCount"].asUInt64();
    return stats;
}

// StreamStorageStats
static Json::Value makeStreamStorageStatsJson(const unordered_map<string, StreamStorageStats> storage_map, const unordered_map<int, StreamTuple> stream_map, int stream_type) {
    Json::Value ret = Json::objectValue;
    auto it_stream = stream_map.find(stream_type);
    if (it_stream != stream_map.end()) {
        auto stream_id = it_stream->second.stream_id;
        auto it_storage = storage_map.find(stream_id);
        if (it_storage != storage_map.end()) {
            ret["archiveIndexRecordCount"] = it_storage->second.archiveIndexRecordCount;
            ret["archiveSizeB"] = it_storage->second.archiveSizeB;
            ret["archiveStartTime"] = it_storage->second.archiveStartTime;
            ret["archiveEndTime"] = it_storage->second.archiveEndTime;
        }
    }
    return ret;
}

static StreamStorageStats getStreamStorageStats(const Json::Value &data) {
    StreamStorageStats stats;
    stats.archiveIndexRecordCount = data["archiveIndexRecordCount"].asUInt64();
    stats.archiveSizeB = data["archiveSizeB"].asUInt64();
    stats.archiveStartTime = data["archiveStartTime"].asUInt64();
    stats.archiveEndTime = data["archiveEndTime"].asUInt64();
    return stats;
}

// StreamStatistic
static Json::Value makeStreamStatisticJson(const unordered_map<int, StreamStatistic> &stats_map, int stream_type) {
    Json::Value ret = Json::objectValue;
    auto it_statistic = stats_map.find(stream_type);
    if (it_statistic != stats_map.end()) {
        ret["live"] = it_statistic->second.live;
        ret["last_change_status"] = it_statistic->second.last_change_status;
        ret["status"] = it_statistic->second.status;
        ret["byte_speed"] = it_statistic->second.byte_speed;
        ret["has_video"] = it_statistic->second.has_video;
        ret["vcodec"] = it_statistic->second.vcodec;
        ret["source_vcodec"] = it_statistic->second.source_vcodec;
        ret["transcoded"] = it_statistic->second.transcoded;
        ret["width"] = it_statistic->second.width;
        ret["height"] = it_statistic->second.height;
        ret["bitrate"] = it_statistic->second.bitrate;
        ret["fps"] = it_statistic->second.fps;
        ret["has_audio"] = it_statistic->second.has_audio;
        ret["acodec"] = it_statistic->second.acodec;
        ret["sample_rate"] = it_statistic->second.sample_rate;
        ret["channel_no"] = it_statistic->second.channel_no;
        ret["sample_bit"] = it_statistic->second.sample_bit;
    }
    return ret;
}

static StreamStatistic getStreamStatistic(const Json::Value &data) {
    StreamStatistic stats;
    stats.live = data["live"].asBool();
    stats.last_change_status = data["last_change_status"].asUInt64();
    stats.status = data["status"].asString();
    stats.byte_speed = data["byte_speed"].asInt();
    stats.has_video = data["has_video"].asBool();
    stats.vcodec = data["vcodec"].asString();
    stats.source_vcodec = data.isMember("source_vcodec") ? data["source_vcodec"].asString() : stats.vcodec;
    stats.transcoded = data["transcoded"].asBool();
    stats.width = data["width"].asInt();
    stats.height = data["height"].asInt();
    stats.bitrate = data["bitrate"].asInt();
    stats.fps = data["fps"].asFloat();
    stats.has_audio = data["has_audio"].asBool();
    stats.acodec = data["acodec"].asString();
    stats.sample_rate = data["sample_rate"].asInt();
    stats.channel_no = data["channel_no"].asInt();
    stats.sample_bit = data["sample_bit"].asInt();
    return stats;
}

static Json::Value makeTransportStatsJson(const TransportStats &stats) {
    Json::Value ret = Json::objectValue;
    ret["liveTransports"] = Json::arrayValue;
    for (const auto &transport : stats.liveTransports) {
        ret["liveTransports"].append(transport);
    }
    ret["replayTransports"] = Json::arrayValue;
    for (const auto &transport : stats.replayTransports) {
        ret["replayTransports"].append(transport);
    }
    return ret;
}

static TransportStats getTransportStats(const Json::Value &data) {
    TransportStats stats;
    for (const auto &transport : data["liveTransports"]) {
        stats.liveTransports.push_back(transport.asString());
    }
    for (const auto &transport : data["replayTransports"]) {
        stats.replayTransports.push_back(transport.asString());
    }
    return stats;
}

// StreamTuple
Json::Value makeRecordingStreamTupleJson(const unordered_map<int, StreamTuple> &stream_map, int stream_type) {
    Json::Value ret = Json::objectValue;
    auto it = stream_map.find(stream_type);
    if (it != stream_map.end()) {
        ret["id"] = it->second.stream_id;
        ret["url"] = it->second.full_url;
    }
    return ret;
}

StreamTuple readRecordingStreamTuple(const Json::Value &data, const DeviceTuple &tuple, int stream_type) {
    StreamTuple stream;
    stream.vhost = tuple.vhost;
    stream.device_id = tuple.device_id;
    stream.name = getStreamTypeString(stream_type);
    stream.stream_id = data["id"].asString();
    stream.full_url = data["url"].asString();
    return stream;
}

// MotionStorageStats
static Json::Value makeMotionStorageStatsJson(const MotionStorageStats &stats) {
    Json::Value ret = Json::objectValue;
    ret["archiveStartTime"] = stats.archiveStartTime;
    ret["archiveEndTime"] = stats.archiveEndTime;
    return ret;
}

static MotionStorageStats getMotionStorageStats(const Json::Value &data) {
    MotionStorageStats stats;
    stats.archiveStartTime = !data["archiveStartTime"].empty() ? data["archiveStartTime"].asUInt64() : 0;
    stats.archiveEndTime = !data["archiveEndTime"].isNull() ? data["archiveEndTime"].asUInt64() : 0;
    return stats;
}

// TierStorageStats
static Json::Value makeTierStorageStatsJson(const unordered_map<int, TierStorageStats> stats_map, int tier_type) {
    Json::Value ret = Json::objectValue;
    auto it_statistic = stats_map.find(tier_type);
    if (it_statistic != stats_map.end()) {
        ret["archiveStartTime"] = it_statistic->second.archiveStartTime;
        ret["archiveEndTime"] = it_statistic->second.archiveEndTime;
    }
    return ret;
}

static TierStorageStats getTierStorageStats(const Json::Value &data) {
    TierStorageStats stats;
    stats.archiveStartTime = !data["archiveStartTime"].empty() ? data["archiveStartTime"].asUInt64() : 0;
    stats.archiveEndTime = !data["archiveEndTime"].empty() ? data["archiveEndTime"].asUInt64() : 0;
    return stats;
}

// ################### info.txt ###########################

void readRecordingJson(const Json::Value &ret, RecordingStatistic &stats, RecordingOption &option) {
    DeviceTuple tuple;
    tuple.vhost = ret["vhost"].asString();
    tuple.device_id = ret["id"].asString();
    tuple.name = ret["name"].asString();
    stats.tuple = tuple;

    option.name = ret["name"].asString();
    option.enableRecord = ret["enableRecord"].asBool();
    option.recordRootPath = ret["recordRootPath"].asString();
    option.keepArchivedMaxFor = ret["keepArchivedMaxFor"].asUInt64();
    option.keepArchivedMaxForAuto = ret["keepArchivedMaxForAuto"].asBool();
    option.keepArchivedMinFor = ret["keepArchivedMinFor"].asUInt64();
    option.keepArchivedMinForAuto = ret["keepArchivedMinForAuto"].asBool();
    option.recordSchedules = ret["recordSchedules"].asString();

    // stream tuple map
    unordered_map<int, StreamTuple> stream_map;
    auto streamUrlsString = ret["streamUrls"].asString();
    Json::Value streamUrlsJson;
    StrJsonUtils::readJsonString(streamUrlsString, streamUrlsJson);
    auto primary_stream = readRecordingStreamTuple(streamUrlsJson[PrimaryStream], tuple, PrimaryStream);
    if (!primary_stream.empty()) {
        stream_map[PrimaryStream] = primary_stream;
    }
    auto secondary_stream = readRecordingStreamTuple(streamUrlsJson[SecondaryStream], tuple, SecondaryStream);
    if (!secondary_stream.empty()) {
        stream_map[SecondaryStream] = secondary_stream;
    }
    stats.stream_map = stream_map;

    // add params
    for (const auto &it : ret["addParams"]) {
        if (it["name"] == "bookmarkStats") {
            Json::Value bm_json;
            StrJsonUtils::readJsonString(it["value"].asString(), bm_json);
            stats.bm = getBookmarkStats(bm_json);
        } else if (it["name"] == "streamStorageInfos") {
            Json::Value storage_json;
            StrJsonUtils::readJsonString(it["value"].asString(), storage_json);
            if (!primary_stream.empty()) {
                auto stream_id = primary_stream.stream_id;
                stats.storage_map[stream_id] = getStreamStorageStats(storage_json[PrimaryStream]);
            }
            if (!secondary_stream.empty()) {
                auto stream_id = secondary_stream.stream_id;
                stats.storage_map[stream_id] = getStreamStorageStats(storage_json[SecondaryStream]);
            }
        } else if (it["name"] == "streamStatisticInfos") {
            Json::Value stream_stats_json;
            StrJsonUtils::readJsonString(it["value"].asString(), stream_stats_json);
            if (!primary_stream.empty()) {
                stats.sinfo_map[PrimaryStream] = getStreamStatistic(stream_stats_json[PrimaryStream]);
            }
            if (!secondary_stream.empty()) {
                stats.sinfo_map[SecondaryStream] = getStreamStatistic(stream_stats_json[SecondaryStream]);
            }
        } else if (it["name"] == "motionStorageStats") {
            Json::Value motion_storage_json;
            StrJsonUtils::readJsonString(it["value"].asString(), motion_storage_json);
            stats.motion_stats = getMotionStorageStats(motion_storage_json);
        } else if (it["name"] == "transportStats") {
            Json::Value transport_stats_json;
            StrJsonUtils::readJsonString(it["value"].asString(), transport_stats_json);
            stats.transport_stats = getTransportStats(transport_stats_json);
        } else if (it["name"] == "tierStorageStats") {
#ifdef ENABLE_TIER_STORAGE
            Json::Value tier_storage_json;
            StrJsonUtils::readJsonString(it["value"].asString(), tier_storage_json);
            stats.tier_storage_map[HotTier] = getTierStorageStats(tier_storage_json[HotTier]);
            stats.tier_storage_map[WarmTier] = getTierStorageStats(tier_storage_json[WarmTier]);
            stats.tier_storage_map[ColdTier] = getTierStorageStats(tier_storage_json[ColdTier]);
#endif // ENABLE_TIER_STORAGE 
        }
    }

    // created_at/updated_at
    stats.created_at = ret["created_at"].asUInt64();
    stats.updated_at= ret["updated_at"].asUInt64();
}

void writeRecordingJson(const RecordingStatistic &stats, const RecordingOption &option, Json::Value &root, Json::Value &params) {
    writeDeviceStatisticHeader(stats.schema, root);
    root["vhost"] = stats.tuple.vhost;
    root["id"] = stats.tuple.device_id;

    root["name"] = option.name;
    root["enableRecord"] = option.enableRecord;
    root["recordRootPath"] = option.recordRootPath;
    root["keepArchivedMaxFor"] = option.keepArchivedMaxFor;
    root["keepArchivedMaxForAuto"] = option.keepArchivedMaxForAuto;
    root["keepArchivedMinFor"] = option.keepArchivedMinFor;
    root["keepArchivedMinForAuto"] = option.keepArchivedMinForAuto;
    root["recordSchedules"] = option.recordSchedules;

    // stream tuple map
    Json::Value streamUrls = Json::arrayValue;
    streamUrls[PrimaryStream] = makeRecordingStreamTupleJson(stats.stream_map, PrimaryStream);
    streamUrls[SecondaryStream] = makeRecordingStreamTupleJson(stats.stream_map, SecondaryStream);
    root["streamUrls"] = StrJsonUtils::writeJsonString(streamUrls);

    // add params
    Json::Value bm_json = makeBookmarkStatsJson(stats.bm);
    params.append(makeJsonKeyValue("bookmarkStats", StrJsonUtils::writeJsonString(bm_json)));
    Json::Value storage_json = Json::arrayValue;
    storage_json.append(makeStreamStorageStatsJson(stats.storage_map, stats.stream_map, PrimaryStream));
    storage_json.append(makeStreamStorageStatsJson(stats.storage_map, stats.stream_map, SecondaryStream));
    params.append(makeJsonKeyValue("streamStorageInfos", StrJsonUtils::writeJsonString(storage_json)));

    Json::Value stream_stats_json = Json::arrayValue;
    stream_stats_json.append(makeStreamStatisticJson(stats.sinfo_map, PrimaryStream));
    stream_stats_json.append(makeStreamStatisticJson(stats.sinfo_map, SecondaryStream));
    params.append(makeJsonKeyValue("streamStatisticInfos", StrJsonUtils::writeJsonString(stream_stats_json)));

    Json::Value motion_storage_json = makeMotionStorageStatsJson(stats.motion_stats);
    params.append(makeJsonKeyValue("motionStorageStats", StrJsonUtils::writeJsonString(motion_storage_json)));

    Json::Value transport_stats_json = makeTransportStatsJson(stats.transport_stats);
    params.append(makeJsonKeyValue("transportStats", StrJsonUtils::writeJsonString(transport_stats_json)));

#ifdef ENABLE_TIER_STORAGE
    Json::Value tier_storage_json = Json::arrayValue;
    tier_storage_json.append(makeTierStorageStatsJson(stats.tier_storage_map, HotTier));
    tier_storage_json.append(makeTierStorageStatsJson(stats.tier_storage_map, WarmTier));
    tier_storage_json.append(makeTierStorageStatsJson(stats.tier_storage_map, ColdTier));
    params.append(makeJsonKeyValue("tierStorageStats", StrJsonUtils::writeJsonString(tier_storage_json)));
#endif

    // created_at/updated_at
    root["created_at"] = stats.created_at;
    root["updated_at"] = stats.updated_at;
}

// ################### RecordingStatisticImp ###########################

std::string RecordingStatisticImp::getStreamID(const int &stream_type) {
    std::lock_guard<std::mutex> lck(_mtx);
    auto &stream_map = recordingData().stream_map;
    auto it = stream_map.find(stream_type);
    return it != stream_map.end() ? it->second.stream_id : "";
}

void RecordingStatisticImp::applyStreamTuples(const std::unordered_map<int, StreamTuple> &input_stream_map) {
    auto &data = recordingData();
    auto it_primary = input_stream_map.find(PrimaryStream);
    if (it_primary != input_stream_map.end()) {
        auto stream = it_primary->second;
        if (!stream.empty()) {
            // Ensure primary stream exist in storage_map and sinfo_map
            if (data.storage_map.find(stream.stream_id) == data.storage_map.end()) {
                data.storage_map[stream.stream_id] = StreamStorageStats();
            }
            if (data.sinfo_map.find(PrimaryStream) == data.sinfo_map.end()) {
                data.sinfo_map[PrimaryStream] = StreamStatistic();
            }
        } else {
            WarnL << "Input primary stream is empty";
        }
    } else {
        WarnL << "Input stream map do not have primary stream";
    }

    auto it_secondary = input_stream_map.find(SecondaryStream);
    if (it_secondary != input_stream_map.end()) {
        auto stream = it_secondary->second;
        if (!stream.empty()) {
            // Ensure secondary stream exist in storage_map and sinfo_map
            if (data.storage_map.find(stream.stream_id) == data.storage_map.end()) {
                data.storage_map[stream.stream_id] = StreamStorageStats();
            }
            if (data.sinfo_map.find(SecondaryStream) == data.sinfo_map.end()) {
                data.sinfo_map[SecondaryStream] = StreamStatistic();
            }
        } else {
            WarnL << "Input secondary stream is empty";
        }
    } else {
        WarnL << "Input stream map do not have secondary stream";
    }
    
    data.stream_map = input_stream_map;
}

RecordingParams RecordingStatisticImp::getRecordingParams() {
    std::lock_guard<std::mutex> lck(_mtx);
    RecordingParams ret;
    static_cast<RecordingStatistic &>(ret) = recordingData();
    ret.option = recordingOption();
    return ret;
}

void RecordingStatisticImp::addArchiveSize(string stream_id, size_t count, size_t size, uint64_t archived_start_time, uint64_t archived_end_time, bool add) {
    std::lock_guard<std::mutex> lck(_mtx);
    auto &data = recordingData();
    // Do not process time blocks with an end time earlier than the info file creation time, so that old media data does not need to be re-aggregated
    if (archived_end_time <= data.created_at) {
        DebugL << "Time block has end time (" << archived_end_time << ") less than or equal created_at of file ("<< data.created_at <<"). Ignore" ;
        return;
    }

    if (data.storage_map.find(stream_id) != data.storage_map.end()) {
        auto &storage = data.storage_map[stream_id]; 
        if (add) {
            storage.archiveSizeB += size;
            storage.archiveIndexRecordCount += count;
            if (storage.archiveStartTime == 0) {
                storage.archiveStartTime = archived_start_time;
            }
            storage.archiveEndTime = archived_end_time;
            DebugL << "Stream " << data.tuple.shortUrl() << "/" << stream_id << " add archived size: " << format_bytes_human_readable(size) << ", count: " << count
                   << ". Total archived size: " << format_bytes_human_readable(storage.archiveSizeB)
                   << ". Total archived count: " << storage.archiveIndexRecordCount
                   << ". First archived time: " << getTimeStr("%Y-%m-%d %H:%M:%S", storage.archiveStartTime)
                   << ". Last archived time: " << getTimeStr("%Y-%m-%d %H:%M:%S", storage.archiveEndTime);
        } else {
            storage.archiveSizeB = storage.archiveSizeB >= size ? (storage.archiveSizeB - size) : 0;
            storage.archiveIndexRecordCount = storage.archiveIndexRecordCount >= count ? (storage.archiveIndexRecordCount - count) : 0;
            if (storage.archiveIndexRecordCount == 0) {
                // record count is equal 0, reset archiveStartTime and archiveEndTime equal 0 to indicate no data
                storage.archiveStartTime = 0;
                storage.archiveEndTime = 0;
            } else {
                // Note: Use the end time block for approximate statistics, not completely accurate. Use the TimeQuery::getFirstBlock function to get the exact number.
                storage.archiveStartTime = archived_end_time;
            }
            DebugL << "Stream " << data.tuple.shortUrl() << "/" << stream_id << " subtract archived size: " << format_bytes_human_readable(size) << ", count: " << count
                   << ". Total archived size: " << format_bytes_human_readable(storage.archiveSizeB)
                   << ". Total archived count: " << storage.archiveIndexRecordCount
                   << ". First archived time: " << getTimeStr("%Y-%m-%d %H:%M:%S", storage.archiveStartTime)
                   << ". Last archived time: " << getTimeStr("%Y-%m-%d %H:%M:%S", storage.archiveEndTime);
        }
        saveRecording();
    } else {
        WarnL << "Camera " << data.tuple.shortUrl() << " do not have stream: " << stream_id << ". Ignore archive size statistic";
    }
}

void RecordingStatisticImp::addBookmarkCount(uint64_t bm_created_at, size_t size, bool add) {
    std::lock_guard<std::mutex> lck(_mtx);
    auto &data = recordingData();
    auto &bm = data.bm;
    // Do not process bookmark with an creation time earlier than the info file creation time, so that old media data does not need to be re-aggregated
    if (bm_created_at <= data.created_at) {
        DebugL << "Bookmark has creation time (" << bm_created_at << ") less than or equal created_at of file ("<< data.created_at <<"). Ignore" ;
        return;
    }
    if (add) {
        bm.recordCount ++;
        bm.recordAverageSizeB += size;
        DebugL << "Camera " << data.tuple.shortUrl() << " add bookmark count: 1. Total bookmark count: " << bm.recordCount;

    } else {
        if (bm.recordCount > 0) {
            bm.recordCount--;
        }
        if (bm.recordAverageSizeB >= size) {
            bm.recordAverageSizeB -= size;
        } else {
            bm.recordAverageSizeB = 0;
        }
        DebugL << "Camera " << data.tuple.shortUrl() << " subtract bookmark count: 1. Total bookmark count: " << bm.recordCount;
    }
    saveRecording();
}

void RecordingStatisticImp::addStreamStatistic(int stream_type, bool live, string status, const TranslationInfo *info) {
    std::lock_guard<std::mutex> lck(_mtx);
    bool changed = false;
    if (applyStreamStatistic(stream_type, live, status, info, changed) && changed) {
        saveRecording();
    }
}

bool RecordingStatisticImp::applyStreamStatistic(int stream_type, bool live, const string &status, const TranslationInfo *info_, bool &changed) {
    auto &data = recordingData();
    auto &sinfo_map = data.sinfo_map;
    if (sinfo_map.find(stream_type) == sinfo_map.end()) {
        WarnL << "Camera " << data.tuple.shortUrl() << " do not have stream type: " << stream_type << ". Ignore add stream statistic";
        return false;
    }
    auto &sinfo = sinfo_map[stream_type];
    // byte_speed is a measurement, not state to restore after a restart: an update that only
    // changes it does not rewrite info.txt.
    auto stats_before = makeStreamStatisticJson(sinfo_map, stream_type);
    stats_before.removeMember("byte_speed");
    if (!sinfo.last_change_status || sinfo.live != live) {
        sinfo.last_change_status = time(nullptr);
    }
    sinfo.live = live;
    sinfo.status = status;
    sinfo.byte_speed = live && info_ ? info_->byte_speed : 0;
    if (live && info_) {
        for (const auto &it : info_->stream_info) {
            switch(it.codec_type) {
                case TrackVideo: 
                    sinfo.has_video = true;
                    sinfo.vcodec = it.codec_name;
                    sinfo.source_vcodec = it.origin_codec_name;
                    sinfo.transcoded = it.origin_codec_name != it.codec_name;
                    sinfo.width = it.video_width;
                    sinfo.height = it.video_height;
                    sinfo.bitrate = it.bitrate;
                    sinfo.fps = it.video_fps;
                    break;
                case TrackAudio:
                    sinfo.has_audio = true;
                    sinfo.acodec = it.codec_name;
                    sinfo.sample_rate = it.audio_sample_rate;
                    sinfo.channel_no = it.audio_channel;
                    sinfo.sample_bit = it.audio_sample_bit;
                    break;
                default:
                    break;
            }
        }
    }
    DebugL << "Stream " << data.stream_map[stream_type].shortUrl() << " statistic: Live=" << sinfo.live << ". Status=" << sinfo.status << ". Byte_speed=" << sinfo.byte_speed << " bytes/s";
    auto stats_after = makeStreamStatisticJson(sinfo_map, stream_type);
    stats_after.removeMember("byte_speed");
    changed = stats_after != stats_before;
    return true;
}

void RecordingStatisticImp::addMotionKeepThreshold(bool start, uint64_t threshold) {
    std::lock_guard<std::mutex> lck(_mtx);
    auto &data = recordingData();
    if (start) {
        data.motion_stats.archiveStartTime = threshold;
        DebugL << "Camera " << data.tuple.shortUrl() << " set motion keep start threshold: " << threshold << " => " << getTimeStr("%Y-%m-%d %H:%M:%S", threshold);
    } else {
        data.motion_stats.archiveEndTime = threshold;
        DebugL << "Camera " << data.tuple.shortUrl() << " set motion keep end threshold: " << threshold << " => " << getTimeStr("%Y-%m-%d %H:%M:%S", threshold);
    }
    saveRecording();
}

void RecordingStatisticImp::addTierKeepThreshold(int tier_type, bool start, uint64_t threshold) {
#ifdef ENABLE_TIER_STORAGE
    std::lock_guard<std::mutex> lck(_mtx);
    auto &data = recordingData();
    auto &tier_stats = data.tier_storage_map[tier_type];
    if (start) {
        tier_stats.archiveStartTime = threshold;
        DebugL << "Camera " << data.tuple.shortUrl() << " set tier " << tierTypeToString(static_cast<TierType>(tier_type)) << " keep start threshold: " << threshold << " seconds";
    } else {
        tier_stats.archiveEndTime = threshold;
        DebugL << "Camera " << data.tuple.shortUrl() << " set tier " << tierTypeToString(static_cast<TierType>(tier_type)) << " keep end threshold: " << threshold << " seconds";
    }
    saveRecording();
#else
    WarnL << "Tier storage is not enabled. Ignore add tier keep threshold.";
#endif
}

} // namespace managerkit
