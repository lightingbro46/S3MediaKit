#ifndef S3MEDIAKIT_HLSVIEWERSESSION_H
#define S3MEDIAKIT_HLSVIEWERSESSION_H

#include <string>

namespace mediakit {

class Parser;

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
    static std::string rewritePlaylist(const std::string &playlist, const std::string &session_id);
};

} // namespace mediakit

#endif // S3MEDIAKIT_HLSVIEWERSESSION_H
