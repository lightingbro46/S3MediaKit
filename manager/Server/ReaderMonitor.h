#ifndef S3MANAGERKIT_READERMONITOR_H
#define S3MANAGERKIT_READERMONITOR_H

#include "ResourceMonitor.h"

namespace managerkit {

using ReaderCountInfo = std::pair<int /*live_count*/, int /*record_count*/>;
using ReaderCountInfoMap = std::unordered_map<std::string, ReaderCountInfo>;

class ReaderMonitor : public ResourceMonitor {
public:
    using Ptr = std::shared_ptr<ReaderMonitor>;

    ReaderMonitor(toolkit::EventPoller::Ptr poller) : ResourceMonitor(ResourceType::READER, poller) {
        start();
    }

    ~ReaderMonitor();

    ReaderCountInfoMap getCurrentUsage();

    void setStreamReaderCount(const std::string &camera_id, int reader_count, bool record = false);

    bool isReaderCountLimit(const std::string &camera_id, bool record = false);

    void setStreamReaderThreshold(int warning_threshold = -1, int critical_threshold = -1);

    int totalReaderCount();

    int totalReaderCount(const std::string &camera_id);

    bool isReaderCountAvailable(const std::string &camera_id);

    bool isReaderCountAvailable();

private:
    void start() override;

    std::unordered_map<std::string, int> totalEachReaderCount();

    void emitStreamReaderAlert(const std::string &camera_id, int usage_count);

private:
    std::shared_ptr<std::atomic<bool>> _alive_flag;
    toolkit::Timer::Ptr _timer;
    ReaderCountInfoMap _map_reader;
    std::atomic<int> _total_reader {0};
    int _stream_reader_warning_threshold = -1;
    int _stream_reader_critical_threshold = -1;
};

} // namespace managerkit

#endif // S3MANAGERKIT_READERMONITOR_H