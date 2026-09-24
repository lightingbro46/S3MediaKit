#ifndef BACKCHANNEL_BACKCHANNEL_MANAGER_H
#define BACKCHANNEL_BACKCHANNEL_MANAGER_H

#include "Backchannel/BackchannelPusher.h"
#include "Backchannel/BackchannelWsSession.h"
#include "Backchannel/LiveAudioSource.h"
#include "Backchannel/FileAudioSource.h"

#include <unordered_map>
#include <mutex>
#include <memory>
#include <string>

namespace mediakit {

enum class ProducerType { None, Live, File };
enum class StartResult {
    Started,            // Live session started successfully
    DeviceBusy,         // Device is busy
    DeviceNotRegistered // Device is not registered or failed to set up audio backchannel
};

class BackchannelOwnershipTicket {
public:
    using Ptr = std::shared_ptr<BackchannelOwnershipTicket>;

    BackchannelOwnershipTicket(std::string device_id, BackchannelMediaSource::Ptr source,
                                std::shared_ptr<void> ownership, ProducerType type)
        : _device_id(std::move(device_id)), _source(std::move(source)),
          _ownership(std::move(ownership)), _type(type) {}

    BackchannelOwnershipTicket(const BackchannelOwnershipTicket &) = delete;
    BackchannelOwnershipTicket &operator=(const BackchannelOwnershipTicket &) = delete;

    const std::string &deviceId() const { return _device_id; }
    ProducerType type() const { return _type; }
    const BackchannelMediaSource::Ptr &source() const { return _source; }

private:
    std::string _device_id;
    BackchannelMediaSource::Ptr _source;
    std::shared_ptr<void> _ownership;
    ProducerType _type;
};

class BackchannelManager {
public:
    static BackchannelManager& Instance();
    ~BackchannelManager() = default;

    void registerDevice(const std::string& deviceId, const std::string& url);

    void unregisterDevice(const std::string& deviceId);

    bool playFile(const std::string& deviceId, const std::string& filePath);

    StartResult startLiveSession(const std::string& deviceId, const uint32_t& sampleRate, const uint8_t& channels);

    bool pushAudioFrame(const std::string& deviceId, const int16_t* pcmData, size_t numSamples);

    void stopSession(const std::string& deviceId);

    std::string getErrorMsg() { return _errMsg; }

private:
    BackchannelManager();

    BackchannelPusher::Ptr getOrCreatePusher(const std::string& deviceId);

    template <typename SourceType, typename... CtorArgs>
    BackchannelOwnershipTicket::Ptr findOrCreateAndAcquire(const std::string &device_id, ProducerType type, CtorArgs &&...ctor_args) {
        MediaTuple tuple{DEFAULT_VHOST, device_id, "backchannel"};

        if (auto existed = MediaSource::find(BACKCHANNEL_SCHEMA, tuple.vhost, tuple.app, tuple.stream)) {
            auto typed = std::dynamic_pointer_cast<SourceType>(existed);
            if (!typed) {
                TraceL << "Device " << device_id << " is occupied by a source of a different type (" << typeid(*existed).name() << ")";
                return nullptr;
            }

            auto ownership = typed->getOwnership();
            if (!ownership) {
                TraceL << "Device " << device_id << " is busy: an existing source of the same type is owned by another client";
                return nullptr;
            }

            TraceL << "Device " << device_id << ": reusing existing source and successfully acquired ownership";
            return std::make_shared<BackchannelOwnershipTicket>(device_id, std::move(typed), std::move(ownership), type);
        }

        BackchannelMediaSource::Ptr created;
        try {
            created = std::make_shared<SourceType>(tuple, std::forward<CtorArgs>(ctor_args)...);
            created->registerSource();
        } catch (std::exception &ex) {
            WarnL << "Failed to create/register " << typeid(SourceType).name() << " for device " << device_id << ": " << ex.what();
            return nullptr;
        }

        auto ownership = created->getOwnership();
        if (!ownership) {
            return nullptr;
        }

        InfoL << "Device " << device_id << ": created new " << typeid(SourceType).name() << " and successfully acquired ownership";
        return std::make_shared<BackchannelOwnershipTicket>(device_id, std::move(created), std::move(ownership), type);
    }

private:
    std::mutex _mutex;
    std::unordered_map<std::string, std::string> _devices;
    std::unordered_map<std::string, BackchannelPusher::Ptr> _activePushers;
    std::unordered_map<std::string, BackchannelOwnershipTicket::Ptr> _ownershipTickets;
    std::string _errMsg;
};

} // namespace mediakit

#endif // BACKCHANNEL_BACKCHANNEL_MANAGER_H
