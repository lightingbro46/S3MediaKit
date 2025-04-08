#ifndef S3MEDIAKIT_TRACK_H
#define S3MEDIAKIT_TRACK_H

#include <memory>
#include <string>
#include "Frame.h"
#include "Rtsp/Rtsp.h"

namespace mediakit{

/**
 * Media channel description class, also supports frame input and output
 */
class Track : public FrameDispatcher, public CodecInfo {
public:
    using Ptr = std::shared_ptr<Track>;

    /**
     * Default constructor
     */
    Track() = default;

    /**
     * Copy, only copy information of derived classes,
     * Circular buffer and proxy relationships cannot be copied, otherwise the relationship will be disordered
     */
    Track(const Track &that) {
        _bit_rate = that._bit_rate;
        setIndex(that.getIndex());
    }

    /**
     * Whether it is ready, it can be used to get information such as sps pps
     */
    virtual bool ready() const = 0;

    /**
     * Clone interface, used to copy this object
     * When calling this interface, only the information of the derived class will be copied
     * Circular buffer and proxy relationships cannot be copied, otherwise the relationship will be disordered
     */
    virtual Track::Ptr clone() const = 0;

    /**
     * Update track information, such as triggering sps/pps parsing
     */
    virtual bool update() { return false; }

    /**
     * Generate sdp
     * @return sdp object
     */
    virtual Sdp::Ptr getSdp(uint8_t payload_type) const = 0;

    /**
     * Get extra data, generally used for rtmp/mp4 generation
     */
    virtual toolkit::Buffer::Ptr getExtraData() const { return nullptr; }

    /**
     * Set extra data,
     */
    virtual void setExtraData(const uint8_t *data, size_t size) {}

    /**
     * Return bitrate
     * @return Bitrate
     */
    virtual int getBitRate() const { return _bit_rate; }

    /**
     * Set bitrate
     * @param bit_rate Bitrate
     */
    virtual void setBitRate(int bit_rate) { _bit_rate = bit_rate; }

private:
    int _bit_rate = 0;
};

/**
 * Video channel description Track class, supports getting width, height and fps information
 */
class VideoTrack : public Track {
public:
    using Ptr = std::shared_ptr<VideoTrack>;

    /**
     * Return video height
     */
    virtual int getVideoHeight() const { return 0; }

    /**
     * Return video width
     */
    virtual int getVideoWidth() const { return 0; }

    /**
     * Return video fps
     */
    virtual float getVideoFps() const { return 0; }

    /**
     * Return related sps/pps, etc.
     */
    virtual std::vector<Frame::Ptr> getConfigFrames() const { return std::vector<Frame::Ptr>{}; }
};

class VideoTrackImp : public VideoTrack {
public:
    using Ptr = std::shared_ptr<VideoTrackImp>;

    /**
     * Constructor
     * @param codec_id Encoding type
     * @param width Width
     * @param height Height
     * @param fps Frame rate
     */
    VideoTrackImp(CodecId codec_id, int width, int height, int fps) {
        _codec_id = codec_id;
        _width = width;
        _height = height;
        _fps = fps;
    }

    int getVideoWidth() const override { return _width; }
    int getVideoHeight() const override { return _height; }
    float getVideoFps() const override { return _fps; }
    bool ready() const override { return true; }

    Track::Ptr clone() const override { return std::make_shared<VideoTrackImp>(*this); }
    Sdp::Ptr getSdp(uint8_t payload_type) const override;
    CodecId getCodecId() const override { return _codec_id; }

private:
    CodecId _codec_id;
    int _width = 0;
    int _height = 0;
    float _fps = 0;
};

/**
 * Audio Track derived class, supports sampling rate, number of channels, and sampling bit information
 */
class AudioTrack : public Track {
public:
    using Ptr = std::shared_ptr<AudioTrack>;

    /**
     * Return audio sampling rate
     */
    virtual int getAudioSampleRate() const  {return 0;};

    /**
     * Return audio sampling bit depth, generally 16 or 8
     */
    virtual int getAudioSampleBit() const {return 0;};

    /**
     * Return audio number of channels
     */
    virtual int getAudioChannel() const {return 0;};
};

class AudioTrackImp : public AudioTrack{
public:
    using Ptr = std::shared_ptr<AudioTrackImp>;

    /**
     * Constructor
     * @param codecId Encoding type
     * @param sample_rate Sampling rate (HZ)
     * @param channels Number of channels
     * @param sample_bit Sampling bit depth, generally 16
     */
    AudioTrackImp(CodecId codecId, int sample_rate, int channels, int sample_bit){
        _codecid = codecId;
        _sample_rate = sample_rate;
        _channels = channels;
        _sample_bit = sample_bit;
    }

    /**
     * Return encoding type
     */
    CodecId getCodecId() const override{
        return _codecid;
    }

    /**
     * Whether it has been initialized
     */
    bool ready() const override {
        return true;
    }

    /**
     * Return audio sampling rate
     */
    int getAudioSampleRate() const override{
        return _sample_rate ? _sample_rate : RtpPayload::getClockRateByCodec(_codecid);
    }

    /**
     * Return audio sampling bit depth, generally 16 or 8
     */
    int getAudioSampleBit() const override{
        return _sample_bit ? _sample_bit : 16;
    }

    /**
     * Return audio number of channels
     */
    int getAudioChannel() const override{
        return _channels ? _channels : 1;
    }

    Track::Ptr clone() const override { return std::make_shared<AudioTrackImp>(*this); }
    Sdp::Ptr getSdp(uint8_t payload_type) const override;

private:
    CodecId _codecid;
    int _sample_rate;
    int _channels;
    int _sample_bit;
};

class TrackSource {
public:
    virtual ~TrackSource() = default;

    /**
     * Get all Tracks
     * @param trackReady Whether to get all ready Tracks
     */
    virtual std::vector<Track::Ptr> getTracks(bool trackReady = true) const = 0;

    /**
     * Get specific Track
     * @param type Track type
     * @param trackReady Whether to get all ready Tracks
     */
    Track::Ptr getTrack(TrackType type , bool trackReady = true) const {
        auto tracks = getTracks(trackReady);
        for(auto &track : tracks){
            if(track->getTrackType() == type){
                return track;
            }
        }
        return nullptr;
    }
};

}//namespace mediakit
#endif //S3MEDIAKIT_TRACK_H