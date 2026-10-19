#include "HealthProbe.h"
#include <cstdlib>
#include <fstream>
#include <sstream>
#if !defined(_WIN32)
#include <dirent.h>
#include <sys/resource.h>
#endif

using namespace std;

namespace managerkit {
namespace HealthProbe {

static bool readText(const string &path, string &out) {
    ifstream in(path, ios::in | ios::binary);
    if (!in) {
        return false;
    }
    stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

static bool readUint(const string &path, uint64_t &out) {
    string text;
    if (!readText(path, text)) {
        return false;
    }
    char *end = nullptr;
    out = strtoull(text.c_str(), &end, 10);
    return end != text.c_str();
}

bool parseLoadAvg(const string &text, double &load1) {
    char *end = nullptr;
    load1 = strtod(text.c_str(), &end);
    return end != text.c_str();
}

bool readLoadAvg(double &load1) {
    string text;
    return readText("/proc/loadavg", text) && parseLoadAvg(text, load1);
}

// Value in kB of "<name>:   1234 kB" lines.
static bool findMeminfoKb(const string &text, const char *name, uint64_t &kb) {
    istringstream in(text);
    string line;
    string prefix = string(name) + ":";
    while (getline(in, line)) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            kb = strtoull(line.c_str() + prefix.size(), nullptr, 10);
            return true;
        }
    }
    return false;
}

bool parseSwap(const string &text, uint64_t &total_bytes, uint64_t &free_bytes) {
    uint64_t total_kb = 0, free_kb = 0;
    if (!findMeminfoKb(text, "SwapTotal", total_kb) || !findMeminfoKb(text, "SwapFree", free_kb)) {
        return false;
    }
    total_bytes = total_kb * 1024;
    free_bytes = free_kb * 1024;
    return true;
}

bool readSwap(uint64_t &total_bytes, uint64_t &free_bytes) {
    string text;
    return readText("/proc/meminfo", text) && parseSwap(text, total_bytes, free_bytes);
}

int parseThreads(const string &text) {
    uint64_t threads = 0;
    // Same "Name:\tvalue" layout as /proc/meminfo.
    if (!findMeminfoKb(text, "Threads", threads)) {
        return -1;
    }
    return (int)threads;
}

int readThreads() {
    string text;
    if (!readText("/proc/self/status", text)) {
        return -1;
    }
    return parseThreads(text);
}

bool readFdUsage(uint64_t &open_fds, uint64_t &limit) {
#if defined(_WIN32)
    return false;
#else
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) {
        return false;
    }
    uint64_t count = 0;
    while (struct dirent *ent = readdir(dir)) {
        if (ent->d_name[0] != '.') {
            ++count;
        }
    }
    closedir(dir);
    // The descriptor opendir held is counted too.
    open_fds = count > 0 ? count - 1 : 0;
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY) {
        limit = 0;
    } else {
        limit = (uint64_t)rl.rlim_cur;
    }
    return true;
#endif
}

bool readNetErrorCounters(const string &iface, uint64_t &errors, uint64_t &drops) {
    const string base = "/sys/class/net/" + iface + "/statistics/";
    uint64_t rx_err = 0, tx_err = 0, rx_drop = 0, tx_drop = 0;
    if (!readUint(base + "rx_errors", rx_err) || !readUint(base + "tx_errors", tx_err)
        || !readUint(base + "rx_dropped", rx_drop) || !readUint(base + "tx_dropped", tx_drop)) {
        return false;
    }
    errors = rx_err + tx_err;
    drops = rx_drop + tx_drop;
    return true;
}

} // namespace HealthProbe
} // namespace managerkit
