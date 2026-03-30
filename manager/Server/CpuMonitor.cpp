#include <iostream>

#ifdef _WIN32
#include <windows.h>
#elif __APPLE__
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <mach/host_info.h>
#include <mach/processor_info.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#elif __ANDROID__ || __linux__
#include <fstream>
#include <string>
#include <sstream>
#include <unistd.h>
#include <dirent.h>
#include <regex>
#endif

#include "CpuMonitor.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

struct CpuTimes {
    uint64_t idleTime = 0;     // Idle CPU time (container or host)
    uint64_t totalTime = 0;    // Total CPU time (container or host)
    uint64_t processTime = 0;  // Process CPU time (container or host)
    bool isCgroup = false;     // whether the times are collected from cgroup (container) or host
};

#ifdef _WIN32

static int get_cpu_core_count() {
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
}

static uint64_t fileTimeToUint64(const FILETIME& ft) {
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static CpuTimes get_cpu_time() {
    FILETIME idleTime, kernelTime, userTime;
    CpuTimes times;

    if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        uint64_t idle = fileTimeToUint64(idleTime);
        uint64_t kernel = fileTimeToUint64(kernelTime);
        uint64_t user = fileTimeToUint64(userTime);

        times.idleTime = idle;
        times.totalTime = kernel + user;

        FILETIME create_, exit_, kernel_, user_;
        GetProcessTimes(GetCurrentProcess(), &create_, &exit_, &kernel_, &user_);
        times.processTime = fileTimeToUint64(kernel_) + fileTimeToUint64(user_);
    }
    return times;
}


#elif __APPLE__

static int get_cpu_core_count() {
    int count = 0;
    size_t size = sizeof(count);
    if (sysctlbyname("hw.logicalcpu", &count, &size, nullptr, 0) == 0 && count > 0)
        return count;
    return 1;
}

static CpuTimes get_cpu_times() {
    CpuTimes times;
    mach_msg_type_number_t count;
    processor_info_array_t infoArray;
    natural_t processorCount;

    kern_return_t kr = host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &processorCount, &infoArray, &count);
    if (kr != KERN_SUCCESS) {
        return times;
    }

    uint64_t total = 0, idle = 0;
    for (natural_t i = 0; i < processorCount; ++i) {
        float user = infoArray[(CPU_STATE_MAX * i) + CPU_STATE_USER];
        float system = infoArray[(CPU_STATE_MAX * i) + CPU_STATE_SYSTEM];
        float nice = infoArray[(CPU_STATE_MAX * i) + CPU_STATE_NICE];
        float idle_time = infoArray[(CPU_STATE_MAX * i) + CPU_STATE_IDLE];

        idle += idle_time;
        total += user + system + nice + idle_time;
    }

    vm_deallocate(mach_task_self(), (vm_address_t)infoArray, count * sizeof(integer_t));

    times.idleTime = idle;
    times.totalTime = total;

    task_thread_times_info_data_t info_;
    mach_msg_type_number_t count_ = TASK_THREAD_TIMES_INFO_COUNT;
    task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, (task_info_t)&info_, &count_);
    times.processTime = (info_.user_time.seconds + info_.system_time.seconds) * 1'000'000 +
           (info_.user_time.microseconds + info_.system_time.microseconds);
    return times;
}

#elif __ANDROID__ || __linux__

static bool file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

static int get_cpu_core_count() {
    // Dùng sysconf là an toàn và nhanh
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs > 0) return static_cast<int>(nprocs);

    // (Dự phòng) Đếm thư mục cpuN trong /sys/devices/system/cpu/
    int count = 0;
    DIR* dir = opendir("/sys/devices/system/cpu/");
    if (dir) {
        struct dirent* entry;
        std::regex cpu_regex("^cpu[0-9]+$");
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_DIR && std::regex_match(entry->d_name, cpu_regex)) {
                ++count;
            }
        }
        closedir(dir);
    }
    return count > 0 ? count : 1;
}

static double getCpuLimit() {
    std::ifstream file("/sys/fs/cgroup/cpu.max");

    if (!file.is_open()) {
        // fallback host
        return std::thread::hardware_concurrency();
    }

    std::string quota_str, period_str;
    file >> quota_str >> period_str;

    if (quota_str == "max") {
        return std::thread::hardware_concurrency();
    }

    double quota = std::stod(quota_str);
    double period = std::stod(period_str);

    return quota / period;
}

static CpuTimes get_cpu_times() {
    CpuTimes times;
    // ưu tiên cgroup v2 (Docker / Kubernetes)
    if (file_exists("/sys/fs/cgroup/cpu.stat") && 0) {
        std::ifstream file("/sys/fs/cgroup/cpu.stat");
        std::string key;
        uint64_t value;

        while (file >> key >> value) {
            if (key == "usage_usec") {
                times.totalTime = value; // microseconds
                break;
            }
        }
        times.idleTime = 0; // cgroup v2 không cung cấp idle time, sẽ tính toán dựa trên total time
        times.isCgroup = true;
    } else {
        std::ifstream file("/proc/stat");
        std::string line;

        if (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string cpu;
            uint64_t user, nice, system, idle, iowait, irq, softirq, steal;

            iss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

            times.idleTime = idle + iowait;
            times.totalTime = user + nice + system + idle + iowait + irq + softirq + steal;
        }
        times.isCgroup = false;
    }

    std::ifstream file_("/proc/self/stat");
    std::string line_;
    if (std::getline(file_, line_)) {
        std::istringstream iss(line_);
        std::string token;
        for (int i = 1; i <= 13; ++i) iss >> token; // skip first 13 fields
        uint64_t utime, stime;
        iss >> utime >> stime;
        times.processTime = utime + stime;
    }
    return times;
}

#else

static int get_cpu_core_count() {
    return 1; // fallback
}

static CpuTimes get_cpu_times() {
    CpuTimes times;
    return times; // fallback
}

#endif

void CpuCollector::collect() {
    _info.cores = get_cpu_core_count();
    CpuTimes t1 = get_cpu_times();
    _poller->doDelayTask(1000, [=]() {
        CpuTimes t2 = get_cpu_times();
        if (t1.isCgroup) {
            uint64_t totalDiff = t2.totalTime - t1.totalTime;
            uint64_t processDiff = t2.processTime - t1.processTime;

            if (totalDiff == 0) {
                _info.usagePct = 0.0;
                _info.procUsagePct = 0.0;
            } else {
                // wall time
                double interval_usec = 1e6; // nếu bạn sleep 1s

                long ticks = sysconf(_SC_CLK_TCK);
                double proc_usec = (double)processDiff * interval_usec / ticks;
                double proc_usage_per_core = (proc_usec / totalDiff) * 100.0;
                double cpu_limit = getCpuLimit();
                _info.procUsagePct = proc_usage_per_core / cpu_limit;
                if (_info.procUsagePct > 100.0) {
                    _info.procUsagePct = 100.0;
                }
                double host_usage = (double)totalDiff / interval_usec * 100.0;
                _info.usagePct = host_usage;
            }
        } else {
            uint64_t idleDiff = t2.idleTime - t1.idleTime;
            uint64_t totalDiff = t2.totalTime - t1.totalTime;
            uint64_t processDiff = t2.processTime - t1.processTime;

            if (totalDiff == 0) {
                _info.usagePct = 0.0;
                _info.procUsagePct = 0.0;
            } else {
                _info.procUsagePct = 100.0 * ((float)processDiff / totalDiff);
                _info.usagePct = 100.0 * (1.0 - (float)idleDiff / totalDiff);
            }   
        }
        onCollect(_info);
        return 0;
    });
}

void CpuMonitor::start() {    
    _collector = std::make_shared<CpuCollector>(_poller);
    _collector->setOnCollect([&](CpuInfo &info) { 
        lock_guard<mutex> lck(_mtx);
        _info = info;
        emitSystemAlert(_info.usagePct);
    });
}

CpuInfo CpuMonitor::getCurrentUsage() { 
    std::lock_guard<std::mutex> lck(_mtx);
    return _info; 
}

} // namespace managerkit 