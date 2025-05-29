// #ifdef ENABLE_MKV
#include <ctime>
#include <sys/stat.h>
#include "Util/File.h"
#include "Common/config.h"
#include "MKVRecorder.h"
#include "Thread/WorkThreadPool.h"
#include "MKVMuxer.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

MKVRecorder::MKVRecorder(const MediaTuple &tuple, const string &path, size_t max_second) {
    _folder_path = path;
    // ///record Business Logic//////
    static_cast<MediaTuple &>(_info) = tuple;
    _info.folder = path;
    GET_CONFIG(uint32_t, s_max_second, Protocol::kMKVMaxSecond);
    _max_second = max_second ? max_second : s_max_second;
}

MKVRecorder::~MKVRecorder() {
    try {
        flush();
        closeFile();
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void MKVRecorder::createFile() {
    closeFile();
    auto date = getTimeStr("%Y-%m-%d");
    auto file_name = getTimeStr("%H-%M-%S") + "-" + std::to_string(_file_index++) + ".mkv";
    auto full_path = _folder_path + date + "/" + file_name;
    auto full_path_tmp = _folder_path + date + "/." + file_name;

    //////record Business Logic//////
    _info.start_time = ::time(NULL);
    _info.file_name = file_name;
    _info.file_path = full_path;
    GET_CONFIG(string, appName, Record::kAppName);
    _info.url = appName + "/" + _info.app + "/" + _info.stream + "/" + date + "/" + file_name;

    try {
        _muxer = std::make_shared<MKVMuxer>();
        TraceL << "Open tmp mkv file: " << full_path_tmp;
        _muxer->openMKV(full_path_tmp);
        for (auto &track :_tracks) {
            // Add track
            _muxer->addTrack(track);
        }
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void MKVRecorder::asyncClose() {
    auto muxer = _muxer;
    auto full_path_tmp = _full_path_tmp;
    auto full_path = _full_path;
    auto info = _info;
    TraceL << "Start close tmp mkv file: " << full_path_tmp;
    WorkThreadPool::Instance().getExecutor()->async([muxer, full_path_tmp, full_path, info]() mutable {
        info.time_len = muxer->getDuration() / 1000.0f;
        // Closing mkv can be very time-consuming, so it should be executed in the background thread
        TraceL << "Closing tmp mp4 file: " << full_path_tmp;
        muxer->closeMKV();
        TraceL << "Closed tmp mp4 file: " << full_path_tmp;
        if (!full_path_tmp.empty()) {
            // Get file size
            info.file_size = File::fileSize(full_path_tmp);
            if (info.file_size < 1024) {
                // The recording file is too small, delete it
                File::delete_file(full_path_tmp);
                return;
            }
            // Change the temporary file name to the official file name to prevent access to the mkv before it is completed
            rename(full_path_tmp.data(), full_path.data());
        }
        TraceL << "Emit mkv record event: " << full_path;
        // Trigger mkv recording slice generation event
        NOTICE_EMIT(BroadcastRecordMKVArgs, Broadcast::kBroadcastRecordMKV, info);
    });
}

void MKVRecorder::closeFile() {
    if (_muxer) {
        asyncClose();
        _muxer = nullptr;
    }
}

void MKVRecorder::flush() {
    if (_muxer) {
        _muxer->flush();
    }
}

bool MKVRecorder::inputFrame(const Frame::Ptr &frame) {
    if (!(_have_video && frame->getTrackType() == TrackAudio)) {
        // If there is video and the input is audio, then the slice logic should be ignored
        if (_last_dts == 0) {
            // first frame assign dts
            _last_dts = frame->dts();
        } else if (_last_dts > frame->dts()) {
            // In the case of b-frames, the dts timestamp may regress
            _last_dts = MIN(frame->dts(), _last_dts);
        }

        auto duration = 5u; // Default is at least one frame 5ms
        if (frame->dts() > 0 && frame->dts() > _last_dts) {
            duration = MAX(duration, frame->dts() - _last_dts);
        }
        if (!_muxer || ((duration > _max_second * 1000) && (!_have_video || (_have_video && frame->keyFrame())))) {
            // Conditions for establishment
            // 1. _muxer is empty
            // 2. It's time to slice, and there is only audio
            // 3. It's time to slice, there is video and a video keyframe is encountered
            _last_dts = 0;
            createFile();
        }
    }

    if (_muxer) {
        // Generate mkv file
        return _muxer->inputFrame(frame);
    }
    return false;
}

bool MKVRecorder::addTrack(const Track::Ptr &track) {
    // Save all tracks in preparation for creating MKVMuxerFile
    _tracks.emplace_back(track);
    if (track->getTrackType() == TrackVideo) {
        _have_video = true;
    }
    return true;
}

void MKVRecorder::resetTracks() {
    closeFile();
    _tracks.clear();
    _have_video = false;
} 

} /* namespace mediakit */

// #endif // ENABLE_MKV