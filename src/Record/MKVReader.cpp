#ifdef ENABLE_MKV

#include "MKVReader.h"
#include "Common/config.h"
#include "Thread/WorkThreadPool.h"
#include "Util/File.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

MKVReader::MKVReader(const MediaTuple &tuple, const string &file_path,
                     toolkit::EventPoller::Ptr poller) {
    ProtocolOption option;
    // Read mkv file and stream it, do not regenerate mkv/webm file repeatedly
    option.enable_mkv = false;
    option.enable_webm = false;
    // mkv supports multiple tracks
    option.max_track = 16;
    setup(tuple, file_path, option, std::move(poller));
}

MKVReader::MKVReader(const MediaTuple &tuple, const string &file_path, const ProtocolOption &option, toolkit::EventPoller::Ptr poller) {
    setup(tuple, file_path, option, std::move(poller));
}

void MKVReader::setup(const MediaTuple &tuple, const std::string &file_path, const ProtocolOption &option, toolkit::EventPoller::Ptr poller) {
    // It is recommended to read and write files in the background thread
    _poller = poller ? std::move(poller) : WorkThreadPool::Instance().getPoller();
    _file_path = file_path;
    if (_file_path.empty()) {
        GET_CONFIG(string, recordPath, Protocol::kMKVSavePath);
        GET_CONFIG(bool, enableVhost, General::kEnableVhost);
        if (enableVhost) {
            _file_path = tuple.shortUrl();
        } else {
            _file_path = tuple.app + "/" + tuple.stream;
        }
        _file_path = File::absolutePath(_file_path, recordPath);
    }

    _demuxer = std::make_shared<MultiMKVDemuxer>();
    _demuxer->openMKV(_file_path);

    if (tuple.stream.empty()) {
        return;
    }

    _muxer = std::make_shared<MultiMediaSourceMuxer>(tuple, _demuxer->getDurationMS() / 1000.0f, option);
    auto tracks = _demuxer->getTracks(false);
    if (tracks.empty()) {
        throw std::runtime_error(StrPrinter << "The mkv file has no valid track:" << _file_path);
    }
    for (auto &track : tracks) {
        _muxer->addTrack(track);
        if (track->getTrackType() == TrackVideo) {
            _have_video = true;
        }
    }
    // After all tracks are added, prevent the maximum waiting time of 3 seconds in the case of a single track
    _muxer->addTrackCompleted();
}

bool MKVReader::readSample() {
    if (_paused) {
        // Ensure that the timeline does not move when paused
        _seek_ticker.resetTime();
        return true;
    }

    bool keyFrame = false;
    bool eof = false;
    while (!eof && _last_dts < getCurrentStamp()) {
        auto frame = _demuxer->readFrame(keyFrame, eof);
        if (!frame) {
            continue;
        }
        _last_dts = frame->dts();
        if (_muxer) {
            _muxer->inputFrame(frame);
        }
    }

    GET_CONFIG(bool, file_repeat, Record::kFileRepeat);
    if (eof && (file_repeat || _file_repeat)) {
        // Need to start from the beginning
        seekTo(0);
        return true;
    }

    return !eof;
}

bool MKVReader::readNextSample() {
    bool keyFrame = false;
    bool eof = false;
    auto frame = _demuxer->readFrame(keyFrame, eof);
    if (!frame) {
        return false;
    }
    if (_muxer) {
        _muxer->inputFrame(frame);
    }
    setCurrentStamp(frame->dts());
    return true;
}

void MKVReader::stopReadMKV() {
    _timer = nullptr;
}

void MKVReader::startReadMKV(uint64_t sample_ms, bool ref_self, bool file_repeat) {
    GET_CONFIG(uint32_t, sampleMS, Record::kSampleMS);
    setCurrentStamp(0);
    auto strong_self = shared_from_this();
    if (_muxer) {
        // Keep reading until all tracks are ready
        while (!_muxer->isAllTrackReady() && readNextSample());
        // Register and then switch OwnerPoller
        _muxer->setMediaListener(strong_self);
    }

    auto timer_sec = (sample_ms ? sample_ms : sampleMS) / 1000.0f;

    // Start the timer
    if (ref_self) {
        _timer = std::make_shared<Timer>(timer_sec, [strong_self]() {
            lock_guard<recursive_mutex> lck(strong_self->_mtx);
            return strong_self->readSample();
        }, _poller);
    } else {
        weak_ptr<MKVReader> weak_self = strong_self;
        _timer = std::make_shared<Timer>(timer_sec, [weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return false;
            }
            lock_guard<recursive_mutex> lck(strong_self->_mtx);
            return strong_self->readSample();
        }, _poller);
    }

    _file_repeat = file_repeat;
}

const MultiMKVDemuxer::Ptr &MKVReader::getDemuxer() const {
    return _demuxer;
}

uint32_t MKVReader::getCurrentStamp() {
    return (uint32_t) (_seek_to + !_paused * _speed * _seek_ticker.elapsedTime());
}

void MKVReader::setCurrentStamp(uint32_t new_stamp) {
    auto old_stamp = getCurrentStamp();
    _seek_to = new_stamp;
    _last_dts = new_stamp;
    _seek_ticker.resetTime();
    if (old_stamp != new_stamp && _muxer) {
        // Do not operate when the timeline is not dragged
        _muxer->setTimeStamp(new_stamp);
    }
}

bool MKVReader::seekTo(MediaSource &sender, uint32_t stamp) {
    // Playback should resume after dragging the progress bar
    pause(sender, false);
    TraceL << getOriginUrl(sender) << ",stamp:" << stamp;
    return seekTo(stamp);
}

bool MKVReader::pause(MediaSource &sender, bool pause) {
    if (_paused == pause) {
        return true;
    }
    // _seek_ticker restarts the timer, whether it is paused or seek does not affect the total playback progress
    setCurrentStamp(getCurrentStamp());
    _paused = pause;
    TraceL << getOriginUrl(sender) << ",pause:" << pause;
    return true;
}

bool MKVReader::speed(MediaSource &sender, float speed) {
    if (speed < 0.1 || speed > 20) {
        WarnL << "The playback speed value range is illegal:" << speed;
        return false;
    }
    // _seek_ticker reset, assign _seek_to
    setCurrentStamp(getCurrentStamp());
    // Playback should resume after setting the playback speed
    _paused = false;
    if (_speed == speed) {
        return true;
    }
    _speed = speed;
    TraceL << getOriginUrl(sender) << ",speed:" << speed;
    return true;
}

bool MKVReader::seekTo(uint32_t stamp_seek) {
    lock_guard<recursive_mutex> lck(_mtx);
    if (stamp_seek > _demuxer->getDurationMS()) {
        // Exceeds the file length
        return false;
    }
    auto stamp = _demuxer->seekTo(stamp_seek);
    if (stamp == -1) {
        // Seek failed
        return false;
    }

    if (!_have_video) {
        // There is no video, no need to search for keyframes; set the current timestamp
        setCurrentStamp((uint32_t) stamp);
        return true;
    }
    // Search for the next keyframe
    bool keyFrame = false;
    bool eof = false;
    while (!eof) {
        auto frame = _demuxer->readFrame(keyFrame, eof);
        if (!frame) {
            // The file has been read but the next keyframe has not been found
            continue;
        }
        if (keyFrame || frame->keyFrame() || frame->configFrame()) {
            // Locate to the keyframe
            if (_muxer) {
                _muxer->inputFrame(frame);
            }
            // Set the current timestamp
            setCurrentStamp(frame->dts());
            return true;
        }
    }
    return false;
}

bool MKVReader::close(MediaSource &sender) {
    _timer = nullptr;
    WarnL << "close media: " << sender.getUrl();
    return true;
}

MediaOriginType MKVReader::getOriginType(MediaSource &sender) const {
    return MediaOriginType::mp4_vod;
}

string MKVReader::getOriginUrl(MediaSource &sender) const {
    return _file_path;
}

toolkit::EventPoller::Ptr MKVReader::getOwnerPoller(MediaSource &sender) {
    return _poller;
}

} /* namespace mediakit */
#endif // ENABLE_MKV