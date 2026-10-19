#ifndef S3MANAGERKIT_HEALTHPROBE_H
#define S3MANAGERKIT_HEALTHPROBE_H

#include <cstdint>
#include <string>

namespace managerkit {

/**
 * Small procfs/sysfs readers for the health monitor (host metrics GlobalMonitor does not sample).
 * Reading procfs is not disk IO, so these run on the monitor poller. The parse* functions take
 * the file text so unit tests feed them fixed input.
 */
namespace HealthProbe {

// "/proc/loadavg": first field. False when the text has no number.
bool parseLoadAvg(const std::string &text, double &load1);
bool readLoadAvg(double &load1);

// "/proc/meminfo": SwapTotal / SwapFree in bytes. False when either line is missing.
bool parseSwap(const std::string &text, uint64_t &total_bytes, uint64_t &free_bytes);
bool readSwap(uint64_t &total_bytes, uint64_t &free_bytes);

// "/proc/self/status": Threads line. -1 when missing.
int parseThreads(const std::string &text);
int readThreads();

// Open descriptors of this process (entries of /proc/self/fd) and RLIMIT_NOFILE soft limit.
bool readFdUsage(uint64_t &open_fds, uint64_t &limit);

// rx_errors+tx_errors and rx_dropped+tx_dropped of /sys/class/net/<iface>/statistics.
bool readNetErrorCounters(const std::string &iface, uint64_t &errors, uint64_t &drops);

} // namespace HealthProbe

} // namespace managerkit

#endif // S3MANAGERKIT_HEALTHPROBE_H
