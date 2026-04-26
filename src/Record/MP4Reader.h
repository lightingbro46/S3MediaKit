#ifndef SRC_MEDIAFILE_MEDIAREADER_H_
#define SRC_MEDIAFILE_MEDIAREADER_H_
#ifdef ENABLE_MP4

#include "MP4Demuxer.h"
#include "Common/MultiMediaSourceMuxer.h"

namespace mediakit {

class MP4Reader : public std::enable_shared_from_this<MP4Reader>, public MediaSourceEvent {
public:
    using Ptr = std::shared_ptr<MP4Reader>;

    /**
     * Play an mp4 file and convert it to a MediaSource stream
     * @param vhost Virtual host
     * @param app Application name
     * @param stream_id Stream id, if empty, only demultiplex mp4, but not generate MediaSource
     * @param file_path File path, if empty, it will be automatically generated according to the configuration file and the above parameters, otherwise use the specified file
     */
    MP4Reader(const MediaTuple &tuple, const std::string &file_path = "", toolkit::EventPoller::Ptr poller = nullptr);

    MP4Reader(const MediaTuple &tuple, const std::string &file_path, const ProtocolOption &option, toolkit::EventPoller::Ptr poller = nullptr);

    /**
     * Start demultiplexing the MP4 file
     * @param sample_ms The amount of file data read each time, in milliseconds, set to 0 to use the configuration file configuration
     * @param ref_self Whether to let the timer reference this object itself, if there is no other object referencing itself, when not looping to read the file, after reading the file, this object will be automatically destroyed
     * @param file_repeat Whether to loop to read the file, if the configuration file is set to loop to read the file, this parameter is invalid
     */
    void startReadMP4(uint64_t sample_ms = 0, bool ref_self = true,  bool file_repeat = false);

    /**
     * Stop demultiplexing the MP4 timer
     */
    void stopReadMP4();

    /**
     * Get the mp4 demultiplexer
     */
    const MultiMP4Demuxer::Ptr& getDemuxer() const;

private:
    //MediaSourceEvent override
    bool seekTo(MediaSource &sender,uint32_t stamp) override;
    bool pause(MediaSource &sender, bool pause) override;
    bool speed(MediaSource &sender, float speed) override;

    bool close(MediaSource &sender) override;
    MediaOriginType getOriginType(MediaSource &sender) const override;
    std::string getOriginUrl(MediaSource &sender) const override;
    toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;

    bool readSample();
    bool readNextSample();
    uint32_t getCurrentStamp();
    void setCurrentStamp(uint32_t stamp);
    bool seekTo(uint32_t stamp_seek);
    void onTracksChanged(const std::vector<Track::Ptr> &new_tracks);

    void setup(const MediaTuple &tuple, const std::string &file_path, const ProtocolOption &option, toolkit::EventPoller::Ptr poller);

private:
    bool _file_repeat = false;
    bool _have_video = false;
    bool _paused = false;
    float _speed = 1.0;
    uint32_t _last_dts = 0;
    uint32_t _seek_to = 0;
    std::string _file_path;
    std::recursive_mutex _mtx;
    toolkit::Ticker _seek_ticker;
    toolkit::Timer::Ptr _timer;
    MultiMP4Demuxer::Ptr _demuxer;
    MultiMediaSourceMuxer::Ptr _muxer;
    toolkit::EventPoller::Ptr _poller;
};

} /* namespace mediakit */
#endif //ENABLE_MP4
#endif /* SRC_MEDIAFILE_MEDIAREADER_H_ */
