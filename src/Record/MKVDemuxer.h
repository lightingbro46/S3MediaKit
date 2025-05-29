#ifndef S3MEDIAKIT_MKVDEMUXER_H
#define S3MEDIAKIT_MKVDEMUXER_H
#ifdef ENABLE_MKV

#include <map>
#include "MKV.h"
#include "Extension/Track.h"
#include "Util/ResourcePool.h"

namespace mediakit {

class MKVDemuxer : public TrackSource {
public:
    using Ptr = std::shared_ptr<MKVDemuxer>;

    ~MKVDemuxer() override;

    /**
     * Open file
     * @param file mkv file path
     */
    void openMKV(const std::string &file);

    /**
     * @brief Close mkv file
     */
    void closeMKV();

    /**
     * Move timeline to a specific location
     * @param stamp_ms Expected timeline position, in milliseconds
     * @return Timeline position
     */
    int64_t seekTo(int64_t stamp_ms);

    /**
     * Read a frame of data
     * @param keyFrame Whether it is a key frame
     * @param eof Whether the file has been read completely
     * @return Frame data, may be empty
     */
    Frame::Ptr readFrame(bool &keyFrame, bool &eof);

    /**
     * Get all Track information
     * @param trackReady Whether to require the track to be ready
     * @return All Tracks
     */
    std::vector<Track::Ptr> getTracks(bool trackReady) const override;

    /**
     * Get file length
     * @return File length, in milliseconds
     */
    uint64_t getDurationMS() const;

private:
    int getAllTracks();
    void onVideoTrack(uint32_t track_id, mkv_codec_t codec, int width, int height, const void *extra, size_t bytes);
    void onAudioTrack(uint32_t track_id, mkv_codec_t codec, int channel_count, int bit_per_sample, int sample_rate, const void *extra, size_t bytes);
    Frame::Ptr makeFrame(uint32_t track_id, const toolkit::Buffer::Ptr &buf, int64_t pts, int64_t dts);

private:
    MKVFileDisk::Ptr _mkv_file;
    MKVFileDisk::Reader _mkv_reader;
    uint64_t _duration_ms = 0;
    std::unordered_map<int, Track::Ptr> _tracks;
    toolkit::ResourcePool<toolkit::BufferRaw> _buffer_pool;
};

class MultiMKVDemuxer : public TrackSource {
public:
    using Ptr = std::shared_ptr<MultiMKVDemuxer>;

    ~MultiMKVDemuxer() override = default;

    /**
     * Open mkv files in batches and treat multiple files as one mkv
     * @param file Multiple mkv file paths, separated by semicolons; or folders containing multiple mkv files
     */
    void openMKV(const std::string &file);

    /**
     * @brief Batch Close mkv Files
     */
    void closeMKV();

    /**
     * Move the overall timeline to somewhere
     * @param stamp_ms The expected overall length position of the timeline, in milliseconds
     * @return Overall length position of timeline
     */
    int64_t seekTo(int64_t stamp_ms);

    /**
     * Read a frame of data
     * @param keyFrame Is a keyframe
     * @param eof Are all files read?
     * @return Frame data, may be empty
     */
    Frame::Ptr readFrame(bool &keyFrame, bool &eof);

    /**
     * Get all the track information in the first file
     * @param trackReady Is it necessary to have track ready?
     * @return All Track information in the first file
     */
    std::vector<Track::Ptr> getTracks(bool trackReady) const override;

    /**
     * Get the total file length
     * @return Total file length, unit milliseconds
     */
    uint64_t getDurationMS() const;

private:
    std::map<int, Track::Ptr> _tracks;
    std::map<uint64_t, MKVDemuxer::Ptr>::iterator _it;
    std::map<uint64_t, MKVDemuxer::Ptr> _demuxers;
};

} // namespace mediakit

#endif // ENABLE_MKV
#endif // S3MEDIAKIT_MKVDEMUXER_H