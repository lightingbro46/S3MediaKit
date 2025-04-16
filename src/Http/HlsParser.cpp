#include <cstdlib>
#include <cinttypes>
#include "HlsParser.h"
#include "Util/util.h"
#include "Common/Parser.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

bool HlsParser::parse(const string &http_url, const string &m3u8) {
    float extinf_dur = 0;
    ts_segment segment;
    map<int, ts_segment> ts_map;
    _total_dur = 0;
    _is_live = true;
    _is_m3u8_inner = false;
    int index = 0;

    auto lines = split(m3u8, "\n");
    for (auto &line : lines) {
        trim(line);
        if (line.size() < 2) {
            continue;
        }

        if ((_is_m3u8_inner || extinf_dur != 0) && line[0] != '#') {
            segment.duration = extinf_dur;
            segment.url = Parser::mergeUrl(http_url, line);
            if (!_is_m3u8_inner) {
                // Sort by order of appearance
                ts_map.emplace(index++, segment);
            } else {
                // Sort sub m3u8 by bandwidth
                ts_map.emplace(segment.bandwidth, segment);
            }
            extinf_dur = 0;
            continue;
        }

        _is_m3u8_inner = false;
        if (line.find("#EXTINF:") == 0) {
            sscanf(line.data(), "#EXTINF:%f,", &extinf_dur);
            _total_dur += extinf_dur;
            continue;
        }
        static const string s_stream_inf = "#EXT-X-STREAM-INF:";
        if (line.find(s_stream_inf) == 0) {
            _is_m3u8_inner = true;
            auto key_val = Parser::parseArgs(line.substr(s_stream_inf.size()), ",", "=");
            segment.program_id = atoi(key_val["PROGRAM-ID"].data());
            segment.bandwidth = atoi(key_val["BANDWIDTH"].data());
            sscanf(key_val["RESOLUTION"].data(), "%dx%d", &segment.width, &segment.height);
            continue;
        }

        if (line == "#EXTM3U") {
            _is_m3u8 = true;
            continue;
        }

        if (line.find("#EXT-X-ALLOW-CACHE:") == 0) {
            _allow_cache = (line.find(":YES") != string::npos);
            continue;
        }

        if (line.find("#EXT-X-VERSION:") == 0) {
            sscanf(line.data(), "#EXT-X-VERSION:%d", &_version);
            continue;
        }

        if (line.find("#EXT-X-TARGETDURATION:") == 0) {
            sscanf(line.data(), "#EXT-X-TARGETDURATION:%d", &_target_dur);
            continue;
        }

        if (line.find("#EXT-X-MEDIA-SEQUENCE:") == 0) {
            sscanf(line.data(), "#EXT-X-MEDIA-SEQUENCE:%" PRId64, &_sequence);
            continue;
        }

        if (line.find("#EXT-X-ENDLIST") == 0) {
            // On-demand
            _is_live = false;
            continue;
        }
        continue;
    }

    return _is_m3u8 && onParsed(_is_m3u8_inner, _sequence, ts_map);
}

bool HlsParser::isM3u8() const {
    return _is_m3u8;
}

bool HlsParser::isLive() const {
    return _is_live;
}

bool HlsParser::allowCache() const {
    return _allow_cache;
}

int HlsParser::getVersion() const {
    return _version;
}

int HlsParser::getTargetDur() const {
    return _target_dur;
}

int64_t HlsParser::getSequence() const {
    return _sequence;
}

bool HlsParser::isM3u8Inner() const {
    return _is_m3u8_inner;
}

float HlsParser::getTotalDuration() const {
    return _total_dur;
}

}//namespace mediakit
