#ifndef HTTP_HLSPARSER_H
#define HTTP_HLSPARSER_H

#include <string>
#include <list>
#include <map>

namespace mediakit {

typedef struct{
    // URL address
    std::string url;
    // TS segment length
    float duration;

    // //// Embedded m3u8 //////
    // Program ID
    int program_id;
    // Bandwidth
    int bandwidth;
    // Width
    int width;
    // Height
    int height;
} ts_segment;

class HlsParser {
public:
    bool parse(const std::string &http_url,const std::string &m3u8);

    /**
     * Whether the #EXTM3U field exists, whether it is an m3u8 file
     */
    bool isM3u8() const;

    /**
     * #EXT-X-ALLOW-CACHE value, whether caching is allowed
     */
    bool allowCache() const;

    /**
     * Whether the #EXT-X-ENDLIST field exists, whether it is a live stream
     */
    bool isLive() const ;

    /**
     * #EXT-X-VERSION value, version number
     */
    int getVersion() const;

    /**
     * #EXT-X-TARGETDURATION field value
     */
    int getTargetDur() const;

    /**
     * #EXT-X-MEDIA-SEQUENCE field value, the sequence number of this m3u8
     */
    int64_t getSequence() const;

    /**
     * Whether it contains sub-m3u8 internally
     */
    bool isM3u8Inner() const;

    /**
     * Get the total time
     */
    float getTotalDuration() const;

protected:
    /**
     * Callback for parsing the m3u8 file
     * @param is_m3u8_inner Whether this m3u8 file contains multiple HLS addresses
     * @param sequence TS sequence number
     * @param ts_list TS address list
     * @return Whether the parsing is successful, returning false will cause HlsParser::parse to return false
     */
    virtual bool onParsed(bool is_m3u8_inner, int64_t sequence, const std::map<int, ts_segment> &ts_list) = 0;

private:
    bool _is_m3u8 = false;
    bool _allow_cache = false;
    bool _is_live = true;
    int _version = 0;
    int _target_dur = 0;
    float _total_dur = 0;
    int64_t _sequence = 0;
    // Whether each part has an m3u8
    bool _is_m3u8_inner = false;
};

}//namespace mediakit
#endif //HTTP_HLSPARSER_H
