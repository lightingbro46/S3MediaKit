#include "Recorder.h"
#include "Common/config.h"
#include "Util/File.h"
#include "Common/MediaSource.h"
#include "MP4Recorder.h"
#include "HlsRecorder.h"
#include "FMP4/FMP4MediaSourceMuxer.h"
#include "TS/TSMediaSourceMuxer.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

string Recorder::getRecordPath(Recorder::type type, const MediaTuple& tuple, const string &customized_path) {
    GET_CONFIG(bool, enableVhost, General::kEnableVhost);
    switch (type) {
        case Recorder::type_hls: {
            GET_CONFIG(string, hlsPath, Protocol::kHlsSavePath);
            string m3u8FilePath;
            if (enableVhost) {
                m3u8FilePath = tuple.shortUrl() + "/hls.m3u8";
            } else {
                m3u8FilePath = tuple.app + "/" + tuple.stream + "/hls.m3u8";
            }
            //Here we use the customized file path.
            if (!customized_path.empty()) {
                return File::absolutePath(m3u8FilePath, customized_path);
            }
            return File::absolutePath(m3u8FilePath, hlsPath);
        }
        case Recorder::type_mp4: {
            GET_CONFIG(string, recordPath, Protocol::kMP4SavePath);
            GET_CONFIG(string, recordAppName, Record::kAppName);
            string mp4FilePath;
            if (enableVhost) {
                mp4FilePath = tuple.vhost + "/" + recordAppName + "/" + tuple.app + "/" + tuple.stream + "/";
            } else {
                mp4FilePath = recordAppName + "/" + tuple.app + "/" + tuple.stream + "/";
            }
            //Here we use the customized file path.
            if (!customized_path.empty()) {
                return File::absolutePath(mp4FilePath, customized_path);
            }
            return File::absolutePath(mp4FilePath, recordPath);
        }
        case Recorder::type_hls_fmp4: {
            GET_CONFIG(string, hlsPath, Protocol::kHlsSavePath);
            string m3u8FilePath;
            if (enableVhost) {
                m3u8FilePath = tuple.shortUrl() + "/hls.fmp4.m3u8";
            } else {
                m3u8FilePath = tuple.app + "/" + tuple.stream + "/hls.fmp4.m3u8";
            }
            // Here we use the customized file path.
            if (!customized_path.empty()) {
                return File::absolutePath(m3u8FilePath, customized_path);
            }
            return File::absolutePath(m3u8FilePath, hlsPath);
        }
        case Recorder::type_mp4_archived: {
            GET_CONFIG(string, recordPath, Protocol::kMP4SavePath);
            GET_CONFIG(string, recordAppName, Record::kAppName);
            GET_CONFIG(string, archive_name, Record::kArchiveStreamName);
            string mp4FilePath;
            if (enableVhost) {
                mp4FilePath = tuple.vhost + "/" + recordAppName + "/" + tuple.app + "/" + archive_name + "/";
            } else {
                mp4FilePath = recordAppName + "/" + tuple.app + "/" + archive_name + "/";
            }
            //Here we use the customized file path.
            if (!customized_path.empty()) {
                return File::absolutePath(mp4FilePath, customized_path);
            }
            return File::absolutePath(mp4FilePath, recordPath);
        }
        default: return "";
    }
}

std::shared_ptr<MediaSinkInterface> Recorder::createRecorder(type type, const MediaTuple& tuple, const ProtocolOption &option){
    switch (type) {
        case Recorder::type_hls: {
#if defined(ENABLE_HLS)
            auto path = Recorder::getRecordPath(type, tuple, option.hls_save_path);
            GET_CONFIG(bool, enable_vhost, General::kEnableVhost);
            auto ret = std::make_shared<HlsRecorder>(path, enable_vhost ? string(VHOST_KEY) + "=" + tuple.vhost : "", option);
            ret->setMediaSource(tuple);
            return ret;
#else
            throw std::invalid_argument("hls related functions are not turned on. Please enable the ENABLE_HLS macro and then compile and test it again");
#endif
        }

        case Recorder::type_mp4: {
#if defined(ENABLE_MP4)
            auto path = Recorder::getRecordPath(type, tuple, option.mp4_save_path);
            return std::make_shared<MP4Recorder>(tuple, path, option.mp4_max_second);
#else
            throw std::invalid_argument("The mp4-related functions are not turned on, please enable the ENABLE_MP4 macro and compile and test it again.");
#endif
        }

        case Recorder::type_hls_fmp4: {
#if defined(ENABLE_MP4)
            auto path = Recorder::getRecordPath(type, tuple, option.hls_save_path);
            GET_CONFIG(bool, enable_vhost, General::kEnableVhost);
            auto ret = std::make_shared<HlsFMP4Recorder>(path, enable_vhost ? string(VHOST_KEY) + "=" + tuple.vhost : "", option);
            ret->setMediaSource(tuple);
            return ret;
#else
            throw std::invalid_argument("hls.fmp4 related functions are not turned on. Please enable the ENABLE_MP4 macro and then compile and test it");
#endif
        }

        case Recorder::type_fmp4: {
#if defined(ENABLE_MP4)
            return std::make_shared<FMP4MediaSourceMuxer>(tuple, option);
#else
            throw std::invalid_argument("fmp4 related functions are not turned on. Please enable the ENABLE_MP4 macro and then compile and test it");
#endif
        }

        case Recorder::type_ts: {
#if defined(ENABLE_HLS) || defined(ENABLE_RTPPROXY)
            return std::make_shared<TSMediaSourceMuxer>(tuple, option);
#else
            throw std::invalid_argument("mpegts related functions are not turned on. Please enable the ENABLE_HLS or ENABLE_RTPPROXY macro and then compile and test it");
#endif
        }

        case Recorder::type_mp4_archived: {
#if defined(ENABLE_MP4)
            auto path = Recorder::getRecordPath(type, tuple, option.mp4_save_path);
            return std::make_shared<MP4Recorder>(tuple, path, option.mp4_max_second);
#else
            throw std::invalid_argument("The mp4-related functions are not turned on, please enable the ENABLE_MP4 macro and compile and test it again.");
#endif
        }

        default: throw std::invalid_argument("Unknown recording type");
    }
}

} /* namespace mediakit */
