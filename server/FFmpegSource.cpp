#include "FFmpegSource.h"
#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Common/MultiMediaSourceMuxer.h"
#include "Util/File.h"
#include "System.h"
#include "Thread/WorkThreadPool.h"
#include "Network/sockutil.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace FFmpeg {
#define FFmpeg_FIELD "ffmpeg."
const string kBin = FFmpeg_FIELD"bin";
const string kCmd = FFmpeg_FIELD"cmd";
const string kLog = FFmpeg_FIELD"log";
const string kSnap = FFmpeg_FIELD"snap";
const string kRestartSec = FFmpeg_FIELD"restart_sec";

onceToken token([]() {
#ifdef _WIN32
    string ffmpeg_bin = trim(System::execute("where ffmpeg"));
#else
    string ffmpeg_bin = trim(System::execute("which ffmpeg"));
#endif
    // Default ffmpeg command path is the path in the environment variable
    mINI::Instance()[kBin] = ffmpeg_bin.empty() ? "ffmpeg" : ffmpeg_bin;
    // ffmpeg log save path
    mINI::Instance()[kLog] = "./ffmpeg/ffmpeg.log";
    mINI::Instance()[kCmd] = "%s -re -i %s -c:a aac -strict -2 -ar 44100 -ab 48k -c:v libx264 -f flv %s";
    mINI::Instance()[kSnap] = "%s -i %s -y -f mjpeg -frames:v 1 -an %s";
    mINI::Instance()[kRestartSec] = 0;
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
    } catch (std::exception &ex) {
        cb(SockException(Err_other, ex.what()));
        return;
    }

    auto ffmpeg_cmd = ffmpeg_cmd_default;
    if (!ffmpeg_cmd_key.empty()) {
        auto cmd_it = mINI::Instance().find(ffmpeg_cmd_key);
        if (cmd_it != mINI::Instance().end()) {
            ffmpeg_cmd = cmd_it->second;
        } else {
            WarnL << "In the configuration file, ffmpeg command template (" << ffmpeg_cmd_key << ") does not exist, the default template has been adopted(" << ffmpeg_cmd_default << ")";
        }
    }

    char cmd[2048] = { 0 };
    snprintf(cmd, sizeof(cmd), ffmpeg_cmd.data(), File::absolutePath("", ffmpeg_bin).data(), src_url.data(), dst_url.data());
    auto log_file = ffmpeg_log.empty() ? "" : File::absolutePath("", ffmpeg_log);
    _process.run(cmd, log_file);
    _cmd = cmd;
    InfoL << cmd;

    if (is_local_ip(_media_info.host)) {
        // Push stream to yourself, judge whether the stream is registered to determine whether it is normal
        if (_media_info.schema != RTSP_SCHEMA && _media_info.schema != RTMP_SCHEMA) {
            cb(SockException(Err_other, "This service only supports rtmp/rtsp push streaming"));
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
            cb(SockException(Err_other, "等待超时"));
        });
    } else{
        // Push stream to other servers, judge whether it is successful by judging whether the FFmpeg process is online
        weak_ptr<FFmpegSource> weakSelf = shared_from_this();
        _timer = std::make_shared<Timer>(timeout_ms / 1000.0f, [weakSelf, cb, timeout_ms]() {
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
        }, _poller);
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
            src->setupRecord(Recorder::type_hls, true, "", 0);
        }
        if (_enable_mp4) {
            src->setupRecord(Recorder::type_mp4, true, "", 0);
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

void FFmpegSnap::makeSnap(bool async, const string &play_url, const string &save_path, float timeout_sec, const onSnap &cb) {
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
    WorkThreadPool::Instance().getPoller()->async([timeout_sec, play_url, save_path, cb, ticker]() {
        auto elapsed_ms = ticker.elapsedTime();
        if (elapsed_ms > timeout_sec * 1000) {
            // Timeout, the background thread load is too high, it takes too long to start this task
            cb(false, "wait work poller schedule snap task timeout");
            return;
        }
        char cmd[2048] = { 0 };
        snprintf(cmd, sizeof(cmd), ffmpeg_snap.data(), File::absolutePath("", ffmpeg_bin).data(), play_url.data(), save_path.data());

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
