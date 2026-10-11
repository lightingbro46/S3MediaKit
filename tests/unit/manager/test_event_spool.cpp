#include <gtest/gtest.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>
#include "Common/EventSpool.h"

using namespace managerkit;

namespace {

class EventSpoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/event_spool_test_XXXXXX";
        ASSERT_NE(nullptr, mkdtemp(tmpl));
        _root = tmpl;
        _dir = _root + "/spool";
    }
    void TearDown() override {
        std::string cmd = "rm -rf '" + _root + "'";
        ASSERT_EQ(0, system(cmd.c_str()));
    }

    EventSpool::Options options(uint64_t max_bytes = 1 << 20, uint64_t segment_bytes = 4096) {
        EventSpool::Options opts;
        opts.dir = _dir;
        opts.maxBytes = max_bytes;
        opts.segmentBytes = segment_bytes;
        opts.maxAgeSec = 3600;
        opts.minFreeBytes = 0;
        return opts;
    }

    static EventRecord record(int i) {
        EventRecord r;
        r.key = "cam-" + std::to_string(i % 3);
        r.headers = { { "schema", "device-event" }, { "seq", std::to_string(i) } };
        r.value = "{\"seq\":" + std::to_string(i) + ",\"pad\":\"" + std::string(100, 'x') + "\"}";
        return r;
    }

    static void appendRange(EventSpool &spool, int from, int to) {
        for (int i = from; i < to; ++i) {
            ASSERT_EQ(EventSpool::AppendResult::Ok, spool.append({ record(i) }));
        }
    }

    // Reads everything pending (without committing) and returns the seq values.
    static std::vector<int> readAll(EventSpool &spool, EventSpool::Position *end = nullptr) {
        std::vector<int> seqs;
        EventSpool::Position pos = spool.checkpoint();
        while (true) {
            std::vector<std::pair<EventSpool::Position, EventRecord>> out;
            EventSpool::Position next;
            spool.read(pos, 7, out, next);
            for (auto &it : out) {
                seqs.push_back(std::stoi(it.second.headers[1].second));
            }
            pos = next;
            if (out.empty()) break;
        }
        if (end) *end = pos;
        return seqs;
    }

    std::vector<std::string> segmentFiles() const {
        std::vector<std::string> ret;
        if (auto d = opendir(_dir.c_str())) {
            while (auto e = readdir(d)) {
                std::string n = e->d_name;
                if (n.compare(0, 4, "seg-") == 0) ret.push_back(n);
            }
            closedir(d);
        }
        std::sort(ret.begin(), ret.end());
        return ret;
    }

    std::string _root;
    std::string _dir;
};

std::vector<int> range(int from, int to) {
    std::vector<int> v;
    for (int i = from; i < to; ++i) v.push_back(i);
    return v;
}

} // namespace

TEST_F(EventSpoolTest, EncodeDecodeRoundTrip) {
    EventRecord r = record(7);
    r.value.push_back('\0');
    r.value += "binary";
    std::string buf;
    EventSpool::encode(r, buf);
    EventRecord back;
    ASSERT_TRUE(EventSpool::decodePayload(buf.data() + 12, buf.size() - 12, back));
    EXPECT_EQ(r.key, back.key);
    EXPECT_EQ(r.headers, back.headers);
    EXPECT_EQ(r.value, back.value);
    EXPECT_FALSE(EventSpool::decodePayload(buf.data() + 12, buf.size() - 13, back));
}

TEST_F(EventSpoolTest, AppendReadCommitInOrder) {
    EventSpool spool(options());
    std::string err;
    ASSERT_TRUE(spool.open(err)) << err;
    EXPECT_TRUE(spool.empty());
    struct stat st;
    ASSERT_EQ(0, stat(_dir.c_str(), &st));
    EXPECT_EQ(0700u, st.st_mode & 0777);

    appendRange(spool, 0, 100);
    EXPECT_EQ(100u, spool.stats().pending);
    EXPECT_GT(segmentFiles().size(), 2u); // 4 KB segments

    EventSpool::Position end;
    EXPECT_EQ(range(0, 100), readAll(spool, &end));
    EXPECT_EQ(spool.end(), end);

    // Deliver the first 40 one batch at a time.
    EventSpool::Position pos = spool.checkpoint();
    for (int done = 0; done < 40;) {
        std::vector<std::pair<EventSpool::Position, EventRecord>> out;
        EventSpool::Position next;
        spool.read(pos, 10, out, next);
        ASSERT_EQ(10u, out.size());
        spool.commit(next, out.size());
        pos = next;
        done += 10;
    }
    EXPECT_EQ(60u, spool.stats().pending);
    EXPECT_EQ(range(40, 100), readAll(spool));

    // Sent segments are deleted once the checkpoint past them is on disk.
    auto before = segmentFiles().size();
    ASSERT_TRUE(spool.flushCheckpoint());
    EXPECT_LT(segmentFiles().size(), before);
    EXPECT_EQ(spool.checkpoint().segment, std::stoull(segmentFiles().front().substr(4, 20)));
}

TEST_F(EventSpoolTest, CrashResumesAtTheFlushedCheckpoint) {
    {
        // Never destroyed: a crash runs no destructor (no checkpoint flush, no segment deletion).
        auto spool = new EventSpool(options());
        std::string err;
        ASSERT_TRUE(spool->open(err)) << err;
        appendRange(*spool, 0, 50);
        std::vector<std::pair<EventSpool::Position, EventRecord>> out;
        EventSpool::Position next;
        spool->read(spool->checkpoint(), 20, out, next);
        spool->commit(next, out.size());
        ASSERT_TRUE(spool->flushCheckpoint());
        // 10 more delivered, then the crash, before the next checkpoint write.
        out.clear();
        spool->read(next, 10, out, next);
        spool->commit(next, out.size());
    }
    EventSpool spool(options());
    std::string err;
    ASSERT_TRUE(spool.open(err)) << err;
    // Only the batch delivered after the last checkpoint comes again.
    EXPECT_EQ(30u, spool.stats().pending);
    EXPECT_EQ(range(20, 50), readAll(spool));

    appendRange(spool, 50, 55);
    EXPECT_EQ(range(20, 55), readAll(spool));
}

TEST_F(EventSpoolTest, DestructorWritesTheCheckpoint) {
    {
        EventSpool spool(options());
        std::string err;
        ASSERT_TRUE(spool.open(err)) << err;
        appendRange(spool, 0, 30);
        std::vector<std::pair<EventSpool::Position, EventRecord>> out;
        EventSpool::Position next;
        spool.read(spool.checkpoint(), 25, out, next);
        spool.commit(next, out.size());
    }
    EventSpool spool(options());
    std::string err;
    ASSERT_TRUE(spool.open(err)) << err;
    EXPECT_EQ(range(25, 30), readAll(spool));
}

TEST_F(EventSpoolTest, DamagedTailIsCut) {
    {
        EventSpool spool(options(1 << 20, 1 << 20));
        std::string err;
        ASSERT_TRUE(spool.open(err)) << err;
        appendRange(spool, 0, 10);
    }
    auto files = segmentFiles();
    ASSERT_EQ(1u, files.size());
    auto path = _dir + "/" + files[0];
    struct stat st;
    ASSERT_EQ(0, stat(path.c_str(), &st));
    // Half-written last record.
    ASSERT_EQ(0, truncate(path.c_str(), st.st_size - 20));
    {
        EventSpool spool(options(1 << 20, 1 << 20));
        std::string err;
        ASSERT_TRUE(spool.open(err)) << err;
        EXPECT_EQ(1u, spool.stats().corrupt);
        EXPECT_EQ(range(0, 9), readAll(spool));
        appendRange(spool, 9, 12); // new segment after the cut one
        EXPECT_EQ(range(0, 12), readAll(spool));
    }
    // Flipped byte in the payload of record 5: records from 5 on in that segment are cut.
    files = segmentFiles();
    path = _dir + "/" + files[0];
    int fd = ::open(path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    std::string one;
    EventSpool::encode(record(0), one);
    off_t at = static_cast<off_t>(one.size() * 5 + 30);
    char c;
    ASSERT_EQ(1, pread(fd, &c, 1, at));
    c ^= 0x55;
    ASSERT_EQ(1, pwrite(fd, &c, 1, at));
    ::close(fd);
    EventSpool spool(options(1 << 20, 1 << 20));
    std::string err;
    ASSERT_TRUE(spool.open(err)) << err;
    EXPECT_EQ(1u, spool.stats().corrupt);
    std::vector<int> expected = range(0, 5);
    for (int i = 9; i < 12; ++i) expected.push_back(i);
    EXPECT_EQ(expected, readAll(spool));
}

TEST_F(EventSpoolTest, OverMaxBytesDropsTheOldestSegment) {
    EventSpool spool(options(8192, 2048));
    std::string err;
    ASSERT_TRUE(spool.open(err)) << err;
    uint64_t dropped_cb = 0;
    spool.setDropCallback([&](uint64_t n, EventSpool::DropReason reason) {
        EXPECT_EQ(EventSpool::DropReason::MaxSize, reason);
        dropped_cb += n;
    });
    appendRange(spool, 0, 200);
    EXPECT_LE(spool.stats().bytes, 8192u);
    EXPECT_GT(spool.stats().droppedRecords, 0u);
    EXPECT_EQ(dropped_cb, spool.stats().droppedRecords);
    auto seqs = readAll(spool);
    ASSERT_FALSE(seqs.empty());
    // The newest survive, contiguous.
    EXPECT_EQ(199, seqs.back());
    EXPECT_EQ(range(seqs.front(), 200), seqs);
    EXPECT_EQ(200u - spool.stats().droppedRecords, seqs.size());
    EXPECT_EQ(seqs.size(), spool.stats().pending);
}

TEST_F(EventSpoolTest, DropCountsOnlyUnsentRecords) {
    EventSpool spool(options(8192, 2048));
    std::string err;
    ASSERT_TRUE(spool.open(err)) << err;
    appendRange(spool, 0, 10);
    std::vector<std::pair<EventSpool::Position, EventRecord>> out;
    EventSpool::Position next;
    spool.read(spool.checkpoint(), 4, out, next);
    spool.commit(next, out.size());
    // Enough to drop the first segment, of which 4 records were sent.
    appendRange(spool, 10, 80);
    auto seqs = readAll(spool);
    EXPECT_EQ(seqs.size(), spool.stats().pending);
    EXPECT_EQ(80u - 4u - spool.stats().droppedRecords, seqs.size());
}

TEST_F(EventSpoolTest, ExpiredSegmentsAreDropped) {
    EventSpool spool(options());
    std::string err;
    ASSERT_TRUE(spool.open(err)) << err;
    appendRange(spool, 0, 100);
    EXPECT_EQ(0u, spool.dropExpired(time(nullptr)));
    EXPECT_EQ(100u, spool.dropExpired(time(nullptr) + 3601));
    EXPECT_TRUE(spool.empty());
    EXPECT_TRUE(segmentFiles().empty());
    appendRange(spool, 100, 105);
    EXPECT_EQ(range(100, 105), readAll(spool));
}

TEST_F(EventSpoolTest, RefusesWhenTheDiskIsAlmostFull) {
    auto opts = options();
    opts.minFreeBytes = ~0ULL >> 1;
    EventSpool spool(opts);
    std::string err;
    ASSERT_TRUE(spool.open(err)) << err;
    EXPECT_EQ(EventSpool::AppendResult::NoSpace, spool.append({ record(0) }));
    EXPECT_TRUE(spool.empty());
}

TEST_F(EventSpoolTest, CommitAcrossSegmentsKeepsCounts) {
    EventSpool spool(options(1 << 20, 1024));
    std::string err;
    ASSERT_TRUE(spool.open(err)) << err;
    appendRange(spool, 0, 60);
    std::vector<std::pair<EventSpool::Position, EventRecord>> out;
    EventSpool::Position next;
    spool.read(spool.checkpoint(), 33, out, next);
    ASSERT_EQ(33u, out.size());
    spool.commit(next, out.size());
    EXPECT_EQ(27u, spool.stats().pending);
    spool.read(next, 100, out, next);
    spool.commit(next, 27);
    EXPECT_TRUE(spool.empty());
    // Once everything is sent, new records start right after.
    appendRange(spool, 60, 63);
    EXPECT_EQ(range(60, 63), readAll(spool));
    ASSERT_TRUE(spool.flushCheckpoint());
    EXPECT_EQ(1u, segmentFiles().size());
}
