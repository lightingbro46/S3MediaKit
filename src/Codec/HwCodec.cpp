#if defined(ENABLE_FFMPEG)

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
// Probe tests run in forked children on Linux only: macOS CoreFoundation/VideoToolbox abort in a
// forked child that did not exec, and an Android app process (ART, JNI for MediaCodec) must not fork.
#if defined(__linux__) && !defined(__ANDROID__)
#define HW_PROBE_IN_CHILD 1
#endif

#if defined(HW_PROBE_IN_CHILD)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__linux__) && !defined(__ANDROID__)
#include <climits>
#include <dlfcn.h>
#include <unistd.h>
#endif
#include "HwCodec.h"
#include "Common/config.h"
#include "Util/util.h"
#include "Util/logger.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

enum {
    kBreakerClosed = 0,
    kBreakerOpen,
    kBreakerHalfOpen,
};

// Shared by the devices of one backend: the NVENC session limit of consumer cards counts every card.
struct HwBackendGroup {
    std::mutex mtx;
    size_t encode_sessions = 0;
    // Encoder session limit learned from an open failure, valid until encode_cap_until.
    size_t encode_cap = 0;
    uint64_t encode_cap_until = 0;
};

// One device of a backend. Lock order: HwBackendState::mtx, then HwBackendGroup::mtx.
struct HwBackendState {
    std::mutex mtx;
    HwBackend backend = HwBackend::None;
    HwBackendCaps caps;
    AVBufferRef *device = nullptr;
    std::string device_name;
    std::shared_ptr<HwBackendGroup> group;
    size_t decode_sessions = 0;
    size_t encode_sessions = 0;
    // Sum of the sessions' hwStreamCost.
    double decode_load = 0;
    double encode_load = 0;
    // Consecutive failed sessions.
    int failures = 0;
    int breaker = kBreakerClosed;
    uint64_t open_until = 0;
    bool trial_in_flight = false;

    ~HwBackendState() { av_buffer_unref(&device); }
};

static string hw_ffmpeg_err(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return errbuf;
}

static int codecIndex(CodecId codec) {
    switch (codec) {
        case CodecH264: return 0;
        case CodecH265: return 1;
        default: return -1;
    }
}

const char *hwBackendName(HwBackend backend) {
    switch (backend) {
        case HwBackend::Cuda: return "cuda";
        case HwBackend::Vaapi: return "vaapi";
        case HwBackend::Qsv: return "qsv";
        case HwBackend::Videotoolbox: return "videotoolbox";
        case HwBackend::D3d11va: return "d3d11va";
        case HwBackend::Dxva2: return "dxva2";
        case HwBackend::Amf: return "amf";
        case HwBackend::Mediacodec: return "mediacodec";
        default: return "none";
    }
}

HwBackend hwBackendFromName(const string &name) {
    auto lower = strToLower(string(name));
    if (lower == "cuda" || lower == "nvidia" || lower == "nvenc") {
        return HwBackend::Cuda;
    }
    if (lower == "vaapi") {
        return HwBackend::Vaapi;
    }
    if (lower == "qsv") {
        return HwBackend::Qsv;
    }
    if (lower == "videotoolbox") {
        return HwBackend::Videotoolbox;
    }
    if (lower == "d3d11va") {
        return HwBackend::D3d11va;
    }
    if (lower == "dxva2") {
        return HwBackend::Dxva2;
    }
    if (lower == "amf") {
        return HwBackend::Amf;
    }
    if (lower == "mediacodec") {
        return HwBackend::Mediacodec;
    }
    return HwBackend::None;
}

static vector<HwBackend> configuredBackends() {
    GET_CONFIG(string, backends_cfg, HwCodec::kBackends);
    GET_CONFIG(bool, check_nvidia_dev, General::kCheckNvidiaDev);
    vector<HwBackend> ret;
    for (auto &name : split(backends_cfg, ",")) {
        trim(name);
        auto backend = hwBackendFromName(name);
        if (backend == HwBackend::None) {
            if (!name.empty()) {
                WarnL << "Unknown " << HwCodec::kBackends << " entry: " << name;
            }
            continue;
        }
        if (backend == HwBackend::Cuda && !check_nvidia_dev) {
            // general.check_nvidia_dev = 0 keeps its old meaning: never use the NVIDIA GPU.
            continue;
        }
        if (std::find(ret.begin(), ret.end(), backend) == ret.end()) {
            ret.emplace_back(backend);
        }
    }
    return ret;
}

// "cuda:0,vaapi:/dev/dri/renderD129" -> device of one backend; an entry without "backend:" applies to all.
static string deviceFor(HwBackend backend, const string &cfg) {
    string all;
    for (auto &item : split(cfg, ",")) {
        trim(item);
        auto pos = item.find(':');
        if (pos != string::npos) {
            auto named = hwBackendFromName(item.substr(0, pos));
            if (named != HwBackend::None) {
                if (named == backend) {
                    return item.substr(pos + 1);
                }
                continue;
            }
        }
        all = item;
    }
    return all;
}

// Load budget of backend from "cuda:720,vaapi:450"; 0 = no limit.
static double budgetFor(HwBackend backend, const string &cfg) {
    auto value = deviceFor(backend, cfg);
    return value.empty() ? 0 : std::max(0.0, atof(value.data()));
}

// Label of a device in logs and loads(): "cuda", "cuda:1", "vaapi:/dev/dri/renderD129".
static string stateLabel(const HwBackendState &state) {
    return state.device_name.empty() ? string(hwBackendName(state.backend))
                                     : string(hwBackendName(state.backend)) + ":" + state.device_name;
}

double hwStreamCost(int width, int height, float fps) {
    if (width <= 0 || height <= 0) {
        width = 1920;
        height = 1080;
    }
    if (fps <= 0) {
        fps = 25;
    }
    return (double)width * height * fps / (1920.0 * 1080.0);
}

//////////////////////////////////////////// HwCodecSession ////////////////////////////////////////////

HwCodecSession::HwCodecSession(std::shared_ptr<HwBackendState> state, bool encode, bool trial, double cost)
    : _state(std::move(state)), _encode(encode), _trial(trial), _cost(cost) {}

HwCodecSession::~HwCodecSession() {
    lock_guard<mutex> lck(_state->mtx);
    auto &count = _encode ? _state->encode_sessions : _state->decode_sessions;
    if (count > 0) {
        --count;
    }
    auto &load = _encode ? _state->encode_load : _state->decode_load;
    load = std::max(0.0, load - _cost);
    if (_encode && _state->group) {
        lock_guard<mutex> lck2(_state->group->mtx);
        if (_state->group->encode_sessions > 0) {
            --_state->group->encode_sessions;
        }
    }
    if (_trial && !_succeeded && !_failed && _state->breaker == kBreakerHalfOpen) {
        // The trial ended without a verdict (stream closed early): let the next session try.
        _state->trial_in_flight = false;
    }
}

HwBackend HwCodecSession::backend() const {
    return _state->backend;
}

const char *HwCodecSession::backendName() const {
    return hwBackendName(_state->backend);
}

AVBufferRef *HwCodecSession::device() const {
    return _state->device;
}

const string &HwCodecSession::deviceName() const {
    return _state->device_name;
}

double HwCodecSession::cost() const {
    lock_guard<mutex> lck(_state->mtx);
    return _cost;
}

void HwCodecSession::updateCost(int width, int height, float fps) {
    // Called per decoded frame by the owner thread: only take the lock when something changed.
    if (width == _width && height == _height && fps == _fps) {
        return;
    }
    _width = width;
    _height = height;
    _fps = fps;
    auto cost = hwStreamCost(width, height, fps);
    lock_guard<mutex> lck(_state->mtx);
    auto &load = _encode ? _state->encode_load : _state->decode_load;
    load = std::max(0.0, load + cost - _cost);
    _cost = cost;
}

AVPixelFormat HwCodecSession::hwPixFmt() const {
    return _state->caps.hw_pix_fmt;
}

void HwCodecSession::reportSuccess() {
    lock_guard<mutex> lck(_state->mtx);
    if (_succeeded || _failed) {
        return;
    }
    _succeeded = true;
    _state->failures = 0;
    if (_state->breaker != kBreakerClosed) {
        InfoL << "GPU backend " << stateLabel(*_state) << " works again, re-enabled";
    }
    _state->breaker = kBreakerClosed;
    _state->trial_in_flight = false;
}

void HwCodecSession::reportFailure(bool open_failure) {
    GET_CONFIG(int, fail_threshold, HwCodec::kFailThreshold);
    GET_CONFIG(int, cooldown_sec, HwCodec::kCooldownSec);
    auto now = getCurrentMillisecond();
    auto cooldown_ms = (uint64_t)std::max(0, cooldown_sec) * 1000;

    lock_guard<mutex> lck(_state->mtx);
    if (_failed) {
        return;
    }
    _failed = true;
    if (_trial) {
        _state->trial_in_flight = false;
    }
    if (open_failure && _encode && _state->group) {
        // Other encoders of this backend run fine: most likely the session limit (consumer NVIDIA
        // cards allow a few NVENC sessions for the whole system, every card together).
        lock_guard<mutex> lck2(_state->group->mtx);
        auto &group = *_state->group;
        if (group.encode_sessions > 1) {
            group.encode_cap = group.encode_sessions - 1;
            group.encode_cap_until = now + cooldown_ms;
            WarnL << "GPU encoder " << stateLabel(*_state) << " failed to open with " << group.encode_cap
                  << " running, new encoders above that count use the CPU for " << cooldown_sec << "s";
            return;
        }
    }
    ++_state->failures;
    if (_state->breaker == kBreakerHalfOpen || _state->failures >= std::max(1, fail_threshold)) {
        if (_state->breaker != kBreakerOpen) {
            WarnL << "GPU backend " << stateLabel(*_state) << " disabled for " << cooldown_sec << "s after "
                  << _state->failures << " failed session(s)";
        }
        _state->breaker = kBreakerOpen;
        _state->open_until = now + cooldown_ms;
    }
}

//////////////////////////////////////////// HwCodecManager ////////////////////////////////////////////

const char *hwEncoderName(HwBackend backend, CodecId codec) {
    const bool h264 = codec == CodecH264;
    if (!h264 && codec != CodecH265) {
        return nullptr;
    }
    switch (backend) {
        case HwBackend::Cuda: return h264 ? "h264_nvenc" : "hevc_nvenc";
        case HwBackend::Vaapi: return h264 ? "h264_vaapi" : "hevc_vaapi";
        case HwBackend::Qsv: return h264 ? "h264_qsv" : "hevc_qsv";
        case HwBackend::Videotoolbox: return h264 ? "h264_videotoolbox" : "hevc_videotoolbox";
        case HwBackend::Amf: return h264 ? "h264_amf" : "hevc_amf";
        case HwBackend::Mediacodec: return h264 ? "h264_mediacodec" : "hevc_mediacodec";
        default: return nullptr;
    }
}

static AVHWDeviceType toDeviceType(HwBackend backend) {
    switch (backend) {
        case HwBackend::Cuda: return AV_HWDEVICE_TYPE_CUDA;
        case HwBackend::Vaapi: return AV_HWDEVICE_TYPE_VAAPI;
        case HwBackend::Qsv: return AV_HWDEVICE_TYPE_QSV;
        case HwBackend::Videotoolbox: return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
        case HwBackend::D3d11va: return AV_HWDEVICE_TYPE_D3D11VA;
        case HwBackend::Dxva2: return AV_HWDEVICE_TYPE_DXVA2;
        // amf/mediacodec encoders take RAM frames and need no device context
        default: return AV_HWDEVICE_TYPE_NONE;
    }
}

std::shared_ptr<AVCodecContext> openHwEncoder(HwBackend backend, CodecId codec, AVBufferRef *device,
                                              int width, int height, int fps, int bitrate, int gop,
                                              AVPixelFormat &input_fmt, bool &upload, std::string &err) {
    upload = false;
    auto name = hwEncoderName(backend, codec);
    if (!name) {
        err = "no GPU encoder for this codec";
        return nullptr;
    }
    auto encoder = avcodec_find_encoder_by_name(name);
    if (!encoder) {
        err = string(name) + " is not built into FFmpeg";
        return nullptr;
    }
    std::shared_ptr<AVCodecContext> ctx(avcodec_alloc_context3(encoder), [](AVCodecContext *ptr) {
        avcodec_free_context(&ptr);
    });
    if (!ctx) {
        err = "avcodec_alloc_context3 failed";
        return nullptr;
    }
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = AVRational { 1, 1000 };
    ctx->framerate = AVRational { fps, 1 };
    ctx->gop_size = gop;
    ctx->max_b_frames = 0;
    ctx->bit_rate = bitrate;
    // No AV_CODEC_FLAG_GLOBAL_HEADER: SPS/PPS stay in-band on each IDR, which H264Track/H265Track need.

    AVDictionary *opts = nullptr;
    if (codec == CodecH264) {
        av_dict_set(&opts, "profile", "main", 0);
    }
    switch (backend) {
        case HwBackend::Cuda:
            // nvenc takes planar YUV420P from RAM, the same format libx264 gets.
            input_fmt = AV_PIX_FMT_YUV420P;
            ctx->pix_fmt = input_fmt;
            // "llhp" exists in FFmpeg 4.4 and 7.x (mapped to p2 + low latency on new SDKs).
            av_dict_set(&opts, "preset", "llhp", 0);
            av_dict_set(&opts, "zerolatency", "1", 0);
            av_dict_set(&opts, "delay", "0", 0);
            av_dict_set(&opts, "forced-idr", "1", 0);
            break;
        case HwBackend::Qsv:
            input_fmt = AV_PIX_FMT_NV12;
            ctx->pix_fmt = input_fmt;
            av_dict_set(&opts, "preset", "veryfast", 0);
            av_dict_set(&opts, "look_ahead", "0", 0);
            av_dict_set(&opts, "async_depth", "1", 0);
            av_dict_set(&opts, "forced_idr", "1", 0);
            break;
        case HwBackend::Vaapi:
            input_fmt = AV_PIX_FMT_NV12;
            ctx->pix_fmt = AV_PIX_FMT_VAAPI;
            av_dict_set(&opts, "rc_mode", "VBR", 0);
            upload = true;
            break;
        case HwBackend::Videotoolbox:
            input_fmt = AV_PIX_FMT_YUV420P;
            ctx->pix_fmt = input_fmt;
            av_dict_set(&opts, "realtime", "1", 0);
            // Fail instead of silently using Apple's software encoder.
            av_dict_set(&opts, "allow_sw", "0", 0);
            av_dict_set(&opts, "prio_speed", "1", 0);
            break;
        case HwBackend::Amf:
            input_fmt = AV_PIX_FMT_NV12;
            ctx->pix_fmt = input_fmt;
            av_dict_set(&opts, "usage", "lowlatency", 0);
            av_dict_set(&opts, "quality", "speed", 0);
            // SPS/PPS on every IDR (option names differ between FFmpeg versions; unknown ones are ignored).
            av_dict_set(&opts, "header_spacing", "1", 0);
            av_dict_set(&opts, "header_insertion_mode", "idr", 0);
            break;
        case HwBackend::Mediacodec:
            input_fmt = AV_PIX_FMT_NV12;
            ctx->pix_fmt = input_fmt;
            av_dict_set(&opts, "bitrate_mode", "vbr", 0);
            break;
        default:
            err = "unknown backend";
            return nullptr;
    }

    if (device && (backend == HwBackend::Cuda || backend == HwBackend::Qsv || backend == HwBackend::Vaapi)) {
        // nvenc/qsv/vaapi share the probed device; the other encoders manage their own.
        ctx->hw_device_ctx = av_buffer_ref(device);
    }
    if (upload) {
        if (!device) {
            av_dict_free(&opts);
            err = "vaapi needs a device";
            return nullptr;
        }
        auto frames = av_hwframe_ctx_alloc(device);
        if (!frames) {
            av_dict_free(&opts);
            err = "av_hwframe_ctx_alloc failed";
            return nullptr;
        }
        auto frames_ctx = (AVHWFramesContext *)frames->data;
        frames_ctx->format = AV_PIX_FMT_VAAPI;
        frames_ctx->sw_format = input_fmt;
        frames_ctx->width = width;
        frames_ctx->height = height;
        frames_ctx->initial_pool_size = 16;
        auto ret = av_hwframe_ctx_init(frames);
        if (ret < 0) {
            av_buffer_unref(&frames);
            av_dict_free(&opts);
            err = "av_hwframe_ctx_init failed: " + hw_ffmpeg_err(ret);
            return nullptr;
        }
        ctx->hw_frames_ctx = frames;
    }

    auto ret = avcodec_open2(ctx.get(), encoder, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        err = string(name) + " open failed: " + hw_ffmpeg_err(ret);
        return nullptr;
    }
    return ctx;
}

//////////////////////////////////////////// child-process tests ////////////////////////////////////////////

#if defined(HW_PROBE_IN_CHILD)
// Runs body in a forked child. Lines the child emits ("name=1 detail" / "name=0 detail") are parsed
// into values; status describes how the child ended. clean = it ran to the end.
struct HwChildRun {
    std::map<string, HwTestResult> values;
    string status;
    bool clean = false;
};

static bool runInChildProcess(const std::function<void(const std::function<void(const string &)> &emit)> &body,
                              int timeout_ms, HwChildRun &run) {
    int fds[2];
    if (pipe(fds) != 0) {
        run.status = string("pipe failed: ") + strerror(errno);
        return false;
    }
    auto pid = fork();
    if (pid < 0) {
        run.status = string("fork failed: ") + strerror(errno);
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        // Child: one thread, copy of the server's memory. Locks held by other server threads at fork
        // time never get released here, so no logger, config, NoticeCenter or poller from now on.
        close(fds[0]);
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            // Drivers and x265 print to stdout/stderr.
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        // The server's crash handler logs a backtrace (logger) and the server enables core dumps.
        for (auto sig : { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGALRM, SIGPIPE }) {
            signal(sig, SIG_DFL);
        }
        sigset_t all;
        sigemptyset(&all);
        sigprocmask(SIG_SETMASK, &all, nullptr);
        struct rlimit no_core = { 0, 0 };
        setrlimit(RLIMIT_CORE, &no_core);
#if defined(__linux__)
        // A piped core_pattern (apport, systemd-coredump, WSL) ignores RLIMIT_CORE and can take
        // seconds per crash; not dumpable means no core at all.
        prctl(PR_SET_DUMPABLE, 0);
#endif
        // Backstop if the parent is gone; the parent normally kills the child first.
        alarm(timeout_ms / 1000 + 5);
        av_log_set_callback(av_log_default_callback);
        av_log_set_level(AV_LOG_QUIET);

        auto emit = [&](const string &line) {
            auto data = line + "\n";
            size_t off = 0;
            while (off < data.size()) {
                auto n = write(fds[1], data.data() + off, data.size() - off);
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                if (n <= 0) {
                    _exit(2);
                }
                off += n;
            }
        };
        try {
            body(emit);
        } catch (...) {
            _exit(3);
        }
        _exit(0);
    }

    close(fds[1]);
    string buf;
    bool timed_out = false;
    auto deadline = getCurrentMillisecond() + timeout_ms;
    while (true) {
        auto now = getCurrentMillisecond();
        if (now >= deadline) {
            timed_out = true;
            break;
        }
        struct pollfd pfd = { fds[0], POLLIN, 0 };
        auto ret = poll(&pfd, 1, (int)(deadline - now));
        if (ret < 0 && errno == EINTR) {
            continue;
        }
        if (ret == 0) {
            timed_out = true;
            break;
        }
        char tmp[1024];
        auto n = read(fds[0], tmp, sizeof(tmp));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        buf.append(tmp, n);
    }
    close(fds[0]);
    if (timed_out) {
        kill(pid, SIGKILL);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    if (timed_out) {
        run.status = "test process hung, killed after " + to_string(timeout_ms) + " ms";
    } else if (WIFSIGNALED(status)) {
        run.status = "test process crashed with signal " + to_string(WTERMSIG(status)) + " (" + strsignal(WTERMSIG(status)) + ")";
    } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        run.status = "test process exited with code " + to_string(WEXITSTATUS(status));
    } else {
        run.status = "ok";
        run.clean = true;
    }

    // Only complete lines: a line cut by a crash is ignored.
    size_t pos = 0;
    while (true) {
        auto end = buf.find('\n', pos);
        if (end == string::npos) {
            break;
        }
        auto line = buf.substr(pos, end - pos);
        pos = end + 1;
        auto eq = line.find('=');
        if (eq == string::npos || eq + 1 >= line.size()) {
            continue;
        }
        HwTestResult result;
        result.ok = line[eq + 1] == '1';
        result.detail = eq + 3 <= line.size() ? line.substr(eq + 3) : string();
        run.values[line.substr(0, eq)] = result;
    }
    return true;
}
#endif

static HwTestResult failedResult(const string &detail) {
    HwTestResult result;
    result.detail = detail;
    return result;
}

bool runHwTestsInChildren(const HwChildTest &setup, const std::vector<std::pair<std::string, HwChildTest>> &tests,
                          int timeout_ms, std::map<std::string, HwTestResult> &results) {
#if !defined(HW_PROBE_IN_CHILD)
    return false;
#else
    results.clear();
    // Every crash removes one test, so tests.size() + 1 children are enough.
    for (size_t attempt = 0; attempt <= tests.size(); ++attempt) {
        HwChildRun run;
        auto ok = runInChildProcess([&](const std::function<void(const string &)> &emit) {
            string detail;
            if (!setup(detail)) {
                emit("setup=0 " + detail);
                return;
            }
            emit("setup=1");
            for (auto &test : tests) {
                if (results.count(test.first)) {
                    continue;
                }
                detail.clear();
                auto passed = test.second(detail);
                emit(test.first + (passed ? "=1 " : "=0 ") + detail);
            }
        }, timeout_ms, run);
        if (!ok) {
            return attempt > 0;
        }

        auto setup_it = run.values.find("setup");
        if (setup_it == run.values.end()) {
            results["setup"] = failedResult(run.status);
            return true;
        }
        results["setup"] = setup_it->second;
        if (!setup_it->second.ok) {
            return true;
        }
        for (auto &test : tests) {
            auto it = run.values.find(test.first);
            if (it != run.values.end()) {
                results[test.first] = it->second;
            }
        }
        if (run.clean) {
            break;
        }
        // The first test without a result is the one that took the child down.
        auto culprit = std::find_if(tests.begin(), tests.end(), [&](const std::pair<std::string, HwChildTest> &test) {
            return !results.count(test.first);
        });
        if (culprit == tests.end()) {
            break;
        }
        results[culprit->first] = failedResult(run.status);
    }
    for (auto &test : tests) {
        if (!results.count(test.first)) {
            results[test.first] = failedResult("not run");
        }
    }
    return true;
#endif
}

//////////////////////////////////////////// FFmpeg probe ////////////////////////////////////////////

static const AVCodecID kProbeIds[2] = { AV_CODEC_ID_H264, AV_CODEC_ID_HEVC };
static const CodecId kProbeCodecs[2] = { CodecH264, CodecH265 };
static const char *kProbeNames[2] = { "h264", "hevc" };
static const int kProbeWidth = 256;
static const int kProbeHeight = 144;
static const int kProbeFrames = 5;

// Surface format of the native decoder for this device type, or NONE when FFmpeg has no hwaccel for it.
static AVPixelFormat decoderHwFormat(AVCodecID id, AVHWDeviceType type) {
    auto decoder = avcodec_find_decoder(id);
    for (int i = 0; decoder; ++i) {
        auto cfg = avcodec_get_hw_config(decoder, i);
        if (!cfg) {
            break;
        }
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type) {
            return cfg->pix_fmt;
        }
    }
    return AV_PIX_FMT_NONE;
}

static std::shared_ptr<AVFrame> makeProbeFrame(AVPixelFormat fmt, int index) {
    std::shared_ptr<AVFrame> frame(av_frame_alloc(), [](AVFrame *ptr) { av_frame_free(&ptr); });
    frame->format = fmt;
    frame->width = kProbeWidth;
    frame->height = kProbeHeight;
    if (av_frame_get_buffer(frame.get(), 32) < 0) {
        return nullptr;
    }
    for (int y = 0; y < kProbeHeight; ++y) {
        for (int x = 0; x < kProbeWidth; ++x) {
            frame->data[0][y * frame->linesize[0] + x] = (uint8_t)(x + y + index * 8);
        }
    }
    for (int y = 0; y < kProbeHeight / 2; ++y) {
        if (fmt == AV_PIX_FMT_NV12) {
            memset(frame->data[1] + y * frame->linesize[1], 128, kProbeWidth);
        } else {
            memset(frame->data[1] + y * frame->linesize[1], 128, kProbeWidth / 2);
            memset(frame->data[2] + y * frame->linesize[2], 128, kProbeWidth / 2);
        }
    }
    frame->pts = index * 200;
    return frame;
}

static bool drainPackets(AVCodecContext *ctx, vector<string> &packets, string &err) {
    std::shared_ptr<AVPacket> pkt(av_packet_alloc(), [](AVPacket *ptr) { av_packet_free(&ptr); });
    while (true) {
        auto ret = avcodec_receive_packet(ctx, pkt.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            err = "avcodec_receive_packet: " + hw_ffmpeg_err(ret);
            return false;
        }
        packets.emplace_back((const char *)pkt->data, pkt->size);
        av_packet_unref(pkt.get());
    }
}

static bool encodeProbeFrames(AVCodecContext *ctx, AVPixelFormat fmt, bool upload, vector<string> &packets, string &err) {
    for (int i = 0; i < kProbeFrames; ++i) {
        auto frame = makeProbeFrame(fmt, i);
        if (!frame) {
            err = "av_frame_get_buffer failed";
            return false;
        }
        frame->pict_type = i == 0 ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        std::shared_ptr<AVFrame> hw_frame;
        auto input = frame.get();
        if (upload) {
            hw_frame.reset(av_frame_alloc(), [](AVFrame *ptr) { av_frame_free(&ptr); });
            auto ret = av_hwframe_get_buffer(ctx->hw_frames_ctx, hw_frame.get(), 0);
            if (ret >= 0) {
                ret = av_hwframe_transfer_data(hw_frame.get(), frame.get(), 0);
            }
            if (ret >= 0) {
                ret = av_frame_copy_props(hw_frame.get(), frame.get());
            }
            if (ret < 0) {
                err = "upload: " + hw_ffmpeg_err(ret);
                return false;
            }
            input = hw_frame.get();
        }
        auto ret = avcodec_send_frame(ctx, input);
        if (ret < 0) {
            err = "avcodec_send_frame: " + hw_ffmpeg_err(ret);
            return false;
        }
        if (!drainPackets(ctx, packets, err)) {
            return false;
        }
    }
    avcodec_send_frame(ctx, nullptr);
    return drainPackets(ctx, packets, err);
}

// Whether an Annex-B packet carries the SPS in-band.
static bool hasInbandSps(const string &data, CodecId codec) {
    for (size_t i = 0; i + 3 < data.size(); ++i) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) {
            continue;
        }
        auto header = (uint8_t)data[i + 3];
        if (codec == CodecH264 ? (header & 0x1f) == 7 : ((header >> 1) & 0x3f) == 33) {
            return true;
        }
    }
    return false;
}

static bool testHwEncode(HwBackend backend, CodecId codec, AVBufferRef *device, string &err) {
    AVPixelFormat input_fmt = AV_PIX_FMT_NONE;
    bool upload = false;
    auto ctx = openHwEncoder(backend, codec, device, kProbeWidth, kProbeHeight, 5, 300000, kProbeFrames, input_fmt, upload, err);
    if (!ctx) {
        return false;
    }
    vector<string> packets;
    if (!encodeProbeFrames(ctx.get(), input_fmt, upload, packets, err)) {
        return false;
    }
    if (packets.empty()) {
        err = "encoder produced no packet";
        return false;
    }
    if (!hasInbandSps(packets[0], codec)) {
        err = "first packet has no in-band SPS";
        return false;
    }
    return true;
}

// Test stream for the decode test, made on the CPU.
static bool encodeSoftware(CodecId codec, vector<string> &packets, string &err) {
    auto encoder = avcodec_find_encoder_by_name(codec == CodecH264 ? "libx264" : "libx265");
    if (!encoder) {
        encoder = avcodec_find_encoder(codec == CodecH264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
    }
    if (!encoder) {
        err = "no software encoder";
        return false;
    }
    std::shared_ptr<AVCodecContext> ctx(avcodec_alloc_context3(encoder), [](AVCodecContext *ptr) {
        avcodec_free_context(&ptr);
    });
    ctx->width = kProbeWidth;
    ctx->height = kProbeHeight;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->time_base = AVRational { 1, 1000 };
    ctx->framerate = AVRational { 5, 1 };
    ctx->gop_size = kProbeFrames;
    ctx->max_b_frames = 0;
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "preset", "ultrafast", 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);
    av_dict_set(&opts, "x265-params", "log-level=none", 0);
    auto ret = avcodec_open2(ctx.get(), encoder, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        err = string(encoder->name) + " open failed: " + hw_ffmpeg_err(ret);
        return false;
    }
    return encodeProbeFrames(ctx.get(), AV_PIX_FMT_YUV420P, false, packets, err);
}

static AVPixelFormat s_probe_hw_fmt = AV_PIX_FMT_NONE;

static AVPixelFormat probeGetFormat(AVCodecContext *, const AVPixelFormat *fmts) {
    for (auto p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == s_probe_hw_fmt) {
            return *p;
        }
    }
    // No software fallback here: the test is whether the GPU decodes.
    return AV_PIX_FMT_NONE;
}

static bool testHwDecode(int idx, AVPixelFormat hw_fmt, AVBufferRef *device, string &err) {
    if (hw_fmt == AV_PIX_FMT_NONE) {
        err = "FFmpeg has no hwaccel for this device";
        return false;
    }
    vector<string> packets;
    if (!encodeSoftware(kProbeCodecs[idx], packets, err)) {
        // Cannot build a test stream: trust FFmpeg's hw config, runtime fallback still applies.
        err = "untested (" + err + ")";
        return true;
    }
    auto decoder = avcodec_find_decoder(kProbeIds[idx]);
    std::shared_ptr<AVCodecContext> ctx(avcodec_alloc_context3(decoder), [](AVCodecContext *ptr) {
        avcodec_free_context(&ptr);
    });
    ctx->hw_device_ctx = av_buffer_ref(device);
    ctx->get_format = probeGetFormat;
    s_probe_hw_fmt = hw_fmt;
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "threads", "1", 0);
    auto ret = avcodec_open2(ctx.get(), decoder, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        err = "decoder open failed: " + hw_ffmpeg_err(ret);
        return false;
    }

    int downloaded = 0;
    std::shared_ptr<AVPacket> pkt(av_packet_alloc(), [](AVPacket *ptr) { av_packet_free(&ptr); });
    std::shared_ptr<AVFrame> frame(av_frame_alloc(), [](AVFrame *ptr) { av_frame_free(&ptr); });
    auto receive = [&]() {
        while (avcodec_receive_frame(ctx.get(), frame.get()) >= 0) {
            if (frame->format == hw_fmt) {
                std::shared_ptr<AVFrame> sw(av_frame_alloc(), [](AVFrame *ptr) { av_frame_free(&ptr); });
                auto ret = av_hwframe_transfer_data(sw.get(), frame.get(), 0);
                if (ret >= 0) {
                    ++downloaded;
                } else {
                    err = "av_hwframe_transfer_data: " + hw_ffmpeg_err(ret);
                }
            }
            av_frame_unref(frame.get());
        }
    };
    for (auto &data : packets) {
        pkt->data = (uint8_t *)data.data();
        pkt->size = (int)data.size();
        ret = avcodec_send_packet(ctx.get(), pkt.get());
        if (ret < 0) {
            err = "avcodec_send_packet: " + hw_ffmpeg_err(ret);
            return false;
        }
        receive();
    }
    avcodec_send_packet(ctx.get(), nullptr);
    receive();
    if (!downloaded) {
        if (err.empty()) {
            err = "no frame decoded on the GPU";
        }
        return false;
    }
    err.clear();
    return true;
}

// Creates the backend's device; backends without one (amf, mediacodec) leave ctx null and succeed.
static int createDevice(HwBackend backend, const string &device, AVBufferRef *&ctx) {
    ctx = nullptr;
    auto type = toDeviceType(backend);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        return 0;
    }
    return av_hwdevice_ctx_create(&ctx, type, device.empty() ? nullptr : device.data(), nullptr, 0);
}

// Open-only encoder check, used where the tests cannot run in a child process (Windows, macOS,
// Android, or fork failed). A driver that crashes while encoding is not caught here.
static bool inProcessProbe(HwBackend backend, const string &device, HwBackendCaps &caps, AVBufferRef *&device_ctx) {
    auto type = toDeviceType(backend);
    AVBufferRef *ctx = nullptr;
    auto ret = createDevice(backend, device, ctx);
    if (ret < 0) {
        InfoL << "GPU backend " << hwBackendName(backend) << " unavailable: " << hw_ffmpeg_err(ret);
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        auto hw_fmt = ctx ? decoderHwFormat(kProbeIds[i], type) : AV_PIX_FMT_NONE;
        caps.decode[i] = hw_fmt != AV_PIX_FMT_NONE;
        if (caps.decode[i]) {
            caps.hw_pix_fmt = hw_fmt;
        }
        if (!hwEncoderName(backend, kProbeCodecs[i])) {
            continue;
        }
        AVPixelFormat input_fmt;
        bool upload;
        string err;
        caps.encode[i] = openHwEncoder(backend, kProbeCodecs[i], ctx, kProbeWidth, kProbeHeight, 5, 300000, 10, input_fmt, upload, err) != nullptr;
        if (!caps.encode[i]) {
            InfoL << "GPU backend " << hwBackendName(backend) << ": " << err;
        }
    }
    if (!ctx && !caps.encode[0] && !caps.encode[1]) {
        return false;
    }
    device_ctx = ctx;
    return true;
}

static bool ffmpegProbe(HwBackend backend, const string &device, HwBackendCaps &caps, AVBufferRef *&device_ctx) {
    auto type = toDeviceType(backend);
    auto name = hwBackendName(backend);
    AVPixelFormat hw_fmt[2] = { decoderHwFormat(kProbeIds[0], type), decoderHwFormat(kProbeIds[1], type) };

    // Only ever used inside the test children.
    AVBufferRef *child_device = nullptr;
    auto setup = [&](string &detail) {
        auto ret = createDevice(backend, device, child_device);
        if (ret < 0) {
            detail = hw_ffmpeg_err(ret);
            return false;
        }
        return true;
    };
    // Only what the backend can do at all: d3d11va/dxva2 have no encoder, amf/mediacodec no hwaccel.
    vector<std::pair<string, HwChildTest>> tests;
    for (int i = 0; i < 2; ++i) {
        if (hw_fmt[i] != AV_PIX_FMT_NONE) {
            tests.emplace_back(string(kProbeNames[i]) + ".decode", [&, i](string &detail) {
                return testHwDecode(i, hw_fmt[i], child_device, detail);
            });
        }
        if (hwEncoderName(backend, kProbeCodecs[i])) {
            tests.emplace_back(string(kProbeNames[i]) + ".encode", [&, i](string &detail) {
                return testHwEncode(backend, kProbeCodecs[i], child_device, detail);
            });
        }
    }
    if (tests.empty()) {
        InfoL << "GPU backend " << name << ": FFmpeg has neither a hwaccel nor an encoder for it";
        return false;
    }

    std::map<string, HwTestResult> results;
    if (!runHwTestsInChildren(setup, tests, 20 * 1000, results)) {
        WarnL << "GPU backend " << name << ": cannot run the probe in a child process, probing in-process";
        return inProcessProbe(backend, device, caps, device_ctx);
    }
    auto &setup_result = results["setup"];
    if (!setup_result.ok) {
        InfoL << "GPU backend " << name << " unavailable: " << setup_result.detail;
        return false;
    }
    bool any = false;
    for (auto &test : tests) {
        auto &result = results[test.first];
        if (!result.ok) {
            InfoL << "GPU backend " << name << ": " << test.first << " disabled, " << result.detail;
        } else if (!result.detail.empty()) {
            InfoL << "GPU backend " << name << ": " << test.first << " " << result.detail;
        }
        any = any || result.ok;
    }
    for (int i = 0; i < 2; ++i) {
        caps.decode[i] = results.count(string(kProbeNames[i]) + ".decode") && results[string(kProbeNames[i]) + ".decode"].ok;
        caps.encode[i] = results.count(string(kProbeNames[i]) + ".encode") && results[string(kProbeNames[i]) + ".encode"].ok;
    }
    if (!any) {
        return false;
    }
    auto ret = createDevice(backend, device, device_ctx);
    if (ret < 0) {
        InfoL << "GPU backend " << name << " unavailable: " << hw_ffmpeg_err(ret);
        device_ctx = nullptr;
        return false;
    }
    caps.hw_pix_fmt = hw_fmt[0] != AV_PIX_FMT_NONE ? hw_fmt[0] : hw_fmt[1];
    return true;
}

HwCodecManager &HwCodecManager::Instance() {
    // Never destroyed: releasing the device contexts during static destruction calls into GPU drivers
    // that may already be torn down (Mesa d3d12 segfaults in vaTerminate). The OS frees them at exit.
    static HwCodecManager *instance = new HwCodecManager();
    return *instance;
}

HwCodecManager::HwCodecManager() = default;
HwCodecManager::~HwCodecManager() = default;

#if defined(__linux__) && !defined(__ANDROID__)
/**
 * vaapi without a configured device: FFmpeg opens the first render node that opens, whatever its
 * driver, and the node numbers follow the driver load order. With an NVIDIA card next to an Intel iGPU
 * that is often the NVIDIA node: the probe fails (or, with nvidia-vaapi-driver, vaapi runs on the same
 * NVDEC as cuda) and the iGPU stays idle. Picks the first i915/xe node, else amdgpu, else any other
 * driver but nvidia. Returns false when render nodes exist but all are NVIDIA: vaapi is not used then.
 * node stays empty when sysfs tells nothing (FFmpeg default).
 */
static bool pickVaapiNode(string &node) {
    node.clear();
    string node_driver;
    bool any = false;
    int best = INT_MAX;
    for (int i = 128; i < 136; ++i) {
        char link[64];
        snprintf(link, sizeof(link), "/sys/class/drm/renderD%d/device/driver", i);
        char target[PATH_MAX];
        auto len = readlink(link, target, sizeof(target) - 1);
        if (len <= 0) {
            continue;
        }
        target[len] = '\0';
        any = true;
        auto slash = strrchr(target, '/');
        string driver = slash ? slash + 1 : target;
        int rank = driver == "i915" || driver == "xe" ? 0 : driver == "amdgpu" ? 1 : driver == "nvidia" ? -1 : 2;
        if (rank >= 0 && rank < best) {
            best = rank;
            node = "/dev/dri/renderD" + to_string(i);
            node_driver = driver;
        }
    }
    if (!node.empty()) {
        InfoL << "GPU backend vaapi: using " << node << " (" << node_driver << ")";
        return true;
    }
    if (any) {
        InfoL << "GPU backend vaapi: only NVIDIA render nodes, vaapi not used (cuda covers them)";
        return false;
    }
    return true;
}
#endif

// Which FFmpeg the process really loaded: the bundled libavutil_s3 next to the binary or a system copy.
static void logFFmpegLibrary() {
    string path = "unknown";
#if defined(__linux__) && !defined(__ANDROID__)
    // The version string lives in libavutil's .rodata, so dladdr names the loaded file.
    Dl_info info;
    if (dladdr(av_version_info(), &info) && info.dli_fname) {
        path = info.dli_fname;
    }
#endif
    InfoL << "FFmpeg " << av_version_info() << " loaded from " << path;
}

void HwCodecManager::probe() {
    logFFmpegLibrary();
    probe(ffmpegProbe, true);
}

void HwCodecManager::probe(const ProbeFunc &func, bool pick_vaapi_node) {
    GET_CONFIG(bool, enable, HwCodec::kEnable);
    GET_CONFIG(string, device_cfg, HwCodec::kDevice);
    vector<std::shared_ptr<HwBackendState>> states;
    if (enable) {
        for (auto backend : configuredBackends()) {
            auto devices = split(deviceFor(backend, device_cfg), ";");
            for (auto &device : devices) {
                trim(device);
            }
            devices.erase(std::remove(devices.begin(), devices.end(), string()), devices.end());
            if (devices.empty()) {
                string device;
#if defined(__linux__) && !defined(__ANDROID__)
                if (backend == HwBackend::Vaapi && pick_vaapi_node && !pickVaapiNode(device)) {
                    continue;
                }
#endif
                devices.emplace_back(device);
            }
            auto group = std::make_shared<HwBackendGroup>();
            for (auto &device : devices) {
                auto state = std::make_shared<HwBackendState>();
                state->backend = backend;
                state->device_name = device;
                state->group = group;
                if (!func(backend, state->device_name, state->caps, state->device)) {
                    continue;
                }
                auto &caps = state->caps;
                InfoL << "GPU backend " << stateLabel(*state) << ": decode h264=" << caps.decode[0] << " h265=" << caps.decode[1]
                      << ", encode h264=" << caps.encode[0] << " h265=" << caps.encode[1];
                if (caps.decode[0] || caps.decode[1] || caps.encode[0] || caps.encode[1]) {
                    states.emplace_back(std::move(state));
                }
            }
        }
    }
    if (states.empty()) {
        InfoL << "No GPU decode/encode backend in use, decoding and encoding on the CPU";
    }
    lock_guard<mutex> lck(_mtx);
    _states = std::move(states);
    _probed = true;
}

bool HwCodecManager::probed() const {
    lock_guard<mutex> lck(_mtx);
    return _probed;
}

HwCodecSession::Ptr HwCodecManager::acquireDecode(CodecId codec, int width, int height, float fps) {
    return acquire(codec, false, hwStreamCost(width, height, fps), false);
}

HwCodecSession::Ptr HwCodecManager::acquireEncode(CodecId codec, int width, int height, float fps) {
    return acquire(codec, true, hwStreamCost(width, height, fps), false);
}

HwCodecSession::Ptr HwCodecManager::acquireEncodeOffline(CodecId codec) {
    GET_CONFIG(float, offline_cost, HwCodec::kOfflineCost);
    return acquire(codec, true, std::max(0.0f, offline_cost), true);
}

HwCodecSession::Ptr HwCodecManager::acquireDecodeOffline(CodecId codec) {
    GET_CONFIG(float, offline_cost, HwCodec::kOfflineCost);
    return acquire(codec, false, std::max(0.0f, offline_cost), true);
}

HwCodecSession::Ptr HwCodecManager::acquire(CodecId codec, bool encode, double cost, bool offline) {
    GET_CONFIG(bool, enable, HwCodec::kEnable);
    GET_CONFIG(bool, decode_on, HwCodec::kDecode);
    GET_CONFIG(bool, encode_on, HwCodec::kEncode);
    GET_CONFIG(int, max_decode, HwCodec::kMaxDecodeSessions);
    GET_CONFIG(int, max_encode, HwCodec::kMaxEncodeSessions);
    GET_CONFIG(string, decode_budget_cfg, HwCodec::kDecodeBudget);
    GET_CONFIG(string, encode_budget_cfg, HwCodec::kEncodeBudget);
    GET_CONFIG(float, offline_headroom, HwCodec::kOfflineHeadroom);

    auto idx = codecIndex(codec);
    if (idx < 0 || !enable || !(encode ? encode_on : decode_on)) {
        return nullptr;
    }
    vector<std::shared_ptr<HwBackendState>> states;
    {
        lock_guard<mutex> lck(_mtx);
        if (!_probed) {
            return nullptr;
        }
        states = _states;
    }

    auto now = getCurrentMillisecond();
    // Walk the configured order again so a hot reload of hwcodec.backends can reorder or drop backends.
    for (auto backend : configuredBackends()) {
        auto budget = budgetFor(backend, encode ? encode_budget_cfg : decode_budget_cfg);
        auto limit = offline ? budget * std::max(0.0f, std::min(1.0f, offline_headroom)) : budget;
        // Devices of the backend that can take the stream, least loaded first.
        vector<std::pair<double, std::shared_ptr<HwBackendState>>> candidates;
        for (auto &state : states) {
            if (state->backend != backend) {
                continue;
            }
            lock_guard<mutex> lck(state->mtx);
            if (!(encode ? state->caps.encode[idx] : state->caps.decode[idx])) {
                continue;
            }
            auto load = encode ? state->encode_load : state->decode_load;
            if (budget > 0 && load + cost > limit) {
                continue;
            }
            candidates.emplace_back(budget > 0 ? load / budget : load, state);
        }
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const std::pair<double, std::shared_ptr<HwBackendState>> &a,
                            const std::pair<double, std::shared_ptr<HwBackendState>> &b) { return a.first < b.first; });

        for (auto &candidate : candidates) {
            auto &state = candidate.second;
            lock_guard<mutex> lck(state->mtx);
            auto &count = encode ? state->encode_sessions : state->decode_sessions;
            auto &load = encode ? state->encode_load : state->decode_load;
            auto max = encode ? max_encode : max_decode;
            if (max > 0 && count >= (size_t)max) {
                continue;
            }
            // Re-checked under the lock: another thread may have taken the room since the sort.
            if (budget > 0 && load + cost > limit) {
                continue;
            }
            if (encode) {
                lock_guard<mutex> lck2(state->group->mtx);
                auto &group = *state->group;
                if (group.encode_cap && now < group.encode_cap_until && group.encode_sessions >= group.encode_cap) {
                    continue;
                }
            }
            bool trial = false;
            if (state->breaker == kBreakerOpen) {
                if (now < state->open_until) {
                    continue;
                }
                state->breaker = kBreakerHalfOpen;
                state->trial_in_flight = false;
            }
            if (state->breaker == kBreakerHalfOpen) {
                if (state->trial_in_flight) {
                    continue;
                }
                trial = state->trial_in_flight = true;
            }
            ++count;
            load += cost;
            if (encode) {
                lock_guard<mutex> lck2(state->group->mtx);
                ++state->group->encode_sessions;
            }
            return std::make_shared<HwCodecSession>(state, encode, trial, cost);
        }
    }
    return nullptr;
}

void HwCodecManager::addFallback() {
    ++_fallbacks;
}

size_t HwCodecManager::hwDecoderCount() const {
    size_t ret = 0;
    lock_guard<mutex> lck(_mtx);
    for (auto &state : _states) {
        lock_guard<mutex> lck2(state->mtx);
        ret += state->decode_sessions;
    }
    return ret;
}

size_t HwCodecManager::hwEncoderCount() const {
    size_t ret = 0;
    lock_guard<mutex> lck(_mtx);
    for (auto &state : _states) {
        lock_guard<mutex> lck2(state->mtx);
        ret += state->encode_sessions;
    }
    return ret;
}

uint64_t HwCodecManager::fallbackCount() const {
    return _fallbacks.load();
}

string HwCodecManager::backends() const {
    string ret;
    HwBackend last = HwBackend::None;
    lock_guard<mutex> lck(_mtx);
    for (auto &state : _states) {
        // Devices of one backend are adjacent: list the backend once.
        if (state->backend == last) {
            continue;
        }
        last = state->backend;
        if (!ret.empty()) {
            ret += ",";
        }
        ret += hwBackendName(state->backend);
    }
    return ret;
}

string HwCodecManager::loads() const {
    GET_CONFIG(string, decode_budget_cfg, HwCodec::kDecodeBudget);
    GET_CONFIG(string, encode_budget_cfg, HwCodec::kEncodeBudget);
    auto format = [](double load, double budget) {
        auto ret = to_string((int)(load + 0.5));
        return budget > 0 ? ret + "/" + to_string((int)(budget + 0.5)) : ret;
    };
    string ret;
    lock_guard<mutex> lck(_mtx);
    for (auto &state : _states) {
        lock_guard<mutex> lck2(state->mtx);
        if (!ret.empty()) {
            ret += ",";
        }
        ret += stateLabel(*state) + " dec=" + format(state->decode_load, budgetFor(state->backend, decode_budget_cfg))
            + " enc=" + format(state->encode_load, budgetFor(state->backend, encode_budget_cfg));
    }
    return ret;
}

} // namespace mediakit

#endif // ENABLE_FFMPEG
