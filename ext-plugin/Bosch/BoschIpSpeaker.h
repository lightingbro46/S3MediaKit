#ifndef S3MEDIAKIT_BOSCHIPSPEAKER_H
#define S3MEDIAKIT_BOSCHIPSPEAKER_H

#include <memory>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <mutex>
#include "../IAudioPlayback.h"
#include "../IDevice.h"
#include "Http/HttpRequester.h"
#include "Http/HttpBody.h"
#include "Util/logger.h"
#include "Util/File.h"

namespace managerkit {

static constexpr size_t kBoschChunkSizeBytes = 524288; // 512 * 1024
static constexpr const char* kOnvifGroup     = "ONVIF_OPERATOR";

struct BoschUser {
    std::string group;
    std::string id;
    std::string username;
};

using OnUsersResult = std::function<void(bool ok, const std::vector<BoschUser>& users, const std::string& err)>;

class BoschIpSpeaker : public IDevice, public IAudioPlayback, public std::enable_shared_from_this<BoschIpSpeaker> {
public:
    explicit BoschIpSpeaker(const SpeakerConfig& cfg);
    ~BoschIpSpeaker() override = default;

    // Connect to the speaker.
    void connect(OnDeviceResult cb) override;

    // Disconnect from the speaker.
    void disconnect(OnDeviceResult cb) override;

    // Create an ONVIF user on the device.
    void createOnvifUser(const std::string& username, const std::string& password, OnDeviceResult cb) override;
    
    // Upload an audio file.
    void uploadAudio(const AudioFile& file, OnDeviceResult cb) override;

    // Play an audio file.
    void playAudio(const std::string& remoteId, OnDeviceResult cb) override;

    // Stop audio playback.
    void stopAudio(const std::string& remoteId, OnDeviceResult cb) override;

    // Delete an audio file.
    void deleteAudio(const std::string& remoteId, OnDeviceResult cb) override;

    // Get the audio file list.
    void listAudio(OnDeviceResult cb) override;

    // Set the playback volume of the device.
    void setAudioVolume(int volume, OnDeviceResult cb) override;

    // Retrieve the current audio settings of the device.
    AudioSettings getAudioSettings() const override;

    // Get the device information.
    SpeakerDeviceInfo getDeviceInfo() const override;

    // Get the speaker type.
    DeviceType getDeviceType() const override { return DeviceType::Speaker; }

    // Get the speaker brand.
    DeviceBrand getBrand() const override { return m_cfg.brand; }

    // Get the speaker configuration.
    SpeakerConfig getConfig() const override { return m_cfg; }

private:
    // Create an HTTP requester and attach the SESSID cookie if required.
    mediakit::HttpRequester::Ptr makeReq(const std::string& method, bool withCookie = true);

    // Parse the SESSID from the Set-Cookie header.
    bool parseSetCookie(const mediakit::Parser& parser);

    // Verify the current session and refresh the cookie if needed.
    void verifySession(OnDeviceResult cb);

    // Retrieve the available free space from the speaker.
    void fetchDeviceInfo(OnDeviceResult cb);

    // Retrieve the current device settings.
    void fetchDeviceSettings(OnDeviceResult cb);

    // Retrieve the list of users configured on the device.
    void fetchUsers(OnUsersResult cb);

    // Add a new user to the device.
    void addUser(const std::string& username, const std::string& password, const std::string& group, OnDeviceResult cb);

    // Retrieve the current audio settings of the device.
    void fetchAudioSettings(OnDeviceResult cb);

    // Upload a file chunk sequentially.
    void uploadChunk(const AudioFile& file, const std::string& mimeType,
                    uint64_t fileSize, size_t chunkIndex, OnDeviceResult cb);

    // Create a message from the uploaded audio file.
    void createMessage(const std::string& label, const std::string& fileName,
                    OnDeviceResult cb);

    // Resolve the message ID by file name and label.
    void resolveMessageId(const std::string& label, const std::string& fileName,
                        OnDeviceResult cb);

    // Update or execute an action on a message.
    void putMessage(const std::string& remoteId, const std::string& action,
                    OnDeviceResult cb);

    // Read a file chunk and encode it as Base64.
    static std::string readChunkAsBase64(const std::string& path,
                                        uint64_t offset, size_t length);

    // Start `req` and keep it until it answers; it is dropped from m_pendingReqs afterwards.
    void send(const mediakit::HttpRequester::Ptr &req, const std::string &url,
              const mediakit::HttpRequester::HttpRequesterResult &cb);

    // Keep the HTTP requester alive until the request completes.
    void keep(mediakit::HttpRequester::Ptr req);

    // Drop a completed requester from m_pendingReqs.
    void release(const mediakit::HttpRequester::Ptr &req);

    int64_t freeStorageSpace() const;

private:
    SpeakerConfig m_cfg;
    std::string m_baseUrl;
    std::string m_sessionId;
    int64_t m_freeStorageSpace = -1;
    SpeakerDeviceInfo m_deviceInfo;
    AudioSettings m_audioSettings;

    std::vector<mediakit::HttpRequester::Ptr> m_pendingReqs;

    // Request callbacks run on whichever HTTP poller served them and the public methods on the
    // caller's thread: every member below m_cfg/m_baseUrl is guarded by m_mtx.
    mutable std::mutex m_mtx;
};

} //namespace managerkit

#endif // S3MEDIAKIT_BOSCHIPSPEAKER_H