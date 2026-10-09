#ifndef COMMON_BLOCKINGPROCESSRUNNER_H
#define COMMON_BLOCKINGPROCESSRUNNER_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace managerkit {

class BlockingProcessCancellation {
public:
    BlockingProcessCancellation();
    void cancel();
    bool cancelled() const;

private:
    friend class BlockingProcessRunner;
    void setProcessGroup(int process_group);
    std::atomic<bool> _cancelled;
    std::mutex _mutex;
    int _process_group;
};

struct BlockingProcessRequest {
    std::string executable;
    std::vector<std::string> argv;
    int timeout_seconds = 0;
    size_t output_limit_bytes = 64 * 1024;
    std::shared_ptr<BlockingProcessCancellation> cancellation;
};

struct BlockingProcessResult {
    int exit_code = -1;
    bool timed_out = false;
    bool cancelled = false;
    bool output_truncated = false;
    std::string standard_output;
    std::string standard_error;
    std::string error;

    bool success() const {
        return error.empty() && !timed_out && !cancelled && exit_code == 0;
    }
};

class IBlockingProcessRunner {
public:
    virtual ~IBlockingProcessRunner() {}
    virtual BlockingProcessResult run(const BlockingProcessRequest &request) = 0;
};

class BlockingProcessRunner : public IBlockingProcessRunner {
public:
    BlockingProcessResult run(const BlockingProcessRequest &request) override;
};

} // namespace managerkit

#endif // COMMON_BLOCKINGPROCESSRUNNER_H
