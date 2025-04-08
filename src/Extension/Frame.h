#ifndef S3MEDIAKIT_FRAME_H
#define S3MEDIAKIT_FRAME_H

#include <map>
#include <mutex>
#include <functional>
#include "Util/List.h"
#include "Util/TimeTicker.h"
#include "Common/Stamp.h"
#include "Network/Buffer.h"

namespace mediakit {

class Stamp;

typedef enum {
    TrackInvalid = -1,
    TrackVideo = 0,
    TrackAudio,
    TrackTitle,
    TrackApplication,
    TrackMax
} TrackType;

#define CODEC_MAP(XX) \
    XX(CodecH264,  TrackVideo, 0, "H264", PSI_STREAM_H264, MOV_OBJECT_H264)          \
    XX(CodecH265,  TrackVideo, 1, "H265", PSI_STREAM_H265, MOV_OBJECT_HEVC)          \
    XX(CodecAAC,   TrackAudio, 2, "mpeg4-generic", PSI_STREAM_AAC, MOV_OBJECT_AAC)   \
    XX(CodecG711A, TrackAudio, 3, "PCMA", PSI_STREAM_AUDIO_G711A, MOV_OBJECT_G711a)  \
    XX(CodecG711U, TrackAudio, 4, "PCMU", PSI_STREAM_AUDIO_G711U, MOV_OBJECT_G711u)  \
    XX(CodecOpus,  TrackAudio, 5, "opus", PSI_STREAM_AUDIO_OPUS, MOV_OBJECT_OPUS)    \
    XX(CodecL16,   TrackAudio, 6, "L16", PSI_STREAM_RESERVED, MOV_OBJECT_NONE)       \
    XX(CodecVP8,   TrackVideo, 7, "VP8", PSI_STREAM_VP8, MOV_OBJECT_VP8)             \
    XX(CodecVP9,   TrackVideo, 8, "VP9", PSI_STREAM_VP9, MOV_OBJECT_VP9)             \
    XX(CodecAV1,   TrackVideo, 9, "AV1", PSI_STREAM_AV1, MOV_OBJECT_AV1)             \
    XX(CodecJPEG,  TrackVideo, 10, "JPEG", PSI_STREAM_JPEG_2000, MOV_OBJECT_JPEG)    \
    XX(CodecH266,  TrackVideo, 11, "H266", PSI_STREAM_H266, MOV_OBJECT_H266)         \
    XX(CodecTS,    TrackVideo, 12, "MP2T", PSI_STREAM_RESERVED, MOV_OBJECT_NONE)     \
    XX(CodecPS,    TrackVideo, 13, "MPEG", PSI_STREAM_RESERVED, MOV_OBJECT_NONE)     \
    XX(CodecMP3,   TrackAudio, 14, "MP3",  PSI_STREAM_MP3, MOV_OBJECT_MP3)           \
    XX(CodecADPCM, TrackAudio, 15, "ADPCM", PSI_STREAM_RESERVED, MOV_OBJECT_NONE)    \
    XX(CodecSVACV, TrackVideo, 16, "SVACV", PSI_STREAM_VIDEO_SVAC, MOV_OBJECT_NONE)  \
    XX(CodecSVACA, TrackAudio, 17, "SVACA", PSI_STREAM_AUDIO_SVAC, MOV_OBJECT_NONE)  \
    XX(CodecG722,  TrackAudio, 18, "G722", PSI_STREAM_AUDIO_G722, MOV_OBJECT_NONE)   \
    XX(CodecG723,  TrackAudio, 19, "G723", PSI_STREAM_AUDIO_G723, MOV_OBJECT_NONE)   \
    XX(CodecG728,  TrackAudio, 20, "G728", PSI_STREAM_RESERVED, MOV_OBJECT_NONE)     \
    XX(CodecG729,  TrackAudio, 21, "G729", PSI_STREAM_AUDIO_G729, MOV_OBJECT_NONE)

typedef enum {
    CodecInvalid = -1,
#define XX(name, type, value, str, mpeg_id, mp4_id) name = value,
    CODEC_MAP(XX)
#undef XX
    CodecMax
} CodecId;

/**
 * String to media type conversion
 */
TrackType getTrackType(const std::string &str);

/**
 * Media type to string conversion
 */
const char* getTrackString(TrackType type);

/**
 * Get codec_id from SDP description
 * @param str
 * @return
 */
CodecId getCodecId(const std::string &str);

/**
 * Get encoder name
 */
const char *getCodecName(CodecId codecId);

/**
 * Get audio/video type
 */
TrackType getTrackType(CodecId codecId);

/**
 * Get mov object id by codecid
 */
int getMovIdByCodec(CodecId codecId);

/**
 * Get CodecId by mov object id
 */
CodecId getCodecByMovId(int object_id);

/**
 * Get mpeg id by codecid
 */
int getMpegIdByCodec(CodecId codec);

/**
 * Get CodecId by mpeg id
 */
CodecId getCodecByMpegId(int mpeg_id);

/**
 * Abstract interface for encoding information
 */
class CodecInfo {
public:
    using Ptr = std::shared_ptr<CodecInfo>;

    virtual ~CodecInfo() = default;

    /**
     * Get codec type
     */
    virtual CodecId getCodecId() const = 0;

    /**
     * Get encoder name
     */
    const char *getCodecName() const;

    /**
     * Get audio/video type
     */
    TrackType getTrackType() const;

    /**
     * Get audio/video type description
     */
    std::string getTrackTypeStr() const;

    /**
     * Set track index, for multi-track support
     */
    void setIndex(int index) { _index = index; }

    /**
     * Get track index, for multi-track support
     */
    int getIndex() const { return _index < 0 ? (int)getTrackType() : _index; }

private:
    int _index = -1;
};

/**
 * Abstract interface for frame types
 */
class Frame : public toolkit::Buffer, public CodecInfo {
public:
    using Ptr = std::shared_ptr<Frame>;

    /**
     * Return decoding timestamp, in milliseconds
     */
    virtual uint64_t dts() const = 0;

    /**
     * Return display timestamp, in milliseconds
     */
    virtual uint64_t pts() const { return dts(); }

    /**
     * Prefix length, for example, the 264 prefix is 0x00 00 00 01, so the prefix length is 4
     * aac prefix is 7 bytes
     */
    virtual size_t prefixSize() const = 0;

    /**
     * Return whether it is a key frame
     */
    virtual bool keyFrame() const = 0;

    /**
     * Whether it is a configuration frame, such as sps pps vps
     */
    virtual bool configFrame() const = 0;

    /**
     * Whether it can be cached
     */
    virtual bool cacheAble() const { return true; }

    /**
     * Whether this frame can be dropped
     * SEI/AUD frames can be dropped
     * By default, no frames can be dropped
     */
    virtual bool dropAble() const { return false; }

    /**
     * Whether it is a decodable frame
     * sps pps frames cannot be decoded
     */
    virtual bool decodeAble() const {
        if (getTrackType() != TrackVideo) {
            // Non-video frames can be decoded
            return true;
        }
        // By default, non-sps pps frames can be decoded
        return !configFrame();
    }

    /**
     * Return the cacheable frame
     */
    static Ptr getCacheAbleFrame(const Ptr &frame);

private:
    // Object count statistics
    toolkit::ObjectStatistic<Frame> _statistic;
};

class FrameImp : public Frame {
public:
    using Ptr = std::shared_ptr<FrameImp>;

    template <typename C = FrameImp>
    static std::shared_ptr<C> create() {
#if 0
        static ResourcePool<C> packet_pool;
        static onceToken token([]() {
            packet_pool.setSize(1024);
        });
        auto ret = packet_pool.obtain2();
        ret->_buffer.clear();
        ret->_prefix_size = 0;
        ret->_dts = 0;
        ret->_pts = 0;
        return ret;
#else
        return std::shared_ptr<C>(new C());
#endif
    }

    char *data() const override { return (char *)_buffer.data(); }
    size_t size() const override { return _buffer.size(); }
    uint64_t dts() const override { return _dts; }
    uint64_t pts() const override { return _pts ? _pts : _dts; }
    size_t prefixSize() const override { return _prefix_size; }
    CodecId getCodecId() const override { return _codec_id; }
    bool keyFrame() const override { return false; }
    bool configFrame() const override { return false; }

public:
    CodecId _codec_id = CodecInvalid;
    uint64_t _dts = 0;
    uint64_t _pts = 0;
    size_t _prefix_size = 0;
    toolkit::BufferLikeString _buffer;

private:
    // Object count statistics
    toolkit::ObjectStatistic<FrameImp> _statistic;

protected:
    friend class toolkit::ResourcePool_l<FrameImp>;
    FrameImp() = default;
};

// Wrap a pointer into a non-cacheable frame
class FrameFromPtr : public Frame {
public:
    using Ptr = std::shared_ptr<FrameFromPtr>;

    FrameFromPtr(CodecId codec_id, char *ptr, size_t size, uint64_t dts, uint64_t pts = 0, size_t prefix_size = 0, bool is_key = false)
        : FrameFromPtr(ptr, size, dts, pts, prefix_size, is_key) {
        _codec_id = codec_id;
    }

    char *data() const override { return _ptr; }
    size_t size() const override { return _size; }
    uint64_t dts() const override { return _dts; }
    uint64_t pts() const override { return _pts ? _pts : dts(); }
    size_t prefixSize() const override { return _prefix_size; }
    bool cacheAble() const override { return false; }
    bool keyFrame() const override { return _is_key; }
    bool configFrame() const override { return false; }

    CodecId getCodecId() const override {
        if (_codec_id == CodecInvalid) {
            throw std::invalid_argument("Invalid codec type of FrameFromPtr");
        }
        return _codec_id;
    }

protected:
    FrameFromPtr() = default;

    FrameFromPtr(char *ptr, size_t size, uint64_t dts, uint64_t pts = 0, size_t prefix_size = 0, bool is_key = false) {
        _ptr = ptr;
        _size = size;
        _dts = dts;
        _pts = pts;
        _prefix_size = prefix_size;
        _is_key = is_key;
    }

protected:
    bool _is_key;
    char *_ptr;
    uint64_t _dts;
    uint64_t _pts = 0;
    size_t _size;
    size_t _prefix_size;
    CodecId _codec_id = CodecInvalid;
};

/**
 * A Frame class can have multiple frames (AAC), and the timestamp will change
 * S3MediaKit will first split this composite frame into single frames and then process it
 * A composite frame can be split into multiple sub-Frames without memory copy
 * The purpose of providing this class is to prevent memory copy when splitting composite frames, improving performance
 */
template <typename Parent>
class FrameInternalBase : public Parent {
public:
    using Ptr = std::shared_ptr<FrameInternalBase>;
    FrameInternalBase(Frame::Ptr parent_frame, char *ptr, size_t size, uint64_t dts, uint64_t pts = 0, size_t prefix_size = 0)
        : Parent(parent_frame->getCodecId(), ptr, size, dts, pts, prefix_size) {
        _parent_frame = std::move(parent_frame);
        this->setIndex(_parent_frame->getIndex());
    }

    bool cacheAble() const override { return _parent_frame->cacheAble(); }

private:
    Frame::Ptr _parent_frame;
};

/**
 * A Frame class can have multiple frames, they are separated by 0x 00 00 01
 * S3MediaKit will first split this composite frame into single frames and then process it
 * A composite frame can be split into multiple sub-Frames without memory copy
 * The purpose of providing this class is to prevent memory copy when splitting composite frames, improving performance
 */
template <typename Parent>
class FrameInternal : public FrameInternalBase<Parent> {
public:
    using Ptr = std::shared_ptr<FrameInternal>;
    FrameInternal(const Frame::Ptr &parent_frame, char *ptr, size_t size, size_t prefix_size)
        : FrameInternalBase<Parent>(parent_frame, ptr, size, parent_frame->dts(), parent_frame->pts(), prefix_size) {}
};

// Manage the lifetime of a pointer and produce a frame
class FrameAutoDelete : public FrameFromPtr {
public:
    template <typename... ARGS>
    FrameAutoDelete(ARGS &&...args) : FrameFromPtr(std::forward<ARGS>(args)...) {}

    ~FrameAutoDelete() override { delete[] _ptr; };

    bool cacheAble() const override { return true; }
};

// Declare a non-cacheable frame as cacheable
template <typename Parent>
class FrameToCache : public Parent {
public:
    template<typename ... ARGS>
    FrameToCache(ARGS &&...args) : Parent(std::forward<ARGS>(args)...) {};

    bool cacheAble() const override {
        return true;
    }
};

// The function of this object is to convert a non-cacheable frame into a cacheable frame
class FrameCacheAble : public FrameFromPtr {
public:
    using Ptr = std::shared_ptr<FrameCacheAble>;

    FrameCacheAble(const Frame::Ptr &frame, bool force_key_frame = false, toolkit::Buffer::Ptr buf = nullptr) {
        setIndex(frame->getIndex());
        if (frame->cacheAble()) {
            _ptr = frame->data();
            _buffer = frame;
        } else if (buf) {
            _ptr = frame->data();
            _buffer = std::move(buf);
        } else {
            auto buffer = std::make_shared<toolkit::BufferLikeString>();
            buffer->assign(frame->data(), frame->size());
            _ptr = buffer->data();
            _buffer = std::move(buffer);
        }
        _size = frame->size();
        _dts = frame->dts();
        _pts = frame->pts();
        _prefix_size = frame->prefixSize();
        _codec_id = frame->getCodecId();
        _key = force_key_frame ? true : frame->keyFrame();
        _config = frame->configFrame();
        _drop_able = frame->dropAble();
        _decode_able = frame->decodeAble();
    }

    /**
     * Can be cached
     */
    bool cacheAble() const override { return true; }
    bool keyFrame() const override { return _key; }
    bool configFrame() const override { return _config; }
    bool dropAble() const override { return _drop_able; }
    bool decodeAble() const override { return _decode_able; }

private:
    bool _key;
    bool _config;
    bool _drop_able;
    bool _decode_able;
    toolkit::Buffer::Ptr _buffer;
};

// This class implements frame-level timestamp overwrite
class FrameStamp : public Frame {
public:
    using Ptr = std::shared_ptr<FrameStamp>;
    FrameStamp(Frame::Ptr frame);
    FrameStamp(Frame::Ptr frame, Stamp &stamp, int modify_stamp);
    ~FrameStamp() override {}

    uint64_t dts() const override { return (uint64_t)_dts; }
    uint64_t pts() const override { return (uint64_t)_pts; }
    size_t prefixSize() const override { return _frame->prefixSize(); }
    bool keyFrame() const override { return _frame->keyFrame(); }
    bool configFrame() const override { return _frame->configFrame(); }
    bool cacheAble() const override { return _frame->cacheAble(); }
    bool dropAble() const override { return _frame->dropAble(); }
    bool decodeAble() const override { return _frame->decodeAble(); }
    char *data() const override { return _frame->data(); }
    size_t size() const override { return _frame->size(); }
    CodecId getCodecId() const override { return _frame->getCodecId(); }
    void setStamp(int64_t dts, int64_t pts);

private:
    int64_t _dts;
    int64_t _pts;
    Frame::Ptr _frame;
};

/**
 * This object can convert a Buffer object into a cacheable Frame object
 */
template <typename Parent>
class FrameFromBuffer : public Parent {
public:
    /**
     * Construct frame
     * @param buf Data cache
     * @param dts Decode timestamp
     * @param pts Display timestamp
     * @param prefix Frame prefix length
     * @param offset Buffer valid data offset
     */
    FrameFromBuffer(toolkit::Buffer::Ptr buf, uint64_t dts, uint64_t pts, size_t prefix = 0, size_t offset = 0)
        : Parent(buf->data() + offset, buf->size() - offset, dts, pts, prefix) {
        _buf = std::move(buf);
    }

    /**
     * Construct frame
     * @param buf Data cache
     * @param dts Decode timestamp
     * @param pts Display timestamp
     * @param prefix Frame prefix length
     * @param offset Buffer valid data offset
     * @param codec Frame type
     */
    FrameFromBuffer(CodecId codec, toolkit::Buffer::Ptr buf, uint64_t dts, uint64_t pts, size_t prefix = 0, size_t offset = 0)
        : Parent(codec, buf->data() + offset, buf->size() - offset, dts, pts, prefix) {
        _buf = std::move(buf);
    }

    /**
     * This frame is cacheable
     */
    bool cacheAble() const override { return true; }

private:
    toolkit::Buffer::Ptr _buf;
};

/**
 * Merge some frames with the same timestamp
 */
class FrameMerger {
public:
    using onOutput = std::function<void(uint64_t dts, uint64_t pts, const toolkit::Buffer::Ptr &buffer, bool have_key_frame)>;
    using Ptr = std::shared_ptr<FrameMerger>;
    enum {
        none = 0,
        h264_prefix,
        mp4_nal_size,
    };

    FrameMerger(int type);

    /**
     * Refresh the output buffer, note that FrameMerger::inputFrame's onOutput callback will be called at this time
     * Please note whether the callback capture parameters are valid at this time
     */
    void flush();
    void clear();
    bool inputFrame(const Frame::Ptr &frame, onOutput cb, toolkit::BufferLikeString *buffer = nullptr);

private:
    bool willFlush(const Frame::Ptr &frame) const;
    void doMerge(toolkit::BufferLikeString &buffer, const Frame::Ptr &frame) const;

private:
    int _type;
    bool _have_decode_able_frame = false;
    onOutput _cb;
    toolkit::List<Frame::Ptr> _frame_cache;
};

/**
 * Abstract interface class for write frame interface
 */
class FrameWriterInterface {
public:
    using Ptr = std::shared_ptr<FrameWriterInterface>;
    virtual ~FrameWriterInterface() = default;

    /**
     * Write frame data
     */
    virtual bool inputFrame(const Frame::Ptr &frame) = 0;

    /**
     * Flush all frame caches in the output
     */
    virtual void flush() {};
};

/**
 * Frame circular buffer that supports proxy forwarding
 */
class FrameDispatcher : public FrameWriterInterface {
public:
    using Ptr = std::shared_ptr<FrameDispatcher>;

    /**
     * Add proxy
     */
    FrameWriterInterface* addDelegate(FrameWriterInterface::Ptr delegate) {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        return _delegates.emplace(delegate.get(), std::move(delegate)).first->second.get();
    }

    FrameWriterInterface* addDelegate(std::function<bool(const Frame::Ptr &frame)> cb);

    /**
     * Delete proxy
     */
    void delDelegate(FrameWriterInterface *ptr) {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        _delegates.erase(ptr);
    }

    /**
     * Write frame and dispatch
     */
    bool inputFrame(const Frame::Ptr &frame) override {
        doStatistics(frame);
        bool ret = false;
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        for (auto &pr : _delegates) {
            if (pr.second->inputFrame(frame)) {
                ret = true;
            }
        }
        return ret;
    }

    /**
     * Return the number of proxies
     */
    size_t size() const {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        return _delegates.size();
    }

    void clear() {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        _delegates.clear();
    }

    /**
     * Get the cumulative number of keyframes
     */
    uint64_t getVideoKeyFrames() const {
        return _video_key_frames;
    }

    /**
     * Get the number of frames
     */
    uint64_t getFrames() const {
        return _frames;
    }

    size_t getVideoGopSize() const {
        return _gop_size;
    }

    size_t getVideoGopInterval() const {
        return _gop_interval_ms;
    }

    int64_t getDuration() const {
        return _stamp.getRelativeStamp();
    }

private:
    void doStatistics(const Frame::Ptr &frame) {
        if (!frame->configFrame() && !frame->dropAble()) {
            // Ignore configuration frames and discardable frames
            ++_frames;
            int64_t out;
            _stamp.revise(frame->dts(), frame->pts(), out, out);
            if (frame->keyFrame() && frame->getTrackType() == TrackVideo) {
                // Statistics when encountering video keyframes
                ++_video_key_frames;
                _gop_size = _frames - _last_frames;
                _gop_interval_ms = _ticker.elapsedTime();
                _last_frames = _frames;
                _ticker.resetTime();
            }
        }
    }

private:
    toolkit::Ticker _ticker;
    size_t _gop_interval_ms = 0;
    size_t _gop_size = 0;
    uint64_t _last_frames = 0;
    uint64_t _frames = 0;
    uint64_t _video_key_frames = 0;
    Stamp _stamp;
    mutable std::recursive_mutex _mtx;
    std::map<void *, FrameWriterInterface::Ptr> _delegates;
};

} // namespace mediakit
#endif // S3MEDIAKIT_FRAME_H