#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "Common/config.h"
#include "FFmpegSource.h"
#include "Transcode/OverlayPrivacyUtils.h"
#include "Util/File.h"
#include "Util/mini.h"

using namespace mediakit;
using namespace toolkit;

namespace {

size_t countOccurrences(const std::string &value, const std::string &needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

class FFmpegOverlayFilterTest : public ::testing::Test {
protected:
    static void TearDownTestSuite() {
        std::remove("/tmp/s3mediakit-ffmpeg-overlay-filter");
    }

    void SetUp() override {
        previous_use_watermark_asset = mINI::Instance()[OverlayPrivacyConfig::kUseWatermarkAsset];
        previous_overlay_root = mINI::Instance()[OverlayPrivacyConfig::kOverlayRoot];

        // GET_CONFIG caches values per call site, so every case in this process
        // must share the first overlay root observed by the production builder.
        overlay_root = "/tmp/s3mediakit-ffmpeg-overlay-filter/";
        asset_path = overlay_root + "watermark_asset.svg";
        generated_svg_path = overlay_root + "generated.overlay.svg";
        ASSERT_TRUE(File::create_path(asset_path, 0777));
        ASSERT_TRUE(File::saveFile(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1280\" height=\"720\">"
            "<rect x=\"0\" y=\"0\" width=\"100\" height=\"40\" fill=\"#ffffff\"/>"
            "</svg>",
            asset_path));

        mINI::Instance()[OverlayPrivacyConfig::kUseWatermarkAsset] = true;
        mINI::Instance()[OverlayPrivacyConfig::kOverlayRoot] = overlay_root;

        policy.watermark_template =
            "{\"overlayAssetId\":\"watermark_asset\",\"canvas\":{\"width\":1280,\"height\":720}}";
        policy.camera_name = "Lobby Camera";
        options.username = "alice";
        tuple = MediaTuple("__defaultVhost__", "camera-id", "main", "");
    }

    void TearDown() override {
        for (const auto &path : temporary_paths) {
            std::remove(path.c_str());
        }
        std::remove(generated_svg_path.c_str());
        std::remove(asset_path.c_str());
        mINI::Instance()[OverlayPrivacyConfig::kUseWatermarkAsset] = previous_use_watermark_asset;
        mINI::Instance()[OverlayPrivacyConfig::kOverlayRoot] = previous_overlay_root;
    }

    std::string buildFilter() {
        temporary_paths.clear();
        bool overlay_required = false;
        const std::string filter = FFmpegOverlayFilter::build(
            tuple, policy, options, generated_svg_path, temporary_paths, overlay_required);
        EXPECT_TRUE(overlay_required);
        return filter;
    }

    std::string previous_use_watermark_asset;
    std::string previous_overlay_root;
    std::string overlay_root;
    std::string asset_path;
    std::string generated_svg_path;
    std::vector<std::string> temporary_paths;
    Broadcast::ViewOverlayPolicy policy;
    ExtractOptions options = {};
    MediaTuple tuple;
};

TEST_F(FFmpegOverlayFilterTest, CombinesWatermarkAssetAndSourceStamp) {
    policy.watermark_enforce = true;
    options.enable_source_stamp = true;

    const std::string filter = buildFilter();

    ASSERT_FALSE(filter.empty());
    EXPECT_EQ(2U, countOccurrences(filter, "movie="));
    EXPECT_NE(std::string::npos, filter.find(asset_path));
    EXPECT_NE(std::string::npos, filter.find(generated_svg_path));
    EXPECT_EQ(1U, countOccurrences(filter, "[v]"));
    ASSERT_NE(temporary_paths.end(),
              std::find(temporary_paths.begin(), temporary_paths.end(), generated_svg_path));
    EXPECT_NE(std::string::npos,
              File::loadFile(generated_svg_path).find("Lobby Camera - Extracted by alice - VMS"));
}

TEST_F(FFmpegOverlayFilterTest, KeepsAssetOnlyBehaviorWithoutSourceStamp) {
    policy.watermark_enforce = true;

    const std::string filter = buildFilter();

    ASSERT_FALSE(filter.empty());
    EXPECT_EQ(1U, countOccurrences(filter, "movie="));
    EXPECT_NE(std::string::npos, filter.find(asset_path));
    EXPECT_EQ(std::string::npos, filter.find(generated_svg_path));
    EXPECT_TRUE(temporary_paths.empty());
}

TEST_F(FFmpegOverlayFilterTest, BuildsSourceStampWhenAssetModeEnabledWithoutEnforcedWatermark) {
    options.enable_source_stamp = true;

    const std::string filter = buildFilter();

    ASSERT_FALSE(filter.empty());
    EXPECT_EQ(1U, countOccurrences(filter, "movie="));
    EXPECT_EQ(std::string::npos, filter.find(asset_path));
    EXPECT_NE(std::string::npos, filter.find(generated_svg_path));
    EXPECT_NE(std::string::npos,
              File::loadFile(generated_svg_path).find("Lobby Camera - Extracted by alice - VMS"));
}

TEST_F(FFmpegOverlayFilterTest, ChainsPrivacyMaskAssetAndSourceStamp) {
    policy.watermark_enforce = true;
    policy.privacy_mask_enforce = true;
    policy.privacy_mask_regions =
        "[{\"id\":\"door\",\"points\":[{\"x\":10,\"y\":10},{\"x\":200,\"y\":10},"
        "{\"x\":200,\"y\":120},{\"x\":10,\"y\":120}],"
        "\"maskType\":\"solid\",\"color\":\"#000000\",\"opacity\":1}]";
    options.enable_source_stamp = true;

    const std::string filter = buildFilter();

    ASSERT_FALSE(filter.empty());
    EXPECT_EQ(3U, countOccurrences(filter, "movie="));
    EXPECT_NE(std::string::npos, filter.find("[poly_pm0]"));
    EXPECT_NE(std::string::npos, filter.find("[wm_stage0]"));
    EXPECT_EQ(1U, countOccurrences(filter, "[v]"));
}

TEST_F(FFmpegOverlayFilterTest, KeepsColorPlanesForBlurPrivacyMask) {
    policy.privacy_mask_enforce = true;
    policy.privacy_mask_regions =
        "[{\"id\":\"door\",\"points\":[{\"x\":10,\"y\":10},{\"x\":200,\"y\":10},"
        "{\"x\":200,\"y\":120},{\"x\":10,\"y\":120}],"
        "\"maskType\":\"blur\",\"blurRadius\":8}]";

    const std::string filter = buildFilter();

    ASSERT_FALSE(filter.empty());
    EXPECT_NE(std::string::npos, filter.find("boxblur="));
    EXPECT_NE(std::string::npos, filter.find("alphamerge[poly_pm0_processed_masked]"));
    EXPECT_NE(std::string::npos,
              filter.find("[poly_pm0_mask_base][poly_pm0_processed_masked]overlay="));
    EXPECT_EQ(std::string::npos, filter.find("maskedmerge"));
}

TEST_F(FFmpegOverlayFilterTest, KeepsColorPlanesForPixelatePrivacyMask) {
    policy.privacy_mask_enforce = true;
    policy.privacy_mask_regions =
        "[{\"id\":\"door\",\"points\":[{\"x\":10,\"y\":10},{\"x\":200,\"y\":10},"
        "{\"x\":200,\"y\":120},{\"x\":10,\"y\":120}],"
        "\"maskType\":\"pixelate\",\"pixelSize\":12}]";

    const std::string filter = buildFilter();

    ASSERT_FALSE(filter.empty());
    EXPECT_NE(std::string::npos, filter.find("flags=area"));
    EXPECT_NE(std::string::npos, filter.find("alphamerge[poly_pm0_processed_masked]"));
    EXPECT_NE(std::string::npos,
              filter.find("[poly_pm0_mask_base][poly_pm0_processed_masked]overlay="));
    EXPECT_EQ(std::string::npos, filter.find("maskedmerge"));
}

} // namespace
