#include "FlvSplitter.h"
#include "utils.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

const char *FlvSplitter::onSearchPacketTail(const char *data, size_t len) {
    if (!_flv_started) {
        // Not yet got the flv header
        if (len < sizeof(FLVHeader)) {
            // Insufficient data
            return nullptr;
        }
        return data + sizeof(FLVHeader);
    }

    // Got the flv header, processing tag data
    if (len < sizeof(RtmpTagHeader)) {
        // Insufficient data
        return nullptr;
    }
    return data + sizeof(RtmpTagHeader);
}

ssize_t FlvSplitter::onRecvHeader(const char *data, size_t len) {
    if (!_flv_started) {
        // Got the flv header
        auto header = reinterpret_cast<const FLVHeader *>(data);
        if (memcmp(header->flv, "FLV", 3)) {
            throw std::invalid_argument("Not the flv container format!");
        }
        if (header->version != FLVHeader::kFlvVersion) {
            throw std::invalid_argument("The version field in the flv header is incorrect");
        }
        if (!header->have_video && !header->have_audio) {
            throw std::invalid_argument("The FLV header states that the audio and video do not exist");
        }
        if (FLVHeader::kFlvHeaderLength != ntohl(header->length)) {
            throw std::invalid_argument("The length field in the flv header is illegal");
        }
        if (0 != ntohl(header->previous_tag_size0)) {
            throw std::invalid_argument("The previous tag size field in the flv header is illegal");
        }
        onRecvFlvHeader(*header);
        _flv_started = true;
        return 0;
    }

    // Got the flv header, processing tag data
    auto tag = reinterpret_cast<const RtmpTagHeader *>(data);
    auto data_size = load_be24(tag->data_size);
    _type = tag->type;
    _time_stamp = load_be24(tag->timestamp);
    _time_stamp |= (tag->timestamp_ex << 24);
    return data_size + 4/*PreviousTagSize*/;
}

void FlvSplitter::onRecvContent(const char *data, size_t len) {
    len -= 4;
    auto previous_tag_size = load_be32(data + len);
    if (len != previous_tag_size - sizeof(RtmpTagHeader)) {
        WarnL << "flv previous tag size field is illegal:" << len << " != " << previous_tag_size - sizeof(RtmpTagHeader);
    }
    RtmpPacket::Ptr packet;
    switch (_type) {
        case MSG_AUDIO : {
            packet = RtmpPacket::create();
            packet->chunk_id = CHUNK_AUDIO;
            packet->stream_index = STREAM_MEDIA;
            break;
        }
        case MSG_VIDEO: {
            packet = RtmpPacket::create();
            packet->chunk_id = CHUNK_VIDEO;
            packet->stream_index = STREAM_MEDIA;
            break;
        }

        case MSG_DATA:
        case MSG_DATA3: {
            BufferLikeString buffer(string(data, len));
            AMFDecoder dec(buffer, _type == MSG_DATA3 ? 3 : 0);
            auto first = dec.load<AMFValue>();
            bool flag = true;
            if (first.type() == AMFType::AMF_STRING) {
                auto type = first.as_string();
                if (type == "@setDataFrame") {
                    type = dec.load<std::string>();
                    if (type == "onMetaData") {
                        flag = onRecvMetadata(dec.load<AMFValue>());
                    } else {
                        WarnL << "unknown type:" << type;
                    }
                } else if (type == "onMetaData") {
                    flag = onRecvMetadata(dec.load<AMFValue>());
                } else {
                    WarnL << "unknown notify:" << type;
                }
            } else {
                WarnL << "Parse flv script data failed, invalid amf value: " << first.to_string();
            }
            if (!flag) {
                throw std::invalid_argument("check rtmp metadata failed");
            }
            return;
        }

        default: WarnL << "Unrecognized flv msg type:" << (int) _type; return;
    }

    packet->time_stamp = _time_stamp;
    packet->type_id = _type;
    packet->body_size = len;
    packet->buffer.assign(data, len);
    onRecvRtmpPacket(std::move(packet));
}


}//namespace mediakit