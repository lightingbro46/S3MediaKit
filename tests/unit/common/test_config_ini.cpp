#include <gtest/gtest.h>

#include "Common/config.h"
#include "Util/File.h"
#include "Util/mini.h"
#include "Util/util.h"

using namespace mediakit;
using namespace toolkit;

// loadIniConfig() takes the defaults before merging the file, so dumpFile() keeps only what differs.
TEST(ConfigIniTest, SavesOnlyValuesDifferingFromDefaults) {
    const auto path = exeDir() + "config_ini_test.ini";
    File::delete_file(path);

    // Missing file: created, and nothing differs from the defaults yet
    EXPECT_FALSE(loadIniConfig(path.data()));
    mINI created;
    created.parseFile(path);
    EXPECT_TRUE(created.empty());

    File::saveFile("[http]\r\ncharSet=gb2312\r\n[unknown]\r\nkey=1\r\n", path);
    EXPECT_TRUE(loadIniConfig(path.data()));
    auto &ini = mINI::Instance();
    EXPECT_EQ("gb2312", ini[Http::kCharSet]);

    ini[General::kMediaServerId] = "abc";
    ini.dumpFile(path);
    mINI saved;
    saved.parseFile(path);
    EXPECT_EQ(3u, saved.size());
    EXPECT_EQ("gb2312", saved[Http::kCharSet]);
    EXPECT_EQ("1", saved["unknown.key"]);
    EXPECT_EQ("abc", saved[General::kMediaServerId]);
    EXPECT_EQ(saved.end(), saved.find(Http::kRootPath));
    File::delete_file(path);
}
