#include "BoschIpSpeaker.h"
#include "manager/Speaker/AudioFileManager.h"
#include "json/json.h"
#include "Common/StrUtil.h"
#include "Util/base64.h"

using namespace std;
using namespace Json;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

BoschIpSpeaker::BoschIpSpeaker(const SpeakerConfig& cfg)
    : m_cfg(cfg), m_baseUrl("http://" + cfg.ip + ":" + std::to_string(cfg.port)) {}

void BoschIpSpeaker::connect(OnDeviceResult cb) {
    TraceL << "[Bosch:" << m_cfg.id << "] Login " << m_cfg.ip << ":" << m_cfg.port;

    std::string body =
        "{\"darkMode\":false,"
        "\"group\":\"API\","
        "\"id\":null,"
        "\"languageCode\":\"en\","
        "\"password\":\"" + m_cfg.password + "\","
        "\"username\":\"" + m_cfg.username + "\"}";

    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("POST", /*withCookie=*/false);
    req->setBody(std::make_shared<HttpStringBody>(body));

    req->startRequester(
        m_baseUrl + "/api/login",
        [weak_self, cb, req](const SockException& ex, const Parser& parser) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            if (ex) {
                WarnL << "[Bosch:" << self->m_cfg.id << "] Login error: " << ex.what();
                cb(false, ex.what());
                return;
            }
            if (parser.status() != "200" && parser.status() != "204") {
                cb(false, "Login HTTP " + parser.status());
                return;
            }

            if (!self->parseSetCookie(parser)) {
                cb(false, "Failed to retrieve SESSID from /api/login");
                return;
            }
            InfoL << "[Bosch:" << self->m_cfg.id << "] Login OK, SESSID=" << self->m_sessionId;

            self->verifySession(cb);
        }
    );
    keep(req);
}

void BoschIpSpeaker::disconnect(OnDeviceResult cb) {
    if (m_sessionId.empty()) {
        cb(true, "already disconnected");
        return;
    }

    TraceL << "[Bosch:" << m_cfg.id << "] Logout";

    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("POST");
    req->startRequester(
        m_baseUrl + "/api/logout",
        [weak_self, cb, req](const SockException& ex, const Parser& parser) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            self->m_sessionId.clear();
            self->m_pendingReqs.clear();

            if (ex) {
                WarnL << "[Bosch:" << self->m_cfg.id << "] Logout error: " << ex.what();
                cb(false, ex.what());
                return;
            }
            InfoL << "[Bosch:" << self->m_cfg.id << "] Logout OK";
            cb(true, "logged out");
        }
    );
    keep(req);
}

void BoschIpSpeaker::uploadAudio(const AudioFile& file, OnDeviceResult cb) {
    InfoL << "[Bosch:" << m_cfg.id << "] Upload: " << file.name << " (localId=" << file.id << ")";

    std::string mimeType;
    try {
        mimeType = FileTypeUtil::getMimeType(file.name, DeviceBrand::BOSCH);
    } catch (const std::exception& e) {
        WarnL << "[Bosch:" << m_cfg.id << "] " << e.what();
        cb(false, e.what());
        return;
    }

    GET_CONFIG(std::string, speaker_path, mediakit::Speaker::kSpeakerSavePath);
    GET_CONFIG(std::string, audio_file_dir, mediakit::Speaker::kAudioFilesDir);
    auto audioFilesDir = toolkit::File::absolutePath(audio_file_dir, speaker_path);
    std::string localPath = file.localPath(audioFilesDir);
    std::string label     = file.label();

    uint64_t fileSize = File::fileSize(localPath);
    if (fileSize == 0) {
        cb(false, "File is empty or does not exist: " + localPath);
        return;
    }
    GET_CONFIG(uint64_t, max_file_size, Speaker::kMaxAudioFileSizeBytes);
    if (fileSize > max_file_size) {
        std::string err = "File exceeds the system size limit: "
                            + std::to_string(fileSize)
                            + " bytes > " + std::to_string(max_file_size)
                            + " bytes (2MB)";
        WarnL << "[Bosch:" << m_cfg.id << "] " << err;
        cb(false, err);
        return;
    }
    InfoL << "[Bosch:" << m_cfg.id << "] file size: " << fileSize << " bytes";

    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    // GET /api/info — check freeProjectSpace
    fetchDeviceInfo([weak_self, file, localPath, label, mimeType, fileSize, cb]
                    (bool ok, const std::string& data) {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        if (!ok) {
            cb(false, "Failed to retrieve /api/info: " + data);
            return;
        }

        if (self->m_lastDeviceInfo.freeProjectSpace >= 0 && static_cast<uint64_t>(self->m_lastDeviceInfo.freeProjectSpace) < fileSize) {
            std::string err = "Insufficient speaker storage: free="
                            + std::to_string(self->m_lastDeviceInfo.freeProjectSpace)
                            + " bytes, required=" + std::to_string(fileSize) + " bytes";
            WarnL << "[Bosch:" << self->m_cfg.id << "] " << err;
            cb(false, err);
            return;
        }

        self->uploadChunk(file, mimeType, fileSize, /*chunkIndex=*/0, cb);
    });
}

void BoschIpSpeaker::playAudio(const std::string& remoteId, OnDeviceResult cb) {
    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    putMessage(remoteId, "play", [weak_self, cb](bool ok, const std::string& data) {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        cb(ok, data);
    });
}

void BoschIpSpeaker::stopAudio(const std::string& remoteId, OnDeviceResult cb) {
    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    putMessage(remoteId, "stop", [weak_self, cb](bool ok, const std::string& data) {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        cb(ok, data);
    });
}

void BoschIpSpeaker::deleteAudio(const std::string& remoteId, OnDeviceResult cb) {
    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("DELETE");
    req->startRequester(
        m_baseUrl + "/api/messages/" + remoteId,
        [weak_self, remoteId, cb, req](const SockException& ex,
                                    const Parser& parser) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            if (ex) { cb(false, ex.what()); return; }
            if (parser.status() != "200" && parser.status() != "204") {
                cb(false, "Delete HTTP " + parser.status());
                return;
            }

            InfoL << "[Bosch:" << self->m_cfg.id << "] Deleted audio file: " << remoteId;

            self->fetchDeviceInfo([weak_self, cb](bool, const std::string&) {
                auto self = weak_self.lock();
                if (!self) {
                    return;
                }
                TraceL << "[Bosch:" << self->m_cfg.id << "] freeProjectSpace after deletion: " << self->m_lastDeviceInfo.freeProjectSpace << " bytes";
                cb(true, "deleted");
            });
        }
    );
    keep(req);
}

void BoschIpSpeaker::listAudio(OnDeviceResult cb) {
    auto req = makeReq("GET");
    req->startRequester(
        m_baseUrl + "/api/messages",
        [cb, req](const SockException& ex, const Parser& parser) {
            if (ex) { cb(false, ex.what()); return; }
            if (parser.status() != "200") {
                cb(false, "List HTTP " + parser.status()); return;
            }
            cb(true, parser.content());
        }
    );
    keep(req);
}

HttpRequester::Ptr BoschIpSpeaker::makeReq(const std::string& method, bool withCookie) {
    auto req = std::make_shared<HttpRequester>();
    req->setMethod(method);
    req->addHeader("Content-Type", "application/json");
    req->addHeader("Accept",       "application/json");

    if (withCookie && !m_sessionId.empty()) {
        req->addHeader("Cookie", "SESSID=" + m_sessionId);
    }
    return req;
}

bool BoschIpSpeaker::parseSetCookie(const Parser& parser) {
    auto& headers = parser.getHeader();

    std::string cookieLine;
    for (auto& kv : headers) {
        std::string key = kv.first;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (key == "set-cookie") {
            cookieLine = kv.second;
            break;
        }
    }
    if (cookieLine.empty()) return false;

    auto eq   = cookieLine.find('=');
    auto semi = cookieLine.find(';');
    if (eq == std::string::npos) return false;

    std::string name = cookieLine.substr(0, eq);
    if (name != "SESSID") return false;

    std::string value = (semi == std::string::npos)
        ? cookieLine.substr(eq + 1)
        : cookieLine.substr(eq + 1, semi - eq - 1);

    m_sessionId = value;
    return !m_sessionId.empty();
}

void BoschIpSpeaker::verifySession(OnDeviceResult cb) {
    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("GET");
    req->startRequester(
        m_baseUrl + "/api/session",
        [weak_self, cb, req](const SockException& ex, const Parser& parser) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            if (ex) {
                cb(false, ex.what());
                return;
            }
            if (parser.status() != "200") {
                cb(false, "Session HTTP " + parser.status());
                return;
            }

            Json::Value obj;
            if (!StrJsonUtils::readJsonString(parser.content(), obj)) {
                cb(false, "Parse /api/session failed: " + parser.content());
                return;
            }

            std::string sid = obj.isMember("id") && obj["id"].isString() ? obj["id"].asString() : "";
            bool initialized = obj.isMember("initialized") &&
                                obj["initialized"].isBool() &&
                                obj["initialized"].asBool();

            if (sid.empty() || !initialized) {
                cb(false, "Session invalid: " + parser.content());
                return;
            }

            self->parseSetCookie(parser);

            TraceL << "[Bosch:" << self->m_cfg.id << "] Session verified: " << sid;
            cb(true, "connected");
        }
    );
    keep(req);
}

void BoschIpSpeaker::fetchDeviceInfo(OnDeviceResult cb) {
    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("GET");
    req->startRequester(
        m_baseUrl + "/api/info",
        [weak_self, cb, req](const SockException& ex, const Parser& parser) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            if (ex) { cb(false, ex.what()); return; }
            if (parser.status() != "200") {
                cb(false, "Info HTTP " + parser.status()); return;
            }

            Json::Value obj;
            if (!StrJsonUtils::readJsonString(parser.content(), obj) || !obj.isMember("freeProjectSpace")) {
                cb(false, "Parse freeProjectSpace failed: " + parser.content());
                return;
            }

            self->m_lastDeviceInfo.freeProjectSpace = obj["freeProjectSpace"].asInt64();
            cb(true, parser.content());
        }
    );
    keep(req);
}

void BoschIpSpeaker::uploadChunk(const AudioFile& file, const std::string& mimeType, uint64_t fileSize, size_t chunkIndex, OnDeviceResult cb) {
    size_t totalChunks = static_cast<size_t>(std::ceil(static_cast<double>(fileSize) / kBoschChunkSizeBytes));
    if (totalChunks == 0) totalChunks = 1;

    uint64_t offset = static_cast<uint64_t>(chunkIndex) * kBoschChunkSizeBytes;
    size_t readSize = static_cast<size_t>(std::min<uint64_t>(kBoschChunkSizeBytes, fileSize - offset));

    GET_CONFIG(std::string, speaker_path, mediakit::Speaker::kSpeakerSavePath);
    GET_CONFIG(std::string, audio_file_dir, mediakit::Speaker::kAudioFilesDir);
    auto audioFilesDir = toolkit::File::absolutePath(audio_file_dir, speaker_path);
    std::string localPath = file.localPath(audioFilesDir);
    std::string chunkB64;
    try {
        chunkB64 = readChunkAsBase64(localPath, offset, readSize);
    } catch (const std::exception& e) {
        cb(false, e.what());
        return;
    }

    std::string body =
        "{\"chunks\":"        + std::to_string(totalChunks) + ","
        "\"currentChunk\":"   + std::to_string(chunkIndex)  + ","
        "\"data\":\""         + chunkB64 + "\","
        "\"fileId\":\"\","
        "\"fileName\":\""     + file.name  + "\","
        "\"fileType\":\""     + mimeType       + "\","
        "\"type\":\"FILETYPE_MESSAGE\"}";

    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("POST");
    req->setBody(std::make_shared<HttpStringBody>(body));

    req->startRequester(
        m_baseUrl + "/api/file",
        [weak_self, file, mimeType, fileSize, chunkIndex, totalChunks, cb, req]
        (const SockException& ex, const Parser& parser) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            if (ex) { cb(false, ex.what()); return; }
            if (parser.status() != "200" && parser.status() != "201") {
                cb(false, "Upload chunk " + std::to_string(chunkIndex)
                            + " HTTP " + parser.status()
                            + ": " + parser.content());
                return;
            }

            size_t nextChunk = chunkIndex + 1;
            if (nextChunk < totalChunks) {
                self->uploadChunk(file, mimeType, fileSize, nextChunk, cb);
                return;
            }

            InfoL << "[Bosch:" << self->m_cfg.id << "] Upload file OK (" << totalChunks << " chunk(s))";

            self->createMessage(file.label(), file.name,
                [weak_self, cb](bool ok, const std::string& data) {
                    auto self = weak_self.lock();
                    if (!self) {
                        return;
                    }
                    if (!ok) { cb(false, data); return; }

                    self->fetchDeviceInfo([weak_self, data, cb](bool, const std::string&) {
                        auto self = weak_self.lock();
                        if (!self) {
                            return;
                        }
                        InfoL << "[Bosch:" << self->m_cfg.id
                                << "] freeProjectSpace updated: "
                                << self->m_lastDeviceInfo.freeProjectSpace
                                << " bytes";
                        cb(true, data);
                    });
                }
            );
        }
    );
    keep(req);
}

void BoschIpSpeaker::createMessage(const std::string& label, const std::string& fileName, OnDeviceResult cb) {
    std::string body =
        "{\"file\":null,"
        "\"fileName\":\"" + fileName + "\","
        "\"id\":null,"
        "\"label\":\"" + label + "\","
        "\"locked\":false,"
        "\"type\":\"FILE\","
        "\"url\":null}";

    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("POST");
    req->setBody(std::make_shared<HttpStringBody>(body));

    req->startRequester(
        m_baseUrl + "/api/messages",
        [weak_self, label, fileName, cb, req]
        (const SockException& ex, const Parser& parser) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            if (ex) { cb(false, ex.what()); return; }
            if (parser.status() != "200" && parser.status() != "201") {
                cb(false, "Create message HTTP " + parser.status() + ": " + parser.content());
                return;
            }

            self->resolveMessageId(label, fileName, cb);
        }
    );
    keep(req);
}

void BoschIpSpeaker::resolveMessageId(const std::string& label, const std::string& fileName, OnDeviceResult cb) {
    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("GET");
    req->startRequester(
        m_baseUrl + "/api/messages",
        [weak_self, label, fileName, cb, req]
        (const SockException& ex, const Parser& parser) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            if (ex) { cb(false, ex.what()); return; }
            if (parser.status() != "200") {
                cb(false, "List messages HTTP " + parser.status());
                return;
            }

            Json::Value arr;
            if (!StrJsonUtils::readJsonString(parser.content(), arr) || !arr.isArray()) {
                cb(false, "Parse /api/messages failed: " + parser.content());
                return;
            }

            std::string remoteId;
            for (const auto& obj : arr) {
                if (!obj.isMember("id") || !obj["id"].isString()) continue;

                std::string objFileName = obj.isMember("fileName") &&
                                            obj["fileName"].isString()
                                            ? obj["fileName"].asString() : "";
                std::string objLabel    = obj.isMember("label") &&
                                            obj["label"].isString()
                                            ? obj["label"].asString() : "";

                if (objFileName == fileName && objLabel == label) {
                    remoteId = obj["id"].asString();
                    break;
                }
            }

            if (remoteId.empty()) {
                cb(false, "Failed to find remoteId in /api/messages (label=" + label + ", fileName=" + fileName + ")");
                return;
            }

            TraceL << "[Bosch:" << self->m_cfg.id << "] " << "Resolve message OK -> remoteId=" << remoteId;
            cb(true, remoteId);
        }
    );
    keep(req);
}

void BoschIpSpeaker::putMessage(const std::string& remoteId, const std::string& action, OnDeviceResult cb) {
    // only remoteId is needed
    std::string fileName = "";
    std::string label = "";
    std::string body =
        "{\"fileName\":\""  + fileName  + "\","
        "\"id\":\""         + remoteId  + "\","
        "\"label\":\""      + label     + "\","
        "\"locked\":"       + "false"   + ","
        "\"type\":\""       + "FILE"    + "\"}";

    auto req = makeReq("PUT", /*withCookie=*/true);
    req->setBody(std::make_shared<HttpStringBody>(body));

    req->startRequester(
        m_baseUrl + "/api/messages/" + remoteId + "/" + action,
        [action, remoteId, cb, req](const SockException& ex, const Parser& parser) {
            if (ex) { cb(false, ex.what()); return; }
            if (parser.status() != "200" && parser.status() != "204") {
                cb(false, action + " HTTP " + parser.status()); return;
            }
            cb(true, remoteId);
        }
    );
    keep(req);
}

std::string BoschIpSpeaker::readChunkAsBase64(const std::string& path, uint64_t offset, size_t length) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Failed to open file: " + path);

    f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!f) throw std::runtime_error("Failed to seek to offset " + std::to_string(offset));

    std::string buf(length, '\0');
    f.read(&buf[0], static_cast<std::streamsize>(length));

    std::streamsize got = f.gcount();
    if (static_cast<size_t>(got) != length) {
        throw std::runtime_error("Failed to read chunk: expected " +
            std::to_string(length) + ", read " +
            std::to_string(got));
    }

    return encodeBase64(buf);
}

void BoschIpSpeaker::keep(HttpRequester::Ptr req) {
    m_pendingReqs.push_back(std::move(req));
}

} //namespace managerkit