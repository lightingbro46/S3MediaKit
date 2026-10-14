#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "Common/config.h"
#include "Local/SearchEngine.h"
#include "Local/TimeDemuxer.h"
#include "Local/TimeQuery.h"
#include "Local/TimeMuxer.h"
#include "Storage/DbStorage.h"
#include "Storage/MigrationHistory.h"
#include "Storage/StoragePool.h"
#ifdef ENABLE_MOTION
#include "Motion/MotionMuxer.h"
#endif
#include "Util/File.h"
#include "Util/local_time.h"
#include "Util/MD5.h"
#include "Util/mini.h"
#include "Util/util.h"

using namespace managerkit;
using namespace mediakit;
using namespace toolkit;

// SearchEngine::findTimePeriod (/media/esc/recordedTimePeriod) over generated timefiles, tier ranges
// and motion files. The JSON of every periodType/detail/motion/statusGaps combination is pinned by
// MD5 so the timeline optimisation (docs/timeline-query-optimization-plan.md) cannot change a
// response. DISABLED_LargeRetention measures a 6-month camera; run it by hand.

namespace {

const uint64_t kBase = 1767225600; // 2026-01-01 00:00:00 UTC
const uint64_t kDay = 86400;
const char *kStreams[] = {"main", "sub"};
std::string g_root;

class TimelineQueryEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        // Dates and hours in the response are local time; pin it.
        setenv("TZ", "UTC", 1);
        tzset();
        // getTimeStr() caches the zone at static init; reload it.
        toolkit::local_time_init();
        char tmpl[] = "/tmp/s3mk_timeline_query_XXXXXX";
        char *dir = mkdtemp(tmpl);
        ASSERT_NE(nullptr, dir);
        g_root = dir;
        mINI::Instance()[Database::kDbSavePath] = g_root;
        mINI::Instance()[Protocol::kMP4SavePath] = g_root + "/record";
        mINI::Instance()[General::kMediaServerId] = "ms-local";
        std::make_shared<MigrationHistoryImp>(Database::kMediaServerDb)->migrate(S3_MSERVER_MIGRATION_DIR);
        std::make_shared<MigrationHistoryImp>(Database::kEdgeStorageControllerDb)->migrate(S3_ESC_MIGRATION_DIR);

        StoragePool hot;
        hot.id = "pool-hot";
        hot.name = "hot";
        hot.type = "LOCAL_DISK";
        hot.tier = "HOT";
        ASSERT_TRUE(StoragePoolImp().add(hot));
        StoragePool cold;
        cold.id = "pool-cold";
        cold.name = "cold";
        cold.type = "MINIO";
        cold.tier = "COLD";
        ASSERT_TRUE(StoragePoolImp().add(cold));
    }

    void TearDown() override {
        if (!g_root.empty()) {
            std::string cmd = "rm -rf '" + g_root + "'";
            (void)std::system(cmd.c_str());
        }
    }
};

::testing::Environment *const g_env = ::testing::AddGlobalTestEnvironment(new TimelineQueryEnvironment);

std::string cameraDir(const std::string &camera) {
    GET_CONFIG(std::string, record_path, Protocol::kMP4SavePath);
    GET_CONFIG(std::string, app_name, Record::kAppName);
    return File::absolutePath(app_name, record_path) + "/" + camera;
}

TimeBlock makeBlock(const std::string &camera, const std::string &stream, uint64_t start, uint32_t len, bool replay) {
    TimeBlock block;
    block.set_app(camera);
    block.set_stream(stream);
    block.set_start_time(start);
    block.set_time_len(len);
    block.set_file_size(1024 * 1024);
    block.set_file_path(camera + "/" + stream + "/" + std::to_string(start) + ".mp4");
    block.set_is_replay(replay);
    return block;
}

// One day file per day, both streams interleaved minute by minute like a live recorder. Gaps,
// short segments, replay hours and a gap-fill block appended out of order make the merge paths run.
void writeTimefiles(const std::string &camera, int days) {
    auto dir = cameraDir(camera);
    File::create_path(dir + "/", 0755);
    for (int d = 0; d < days; ++d) {
        auto day = kBase + d * kDay;
        TimeMuxer muxer;
        muxer.openFile(dir + "/" + getTimeStr("%Y-%m-%d", day) + ".s3db");
        for (int m = 0; m < 1440; ++m) {
            auto start = day + m * 60;
            int hour = m / 60;
            uint32_t len = (m % 240 == 7) ? 30 : 60;
            if (!(d % 3 == 1 && hour == 3)) {
                muxer.inputBlock(makeBlock(camera, "main", start, len, d % 5 == 0 && hour == 20));
            }
            if (!(d % 2 == 0 && m >= 600 && m < 660)) {
                muxer.inputBlock(makeBlock(camera, "sub", start, len, false));
            }
        }
        if (d % 3 == 1) {
            muxer.inputBlock(makeBlock(camera, "main", day + 3 * 3600 + 600, 60, false));
        }
        muxer.closeFile();
    }
}

// One range per stream and hour (the split granularity), COLD on object storage except the last
// two days; hour 6 has no range (implicit HOT) and one range is DELETED.
void writeTierRanges(const std::string &camera, int days) {
    std::string sql;
    for (int d = 0; d < days; ++d) {
        for (int h = 0; h < 24; ++h) {
            if (h == 6) {
                continue;
            }
            for (auto stream : kStreams) {
                auto start = kBase + d * kDay + h * 3600;
                bool hot = d >= days - 2;
                std::string status = (d == 0 && h == 5) ? "DELETED" : "AVAILABLE";
                sql += StrPrinter << "INSERT INTO segment_tier_ranges (range_id, camera_id, stream_id, tier, pool_id, "
                                     "origin_pool_id, status, start_time, end_time, segment_count, size_bytes, "
                                     "created_at, updated_at) VALUES ('"
                                  << camera << "-" << stream << "-" << start << "', '" << camera << "', '" << stream
                                  << "', '" << (hot ? "HOT" : "COLD") << "', '" << (hot ? "pool-hot" : "pool-cold")
                                  << "', 'pool-hot', '" << status << "', " << start << ", " << start + 3600
                                  << ", 60, 62914560, " << start << ", " << start << ");\n";
            }
        }
    }
    auto executor = std::make_shared<SqliteQueryExecutor>(Database::kMediaServerDb);
    auto txn = executor->beginWriteTxn();
    txn->execScript(sql);
    txn->commit();
}

#ifdef ENABLE_MOTION
// A 5-minute motion summary every two hours.
void writeMotion(const std::string &camera, int days) {
    GET_CONFIG(std::string, archive_name, Record::kArchiveName);
    auto dir = cameraDir(camera) + "/" + archive_name + "/motion/";
    File::create_path(dir, 0755);
    for (int d = 0; d < days; ++d) {
        auto day = kBase + d * kDay;
        auto date = getTimeStr("%Y-%m-%d", day);
        MotionEventWriter writer;
        ASSERT_TRUE(writer.open(dir + date + ".mblk", dir + date + ".idx"));
        for (int h = 0; h < 24; h += 2) {
            auto start_ms = (day + h * 3600 + 900) * 1000;
            MotionSummaryExtHeader ext{};
            ext.end_stamp = start_ms + 300 * 1000;
            ext.rows = 1;
            ext.cols = 1;
            ext.active_cells = 1;
            writer.appendSummary(MotionSummaryBlock(start_ms, ext, std::vector<uint8_t>(1, 1)));
        }
        writer.flush();
        writer.close();
    }
}
#endif

void buildCamera(const std::string &camera, int days) {
    writeTimefiles(camera, days);
    writeTierRanges(camera, days);
#ifdef ENABLE_MOTION
    writeMotion(camera, days);
#endif
}

Json::Value findTimePeriod(const std::string &camera, uint64_t start, uint64_t end, int period_type, int detail,
                           bool motion, bool status) {
    MediaTuple tuple = {DEFAULT_VHOST, camera, "", ""};
    Json::Value out;
    bool called = false;
    SearchEngine::findTimePeriod(tuple, start, end, period_type, detail, motion, status, "", false,
                                 [&](const SockException &ex, const Json::Value &data) {
                                     EXPECT_FALSE(ex) << ex.what();
                                     out = data;
                                     called = true;
                                 });
    EXPECT_TRUE(called);
    return out;
}

// Stream order follows unordered_map iteration and carries no meaning; sort it away.
void canonicalize(Json::Value &v) {
    if (v.isObject()) {
        for (const auto &key : v.getMemberNames()) {
            canonicalize(v[key]);
        }
    } else if (v.isArray()) {
        std::vector<Json::Value> items;
        for (auto &item : v) {
            canonicalize(item);
            items.push_back(item);
        }
        if (!items.empty() && items[0].isObject() && items[0].isMember("streamId") && !items[0].isMember("startTime")) {
            std::stable_sort(items.begin(), items.end(), [](const Json::Value &a, const Json::Value &b) {
                return a["streamId"].asString() < b["streamId"].asString();
            });
            v = Json::arrayValue;
            for (auto &item : items) {
                v.append(item);
            }
        }
    }
}

std::string canonicalJson(Json::Value v) {
    canonicalize(v);
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, v);
}

// Fixed after the response format was checked once; any change of a response breaks these.
const std::map<std::string, std::string> kGolden = {
    {"full/t0/d0/m0/s0", "f456b5dfd6903d485038bcc8dd25a82b"},
    {"full/t0/d0/m0/s1", "07433c42e28fbc1c3687383f105ef225"},
    {"full/t0/d0/m1/s0", "dd8fbde2d27afffce7e4982ee885392d"},
    {"full/t0/d0/m1/s1", "11b1abc4b76436316b241ed32180de06"},
    {"full/t0/d1/m0/s0", "f456b5dfd6903d485038bcc8dd25a82b"},
    {"full/t0/d1/m0/s1", "07433c42e28fbc1c3687383f105ef225"},
    {"full/t0/d1/m1/s0", "dd8fbde2d27afffce7e4982ee885392d"},
    {"full/t0/d1/m1/s1", "11b1abc4b76436316b241ed32180de06"},
    {"full/t1/d0/m0/s0", "91d377531740d0a4b9163037e50a81ff"},
    {"full/t1/d0/m0/s1", "91d377531740d0a4b9163037e50a81ff"},
    {"full/t1/d0/m1/s0", "4eb820cf6d145fb576cc12b220a652f3"},
    {"full/t1/d0/m1/s1", "4eb820cf6d145fb576cc12b220a652f3"},
    {"full/t1/d1/m0/s0", "2aaca43fbe024740639fd0b2d3b2563b"},
    {"full/t1/d1/m0/s1", "d54828f59c8f72c0162d39f4e26c9465"},
    {"full/t1/d1/m1/s0", "3d94abe09ffc9ddf0adae72f180a7bfb"},
    {"full/t1/d1/m1/s1", "7bccf806026e8352577262f541f4b37b"},
    {"full/t2/d0/m0/s0", "8854b8a6362a680fe26f20c0215123e0"},
    {"full/t2/d0/m0/s1", "8854b8a6362a680fe26f20c0215123e0"},
    {"full/t2/d0/m1/s0", "d250304534114096b15a66fb12735aa4"},
    {"full/t2/d0/m1/s1", "d250304534114096b15a66fb12735aa4"},
    {"full/t2/d1/m0/s0", "f8fc30a72f556beef4d5b7c2b5b0ea00"},
    {"full/t2/d1/m0/s1", "74faee021fb6d4c9bc1044fab364b73d"},
    {"full/t2/d1/m1/s0", "8df1d8c5f5c5203ae35f76e7e88bf434"},
    {"full/t2/d1/m1/s1", "33a258597ce012939242477dc42517ae"},
    {"partial/t0/d0/m0/s0", "9bd7b8a9561d23c74b93fca49a1be710"},
    {"partial/t0/d0/m0/s1", "6a995823302acc386af3e7945477ca74"},
    {"partial/t0/d0/m1/s0", "84b32dc70e1fa9c8332f6fb1757896fd"},
    {"partial/t0/d0/m1/s1", "acb294d4ff669a8e2a79bd4b5f6c5ea5"},
    {"partial/t0/d1/m0/s0", "9bd7b8a9561d23c74b93fca49a1be710"},
    {"partial/t0/d1/m0/s1", "6a995823302acc386af3e7945477ca74"},
    {"partial/t0/d1/m1/s0", "84b32dc70e1fa9c8332f6fb1757896fd"},
    {"partial/t0/d1/m1/s1", "acb294d4ff669a8e2a79bd4b5f6c5ea5"},
    {"partial/t1/d0/m0/s0", "ac5bfd7a367d5d59ed968c54666a878c"},
    {"partial/t1/d0/m0/s1", "ac5bfd7a367d5d59ed968c54666a878c"},
    {"partial/t1/d0/m1/s0", "7b38aedd2237771140a0419acc3d24e9"},
    {"partial/t1/d0/m1/s1", "7b38aedd2237771140a0419acc3d24e9"},
    {"partial/t1/d1/m0/s0", "83e828598e1900b0c9d9641e36f466cc"},
    {"partial/t1/d1/m0/s1", "19e982ce0a3c11f43831097f0e7c19f0"},
    {"partial/t1/d1/m1/s0", "80457f94c9b43396adf71df690891613"},
    {"partial/t1/d1/m1/s1", "1797a2048b5bcd7a7288da3b152cdcc0"},
    {"partial/t2/d0/m0/s0", "0f0697b596072ffcaf121269225b912b"},
    {"partial/t2/d0/m0/s1", "0f0697b596072ffcaf121269225b912b"},
    {"partial/t2/d0/m1/s0", "0f0e5db3c388cad4bf235d6ccb4c551f"},
    {"partial/t2/d0/m1/s1", "0f0e5db3c388cad4bf235d6ccb4c551f"},
    {"partial/t2/d1/m0/s0", "f06325531cec4b5923655df2adae4f9a"},
    {"partial/t2/d1/m0/s1", "aa3c05d0a3c63c076630b400efc49c23"},
    {"partial/t2/d1/m1/s0", "a116de00e74e33fd4256b501efcd15bb"},
    {"partial/t2/d1/m1/s1", "b0f63c7180db46c9a6d7b4bfa9412e66"},
    {"day2/t0/d0/m0/s0", "91b420642acaae3172a4c6665ddd4bf3"},
    {"day2/t0/d0/m0/s1", "3176cdd17de436b86e451659c0d5caae"},
    {"day2/t0/d0/m1/s0", "3ea4b4da6440c90d266484ad4651accd"},
    {"day2/t0/d0/m1/s1", "9127eb087201d0326f36f3a247288d3a"},
    {"day2/t0/d1/m0/s0", "91b420642acaae3172a4c6665ddd4bf3"},
    {"day2/t0/d1/m0/s1", "3176cdd17de436b86e451659c0d5caae"},
    {"day2/t0/d1/m1/s0", "3ea4b4da6440c90d266484ad4651accd"},
    {"day2/t0/d1/m1/s1", "9127eb087201d0326f36f3a247288d3a"},
    {"day2/t1/d0/m0/s0", "b6a344a70ade83a19b26bd5cca85e67e"},
    {"day2/t1/d0/m0/s1", "b6a344a70ade83a19b26bd5cca85e67e"},
    {"day2/t1/d0/m1/s0", "c3e4f3c57c1e503d598c1fbc83baddf8"},
    {"day2/t1/d0/m1/s1", "c3e4f3c57c1e503d598c1fbc83baddf8"},
    {"day2/t1/d1/m0/s0", "0e80e986da38a1ee3646ddd2c5c3bcd5"},
    {"day2/t1/d1/m0/s1", "d13885e0413d0cfdabf79beb2ae75b86"},
    {"day2/t1/d1/m1/s0", "33d8eb80d6de28de292e9a7c0c60d350"},
    {"day2/t1/d1/m1/s1", "1ddb88a79f2ff612a2ff58fe64af1e79"},
    {"day2/t2/d0/m0/s0", "7710582d01fd13b184c4348f68803e2b"},
    {"day2/t2/d0/m0/s1", "7710582d01fd13b184c4348f68803e2b"},
    {"day2/t2/d0/m1/s0", "7c29d90879e3e67ea64fe7a6967930c9"},
    {"day2/t2/d0/m1/s1", "7c29d90879e3e67ea64fe7a6967930c9"},
    {"day2/t2/d1/m0/s0", "1e05a8c7e59ddbcbd90cb58b17e5d2ee"},
    {"day2/t2/d1/m0/s1", "99072146eeecf15c63155fc07474b6ed"},
    {"day2/t2/d1/m1/s0", "049f633c46d937b4e265dc34ea5623be"},
    {"day2/t2/d1/m1/s1", "95f479f9e1571281f091359ac3393f57"},
};

} // namespace

TEST(TimelineQueryTest, ResponsesMatchGolden) {
    const std::string camera = "cam-golden";
    buildCamera(camera, 3);

    const char *dump_dir = getenv("TIMELINE_GOLDEN_DUMP");
    struct Window {
        const char *name;
        uint64_t start;
        uint64_t end;
    } windows[] = {
        {"full", kBase, kBase + 3 * kDay},
        {"partial", kBase + 43200 + 123, kBase + kDay + 50000},
        {"day2", kBase + 2 * kDay, kBase + 3 * kDay - 1},
    };
    for (const auto &w : windows) {
        for (int type = 0; type <= 2; ++type) {
            for (int detail = 0; detail <= 1; ++detail) {
                for (int motion = 0; motion <= 1; ++motion) {
                    for (int status = 0; status <= 1; ++status) {
                        std::string key = StrPrinter << w.name << "/t" << type << "/d" << detail << "/m" << motion
                                                     << "/s" << status;
                        auto json = canonicalJson(findTimePeriod(camera, w.start, w.end, type, detail, motion, status));
                        auto md5 = MD5(json).hexdigest();
                        if (dump_dir) {
                            std::string file = key;
                            std::replace(file.begin(), file.end(), '/', '_');
                            std::ofstream(std::string(dump_dir) + "/" + file + ".json") << json;
                        }
                        auto it = kGolden.find(key);
                        if (it == kGolden.end()) {
                            ADD_FAILURE() << "no golden for {\"" << key << "\", \"" << md5 << "\"},";
                        } else {
                            EXPECT_EQ(it->second, md5) << key;
                        }
                    }
                }
            }
        }
    }
}

// Shape checks the golden hashes cannot explain on their own.
TEST(TimelineQueryTest, SplitsTiersAndMarksGaps) {
    const std::string camera = "cam-shape";
    buildCamera(camera, 3);

    auto data = findTimePeriod(camera, kBase, kBase + 3 * kDay, 1, 1, false, false);
    ASSERT_EQ(2U, data["streams"].size());
    for (const auto &stream : data["streams"]) {
        bool cold = false, hot = false;
        for (const auto &p : stream["periods"]) {
            cold |= p["tier"].asString() == "COLD" && p["restore_required"].asBool();
            hot |= p["tier"].asString() == "HOT" && !p["restore_required"].asBool();
            EXPECT_EQ("ms-local", p["mediaServerId"].asString());
        }
        EXPECT_TRUE(cold) << stream["streamId"].asString();
        EXPECT_TRUE(hot) << stream["streamId"].asString();
    }

    // Day 1 main has no recording 03:00-04:00 except the gap-fill minute at 03:10.
    auto hours = findTimePeriod(camera, kBase + kDay, kBase + 2 * kDay, 2, 1, false, false);
    for (const auto &stream : hours["streams"]) {
        if (stream["streamId"].asString() != "main") {
            continue;
        }
        const auto &hour3 = stream["dates"]["2026-01-02"][3];
        ASSERT_EQ(1U, hour3.size());
        EXPECT_EQ(kBase + kDay + 3 * 3600 + 600, hour3[0]["startTime"].asUInt64());
        EXPECT_EQ(60U, hour3[0]["duration"].asUInt());
    }
}

// TimeQuery opened for a window reads only the day files around it, yet still sees a block that
// started on the previous day and runs past midnight.
TEST(TimelineQueryTest, WindowedOpenKeepsBlockAcrossMidnight) {
    const std::string camera = "cam-window";
    auto dir = cameraDir(camera);
    File::create_path(dir + "/", 0755);
    for (int d = 0; d < 5; ++d) {
        auto day = kBase + d * kDay;
        TimeMuxer muxer;
        muxer.openFile(dir + "/" + getTimeStr("%Y-%m-%d", day) + ".s3db");
        muxer.inputBlock(makeBlock(camera, "main", day + 3600, 60, false));
        muxer.inputBlock(makeBlock(camera, "main", day + kDay - 30, 60, false));
        muxer.closeFile();
    }

    const uint64_t start = kBase + 2 * kDay, end = kBase + 2 * kDay + 600;
    MediaTuple tuple = {DEFAULT_VHOST, camera, "", ""};
    TimeQuery query(tuple, "", start, end);
    std::vector<TimeRange> ranges;
    query.getRecordedTimePeriod(start, end, [&](std::vector<TimeRange> &ret) { ranges = ret; });
    ASSERT_EQ(1U, ranges.size());
    EXPECT_EQ(start, ranges[0].startTime);
    EXPECT_EQ(30U, ranges[0].duration);

    // Files outside the window are not read at all.
    std::vector<TimeBlock> blocks;
    std::string error;
    ASSERT_TRUE(query.queryBlocks(kBase, kBase + 5 * kDay, blocks, error)) << error;
    for (const auto &block : blocks) {
        EXPECT_GE(block.start_time(), kBase + kDay) << block.start_time();
        EXPECT_LT(block.start_time(), kBase + 3 * kDay) << block.start_time();
    }
    EXPECT_FALSE(blocks.empty());
}

TEST(TimelineQueryTest, DayFileWindow) {
    const uint64_t day = kBase + 10 * kDay;
    EXPECT_TRUE(dayFileInWindow("/x/2026-01-11.s3db", day, day + 10));
    EXPECT_TRUE(dayFileInWindow("/x/2026-01-10.s3db", day, day + 10));
    EXPECT_FALSE(dayFileInWindow("/x/2026-01-09.s3db", day, day + 10));
    EXPECT_FALSE(dayFileInWindow("/x/2026-01-12.s3db", day, day + 10));
    EXPECT_TRUE(dayFileInWindow("/x/2026-01-12.mblk", day, day + kDay));
    // No window, or a name that is not a date: always opened.
    EXPECT_TRUE(dayFileInWindow("/x/2026-01-01.s3db", day, 0));
    EXPECT_TRUE(dayFileInWindow("/x/archive.s3db", day, day + 10));
}

TEST(TimelineQueryTest, DISABLED_LargeRetention) {
    const std::string camera = "cam-large";
    const char *env_days = getenv("TIMELINE_PERF_DAYS");
    const int days = env_days ? atoi(env_days) : 180;
    Ticker ticker;
    buildCamera(camera, days);
    printf("generated %d days in %d ms\n", days, (int)ticker.elapsedTime());

    const uint64_t end = kBase + days * kDay;
    struct Case {
        const char *name;
        uint64_t start;
        int type, detail, motion;
    } cases[] = {
        {"console 1 day", end - kDay, 2, 1, 1},
        {"console 30 days", end - 30 * kDay, 2, 1, 1},
        {"console all", kBase, 2, 1, 1},
        {"hours all", kBase, 2, 0, 0},
        {"merged all", kBase, 1, 1, 0},
        {"blocks 7 days", end - 7 * kDay, 0, 0, 0},
    };
    for (const auto &c : cases) {
        ticker.resetTime();
        auto data = findTimePeriod(camera, c.start, end, c.type, c.detail, c.motion, false);
        auto query_ms = ticker.elapsedTime();
        ticker.resetTime();
        auto body = data.toStyledString();
        printf("%-16s query %6d ms, toStyledString %5d ms, %zu bytes\n", c.name, (int)query_ms,
               (int)ticker.elapsedTime(), body.size());
    }
}
