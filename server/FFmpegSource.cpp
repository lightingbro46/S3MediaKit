#include "FFmpegSource.h"
#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Common/MultiMediaSourceMuxer.h"
#include "Util/base64.h"
#include "Util/File.h"
#include "Util/NoticeCenter.h"
#include "System.h"
#include "Thread/WorkThreadPool.h"
#include "Network/sockutil.h"
#include "Network/Socket.h"
#include "Local/TimeQuery.h"
#include "Transcode/OverlayPrivacyUtils.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include "Common/StrUtil.h"
#include "User/UserAuditLog.h"
#include "Local/StatisticRecorder.h"
#include "WebApiErrCode.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
using namespace managerkit;

namespace FFmpeg {
#define FFmpeg_FIELD "ffmpeg."
const string kBin = FFmpeg_FIELD"bin";
const string kBinP = FFmpeg_FIELD"binP";
const string kCmd = FFmpeg_FIELD"cmd";
const string kLog = FFmpeg_FIELD"log";
const string kSnap = FFmpeg_FIELD"snap";
const string kExtract = FFmpeg_FIELD"extract";
const string kExtractOverlay = FFmpeg_FIELD"extract_overlay";
const string kProbe = FFmpeg_FIELD"probe";
const string kRestartSec = FFmpeg_FIELD"restart_sec";
const string kDelayCloseSec = FFmpeg_FIELD"delay_close_sec";

onceToken token([]() {
#ifdef _WIN32
    string ffmpeg_bin = trim(System::execute("where ffmpeg"));
    string ffprobe_bin = trim(System::execute("where ffprobe"));
#else
    string ffmpeg_bin = trim(System::execute("which ffmpeg"));
    string ffprobe_bin = trim(System::execute("which ffprobe"));
#endif
    // Default ffmpeg command path is the path in the environment variable
    mINI::Instance()[kBin] = ffmpeg_bin.empty() ? "ffmpeg" : ffmpeg_bin;
    mINI::Instance()[kBinP] = ffprobe_bin.empty() ? "ffprobe" : ffprobe_bin;
    // ffmpeg log save path
    mINI::Instance()[kLog] = "./ffmpeg/ffmpeg.log";
    mINI::Instance()[kCmd] = "%s -re -i %s -c:a aac -strict -2 -ar 44100 -ab 48k -c:v libx264 -f flv %s";
    // mINI::Instance()[kSnap] = "%s -i %s -y -f mjpeg -frames:v 1 -an %s"; // backward compatibility, do not delete
    mINI::Instance()[kSnap] = "%s -i %s -ss %s -y -f mjpeg -frames:v 1 -an %s";
    // mINI::Instance()[kExtract] = "%s -f concat -safe 0 -i %s -y -metadata title=%s -metadata comment=%s -metadata date=%s -metadata artist=%s -c copy %s"; // backward compatibility, do not delete
    mINI::Instance()[kExtract] = "%s -f concat -safe 0 -i %s -y -ss %s -to %s -metadata title=%s -metadata comment=%s -metadata date=%s -metadata artist=%s -c:v copy -c:a aac %s";
    mINI::Instance()[kExtractOverlay] = "%s -f concat -safe 0 -i %s -y -ss %s -to %s -filter_complex %s -map [v] -map 0:a? "
                                            "-metadata title=%s -metadata comment=%s -metadata date=%s -metadata artist=%s "
                                            "-c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac %s";
    mINI::Instance()[kProbe] = "%s -rtsp_transport tcp -print_format json -show_streams -show_format -show_error -select_streams v:0 %s";
    mINI::Instance()[kRestartSec] = 0;
    mINI::Instance()[kDelayCloseSec] = 300;
});
}

FFmpegSource::FFmpegSource() {
    _poller = EventPollerPool::Instance().getPoller();
}

FFmpegSource::~FFmpegSource() {
    DebugL;
}

static bool is_local_ip(const string &ip){
    if (ip == "127.0.0.1" || ip == "localhost") {
        return true;
    }
    auto ips = SockUtil::getInterfaceList();
    for (auto &obj : ips) {
        if (ip == obj["ip"]) {
            return true;
        }
    }
    return false;
}

static size_t countSubString(const std::string &str, const std::string &sub) {
    if (sub.empty()) {
        return 0;
    }
    size_t count = 0;
    size_t pos = 0;
    while ((pos = str.find(sub, pos)) != std::string::npos) {
        ++count;
        pos += sub.length();
    }
    return count;
}

static std::string format_duration_hms(int64_t total_seconds) {
    int64_t seconds = total_seconds % 60;
    int64_t total_minutes = total_seconds / 60;
    int64_t minutes = total_minutes % 60;
    int64_t hours = total_minutes / 60;

    _StrPrinter oss;
    oss << std::setw(2) << std::setfill('0') << hours << ":"
        << std::setw(2) << std::setfill('0') << minutes << ":"
        << std::setw(2) << std::setfill('0') << seconds;
    return oss;
}

void FFmpegSource::setupRecordFlag(bool enable_hls, bool enable_mp4){
    _enable_hls = enable_hls;
    _enable_mp4 = enable_mp4;
}

void FFmpegSource::play(const string &ffmpeg_cmd_key, const string &src_url, const string &dst_url, int timeout_ms, const onPlay &cb) {
    GET_CONFIG(string, ffmpeg_bin, FFmpeg::kBin);
    GET_CONFIG(string, ffmpeg_cmd_default, FFmpeg::kCmd);
    GET_CONFIG(string, ffmpeg_log, FFmpeg::kLog);

    _src_url = src_url;
    _dst_url = dst_url;
    _ffmpeg_cmd_key = ffmpeg_cmd_key;

    try {
        _media_info.parse(dst_url);

        auto ffmpeg_cmd = ffmpeg_cmd_default;
        if (!ffmpeg_cmd_key.empty()) {
            auto cmd_it = mINI::Instance().find(ffmpeg_cmd_key);
            if (cmd_it != mINI::Instance().end()) {
                ffmpeg_cmd = cmd_it->second;
            } else {
                WarnL << "In the configuration file, the ffmpeg command template (" << ffmpeg_cmd_key << ")Does not exist, the default template has been adopted(" << ffmpeg_cmd_default << ")";
            }
        }
        if (!toolkit::start_with(ffmpeg_cmd, "%s")) {
            throw std::invalid_argument("ffmpeg cmd template must start with '%s'");
        }

        char cmd[2048] = { 0 };
        snprintf(cmd, sizeof(cmd), ffmpeg_cmd.data(), File::absolutePath("", ffmpeg_bin).data(), src_url.data(), dst_url.data());
        auto log_file = ffmpeg_log.empty() ? "" : File::absolutePath("", ffmpeg_log);
        _process.run(cmd, log_file);
        _cmd = cmd;
        InfoL << cmd;

        if (is_local_ip(_media_info.host)) {
            // Push stream to yourself, judge whether the stream is registered to determine whether it is normal
            if (_media_info.schema != RTSP_SCHEMA && _media_info.schema != RTMP_SCHEMA && _media_info.schema != "srt") {
                cb(SockException(Err_other, "This service only supports rtmp/rtsp/srt streaming"));
                return;
            }
            weak_ptr<FFmpegSource> weakSelf = shared_from_this();
            findAsync(timeout_ms, [cb, weakSelf, timeout_ms](const MediaSource::Ptr &src) {
                auto strongSelf = weakSelf.lock();
                if (!strongSelf) {
                    // Self has been destroyed
                    return;
                }
                if (src) {
                    // Push stream to yourself successfully
                    cb(SockException());
                    strongSelf->onGetMediaSource(src);
                    strongSelf->startTimer(timeout_ms);
                    return;
                }
                // Push stream failed
                if (!strongSelf->_process.wait(false)) {
                    // ffmpeg process has exited
                    cb(SockException(Err_other, StrPrinter << "ffmpeg has exited, exit code = " << strongSelf->_process.exit_code()));
                    return;
                }
                // ffmpeg process is still online, but waiting for the stream to timeout
                cb(SockException(Err_other, "Waiting timed out"));
            });
        } else {
            // Push stream to other servers, judge whether it is successful by judging whether the FFmpeg process is online
            weak_ptr<FFmpegSource> weakSelf = shared_from_this();
            _timer = std::make_shared<Timer>(
                timeout_ms / 1000.0f,
                [weakSelf, cb, timeout_ms]() {
                    auto strongSelf = weakSelf.lock();
                    if (!strongSelf) {
                        // Self has been destroyed
                        return false;
                    }
                    // FFmpeg is still online, so we think the push stream is successful
                    if (strongSelf->_process.wait(false)) {
                        cb(SockException());
                        strongSelf->startTimer(timeout_ms);
                        return false;
                    }
                    // ffmpeg process has exited
                    cb(SockException(Err_other, StrPrinter << "ffmpeg has exited, exit code = " << strongSelf->_process.exit_code()));
                    return false;
                },
                _poller);
        }
    } catch (std::exception &ex) {
        WarnL << ex.what();
        cb(SockException(Err_other, ex.what()));
    }
}

void FFmpegSource::findAsync(int maxWaitMS, const function<void(const MediaSource::Ptr &src)> &cb) {
    auto src = MediaSource::find(_media_info.schema, _media_info.vhost, _media_info.app, _media_info.stream);
    if (src || !maxWaitMS) {
        cb(src);
        return;
    }

    void *listener_tag = this;
    // Execute the media registration timeout callback after a few seconds
    auto onRegistTimeout = _poller->doDelayTask(maxWaitMS, [cb, listener_tag]() {
        // Cancel listening to this event
        NoticeCenter::Instance().delListener(listener_tag, Broadcast::kBroadcastMediaChanged);
        cb(nullptr);
        return 0;
    });

    weak_ptr<FFmpegSource> weakSelf = shared_from_this();
    auto onRegist = [listener_tag, weakSelf, cb, onRegistTimeout](BroadcastMediaChangedArgs) {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            // Self has been destroyed, cancel the delayed task
            onRegistTimeout->cancel();
            NoticeCenter::Instance().delListener(listener_tag, Broadcast::kBroadcastMediaChanged);
            return;
        }

        if (!bRegist || sender.getSchema() != strongSelf->_media_info.schema ||
            !equalMediaTuple(sender.getMediaTuple(), strongSelf->_media_info)) {
            // Not an event of interest, ignore it
            return;
        }

        // The stream you are looking for is finally registered; cancel the delayed task to prevent multiple callbacks
        onRegistTimeout->cancel();
        // Cancel event listening
        NoticeCenter::Instance().delListener(listener_tag, Broadcast::kBroadcastMediaChanged);

        // Switch to your own thread and then reply
        strongSelf->_poller->async([weakSelf, cb]() {
            if (auto strongSelf = weakSelf.lock()) {
                // Find the media source again, usually you can find it
                strongSelf->findAsync(0, cb);
            }
        }, false);
    };
    // Listen to media registration events
    NoticeCenter::Instance().addListener(listener_tag, Broadcast::kBroadcastMediaChanged, onRegist);
}

/**
 * Check if the media is online regularly
 */
void FFmpegSource::startTimer(int timeout_ms) {
    weak_ptr<FFmpegSource> weakSelf = shared_from_this();
    GET_CONFIG(uint64_t,ffmpeg_restart_sec,FFmpeg::kRestartSec);
    _timer = std::make_shared<Timer>(1.0f, [weakSelf, timeout_ms]() {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            // Self has been destroyed
            return false;
        }
        bool needRestart = ffmpeg_restart_sec > 0 && strongSelf->_replay_ticker.elapsedTime() > ffmpeg_restart_sec * 1000;
        if (is_local_ip(strongSelf->_media_info.host)) {
            // Push stream to yourself, we judge whether FFmpeg is working properly by checking whether it has been registered
            strongSelf->findAsync(0, [&](const MediaSource::Ptr &src) {
                // Synchronously find the stream
                if (!src || needRestart) {
                    if (needRestart) {
                        strongSelf->_replay_ticker.resetTime();
                        if (strongSelf->_process.wait(false)) {
                            // The FFmpeg process is still running, timeout and close it
                            strongSelf->_process.kill(2000);
                        }
                        InfoL << "FFmpeg will be restarted soon and will continue to pull the stream " << strongSelf->_src_url;
                    }
                    // The stream is not online, re-pull the stream, here the original timeout was 10 seconds, but it was found that 10 seconds was not enough, so it was changed to 20 seconds
                    if (strongSelf->_replay_ticker.elapsedTime() > 20 * 1000) {
                        // The last retry time exceeds 10 seconds, then retry FFmpeg to pull the stream
                        strongSelf->_replay_ticker.resetTime();
                        strongSelf->play(strongSelf->_ffmpeg_cmd_key, strongSelf->_src_url, strongSelf->_dst_url, timeout_ms, [](const SockException &) {});
                    }
                }
            });
        } else {
            // Push stream to other servers, we judge whether the FFmpeg process is online, if FFmpeg push stream is interrupted, then it should exit automatically
            if (!strongSelf->_process.wait(false) || needRestart) {
                if (needRestart) {
                    strongSelf->_replay_ticker.resetTime();
                    if (strongSelf->_process.wait(false)) {
                        // The FFmpeg process is still running, timeout and close it
                        strongSelf->_process.kill(2000);
                    }
                    InfoL << "FFmpeg will be restarted soon and will continue to pull the stream " << strongSelf->_src_url;
                }
                // ffmpeg is not online, re-pull the stream
                strongSelf->play(strongSelf->_ffmpeg_cmd_key, strongSelf->_src_url, strongSelf->_dst_url, timeout_ms, [weakSelf](const SockException &ex) {
                    if (!ex) {
                        // No error
                        return;
                    }
                    auto strongSelf = weakSelf.lock();
                    if (!strongSelf) {
                        // Self has been destroyed
                        return;
                    }
                    // Retry FFmpeg stream pulling if the last retry time is over 10 seconds
                    strongSelf->startTimer(10 * 1000);
                });
            }
        }
        return true;
    }, _poller);
}

void FFmpegSource::setOnClose(const function<void()> &cb){
    _onClose = cb;
}

bool FFmpegSource::close(MediaSource &sender) {
    auto listener = getDelegate();
    if (listener && !listener->close(sender)) {
        // Close failed
        return false;
    }
    // No one is watching this stream, let's stop it
    if (_onClose) {
        _onClose();
    }
    return true;
}

MediaOriginType FFmpegSource::getOriginType(MediaSource &sender) const{
    return MediaOriginType::ffmpeg_pull;
}

string FFmpegSource::getOriginUrl(MediaSource &sender) const {
    return _src_url;
}

void FFmpegSource::onGetMediaSource(const MediaSource::Ptr &src) {
    auto muxer = src->getMuxer();
    auto listener = muxer ? muxer->getDelegate() : nullptr;
    if (listener && listener.get() != this) {
        // Prevent the bug of infinite recursive calls caused by entering the onGetMediaSource function multiple times
        setDelegate(listener);
        muxer->setDelegate(shared_from_this());
        if (_enable_hls) {
            src->getOwnerPoller()->async([=]() mutable {
                 src->setupRecord(Recorder::type_hls, true, "", 0);
            });
        }
        if (_enable_mp4) {
            src->getOwnerPoller()->async([=]() mutable {
                src->setupRecord(Recorder::type_mp4, true, "", 0);
            });
        }
    }
}

#if defined(ENABLE_FFMPEG)
#include "Player/MediaPlayer.h"
#include "Codec/Transcode.h"

static void makeSnapAsync(const string &play_url, const string &save_path, float timeout_sec, const FFmpegSnap::onSnap &cb) {
    struct Holder {
        MediaPlayer::Ptr player;
    };
    auto holder = std::make_shared<Holder>();
    auto player = std::make_shared<MediaPlayer>();
    (*player)[mediakit::Client::kTimeoutMS] = timeout_sec * 1000;

    player->setOnPlayResult([holder, save_path, cb, timeout_sec](const SockException &ex) mutable {
        onceToken token(nullptr, [&]() { holder->player = nullptr; });
        auto video = ex ? nullptr : dynamic_pointer_cast<VideoTrack>(holder->player->getTrack(TrackVideo, false));
        if (!video) {
            cb(false, ex ? ex.what() : "none video track");
            return;
        }
        auto decoder = std::make_shared<FFmpegDecoder>(video);
        auto new_holder = std::make_shared<Holder>(*holder);
        auto timer = EventPollerPool::Instance().getPoller()->doDelayTask(1000 * timeout_sec, [cb, new_holder]() {
            // Prevents the player from being unable to release if decoding fails
            new_holder->player = nullptr;
            cb(false, "decode frame timeout");
            return 0;
        });
        auto done = false;
        decoder->setOnDecode([save_path, new_holder, cb, done, timer](const FFmpegFrame::Ptr &frame) mutable {
            if (done) {
                return;
            }
            onceToken token(nullptr, [&]() { new_holder->player = nullptr; timer->cancel(); done = true; });
            auto ret = FFmpegUtils::saveFrame(frame, save_path.data());
            cb(std::get<0>(ret), std::get<1>(ret));
        });
        video->addDelegate([decoder](const Frame::Ptr &frame) { return decoder->inputFrame(frame, false, true); });
    });
    player->play(play_url);
    holder->player = std::move(player);
}

#endif

void FFmpegSnap::makeSnap(bool async, const string &play_url, const string &save_path, uint64_t seek_time, float timeout_sec, const onSnap &cb) {
#if defined(ENABLE_FFMPEG)
    if (async) {
        makeSnapAsync(play_url, save_path, timeout_sec, cb);
        return;
    }
#endif

    GET_CONFIG(string, ffmpeg_bin, FFmpeg::kBin);
    GET_CONFIG(string, ffmpeg_snap, FFmpeg::kSnap);
    GET_CONFIG(string, ffmpeg_log, FFmpeg::kLog);
    Ticker ticker;
    WorkThreadPool::Instance().getPoller()->async([timeout_sec, play_url, save_path, seek_time, cb, ticker]() {
        auto elapsed_ms = ticker.elapsedTime();
        if (elapsed_ms > timeout_sec * 1000) {
            // Timeout, the background thread load is too high, it takes too long to start this task
            cb(false, "wait work poller schedule snap task timeout");
            return;
        }
        char cmd[2048] = { 0 };
        if (countSubString(ffmpeg_snap, "%s") == 3) {
            // Backward compatibility, the ffmpeg command template does not contain the time parameter, so it is considered that the ffmpeg command template does not contain the time parameter
            snprintf(cmd, sizeof(cmd), ffmpeg_snap.data(), File::absolutePath("", ffmpeg_bin).data(), play_url.data(), save_path.data());
        } else {
            snprintf(cmd, sizeof(cmd), ffmpeg_snap.data(), File::absolutePath("", ffmpeg_bin).data(), play_url.data(), format_duration_hms(seek_time).data(), save_path.data());
        }

        std::shared_ptr<Process> process = std::make_shared<Process>();
        auto log_file = ffmpeg_log.empty() ? ffmpeg_log : File::absolutePath("", ffmpeg_log);
        process->run(cmd, log_file);

        // The timer delay should be reduced by the delay of the background task startup
        auto delayTask = EventPollerPool::Instance().getPoller()->doDelayTask(
            (uint64_t)(timeout_sec * 1000 - elapsed_ms), [process, cb, log_file, save_path]() {
                if (process->wait(false)) {
                    // The FFmpeg process is still running, close it if it times out
                    process->kill(2000);
                }
                return 0;
            });

        // Wait for the FFmpeg process to exit
        process->wait(true);
        // The FFmpeg process has exited, the timer can be canceled
        delayTask->cancel();
        // Execute the callback function
        bool success = process->exit_code() == 0 && File::fileSize(save_path);
        cb(success, (!success && !log_file.empty()) ? File::loadFile(log_file) : "");
    });
}

FFmpegExtractor::FFmpegExtractor(MediaTuple &tuple, ExtractOptions &options, int timeout_ms, toolkit::EventPoller::Ptr poller)
    : _tuple(tuple), _options(options), _timeout_ms(timeout_ms) {
    _poller = _poller ? std::move(poller) : EventPollerPool::Instance().getPoller();
    _created_at = time(nullptr);
}

FFmpegExtractor::~FFmpegExtractor() {
    DebugL;
}

static void makeIndexFile(string &file_path, string &camera_id, string &stream_id, uint64_t start_time, uint64_t end_time,
        const function<void(const SockException &ex, uint32_t &duration_start, uint32_t &duration_end)> &cb) {
    uint32_t duration_start = 0;
    uint32_t duration_end = 0;
    auto file_ptr = std::shared_ptr<FILE>(File::create_file(file_path, "wb"), [](FILE *fp) {
        if (fp) {
            fflush(fp);
            fclose(fp);
        }
    });
    if (!file_ptr) {
        string err = (StrPrinter << "Failed to open the file:" << file_path);
        return cb(SockException(Err_other, err, ApiErrCode::CODE_EXTRACT_FAILED), duration_start, duration_end);
    }

    struct FileIndexs {
        vector<string> file_path;
        uint64_t dur_start;
        uint64_t dur_end;
    };
    unordered_map<string, FileIndexs> file_indexs_map;
    try {
        MediaTuple tuple = { DEFAULT_VHOST, camera_id, stream_id, "" };
        auto query = std::make_shared<TimeQuery>(tuple);
        query->getRecordedTimePeriod(start_time, end_time, [start_time, end_time, &file_indexs_map](const vector<TimeBlock> &ret) {
            for (const auto &block : ret) {
                auto stream_id = block.stream();
                auto &file_indexs = file_indexs_map[stream_id];
                uint64_t start_pos = block.start_time();
                uint64_t end_pos = block.start_time() + block.time_len();
                if (start_pos < start_time) {
                    file_indexs.dur_start += start_time - start_pos;
                    file_indexs.dur_end += file_indexs.dur_start;
                    start_pos = start_time;
                }
                if (end_pos > end_time) {
                    end_pos = end_time;
                }
                file_indexs.dur_end += end_pos - start_pos;
                file_indexs.file_path.push_back(block.file_path());
            }
        });
    } catch (const std::exception& ex) {
        WarnL << "TimeQuery init failed: " << ex.what();
    }

    uint64_t total_dur = 0;
    // Find the stream with the longest duration in the time period, and then extract the video based on this stream
    unordered_map<string, FileIndexs>::iterator file_indexs_it = file_indexs_map.end();
    for (auto it = file_indexs_map.begin(); it != file_indexs_map.end(); ++it) {
        auto &file_indexs = it->second;
        if (file_indexs.dur_end - file_indexs.dur_start <= 0 || file_indexs.file_path.empty()) {
            continue;
        }

        if (total_dur == 0 || file_indexs.dur_end - file_indexs.dur_start > total_dur) {
            file_indexs_it = it;
            total_dur = file_indexs.dur_end - file_indexs.dur_start;
        }
    }

    if (file_indexs_it != file_indexs_map.end()) {
        duration_start = file_indexs_it->second.dur_start;
        duration_end = file_indexs_it->second.dur_end;
        for (const auto &file_path : file_indexs_it->second.file_path) {
            auto line = "file '" + decodeBase64(file_path) + "'\n";
            fwrite(line.c_str(), line.size(), 1, file_ptr.get());
        }
    }
    // If there is no data in the time period, the index file will not be generated, and the callback will be executed directly
    return cb(total_dur == 0 ? SockException(Err_other, "No data in time period", ApiErrCode::CODE_EXTRACT_SEGMENT_NO_DATA) : SockException(), duration_start, duration_end);
}

static std::string getFileExtension(const std::string &filename) {
    auto pos = filename.find_last_of('.');
    if (pos == std::string::npos)
        return "";
    return filename.substr(pos + 1);
}

static std::string escape(const std::string &str) {
    std::string out = "\"";
    for (char c : str) {
        if (c == '\\' || c == '\"') {
            out += '\\';
        }
        out += c;
    }
    out += "\"";
    return out;
}

static std::string escape(const char* str) {
    return escape(std::string(str));
}

namespace {

// Minimal SockInfo used only to satisfy kBroadcastMediaViewOverlay's signature;
// the extraction task has no real network peer.
class NullSockInfo : public toolkit::SockInfo {
public:
    std::string get_local_ip() override { return ""; }
    uint16_t get_local_port() override { return 0; }
    std::string get_peer_ip() override { return ""; }
    uint16_t get_peer_port() override { return 0; }
};

// scale2ref's main_w/main_h refer to its first input. The extraction graph puts
// the SVG first, so those variables resolve to the SVG canvas rather than the
// source video. Probe the concat input and scale the SVG with explicit dimensions
// instead. The concat file is already generated by makeIndexFile at this point.
static bool probeConcatVideoSize(const std::string &concat_path, int &width, int &height) {
    GET_CONFIG(string, ffprobe_bin, FFmpeg::kBinP);
    if (ffprobe_bin.empty() || concat_path.empty()) {
        return false;
    }

    std::string command = escape(File::absolutePath("", ffprobe_bin)) +
        " -v error -f concat -safe 0 -i " + escape(concat_path) +
        " -select_streams v:0 -show_entries stream=width,height -of csv=p=0";
    const std::string output = trim(System::execute(command));
    if (output.empty()) {
        return false;
    }

    int parsed_width = 0;
    int parsed_height = 0;
    if (sscanf(output.c_str(), "%d,%d", &parsed_width, &parsed_height) != 2 &&
        sscanf(output.c_str(), "%dx%d", &parsed_width, &parsed_height) != 2) {
        return false;
    }
    if (parsed_width <= 0 || parsed_height <= 0) {
        return false;
    }
    width = parsed_width;
    height = parsed_height;
    return true;
}

// Build privacy-mask stages from polygon alpha masks. FFmpeg's drawbox/crop filters
// are rectangle-only, so the old implementation necessarily expanded every polygon
// to its bounding box. SVG rasterization gives FFmpeg an actual per-pixel polygon
// alpha channel while preserving the same canvas-to-frame coordinate mapping.
static std::string buildPolygonPrivacyMaskFilterComplex(
    const std::vector<PrivacyMaskRegion> &masks,
    int canvas_width, int canvas_height,
    int video_width, int video_height,
    const std::string &path_prefix,
    std::vector<std::string> &temporary_paths,
    std::string &last_label) {
    if (masks.empty() || video_width <= 0 || video_height <= 0) {
        last_label = "0:v";
        return "";
    }

    std::string current = "0:v";
    std::ostringstream graph;
    int stage = 0;
    for (size_t i = 0; i < masks.size(); ++i) {
        const PrivacyMaskRegion &mask = masks[i];
        if (mask.points.size() < 3) {
            continue;
        }

        double min_x = mask.points[0].first;
        double max_x = min_x;
        double min_y = mask.points[0].second;
        double max_y = min_y;
        for (size_t p = 1; p < mask.points.size(); ++p) {
            min_x = std::min(min_x, mask.points[p].first);
            max_x = std::max(max_x, mask.points[p].first);
            min_y = std::min(min_y, mask.points[p].second);
            max_y = std::max(max_y, mask.points[p].second);
        }
        min_x = std::max(0.0, std::min(static_cast<double>(canvas_width), min_x));
        max_x = std::max(0.0, std::min(static_cast<double>(canvas_width), max_x));
        min_y = std::max(0.0, std::min(static_cast<double>(canvas_height), min_y));
        max_y = std::max(0.0, std::min(static_cast<double>(canvas_height), max_y));

        const int crop_x = std::max(0, std::min(video_width - 1,
            static_cast<int>(min_x * video_width / std::max(1, canvas_width))));
        const int crop_y = std::max(0, std::min(video_height - 1,
            static_cast<int>(min_y * video_height / std::max(1, canvas_height))));
        const int crop_right = std::max(crop_x, std::min(video_width - 1,
            static_cast<int>(max_x * video_width / std::max(1, canvas_width))));
        const int crop_bottom = std::max(crop_y, std::min(video_height - 1,
            static_cast<int>(max_y * video_height / std::max(1, canvas_height))));
        const int crop_width = crop_right - crop_x + 1;
        const int crop_height = crop_bottom - crop_y + 1;

        const char *type_name = mask.mask_type == PrivacyMaskRegion::SOLID ? "solid" :
                                mask.mask_type == PrivacyMaskRegion::BLUR ? "blur" : "pixelate";
        const std::string mask_path = path_prefix + ".privacy." + std::to_string(i) + "." + type_name + ".svg";
        std::vector<PrivacyMaskRegion> single_mask(1, mask);
        const std::string mask_svg = OverlayPrivacyUtils::buildPrivacyMaskSvg(
            single_mask, canvas_width, canvas_height, mask.mask_type, mask.mask_type != PrivacyMaskRegion::SOLID);
        if (!File::saveFile(mask_svg, mask_path)) {
            WarnL << "Extract video: cannot save polygon privacy mask SVG to " << mask_path;
            continue;
        }
        temporary_paths.push_back(mask_path);

        const std::string next = "poly_pm" + std::to_string(stage++);
        if (graph.tellp() > 0) {
            graph << ";";
        }

        if (mask.mask_type == PrivacyMaskRegion::SOLID) {
            graph << "movie=" << OverlayPrivacyUtils::escapeMoviePath(mask_path)
                  << ",loop=loop=-1:size=1:start=0,scale=" << video_width << ":" << video_height
                  << ":flags=lanczos[" << next << "_mask_full];"
                  << "[" << next << "_mask_full]crop=w=" << crop_width << ":h=" << crop_height
                  << ":x=" << crop_x << ":y=" << crop_y << "[" << next << "_mask];"
                  << "[" << current << "][" << next << "_mask]overlay=x=" << crop_x << ":y=" << crop_y
                  << ":eof_action=repeat:format=auto[" << next << "]";
        } else {
            graph << "[" << current << "]split=2[" << next << "_base][" << next << "_src];";
            graph << "[" << next << "_src]crop=w=" << crop_width << ":h=" << crop_height
                  << ":x=" << crop_x << ":y=" << crop_y << "[" << next << "_crop];";
            if (mask.mask_type == PrivacyMaskRegion::BLUR) {
                graph << "[" << next << "_crop]boxblur=luma_radius=2:luma_power=1:"
                      << "chroma_radius=2:chroma_power=1[" << next << "_processed];";
            } else {
                graph << "[" << next << "_crop]scale=w=\\'max(1,trunc(iw/12))\\':"
                      << "h=\\'max(1,trunc(ih/12))\\':flags=area,"
                      << "scale=w=" << crop_width << ":h=" << crop_height
                      << ":flags=neighbor[" << next << "_processed];";
            }
            graph << "movie=" << OverlayPrivacyUtils::escapeMoviePath(mask_path)
                  << ",loop=loop=-1:size=1:start=0,scale=" << video_width << ":" << video_height
                  << ":flags=neighbor[" << next << "_alpha_full];"
                  << "[" << next << "_alpha_full]crop=w=" << crop_width << ":h=" << crop_height
                  << ":x=" << crop_x << ":y=" << crop_y << "[" << next << "_alpha_src];"
                  << "[" << next << "_processed]format=rgba[" << next << "_rgba];"
                  << "[" << next << "_alpha_src]format=gray[" << next << "_alpha];"
                  << "[" << next << "_rgba][" << next << "_alpha]alphamerge[" << next << "_masked];"
                  << "[" << next << "_base][" << next << "_masked]overlay=x=" << crop_x << ":y=" << crop_y << ":"
                  << "eof_action=repeat:format=auto[" << next << "]";
        }
        current = next;
    }
    last_label = current;
    return graph.str();
}

// Query the camera's current watermark/privacy mask policy (kBroadcastMediaViewOverlay) and,
// if enforced, build the matching ffmpeg filter_complex: privacy masks are burned in with real
// ffmpeg filters (drawbox/boxblur/pixelate), while the watermark (plus an optional source stamp)
// is still rendered to an SVG and overlaid on top. Returns an empty string when no overlay
// should be applied to the extracted clip.
static std::string buildOverlayFilterComplex(const MediaTuple &tuple, const ExtractOptions &extract_options,
                                             const std::string &svg_path, int video_width, int video_height,
                                             std::vector<std::string> &temporary_paths) {
    MediaInfo media_info;
    media_info.vhost = tuple.vhost;
    media_info.app = tuple.app;
    media_info.stream = tuple.stream;

    Broadcast::ViewOverlayPolicy policy;
    NullSockInfo sock_info;
    Broadcast::ViewOverlayPolicyInvoker invoker = [&](const Broadcast::ViewOverlayPolicy &p) { policy = p; };
    NOTICE_EMIT(BroadcastMediaViewOverlayArgs, Broadcast::kBroadcastMediaViewOverlay, media_info, extract_options.jwt_token, invoker, sock_info);

    const bool use_watermark = policy.watermark_enforce && !policy.watermark_excluded;
    const bool use_privacy_mask = policy.privacy_mask_enforce && !policy.privacy_mask_excluded;
    const bool use_source_stamp = extract_options.enable_source_stamp;
    GET_CONFIG(bool, use_watermark_asset, OverlayPrivacyConfig::kUseWatermarkAsset);
    if (!use_watermark && !use_privacy_mask && !use_source_stamp) {
        return "";
    }

    vector<OverlayComponent> components;
    OverlayBuildOptions options;
    options.resolve_dynamic_tokens = true;
    options.username = policy.username;
    options.camera_name = policy.camera_name;
    std::string watermark_path;
    if (use_watermark && use_watermark_asset) {
        std::string watermark_error;
        if (!OverlayPrivacyUtils::resolveWatermarkAsset(policy.watermark_template, options,
                                                        watermark_path, watermark_error)) {
            WarnL << "Extract video: cannot resolve watermark SVG asset for " << tuple.app
                  << ": " << watermark_error;
        }
    } else if (use_watermark) {
        if (!OverlayPrivacyUtils::parseComponents(policy.watermark_template, components, options)) {
            WarnL << "Extract video: watermark template is invalid, skip watermark overlay for " << tuple.app;
            components.clear();
        } else {
            std::string local_image_error;
            if (!OverlayPrivacyUtils::resolveLocalImages(components, local_image_error)) {
                WarnL << "Extract video: cannot resolve watermark image assets for " << tuple.app << ": " << local_image_error;
                components.clear();
            }
        }
    }

    if (use_source_stamp) {
        // Provenance stamp: which camera and which VMS user extracted this clip, independent of
        // any watermark/privacy mask policy the camera itself enforces.
        const std::string camera_name = policy.camera_name.empty() ? tuple.app : policy.camera_name;
        OverlayComponent stamp;
        stamp.type = OverlayComponent::TEXT;
        stamp.id = "source_stamp";
        stamp.text = camera_name + " - Extracted by " + extract_options.username + " - VMS";
        stamp.font_size = 22;
        stamp.color = "#ffffff";
        stamp.opacity = 0.85;
        const double margin = 24.0;
        const double text_width = stamp.text.size() * stamp.font_size * 0.6;
        stamp.x = std::max(margin, options.canvas_width - text_width - margin);
        stamp.y = options.canvas_height - margin;
        components.push_back(stamp);
    }

    vector<PrivacyMaskRegion> privacy_masks;
    if (use_privacy_mask && !OverlayPrivacyUtils::parsePrivacyMasks(policy.privacy_mask_regions, privacy_masks, options)) {
        WarnL << "Extract video: privacy mask configuration is invalid, skip privacy mask overlay for " << tuple.app;
    }
    if (components.empty() && watermark_path.empty() && privacy_masks.empty()) {
        return "";
    }

    std::string last_label;
    std::string filter_complex = buildPolygonPrivacyMaskFilterComplex(
        privacy_masks, options.canvas_width, options.canvas_height,
        video_width, video_height, svg_path, temporary_paths, last_label);

    if (components.empty() && watermark_path.empty()) {
        // Privacy masks only: alias the final processed stream to the [v] output label.
        return filter_complex + (filter_complex.empty() ? "" : ";") + "[" + last_label + "]null[v]";
    }

    if (watermark_path.empty()) {
        // The generated SVG now only carries the watermark; privacy masks are
        // already burned in above.
        auto svg = OverlayPrivacyUtils::buildSvg(components, options);
        if (!File::saveFile(svg, svg_path)) {
            WarnL << "Extract video: cannot save overlay svg to " << svg_path;
            if (filter_complex.empty()) {
                return "";
            }
            return filter_complex + ";[" + last_label + "]null[v]";
        }
        watermark_path = svg_path;
        temporary_paths.push_back(svg_path);
    }

    // Scale the watermark SVG to exactly the video's resolution before overlaying it. The SVG
    // coordinates are defined in the camera's overlay canvas, so the canvas must cover the whole
    // frame; otherwise a different aspect ratio would leave unscaled regions and shift the layout.
    if (video_width <= 0 || video_height <= 0) {
        WarnL << "Extract video: cannot determine source video dimensions, skip SVG watermark";
        return filter_complex + (filter_complex.empty() ? "" : ";") + "[" + last_label + "]null[v]";
    }
    std::ostringstream watermark_stage;
    watermark_stage << "movie=" << OverlayPrivacyUtils::escapeMoviePath(watermark_path)
                    << ",scale=" << video_width << ":" << video_height << ":flags=lanczos[wm];"
                    << "[" << last_label << "][wm]overlay=x=0:y=0:eof_action=repeat:format=auto,"
                    << "format=yuv420p[v]";
    return filter_complex + (filter_complex.empty() ? "" : ";") + watermark_stage.str();
}

} // namespace

void FFmpegExtractor::makeExtract(const string &key, const string &root_path, const onExtract &cb) {
    GET_CONFIG(string, ffmpeg_bin, FFmpeg::kBin);
    GET_CONFIG(string, ffmpeg_extract, FFmpeg::kExtract);
    GET_CONFIG(string, ffmpeg_extract_overlay, FFmpeg::kExtractOverlay);
    GET_CONFIG(string, ffmpeg_log, FFmpeg::kLog);

    uint32_t duration_start = 0;
    uint32_t duration_end = 0;
    _src_path = File::absolutePath(key + ".txt", root_path);
    makeIndexFile(_src_path, _tuple.app, _tuple.stream, _options.start_time, _options.end_time, [&](const SockException &ex, uint32_t &start, uint32_t &end) {
        if (ex) {
            cb(ex);
            return;
        }
        _duration = end - start;
        duration_start = start;
        duration_end = end;
    });
    if (_duration == 0) {
        WarnL << "No data in time period: " << getTimeStr("%Y-%m-%d %H:%M:%S" , _options.start_time) << " - " << getTimeStr("%Y-%m-%d %H:%M:%S" , _options.end_time)
              << ", camera_id: " << _tuple.app << ", stream_id: " << _tuple.stream;
        return;
    }
    auto save_format = getFileExtension(_options.filename);
    _save_path = File::absolutePath(key + "." + save_format, root_path);
    DebugL << "Make video extract of device " << _tuple.app << "/" << _tuple.stream << " duration: " << format_duration_hms(_duration) << "s, save path: " << _save_path;

    auto overlay_svg_path = File::absolutePath(key + ".overlay.svg", root_path);
    int video_width = 0;
    int video_height = 0;
    if (!probeConcatVideoSize(_src_path, video_width, video_height)) {
        WarnL << "Extract video: cannot probe source dimensions from " << _src_path;
    }
    std::vector<std::string> temporary_paths;
    auto filter_complex = buildOverlayFilterComplex(_tuple, _options, overlay_svg_path,
                                                    video_width, video_height, temporary_paths);
    _overlay_temp_paths = temporary_paths;

    // Polygon alpha-mask graphs can exceed the legacy 2 KiB buffer. Truncating
    // the command cuts quoted metadata/output paths and breaks command parsing.
    char cmd[65536] = { 0 };
    int command_length = 0;
    if (!filter_complex.empty()) {
        // Camera has an enforced watermark/privacy mask policy: re-encode the video while
        // burning in the overlay instead of stream-copying it.
        command_length = snprintf(cmd, sizeof(cmd),
            ffmpeg_extract_overlay.data(),
            File::absolutePath("", ffmpeg_bin).data(),
            _src_path.data(),
            format_duration_hms(duration_start).data(),
            format_duration_hms(duration_end).data(),
            filter_complex.data(),
            escape(_options.filename).data(),
            escape(_options.description + " -- By -- " + _options.username).data(),
            escape(getTimeStr("%Y-%m-%d %H:%M:%S", _created_at)).data(),
            escape(kServerShortName).data(),
            _save_path.data());
    } else if (countSubString(ffmpeg_extract, "%s") == 7) {
        // Backward compatibility, if the ffmpeg extract command template does not contain the start time and end time parameters, it will be automatically compatible with the old template
        command_length = snprintf(cmd, sizeof(cmd), ffmpeg_extract.data(), File::absolutePath("", ffmpeg_bin).data(),
            _src_path.data(),
            escape(_options.filename).data(),
            escape(_options.description + " -- By -- " + _options.username).data(),
            escape(getTimeStr("%Y-%m-%d %H:%M:%S", _created_at)).data(),
            escape(kServerShortName).data(),
            _save_path.data());
        
    } else {
        command_length = snprintf(cmd, sizeof(cmd), ffmpeg_extract.data(), File::absolutePath("", ffmpeg_bin).data(),
            _src_path.data(),
            format_duration_hms(duration_start).data(),
            format_duration_hms(duration_end).data(),
            escape(_options.filename).data(),
            escape(_options.description + " -- By -- " + _options.username).data(),
            escape(getTimeStr("%Y-%m-%d %H:%M:%S", _created_at)).data(),
            escape(kServerShortName).data(),
            _save_path.data());
    }
    if (command_length < 0 || static_cast<size_t>(command_length) >= sizeof(cmd)) {
        WarnL << "Extract video: generated FFmpeg command exceeds " << sizeof(cmd) - 1 << " bytes";
        cb(SockException(Err_other, "FFmpeg extract command is too long"));
        return;
    }
    _log_file = ffmpeg_log.empty() ? "" : File::absolutePath("", ffmpeg_log);
    _process.run(cmd, _log_file);
    _cmd = cmd;
    InfoL << cmd;

    // judge whether it is successful by judging whether the FFmpeg process is online
    weak_ptr<FFmpegExtractor> weakSelf = shared_from_this();
    _poller->doDelayTask(_timeout_ms, [weakSelf, cb]() {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            // Self has been destroyed
            return 0;
        }
        // FFmpeg is still online, so we think the extract stream is successful
        if (strongSelf->_process.wait(false)) {
            cb(SockException());
            strongSelf->startTimer();
            return 0;
        }
        // ffmpeg process has exited
        bool success = strongSelf->_process.exit_code() == 0;
        string err_msg;
        if (!success) {
            err_msg = StrPrinter << "ffmpeg has exited, exit code = " << strongSelf->_process.exit_code();
        }
        {
            lock_guard<mutex> lock(strongSelf->_status_mtx);
            strongSelf->_finished = true;
            strongSelf->_success = success;
            strongSelf->_err_msg = err_msg;
            if (success) {
                strongSelf->_progress = 100.0f;
            }
        }
        if (success) {
            strongSelf->emitEvent(true);
            cb(SockException());
        } else {
            strongSelf->emitEvent(false, err_msg);
            cb(SockException(Err_other, err_msg, ApiErrCode::CODE_EXTRACT_FAILED));
        }
        // close after process finished
        strongSelf->closeAfterDelaySec();
        return 0;
    });
}

/**
 * Read ffmpeg log and calculation progress
 */
float trackFFmpegProgress(const std::string &log_path, const float &total_duration) {
    float duration = 0.0f;
    float progress = 0.0f;
    if (!log_path.empty() && File::fileExist(log_path)) {
        auto line = File::loadFile(log_path);
        string pid;
        string pid_key = "pid=";
        size_t start_pos_pid = line.find(pid_key);
        if (start_pos_pid != std::string::npos) {
            start_pos_pid += pid_key.size();
            size_t end_pos_pid = line.find(",", start_pos_pid);
            if (end_pos_pid != std::string::npos) { 
                pid = line.substr(start_pos_pid, end_pos_pid - start_pos_pid);
            }
        }

        string time_key = "time=";
        size_t start_pos_time = line.rfind(time_key);
        if (start_pos_time != std::string::npos) {
            start_pos_time += time_key.size();
            size_t end_pos_time = line.find(" ", start_pos_time);
            if (end_pos_time != std::string::npos) {
                auto time_str = line.substr(start_pos_time, end_pos_time - start_pos_time);
                int hour = stoi(time_str.substr(0, 2));
                int minute = stoi(time_str.substr(3, 2));
                float second = stof(time_str.substr(6));
                duration  = hour * 3600 + minute * 60 + second;
                progress = duration > 0 && total_duration > 0 ? (duration / total_duration) * 100.0f : 0.0f;
                if (progress >= 100.0f) {
                    progress = 99.9f;
                }
            }
        }
        char buf[1024];
        snprintf(buf, sizeof(buf), "FFmpeg (pid:%s) progress: %.1fs/%.1fs (%.1f%%)", pid.data(), duration, total_duration, progress);
        DebugL << buf;
    } else {
        ErrorL << "Open file log " << log_path << " failed: No such file or directory";
    }
    
    return progress;
}

/**
 * Check if the media is online regularly
 */
void FFmpegExtractor::startTimer() {
    uint64_t timeout_ms = _duration * 1000;
    weak_ptr<FFmpegExtractor> weakSelf = shared_from_this();
    _timer = std::make_shared<Timer>(1.0f, [weakSelf, timeout_ms]() {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            // Self has been destroyed
            return false;
        }
        // we judge whether the FFmpeg process is online, if FFmpeg extract clip is interrupted, then it should exit automatically
        if (strongSelf->_process.wait(false)) {
            // The FFmpeg process is still running, close it if it times out
            auto elapsed_ms = strongSelf->_ticker.elapsedTime();
            if (timeout_ms > 0 && elapsed_ms > timeout_ms) {
                strongSelf->_process.kill(2000);
            }
            // find progress bar
            auto progress = trackFFmpegProgress(strongSelf->_log_file, strongSelf->_duration);
            {
                lock_guard<mutex> lock(strongSelf->_status_mtx);
                strongSelf->_progress = progress;
            }
            return true;
        } else {
            // ffmpeg is not online, check output file and set status
            bool success = strongSelf->_process.exit_code() == 0 && File::fileSize(strongSelf->_save_path);
            string err_msg = (!success && !strongSelf->_log_file.empty()) ? File::loadFile(strongSelf->_log_file) : "";
            {
                lock_guard<mutex> lock(strongSelf->_status_mtx);
                strongSelf->_finished = true;
                strongSelf->_success = success;
                if (success) {
                    strongSelf->_progress = 100.0f;
                }
                strongSelf->_err_msg = err_msg;
            }
            strongSelf->emitEvent(success, err_msg);
            // close after process finished
            strongSelf->closeAfterDelaySec();
            return false;
        }
    }, _poller);
}

FFmpegExtractor::Status FFmpegExtractor::status() const {
    lock_guard<mutex> lock(_status_mtx);
    Status ret;
    ret.progress = _progress;
    ret.finished = _finished;
    ret.success = _success;
    ret.err_msg = _err_msg;
    return ret;
}

void FFmpegExtractor::setOnClose(const function<void()> &cb){
    _onClose = cb;
}

void FFmpegExtractor::closeAfterDelaySec() {
    GET_CONFIG(uint64_t, delay_close_sec, FFmpeg::kDelayCloseSec);
    weak_ptr<FFmpegExtractor> weakSelf = shared_from_this();
    _poller->doDelayTask((uint64_t)(delay_close_sec * 1000), [weakSelf]() {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            // Self has been destroyed
            return 0;
        }
        strongSelf->close();
        return 0;
    });
}

bool FFmpegExtractor::close() {
    if (_onClose) {
        _onClose();
    }
    File::delete_file(_src_path);
    File::delete_file(_save_path);
    for (size_t i = 0; i < _overlay_temp_paths.size(); ++i) {
        File::delete_file(_overlay_temp_paths[i]);
    }
    return true;
}

void FFmpegExtractor::emitEvent(bool success, const string &err_msg) {
    auto tuple = _tuple;
    auto options = _options;
    auto duration = _duration;
    auto save_path = _save_path;
    auto session = _session;
    auto created_at = _created_at;

    WorkThreadPool::Instance().getPoller()->async([tuple, options, duration, save_path, session, created_at, success, err_msg]() {
        ExtractAuditLogArgs log;
        log.camera_id = tuple.app;
        log.stream_id = tuple.stream;
        {
            auto recorder = StatisticRecorder::Instance().getRecorder(tuple.app, false);
            if (recorder) {
                auto params = recorder->getParams();
                log.camera_name = params.option.name;
                for (const auto &it : params.stream_map) {
                    if (it.second.stream_id == tuple.stream) {
                        log.stream_profile = getStreamTypeString(it.first);
                        break;
                    }
                }
            }
        }
        log.start_time = options.start_time;
        log.end_time = options.end_time;
        log.duration = duration;
        log.file_size = File::fileSize(save_path);
        log.action_created_at = created_at;
        log.action_duration = time(nullptr) - created_at;
        log.filename = options.filename;
        log.user_id = options.user_id;
        log.user_name = options.username;
        log.is_success = success;
        log.message = success ? "Success" : err_msg;

        NOTICE_EMIT(BroadcastUserAuditLogArgs, Broadcast::kBroadcastUserAuditLog, UserAuditLogType::EXTRACT_VIDEO, tuple.app, log.toJson(), session);
    });
}

static bool parse_probe_log(ProbeInfo &info, const string &log_string) {
    if (log_string.empty()) {
        return false;
    }

    // find json string
    size_t start_point = log_string.find('{');
    size_t end_point = log_string.rfind('}');

    if (start_point == std::string::npos || end_point == std::string::npos || end_point < start_point) {
        DebugL << "Can not find stream information.";
        return false;
    }

    auto json_str = log_string.substr(start_point, end_point - start_point + 1);

    Json::Value data;
    if (!StrJsonUtils::readJsonString(json_str, data)) {
        return false;
    }
    // get stream information from json var
    DebugL << data.toStyledString();
    if (!data.isMember("streams") || !data["streams"].isArray()) {
        DebugL << "Can not find stream information..";
        return false;
    }

    for (const auto &stream : data["streams"]) {
        if (!stream.isMember("codec_type")) {
            continue;
        }
        if (stream["codec_type"] == "video") {
            info.hasVideo = true;
            if (stream.isMember("codec_name")) {
                info.vcodec = stream["codec_name"].asString();
            }
            if (stream.isMember("width")) {
                info.width = std::stoi(stream["width"].asString());
            }
            if (stream.isMember("height")) {
                info.height = std::stoi(stream["height"].asString());
            }
            if (stream.isMember("avg_frame_rate")) {
                auto fr = stream["avg_frame_rate"].asString();
                int num, den;
                if (sscanf(fr.c_str(), "%d/%d", &num, &den) == 2 && den != 0)
                    info.fps = float(num) / float(den);
            }
            if (stream.isMember("bit_rate")) {
                info.bitrate = std::stoi(stream["bit_rate"].asString());
            }
            if (stream.isMember("avg_quality")) {
                info.quality = std::stof(stream["avg_quality"].asString());
            }
        }

        if (stream["codec_type"] == "audio") {
            // todo: parse audio codec
        }
    }
    return true;
}

void FFmpegProbe::makeProbe(const string &play_url, float timeout_sec, const onProbe &cb, const toolkit::EventPoller::Ptr &poller) {
    GET_CONFIG(string, ffprobe_bin, FFmpeg::kBinP);
    GET_CONFIG(string, ffmpeg_probe, FFmpeg::kProbe);
    GET_CONFIG(string, ffmpeg_log, FFmpeg::kLog);
    Ticker ticker;
    auto _poller = poller ? poller : WorkThreadPool::Instance().getPoller();
    _poller->async([timeout_sec, play_url, cb, ticker]() {
        ProbeInfo info;
        info.url = play_url;
        auto elapsed_ms = ticker.elapsedTime();
        if (elapsed_ms > timeout_sec * 1000) {
            // Timeout, the background thread load is too high, it takes too long to start this task
            cb(false, "wait work poller schedule probe task timeout", info);
            return;
        }
        char cmd[2048] = { 0 };
        snprintf(cmd, sizeof(cmd), ffmpeg_probe.data(), File::absolutePath("", ffprobe_bin).data(), escape(play_url).data());
        std::shared_ptr<Process> process = std::make_shared<Process>();
        auto log_file = ffmpeg_log.empty() ? ffmpeg_log : File::absolutePath("", ffmpeg_log);
        process->run(cmd, log_file);

        // The timer delay should be reduced by the delay of the background task startup
        auto delayTask = EventPollerPool::Instance().getPoller()->doDelayTask(
            (uint64_t)(timeout_sec * 1000 - elapsed_ms), [process, cb, log_file]() {
                if (process->wait(false)) {
                    // The FFmpeg process is still running, close it if it times out
                    process->kill(2000);
                }
                return 0;
            });

        // Wait for the FFmpeg process to exit
        process->wait(true);
        // The FFmpeg process has exited, the timer can be canceled
        delayTask->cancel();
        // Execute the callback function
        string log_string = File::loadFile(log_file);
        bool success = process->exit_code() == 0 && parse_probe_log(info, log_string);
        cb(success, (!success && !log_file.empty()) ? File::loadFile(log_file) : "", info);
    });
}
