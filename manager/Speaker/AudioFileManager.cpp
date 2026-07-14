#include "AudioFileManager.h"
#include "Common/StrUtil.h"
#include "Common/config.h"
#include "Thread/WorkThreadPool.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

// ################### FileTypeUtil ###########################
std::string FileTypeUtil::getMimeType(const std::string& fileName, DeviceBrand brand) {
    std::string ext = getExtension(fileName);

    switch (brand) {
        case DeviceBrand::BOSCH:
            return getMimeTypeBosch(ext);

        default:
            throw std::runtime_error("The speaker brand is not supported in FileTypeUtil");
    }
}

bool FileTypeUtil::isSupported(const std::string& fileName, DeviceBrand brand) {
    try {
        getMimeType(fileName, brand);
        return true;
    } catch (...) {
        return false;
    }
}

std::string FileTypeUtil::getExtension(const std::string& fileName) {
    auto pos = fileName.find_last_of('.');
    if (pos == std::string::npos) return "";

    std::string ext = fileName.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    return ext;
}

std::string FileTypeUtil::getMimeTypeBosch(const std::string& ext) {
    if (ext == "wav")  return "audio/wav";
    if (ext == "mp3")  return "audio/mpeg";
    if (ext == "ogg")  return "audio/ogg";
    if (ext == "opus") return "audio/ogg";

    throw std::runtime_error("Unsupported file format for Bosch speakers: ." + ext +
        "(only .wav, .mp3, .ogg, .opus formats are supported)"
    );
}

// ################### AudioFileManagerHelper ###########################

bool AudioFileManagerHelper::getParams(const std::string &json_str, std::unordered_map<std::string, managerkit::AudioFile> &files) {
    Json::Value root;
    if (!StrJsonUtils::readJsonString(json_str, root)) {
        WarnL << "Parse json string failed";
        return false;
    }

    if (!root.isArray()) {
        WarnL << "Audio files list must be array";
        return false;
    }

    for (const auto &item : root) {
        AudioFile file;

        file.id = item["id"].asString();
        file.name = item["fileName"].asString();
        file.size = item["size"].asUInt64();
        file.soundPath = item["soundPath"].asString();
        file.duration = item["duration"].asFloat();
        file.createdAt = item["createdAt"].asString();
        file.updatedAt = item["updatedAt"].asString();
        file.downloaded = item["downloaded"].asBool();

        files.emplace(file.id, std::move(file));
    }
    return true;
}

std::string AudioFileManagerHelper::getParamsString(const std::unordered_map<std::string, managerkit::AudioFile> &files) {
    Json::Value root(Json::arrayValue);

    for (const auto &it : files) {
        const auto &file = it.second;
        Json::Value item;
        item["id"] = file.id;
        item["fileName"] = file.name;
        item["size"] = file.size;
        item["soundPath"] = file.soundPath;
        item["duration"] = file.duration;
        item["createdAt"] = file.createdAt;
        item["updatedAt"] = file.updatedAt;
        item["downloaded"] = file.downloaded;

        root.append(item);
    }

    return root.toStyledString();
}

INSTANCE_IMP(AudioFileManager)

AudioFileManager::AudioFileManager() {
    GET_CONFIG(string, speaker_path, Speaker::kSpeakerSavePath);
    GET_CONFIG(string, audio_file_dir, Speaker::kAudioFilesDir);
    _folder_path = File::absolutePath(audio_file_dir, speaker_path);
    CHECK(!_folder_path.empty(), "File path cannot be empty");
    auto record_file = _folder_path + "/info.txt";
    _recorder = std::make_shared<FileRecorder<DataType, AudioFileManagerHelper>>(record_file);
    if (!_recorder->empty()) {
        load();
    }
}

bool AudioFileManager::addAudioFile(AudioFile audioFile) {
    std::lock_guard<std::mutex> lock(_mtx);
    auto it = _audio_files.find(audioFile.id);
    if (it != _audio_files.end()) {
        audioFile.downloaded = it->second.downloaded;
    }
    _audio_files[audioFile.id] = std::move(audioFile);

    save();
    return true;
}

bool AudioFileManager::delAudioFile(const std::string &fileId) {
    std::lock_guard<std::mutex> lock(_mtx);
    auto it = _audio_files.find(fileId);
    if (it == _audio_files.end()) {
        return false;
    }
    std::string localPath = it->second.localPath(_folder_path);
    deleteLocalFile(localPath);
    _audio_files.erase(it);
    save();
    return true;
}

AudioFile AudioFileManager::getAudioFile(const std::string &fileId) {
    auto it = _audio_files.find(fileId);
    if (it != _audio_files.end()) {
        return it->second;
    }
    return AudioFile{};
}

std::vector<std::string> AudioFileManager::getAllAudioFileIds() {
    if (_audio_files.empty()) {
        return {};
    }

    std::vector<std::string> ids;
    for (const auto &file : _audio_files) {
        ids.push_back(file.first);
    }
    return ids;
}

bool AudioFileManager::save() {
    if (_recorder) {
        _recorder->save(_audio_files);
    }
    return true;
}

void AudioFileManager::syncDownload() {
    if (_downloading.exchange(true)) {
        InfoL << "Download task already running";
        return;
    }

    std::weak_ptr<AudioFileManager> weak_self = shared_from_this();
    WorkThreadPool::Instance().getPoller()->async([weak_self]() {
        auto self = weak_self.lock();
        if (!self) return;
        self->_downloading_it = self->_audio_files.begin();
        self->downloadNext();
    });
}

void AudioFileManager::load() {
    DataType files;
    if (_recorder->load(files)) {
        _audio_files = files;
    }
}

void AudioFileManager::downloadFile(const std::string& sound_path, const std::string &local_save_path, OnDeviceResult cb) {
    if (sound_path.empty())   { cb(false, "Sound path is empty");  return; }
    if (local_save_path.empty()) { cb(false, "Local save path is empty");  return; }

    Broadcast::DownloadFileInvoker invoker = [cb](const std::string& err, const std::string& path) {
        if (!err.empty()) {
            WarnL << "Download failed: " << err;
            cb(false, "");
            return;
        }
        cb(true, path);
    };

    NOTICE_EMIT(BroadcastDownloadAudioFileArgs, Broadcast::kBroadcastDownloadAudioFile, sound_path, local_save_path, invoker);
}

void AudioFileManager::downloadNext() {
    if (_downloading_it == _audio_files.end()) {
        _downloading = false;
        InfoL << "All audio files downloaded";
        return;
    }

    auto file = _downloading_it->second;
    if (file.downloaded) {
        ++_downloading_it;
        downloadNext();
        return;
    }

    _downloading = true;
    auto file_id = file.id;
    auto sound_path = file.soundPath;
    auto local_save_path = _folder_path + "/" + file.name;
    weak_ptr<AudioFileManager> weak_self = shared_from_this();
    downloadFile(sound_path, local_save_path, [weak_self, file_id](bool ok, const std::string& path) {
        auto self = weak_self.lock();
        if (!self) return;
        if (ok) {
            auto it = self->_audio_files.find(file_id);
            if (it != self->_audio_files.end()) {
                it->second.downloaded = true;
                self->save();
            } else {
                WarnL << "Downloaded file not found in audio files list: " << file_id;
                self->deleteLocalFile(path);
            }
        }
        ++self->_downloading_it;
        self->downloadNext();
    });
}

void AudioFileManager::deleteLocalFile(const std::string& localPath) {
    if (localPath.empty()) return;

    File::delete_file(localPath);
    InfoL << "Local file deleted: " << localPath;
}

} // namespace managerkit