#ifndef LOCAL_TIERMEDIATRANSFORMER_H
#define LOCAL_TIERMEDIATRANSFORMER_H

#ifdef ENABLE_TIER_STORAGE

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Common/BlockingProcessRunner.h"
#include "Local/StorageTier.h"

namespace managerkit {

struct TierMediaDescriptor {
    std::string video_codec;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int64_t video_bitrate_kbps = 0;
    bool has_audio = false;
    double duration_seconds = 0.0;
};

enum class TierTransformAction {
    COPY_POLICY_NOOP,
    TRANSCODE
};

struct TierTransformDecision {
    TierTransformAction action = TierTransformAction::COPY_POLICY_NOOP;
    int target_width = 0;
    int target_height = 0;
    int target_fps = 0;
    int target_video_bitrate_kbps = 0;
    std::string target_codec;
};

struct TierTransformRequest {
    std::string job_id;
    std::string stream_id;
    std::string segment_path;
    std::string source_path;
    TierQualityProfile profile;
    std::shared_ptr<BlockingProcessCancellation> cancellation;
};

struct TierTransformResult {
    bool success = false;
    bool copied_without_transcode = false;
    std::string output_path;
    int64_t output_bytes = 0;
    std::string encoder_backend;
    std::string error_code;
    std::string error_message;
};

struct TierTranscodeCapabilities {
    bool probing = false;
    bool h264_available = false;
    bool h265_available = false;
    std::vector<std::string> h264_encoders;
    std::vector<std::string> h265_encoders;
};

// SHA-256 of the canonical compact TierQualityProfile JSON persisted on the
// parent job. This is stable across retries and is never derived from raw FE JSON.
std::string tierQualityProfileFingerprint(const std::string &canonical_profile_json);

// Stable directory component used to isolate staging files owned by one job.
std::string tierTranscodeStagingJobKey(const std::string &job_id);

class ITierMediaTransformer {
public:
    virtual ~ITierMediaTransformer() {}
    virtual TierTransformResult transform(const TierTransformRequest &request) = 0;
};

class FfmpegTierMediaTransformer : public ITierMediaTransformer {
public:
    explicit FfmpegTierMediaTransformer(
        const std::shared_ptr<IBlockingProcessRunner> &runner =
            std::shared_ptr<IBlockingProcessRunner>());

    bool inspect(const std::string &path, TierMediaDescriptor &descriptor,
                 std::string &error);
    TierTransformDecision plan(const TierMediaDescriptor &source,
                               const TierQualityProfile &profile) const;
    TierTransformResult transform(const TierTransformRequest &request) override;
    bool validate(const std::string &path, const TierTransformDecision &decision,
                  std::string &error);
    TierTranscodeCapabilities probe();
    TierTranscodeCapabilities snapshot() const;
    bool operational(const std::string &codec) const;

private:
    std::vector<std::string> configuredEncoders(const std::string &codec) const;
    bool smokeTestEncoder(const std::string &encoder);
    std::string stagingPath(const TierTransformRequest &request) const;

    std::shared_ptr<IBlockingProcessRunner> _runner;
    mutable std::mutex _capability_mutex;
    TierTranscodeCapabilities _capabilities;
};

class TierEncoderCapabilityRegistry {
public:
    static TierEncoderCapabilityRegistry &Instance();
    TierTranscodeCapabilities probe();
    TierTranscodeCapabilities snapshot() const;
    bool operational(const std::string &codec) const;

private:
    TierEncoderCapabilityRegistry() = default;
    FfmpegTierMediaTransformer _transformer;
};

} // namespace managerkit

#endif // ENABLE_TIER_STORAGE
#endif // LOCAL_TIERMEDIATRANSFORMER_H
