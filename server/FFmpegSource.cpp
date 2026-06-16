#include "FFmpegSource.h"
#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Common/MultiMediaSourceMuxer.h"
#include "Util/base64.h"
#include "Util/File.h"
#include "System.h"
#include "Thread/WorkThreadPool.h"
#include "Network/sockutil.h"
#include "Local/TimeQuery.h"
#include <iomanip>
#include "Common/StrUtil.h"
#include "User/UserAuditLog.h"
#include "Local/StatisticRecorder.h"

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
        if (countSubString(ffmpeg_snap, "%s") == 3 || seek_time == 0) {
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
        const function<void(const string &err, uint32_t &duration_start, uint32_t &duration_end)> &cb) {
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
        return cb(err, duration_start, duration_end);
    }

    MediaTuple tuple = { DEFAULT_VHOST, camera_id, stream_id, "" };
    auto query = std::make_shared<TimeQuery>(tuple);
    query->getRecordedTimePeriod(start_time, end_time, [&duration_start, &duration_end, start_time, end_time, file_ptr](const vector<TimeBlock> &ret) {
        for (const auto &block : ret) {
            uint64_t start_pos = block.start_time();
            uint64_t end_pos = block.start_time() + block.time_len();
            if (start_pos < start_time) {
                duration_start += start_time - start_pos;
                duration_end += duration_start;
                start_pos = start_time;
            }
            if (end_pos > end_time) {
                end_pos = end_time;
            }
            duration_end += end_pos - start_pos;
            auto line = "file '" + decodeBase64(block.file_path()) + "'\n";
            fwrite(line.c_str(), line.size(), 1, file_ptr.get());
        }
    });

    return cb((duration_end - duration_start) == 0 ? "No data in time period" : "", duration_start, duration_end);
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

void FFmpegExtractor::makeExtract(const string &key, const string &root_path, const onExtract &cb) {
    GET_CONFIG(string, ffmpeg_bin, FFmpeg::kBin);
    GET_CONFIG(string, ffmpeg_extract, FFmpeg::kExtract);
    GET_CONFIG(string, ffmpeg_log, FFmpeg::kLog);

    uint32_t duration_start = 0;
    uint32_t duration_end = 0;
    _src_path = File::absolutePath(key + ".txt", root_path);
    makeIndexFile(_src_path, _tuple.app, _tuple.stream, _options.start_time, _options.end_time, [&](const string &err, uint32_t &start, uint32_t &end) {
        if (!err.empty()) {
            cb(SockException(Err_other, err));
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

    char cmd[2048] = { 0 };
    if (countSubString(ffmpeg_extract, "%s") == 7) {
        // Backward compatibility, if the ffmpeg extract command template does not contain the start time and end time parameters, it will be automatically compatible with the old template
        snprintf(cmd, sizeof(cmd), ffmpeg_extract.data(), File::absolutePath("", ffmpeg_bin).data(), 
            _src_path.data(),
            escape(_options.filename).data(),
            escape(_options.description + " -- By -- " + _options.username).data(),
            escape(getTimeStr("%Y-%m-%d %H:%M:%S", _created_at)).data(),
            escape(kServerShortName).data(),
            _save_path.data());
        
    } else {
        snprintf(cmd, sizeof(cmd), ffmpeg_extract.data(), File::absolutePath("", ffmpeg_bin).data(), 
            _src_path.data(),
            format_duration_hms(duration_start).data(),
            format_duration_hms(duration_end).data(),
            escape(_options.filename).data(),
            escape(_options.description + " -- By -- " + _options.username).data(),
            escape(getTimeStr("%Y-%m-%d %H:%M:%S", _created_at)).data(),
            escape(kServerShortName).data(),
            _save_path.data());
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
        strongSelf->_finished = true;
        strongSelf->_success = strongSelf->_process.exit_code() == 0;
        if (strongSelf->_success) {
            strongSelf->_progress = 100.0f;
            strongSelf->emitEvent(true);
            cb(SockException());
        } else {
            string err_msg = StrPrinter << "ffmpeg has exited, exit code = " << strongSelf->_process.exit_code();
            strongSelf->emitEvent(false, err_msg);
            cb(SockException(Err_other, err_msg));            
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
    _timer = std::make_shared<Timer>(1.0f, [weakSelf, &timeout_ms]() {
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
            strongSelf->_progress = trackFFmpegProgress(strongSelf->_log_file, strongSelf->_duration);
            return true;
        } else {
            // ffmpeg is not online, check output file and set status
            strongSelf->_finished = true;
            bool success = strongSelf->_process.exit_code() == 0 && File::fileSize(strongSelf->_save_path);
            strongSelf->_success = success;
            strongSelf->_progress = success ? 100.0f : strongSelf->_progress;
            strongSelf->_err_msg = (!success && !strongSelf->_log_file.empty()) ? File::loadFile(strongSelf->_log_file) : "";
            strongSelf->emitEvent(success, strongSelf->_err_msg);
            // close after process finished
            strongSelf->closeAfterDelaySec();
            return false;
        }
    }, _poller);
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