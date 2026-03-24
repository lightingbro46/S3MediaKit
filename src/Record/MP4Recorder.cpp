#ifdef ENABLE_MP4
#include <ctime>
#include <sys/stat.h>
#include "Util/File.h"
#include "Common/config.h"
#include "MP4Recorder.h"
#include "Thread/WorkThreadPool.h"
#include "MP4Muxer.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

MP4Recorder::MP4Recorder(const MediaTuple &tuple, const string &path, size_t max_second) {
    // ///record Business Logic//////
    static_cast<MediaTuple &>(_info) = tuple;
    _info.folder = path;
    GET_CONFIG(uint32_t, s_max_second, Protocol::kMP4MaxSecond);
    _max_second = max_second ? max_second : s_max_second;
}

MP4Recorder::~MP4Recorder() {
    try {
        flush();
        closeFile();
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void MP4Recorder::createFile() {
    closeFile();
    time_t file_time = (_next_file_time != 0) ? _next_file_time : ::time(NULL);
    _next_file_time = 0; // consume override
    auto date = getTimeStr("%Y-%m-%d", file_time);
    auto file_name = date + "-" + getTimeStr("%H-%M-%S", file_time) + "-" + std::to_string(_file_index++) + ".mp4";
    auto full_path = _info.folder + date + "/" + file_name;
    auto full_path_tmp = _info.folder + date + "/." + file_name;

    // ///record Business Logic//////
    _info.start_time = file_time;
    _info.file_name = file_name;
    _info.file_path = full_path;
    GET_CONFIG(string, appName, Record::kAppName);
    _info.url = appName + "/" + _info.app + "/" + _info.stream + "/" + date + "/" + file_name;

    try {
        _muxer = std::make_shared<MP4Muxer>();
        TraceL << "Open tmp mp4 file: " << full_path_tmp;
        _muxer->openMP4(full_path_tmp);
        for (auto &track :_tracks) {
            // Add track
            _muxer->addTrack(track);
        }
        _full_path_tmp = full_path_tmp;
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void MP4Recorder::asyncClose() {
    auto muxer = _muxer;
    auto full_path_tmp = _full_path_tmp;
    auto info = _info;
    TraceL << "Start close tmp mp4 file: " << full_path_tmp;
    WorkThreadPool::Instance().getExecutor()->async([muxer, full_path_tmp, info]() mutable {
        info.time_len = muxer->getDuration() / 1000.0f;
        // Closing mp4 can be very time-consuming, so it should be executed in the background thread
        TraceL << "Closing tmp mp4 file: " << full_path_tmp;
        muxer->closeMP4();
        TraceL << "Closed tmp mp4 file: " << full_path_tmp;
        if (!full_path_tmp.empty()) {
            // Get file size
            info.file_size = File::fileSize(full_path_tmp);
            if (info.file_size < 1024) {
                // The recording file is too small, delete it
                File::delete_file(full_path_tmp);
                return;
            }
            // Change the temporary file name to the official file name to prevent access to the mp4 before it is completed
            rename(full_path_tmp.data(), info.file_path.data());
        }
        TraceL << "Emit mp4 record event: " << info.file_path;
        // Trigger mp4 recording slice generation event
        NOTICE_EMIT(BroadcastRecordMP4Args, Broadcast::kBroadcastRecordMP4, info);
    });
}

void MP4Recorder::closeFile() {
    if (_muxer) {
        asyncClose();
        _muxer = nullptr;
    }
}

void MP4Recorder::flush() {
    if (_muxer) {
        _muxer->flush();
    }
}

bool MP4Recorder::inputFrame(const Frame::Ptr &frame) {
    auto stamp_inc = _delta_stamp[frame->getTrackType()].relativeStamp(frame->pts(), false);
    if (!_muxer || (stamp_inc > int64_t(_max_second) * 1000 && (!_have_video || frame->keyFrame()))) {
        // Conditions for establishment
        // 1. _muxer is empty
        // 2. It's time to slice, and there is only audio
        // 3. It's time to slice, there is video and a video keyframe is encountered
        createFile();
        for (auto &ref : _delta_stamp) {
            ref.reset();
        }
    }

    if (_muxer) {
        // Generate mp4 file
        return _muxer->inputFrame(frame);
    }
    return false;
}

bool MP4Recorder::addTrack(const Track::Ptr &track) {
    // Save all tracks in preparation for creating MP4MuxerFile
    _tracks.emplace_back(track);
    if (track->getTrackType() == TrackVideo) {
        _have_video = true;
    }
    return true;
}

void MP4Recorder::resetTracks() {
    closeFile();
    _tracks.clear();
    _have_video = false;
}

} /* namespace mediakit */

#endif //ENABLE_MP4
