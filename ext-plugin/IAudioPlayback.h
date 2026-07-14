#ifndef S3MEDIAKIT_IAUDIOPLAYBACK_H
#define S3MEDIAKIT_IAUDIOPLAYBACK_H

#include <cstdint>
#include <string>

#include "IDevice.h"

namespace managerkit {

struct AudioFile {
    std::string id;
    std::string name;
    uint64_t    size = 0;
    std::string soundPath;
    double      duration = 0;
    std::string createdAt;
    std::string updatedAt;
    bool        downloaded = false;

    std::string localPath(const std::string &audioFilesDir) const {
        return audioFilesDir + "/" + name; 
    }

    std::string label() const {
        auto pos = name.find_last_of('.');
        return (pos == std::string::npos) ? name : name.substr(0, pos);
    }
};

class IAudioPlayback {
public:
    using Ptr = std::shared_ptr<IAudioPlayback>;
    virtual ~IAudioPlayback() = default;

    // Upload an audio file to the device.
    virtual void uploadAudio(const AudioFile& file, OnDeviceResult cb) = 0;

    // Play an uploaded audio file by remoteId.
    virtual void playAudio(const std::string& remoteId, OnDeviceResult cb) = 0;

    // Stop playback of the specified audio file.
    virtual void stopAudio(const std::string& remoteId, OnDeviceResult cb) = 0;

    // Delete an audio file from the device.
    virtual void deleteAudio(const std::string& remoteId, OnDeviceResult cb) = 0;

    // Retrieve the list of audio files stored on the device.
    virtual void listAudio(OnDeviceResult cb) = 0;
};

} //namespace managerkit

#endif // S3MEDIAKIT_IAUDIOPLAYBACK_H