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
    uint64_t idleTime = 0;
    uint64_t totalTime = 0;
    uint64_t processTime = 0;
};

#ifdef _WIN32

int get_cpu_core_count() {
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
}

uint64_t fileTimeToUint64(const FILETIME& ft) {
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

CpuTimes get_cpu_time() {
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

int get_cpu_core_count() {
    int count = 0;
    size_t size = sizeof(count);
    if (sysctlbyname("hw.logicalcpu", &count, &size, nullptr, 0) == 0 && count > 0)
        return count;
    return 1;
}

CpuTimes get_cpu_times() {
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

int get_cpu_core_count() {
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

CpuTimes get_cpu_times() {
    std::ifstream file("/proc/stat");
    std::string line;
    CpuTimes times;

    if (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string cpu;
        uint64_t user, nice, system, idle, iowait, irq, softirq, steal;

        iss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

        times.idleTime = idle + iowait;
        times.totalTime = user + nice + system + idle + iowait + irq + softirq + steal;
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

int get_cpu_core_count() {
    return 1; // fallback
}

CpuTimes get_cpu_times() {
    CpuTimes times;
    return times; // fallback
}

#endif

void CpuCollector::collect() {
    int interval_ms = 100;
    _info.cores = get_cpu_core_count();
    CpuTimes t1 = get_cpu_times();
    _poller->doDelayTask(100, [&]() {
        CpuTimes t2 = get_cpu_times();
        uint64_t idleDiff = t2.idleTime - t1.idleTime;
        uint64_t totalDiff = t2.totalTime - t1.totalTime;
        uint64_t processDiff = t2.processTime - t1.processTime;

        if (totalDiff == 0) {
            _info.usagePct = 0.0;
            _info.procUsagePct = 0.0;
        } else {
            _info.procUsagePct = 100.0 * ((double)processDiff / totalDiff);
            _info.usagePct = 100.0 * (1.0 - (double)idleDiff / totalDiff);
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
    });
}

CpuInfo CpuMonitor::getCurrentUsage() { 
    std::lock_guard<std::mutex> lck(_mtx);
    return _info; 
}

} // namespace managerkit 