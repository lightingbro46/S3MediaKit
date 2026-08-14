#if defined(ENABLE_FFMPEG)
#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include "Util/File.h"
#include "Util/uv_errno.h"
#include "Transcode.h"
#include "Common/config.h"
#include "Extension/Factory.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#define MAX_DELAY_SECOND 3

using namespace std;
using namespace toolkit;

namespace toolkit {
    StatisticImp(mediakit::FFmpegDecoder)
    StatisticImp(mediakit::FFmpegEncoder)
}

namespace mediakit {

int getDefaultTranscodeBitrate(CodecId codec, int width, int height, int fps) {
    const int target_width = width > 0 ? width : 1280;
    const int target_height = height > 0 ? height : 720;
    const int target_fps = fps > 0 ? fps : 5;
    const int64_t pixels = static_cast<int64_t>(target_width) * target_height;
    int64_t bitrate = 2500000LL * pixels / (1280LL * 720) * target_fps / 25;
    if (codec == CodecH265) {
        bitrate = bitrate * 65 / 100;
    }
    if (bitrate < 512000) {
        bitrate = 512000;
    } else if (bitrate > 8000000) {
        bitrate = 8000000;
    }
    return static_cast<int>(((bitrate + 63999) / 64000) * 64000);
}

void getTranscodeOutputSize(int source_width, int source_height,
                            int requested_width, int requested_height,
                            int &output_width, int &output_height) {
    source_width = std::max(2, source_width);
    source_height = std::max(2, source_height);
    output_width = requested_width > 0 ? requested_width : source_width;
    output_height = requested_height > 0 ? requested_height : source_height;

    if (requested_width > 0 && requested_height == 0) {
        output_height = std::max(2, static_cast<int>(output_width * source_height / static_cast<double>(source_width)));
    } else if (requested_width == 0 && requested_height > 0) {
        output_width = std::max(2, static_cast<int>(output_height * source_width / static_cast<double>(source_height)));
    }

    const double scale = std::min(1.0, std::min(1920.0 / output_width, 1080.0 / output_height));
    output_width = std::max(2, static_cast<int>(output_width * scale) & ~1);
    output_height = std::max(2, static_cast<int>(output_height * scale) & ~1);
}

static string ffmpeg_err(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return errbuf;
}

std::unique_ptr<AVPacket, void (*)(AVPacket *)> alloc_av_packet() {
    return std::unique_ptr<AVPacket, void (*)(AVPacket *)>(av_packet_alloc(), [](AVPacket *pkt) { av_packet_free(&pkt); });
}

//////////////////////////////////////////////////////////////////////////////////////////
static void on_ffmpeg_log(void *ctx, int level, const char *fmt, va_list args) {
    GET_CONFIG(bool, enable_ffmpeg_log, General::kEnableFFmpegLog);
    if (!enable_ffmpeg_log) {
        return;
    }
    LogLevel lev;
    switch (level) {
        case AV_LOG_FATAL: lev = LError; break;
        case AV_LOG_ERROR: lev = LError; break;
        case AV_LOG_WARNING: lev = LWarn; break;
        case AV_LOG_INFO: lev = LInfo; break;
        case AV_LOG_VERBOSE: lev = LDebug; break;
        case AV_LOG_DEBUG: lev = LDebug; break;
        case AV_LOG_TRACE: lev = LTrace; break;
        default: lev = LTrace; break;
    }
    LoggerWrapper::printLogV(::toolkit::getLogger(), lev, __FILE__, ctx ? av_default_item_name(ctx) : "NULL", level, fmt, args);
}

static bool setupFFmpeg_l() {
    av_log_set_level(AV_LOG_TRACE);
    av_log_set_flags(AV_LOG_PRINT_LEVEL);
    av_log_set_callback(on_ffmpeg_log);
#if (LIBAVCODEC_VERSION_MAJOR < 58)
    avcodec_register_all();
#endif
    return true;
}

static void setupFFmpeg() {
    static auto flag = setupFFmpeg_l();
}

static bool checkIfSupportedNvidia_l() {
#if !defined(_WIN32)
    GET_CONFIG(bool, check_nvidia_dev, General::kCheckNvidiaDev);
    if (!check_nvidia_dev) {
        return false;
    }
    auto so = dlopen("libnvcuvid.so.1", RTLD_LAZY);
    if (!so) {
        WarnL << "libnvcuvid.so.1 failed to load:" << get_uv_errmsg();
        return false;
    }
    dlclose(so);

    bool find_driver = false;
    File::scanDir("/dev", [&](const string &path, bool is_dir) {
        if (!is_dir && start_with(path, "/dev/nvidia")) {
            // Find the Nvidia driver
            find_driver = true;
            return false;
        }
        return true;
    }, false);

    if (!find_driver) {
        WarnL << "Nvidia hardware codec driver file /dev/nvidia*does not exist";
    }
    return find_driver;
#else
    return false;
#endif
}

static bool checkIfSupportedNvidia() {
    static auto ret = checkIfSupportedNvidia_l();
    return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////

bool TaskManager::addEncodeTask(function<void()> task) {
    {
        lock_guard<mutex> lck(_task_mtx);
        _task.emplace_back(std::move(task));
        if (_task.size() > _max_task) {
            WarnL << "encoder thread task is too more, now drop frame!";
            _task.pop_front();
        }
    }
    _sem.post();
    return true;
}

bool TaskManager::addDecodeTask(bool key_frame, function<void()> task) {
    {
        lock_guard<mutex> lck(_task_mtx);
        if (_decode_drop_start) {
            if (!key_frame) {
                TraceL << "decode thread drop frame";
                return false;
            }
            _decode_drop_start = false;
            InfoL << "decode thread stop drop frame";
        }

        _task.emplace_back(std::move(task));
        if (_task.size() > _max_task) {
            _decode_drop_start = true;
            WarnL << "decode thread start drop frame";
        }
    }
    _sem.post();
    return true;
}

void TaskManager::setMaxTaskSize(size_t size) {
    CHECK(size >= 3 && size <= 1000, "async task size limited to 3 ~ 1000, now size is:", size);
    _max_task = size;
}

void TaskManager::startThread(const string &name) {
    _thread.reset(new thread([this, name]() {
        onThreadRun(name);
    }), [](thread *ptr) {
        if (ptr->joinable()) {
            ptr->join();
        }
        delete ptr;
    });
}

void TaskManager::stopThread(bool drop_task) {
    TimeTicker();
    if (!_thread) {
        return;
    }
    {
        lock_guard<mutex> lck(_task_mtx);
        if (drop_task) {
            _exit = true;
            _task.clear();
        }
        _task.emplace_back([]() {
            throw ThreadExitException();
        });
    }
    _sem.post(10);
    _thread = nullptr;
}

TaskManager::~TaskManager() {
    stopThread(true);
}

bool TaskManager::isEnabled() const {
    return _thread.operator bool();
}

void TaskManager::onThreadRun(const string &name) {
    setThreadName(name.data());
    function<void()> task;
    _exit = false;
    while (!_exit) {
        _sem.wait();
        {
            unique_lock<mutex> lck(_task_mtx);
            if (_task.empty()) {
                continue;
            }
            task = _task.front();
            _task.pop_front();
        }

        try {
            TimeTicker2(50, TraceL);
            task();
            task = nullptr;
        } catch (ThreadExitException &ex) {
            break;
        } catch (std::exception &ex) {
            WarnL << ex.what();
            continue;
        } catch (...) {
            WarnL << "catch one unknown exception";
            throw;
        }
    }
    InfoL << name << " exited!";
}

//////////////////////////////////////////////////////////////////////////////////////////

FFmpegFrame::FFmpegFrame(std::shared_ptr<AVFrame> frame) {
    if (frame) {
        _frame = std::move(frame);
    } else {
        _frame.reset(av_frame_alloc(), [](AVFrame *ptr) {
            av_frame_free(&ptr);
        });
    }
}

FFmpegFrame::~FFmpegFrame() {
}

AVFrame *FFmpegFrame::get() const {
    return _frame.get();
}

void FFmpegFrame::fillPicture(AVPixelFormat target_format, int target_width, int target_height) {
    auto buffer_size = av_image_get_buffer_size(target_format, target_width, target_height, 32);
    _data = std::unique_ptr<char[]>(new char[buffer_size]);
    av_image_fill_arrays(_frame->data, _frame->linesize, (uint8_t *)_data.get(), target_format, target_width, target_height, 32);
}

int FFmpegFrame::getChannels() const {
    if (!_frame) return 0;
#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
    return _frame->ch_layout.nb_channels;
#else
    return _frame->channels;
#endif
}

// Called before resource pool reuse
void FFmpegFrame::reset() {
    _data.reset();
    if (_frame) {
        av_frame_unref(_frame.get()); // Clean up AVFrame data references
    }
}

FFmpegFrame::Ptr FFmpegFrame::clone() const {
    auto new_frame = std::make_shared<FFmpegFrame>();
    if (_frame) {
        new_frame->get()->format = _frame->format;
        new_frame->get()->width = _frame->width;
        new_frame->get()->height = _frame->height;
        auto ret = av_frame_get_buffer(new_frame->get(), 32); // 32-byte alignment
        if (ret < 0) {
            WarnL << "av_frame_get_buffer failed: " << ffmpeg_err(ret);
            return nullptr;
        }

        ret = av_frame_copy_props(new_frame->get(), _frame.get());
        if (ret < 0) {
            WarnL << "av_frame_copy_properties failed: " << ffmpeg_err(ret);
            return nullptr;
        }

        ret = av_frame_copy(new_frame->get(), _frame.get());
        if (ret < 0) {
            WarnL << "av_frame_copy failed: " << ffmpeg_err(ret);
            return nullptr;
        }
    }
    return new_frame;
}

///////////////////////////////////////////////////////////////////////////

template<bool decoder = true>
static inline const AVCodec *getCodec_l(const char *name) {
    auto codec = decoder ? avcodec_find_decoder_by_name(name) : avcodec_find_encoder_by_name(name);
    if (codec) {
        InfoL << (decoder ? "got decoder:" : "got encoder:") << name;
    } else {
        TraceL << (decoder ? "decoder:" : "encoder:") << name << " not found";
    }
    return codec;
}

template<bool decoder = true>
static inline const AVCodec *getCodec_l(enum AVCodecID id) {
    auto codec = decoder ? avcodec_find_decoder(id) : avcodec_find_encoder(id);
    if (codec) {
        InfoL << (decoder ? "got decoder:" : "got encoder:") << avcodec_get_name(id);
    } else {
        TraceL << (decoder ? "decoder:" : "encoder:") << avcodec_get_name(id) << " not found";
    }
    return codec;
}

class CodecName {
public:
    CodecName(string name) : _codec_name(std::move(name)) {}
    CodecName(enum AVCodecID id) : _id(id) {}

    template <bool decoder>
    const AVCodec *getCodec() const {
        if (!_codec_name.empty()) {
            return getCodec_l<decoder>(_codec_name.data());
        }
        return getCodec_l<decoder>(_id);
    }

private:
    string _codec_name;
    enum AVCodecID _id;
};

template <bool decoder = true>
static inline const AVCodec *getCodec(const std::initializer_list<CodecName> &codec_list) {
    const AVCodec *ret = nullptr;
    for (int i = codec_list.size(); i >= 1; --i) {
        ret = codec_list.begin()[i - 1].getCodec<decoder>();
        if (ret) {
            return ret;
        }
    }
    return ret;
}

template<bool decoder = true>
static inline const AVCodec *getCodecByName(const std::vector<std::string> &codec_list) {
    const AVCodec *ret = nullptr;
    for (auto &codec : codec_list) {
        ret = getCodec_l<decoder>(codec.data());
        if (ret) {
            return ret;
        }
    }
    return ret;
}

FFmpegDecoder::FFmpegDecoder(const Track::Ptr &track, int thread_num, const std::vector<std::string> &codec_name) {
    setupFFmpeg();
    _frame_pool.setSize(AV_NUM_DATA_POINTERS);
    const AVCodec *codec = nullptr;
    const AVCodec *codec_default = nullptr;
    if (!codec_name.empty()) {
        codec = getCodecByName(codec_name);
    }
    switch (track->getCodecId()) {
        case CodecH264:
            codec_default = getCodec({AV_CODEC_ID_H264});
            if (codec && codec->id == AV_CODEC_ID_H264) {
                break;
            }
            if (checkIfSupportedNvidia()) {
                codec = getCodec({{"libopenh264"}, {AV_CODEC_ID_H264}, {"h264_qsv"}, {"h264_videotoolbox"}, {"h264_cuvid"}, {"h264_nvmpi"}});
            } else {
                codec = getCodec({{"libopenh264"}, {AV_CODEC_ID_H264}, {"h264_qsv"}, {"h264_videotoolbox"}, {"h264_nvmpi"}});
            }
            break;
        case CodecH265:
            codec_default = getCodec({AV_CODEC_ID_HEVC});
            if (codec && codec->id == AV_CODEC_ID_HEVC) {
                break;
            }
            if (checkIfSupportedNvidia()) {
                codec = getCodec({{AV_CODEC_ID_HEVC}, {"hevc_qsv"}, {"hevc_videotoolbox"}, {"hevc_cuvid"}, {"hevc_nvmpi"}});
            } else {
                // codec = getCodec({{AV_CODEC_ID_HEVC}, {"hevc_qsv"}, {"hevc_videotoolbox"}, {"hevc_nvmpi"}});
                codec = getCodec({{AV_CODEC_ID_HEVC}});
            }
            break;
        case CodecAAC:
            if (codec && codec->id == AV_CODEC_ID_AAC) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_AAC});
            break;
        case CodecG711A:
            if (codec && codec->id == AV_CODEC_ID_PCM_ALAW) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_PCM_ALAW});
            break;
        case CodecG711U:
            if (codec && codec->id == AV_CODEC_ID_PCM_MULAW) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_PCM_MULAW});
            break;
        case CodecOpus:
            if (codec && codec->id == AV_CODEC_ID_OPUS) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_OPUS});
            break;
        case CodecJPEG:
            if (codec && codec->id == AV_CODEC_ID_MJPEG) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_MJPEG});
            break;
        case CodecVP8:
            if (codec && codec->id == AV_CODEC_ID_VP8) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_VP8});
            break;
        case CodecVP9:
            if (codec && codec->id == AV_CODEC_ID_VP9) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_VP9});
            break;
        default: codec = nullptr; break;
    }

    codec = codec ? codec : codec_default;
    if (!codec) {
        throw std::runtime_error("No decoder found");
    }

    while (true) {
        _context.reset(avcodec_alloc_context3(codec), [](AVCodecContext *ctx) {
            avcodec_free_context(&ctx);
        });

        if (!_context) {
            throw std::runtime_error("Failed to create a decoder");
        }

        // Save the AVFrame reference
#ifdef FF_API_OLD_ENCDEC
        _context->refcounted_frames = 1;
#endif
        _context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        _context->flags2 |= AV_CODEC_FLAG2_FAST;
        if (track->getTrackType() == TrackVideo) {
            _context->width = static_pointer_cast<VideoTrack>(track)->getVideoWidth();
            _context->height = static_pointer_cast<VideoTrack>(track)->getVideoHeight();
            InfoL << "media source :" << _context->width << " X " << _context->height;
        }

        switch (track->getCodecId()) {
            case CodecG711A:
            case CodecG711U: {
                AudioTrack::Ptr audio = static_pointer_cast<AudioTrack>(track);

#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
                av_channel_layout_default(&_context->ch_layout, audio->getAudioChannel());
#else
                _context->channels = audio->getAudioChannel();
                _context->channel_layout = av_get_default_channel_layout(_context->channels);
#endif

                _context->sample_rate = audio->getAudioSampleRate();
                break;
            }
            default:
                break;
        }
        AVDictionary *dict = nullptr;
        if (thread_num <= 0) {
            av_dict_set(&dict, "threads", "auto", 0);
        } else {
            av_dict_set(&dict, "threads", to_string(MIN((unsigned int)thread_num, thread::hardware_concurrency())).data(), 0);
        }
        av_dict_set(&dict, "zerolatency", "1", 0);
        av_dict_set(&dict, "strict", "-2", 0);

#ifdef AV_CODEC_CAP_TRUNCATED
        if (codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
            /* we do not send complete frames */
            _context->flags |= AV_CODEC_FLAG_TRUNCATED;
            _do_merger = false;
        } else {
            // The business layer should need to merge frames at this time
            _do_merger = true;
        }
#endif

        int ret = avcodec_open2(_context.get(), codec, &dict);
        av_dict_free(&dict);
        if (ret >= 0) {
            // Success
            InfoL << "Open the decoder successfully:" << codec->name;
            break;
        }

        if (codec_default && codec_default != codec) {
            // Hardware codec failed to open, try software codec
            WarnL << "Turn on the decoder" << codec->name << "failed because:" << ffmpeg_err(ret) << ", try opening the decoder again" << codec_default->name;
            codec = codec_default;
            continue;
        }
        throw std::runtime_error(StrPrinter << "Turn on the decoder" << codec->name << "fail:" << ffmpeg_err(ret));
    }
}

FFmpegDecoder::~FFmpegDecoder() {
    stopThread(true);
    if (_do_merger) {
        _merger.flush();
    }
    flush();
}

void FFmpegDecoder::flush() {
    while (true) {
        auto out_frame = _frame_pool.obtain2();
        auto ret = avcodec_receive_frame(_context.get(), out_frame->get());
        if (ret == AVERROR(EAGAIN)) {
            avcodec_send_packet(_context.get(), nullptr);
            continue;
        }
        if (ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            WarnL << "avcodec_receive_frame failed:" << ffmpeg_err(ret);
            break;
        }
        onDecode(out_frame);
    }
}

const AVCodecContext *FFmpegDecoder::getContext() const {
    return _context.get();
}

bool FFmpegDecoder::inputFrame_l(const Frame::Ptr &frame, bool live, bool enable_merge) {
    if (_do_merger && enable_merge) {
        return _merger.inputFrame(frame, [this, live](uint64_t dts, uint64_t pts, const Buffer::Ptr &buffer, bool have_idr) {
            decodeFrame(buffer->data(), buffer->size(), dts, pts, live, have_idr);
        });
    }

    return decodeFrame(frame->data(), frame->size(), frame->dts(), frame->pts(), live, frame->keyFrame());
}

bool FFmpegDecoder::inputFrame(const Frame::Ptr &frame, bool live, bool async, bool enable_merge) {
    if (async && !TaskManager::isEnabled() && getContext()->codec_type == AVMEDIA_TYPE_VIDEO) {
        // Enable asynchronous encoding, and it is video, try to start asynchronous decoding thread
        startThread("decoder thread");
    }

    if (!async || !TaskManager::isEnabled()) {
        return inputFrame_l(frame, live, enable_merge);
    }

    auto frame_cache = Frame::getCacheAbleFrame(frame);
    return addDecodeTask(frame->keyFrame(), [this, live, frame_cache, enable_merge]() {
        inputFrame_l(frame_cache, live, enable_merge);
        // Here simulates decoding too slow, resulting in active frame dropping
        // usleep(100 * 1000);
    });
}

bool FFmpegDecoder::decodeFrame(const char *data, size_t size, uint64_t dts, uint64_t pts, bool live, bool key_frame) {
    TimeTicker2(30, TraceL);

    auto pkt = alloc_av_packet();
    pkt->data = (uint8_t *)data;
    pkt->size = size;
    pkt->dts = dts;
    pkt->pts = pts;
    if (key_frame) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }

    auto ret = avcodec_send_packet(_context.get(), pkt.get());
    if (ret < 0) {
        if (ret != AVERROR_INVALIDDATA) {
            WarnL << "avcodec_send_packet failed:" << ffmpeg_err(ret);
        }
        return false;
    }

    for (;;) {
        auto out_frame = _frame_pool.obtain2();
        ret = avcodec_receive_frame(_context.get(), out_frame->get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            WarnL << "avcodec_receive_frame failed:" << ffmpeg_err(ret);
            break;
        }
        if (live && pts - out_frame->get()->pts > MAX_DELAY_SECOND * 1000 && _ticker.createdTime() > 10 * 1000) {
            // The following frames are ignored to prevent the Track from being ready
            WarnL << "When decoding, ignore" << MAX_DELAY_SECOND << " data from seconds ago:" << pts << " " << out_frame->get()->pts;
            continue;
        }
        onDecode(out_frame);
    }
    return true;
}

void FFmpegDecoder::setOnDecode(FFmpegDecoder::onDec cb) {
    _cb = std::move(cb);
}

void FFmpegDecoder::onDecode(const FFmpegFrame::Ptr &frame) {
    if (_cb) {
        _cb(frame);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////
#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
FFmpegSwr::FFmpegSwr(AVSampleFormat output, AVChannelLayout *ch_layout, int samplerate) {
    _target_format = output;
    av_channel_layout_copy(&_target_ch_layout, ch_layout);
    _target_samplerate = samplerate;
}
#else
FFmpegSwr::FFmpegSwr(AVSampleFormat output, int channel, int channel_layout, int samplerate) {
    _target_format = output;
    _target_channels = channel;
    _target_channel_layout = channel_layout;
    _target_samplerate = samplerate;

    _swr_frame_pool.setSize(AV_NUM_DATA_POINTERS);
}
#endif

FFmpegSwr::~FFmpegSwr() {
    if (_ctx) {
        swr_free(&_ctx);
    }
#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
    av_channel_layout_uninit(&_target_ch_layout);
#endif
}

FFmpegFrame::Ptr FFmpegSwr::inputFrame(const FFmpegFrame::Ptr &frame) {
    if (frame->get()->format == _target_format &&

#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
        !av_channel_layout_compare(&(frame->get()->ch_layout), &_target_ch_layout) &&
#else
        frame->get()->channels == _target_channels && frame->get()->channel_layout == (uint64_t)_target_channel_layout &&
#endif

        frame->get()->sample_rate == _target_samplerate) {
        // Do not convert format
        return frame;
    }
    if (!_ctx) {

#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
        _ctx = swr_alloc();
        swr_alloc_set_opts2(&_ctx, 
                    &_target_ch_layout, _target_format, _target_samplerate, 
                    &frame->get()->ch_layout, (AVSampleFormat)frame->get()->format, frame->get()->sample_rate,
                     0, nullptr);
#else
        _ctx = swr_alloc_set_opts(nullptr, _target_channel_layout, _target_format, _target_samplerate,
                                  frame->get()->channel_layout, (AVSampleFormat) frame->get()->format,
                                  frame->get()->sample_rate, 0, nullptr);
#endif

        InfoL << "swr_alloc_set_opts:" << av_get_sample_fmt_name((enum AVSampleFormat) frame->get()->format) << " -> "
              << av_get_sample_fmt_name(_target_format);
    }
    if (_ctx) {
        auto out = _swr_frame_pool.obtain2();
        out->get()->format = _target_format;

#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
        out->get()->ch_layout = _target_ch_layout;
        av_channel_layout_copy(&(out->get()->ch_layout), &_target_ch_layout);
#else
        out->get()->channel_layout = _target_channel_layout;
        out->get()->channels = _target_channels;
#endif

        out->get()->sample_rate = _target_samplerate;
        out->get()->pkt_dts = frame->get()->pkt_dts;
        out->get()->pts = frame->get()->pts;

        int ret = 0;
        if (0 != (ret = swr_convert_frame(_ctx, out->get(), frame->get()))) {
            WarnL << "swr_convert_frame failed:" << ffmpeg_err(ret);
            return nullptr;
        }
        return out;
    }

    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////

FFmpegSws::FFmpegSws(AVPixelFormat output, int width, int height) {
    _target_format = output;
    _target_width = width;
    _target_height = height;

    _sws_frame_pool.setSize(AV_NUM_DATA_POINTERS);
}

FFmpegSws::~FFmpegSws() {
    if (_ctx) {
        sws_freeContext(_ctx);
        _ctx = nullptr;
    }
}

int FFmpegSws::inputFrame(const FFmpegFrame::Ptr &frame, uint8_t *data) {
    int ret;
    inputFrame(frame, ret, data);
    return ret;
}

FFmpegFrame::Ptr FFmpegSws::inputFrame(const FFmpegFrame::Ptr &frame) {
    int ret;
    return inputFrame(frame, ret, nullptr);
}

FFmpegFrame::Ptr FFmpegSws::inputFrame(const FFmpegFrame::Ptr &frame, int &ret, uint8_t *data) {
    ret = -1;
    TimeTicker2(30, TraceL);
    auto target_width = _target_width ? _target_width : frame->get()->width;
    auto target_height = _target_height ? _target_height : frame->get()->height;
    if (frame->get()->format == _target_format && frame->get()->width == target_width && frame->get()->height == target_height) {
        // Do not convert format
        return frame;
    }
    if (_ctx && (_src_width != frame->get()->width || _src_height != frame->get()->height || _src_format != (enum AVPixelFormat)frame->get()->format)) {
        // Input resolution has changed
        sws_freeContext(_ctx);
        _ctx = nullptr;
    }
    if (!_ctx) {
        _src_format = (enum AVPixelFormat) frame->get()->format;
        _src_width = frame->get()->width;
        _src_height = frame->get()->height;
        _ctx = sws_getContext(frame->get()->width, frame->get()->height, (enum AVPixelFormat) frame->get()->format, target_width, target_height, _target_format, SWS_FAST_BILINEAR, NULL, NULL, NULL);
        DebugL << "sws_getContext:" << av_get_pix_fmt_name((enum AVPixelFormat) frame->get()->format) << " -> " << av_get_pix_fmt_name(_target_format);
    }
    if (_ctx) {
        auto out = _sws_frame_pool.obtain2();
        out->reset(); // Clean old data and frame references
        if (!out->get()->data[0]) {
            if (data) {
                av_image_fill_arrays(out->get()->data, out->get()->linesize, data, _target_format, target_width, target_height, 32);
            } else {
                out->fillPicture(_target_format, target_width, target_height);
            }
        }
        if (0 >= (ret = sws_scale(_ctx, frame->get()->data, frame->get()->linesize, 0, frame->get()->height, out->get()->data, out->get()->linesize))) {
            WarnL << "sws_scale failed:" << ffmpeg_err(ret);
            return nullptr;
        }

        out->get()->format = _target_format;
        out->get()->width = target_width;
        out->get()->height = target_height;
        out->get()->pkt_dts = frame->get()->pkt_dts;
        out->get()->pts = frame->get()->pts;
        return out;
    }
    return nullptr;
}

//////////////////////////////// FFmpegFrameRateFilter ////////////////////////////////

FFmpegFrameRateFilter::FFmpegFrameRateFilter(int target_fps, double source_fps)
    : _target_fps(target_fps > 0 ? target_fps : 5),
      _source_fps(source_fps > 0.0 ? source_fps : 0.0) {}

FFmpegFrameRateFilter::~FFmpegFrameRateFilter() {
    clearGraph();
}

bool FFmpegFrameRateFilter::isPassthrough() const {
    return _source_fps > 0.0 && std::abs(_source_fps - _target_fps) <= 0.01;
}

void FFmpegFrameRateFilter::setSourceFps(double source_fps) {
    source_fps = source_fps > 0.0 ? source_fps : 0.0;
    if (std::abs(source_fps - _source_fps) < 0.01) {
        return;
    }
    reset();
    _source_fps = source_fps;
}

void FFmpegFrameRateFilter::clearGraph() {
    if (_graph) {
        avfilter_graph_free(&_graph);
    }
    _source = nullptr;
    _sink = nullptr;
    _graph_width = 0;
    _graph_height = 0;
    _graph_format = AV_PIX_FMT_NONE;
    _sink_time_base = AVRational{ 1, 1000 };
}

void FFmpegFrameRateFilter::resetEpoch() {
    _source_epoch_pts = AV_NOPTS_VALUE;
    _last_source_pts = AV_NOPTS_VALUE;
    if (_last_output_pts != AV_NOPTS_VALUE) {
        _output_epoch_pts = _last_output_pts + std::max<int64_t>(1, 1000 / _target_fps);
    }
}

void FFmpegFrameRateFilter::reset() {
    clearGraph();
    resetEpoch();
}

int64_t FFmpegFrameRateFilter::normalizeSourcePts(const AVFrame *frame) {
    const int64_t fallback_interval = std::max<int64_t>(
        1, static_cast<int64_t>(std::llround(1000.0 / (_source_fps > 0.0 ? _source_fps : _target_fps))));
    int64_t source_pts = frame->pts;
    if (source_pts == AV_NOPTS_VALUE) {
        source_pts = _last_source_pts == AV_NOPTS_VALUE ? 0 : _last_source_pts + fallback_interval;
    }

    // A seek/reconnect starts a new source epoch, while the derived stream
    // remains monotonic for already connected muxer readers.
    if (_last_source_pts != AV_NOPTS_VALUE &&
        (source_pts < _last_source_pts || source_pts - _last_source_pts > 3000)) {
        reset();
    }
    if (_source_epoch_pts == AV_NOPTS_VALUE) {
        _source_epoch_pts = source_pts;
    }
    _last_source_pts = source_pts;
    return _output_epoch_pts + source_pts - _source_epoch_pts;
}

bool FFmpegFrameRateFilter::emitPassthrough(const FFmpegFrame::Ptr &frame, int64_t pts,
                                                const onOutput &callback) {
    AVFrame *copy = av_frame_clone(frame->get());
    if (!copy) {
        WarnL << "FFmpegFrameRateFilter: av_frame_clone failed";
        return false;
    }
    copy->pts = pts;
    copy->pkt_dts = pts;
    auto output = std::make_shared<FFmpegFrame>(
        std::shared_ptr<AVFrame>(copy, [](AVFrame *ptr) { av_frame_free(&ptr); }));
    _last_output_pts = pts;
    return callback ? callback(output) : true;
}

bool FFmpegFrameRateFilter::buildGraph(const AVFrame *frame) {
    clearGraph();
    _graph = avfilter_graph_alloc();
    if (!_graph) {
        WarnL << "FFmpegFrameRateFilter: avfilter_graph_alloc failed";
        return false;
    }

    const AVFilter *buffer = avfilter_get_by_name("buffer");
    const AVFilter *fps = avfilter_get_by_name("fps");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    if (!buffer || !fps || !buffersink) {
        WarnL << "FFmpegFrameRateFilter: required FFmpeg filters are unavailable";
        clearGraph();
        return false;
    }

    const AVRational sar = frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0
                               ? frame->sample_aspect_ratio
                               : AVRational{ 1, 1 };
    char source_args[512];
    snprintf(source_args, sizeof(source_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/1000:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format, sar.num, sar.den);
    int ret = avfilter_graph_create_filter(&_source, buffer, "transcode_fps_in",
                                           source_args, nullptr, _graph);
    if (ret < 0) {
        WarnL << "FFmpegFrameRateFilter: create buffer failed: " << ffmpeg_err(ret);
        clearGraph();
        return false;
    }

    AVFilterContext *fps_context = nullptr;
    const std::string fps_args = StrPrinter << "fps=" << _target_fps << ":round=near:eof_action=pass";
    ret = avfilter_graph_create_filter(&fps_context, fps, "transcode_fps",
                                       fps_args.data(), nullptr, _graph);
    if (ret < 0) {
        WarnL << "FFmpegFrameRateFilter: create fps failed: " << ffmpeg_err(ret);
        clearGraph();
        return false;
    }
    ret = avfilter_graph_create_filter(&_sink, buffersink, "transcode_fps_out",
                                       nullptr, nullptr, _graph);
    if (ret < 0 || avfilter_link(_source, 0, fps_context, 0) < 0 ||
        avfilter_link(fps_context, 0, _sink, 0) < 0 ||
        (ret = avfilter_graph_config(_graph, nullptr)) < 0) {
        WarnL << "FFmpegFrameRateFilter: configure graph failed: " << ffmpeg_err(ret);
        clearGraph();
        return false;
    }

    _sink_time_base = av_buffersink_get_time_base(_sink);
    _graph_width = frame->width;
    _graph_height = frame->height;
    _graph_format = static_cast<AVPixelFormat>(frame->format);
    InfoL << "FFmpegFrameRateFilter: CFR " << _source_fps << "fps -> "
          << _target_fps << "fps, time_base=" << _sink_time_base.num << "/" << _sink_time_base.den;
    return true;
}

bool FFmpegFrameRateFilter::drain(const onOutput &callback) {
    bool success = true;
    while (true) {
        AVFrame *raw = av_frame_alloc();
        if (!raw) {
            return false;
        }
        const int ret = av_buffersink_get_frame(_sink, raw);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&raw);
            break;
        }
        if (ret < 0) {
            WarnL << "FFmpegFrameRateFilter: buffersink failed: " << ffmpeg_err(ret);
            av_frame_free(&raw);
            return false;
        }

        raw->pts = av_rescale_q(raw->pts, _sink_time_base, AVRational{ 1, 1000 });
        raw->pkt_dts = raw->pts;
        _last_output_pts = raw->pts;
        auto output = std::make_shared<FFmpegFrame>(
            std::shared_ptr<AVFrame>(raw, [](AVFrame *ptr) { av_frame_free(&ptr); }));
        if (callback && !callback(output)) {
            success = false;
        }
    }
    return success;
}

bool FFmpegFrameRateFilter::inputFrame(const FFmpegFrame::Ptr &frame,
                                           const onOutput &callback) {
    if (!frame || !frame->get()) {
        return false;
    }
    if (!isPassthrough() && _graph &&
        (frame->get()->width != _graph_width || frame->get()->height != _graph_height ||
         frame->get()->format != _graph_format)) {
        reset();
    }
    const int64_t normalized_pts = normalizeSourcePts(frame->get());
    if (isPassthrough()) {
        return emitPassthrough(frame, normalized_pts, callback);
    }

    AVFrame *input = av_frame_clone(frame->get());
    if (!input) {
        return false;
    }
    input->pts = normalized_pts;
    input->pkt_dts = normalized_pts;

    if (!_graph) {
        if (!buildGraph(input)) {
            av_frame_free(&input);
            return false;
        }
    }
    const int ret = av_buffersrc_add_frame_flags(_source, input,
                                                  AV_BUFFERSRC_FLAG_KEEP_REF);
    av_frame_free(&input);
    if (ret < 0) {
        WarnL << "FFmpegFrameRateFilter: buffersrc failed: " << ffmpeg_err(ret);
        return false;
    }
    return drain(callback);
}

std::tuple<bool, std::string> FFmpegUtils::saveFrame(const FFmpegFrame::Ptr &frame, const char *filename, AVPixelFormat fmt, int w, int h, const char *font_path) {
    std::shared_ptr<AVFilterGraph> _filter_graph;
    AVFilterContext *buffersrc_ctx = nullptr;
    AVFilterContext *buffersink_ctx = nullptr;
    const AVFilter *buffersrc = nullptr;
    const AVFilter *buffersink = nullptr;
    // kServerName
    const string mark = "S3MediaKit"; 
    char drawtext_args1[512];
    _StrPrinter ss;

    std::unique_ptr<FILE, void (*)(FILE *)> tmp_save_file_jpg(File::create_file(filename, "wb"), [](FILE *fp) {
        if (fp) {
            fclose(fp);
        }
    });

    if (!tmp_save_file_jpg) {
        ss << "Could not open the file " << filename;
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    std::string fontfile("");
    if (font_path && File::fileExist(font_path)) {
        fontfile = font_path;
    } else {
        // Fallback to common default
        fontfile = exeDir() + "/DejaVuSans.ttf";
    }

    snprintf(drawtext_args1, sizeof(drawtext_args1), "text='%s':fontfile='%s':fontcolor=white@0.1:fontsize=h/50:x=w*0.02:y=h-th-h*0.02", mark.data(), fontfile.c_str());

    const AVCodec *jpeg_codec = avcodec_find_encoder(fmt == AV_PIX_FMT_YUVJ420P ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_PNG);
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext *)> jpeg_codec_ctx(
        jpeg_codec ? avcodec_alloc_context3(jpeg_codec) : nullptr, [](AVCodecContext *ctx) { avcodec_free_context(&ctx); });

    if (!jpeg_codec_ctx) {
        ss << "Could not allocate JPEG/PNG codec context";
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    jpeg_codec_ctx->width = (w > 0 && w < 8192) ? w : frame->get()->width;
    jpeg_codec_ctx->height = (h > 0 && h < 4320) ? h : frame->get()->height;
    jpeg_codec_ctx->pix_fmt = fmt;
    jpeg_codec_ctx->time_base = { 1, 1 };

    auto ret = avcodec_open2(jpeg_codec_ctx.get(), jpeg_codec, NULL);
    if (ret < 0) {
        ss << "Could not open JPEG/PNG codec, " << ffmpeg_err(ret);
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    FFmpegSws sws(fmt, jpeg_codec_ctx->width, jpeg_codec_ctx->height);
    auto new_frame = sws.inputFrame(frame);
    if (!new_frame) {
        ss << "Could not scale the frame";
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    _filter_graph.reset(avfilter_graph_alloc(), [](AVFilterGraph *ctx) { avfilter_graph_free(&ctx); });
    if (!_filter_graph) {
        ss << "avfilter_graph_alloc failed";
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    char args[512];
    snprintf(
        args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", jpeg_codec_ctx->width, jpeg_codec_ctx->height,
        jpeg_codec_ctx->pix_fmt, jpeg_codec_ctx->time_base.num, jpeg_codec_ctx->time_base.den, jpeg_codec_ctx->sample_aspect_ratio.num,
        jpeg_codec_ctx->sample_aspect_ratio.den);

    buffersrc = avfilter_get_by_name("buffer");

    if ((ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, _filter_graph.get())) < 0) {
        ss << "avfilter_graph_create_filter buffersrc failed: " << ret << " " << ffmpeg_err(ret);
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    buffersink = avfilter_get_by_name("buffersink");
    if ((ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, _filter_graph.get())) < 0) {
        ss << "avfilter_graph_create_filter buffersink failed: " << ret << " " << ffmpeg_err(ret);
        return make_tuple<bool, std::string>(false, ss.data());
    }

    AVFilterContext *drawtext_ctx1 = nullptr;

    const AVFilter *drawtext_filter = avfilter_get_by_name("drawtext");
    if (!drawtext_filter) {
        ss << "drawtext filter not found";
        return make_tuple<bool, std::string>(false, ss.data());
    }

    if ((ret = avfilter_graph_create_filter(&drawtext_ctx1, drawtext_filter, "drawtext", drawtext_args1, NULL, _filter_graph.get())) < 0) {
        ss << "avfilter_graph_create_filter drawtext_filter failed: " << ret << " " << ffmpeg_err(ret);
        return make_tuple<bool, std::string>(false, ss.data());
    }

    if ((ret = avfilter_link(buffersrc_ctx, 0, drawtext_ctx1, 0) < 0 || avfilter_link(drawtext_ctx1, 0, buffersink_ctx, 0))< 0) {
        ss << "avfilter_link: " << ret << " " << ffmpeg_err(ret);
        return make_tuple<bool, std::string>(false, ss.data());
    }

    if ((ret = avfilter_graph_config(_filter_graph.get(), NULL)) < 0) {
        ss << "avfilter_graph_config failed: " << ret << " " << ffmpeg_err(ret);
        return make_tuple<bool, std::string>(false, ss.data());
    }

    if ((ret = av_buffersrc_add_frame_flags(buffersrc_ctx, new_frame->get(), 0)) < 0) {
        ss << "av_buffersink_get_frame failed: " << ret << " " << ffmpeg_err(ret);
        return make_tuple<bool, std::string>(false, ss.data());
    }

    auto pkt = alloc_av_packet();
    while (av_buffersink_get_frame(buffersink_ctx, new_frame->get()) >= 0) {
        if (avcodec_send_frame(jpeg_codec_ctx.get(), new_frame->get()) == 0) {
            while (avcodec_receive_packet(jpeg_codec_ctx.get(), pkt.get()) == 0) {
                fwrite(pkt.get()->data, pkt.get()->size, 1, tmp_save_file_jpg.get());
            }
        }
    }
    return make_tuple<bool, std::string>(true, "");
}

std::shared_ptr<std::vector<uint8_t>> FFmpegUtils::encodeFrameToBuffer(
    const FFmpegFrame::Ptr &frame, AVPixelFormat fmt, int w, int h) {
    const AVCodec *jpeg_codec = avcodec_find_encoder(
        fmt == AV_PIX_FMT_YUVJ420P ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_PNG);
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext *)> ctx(
        jpeg_codec ? avcodec_alloc_context3(jpeg_codec) : nullptr,
        [](AVCodecContext *c) { avcodec_free_context(&c); });
    if (!ctx) return nullptr;

    ctx->width     = (w > 0 && w < 8192) ? w : frame->get()->width;
    ctx->height    = (h > 0 && h < 4320) ? h : frame->get()->height;
    ctx->pix_fmt   = fmt;
    ctx->time_base = {1, 1};

    if (avcodec_open2(ctx.get(), jpeg_codec, nullptr) < 0) return nullptr;

    FFmpegSws sws(fmt, ctx->width, ctx->height);
    auto scaled = sws.inputFrame(frame);
    if (!scaled) return nullptr;

    auto result = std::make_shared<std::vector<uint8_t>>();
    auto pkt = alloc_av_packet();
    if (avcodec_send_frame(ctx.get(), scaled->get()) == 0) {
        while (avcodec_receive_packet(ctx.get(), pkt.get()) == 0) {
            const uint8_t *d = pkt->data;
            result->insert(result->end(), d, d + pkt->size);
        }
    }
    return result->empty() ? nullptr : result;
}

std::tuple<bool, std::string> FFmpegUtils::drawGrid(const FFmpegFrame::Ptr &frame, int grid_rows, int grid_cols) {
    std::shared_ptr<AVFilterGraph> _filter_graph;
    AVFilterContext *buffersrc_ctx = nullptr;
    AVFilterContext *buffersink_ctx = nullptr;
    const AVFilter *buffersrc = nullptr;
    const AVFilter *buffersink = nullptr;
    char drawgrid_args[512];
    _StrPrinter ss;
    int ret = 0;

    snprintf(drawgrid_args, sizeof(drawgrid_args), "w=iw/%d:h=ih/%d:t=%d:c=red@0.25", grid_cols, grid_rows, 1);

    _filter_graph.reset(avfilter_graph_alloc(), [](AVFilterGraph *ctx) { avfilter_graph_free(&ctx); });
    if (!_filter_graph) {
        ss << "avfilter_graph_alloc failed";
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    char args[512];
    AVRational time_base = {1, 1};
    snprintf(
        args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", frame->get()->width, frame->get()->height, 
        frame->get()->format, time_base.num, time_base.den, frame->get()->sample_aspect_ratio.num, frame->get()->sample_aspect_ratio.den);

    buffersrc = avfilter_get_by_name("buffer");
    if ((ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, _filter_graph.get())) < 0) {
        ss << "avfilter_graph_create_filter buffersrc failed: " << ret << " " << ffmpeg_err(ret);
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    buffersink = avfilter_get_by_name("buffersink");
    if ((ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, _filter_graph.get())) < 0) {
        ss << "avfilter_graph_create_filter buffersink failed: " << ret << " " << ffmpeg_err(ret);
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    AVFilterContext *drawgrid_ctx = nullptr;

    const AVFilter *drawgrid_filter = avfilter_get_by_name("drawgrid");
    if (!drawgrid_filter) {
        ss << "drawgrid filter not found";
        return make_tuple<bool, std::string>(false, ss.data());
    }

    if ((ret = avfilter_graph_create_filter(&drawgrid_ctx, drawgrid_filter, "drawgrid", drawgrid_args, NULL, _filter_graph.get())) < 0) {
        ss << "avfilter_graph_create_filter drawgrid failed: " << ret << " " << ffmpeg_err(ret);
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    if ((ret = avfilter_link(buffersrc_ctx, 0, drawgrid_ctx, 0)) < 0 || (ret = avfilter_link(drawgrid_ctx, 0, buffersink_ctx, 0)) < 0) {
        ss << "avfilter_link: " << ret << " " << ffmpeg_err(ret);
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    if ((ret = avfilter_graph_config(_filter_graph.get(), NULL)) < 0) {
        ss << "avfilter_graph_config failed: " << ret << " " << ffmpeg_err(ret);
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    if ((ret = av_buffersrc_add_frame_flags(buffersrc_ctx, frame->get(), 0)) < 0) {
        ss << "av_buffersrc_add_frame_flags failed: " << ret << " " << ffmpeg_err(ret);
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    while (av_buffersink_get_frame(buffersink_ctx, frame->get()) >= 0) {
        // Here we just test the filter, so we do not care about the output frame, just return success
        break;
    }

    return make_tuple<bool, std::string>(true, "");
}

//////////////////////////////////////// FFmpegEncoder ////////////////////////////////////////

FFmpegEncoder::FFmpegEncoder(CodecId codec, int width, int height, int fps, int bitrate, int gop)
    : _codec(codec), _width(width), _height(height), _fps(fps > 0 ? fps : 5), _bitrate(bitrate), _gop(gop) {
    if (codec != CodecH264 && codec != CodecH265) {
        throw std::invalid_argument("FFmpegEncoder only supports H264/H265");
    }
}

FFmpegEncoder::~FFmpegEncoder() {
    try {
        flush();
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void FFmpegEncoder::setOnEncode(onEnc cb) {
    _cb = std::move(cb);
}

bool FFmpegEncoder::openEncoder(const FFmpegFrame::Ptr &frame) {
    auto src = frame->get();
    int out_w = 0;
    int out_h = 0;
    getTranscodeOutputSize(src->width, src->height, _width, _height, out_w, out_h);
    // libx264/libx265 require even dimensions for YUV420P
    out_w &= ~1;
    out_h &= ~1;
    if (out_w <= 0 || out_h <= 0) {
        WarnL << "FFmpegEncoder: invalid output size " << out_w << "x" << out_h;
        return false;
    }
    _width = out_w;
    _height = out_h;

    if (_bitrate <= 0) {
        _bitrate = getDefaultTranscodeBitrate(_codec, _width, _height, _fps);
    }

    const char *codec_name = (_codec == CodecH264) ? "libx264" : "libx265";
    const AVCodec *encoder = avcodec_find_encoder_by_name(codec_name);
    if (!encoder) {
        // Fallback to the built-in encoder id lookup
        encoder = avcodec_find_encoder(_codec == CodecH264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
    }
    if (!encoder) {
        WarnL << "FFmpegEncoder: encoder not found for " << codec_name;
        return false;
    }

    _context.reset(avcodec_alloc_context3(encoder), [](AVCodecContext *ctx) { avcodec_free_context(&ctx); });
    if (!_context) {
        WarnL << "FFmpegEncoder: avcodec_alloc_context3 failed";
        return false;
    }

    // Keep the encoder input format deterministic. FFmpegSws is only used
    // when the decoder/overlay frame is not already YUV420P at the target
    // dimensions.
    _enc_fmt = AV_PIX_FMT_YUV420P;
    _context->width = _width;
    _context->height = _height;
    _context->pix_fmt = _enc_fmt;
    // Work in millisecond time base so we can feed frame dts/pts (already in ms).
    _context->time_base = AVRational{ 1, 1000 };
    _context->framerate = AVRational{ _fps, 1 };
    _context->gop_size = (_gop > 0) ? _gop : (_fps * 2);
    _context->max_b_frames = 0;
    _context->bit_rate = _bitrate;
    // Do NOT set AV_CODEC_FLAG_GLOBAL_HEADER: we want in-band SPS/PPS (Annex-B) so
    // that H264Track/H265Track can auto-extract config from the keyframes.
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "preset", "veryfast", 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);
    if (_codec == CodecH264) {
        av_dict_set(&opts, "profile", "main", 0);
    }

    int ret = avcodec_open2(_context.get(), encoder, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        WarnL << "FFmpegEncoder: avcodec_open2 failed: " << ffmpeg_err(ret);
        _context = nullptr;
        return false;
    }

    InfoL << "FFmpegEncoder opened: " << codec_name << " " << _width << "x" << _height
          << " @" << _fps << "fps, bitrate=" << _context->bit_rate;
    return true;
}

bool FFmpegEncoder::inputFrame(const FFmpegFrame::Ptr &frame) {
    if (!frame || !frame->get()) {
        return false;
    }
    return inputFrame(frame, frame->get()->pts);
}

bool FFmpegEncoder::inputFrame(const FFmpegFrame::Ptr &frame, int64_t output_pts) {
    if (!frame || !frame->get()) {
        return false;
    }
    if (!_context && !openEncoder(frame)) {
        return false;
    }

    if (output_pts != AV_NOPTS_VALUE && _last_encoded_pts != AV_NOPTS_VALUE) {
        if (output_pts < _last_encoded_pts) {
            WarnL << "FFmpegEncoder: dropping non-monotonic frame pts=" << output_pts << ", last=" << _last_encoded_pts;
            return true;
        }

        // Frame-rate pacing is performed by TranscodeProcessor before the
        // overlay graph. Do not apply a second interval filter here, otherwise
        // frames near the boundary can be dropped twice and reduce output FPS.
    }

    auto src = frame->get();
    FFmpegFrame::Ptr scaled = frame;
    if (src->width != _width || src->height != _height || src->format != _enc_fmt) {
        if (!_sws) {
            _sws = std::make_shared<FFmpegSws>(_enc_fmt, _width, _height);
        }
        scaled = _sws->inputFrame(frame);
        if (!scaled) {
            return false;
        }
    } else {
        // Decoder frames are obtained from a reusable ResourcePool. Keep an
        // encoder-owned copy even when no conversion is needed.
        scaled = frame->clone();
        if (!scaled) {
            return false;
        }
    }
    if (output_pts != AV_NOPTS_VALUE) {
        // scaled is either produced by FFmpegSws or cloned above. It is now
        // owned by the encoder path and safe to retimestamp in place.
        scaled->get()->pts = output_pts;
        scaled->get()->pkt_dts = output_pts;
    }
    const bool encoded = encodeFrame(scaled->get());
    if (encoded && output_pts != AV_NOPTS_VALUE) {
        _last_encoded_pts = output_pts;
    }
    return encoded;
}

bool FFmpegEncoder::encodeFrame(AVFrame *frame) {
    if (frame) {
        // Feed pts in ms (the source AVFrame carries a ms pts already).
        frame->pict_type = _request_idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        _request_idr = false;
    }
    int ret = avcodec_send_frame(_context.get(), frame);
    if (ret < 0) {
        WarnL << "FFmpegEncoder: avcodec_send_frame failed: " << ffmpeg_err(ret);
        return false;
    }
    auto pkt = alloc_av_packet();
    while (true) {
        ret = avcodec_receive_packet(_context.get(), pkt.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            WarnL << "FFmpegEncoder: avcodec_receive_packet failed: " << ffmpeg_err(ret);
            return false;
        }
        onEncode(pkt.get());
        av_packet_unref(pkt.get());
    }
    return true;
}

void FFmpegEncoder::onEncode(AVPacket *pkt) {
    if (!_cb || !pkt->data || pkt->size <= 0) {
        return;
    }
    int64_t dts = (pkt->dts == AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
    int64_t pts = (pkt->pts == AV_NOPTS_VALUE) ? dts : pkt->pts;
    if (dts < 0) {
        dts = 0;
    }
    if (pts < 0) {
        pts = dts;
    }
    // libx264/libx265 emit Annex-B (start-code prefixed) NALs in-band, which
    // Factory::getFrameFromPtr expects for H264/H265.
    auto out = Factory::getFrameFromPtr(_codec, reinterpret_cast<const char *>(pkt->data),
                                        (size_t)pkt->size, (uint64_t)dts, (uint64_t)pts);
    if (out) {
        _cb(out);
    }
}

void FFmpegEncoder::flush() {
    if (_context) {
        encodeFrame(nullptr);
    }
}

void FFmpegEncoder::requestKeyFrame() {
    _request_idr = true;
}

} // namespace mediakit
#endif // ENABLE_FFMPEG
