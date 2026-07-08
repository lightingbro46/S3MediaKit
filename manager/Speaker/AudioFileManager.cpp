#include "AudioFileManager.h"
#include "Common/StrUtil.h"
#include "server/WebHook.h"

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

bool AudioFileManagerHelper::getParams(const std::string &json_str, std::vector<AudioFile> &files) {
    Json::Value root;
    if (!StrJsonUtils::readJsonString(json_str, root)) {
        WarnL << "Parse json string failed";
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

        files.emplace_back(std::move(file));
    }
    return true;
}

std::string AudioFileManagerHelper::getParamsString(const std::vector<AudioFile> &files) {
    Json::Value root(Json::arrayValue);

    for (const auto &file : files) {
        Json::Value item;
        item["id"] = file.id;
        item["fileName"] = file.name;
        item["size"] = file.size;
        item["soundPath"] = file.soundPath;
        item["duration"] = file.duration;
        item["createdAt"] = file.createdAt;
        item["createdAt"] = file.updatedAt;
        item["downloaded"] = file.downloaded;

        root.append(item);
    }

    return root.toStyledString();
}

INSTANCE_IMP(AudioFileManager)

AudioFileManager::AudioFileManager() {
    GET_CONFIG(string, speaker_path, Speaker::kSpeakerSavePath);
    GET_CONFIG(string, audio_file_dir, Speaker::kAudioFilesDir);
    _file_path = File::absolutePath(audio_file_dir, speaker_path);
    CHECK(!_file_path.empty(), "File path cannot be empty");
    _file = std::make_shared<FileRecorder<std::vector<AudioFile>, AudioFileManagerHelper>>(_file_path + "/info.txt");
    if (!_file->empty()) {
        load();
    }
}

bool AudioFileManager::addAudioFile(AudioFile audioFile) {
    auto it = std::find_if(_audio_files.begin(), _audio_files.end(),
        [&audioFile](const AudioFile &item)
        {
            return item.id == audioFile.id;
        });

    if (it != _audio_files.end()) {
        audioFile.downloaded = it->downloaded;
        *it = std::move(audioFile);
    } else {
        _audio_files.emplace_back(std::move(audioFile));
    }

    return true;
}

bool AudioFileManager::delAudioFile(const std::string &fileId) {
    auto it = std::find_if(_audio_files.begin(), _audio_files.end(),
        [&fileId](const AudioFile &item)
        {
            return item.id == fileId;
        });

    if (it == _audio_files.end()) {
        return false;
    }
    GET_CONFIG(std::string, speaker_path, Speaker::kSpeakerSavePath);
    GET_CONFIG(std::string, audio_file_dir, Speaker::kAudioFilesDir);
    auto audioFilesDir = File::absolutePath(audio_file_dir, speaker_path);
    std::string localPath = it->localPath(audioFilesDir);
    deleteLocalFile(localPath);
    _audio_files.erase(it);
    return true;
}

AudioFile AudioFileManager::getAudioFile(const std::string &fileId) {
    auto it = std::find_if(
        _audio_files.begin(),
        _audio_files.end(),
        [&fileId](const AudioFile &item)
        {
            return item.id == fileId;
        });

    if (it != _audio_files.end()) {
        return *it;
    }

    return AudioFile{};
}

std::vector<std::string> AudioFileManager::getAllAudioFileIds() {
    if (_audio_files.empty()) {
        return {};
    }

    std::vector<std::string> ids;
    ids.reserve(_audio_files.size());

    for (const auto &file : _audio_files) {
        ids.push_back(file.id);
    }
    return ids;
}

bool AudioFileManager::save() {
    if (_file) {
        _file->save(_audio_files);
    }
    return true;
}

void AudioFileManager::syncDownload() {
    if (_downloading.exchange(true)) {
        InfoL << "Download task already running";
        return;
    }

    WorkThreadPool::Instance().getPoller()->async(
        [this]() {
            downloadNext(0);
        });
}

void AudioFileManager::load() {
    std::vector<AudioFile> files;
    if (_file->load(files)) {
        _audio_files = files;
    }
}

void AudioFileManager::downloadFile(const std::string& url, const std::string& fileName, OnDeviceResult cb) {
    if (url.empty())        { cb(false, "URL is empty");        return; }
    if (fileName.empty())   { cb(false, "File name is empty");  return; }
    if (_file_path.empty()) { cb(false, "File path is empty");  return; }


    std::string localPath = _file_path + "/" + fileName;

    InfoL << "Start downloading file: " << url << " -> " << localPath;
    auto downloader = std::make_shared<HttpDownloader>();

    downloader->setOnResult([downloader, cb, url]
        (const SockException& ex, const std::string& filePath) {
            if (ex) {
                WarnL << "Download failed: " << url << " — " << ex.what();
                cb(false, ex.what());
                return;
            }

            uint64_t size = File::fileSize(filePath);
            if (size == 0) {
                cb(false, "Downloaded file is empty: " + filePath);
                return;
            }

            InfoL << "Download completed: " << filePath << " (" << size << " bytes)";
            cb(true, filePath);
        }
    );

    downloader->startDownload(url, localPath);

    keepDownloader(downloader);
}

void AudioFileManager::downloadNext(size_t index) {
    if (index >= _audio_files.size()) {
        _downloading = false;
        save();
        InfoL << "All audio files downloaded";
        return;
    }

    auto file = _audio_files[index];

    if (file.downloaded) {
        downloadNext(index + 1);
        return;
    }

    GET_CONFIG(string, api_url, Hook::kApiUrl);
    GET_CONFIG(string, download_path, Speaker::kFileDownloadPath);
    std::string url = api_url + "/" + download_path + "/" + file.soundPath;

    downloadFile(url, file.name,
        [this, index](bool ok, const std::string& data)
        {
            if (ok) {
                _audio_files[index].downloaded = true;
                save();
            } else {
                WarnL << "Download failed: " << data;
            }

            downloadNext(index + 1);
        });
}

void AudioFileManager::deleteLocalFile(const std::string& localPath) {
    if (localPath.empty()) return;

    File::delete_file(localPath);
    InfoL << "Local file deleted: " << localPath;
}

void AudioFileManager::keepDownloader(HttpDownloader::Ptr downloader) {
    static std::vector<HttpDownloader::Ptr> pending;
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    pending.push_back(downloader);
    pending.erase(
        std::remove_if(pending.begin(), pending.end(),
            [](const HttpDownloader::Ptr& d) { return d.use_count() <= 1; }),
        pending.end()
    );
}

} // namespace managerkit