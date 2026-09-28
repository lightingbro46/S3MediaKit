#include "ReplayTimelinePlanner.h"

#include <algorithm>

namespace mediakit {

namespace {

struct Run {
    const ReplayBlock *block;
    uint64_t begin;
    uint64_t end;
};

bool sameClipSource(const ReplayBlock *left, const ReplayBlock *right) {
    return left->source == right->source && left->stream_id == right->stream_id &&
           left->file_path == right->file_path;
}

} // namespace

ReplayMode ReplayTimelinePlanner::resolveMode(const std::string &quality,
                                              const std::string &preferred) {
    if (quality == "hi") {
        return ReplayMode::OnlyHigh;
    }
    if (quality == "lo") {
        return ReplayMode::OnlyLow;
    }
    return preferred == "lo" ? ReplayMode::AutoLow : ReplayMode::AutoHigh;
}

std::vector<Broadcast::ReplayClip> ReplayTimelinePlanner::build(
    const std::vector<ReplayBlock> &blocks,
    uint64_t start_time,
    uint64_t end_time,
    ReplayMode mode,
    const ReplayPlanOptions &options) {
    std::vector<Broadcast::ReplayClip> clips;
    if (start_time >= end_time) {
        return clips;
    }

    std::vector<uint64_t> points;
    points.push_back(start_time);
    points.push_back(end_time);
    for (const auto &block : blocks) {
        const uint64_t begin = std::max(start_time, block.start_time);
        const uint64_t end = std::min(end_time, block.end_time);
        if (begin < end) {
            points.push_back(begin);
            points.push_back(end);
        }
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());

    const int required_source = mode == ReplayMode::OnlyHigh ? 0
                              : mode == ReplayMode::OnlyLow ? 1 : -1;
    const int preferred_source = mode == ReplayMode::AutoHigh ? 0 : 1;

    // Pick a block for every elementary interval and merge equal neighbours.
    std::vector<Run> runs;
    uint64_t gap_begin = 0;
    bool in_gap = false;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const uint64_t begin = points[i];
        const uint64_t end = points[i + 1];
        const ReplayBlock *chosen = nullptr;
        for (const auto &block : blocks) {
            if (block.start_time > begin || block.end_time < end) {
                continue;
            }
            if (required_source >= 0 && block.source != required_source) {
                continue;
            }
            if (!chosen || (required_source < 0 && block.source == preferred_source &&
                            chosen->source != preferred_source)) {
                chosen = &block;
            }
        }

        // A missing interval at the requested timestamp yields an empty
        // result. Later holes up to max_gap_s are skipped (the reader waits
        // for the next clip start); a longer hole ends the replay range.
        if (!chosen) {
            if (runs.empty()) {
                break;
            }
            if (!in_gap) {
                in_gap = true;
                gap_begin = begin;
            }
            if (end - gap_begin > options.max_gap_s) {
                break;
            }
            continue;
        }
        in_gap = false;

        if (!runs.empty() && runs.back().end == begin && sameClipSource(runs.back().block, chosen)) {
            runs.back().end = end;
        } else {
            runs.push_back(Run{chosen, begin, end});
        }
    }
    // Hysteresis for Auto modes: keep a short preferred stretch on the stream
    // that is already playing when that stream's block still covers it.
    if (required_source < 0 && options.min_switch_s > 0) {
        std::vector<Run> smoothed;
        for (const auto &run : runs) {
            if (!smoothed.empty()) {
                auto &last = smoothed.back();
                const bool adjacent = last.end == run.begin;
                if (adjacent && sameClipSource(last.block, run.block)) {
                    last.end = run.end;
                    continue;
                }
                const bool short_switch = run.block->source == preferred_source &&
                                          last.block->source != preferred_source &&
                                          run.end - run.begin < options.min_switch_s;
                if (adjacent && short_switch && last.block->end_time >= run.end) {
                    last.end = run.end;
                    continue;
                }
            }
            smoothed.push_back(run);
        }
        runs.swap(smoothed);
    }

    for (const auto &run : runs) {
        Broadcast::ReplayClip clip;
        clip.source = run.block->source;
        clip.stream_id = run.block->stream_id;
        clip.file_path = run.block->file_path;
        clip.start_time = run.begin;
        clip.duration = static_cast<uint32_t>(run.end - run.begin);
        clip.file_start_time = run.block->file_start_time;
        clips.push_back(clip);
    }
    return clips;
}

} // namespace mediakit
