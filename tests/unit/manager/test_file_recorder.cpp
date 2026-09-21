#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "Local/FileRecorder.h"

using namespace managerkit;

namespace {

// A minimal payload so the test does not drag in protobuf or the statistic entities.
struct Payload {
    std::string text;
};

struct PayloadHelper {
    static bool getParams(const std::string &in, Payload &out) {
        out.text = in;
        return true;
    }
    static std::string getParamsString(const Payload &in) { return in.text; }
};

using Recorder = FileRecorder<Payload, PayloadHelper>;

// How many descriptors of this process point at `path`.
size_t openHandlesOf(const std::string &path) {
    size_t found = 0;
    auto *dir = ::opendir("/proc/self/fd");
    if (!dir) {
        return 0;
    }
    while (auto *entry = ::readdir(dir)) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        char target[4096] = {0};
        auto len = ::readlink(("/proc/self/fd/" + name).c_str(), target, sizeof(target) - 1);
        if (len > 0 && path == std::string(target, len)) {
            ++found;
        }
    }
    ::closedir(dir);
    return found;
}

} // namespace

// Regression: the constructor called File::create_file() for its mkdir -p of the parent directories
// and dropped the FILE* it returns, so creating a record file leaked one descriptor for the life of
// the process — one per camera info.txt, per speaker, per audio file record. Found while attributing
// the descriptors a node keeps after a failover cycle.
TEST(FileRecorderTest, DoesNotLeakTheHandleOfANewlyCreatedFile) {
    const std::string dir = "/tmp/s3mediakit-unit-file-recorder";
    const std::string path = dir + "/nested/info.txt";
    ::system(("rm -rf " + dir).c_str());

    ASSERT_EQ(0U, openHandlesOf(path));
    {
        auto recorder = std::make_shared<Recorder>(path);
        // The file (and its parent directories) exist, but nothing is held open for it.
        EXPECT_TRUE(toolkit::File::fileExist(path.data()));
        EXPECT_EQ(0U, openHandlesOf(path)) << "create_file() handle was not closed";

        // And the record still works: each access opens its own stream.
        Payload written;
        written.text = "{\"camera\":\"cam-0001\"}";
        recorder->save(written);
        EXPECT_FALSE(recorder->empty());
        EXPECT_EQ(0U, openHandlesOf(path)) << "save() left a stream open";

        Payload read;
        ASSERT_TRUE(recorder->load(read));
        EXPECT_EQ(written.text, read.text);
        EXPECT_EQ(0U, openHandlesOf(path)) << "load() left a stream open";
    }

    // An existing file skips the creation branch entirely.
    {
        auto recorder = std::make_shared<Recorder>(path);
        EXPECT_EQ(0U, openHandlesOf(path));
        Payload read;
        ASSERT_TRUE(recorder->load(read));
        EXPECT_EQ("{\"camera\":\"cam-0001\"}", read.text);
    }

    ::system(("rm -rf " + dir).c_str());
}
