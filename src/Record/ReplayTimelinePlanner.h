#ifndef S3MEDIAKIT_REPLAYTIMELINEPLANNER_H
#define S3MEDIAKIT_REPLAYTIMELINEPLANNER_H

#include <cstdint>
#include <string>
#include <vector>

#include "Common/config.h"

namespace mediakit {

enum class ReplayMode {
    OnlyLow,
    OnlyHigh,
    AutoHigh,
    AutoLow,
};

struct ReplayBlock {
    int source = 0; // 0 = MainStream, 1 = SubStream
    std::string stream_id;
    std::string file_path;
    uint64_t start_time = 0;
    uint64_t end_time = 0;
    uint64_t file_start_time = 0;
};

struct ReplayPlanOptions {
    // A hole of at most this many seconds between recorded blocks is skipped;
    // a longer hole ends the replay range.
    uint64_t max_gap_s = 2;
    // In Auto modes a stretch on the preferred stream shorter than this stays
    // on the current stream when that stream still covers it, so short holes
    // in the preferred stream do not reset the output tracks twice.
    uint64_t min_switch_s = 5;
};

class ReplayTimelinePlanner {
public:
    static ReplayMode resolveMode(const std::string &quality, const std::string &preferred);

    static std::vector<Broadcast::ReplayClip> build(const std::vector<ReplayBlock> &blocks,
                                                    uint64_t start_time,
                                                    uint64_t end_time,
                                                    ReplayMode mode,
                                                    const ReplayPlanOptions &options = ReplayPlanOptions());
};

} // namespace mediakit

#endif // S3MEDIAKIT_REPLAYTIMELINEPLANNER_H
