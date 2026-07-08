#ifndef SPEAKER_AUDIOFILEMANAGER_H
#define SPEAKER_AUDIOFILEMANAGER_H

#include <string>
#include <algorithm>
#include <stdexcept>

#include "ext-plugin/IAudioPlayback.h"
#include "Http/HttpDownloader.h"
#include "Local/FileRecorder.h"
#include "Util/File.h"

namespace managerkit {

class FileTypeUtil {
public:
    static std::string getMimeType(const std::string& fileName, mediakit::DeviceBrand brand);

    static bool isSupported(const std::string& fileName, mediakit::DeviceBrand brand);

private:
    static std::string getExtension(const std::string& fileName);

    // Bosch: .wav, .mp3, .ogg, .opus
    static std::string getMimeTypeBosch(const std::string& ext);
};

class AudioFileManagerHelper {
public:
    static bool getParams(const std::string &json_str, std::vector<mediakit::AudioFile> &files);

    static std::string getParamsString(const std::vector<mediakit::AudioFile> &files);
};

class AudioFileManager : public std::enable_shared_from_this<AudioFileManager> {
public:
    using Ptr = std::shared_ptr<AudioFileManager>;

    static AudioFileManager& Instance();
    ~AudioFileManager() = default;

    bool addAudioFile(mediakit::AudioFile file);

    bool delAudioFile(const std::string &fileId);

    mediakit::AudioFile getAudioFile(const std::string &fileId);
    
    std::vector<std::string> getAllAudioFileIds();

    bool save();

    void syncDownload();
    
private:
    AudioFileManager();
    
    void load();
    
    void downloadFile(const std::string& url, const std::string& fileName, mediakit::OnDeviceResult cb);
    
    void downloadNext(size_t index);
    
    void deleteLocalFile(const std::string& localPath);

    void keepDownloader(mediakit::HttpDownloader::Ptr downloader);

private:
    std::string _file_path;
    FileRecorder<std::vector<mediakit::AudioFile>, AudioFileManagerHelper>::Ptr _file;
    std::vector<mediakit::AudioFile> _audio_files;
    std::atomic<bool> _downloading{false};
};

} // namespace managerkit

#endif // SPEAKER_AUDIOFILEMANAGER_H