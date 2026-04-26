#ifndef S3MEDIAKIT_MP4MUXER_H
#define S3MEDIAKIT_MP4MUXER_H

#if defined(ENABLE_MP4)

#include "Common/MediaSink.h"
#include "Common/Stamp.h"
#include "MP4.h"

namespace mediakit {

class MP4MuxerInterface : public MediaSinkInterface {
public:

    /**
     * Add tracks that are in ready state
     */
    bool addTrack(const Track::Ptr &track) override;

    /**
     * Input frame
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * Reset all tracks
     */
    void resetTracks() override;

    /**
     * Refresh all frame cache output
     */
    void flush() override;

    /**
     * Whether it contains video
     */
    bool haveVideo() const;

    /**
     * Save fmp4 fragment
     */
    void saveSegment();

    /**
     * Create new fragment
     */
    void initSegment();

    /**
     * Get mp4 duration, in milliseconds
     */
    uint64_t getDuration() const;

protected:
    virtual MP4FileIO::Writer createWriter() = 0;

    /**
     * Seed the relative stamp of all tracks with 'offset' (milliseconds).
     * Called after resetTracks()+addTrackCompleted() for VOD to ensure timestamps
     * continue from where the previous segment left off rather than restarting at 0.
     * The Stamp's first deltaStamp() call returns 0 (initialisation rule), so the
     * pre-seeded _relative_stamp is used verbatim for the first frame and subsequent
     * frames accumulate normally from there.
     */
    void seedStampOffsets(int64_t offset_ms);

private:
    void stampSync();

private:
    bool _started = false;
    bool _have_video = false;
    MP4FileIO::Writer _mov_writter;
    int _non_iframe_video_count; // Non-I frames

    class FrameMergerImp : public FrameMerger {
    public:
        FrameMergerImp() : FrameMerger(FrameMerger::mp4_nal_size) {}
    };

    struct MP4Track {
        int track_id = -1;
        Stamp stamp;
        FrameMergerImp merger;
    };
    std::unordered_map<int, MP4Track> _tracks;
};

class MP4Muxer : public MP4MuxerInterface{
public:
    using Ptr = std::shared_ptr<MP4Muxer>;
    ~MP4Muxer() override;
    /**
     * Reset all tracks
     */
    void resetTracks() override;

    /**
     * Open mp4
     * @param file Full file path
     */
    void openMP4(const std::string &file);

    /**
     * Manually close the file (it will be closed automatically when the object is destructed)
     */
    void closeMP4();

protected:
    MP4FileIO::Writer createWriter() override;

private:
    std::string _file_name;
    MP4FileDisk::Ptr _mp4_file;
};

class MP4MuxerMemory : public MP4MuxerInterface{
public:
    MP4MuxerMemory();

    /**
     * Reset all tracks
     */
    void resetTracks() override;

    /**
     * Input frame
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * Get fmp4 init segment
     */
    const std::string &getInitSegment();

protected:
    /**
     * Output fmp4 fragment callback function
     * @param std::string Fragment content
     * @param stamp Fragment end timestamp
     * @param key_frame Whether there is a key frame
     */
    virtual void onSegmentData(std::string string, uint64_t stamp, bool key_frame) = 0;

protected:
    MP4FileIO::Writer createWriter() override;

private:
    bool _key_frame = false;
    uint64_t _last_dst = 0;
    std::string _init_segment;
    MP4FileMemory::Ptr _memory_file;
};

} // namespace mediakit

#else

#include "Common/MediaSink.h"

namespace mediakit {

class MP4MuxerMemory : public MediaSinkInterface {
public:
    bool addTrack(const Track::Ptr & track) override { return false; }
    bool inputFrame(const Frame::Ptr &frame) override { return false; }
    const std::string &getInitSegment() { static std::string kNull; return kNull; };

protected:
    /**
     * Output fmp4 fragment callback function
     * @param std::string Fragment content
     * @param stamp Fragment end timestamp
     * @param key_frame Whether there is a key frame
     */
    virtual void onSegmentData(std::string string, uint64_t stamp, bool key_frame) = 0;
};

} // namespace mediakit

#endif //defined(ENABLE_MP4)
#endif //S3MEDIAKIT_MP4MUXER_H
