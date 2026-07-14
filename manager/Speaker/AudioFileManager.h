#ifndef SPEAKER_AUDIOFILEMANAGER_H
#define SPEAKER_AUDIOFILEMANAGER_H

#include <string>
#include <algorithm>
#include <stdexcept>

#include "ext-plugin/IAudioPlayback.h"
#include "Local/FileRecorder.h"
#include "Util/File.h"

namespace managerkit {

class FileTypeUtil {
public:
    static std::string getMimeType(const std::string& fileName, managerkit::DeviceBrand brand);

    static bool isSupported(const std::string& fileName, managerkit::DeviceBrand brand);

private:
    static std::string getExtension(const std::string& fileName);

    // Bosch: .wav, .mp3, .ogg, .opus
    static std::string getMimeTypeBosch(const std::string& ext);
};

class AudioFileManagerHelper {
public:
    static bool getParams(const std::string &json_str, std::unordered_map<std::string, managerkit::AudioFile> &files);

    static std::string getParamsString(const std::unordered_map<std::string, managerkit::AudioFile> &files);
};

class AudioFileManager : public std::enable_shared_from_this<AudioFileManager> {
public:
    using Ptr = std::shared_ptr<AudioFileManager>;
    using DataType = std::unordered_map<std::string, managerkit::AudioFile>;

    static AudioFileManager& Instance();
    ~AudioFileManager() = default;

    bool addAudioFile(managerkit::AudioFile file);

    bool delAudioFile(const std::string &fileId);

    managerkit::AudioFile getAudioFile(const std::string &fileId);
    
    std::vector<std::string> getAllAudioFileIds();

    void syncDownload();
    
private:
    AudioFileManager();
    
    void load();

    bool save();
    
    void downloadFile(const std::string& sound_path, const std::string &folder_path, managerkit::OnDeviceResult cb);

    void downloadNext();
    
    void deleteLocalFile(const std::string& localPath);

private:
    std::mutex _mtx;
    std::string _folder_path;
    FileRecorder<DataType, AudioFileManagerHelper>::Ptr _recorder;
    DataType _audio_files;

    // runtime
    std::atomic<bool> _downloading{false};
    DataType::iterator _downloading_it;
};

} // namespace managerkit

#endif // SPEAKER_AUDIOFILEMANAGER_H