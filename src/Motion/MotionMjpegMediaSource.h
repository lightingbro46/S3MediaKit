#ifndef MOTION_MOTIONMJPEGMEDIASOURCE_H
#define MOTION_MOTIONMJPEGMEDIASOURCE_H

#ifdef ENABLE_MOTION

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "Mjpeg/MjpegMediaSource.h"
#include "Common/config.h"
#include "Transcode/OverlayPrivacyUtils.h"

namespace mediakit {

/**
 * A single encoded MJPEG frame produced by MotionProcessor.
 * Used as the FrameType for MjpegMediaSource<MotionJpegFrame>.
 */
struct MotionJpegFrame {
    using Ptr = std::shared_ptr<MotionJpegFrame>;

    bool     motion       = false; // true if motion was detected in this frame
    uint64_t stamp_ms     = 0;     // PTS in milliseconds
    int      active_cells = 0;     // number of motion-active grid cells
    // JPEG-encoded bytes; nullptr if encoding failed
    std::shared_ptr<std::vector<uint8_t>> jpeg;
};

/**
 * Live MJPEG media source for motion-analysis streams.
 * Extends MjpegMediaSource<MotionJpegFrame> with motion-specific overlay flags.
 *
 * Lifecycle: same as MjpegMediaSource — call startStream() after make_shared.
 *
 * Clients connect via:
 *   GET /media/{app}/{stream}.motion.mjpeg?overlay_motion=0|1&overlay_roi=0|1
 *
 * Viewers without a view overlay read the base ring (getRing()). Viewers that
 * require a watermark/privacy mask read a per-policy overlay variant ring.
 */
class MotionMjpegMediaSource final : public MjpegMediaSource<MotionJpegFrame> {
public:
    using Ptr      = std::shared_ptr<MotionMjpegMediaSource>;
    using RingType = MjpegMediaSource<MotionJpegFrame>::RingType;

    explicit MotionMjpegMediaSource(const MediaTuple &tuple, int ring_size = 10)
        : MjpegMediaSource<MotionJpegFrame>(MOTION_MJPEG_SCHEMA, tuple, ring_size) {}

    ~MotionMjpegMediaSource() override = default;

    /**
     * Toggle overlays drawn on each live MJPEG frame (thread-safe).
     * Setting is shared across all connected clients — last caller wins.
     *
     * @param motion  Highlight motion-active grid cells
     * @param roi     Draw ROI mask border
     */
    void setOverlay(bool motion, bool roi) {
        _overlay_motion.store(motion);
        _overlay_roi.store(roi);
    }

    bool overlayMotion() const { return _overlay_motion.load(); }
    bool overlayRoi()    const { return _overlay_roi.load();    }

    /**
     * Frames rendered with one viewer overlay policy (watermark and/or privacy mask).
     * Viewers whose policy tag matches share the variant. Immutable once created,
     * except for the ring.
     */
    struct OverlayVariant {
        using Ptr = std::shared_ptr<OverlayVariant>;
        std::string tag;
        std::vector<OverlayComponent> components;
        OverlayBuildOptions options;
        RingType::Ptr ring;
    };
    using OverlayVariantMap = std::map<std::string, OverlayVariant::Ptr>;

    /**
     * Attach a reader to the overlay variant identified by @p tag, creating the
     * variant when no viewer uses it yet. Variants are not registered as media
     * sources, so they are reachable only through the view overlay policy.
     * Must be called on @p poller. The variant is dropped with its last reader.
     *
     * @return nullptr when creating the variant would exceed @p max_variants.
     */
    RingType::RingReader::Ptr attachOverlayVariant(const std::string &tag,
                                                   const std::vector<OverlayComponent> &components,
                                                   const OverlayBuildOptions &options,
                                                   const toolkit::EventPoller::Ptr &poller,
                                                   size_t max_variants) {
        std::lock_guard<std::recursive_mutex> lck(_variant_mtx);
        auto it = _variants.find(tag);
        if (it == _variants.end()) {
            if (_variants.size() >= max_variants) {
                return nullptr;
            }
            auto variant = std::make_shared<OverlayVariant>();
            variant->tag = tag;
            variant->components = components;
            variant->options = options;
            std::weak_ptr<MotionMjpegMediaSource> weak_self =
                std::static_pointer_cast<MotionMjpegMediaSource>(shared_from_this());
            variant->ring = std::make_shared<RingType>(_ring_size, [weak_self, tag](int) {
                if (auto s = weak_self.lock()) s->onVariantReaderChanged(tag);
            });
            it = _variants.emplace(tag, variant).first;
        }
        // attach() reports the new reader synchronously; the mutex is recursive for that.
        return it->second->ring->attach(poller);
    }

    /** Snapshot of the overlay variants that currently have viewers. */
    OverlayVariantMap overlayVariants() const {
        std::lock_guard<std::recursive_mutex> lck(_variant_mtx);
        return _variants;
    }

    int readerCount() override {
        int count = MjpegMediaSource<MotionJpegFrame>::readerCount();
        std::lock_guard<std::recursive_mutex> lck(_variant_mtx);
        for (auto &pr : _variants) {
            count += pr.second->ring->readerCount();
        }
        return count;
    }

    void getPlayerList(
        const std::function<void(const std::list<toolkit::Any> &)> &cb,
        const std::function<toolkit::Any(toolkit::Any &&)>         &on_change) override
    {
        std::vector<RingType::Ptr> rings;
        if (_ring) rings.push_back(_ring);
        {
            std::lock_guard<std::recursive_mutex> lck(_variant_mtx);
            for (auto &pr : _variants) rings.push_back(pr.second->ring);
        }
        auto players = std::make_shared<std::list<toolkit::Any>>();
        auto mtx = std::make_shared<std::mutex>();
        // Fires once every ring has delivered its list.
        std::shared_ptr<void> on_finished(nullptr, [cb, players](void *) { cb(*players); });
        for (auto &ring : rings) {
            ring->getInfoList([players, mtx, on_finished](std::list<toolkit::Any> &lst) {
                std::lock_guard<std::mutex> lck(*mtx);
                players->insert(players->end(), lst.begin(), lst.end());
            }, on_change);
        }
    }

private:
    void onVariantReaderChanged(const std::string &tag) {
        {
            std::lock_guard<std::recursive_mutex> lck(_variant_mtx);
            auto it = _variants.find(tag);
            // Not yet inserted (ring constructor) or already dropped.
            if (it == _variants.end()) return;
            if (it->second->ring->readerCount() == 0) {
                _variants.erase(it);
            }
        }
        onReaderChanged(readerCount());
    }

private:
    std::atomic<bool> _overlay_motion{false};
    std::atomic<bool> _overlay_roi{false};
    mutable std::recursive_mutex _variant_mtx;
    OverlayVariantMap _variants;
};

} // namespace mediakit

#endif // MOTION_MOTIONMJPEGMEDIASOURCE_H

#endif // ENABLE_MOTION
