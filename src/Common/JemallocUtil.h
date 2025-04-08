#ifndef S3MEDIAKIT_JEMALLOCUTIL_H
#define S3MEDIAKIT_JEMALLOCUTIL_H
#include <functional>
#include <string>
#include <cstdint>
namespace mediakit {
class JemallocUtil {
public:
    static void enable_profiling();

    static void disable_profiling();

    static void dump(const std::string &file_name);
    static std::string get_malloc_stats();
    static void some_malloc_stats(const std::function<void(const char *, uint64_t)> &fn);
};
} // namespace mediakit
#endif // S3MEDIAKIT_JEMALLOCUTIL_H
