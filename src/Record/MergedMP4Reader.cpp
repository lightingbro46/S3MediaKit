#ifdef ENABLE_MP4

#include "MergedMP4Reader.h"
#include "Common/Parser.h"
#include "Common/config.h"
#include "Extension/Factory.h"
#include "Extension/Frame.h"
#include "Thread/WorkThreadPool.h"
#include "Util/logger.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

using namespace std;
using namespace toolkit;

namespace mediakit {

static const int MUTE_AUDIO_INDEX = 0xFFFF;
static uint8_t s_mute_adts_cfg[] = {0x15, 0x88};

bool parseReplayStartTime(const string &stream, uint64_t &start_s) {
    const auto parts = split(stream.substr(0, stream.find(".replay.")), "/");
    if (parts.size() <= 2) return false;
    const auto &stamp = parts.back();
    if (stamp.empty() || stamp.size() > 19 || stamp.find_first_not_of("0123456789") != string::npos) return false;
    start_s = strtoull(stamp.data(), nullptr, 10);
    return true;
}

ReplayClipWindow makeReplayClipWindow(const Broadcast::ReplayClip &clip, uint64_t replay_start_s) {
    ReplayClipWindow window;
    window.start_ms = (static_cast<int64_t>(clip.start_time) - static_cast<int64_t>(replay_start_s)) * 1000;
    window.end_ms = window.start_ms + static_cast<int64_t>(clip.duration) * 1000;
    window.file_offset_ms = (static_cast<int64_t>(clip.start_time) - static_cast<int64_t>(clip.file_start_time)) * 1000;
    window.play_from_ms = window.start_ms;
    return window;
}

int64_t seekKeyframeAtOrBefore(const function<int64_t(int64_t)> &seek, int64_t target_ms) {
    int64_t landed = seek(target_ms);
    // The nearest keyframe may follow the target: step back with a growing
    // distance until the nearest one is at or before it.
    int64_t step = landed > target_ms ? landed - target_ms + 1 : 0;
    for (int tries = 0; landed > target_ms && tries < 8; ++tries) {
        const int64_t probe = max<int64_t>(0, target_ms - step);
        landed = seek(probe);
        if (probe == 0) break;
        step *= 2;
    }
    return landed;
}

bool replaySwitchPoint(const ReplayClipWindow &prev, const ReplayClipWindow &next,
                       int64_t landed_file_ms, int64_t &switch_ms) {
    if (landed_file_ms < 0 || prev.end_ms != next.start_ms) return false;
    const int64_t keyframe_ms = next.map(landed_file_ms);
    if (keyframe_ms > next.start_ms || keyframe_ms <= prev.play_from_ms) return false;
    switch_ms = keyframe_ms;
    return true;
}

MergedMP4Reader::MergedMP4Reader(const MediaTuple &tuple,
                                 toolkit::EventPoller::Ptr poller,
                                 bool enable_audio,
                                 bool add_mute_audio) {
    ProtocolOption option;
    option.enable_mp4 = false;
    option.enable_rtsp = false;
    option.enable_rtmp = false;
    option.enable_hls = false;
    option.enable_hls_fmp4 = false;
    option.enable_ts = false;
    option.enable_fmp4 = true;
    option.max_track = 16;
    option.enable_audio = enable_audio;
    option.add_mute_audio = add_mute_audio;
    setup(tuple, option, std::move(poller));
}

MergedMP4Reader::MergedMP4Reader(const MediaTuple &tuple,
                                 const ProtocolOption &option,
                                 toolkit::EventPoller::Ptr poller) {
    setup(tuple, option, std::move(poller));
}

MergedMP4Reader::~MergedMP4Reader() {
    stopReplay();
}

void MergedMP4Reader::setup(const MediaTuple &tuple,
                            const ProtocolOption &option,
                            toolkit::EventPoller::Ptr poller) {
    _poller = poller ? std::move(poller) : WorkThreadPool::Instance().getPoller();
    _tuple = tuple;
    _option = option;
    _enable_audio = option.enable_audio;
    _add_mute_audio = option.add_mute_audio;

    GET_CONFIG(string, app_name, Record::kAppName);
    _replay_mode = tuple.app == app_name;
}

void MergedMP4Reader::setupMuteAudio(vector<Track::Ptr> &tracks) {
    _mute_audio_maker = nullptr;
    if (!_add_mute_audio || !_enable_audio) return;

    bool has_video = false, has_audio = false;
    for (const auto &track : tracks) {
        has_video = has_video || track->getTrackType() == TrackVideo;
        has_audio = has_audio || track->getTrackType() == TrackAudio;
    }
    if (!has_video || has_audio) return;

    auto audio = Factory::getTrackByCodecId(CodecAAC);
    audio->setIndex(MUTE_AUDIO_INDEX);
    audio->setExtraData(s_mute_adts_cfg, sizeof(s_mute_adts_cfg));
    tracks.push_back(audio);

    auto maker = std::make_shared<MuteAudioMaker>();
    maker->addDelegate([this](const Frame::Ptr &frame) {
        return _muxer->inputFrame(frame);
    });
    _mute_audio_maker = std::move(maker);
}

void MergedMP4Reader::inputFrame(const Frame::Ptr &frame) {
    if (!_enable_audio && frame->getTrackType() == TrackAudio) return;
    if (_muxer) _muxer->inputFrame(frame);
    if (_mute_audio_maker && frame->getTrackType() == TrackVideo) {
        _mute_audio_maker->inputFrame(frame);
    }
}

void MergedMP4Reader::initOutputMuxer(const vector<Track::Ptr> &tracks) {
    if (!_muxer) _muxer = make_shared<MultiMediaSourceMuxer>(_tuple, 0.0f, _option);

    auto track_list = tracks;
    if (!_enable_audio) {
        track_list.erase(remove_if(track_list.begin(), track_list.end(), [](const Track::Ptr &track) {
            return track->getTrackType() == TrackAudio;
        }), track_list.end());
    }
    setupMuteAudio(track_list);
    for (const auto &track : track_list) _muxer->addTrack(track);
    _muxer->addTrackCompleted();
    _muxer->setMediaListener(shared_from_this());
}

void MergedMP4Reader::switchToMuxer(const vector<Track::Ptr> &tracks) {
    auto track_list = tracks;
    if (!_enable_audio) {
        track_list.erase(remove_if(track_list.begin(), track_list.end(), [](const Track::Ptr &track) {
            return track->getTrackType() == TrackAudio;
        }), track_list.end());
    }
    setupMuteAudio(track_list);
    _muxer->resetTracks();
    for (const auto &track : track_list) _muxer->addTrack(track);
    _muxer->addTrackCompleted();
}

vector<Track::Ptr> MergedMP4Reader::normalizeTracks(const vector<Track::Ptr> &tracks) const {
    vector<Track::Ptr> result;
    bool have_video = false, have_audio = false;
    for (const auto &track : tracks) {
        if (!track) continue;
        auto clone = track->clone();
        if (track->getTrackType() == TrackVideo) {
            if (have_video) continue;
            clone->setIndex(0);
            have_video = true;
        } else if (track->getTrackType() == TrackAudio) {
            if (have_audio) continue;
            clone->setIndex(1);
            have_audio = true;
        } else {
            continue;
        }
        result.push_back(std::move(clone));
    }
    return result;
}

bool MergedMP4Reader::sameTrackSet(const vector<Track::Ptr> &left_tracks,
                                   const vector<Track::Ptr> &right_tracks) const {
    if (left_tracks.size() != right_tracks.size()) return false;
    for (const auto &left : left_tracks) {
        auto it = find_if(right_tracks.begin(), right_tracks.end(), [&](const Track::Ptr &right) {
            return left->getIndex() == right->getIndex() && left->getCodecId() == right->getCodecId();
        });
        if (it == right_tracks.end()) return false;
        auto left_extra = left->getExtraData();
        auto right_extra = (*it)->getExtraData();
        if (bool(left_extra) != bool(right_extra)) return false;
        if (left_extra && (left_extra->size() != right_extra->size() ||
                           memcmp(left_extra->data(), right_extra->data(), left_extra->size()) != 0)) {
            return false;
        }
    }
    return true;
}

void MergedMP4Reader::onTrackReady(TrackMap &map,
                                   const vector<Track::Ptr> &tracks,
                                   function<void(const Frame::Ptr &)> cb) {
    map.clear();
    for (const auto &track : tracks) {
        auto clone = track->clone();
        clone->addDelegate([cb](const Frame::Ptr &frame) {
            cb(frame);
            return true;
        });
        map[track->getIndex()] = {std::move(clone), false};
    }
}

void MergedMP4Reader::onTrackFrame(TrackMap &map,
                                   const Frame::Ptr &frame,
                                   const function<void(const Frame::Ptr &)> &cb) {
    auto it = map.find(frame->getIndex());
    if (it == map.end()) {
        cb(frame);
        return;
    }
    it->second.second = true;
    it->second.first->inputFrame(frame);
}

void MergedMP4Reader::closeTimelineClip() {
    _timeline_demuxer.reset();
    _timeline_track_map.clear();
    _timeline_pending_frame.reset();
    _timeline_pending_key = false;
    _timeline_priming = false;
}

void MergedMP4Reader::resetPreparedClip() {
    _timeline_next = PreparedClip();
}

int64_t MergedMP4Reader::clipPlayFromMs(size_t index) const {
    if (_timeline_demuxer && index == _timeline_clip_idx) return _timeline_window.play_from_ms;
    if (_timeline_next.demuxer && index == _timeline_next.index) return _timeline_next.play_from_ms;
    return makeReplayClipWindow(_timeline_clips[index], _replay_start_time_s).play_from_ms;
}

bool MergedMP4Reader::openTimelineClip(size_t index) {
    if (index >= _timeline_clips.size()) return false;
    const auto &clip = _timeline_clips[index];
    auto window = makeReplayClipWindow(clip, _replay_start_time_s);
    MP4Demuxer::Ptr demuxer;
    if (_timeline_next.demuxer && _timeline_next.index == index) {
        demuxer = _timeline_next.demuxer;
        window.play_from_ms = _timeline_next.play_from_ms;
    }
    resetPreparedClip();
    const bool prepared = !!demuxer;
    try {
        if (!prepared) {
            demuxer = make_shared<MP4Demuxer>();
            demuxer->openMP4(clip.file_path);
        }
        auto raw_tracks = demuxer->getTracks(false);
        auto tracks = normalizeTracks(raw_tracks);
        if (tracks.empty()) return false;

        const bool track_changed = _muxer && !sameTrackSet(_timeline_tracks, tracks);
        _timeline_demuxer = std::move(demuxer);
        _timeline_tracks = tracks;
        _timeline_clip_idx = index;
        _timeline_window = window;
        _timeline_pending_frame.reset();
        _timeline_pending_key = false;
        _timeline_priming = false;
        for (const auto &track : tracks) {
            if (track->getTrackType() == TrackVideo) {
                _timeline_priming = true;
                break;
            }
        }

        const int64_t file_offset = _timeline_window.file_offset_ms;
        if (!prepared && file_offset > 0) {
            // The seek lands on the nearest keyframe; frames before the clip
            // start are dropped in readTimelineTick until the next IDR.
            const int64_t landed = _timeline_demuxer->seekTo(file_offset);
            if (landed < 0) {
                WarnL << "Replay clip seek failed: " << clip.file_path << ", offset=" << file_offset;
            } else if (landed != file_offset) {
                DebugL << "Replay clip seek landed at " << landed << "ms instead of " << file_offset
                       << "ms: " << clip.file_path;
            }
        }

        onTrackReady(_timeline_track_map, raw_tracks, [this](const Frame::Ptr &frame) {
            frame->setIndex(frame->getTrackType() == TrackVideo ? 0 : 1);
            inputFrame(frame);
        });

        if (!_muxer) initOutputMuxer(_timeline_tracks);
        else if (track_changed) switchToMuxer(_timeline_tracks);
        prepareNextClip(index);
        return true;
    } catch (const exception &ex) {
        WarnL << "Failed to open replay clip: " << clip.file_path << ", error=" << ex.what();
        closeTimelineClip();
        return false;
    }
}

void MergedMP4Reader::prepareNextClip(size_t index) {
    const size_t next_index = index + 1;
    if (next_index >= _timeline_clips.size()) return;
    const auto &clip = _timeline_clips[next_index];
    // Only a Main<->Sub switch at a shared boundary: the next stream was
    // already recording, so its file has a keyframe before the boundary.
    if (clip.source == _timeline_clips[index].source) return;
    const auto next_window = makeReplayClipWindow(clip, _replay_start_time_s);
    if (next_window.file_offset_ms <= 0 || next_window.start_ms != _timeline_window.end_ms) return;

    try {
        auto demuxer = make_shared<MP4Demuxer>();
        demuxer->openMP4(clip.file_path);
        const int64_t landed = seekKeyframeAtOrBefore([demuxer](int64_t stamp) {
            return demuxer->seekTo(stamp);
        }, next_window.file_offset_ms);

        PreparedClip prepared;
        prepared.index = next_index;
        prepared.demuxer = demuxer;
        prepared.play_from_ms = next_window.play_from_ms;
        int64_t switch_ms = 0;
        if (replaySwitchPoint(_timeline_window, next_window, landed, switch_ms)) {
            // Play the current clip up to the next clip's keyframe, then the
            // next clip from that keyframe: no overlap and no dropped GOP.
            _timeline_window.end_ms = switch_ms;
            prepared.play_from_ms = switch_ms;
            DebugL << "Replay switch to " << clip.file_path << " moved from " << next_window.start_ms
                   << "ms to keyframe at " << switch_ms << "ms";
        }
        _timeline_next = std::move(prepared);
    } catch (const exception &ex) {
        // openTimelineClip retries the file and reports a terminal error.
        WarnL << "Unable to prepare replay clip: " << clip.file_path << ", error=" << ex.what();
    }
}

void MergedMP4Reader::configureTimelineAudio() {
    bool baseline_set = false;
    bool baseline_has_audio = false;
    CodecId baseline_codec = CodecInvalid;
    Buffer::Ptr baseline_extra;
    unordered_set<string> inspected;

    // A recorded stream keeps its audio config across files, so probing the
    // first clip of each stream is enough and avoids opening a whole day of
    // MP4 headers before playback starts.
    for (const auto &clip : _timeline_clips) {
        if (!inspected.insert(clip.stream_id).second) {
            continue;
        }
        try {
            auto demuxer = make_shared<MP4Demuxer>();
            demuxer->openMP4(clip.file_path);
            Track::Ptr audio;
            for (const auto &track : demuxer->getTracks(false)) {
                if (track && track->getTrackType() == TrackAudio && !audio) {
                    audio = track;
                } else if (track && track->getTrackType() == TrackVideo) {
                    // View overlay transcode keeps the largest clip size across Main<->Sub switches.
                    auto video = static_pointer_cast<VideoTrack>(track);
                    if (video->getVideoWidth() * video->getVideoHeight() > _option.transcode_width * _option.transcode_height) {
                        _option.transcode_width = video->getVideoWidth();
                        _option.transcode_height = video->getVideoHeight();
                    }
                }
            }
            if (!_enable_audio) {
                continue;
            }

            const bool has_audio = !!audio;
            if (!baseline_set) {
                baseline_set = true;
                baseline_has_audio = has_audio;
                if (audio) {
                    baseline_codec = audio->getCodecId();
                    baseline_extra = audio->getExtraData();
                }
                continue;
            }

            bool compatible = baseline_has_audio == has_audio;
            if (compatible && audio) {
                auto extra = audio->getExtraData();
                compatible = baseline_codec == audio->getCodecId() &&
                             bool(baseline_extra) == bool(extra);
                if (compatible && baseline_extra) {
                    compatible = baseline_extra->size() == extra->size() &&
                                 memcmp(baseline_extra->data(), extra->data(), extra->size()) == 0;
                }
            }
            if (!compatible) {
                _enable_audio = false;
                _add_mute_audio = false;
                InfoL << "Disable replay audio because selected clips have incompatible audio tracks";
            }
        } catch (const exception &ex) {
            // Opening the clip itself will report the terminal replay error.
            WarnL << "Unable to inspect replay audio for " << clip.file_path << ": " << ex.what();
        }
    }
}

bool MergedMP4Reader::readTimelineTick() {
    // seekTo() runs on the caller's thread and rebuilds the clip state.
    lock_guard<recursive_mutex> lck(_mtx);
    if (!_muxer || _timeline_clips.empty()) return false;
    if (_replay_paused) return true;

    GET_CONFIG(uint32_t, sample_ms, Record::kSampleMS);
    uint64_t target_ms = _replay_current_ms + uint64_t(sample_ms * _replay_speed);

    for (;;) {
        if (_timeline_clip_idx >= _timeline_clips.size()) {
            if (!_replay_file_repeat) return false;
            _timeline_clip_idx = 0;
            _replay_current_ms = 0;
            _timeline_discard_before_ms = 0;
            closeTimelineClip();
            resetPreparedClip();
            if (!openTimelineClip(0)) return false;
            continue;
        }

        const uint64_t clip_start = static_cast<uint64_t>(max<int64_t>(0, clipPlayFromMs(_timeline_clip_idx)));
        if (_replay_current_ms < clip_start) {
            _replay_current_ms = min(target_ms, clip_start);
            return true;
        }
        if (!_timeline_demuxer && !openTimelineClip(_timeline_clip_idx)) return false;

        bool key = false, eof = false;
        Frame::Ptr raw;
        if (_timeline_pending_frame) {
            raw = std::move(_timeline_pending_frame);
            key = _timeline_pending_key;
        } else {
            raw = _timeline_demuxer->readFrame(key, eof);
        }
        if (eof || !raw) {
            closeTimelineClip();
            ++_timeline_clip_idx;
            continue;
        }

        const int64_t mapped_dts = _timeline_window.map(static_cast<int64_t>(raw->dts()));
        const int64_t mapped_pts = _timeline_window.map(static_cast<int64_t>(raw->pts()));
        if (_timeline_window.pastEnd(mapped_dts)) {
            closeTimelineClip();
            ++_timeline_clip_idx;
            continue;
        }
        // Frames before the clip start belong to the previous clip's range.
        if (_timeline_window.beforeStart(mapped_dts)) continue;
        if (mapped_dts < static_cast<int64_t>(_timeline_discard_before_ms)) continue;
        if (_timeline_priming) {
            // Skip to the clip's first video sync sample. The MP4 sync flag is
            // used rather than Frame::keyFrame(): a recorded keyframe sample
            // carries SPS/PPS in front of the IDR (a config frame), and the
            // clip's track clone may flag the first slice after a config
            // frame as a keyframe (H264Track), so a P-frame would end priming.
            if (raw->getTrackType() != TrackVideo || !key) continue;
            _timeline_priming = false;
            // The skipped frames are not paced: playback starts at the
            // keyframe at once instead of up to one GOP after the request.
            if (mapped_dts > static_cast<int64_t>(_replay_current_ms)) {
                _replay_current_ms = static_cast<uint64_t>(mapped_dts);
                target_ms = _replay_current_ms + uint64_t(sample_ms * _replay_speed);
            }
        }
        if (mapped_dts > static_cast<int64_t>(target_ms)) {
            _timeline_pending_frame = std::move(raw);
            _timeline_pending_key = key;
            _replay_current_ms = target_ms;
            return true;
        }

        auto stamped = make_shared<FrameStamp>(raw);
        stamped->setStamp(mapped_dts, mapped_pts);

        onTrackFrame(_timeline_track_map, stamped, [this](const Frame::Ptr &frame) {
            frame->setIndex(frame->getTrackType() == TrackVideo ? 0 : 1);
            inputFrame(frame);
        });
        if (mapped_dts > static_cast<int64_t>(_replay_current_ms)) {
            _replay_current_ms = static_cast<uint64_t>(mapped_dts);
        }
        if (_replay_current_ms >= target_ms) return true;
    }
}

bool MergedMP4Reader::startTimelineReplay(uint64_t sample_ms, bool ref_self, bool file_repeat) {
    if (!parseReplayStartTime(_tuple.stream, _replay_start_time_s)) {
        WarnL << "Invalid replay stream id: " << _tuple.stream;
        return false;
    }
    const string app = split(_tuple.stream, "/").front();

    string main_stream_id, sub_stream_id;
    Broadcast::StreamQualityInvoker quality_invoker = [&](const map<int, string> &quality_map) {
        auto main = quality_map.find(0);
        auto sub = quality_map.find(1);
        if (main != quality_map.end()) main_stream_id = main->second;
        if (sub != quality_map.end()) sub_stream_id = sub->second;
    };
    NOTICE_EMIT(BroadcastGetStreamQualityArgs, Broadcast::kBroadcastGetStreamQuality,
                _tuple.vhost, app, quality_invoker);

    auto params = Parser::parseArgs(_tuple.params);
    const string quality = params.count("quality") ? params["quality"] : "auto";
    const string preferred = params.count("prefered") ? params["prefered"] : "hi";
    const uint64_t max_duration = params.count("duration")
        ? strtoull(params["duration"].data(), nullptr, 10) : 0;

    Broadcast::ReplayTimelineInvoker timeline_invoker = [&](const vector<Broadcast::ReplayClip> &clips, uint64_t total) {
        _timeline_clips = clips;
        _replay_total_dur_s = total;
    };
    NOTICE_EMIT(BroadcastGetReplayTimelineArgs, Broadcast::kBroadcastGetReplayTimeline,
                _tuple.vhost, app, main_stream_id, sub_stream_id, _replay_start_time_s,
                max_duration, quality, preferred, timeline_invoker);
    if (_timeline_clips.empty()) {
        WarnL << "No replay clips found for app=" << app << ", start=" << _replay_start_time_s;
        return false;
    }

    _timeline_mode = true;
    configureTimelineAudio();
    _timeline_clip_idx = 0;
    _replay_current_ms = 0;
    _timeline_discard_before_ms = 0;
    _replay_file_repeat = file_repeat;
    closeTimelineClip();
    resetPreparedClip();
    if (!openTimelineClip(0)) return false;

    GET_CONFIG(uint32_t, sampleMS, Record::kSampleMS);
    auto timer_sec = (sample_ms ? sample_ms : sampleMS) / 1000.0f;
    if (ref_self) {
        auto self = shared_from_this();
        _replay_timer = make_shared<Timer>(timer_sec, [self]() { return self->readTimelineTick(); }, _poller);
    } else {
        weak_ptr<MergedMP4Reader> weak_self = shared_from_this();
        _replay_timer = make_shared<Timer>(timer_sec, [weak_self]() {
            auto self = weak_self.lock();
            return self ? self->readTimelineTick() : false;
        }, _poller);
    }
    readTimelineTick();
    return true;
}

bool MergedMP4Reader::openForReplay(uint64_t sample_ms, bool ref_self, bool file_repeat) {
    if (!_replay_mode) {
        WarnL << "Replay mode is unavailable for app=" << _tuple.app;
        return false;
    }
    if (!startTimelineReplay(sample_ms, ref_self, file_repeat)) {
        WarnL << "Replay timeline could not be built for " << _tuple.shortUrl();
        return false;
    }
    return true;
}

void MergedMP4Reader::stopReplay() {
    lock_guard<recursive_mutex> lck(_mtx);
    _replay_timer.reset();
    _timeline_clips.clear();
    _timeline_clip_idx = 0;
    _timeline_tracks.clear();
    _timeline_discard_before_ms = 0;
    _timeline_mode = false;
    closeTimelineClip();
    resetPreparedClip();
}

bool MergedMP4Reader::seekTo(MediaSource &sender, uint32_t stamp) {
    pause(sender, false);
    TraceL << getOriginUrl(sender) << ",stamp:" << stamp;
    return seekTo(stamp);
}

bool MergedMP4Reader::pause(MediaSource &sender, bool pause_value) {
    lock_guard<recursive_mutex> lck(_mtx);
    _replay_paused = pause_value;
    return true;
}

bool MergedMP4Reader::speed(MediaSource &sender, float speed_value) {
    if (speed_value < 0.1f || speed_value > 20.0f) return false;
    lock_guard<recursive_mutex> lck(_mtx);
    _replay_paused = false;
    _replay_speed = speed_value;
    return true;
}

bool MergedMP4Reader::close(MediaSource &sender) {
    _replay_timer.reset();
    return true;
}

MediaOriginType MergedMP4Reader::getOriginType(MediaSource &sender) const {
    return MediaOriginType::mp4_vod;
}

string MergedMP4Reader::getOriginUrl(MediaSource &sender) const {
    return _tuple.shortUrl();
}

toolkit::EventPoller::Ptr MergedMP4Reader::getOwnerPoller(MediaSource &sender) {
    return _poller;
}

int MergedMP4Reader::totalReaderCount(MediaSource &sender) {
    return _muxer ? _muxer->totalReaderCount() : 0;
}

bool MergedMP4Reader::seekTo(uint32_t stamp_seek) {
    lock_guard<recursive_mutex> lck(_mtx);
    if (!_timeline_mode) return false;
    if (stamp_seek > _replay_total_dur_s * 1000) return false;

    size_t target = _timeline_clips.size();
    for (size_t i = 0; i < _timeline_clips.size(); ++i) {
        const uint64_t begin = (_timeline_clips[i].start_time - _replay_start_time_s) * 1000;
        const uint64_t end = begin + uint64_t(_timeline_clips[i].duration) * 1000;
        if (end > stamp_seek) {
            target = i;
            break;
        }
    }

    _replay_current_ms = stamp_seek;
    _timeline_discard_before_ms = stamp_seek;
    closeTimelineClip();
    resetPreparedClip();
    _timeline_clip_idx = target;
    if (target < _timeline_clips.size()) {
        const uint64_t begin = (_timeline_clips[target].start_time - _replay_start_time_s) * 1000;
        if (stamp_seek >= begin && !openTimelineClip(target)) return false;
    }
    return true;
}

} // namespace mediakit

#endif // ENABLE_MP4
