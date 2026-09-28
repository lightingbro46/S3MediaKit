#include <gtest/gtest.h>

#include "Common/config.h"

using namespace mediakit;

TEST(ViewOverlayPolicyTest, ReportsIndependentEffectiveRequirements) {
    Broadcast::ViewOverlayPolicy policy;
    policy.status = Broadcast::ViewOverlayPolicy::Status::Resolved;

    EXPECT_FALSE(policy.requiresWatermark());
    EXPECT_FALSE(policy.requiresPrivacyMask());
    EXPECT_FALSE(policy.requiresOverlay());

    policy.watermark_enforce = true;
    EXPECT_TRUE(policy.requiresWatermark());
    EXPECT_FALSE(policy.requiresPrivacyMask());
    EXPECT_TRUE(policy.requiresOverlay());

    policy.watermark_excluded = true;
    policy.privacy_mask_enforce = true;
    EXPECT_FALSE(policy.requiresWatermark());
    EXPECT_TRUE(policy.requiresPrivacyMask());
    EXPECT_TRUE(policy.requiresOverlay());

    policy.privacy_mask_excluded = true;
    EXPECT_FALSE(policy.requiresOverlay());
}

TEST(ViewOverlayPolicyTest, ReportsUnavailableManagedPolicy) {
    Broadcast::ViewOverlayPolicy policy;
    EXPECT_FALSE(policy.isUnavailable());

    policy.status = Broadcast::ViewOverlayPolicy::Status::Unavailable;
    EXPECT_TRUE(policy.isUnavailable());
}
