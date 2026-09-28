#ifndef S3MEDIAKIT_HLSVIEWERSESSION_H
#define S3MEDIAKIT_HLSVIEWERSESSION_H

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Common/config.h"

namespace mediakit {

class Parser;
class StrCaseMap;

class HlsViewerSession {
public:
    struct Identity {
        enum Origin {
            Query,
            Cookie,
            Generated,
            Invalid
        };

        Origin origin;
        std::string session_id;
    };

    static const char *kSessionIdParam;

    static std::string getSessionId(const Parser &parser);
    static Identity resolve(const Parser &parser, const std::string &cookie_session_id);
    static std::string createSessionId();
    static bool isValidSessionId(const std::string &session_id);
    static std::string appendSessionIdToUri(const std::string &uri, const std::string &session_id);
    static std::string makePlaylistRedirectUrl(const std::string &public_path, const std::string &params,
                                               const std::string &session_id);
    static StrCaseMap makeHlsPlaylistRedirectHeader(const std::string &public_path, const std::string &params,
                                                    const std::string &session_id);
    static std::string rewritePlaylist(const std::string &playlist, const std::string &session_id);
};

/**
 * View overlay policy of one HLS playback session.
 *
 * HLS segment/init/key URIs carry only session_id, never the JWT, so a policy
 * resolved for them would lose the viewer's exclusions. The playlist request,
 * which carries the token, stores the viewer's policy here; later requests of
 * the same session read it back instead of resolving it again.
 *
 * Key: HttpFileManager::getHlsViewerKey(), i.e. camera scope + session_id, so a
 * session never reuses a policy across cameras. An entry expires after its TTL
 * without access; every read extends it, like the HLS cookie.
 */
class HlsViewerPolicyCache {
public:
    static HlsViewerPolicyCache &Instance();

    void put(const std::string &key, const Broadcast::ViewOverlayPolicy &policy, uint64_t ttl_ms, uint64_t now_ms);
    bool get(const std::string &key, uint64_t now_ms, Broadcast::ViewOverlayPolicy &policy);
    size_t size();

private:
    struct Entry {
        Broadcast::ViewOverlayPolicy policy;
        uint64_t ttl_ms = 0;
        uint64_t expire_ms = 0;
    };

    void purgeExpired_l(uint64_t now_ms);

    std::mutex _mtx;
    std::unordered_map<std::string, Entry> _entries;
    uint64_t _next_purge_ms = 0;
};

} // namespace mediakit

#endif // S3MEDIAKIT_HLSVIEWERSESSION_H
