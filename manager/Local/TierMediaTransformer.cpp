#include "Local/TierMediaTransformer.h"

#ifdef ENABLE_TIER_STORAGE

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include <openssl/sha.h>

#include "Common/config.h"
#include "Util/mini.h"
#include "Util/File.h"
#include "Util/logger.h"
#include "Util/util.h"
#if defined(ENABLE_FFMPEG)
#include <map>
#include "Codec/HwCodec.h"
#endif

using namespace toolkit;

namespace managerkit {
namespace {

std::vector<std::string> splitComma(const std::string &value) {
    std::vector<std::string> result;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        const size_t first = item.find_first_not_of(" \t");
        const size_t last = item.find_last_not_of(" \t");
        if (first != std::string::npos)
            result.push_back(item.substr(first, last - first + 1));
    }
    return result;
}

std::string sha256Hex(const std::string &value) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), digest);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < sizeof(digest); ++i)
        out << std::setw(2) << static_cast<unsigned int>(digest[i]);
    return out.str();
}

double parseRate(const std::string &rate) {
    const size_t slash = rate.find('/');
    if (slash == std::string::npos)
        return std::atof(rate.c_str());
    const double numerator = std::atof(rate.substr(0, slash).c_str());
    const double denominator = std::atof(rate.substr(slash + 1).c_str());
    return denominator == 0.0 ? 0.0 : numerator / denominator;
}

std::string normalizedCodec(const std::string &codec) {
    std::string result = codec;
    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
    if (result == "H265" || result == "HEVC") return "H265";
    if (result == "H264" || result == "AVC") return "H264";
    return result;
}

// One ffmpeg run of transform(): the CPU encoders, preceded by the GPU of hwcodec when one is free.
struct EncodeAttempt {
    std::vector<std::string> input_args;  // before -i
    std::string filter;                    // -vf, empty for none
    std::vector<std::string> encode_args;  // -c:v and its rate control
    std::string encoder;
    int gpu_level = 0;                     // 0 CPU, 1 GPU encode, 2 GPU decode + scale + encode
};

#if defined(ENABLE_FFMPEG)
// The tier ffmpeg binary may lack an encoder or filter the in-process probe found working: after a
// failed GPU run, transcodes leave that backend (or only its GPU decode) alone for
// hwcodec.cooldownSec. The in-process breaker is not touched, as for overlay extracts.
std::mutex s_hw_off_mtx;
std::map<int, uint64_t> s_hw_off_until;

int hwOffKey(mediakit::HwBackend backend, bool decode) {
    return static_cast<int>(backend) * 2 + (decode ? 1 : 0);
}

bool hwDisabled(mediakit::HwBackend backend, bool decode) {
    std::lock_guard<std::mutex> lock(s_hw_off_mtx);
    auto it = s_hw_off_until.find(hwOffKey(backend, decode));
    return it != s_hw_off_until.end() && getCurrentMillisecond() < it->second;
}

void disableHw(mediakit::HwBackend backend, bool decode) {
    const int cooldown_sec = mINI::Instance()[mediakit::HwCodec::kCooldownSec];
    std::lock_guard<std::mutex> lock(s_hw_off_mtx);
    s_hw_off_until[hwOffKey(backend, decode)] =
        getCurrentMillisecond() + static_cast<uint64_t>(std::max(0, cooldown_sec)) * 1000;
}

bool numericDevice(const std::string &device) {
    return !device.empty() && device.find_first_not_of("0123456789") == std::string::npos;
}

// GPU encoder arguments of a session; false when its backend cannot encode from the ffmpeg command
// line here. hw_frames: the frames already are GPU surfaces (GPU decode).
bool hwEncodeArgs(const mediakit::HwCodecSession &session, mediakit::CodecId codec,
                  int bitrate_kbps, bool hw_frames, std::vector<std::string> &args) {
    const char *name = mediakit::hwEncoderName(session.backend(), codec);
    if (!name) return false;
    const std::string rate = std::to_string(bitrate_kbps) + "k";
    const std::string &device = session.deviceName();
    args = {"-c:v", name};
    switch (session.backend()) {
        case mediakit::HwBackend::Cuda:
            args.insert(args.end(), {"-preset", "p4", "-rc", "vbr"});
            if (bitrate_kbps > 0) {
                // GPU VBR overshoots more than libx264; validate() allows only 10% above the cap.
                args.insert(args.end(), {"-b:v", rate, "-maxrate", rate, "-bufsize",
                                         std::to_string(bitrate_kbps * 2) + "k"});
            } else {
                args.insert(args.end(), {"-cq", "23", "-b:v", "0"});
            }
            if (!hw_frames) args.insert(args.end(), {"-pix_fmt", "yuv420p"});
            if (numericDevice(device)) args.insert(args.end(), {"-gpu", device});
            return true;
        case mediakit::HwBackend::Vaapi:
            if (bitrate_kbps > 0) {
                args.insert(args.end(), {"-rc_mode", "VBR", "-b:v", rate, "-maxrate", rate});
            } else {
                args.insert(args.end(), {"-qp", "23"});
            }
            return true;
        case mediakit::HwBackend::Qsv:
            args.insert(args.end(), {"-preset", "veryfast"});
            if (bitrate_kbps > 0) {
                args.insert(args.end(), {"-b:v", rate, "-maxrate", rate});
            } else {
                args.insert(args.end(), {"-global_quality", "23"});
            }
            args.insert(args.end(), {"-pix_fmt", "nv12"});
            return true;
        default:
            return false;
    }
}
#endif

} // namespace

std::string tierQualityProfileFingerprint(const std::string &canonical_profile_json) {
    return sha256Hex(canonical_profile_json);
}

std::string tierTranscodeStagingJobKey(const std::string &job_id) {
    return sha256Hex(job_id).substr(0, 24);
}

FfmpegTierMediaTransformer::FfmpegTierMediaTransformer(
        const std::shared_ptr<IBlockingProcessRunner> &runner)
    : _runner(runner ? runner : std::shared_ptr<IBlockingProcessRunner>(new BlockingProcessRunner())) {}

bool FfmpegTierMediaTransformer::inspect(const std::string &path,
                                         TierMediaDescriptor &descriptor,
                                         std::string &error) {
    BlockingProcessRequest request;
    request.executable = mINI::Instance()[mediakit::Storage::kTierTranscodeFfprobeBin];
    request.argv = {"-v", "error", "-show_streams", "-show_format", "-of", "json", path};
    request.timeout_seconds = 30;
    BlockingProcessResult result = _runner->run(request);
    if (!result.success()) {
        error = result.error.empty() ? result.standard_error : result.error;
        return false;
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(result.standard_output, root, false) || !root["streams"].isArray()) {
        error = "TIER_TRANSCODE_INVALID_PROBE: ffprobe returned invalid JSON";
        return false;
    }
    bool found_video = false;
    for (Json::ArrayIndex i = 0; i < root["streams"].size(); ++i) {
        const Json::Value &stream = root["streams"][i];
        const std::string type = stream.get("codec_type", "").asString();
        if (type == "video" && !found_video) {
            found_video = true;
            descriptor.video_codec = normalizedCodec(stream.get("codec_name", "").asString());
            descriptor.width = stream.get("width", 0).asInt();
            descriptor.height = stream.get("height", 0).asInt();
            descriptor.fps = parseRate(stream.get("avg_frame_rate", "0/1").asString());
            descriptor.video_bitrate_kbps =
                std::strtoll(stream.get("bit_rate", "0").asString().c_str(), nullptr, 10) / 1000;
        } else if (type == "audio" && !descriptor.has_audio) {
            descriptor.has_audio = true;
            descriptor.audio_codec = stream.get("codec_name", "").asString();
        }
    }
    descriptor.duration_seconds = std::atof(root["format"].get("duration", "0").asString().c_str());
    if (!found_video || descriptor.width <= 0 || descriptor.height <= 0) {
        error = "TIER_TRANSCODE_INVALID_PROBE: no valid video stream";
        return false;
    }
    return true;
}

TierTransformDecision FfmpegTierMediaTransformer::plan(
        const TierMediaDescriptor &source, const TierQualityProfile &profile) const {
    TierTransformDecision decision;
    decision.target_codec = normalizedCodec(profile.video_codec);
    decision.target_width = source.width;
    decision.target_height = source.height;
    decision.target_fps = profile.max_fps > 0 && source.fps > profile.max_fps
        ? profile.max_fps : 0;
    decision.max_video_bitrate_kbps = profile.max_video_bitrate_kbps;
    // A forced transcode (fps/codec/size) must not raise the bitrate above the source.
    decision.target_video_bitrate_kbps = profile.max_video_bitrate_kbps > 0 &&
            source.video_bitrate_kbps > 0 &&
            source.video_bitrate_kbps < profile.max_video_bitrate_kbps
        ? static_cast<int>(source.video_bitrate_kbps) : profile.max_video_bitrate_kbps;

    if (profile.max_width > 0 && profile.max_height > 0 &&
        (source.width > profile.max_width || source.height > profile.max_height)) {
        const double scale = std::min(static_cast<double>(profile.max_width) / source.width,
                                      static_cast<double>(profile.max_height) / source.height);
        decision.target_width = std::max(2, static_cast<int>(std::floor(source.width * scale / 2.0)) * 2);
        decision.target_height = std::max(2, static_cast<int>(std::floor(source.height * scale / 2.0)) * 2);
    }

    const bool bitrate_exceeds = profile.max_video_bitrate_kbps > 0 &&
        (source.video_bitrate_kbps <= 0 ||
         source.video_bitrate_kbps > profile.max_video_bitrate_kbps);
    const bool requires = normalizedCodec(source.video_codec) != decision.target_codec ||
        decision.target_width != source.width || decision.target_height != source.height ||
        decision.target_fps > 0 || bitrate_exceeds;
    decision.action = requires ? TierTransformAction::TRANSCODE
                               : TierTransformAction::COPY_POLICY_NOOP;
    return decision;
}

std::vector<std::string> FfmpegTierMediaTransformer::configuredEncoders(
        const std::string &codec) const {
    const std::string key = normalizedCodec(codec) == "H265"
        ? mediakit::Storage::kTierTranscodeH265Encoders
        : mediakit::Storage::kTierTranscodeH264Encoders;
    return splitComma(mINI::Instance()[key]);
}

bool FfmpegTierMediaTransformer::smokeTestEncoder(const std::string &encoder) {
    BlockingProcessRequest request;
    request.executable = mINI::Instance()[mediakit::Storage::kTierTranscodeFfmpegBin];
    request.argv = {"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                    "color=size=64x64:rate=1", "-frames:v", "1", "-an", "-c:v",
                    encoder, "-f", "null", "-"};
    request.timeout_seconds = 15;
    return _runner->run(request).success();
}

TierTranscodeCapabilities FfmpegTierMediaTransformer::probe() {
    {
        std::lock_guard<std::mutex> lock(_capability_mutex);
        _capabilities.probing = true;
    }
    TierTranscodeCapabilities next;
    next.probing = true;
    const std::vector<std::string> h264 = configuredEncoders("H264");
    const std::vector<std::string> h265 = configuredEncoders("H265");
    for (size_t i = 0; i < h264.size(); ++i) {
        if (smokeTestEncoder(h264[i])) next.h264_encoders.push_back(h264[i]);
    }
    for (size_t i = 0; i < h265.size(); ++i) {
        if (smokeTestEncoder(h265[i])) next.h265_encoders.push_back(h265[i]);
    }
    next.h264_available = !next.h264_encoders.empty();
    next.h265_available = !next.h265_encoders.empty();
    next.probing = false;
    next.probed = true;
    {
        std::lock_guard<std::mutex> lock(_capability_mutex);
        _capabilities = next;
    }
    return next;
}

TierTranscodeCapabilities FfmpegTierMediaTransformer::snapshot() const {
    std::lock_guard<std::mutex> lock(_capability_mutex);
    return _capabilities;
}

bool FfmpegTierMediaTransformer::operational(const std::string &codec) const {
    TierTranscodeCapabilities capabilities = snapshot();
    if (normalizedCodec(codec) == "H265" ? capabilities.h265_available
                                         : capabilities.h264_available)
        return true;
#if defined(ENABLE_FFMPEG)
    // No CPU encoder in the tier ffmpeg: a GPU of hwcodec can still encode (a full GPU only
    // delays the job to the CPU encoders, which then fail and count as transform failures).
    return mINI::Instance()[mediakit::Storage::kTierTranscodeHwaccel].as<bool>() &&
        !mediakit::HwCodecManager::Instance().backends().empty();
#else
    return false;
#endif
}

std::vector<std::string> FfmpegTierMediaTransformer::cpuEncoders(const std::string &codec) const {
    // Only the encoders that passed the smoke test. Jobs use their own transformer, so take the
    // registry's probe; until one finished, try the configured list.
    TierTranscodeCapabilities capabilities = snapshot();
    if (!capabilities.probed) capabilities = TierEncoderCapabilityRegistry::Instance().snapshot();
    if (!capabilities.probed) return configuredEncoders(codec);
    return normalizedCodec(codec) == "H265" ? capabilities.h265_encoders
                                             : capabilities.h264_encoders;
}

TierEncoderCapabilityRegistry &TierEncoderCapabilityRegistry::Instance() {
    static TierEncoderCapabilityRegistry instance;
    return instance;
}

TierTranscodeCapabilities TierEncoderCapabilityRegistry::probe() {
    return _transformer.probe();
}

TierTranscodeCapabilities TierEncoderCapabilityRegistry::snapshot() const {
    return _transformer.snapshot();
}

bool TierEncoderCapabilityRegistry::operational(const std::string &codec) const {
    return _transformer.operational(codec);
}

std::string FfmpegTierMediaTransformer::stagingPath(
        const TierTransformRequest &request) const {
    const std::string root = mINI::Instance()[mediakit::Storage::kTierTranscodeStagingPath];
    return root + "/" + tierTranscodeStagingJobKey(request.job_id) + "/" +
        sha256Hex(request.stream_id).substr(0, 24) + "/" +
        sha256Hex(request.segment_path).substr(0, 32) + ".mp4.tmp";
}

TierTransformResult FfmpegTierMediaTransformer::transform(
        const TierTransformRequest &request) {
    TierTransformResult output;
    TierMediaDescriptor source;
    if (!inspect(request.source_path, source, output.error_message)) {
        output.error_code = "TIER_TRANSCODE_PROBE_FAILED";
        return output;
    }
    const TierTransformDecision decision = plan(source, request.profile);
    if (decision.action == TierTransformAction::COPY_POLICY_NOOP) {
        output.success = true;
        output.copied_without_transcode = true;
        output.output_path = request.source_path;
        output.output_bytes = static_cast<int64_t>(File::fileSize(request.source_path));
        return output;
    }

    const std::string destination = stagingPath(request);
    if (!File::create_path(destination, 0755)) {
        output.error_code = "TIER_TRANSCODE_STAGING_FAILED";
        output.error_message = "Cannot create tier transcode staging directory";
        return output;
    }

    // The mp4 muxer only takes these as-is. G.711 is re-encoded: the mov muxer would keep it,
    // but as one-byte samples that MP4Reader replays as ~8000 audio packets per second.
    const bool copy_audio = !source.has_audio || source.audio_codec == "aac" ||
        source.audio_codec == "mp3" || source.audio_codec == "opus";
    const bool rescale =
        decision.target_width != source.width || decision.target_height != source.height;
    const std::string size = std::to_string(decision.target_width) + ":" +
        std::to_string(decision.target_height);
    const std::string fps_filter =
        decision.target_fps > 0 ? "fps=" + std::to_string(decision.target_fps) : std::string();
    std::string cpu_filter = rescale ? "scale=" + size : std::string();
    if (!fps_filter.empty()) cpu_filter += (cpu_filter.empty() ? "" : ",") + fps_filter;

    std::vector<EncodeAttempt> attempts;
#if defined(ENABLE_FFMPEG)
    // GPU first: hwcodec picks the backend and card and counts the job in its budget. ffmpeg
    // encodes as fast as it can, so the sessions are offline ones and leave room for live streams.
    mediakit::HwCodecSession::Ptr hw_encode;
    mediakit::HwCodecSession::Ptr hw_decode;
    if (mINI::Instance()[mediakit::Storage::kTierTranscodeHwaccel].as<bool>()) {
        const mediakit::CodecId target =
            decision.target_codec == "H265" ? mediakit::CodecH265 : mediakit::CodecH264;
        hw_encode = mediakit::HwCodecManager::Instance().acquireEncodeOffline(target);
        if (hw_encode && hwDisabled(hw_encode->backend(), false)) hw_encode.reset();
        EncodeAttempt encode_only;
        encode_only.gpu_level = 1;
        if (hw_encode && hwEncodeArgs(*hw_encode, target, decision.target_video_bitrate_kbps,
                                      false, encode_only.encode_args)) {
            encode_only.encoder = encode_only.encode_args[1];
            encode_only.filter = cpu_filter;
            if (hw_encode->backend() == mediakit::HwBackend::Vaapi) {
                const std::string &device = hw_encode->deviceName();
                encode_only.input_args = {"-init_hw_device",
                                          "vaapi=gpu" + (device.empty() ? "" : ":" + device),
                                          "-filter_hw_device", "gpu"};
                encode_only.filter += (encode_only.filter.empty() ? "" : ",") +
                    std::string("format=nv12,hwupload");
            }
        } else {
            hw_encode.reset();
        }

        // Decode and scale on the same card too (cuda, vaapi): frames never reach system memory.
        const std::string source_codec = normalizedCodec(source.video_codec);
        const bool gpu_decodable = source_codec == "H264" || source_codec == "H265";
        if (hw_encode && gpu_decodable &&
            (hw_encode->backend() == mediakit::HwBackend::Cuda ||
             hw_encode->backend() == mediakit::HwBackend::Vaapi) &&
            !hwDisabled(hw_encode->backend(), true)) {
            hw_decode = mediakit::HwCodecManager::Instance().acquireDecodeOffline(
                source_codec == "H265" ? mediakit::CodecH265 : mediakit::CodecH264);
            // GPU surfaces do not cross cards: another backend or card means encode only.
            if (hw_decode && (hw_decode->backend() != hw_encode->backend() ||
                              hw_decode->deviceName() != hw_encode->deviceName()))
                hw_decode.reset();
        }
        if (hw_decode) {
            EncodeAttempt full;
            full.gpu_level = 2;
            hwEncodeArgs(*hw_encode, target, decision.target_video_bitrate_kbps, true,
                         full.encode_args);
            full.encoder = full.encode_args[1];
            const std::string &device = hw_decode->deviceName();
            // fps first: frames are dropped before they are scaled.
            full.filter = fps_filter;
            if (hw_decode->backend() == mediakit::HwBackend::Cuda) {
                full.input_args = {"-hwaccel", "cuda", "-hwaccel_output_format", "cuda"};
                if (numericDevice(device))
                    full.input_args.insert(full.input_args.end(), {"-hwaccel_device", device});
                // scale_cuda of FFmpeg 4.4 has no format option; a 10-bit source then fails here
                // (h264_nvenc takes no p010) and is redone with encode only.
                if (rescale)
                    full.filter += (full.filter.empty() ? "" : ",") + std::string("scale_cuda=") + size;
            } else {
                full.input_args = {"-hwaccel", "vaapi", "-hwaccel_output_format", "vaapi"};
                if (!device.empty())
                    full.input_args.insert(full.input_args.end(), {"-hwaccel_device", device});
                // Always scale_vaapi: it also turns a 10-bit source into nv12 for the encoder.
                full.filter += (full.filter.empty() ? "" : ",") + std::string("scale_vaapi=") +
                    (rescale ? "w=" + std::to_string(decision.target_width) + ":h=" +
                                   std::to_string(decision.target_height) + ":"
                             : std::string()) +
                    "format=nv12";
            }
            attempts.push_back(full);
        }
        if (hw_encode) attempts.push_back(encode_only);
    }
#endif
    const std::vector<std::string> encoders = cpuEncoders(decision.target_codec);
    for (size_t i = 0; i < encoders.size(); ++i) {
        EncodeAttempt cpu;
        cpu.encoder = encoders[i];
        cpu.filter = cpu_filter;
        cpu.encode_args = {"-c:v", encoders[i]};
        if (decision.target_video_bitrate_kbps > 0) {
            cpu.encode_args.push_back("-b:v");
            cpu.encode_args.push_back(std::to_string(decision.target_video_bitrate_kbps) + "k");
        }
        attempts.push_back(cpu);
    }

    for (size_t i = 0; i < attempts.size(); ++i) {
        const EncodeAttempt &attempt = attempts[i];
        std::vector<std::string> args = {"-hide_banner", "-loglevel", "error", "-y"};
        args.insert(args.end(), attempt.input_args.begin(), attempt.input_args.end());
        args.insert(args.end(), {"-i", request.source_path, "-map", "0:v:0", "-map", "0:a:0?"});
        args.insert(args.end(), attempt.encode_args.begin(), attempt.encode_args.end());
        if (!attempt.filter.empty()) {
            args.push_back("-vf"); args.push_back(attempt.filter);
        }
        // No B-frames: replay through the server expects pts == dts. Keyframe every ~2 s
        // so seeking inside a segment stays fine-grained (encoder default keyint is 250).
        args.push_back("-bf"); args.push_back("0");
        const double fps = decision.target_fps > 0 ? decision.target_fps : source.fps;
        if (fps > 0) {
            args.push_back("-g");
            args.push_back(std::to_string(std::max(1L, std::lround(fps * 2))));
        }
        args.push_back("-c:a");
        if (copy_audio) {
            args.push_back("copy");
        } else {
            args.push_back("aac"); args.push_back("-b:a"); args.push_back("32k");
        }
        args.push_back("-movflags"); args.push_back("+faststart");
        args.push_back("-f"); args.push_back("mp4");
        args.push_back(destination);

        BlockingProcessRequest process;
        process.executable = mINI::Instance()[mediakit::Storage::kTierTranscodeFfmpegBin];
        process.argv = args;
        process.timeout_seconds = mINI::Instance()[mediakit::Storage::kTierTranscodeTimeoutSeconds];
        process.cancellation = request.cancellation;
        BlockingProcessResult result = _runner->run(process);
        std::string validation_error;
        const bool ran = result.success();
        if (!ran || !validate(destination, decision, validation_error)) {
            output.error_message = !ran
                ? (result.error.empty() ? result.standard_error : result.error)
                : validation_error;
            std::remove(destination.c_str());
            if (request.cancellation && request.cancellation->cancelled()) break;
#if defined(ENABLE_FFMPEG)
            if (attempt.gpu_level == 2) {
                WarnL << "Tier transcode: GPU decode on " << hw_decode->backendName()
                      << " failed, retrying with GPU encode only: " << output.error_message;
                disableHw(hw_decode->backend(), true);
                hw_decode.reset();
            } else if (attempt.gpu_level == 1) {
                WarnL << "Tier transcode: GPU encoder " << attempt.encoder
                      << " failed, retrying on the CPU: " << output.error_message;
                disableHw(hw_encode->backend(), false);
                mediakit::HwCodecManager::Instance().addFallback();
                hw_encode.reset();
            }
#endif
            continue;
        }
#if defined(ENABLE_FFMPEG)
        if (attempt.gpu_level >= 1) hw_encode->reportSuccess();
        if (attempt.gpu_level == 2) hw_decode->reportSuccess();
#endif
        const int64_t source_bytes = static_cast<int64_t>(File::fileSize(request.source_path));
        const int64_t output_bytes = static_cast<int64_t>(File::fileSize(destination));
        if (output_bytes >= source_bytes &&
            normalizedCodec(source.video_codec) == decision.target_codec) {
            // Reducing quality must not grow the file; keep the original instead.
            std::remove(destination.c_str());
            output.success = true;
            output.copied_without_transcode = true;
            output.output_path = request.source_path;
            output.output_bytes = source_bytes;
            output.error_message.clear();
            return output;
        }
        output.success = true;
        output.output_path = destination;
        output.output_bytes = output_bytes;
        output.encoder_backend = attempt.encoder;
        return output;
    }
    output.error_code = request.cancellation && request.cancellation->cancelled()
        ? "TIER_TRANSCODE_CANCELLED" : "TIER_TRANSCODE_ENCODER_FAILED";
    std::remove(destination.c_str());
    if (output.error_message.empty()) output.error_message = "No configured encoder succeeded";
    return output;
}

bool FfmpegTierMediaTransformer::validate(const std::string &path,
                                          const TierTransformDecision &decision,
                                          std::string &error) {
    if (!File::fileExist(path) || File::fileSize(path) == 0) {
        error = "TIER_TRANSCODE_INVALID_OUTPUT: output is missing or empty";
        return false;
    }
    TierMediaDescriptor descriptor;
    if (!inspect(path, descriptor, error))
        return false;
    if (normalizedCodec(descriptor.video_codec) != decision.target_codec ||
        descriptor.width > decision.target_width || descriptor.height > decision.target_height ||
        (decision.target_fps > 0 && descriptor.fps > decision.target_fps + 0.1) ||
        (decision.max_video_bitrate_kbps > 0 && descriptor.video_bitrate_kbps > 0 &&
         descriptor.video_bitrate_kbps >
             static_cast<int64_t>(decision.max_video_bitrate_kbps * 1.10))) {
        error = "TIER_TRANSCODE_INVALID_OUTPUT: output exceeds requested profile";
        return false;
    }
    return true;
}

} // namespace managerkit

#endif // ENABLE_TIER_STORAGE
