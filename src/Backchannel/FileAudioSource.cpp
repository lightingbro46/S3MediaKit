#include "FileAudioSource.h"
#include "Util/logger.h"
#include "Thread/WorkThreadPool.h"

#include <cstring>
#include <stdexcept>

using namespace toolkit;

namespace mediakit {

namespace {
std::string shellEscape(const std::string &s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}
} // namespace

FileAudioSource::FileAudioSource(const MediaTuple &tuple, const std::string &filePath, uint32_t sampleRate)
    : BackchannelMediaSource(tuple)
    , _filePath(filePath)
    , _frameSamples((size_t)(sampleRate * 20 / 1000)) {
    _sampleRate = sampleRate;
    _channels = 1;
}

FileAudioSource::~FileAudioSource() {
    stopProducing();
}

bool FileAudioSource::openPipe() {
    // -ar <_sampleRate>: resample to _sampleRate kHz
    // -ac 1: downmix to mono
    // -f s16le: output raw PCM16 little-endian
    // -loglevel error: suppress non-error messages
    // -i <file>: FFmpeg automatically detects the input codec and container
    std::string cmd = "ffmpeg -loglevel error -i " + shellEscape(_filePath) +
                       " -f s16le -acodec pcm_s16le -ar " + std::to_string(_sampleRate) +
                       " -ac 1 - 2>/dev/null";
    _pipe = popen(cmd.c_str(), "r");
    return _pipe != nullptr;
}

void FileAudioSource::closePipe() {
    if (_pipe) {
        pclose(_pipe);
        _pipe = nullptr;
    }
}

bool FileAudioSource::start(std::string &errMsg) {
    if (!openPipe()) {
        errMsg = "Failed to open FFmpeg pipe for file: " + _filePath;
        return false;
    }

    auto poller = std::dynamic_pointer_cast<EventPoller>(WorkThreadPool::Instance().getExecutor());
    if (!poller) {
        errMsg = "Failed to obtain an EventPoller from WorkThreadPool";
        closePipe();
        return false;
    }

    _running = true;
    _timer = std::make_shared<Timer>(0.02f, [this]() { return tick(); }, poller);
    return true;
}

bool FileAudioSource::tick() {
    if (!_running || !_pipe) {
        return false;
    }

    const size_t byteSize = _frameSamples * sizeof(int16_t);
    std::vector<int16_t> pcm(_frameSamples);

    size_t bytesRead = std::fread(pcm.data(), 1, byteSize, _pipe);
    if (bytesRead == 0) {
        // End of stream (EOF) or FFmpeg error -> stop audio source
        _running = false;
        closePipe();
        if (_onFinished) {
            InfoL << "Finished playing audio file: " << _filePath;
            _onFinished();
        }
        return false;
    }
    if (bytesRead < byteSize) {
        // The last frame is shorter than 20 ms -> pad with silence to required length
        std::memset(reinterpret_cast<uint8_t*>(pcm.data()) + bytesRead, 0, byteSize - bytesRead);
    }

    onWrite(BackchannelFrame::fromPcm(pcm), true);
    return true;
}

void FileAudioSource::stopProducing() {
    _running = false;
    _timer.reset();
    closePipe();
}

} // namespace mediakit