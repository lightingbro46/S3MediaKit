#include "HlsViewerSession.h"

#include <cctype>

#include "Common/Parser.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

const char *HlsViewerSession::kSessionIdParam = "session_id";

string HlsViewerSession::getSessionId(const Parser &parser) {
    auto it = parser.getUrlArgs().find(kSessionIdParam);
    if (it == parser.getUrlArgs().end() || !isValidSessionId(it->second)) {
        return "";
    }
    return it->second;
}

HlsViewerSession::Identity HlsViewerSession::resolve(const Parser &parser, const string &cookie_session_id) {
    auto it = parser.getUrlArgs().find(kSessionIdParam);
    if (it != parser.getUrlArgs().end()) {
        if (!isValidSessionId(it->second)) {
            return Identity { Identity::Invalid, "" };
        }
        return Identity { Identity::Query, it->second };
    }
    if (isValidSessionId(cookie_session_id)) {
        return Identity { Identity::Cookie, cookie_session_id };
    }
    return Identity { Identity::Generated, createSessionId() };
}

string HlsViewerSession::createSessionId() {
    return makeRandStr(32);
}

bool HlsViewerSession::isValidSessionId(const string &session_id) {
    if (session_id.empty() || session_id.size() > 128) {
        return false;
    }
    for (auto ch : session_id) {
        auto value = static_cast<unsigned char>(ch);
        if (!isalnum(value) && ch != '-' && ch != '_' && ch != '.' && ch != '~') {
            return false;
        }
    }
    return true;
}

string HlsViewerSession::appendSessionIdToUri(const string &uri, const string &session_id) {
    if (!isValidSessionId(session_id) || uri.empty()) {
        return uri;
    }

    auto fragment_pos = uri.find('#');
    auto uri_without_fragment = uri.substr(0, fragment_pos);
    auto fragment = fragment_pos == string::npos ? string() : uri.substr(fragment_pos);
    auto query_pos = uri_without_fragment.find('?');
    auto path = uri_without_fragment.substr(0, query_pos);
    auto query = query_pos == string::npos ? string() : uri_without_fragment.substr(query_pos + 1);

    string updated_query;
    bool replaced = false;
    size_t offset = 0;
    while (offset <= query.size() && !query.empty()) {
        auto end = query.find('&', offset);
        auto item = query.substr(offset, end == string::npos ? string::npos : end - offset);
        auto equal = item.find('=');
        auto key = item.substr(0, equal);
        if (!updated_query.empty()) {
            updated_query += '&';
        }
        if (key == kSessionIdParam) {
            updated_query += string(kSessionIdParam) + '=' + session_id;
            replaced = true;
        } else {
            updated_query += item;
        }
        if (end == string::npos) {
            break;
        }
        offset = end + 1;
    }
    if (!replaced) {
        if (!updated_query.empty()) {
            updated_query += '&';
        }
        updated_query += string(kSessionIdParam) + '=' + session_id;
    }
    return path + '?' + updated_query + fragment;
}

static string rewritePlaylistLine(const string &line, const string &session_id) {
    if (line.empty()) {
        return line;
    }
    if (line[0] != '#') {
        return HlsViewerSession::appendSessionIdToUri(line, session_id);
    }

    string result = line;
    const string marker = "URI=\"";
    size_t offset = 0;
    while ((offset = result.find(marker, offset)) != string::npos) {
        auto value_begin = offset + marker.size();
        auto value_end = result.find('"', value_begin);
        if (value_end == string::npos) {
            break;
        }
        auto uri = result.substr(value_begin, value_end - value_begin);
        auto updated = HlsViewerSession::appendSessionIdToUri(uri, session_id);
        result.replace(value_begin, value_end - value_begin, updated);
        offset = value_begin + updated.size() + 1;
    }
    return result;
}

string HlsViewerSession::rewritePlaylist(const string &playlist, const string &session_id) {
    if (!isValidSessionId(session_id) || playlist.empty()) {
        return playlist;
    }

    string result;
    result.reserve(playlist.size() + 128);
    size_t offset = 0;
    while (offset < playlist.size()) {
        auto newline = playlist.find('\n', offset);
        auto line_end = newline == string::npos ? playlist.size() : newline;
        bool has_cr = line_end > offset && playlist[line_end - 1] == '\r';
        auto content_end = has_cr ? line_end - 1 : line_end;
        result += rewritePlaylistLine(playlist.substr(offset, content_end - offset), session_id);
        if (has_cr) {
            result += '\r';
        }
        if (newline != string::npos) {
            result += '\n';
            offset = newline + 1;
        } else {
            break;
        }
    }
    return result;
}

} // namespace mediakit
