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

    // Built with the JSON writer so a password with quotes or backslashes stays one valid string.
    Json::Value obj;
    obj["darkMode"]     = false;
    obj["group"]        = "API";
    obj["id"]           = Json::nullValue;
    obj["languageCode"] = "en";
    obj["password"]     = m_cfg.password;
    obj["username"]     = m_cfg.username;
    std::string body = StrJsonUtils::writeJsonString(obj);

    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("POST", /*withCookie=*/false);
    req->setBody(std::make_shared<HttpStringBody>(body));

    send(req,
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
            InfoL << "[Bosch:" << self->m_cfg.id << "] Login OK";

            self->verifySession([weak_self, cb](bool ok, const std::string& data) {
                auto self = weak_self.lock();
                if (!self) {
                    return;
                }
                if (!ok) {
                    cb(false, "Verify session failed: " + data);
                    return;
                }

                self->fetchDeviceSettings([weak_self, cb](bool infoOk, const std::string& infoData) {
                    auto self = weak_self.lock();
                    if (!self) {
                        return;
                    }
                    if (!infoOk) {
                        cb(false, "Fetch device info failed: " + infoData);
                        return;
                    }
                    
                    self->fetchAudioSettings([weak_self, cb](bool audioOk, const std::string& audioData) {
                        auto self = weak_self.lock();
                        if (!self) {
                            return;
                        }
                        if (!audioOk) {
                            cb(true, "Fetching audio settings failed, but the connection is still considered successful");
                            return;
                        }

                        Json::Value obj;
                        if (!StrJsonUtils::readJsonString(audioData, obj) || !obj.isMember("user_level")) {
                            cb(true, "Fetching audio settings failed, but the connection is still considered successful");
                            return;
                        }
                        {
                            std::lock_guard<std::mutex> lock(self->m_mtx);
                            self->m_audioSettings.minAudioVolume = -80;
                            self->m_audioSettings.maxAudioVolume = 20;
                            self->m_audioSettings.deviceAudioVolume = obj["user_level"].asInt64();
                        }
                        cb(true, "Connect success");
                    });
                });
            });
        }
    );
}

void BoschIpSpeaker::disconnect(OnDeviceResult cb) {
    bool loggedIn;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        loggedIn = !m_sessionId.empty();
    }
    if (!loggedIn) {
        cb(true, "already disconnected");
        return;
    }

    TraceL << "[Bosch:" << m_cfg.id << "] Logout";

    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("POST");
    send(req,
        m_baseUrl + "/api/logout",
        [weak_self, cb, req](const SockException& ex, const Parser& parser) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            std::vector<HttpRequester::Ptr> pending;
            {
                std::lock_guard<std::mutex> lock(self->m_mtx);
                self->m_sessionId.clear();
                pending.swap(self->m_pendingReqs);
            }

            if (ex) {
                WarnL << "[Bosch:" << self->m_cfg.id << "] Logout error: " << ex.what();
                cb(false, ex.what());
                return;
            }
            InfoL << "[Bosch:" << self->m_cfg.id << "] Logout OK";
            cb(true, "logged out");
        }
    );
}

void BoschIpSpeaker::createOnvifUser(const std::string& username, const std::string& password, OnDeviceResult cb) {
    InfoL << "[Bosch:" << m_cfg.id << "] create onvif user";
    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();

    fetchUsers([weak_self, username, password, cb](bool ok, const std::vector<BoschUser>& users, const std::string& err) {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        if (!ok) {
            cb(false, "Fetch users failed: " + err);
            return;
        }

        std::vector<BoschUser> onvifUsers;
        for (const auto& u : users) {
            if (u.group == kOnvifGroup) {
                onvifUsers.push_back(u);
            }
        }

        if (!onvifUsers.empty()) {
            bool duplicated = std::any_of(onvifUsers.begin(), onvifUsers.end(),
                [&username](const BoschUser& u) {
                    return u.username == username;
                });

            if (duplicated) {
                cb(false, "ONVIF user with username: " + username + " already exists");
                return;
            }

            self->addUser(username, password, kOnvifGroup, cb);
            return;
        }

        self->addUser(username, password, kOnvifGroup, cb);
    });
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

        int64_t freeSpace = self->freeStorageSpace();
        if (freeSpace >= 0 && static_cast<uint64_t>(freeSpace) < fileSize) {
            std::string err = "Insufficient speaker storage: free="
                            + std::to_string(freeSpace)
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
    send(req,
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
                TraceL << "[Bosch:" << self->m_cfg.id << "] freeStorageSpace after deletion: " << self->freeStorageSpace() << " bytes";
                cb(true, "deleted");
            });
        }
    );
}

void BoschIpSpeaker::listAudio(OnDeviceResult cb) {
    auto req = makeReq("GET");
    send(req,
        m_baseUrl + "/api/messages",
        [cb, req](const SockException& ex, const Parser& parser) {
            if (ex) { cb(false, ex.what()); return; }
            if (parser.status() != "200") {
                cb(false, "List HTTP " + parser.status()); return;
            }
            cb(true, parser.content());
        }
    );
}

void BoschIpSpeaker::setAudioVolume(int volume, OnDeviceResult cb) {
    std::string body =
        "{\"key\":\"audio/user_level\","
        "\"value\":" + std::to_string(volume) + "}";

    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("PATCH");
    req->setBody(std::make_shared<HttpStringBody>(body));

    send(req,
        m_baseUrl + "/api/key",
        [weak_self, cb, volume, req](const SockException& ex, const Parser& parser) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            if (ex) {
                cb(false, ex.what());
                return;
            }
            if (parser.status() != "200") {
                cb(false, "Change volume HTTP " + parser.status());
                return;
            }
            {
                std::lock_guard<std::mutex> lock(self->m_mtx);
                self->m_audioSettings.deviceAudioVolume = volume;
            }
            cb(true, "Change volume success");
        }
    );
}

HttpRequester::Ptr BoschIpSpeaker::makeReq(const std::string& method, bool withCookie) {
    auto req = std::make_shared<HttpRequester>();
    req->setMethod(method);
    req->addHeader("Content-Type", "application/json");
    req->addHeader("Accept",       "application/json");

    std::string sessionId;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        sessionId = m_sessionId;
    }
    if (withCookie && !sessionId.empty()) {
        req->addHeader("Cookie", "SESSID=" + sessionId);
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

    std::lock_guard<std::mutex> lock(m_mtx);
    m_sessionId = value;
    return !m_sessionId.empty();
}

void BoschIpSpeaker::verifySession(OnDeviceResult cb) {
    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("GET");
    send(req,
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
}

void BoschIpSpeaker::fetchDeviceInfo(OnDeviceResult cb) {
    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("GET");
    send(req,
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

            {
                std::lock_guard<std::mutex> lock(self->m_mtx);
                self->m_freeStorageSpace = obj["freeProjectSpace"].asInt64();
            }
            cb(true, parser.content());
        }
    );
}

void BoschIpSpeaker::fetchDeviceSettings(OnDeviceResult cb) {
    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("GET");
    send(req,
        m_baseUrl + "/api/settings",
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
            if (!StrJsonUtils::readJsonString(parser.content(), obj)
                || !obj.isMember("productName")
                || !obj.isMember("firmwareVersion")
                || !obj.isMember("serialNumber")
                || !obj.isMember("macAddress")) {
                cb(false, "Parse device info failed: " + parser.content());
                return;
            }

            {
                std::lock_guard<std::mutex> lock(self->m_mtx);
                self->m_deviceInfo.manufacturer    = "Bosch";
                self->m_deviceInfo.model           = obj["productName"].asString();
                self->m_deviceInfo.firmwareVersion = obj["firmwareVersion"].asString();
                self->m_deviceInfo.serialNumber    = obj["serialNumber"].asString();
                self->m_deviceInfo.macAddress      = obj["macAddress"].asString();
            }

            cb(true, parser.content());
        }
    );
}

void BoschIpSpeaker::fetchUsers(OnUsersResult cb) {
    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("GET");
    send(req,
        m_baseUrl + "/api/users",
        [weak_self, cb, req](const SockException& ex, const Parser& parser) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            if (ex) {
                cb(false, {}, ex.what());
                return;
            }
            if (parser.status() != "200") {
                cb(false, {}, "Get users HTTP " + parser.status());
                return;
            }

            Json::Value arr;
            if (!StrJsonUtils::readJsonString(parser.content(), arr) || !arr.isArray()) {
                cb(false, {}, "Parse users list failed: " + parser.content());
                return;
            }

            std::vector<BoschUser> users;
            users.reserve(arr.size());
            for (const auto& item : arr) {
                if (!item.isMember("group") || !item.isMember("id") || !item.isMember("username")) continue;

                BoschUser u;
                u.group    = item["group"].asString();
                u.id       = item["id"].asString();
                u.username = item["username"].asString();
                users.push_back(std::move(u));
            }

            cb(true, users, "");
        }
    );
}

void BoschIpSpeaker::addUser(const std::string& username, const std::string& password, const std::string& group, OnDeviceResult cb) {
    // Built with the JSON writer so a password with quotes or backslashes stays one valid string.
    Json::Value obj;
    obj["darkMode"]     = false;
    obj["group"]        = group;
    obj["id"]           = Json::nullValue;
    obj["languageCode"] = "en";
    obj["password"]     = password;
    obj["username"]     = username;
    std::string body = StrJsonUtils::writeJsonString(obj);

    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("POST");
    req->setBody(std::make_shared<HttpStringBody>(body));

    send(req,
        m_baseUrl + "/api/users",
        [weak_self, cb, req](const SockException& ex, const Parser& parser) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            if (ex) {
                cb(false, ex.what());
                return;
            }
            if (parser.status() != "200" && parser.status() != "201" && parser.status() != "204") {
                cb(false, "Add user HTTP " + parser.status());
                return;
            }
            cb(true, "Add user success");
        }
    );
}

void BoschIpSpeaker::fetchAudioSettings(OnDeviceResult cb) {
    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("GET");
    send(req,
        m_baseUrl + "/api/audioSettings",
        [weak_self, cb, req](const SockException& ex, const Parser& parser) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            if (ex) { cb(false, ex.what()); return; }
            if (parser.status() != "200") {
                cb(false, "Info HTTP " + parser.status()); return;
            }

            cb(true, parser.content());
        }
    );
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

    send(req,
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
                        InfoL << "[Bosch:" << self->m_cfg.id << "] freeStorageSpace updated: " << self->freeStorageSpace() << " bytes";
                        cb(true, data);
                    });
                }
            );
        }
    );
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

    send(req,
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
}

void BoschIpSpeaker::resolveMessageId(const std::string& label, const std::string& fileName, OnDeviceResult cb) {
    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    auto req = makeReq("GET");
    send(req,
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

    send(req,
        m_baseUrl + "/api/messages/" + remoteId + "/" + action,
        [action, remoteId, cb, req](const SockException& ex, const Parser& parser) {
            if (ex) { cb(false, ex.what()); return; }
            if (parser.status() != "200" && parser.status() != "204") {
                cb(false, action + " HTTP " + parser.status()); return;
            }
            cb(true, remoteId);
        }
    );
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

AudioSettings BoschIpSpeaker::getAudioSettings() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_audioSettings;
}

SpeakerDeviceInfo BoschIpSpeaker::getDeviceInfo() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_deviceInfo;
}

int64_t BoschIpSpeaker::freeStorageSpace() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_freeStorageSpace;
}

void BoschIpSpeaker::send(const HttpRequester::Ptr &req, const std::string &url, const HttpRequester::HttpRequesterResult &cb) {
    std::weak_ptr<BoschIpSpeaker> weak_self = shared_from_this();
    std::weak_ptr<HttpRequester> weak_req = req;
    keep(req);
    req->startRequester(url, [weak_self, weak_req, cb](const SockException &ex, const Parser &parser) {
        if (auto self = weak_self.lock()) {
            // Before cb: a failure it reports reads the cause of this request.
            self->m_lastFailure = static_cast<int>(classifyFailure(ex.getErrCode(), atoi(parser.status().c_str())));
        }
        cb(ex, parser);
        auto self = weak_self.lock();
        auto req = weak_req.lock();
        if (self && req) {
            self->release(req);
        }
    });
}

DeviceFailure BoschIpSpeaker::classifyFailure(int sock_err, int http_status) {
    // An HTTP answer came back (even when the body then failed): the status says it all.
    if (http_status) {
        if (http_status == 401 || http_status == 403) {
            return DeviceFailure::Unauthorized;
        }
        return http_status / 100 == 2 ? DeviceFailure::None : DeviceFailure::Other;
    }
    switch (sock_err) {
        case Err_success:
            return DeviceFailure::Other;
        // Connected, but no answer: response timeout, or the speaker dropped the connection.
        case Err_timeout:
        case Err_eof:
        case Err_reset:
        case Err_shutdown:
            return DeviceFailure::Timeout;
        // No connection: refused, unknown host, no route.
        default:
            return DeviceFailure::Unreachable;
    }
}

void BoschIpSpeaker::keep(HttpRequester::Ptr req) {
    std::lock_guard<std::mutex> lock(m_mtx);
    m_pendingReqs.push_back(std::move(req));
}

void BoschIpSpeaker::release(const HttpRequester::Ptr &req) {
    HttpRequester::Ptr released;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        auto it = std::find(m_pendingReqs.begin(), m_pendingReqs.end(), req);
        if (it == m_pendingReqs.end()) {
            return;
        }
        // Destroyed outside the lock.
        released = std::move(*it);
        m_pendingReqs.erase(it);
    }
}

} //namespace managerkit