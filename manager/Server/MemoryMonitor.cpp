#include "MemoryMonitor.h"

#include <unistd.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <fstream>
#include <string>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

using namespace std;

namespace managerkit {

struct MemoryStats {
    uint64_t total_physical;     // Tổng RAM hệ thống (bytes)
    uint64_t available_physical; // RAM còn trống/available (bytes)
    uint64_t process_used;       // RAM do process hiện tại sử dụng (bytes)
};

#if defined(__linux__) || defined(__ANDROID__)

static MemoryStats get_system_memory_info() {
    std::ifstream file("/proc/meminfo");
    std::string key;
    uint64_t value;
    std::string unit;
    MemoryStats info = {};

    while (file >> key >> value >> unit) {
        if (key == "MemTotal:") {
            info.total_physical = value * 1024;
        } else if (key == "MemAvailable:") {
            info.available_physical = value * 1024;
        }
        if (info.total_physical && info.available_physical) break;
    }

    // Đọc RSS từ /proc/self/statm
    std::ifstream statm("/proc/self/statm");
    uint64_t total_pages, resident_pages;
    if (statm >> total_pages >> resident_pages) {
        long page_size = sysconf(_SC_PAGESIZE);
        info.process_used = resident_pages * page_size;
    }
    return info;
}

#elif defined(__APPLE__)

static MemoryStats get_system_memory_info() {
    MemoryStats info = {};
    int64_t memsize = 0;
    size_t len = sizeof(memsize);
    sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0);
    info.total_physical = memsize;

    vm_statistics64_data_t vm_stat;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm_stat, &count);

    uint64_t free_pages = vm_stat.free_count + vm_stat.inactive_count;
    info.available_physical = free_pages * sysconf(_SC_PAGESIZE);

    // RSS của process hiện tại
    task_basic_info_data_t task_info;
    mach_msg_type_number_t tcount = TASK_BASIC_INFO_COUNT;
    if (task_info_t ti = reinterpret_cast<task_info_t>(&task_info);
        task_info(mach_task_self(), TASK_BASIC_INFO, ti, &tcount) == KERN_SUCCESS)
    {
        info.process_used = task_info.resident_size;
    }
    return info;
}

#elif defined(_WIN32)

static MemoryStats get_system_memory_info() {
    MemoryStats info = {};
    MEMORYSTATUSEX mem = { sizeof(mem) };
    GlobalMemoryStatusEx(&mem);
    info.total_physical = mem.ullTotalPhys;
    info.available_physical = mem.ullAvailPhys;

    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryStats(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        info.process_used = pmc.WorkingSetSize;
    }
    return info;
}

#else

static MemoryStats get_system_memory_info() {
    GlobalMemoryStatusEx(&mem);
    return MemoryStats {
        .total_physical = 0,
        .available_physical = 0
        .process_used = 0
    };
}

#endif

void MemoryCollector::collect() {
    MemoryStats info = get_system_memory_info();
    _info.totalMemory = info.total_physical;
    _info.usageMemory = info.total_physical - info.available_physical;
    _info.procUsageMemory = info.process_used;
    if (_info.totalMemory == 0) {
        _info.usagePct = 0.0;
        _info.procUsagePct = 0.0;
    } else {
        _info.usagePct = 100 * ((float)_info.usageMemory / _info.totalMemory);
        _info.procUsagePct = 100 * ((float)_info.procUsageMemory / _info.totalMemory);
    }
    onCollect(_info);
    return ;
}

void MemoryMonitor::start() {
    _collector = std::make_shared<MemoryCollector>(_poller);
    _collector->setOnCollect([&](MemoryInfo &info) { 
        lock_guard<mutex> lck(_mtx);
        _info = info;
        emitSystemAlert(_info.usagePct);
    });
}

MemoryInfo MemoryMonitor::getCurrentUsage() {
    lock_guard<mutex> lck(_mtx);
    return _info;
}

} // namespace managerkit
