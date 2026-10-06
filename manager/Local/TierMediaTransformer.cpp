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
#include "Util/util.h"

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
        } else if (type == "audio") {
            descriptor.has_audio = true;
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
    decision.target_video_bitrate_kbps = profile.max_video_bitrate_kbps;

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
    return normalizedCodec(codec) == "H265" ? capabilities.h265_available
                                             : capabilities.h264_available;
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

    std::vector<std::string> encoders = configuredEncoders(decision.target_codec);
    for (size_t i = 0; i < encoders.size(); ++i) {
        std::vector<std::string> args = {"-hide_banner", "-loglevel", "error", "-y",
                                         "-i", request.source_path, "-map", "0:v:0",
                                         "-map", "0:a:0?", "-c:v", encoders[i]};
        std::vector<std::string> filters;
        if (decision.target_width != source.width || decision.target_height != source.height) {
            filters.push_back("scale=" + std::to_string(decision.target_width) + ":" +
                              std::to_string(decision.target_height));
        }
        if (decision.target_fps > 0)
            filters.push_back("fps=" + std::to_string(decision.target_fps));
        if (!filters.empty()) {
            std::string joined = filters[0];
            for (size_t j = 1; j < filters.size(); ++j) joined += "," + filters[j];
            args.push_back("-vf"); args.push_back(joined);
        }
        if (decision.target_video_bitrate_kbps > 0) {
            args.push_back("-b:v");
            args.push_back(std::to_string(decision.target_video_bitrate_kbps) + "k");
        }
        args.push_back("-c:a"); args.push_back("copy");
        args.push_back("-movflags"); args.push_back("+faststart");
        args.push_back("-f"); args.push_back("mp4");
        args.push_back(destination);

        BlockingProcessRequest process;
        process.executable = mINI::Instance()[mediakit::Storage::kTierTranscodeFfmpegBin];
        process.argv = args;
        process.timeout_seconds = mINI::Instance()[mediakit::Storage::kTierTranscodeTimeoutSeconds];
        process.cancellation = request.cancellation;
        BlockingProcessResult result = _runner->run(process);
        if (!result.success()) {
            output.error_message = result.error.empty() ? result.standard_error : result.error;
            std::remove(destination.c_str());
            continue;
        }
        std::string validation_error;
        if (!validate(destination, decision, validation_error)) {
            output.error_message = validation_error;
            std::remove(destination.c_str());
            continue;
        }
        output.success = true;
        output.output_path = destination;
        output.output_bytes = static_cast<int64_t>(File::fileSize(destination));
        output.encoder_backend = encoders[i];
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
        (decision.target_video_bitrate_kbps > 0 && descriptor.video_bitrate_kbps > 0 &&
         descriptor.video_bitrate_kbps >
             static_cast<int64_t>(decision.target_video_bitrate_kbps * 1.10))) {
        error = "TIER_TRANSCODE_INVALID_OUTPUT: output exceeds requested profile";
        return false;
    }
    return true;
}

} // namespace managerkit

#endif // ENABLE_TIER_STORAGE
