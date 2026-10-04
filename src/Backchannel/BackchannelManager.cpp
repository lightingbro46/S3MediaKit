#include "BackchannelManager.h"
#include "Util/logger.h"

using namespace toolkit;

namespace mediakit {

INSTANCE_IMP(BackchannelManager)

BackchannelManager::BackchannelManager() {}

void BackchannelManager::registerDevice(const std::string& deviceId, const std::string& url) {
    AudioBackchannelInfo info;
    info.transport = AudioBackchannelInfo::RTSP_ONVIF;
    info.url = url;
    registerDevice(deviceId, info);
}

void BackchannelManager::registerDevice(const std::string& deviceId, const AudioBackchannelInfo& info) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _devices.find(deviceId);
    if (it == _devices.end()) {
        _devices.emplace(deviceId, info);
        InfoL << "Registered device for backchannel: " << deviceId
              << (info.transport == AudioBackchannelInfo::HTTP_ISAPI ? " (ISAPI two-way audio)" : " (RTSP)");
    } else {
        // Update the target if the device already exists
        it->second = info;
    }
}

void BackchannelManager::unregisterDevice(const std::string& deviceId) {
    bool shouldClearSession = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_devices.erase(deviceId) == 0) {
            return;
        }
        shouldClearSession = _activePushers.find(deviceId) != _activePushers.end()
                          || _ownershipTickets.find(deviceId) != _ownershipTickets.end();
    }

    if (shouldClearSession) {
        stopSession(deviceId);
    }

    InfoL << "Unregistered device from backchannel: " << deviceId;
}

IBackchannelPusher::Ptr BackchannelManager::getOrCreatePusher(const std::string& deviceId) {

    auto it = _activePushers.find(deviceId);
    if (it != _activePushers.end()) {
        return it->second;
    }

    auto cfgIt = _devices.find(deviceId);
    if (cfgIt == _devices.end()) {
        _errMsg = "Device ID is not registered: " + deviceId;
        WarnL << _errMsg;
        return nullptr;
    }

    IBackchannelPusher::Ptr pusher;
    if (cfgIt->second.transport == AudioBackchannelInfo::HTTP_ISAPI) {
        pusher = std::make_shared<IsapiTwoWayAudioPusher>(cfgIt->second);
    } else {
        pusher = std::make_shared<BackchannelPusher>(cfgIt->second.url);
    }
    const std::string dID = deviceId;
    pusher->onFailed = [dID](const toolkit::SockException& ex) {
        if (ex.getCustomCode() == IsapiTwoWayAudioPusher::kDeviceBusyCode) {
            WarnL << "Backchannel to " << dID << " failed: device busy (another two-way audio session): " << ex.what();
        } else {
            WarnL << "Backchannel to " << dID << " failed: " << ex.what();
        }
        BackchannelManager::Instance().stopSession(dID);
    };

    _activePushers[deviceId] = pusher;
    pusher->start();
    return pusher;
}

bool BackchannelManager::playFile(const std::string &deviceId, const std::string &filePath) {
    BackchannelOwnershipTicket::Ptr ticket;
    IBackchannelPusher::Ptr pusher;
    FileAudioSource::Ptr fileSource;
    {
        std::lock_guard<std::mutex> lock(_mutex);

        ticket = findOrCreateAndAcquire<FileAudioSource>(deviceId, ProducerType::File, filePath);
        if (!ticket) {
            _errMsg = "Device became busy";
            WarnL << _errMsg;
            return false;
        }

        fileSource = std::dynamic_pointer_cast<FileAudioSource>(ticket->source());
        if (!fileSource) {
            _errMsg = "Invalid FileAudioSource";
            WarnL << _errMsg;
            return false;
        }

        _ownershipTickets[deviceId] = ticket;

        pusher = getOrCreatePusher(deviceId);
        if (!pusher) {
            _ownershipTickets.erase(deviceId);
            return false;
        }
    }

    std::string err;
    if (!fileSource->start(err)) {
        _errMsg = err;
        WarnL << _errMsg;
        stopSession(deviceId);
        return false;
    }

    fileSource->setOnFinished([deviceId]() {
        InfoL << "Finished playing audio file for device: " << deviceId;
        BackchannelManager::Instance().stopSession(deviceId);
    });

    pusher->attachSource(fileSource);
    return true;
}

StartResult BackchannelManager::startLiveSession(const std::string& deviceId, const uint32_t& sampleRate, const uint8_t& channels) {
    BackchannelOwnershipTicket::Ptr ticket;
    IBackchannelPusher::Ptr pusher;
    LiveAudioSource::Ptr liveSource;
    {
        std::lock_guard<std::mutex> lock(_mutex);

        ticket = findOrCreateAndAcquire<LiveAudioSource>(deviceId, ProducerType::Live, sampleRate, channels);
        if (!ticket) {
            return StartResult::DeviceBusy;
        }

        liveSource = std::dynamic_pointer_cast<LiveAudioSource>(ticket->source());

        if (!liveSource) {
            WarnL << "Failed to cast source to LiveAudioSource for device " << deviceId;
            return StartResult::DeviceBusy;
        }

        _ownershipTickets[deviceId] = ticket;

        pusher = getOrCreatePusher(deviceId);
        if (!pusher) {
            _ownershipTickets.erase(deviceId);
            return StartResult::DeviceNotRegistered;
        }
    }

    pusher->onSourceFinished = nullptr;
    pusher->attachSource(liveSource);
    return StartResult::Started;
}

bool BackchannelManager::pushAudioFrame(const std::string& deviceId, const int16_t* pcmData, size_t numSamples) {
    LiveAudioSource::Ptr liveSource;
    {
        std::lock_guard<std::mutex> lock(_mutex);

        auto it = _ownershipTickets.find(deviceId);
        if (it == _ownershipTickets.end()) {
            return false;
        }

        const auto& ticket = it->second;
        if (!ticket || ticket->type() != ProducerType::Live) {
            return false;
        }

        liveSource = std::dynamic_pointer_cast<LiveAudioSource>(ticket->source());
        if (!liveSource) {
            return false;
        }
    }
    liveSource->pushFrame(pcmData, numSamples);
    return true;
}

void BackchannelManager::stopSession(const std::string& deviceId) {
    IBackchannelPusher::Ptr pusher;
    BackchannelMediaSource::Ptr mediaSource;
    {
        std::lock_guard<std::mutex> lock(_mutex);

        auto pusherIt = _activePushers.find(deviceId);
        if (pusherIt != _activePushers.end()) {
            pusher = pusherIt->second;
            _activePushers.erase(pusherIt);
        }

        auto ticketIt = _ownershipTickets.find(deviceId);
        if (ticketIt != _ownershipTickets.end()) {
            const auto& ticket = ticketIt->second;
            if (ticket) {
                mediaSource = ticket->source();
            }

            _ownershipTickets.erase(ticketIt);
        }

        if (!pusher && !mediaSource) {
            return;
        }
    }

    if (mediaSource) {
        mediaSource->stopProducing();
    }

    if (pusher) {
        pusher->stopSession();
    }
}

} // namespace mediakit
