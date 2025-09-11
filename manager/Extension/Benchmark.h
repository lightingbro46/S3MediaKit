#ifndef EXTENSION_BENCHMARK_H
#define EXTENSION_BENCHMARK_H

#include <cmath>
#include <algorithm>
#include "Util/logger.h"

#define SAFE_ULTILIZATION_THRESHOLD 0.7
#define OVERHEAD_FACTOR_NETWORK     1.1  // ~1.05–1.15
#define OVERHEAD_FACTOR_STORAGE     1.15 // ~1.05–1.2
#define OVERHEAD_FACTOR_CPU         1.2  // ~1.1–1.3
#define OVERHEAD_FACTOR_MEMORY      1.1  // ~1.1–1.2
#define OVERHEAD_FACTOR_SOFRWARE    1.1  // ~1.05–1.1
#define AVERAGE_BITRATE_PER_CAMERA  5    // ~1–8    unit: Mbps/camera
#define CPU_MBPS_PER_CORE           150  // ~50–200 unit: Mbps/core, depends on workload
#define MEMORY_MBPS_PER_CAMERA      50   // ~50–200 unit: MB/camera, depends on workload: ~20–50: only record, ~50–100: include motion detect

namespace managerkit {

inline int estimate_network_limit(int net_cap) {
    double bitrate_effective = AVERAGE_BITRATE_PER_CAMERA * OVERHEAD_FACTOR_NETWORK;
    return floor((net_cap * SAFE_ULTILIZATION_THRESHOLD) / bitrate_effective);
}

inline int estimate_disk_limit(int disk_cap) {
    double per_cam_MB_s = AVERAGE_BITRATE_PER_CAMERA * OVERHEAD_FACTOR_NETWORK / 8.0;
    return floor((disk_cap * SAFE_ULTILIZATION_THRESHOLD) / per_cam_MB_s);
}

inline int estimate_cpu_limit(int cpu_cores) {
    double bitrate_effective = AVERAGE_BITRATE_PER_CAMERA * OVERHEAD_FACTOR_NETWORK;
    return floor((cpu_cores * CPU_MBPS_PER_CORE * SAFE_ULTILIZATION_THRESHOLD) / bitrate_effective);
}

inline int estimate_memory_limit(int memory_cap) {
    double per_cam_MB_s = AVERAGE_BITRATE_PER_CAMERA * OVERHEAD_FACTOR_NETWORK / 8.0;
    return floor((memory_cap * MEMORY_MBPS_PER_CAMERA * SAFE_ULTILIZATION_THRESHOLD) / per_cam_MB_s);
}

class Benchmark {
public:
    static int estimateAvailableDevice(int net_cap, int disk_cap, int cpu_cores, int memory_cap) {
        int net_limit = estimate_network_limit(net_cap);
        int disk_limit = estimate_disk_limit(disk_cap);
        int cpu_limit = estimate_cpu_limit(cpu_cores);
        int ram_limit = estimate_memory_limit(memory_cap);
        auto max_device = std::min({net_limit, disk_limit, cpu_limit, ram_limit});
        InfoL << "Estimate available device: " << max_device;
        return max_device;
    }
};

} // namespace managerkit

#endif // EXTENSION_BENCHMARK_H