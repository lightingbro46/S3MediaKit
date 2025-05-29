#ifndef S3MEDIAKIT_MKVMUXER_H
#define S3MEDIAKIT_MKVMUXER_H

#if defined(ENABLE_MKV)

#include "Common/MediaSink.h"
#include "Common/Stamp.h"
#include "MKV.h"

namespace mediakit {

class MKVMuxerInterface : public MediaSinkInterface {
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
     * Get mkv duration, in milliseconds
     */
    uint64_t getDuration() const;

protected:
    virtual MKVFileIO::Writer createWriter() = 0;

private:
    void stampSync();

private:
    bool _started = false;
    bool _have_video = false;
    MKVFileIO::Writer _mkv_writter;
    int _non_iframe_video_count; // Non-I frames

    class FrameMergerImp : public FrameMerger {
    public:
        FrameMergerImp() : FrameMerger(FrameMerger::mp4_nal_size) {}
    };

    struct MKVTrack {
        int track_id = -1;
        Stamp stamp;
        FrameMergerImp merger;
    };
    std::unordered_map<int, MKVTrack> _tracks;   
};

class MKVMuxer : public MKVMuxerInterface {
public:
    using Ptr = std::shared_ptr<MKVMuxer>;
    ~MKVMuxer() override;
    /**
     * Reset all tracks
     */
    void resetTracks() override;

    /**
     * Open mkv
     * @param file Full file path
     */
    void openMKV(const std::string &file);

    /**
     * Manually close the file (it will be closed automatically when the object is destructed)
     */
    void closeMKV();

protected:
    MKVFileIO::Writer createWriter() override;

private:
    std::string _file_name;
    MKVFileDisk::Ptr _mkv_file;
};

class MKVMuxerMemory : public MKVMuxerInterface {
public:
    MKVMuxerMemory();

    /**
     * Reset all tracks
     */
    void resetTracks() override;

    /**
     * Input frame
     */
    bool inputFrame(const Frame::Ptr &frame) override;

protected:
    /**
     * Output fragment callback function
     * @param std::string Fragment content
     * @param stamp Fragment end timestamp
     * @param key_frame Whether there is a key frame
     */
    virtual void onSegmentData(std::string string, uint64_t stamp, bool key_frame) = 0;

protected:
    MKVFileIO::Writer createWriter() override;

private:
    bool _key_frame = false;
    uint64_t _last_dst = 0;
    MKVFileMemory::Ptr _memory_file;
};

} // namespace mediakit

#else

#include "Common/MediaSink.h"

namespace mediakit {

class MKVMuxerMemory : public MediaSinkInterface {
public:
    bool addTrack(const Track::Ptr & track) override { return false; }
    bool inputFrame(const Frame::Ptr &frame) override { return false; }


protected:
    /**
     * Output fragment callback function
     * @param std::string Fragment content
     * @param stamp Fragment end timestamp
     * @param key_frame Whether there is a key frame
     */
    virtual void onSegmentData(std::string string, uint64_t stamp, bool key_frame) = 0;
};

} // namespace mediakit

#endif // defined(ENABLE_MKV)
#endif // S3MEDIAKIT_MKVMUXER_H