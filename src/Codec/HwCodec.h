#ifndef S3MEDIAKIT_HWCODEC_H
#define S3MEDIAKIT_HWCODEC_H

#if defined(ENABLE_FFMPEG)

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "Extension/Frame.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "libavcodec/avcodec.h"
#include "libavutil/buffer.h"
#include "libavutil/hwcontext.h"
#ifdef __cplusplus
}
#endif

namespace mediakit {

/**
 * GPU decode/encode for the in-process FFmpeg pipeline (motion, transcode, video stack).
 *
 * HwCodecManager probes the configured backends once (hwcodec.backends), keeps one shared FFmpeg
 * device context per working backend and hands out HwCodecSession objects: a session is one GPU
 * decoder or encoder and holds a slot of the backend's session budget until it is destroyed.
 * Until the probe has finished, or when no backend works, acquire*() returns null and the caller
 * uses the CPU exactly as before.
 *
 * Each backend has a circuit breaker: after hwcodec.failThreshold failed sessions it is skipped
 * for hwcodec.cooldownSec, then one trial session is allowed; a trial that works closes it again.
 */
enum class HwBackend : int {
    None = 0,
    Cuda,         // NVIDIA, Linux + Windows: NVDEC hwaccel, nvenc
    Vaapi,        // Intel/AMD Linux: vaapi hwaccel + encoders
    Qsv,          // Intel Linux + Windows: encode only (decode Intel with vaapi/d3d11va)
    Videotoolbox, // macOS/iOS: videotoolbox hwaccel + encoders
    D3d11va,      // Windows, any vendor: decode only
    Dxva2,        // older Windows: decode only
    Amf,          // AMD Windows: encode only, no device context
    Mediacodec,   // Android: encode only here (decoding uses the h264/hevc_mediacodec decoders)
};

const char *hwBackendName(HwBackend backend);
HwBackend hwBackendFromName(const std::string &name);

struct HwBackendCaps {
    // Index 0 = H264, 1 = H265.
    bool decode[2] = { false, false };
    bool encode[2] = { false, false };
    // Surface format the native decoder outputs with this backend (AV_PIX_FMT_CUDA, AV_PIX_FMT_VAAPI).
    AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
};

struct HwBackendState;

class HwCodecSession {
public:
    using Ptr = std::shared_ptr<HwCodecSession>;

    HwCodecSession(std::shared_ptr<HwBackendState> state, bool encode, bool trial);
    ~HwCodecSession();

    HwBackend backend() const;
    const char *backendName() const;
    // Shared device context, borrowed: av_buffer_ref() it to keep it. May be null in tests.
    AVBufferRef *device() const;
    AVPixelFormat hwPixFmt() const;

    // The session produced output: resets the backend's failure count and closes a half-open breaker.
    void reportSuccess();
    /**
     * The session failed and its owner falls back to the CPU. Counted once per session.
     * @param open_failure the encoder could not be opened. When other encoders of this backend are
     *        running this is taken as the device session limit, not as a broken backend: new encoders
     *        are capped at the running count for hwcodec.cooldownSec.
     */
    void reportFailure(bool open_failure = false);

private:
    std::shared_ptr<HwBackendState> _state;
    bool _encode;
    bool _trial;
    bool _succeeded = false;
    bool _failed = false;
};

class HwCodecManager {
public:
    /**
     * Probes one backend. Returns false when it is unusable; otherwise fills caps and hands over one
     * reference of the device context (may stay null in tests).
     */
    using ProbeFunc = std::function<bool(HwBackend backend, const std::string &device, HwBackendCaps &caps,
                                         AVBufferRef *&device_ctx)>;

    static HwCodecManager &Instance();

    HwCodecManager();
    ~HwCodecManager();

    /**
     * Probes the backends of hwcodec.backends with FFmpeg. In a child process (runHwTestsInChildren)
     * it creates the device, decodes a few 256x144 frames made by libx264/libx265 on the GPU and
     * encodes a few on the GPU, per codec; only what passed is used, and only then is the device
     * created in this process. Blocking (creating a CUDA context takes 100-500 ms):
     * run it on a BlockingExecutors pool, never on a poller. Calling it again replaces the result;
     * sessions already handed out keep their old device.
     */
    void probe();
    void probe(const ProbeFunc &func);
    bool probed() const;

    // A GPU session for decoding/encoding codec (H264/H265 only), or null to use the CPU.
    HwCodecSession::Ptr acquireDecode(CodecId codec);
    HwCodecSession::Ptr acquireEncode(CodecId codec);

    // A decoder/encoder that had a GPU session went back to the CPU.
    void addFallback();

    size_t hwDecoderCount() const;
    size_t hwEncoderCount() const;
    uint64_t fallbackCount() const;
    // Comma separated names of the probed, working backends (empty before the probe or without GPU).
    std::string backends() const;

private:
    HwCodecSession::Ptr acquire(CodecId codec, bool encode);

private:
    mutable std::mutex _mtx;
    bool _probed = false;
    std::vector<std::shared_ptr<HwBackendState>> _states;
    std::atomic<uint64_t> _fallbacks { 0 };
};

struct HwTestResult {
    bool ok = false;
    std::string detail;
};
using HwChildTest = std::function<bool(std::string &detail)>;

/**
 * Runs GPU tests in forked child processes so a driver that crashes or hangs cannot take the server
 * down. setup runs first in every child (it creates the device); when it fails nothing else runs.
 * Each test result is sent to the parent as soon as it is known. A test whose child crashes or
 * exceeds timeout_ms is recorded as failed with that reason, and the remaining tests run in a new child.
 * results gets "setup" plus one entry per test name. Returns false when no child can be started
 * (no fork on Windows); the caller then has to test in-process.
 */
bool runHwTestsInChildren(const HwChildTest &setup, const std::vector<std::pair<std::string, HwChildTest>> &tests,
                          int timeout_ms, std::map<std::string, HwTestResult> &results);

/**
 * Encoder name of a backend: h264_nvenc/hevc_nvenc, h264_vaapi/hevc_vaapi, h264_qsv/hevc_qsv.
 */
const char *hwEncoderName(HwBackend backend, CodecId codec);

/**
 * Opens the GPU encoder of backend. Output keeps the FFmpegEncoder invariants: Annex-B SPS/PPS
 * in-band on every IDR (no global header), no B-frames, a frame with pict_type I becomes an IDR.
 * @param input_fmt  set to the RAM format frames must be in (YUV420P for nvenc, NV12 otherwise)
 * @param upload     set when frames must be uploaded into ctx->hw_frames_ctx before sending (vaapi)
 * @return the opened context, or null (reason in err)
 */
std::shared_ptr<AVCodecContext> openHwEncoder(HwBackend backend, CodecId codec, AVBufferRef *device,
                                              int width, int height, int fps, int bitrate, int gop,
                                              AVPixelFormat &input_fmt, bool &upload, std::string &err);

} // namespace mediakit

#endif // ENABLE_FFMPEG
#endif // S3MEDIAKIT_HWCODEC_H
