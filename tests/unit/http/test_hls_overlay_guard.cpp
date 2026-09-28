#include <gtest/gtest.h>

#include "Common/MediaSource.h"
#include "Http/HlsViewerSession.h"
#include "Http/HttpSession.h"

using namespace mediakit;

namespace {

class OverlayGuardMediaSource : public MediaSource {
public:
    explicit OverlayGuardMediaSource(const std::string &stream)
        : MediaSource(HLS_SCHEMA, MediaTuple(DEFAULT_VHOST, "camera", stream, "")) {}

    int readerCount() override { return 0; }
};

class OriginMediaEvent : public MediaSourceEvent {
public:
    explicit OriginMediaEvent(MediaOriginType origin) : _origin(origin) {}

    MediaOriginType getOriginType(MediaSource &) const override { return _origin; }

private:
    MediaOriginType _origin;
};

class TestableHttpSession : public HttpSession {
public:
    using HttpSession::isTrustedViewOverlayDerivedSource;
    using HttpSession::makeViewOverlayPolicyTag;
    using HttpSession::resolveHlsViewOverlayPolicy;
};

Broadcast::ViewOverlayPolicy makePolicy(bool watermark, bool privacy_mask) {
    Broadcast::ViewOverlayPolicy policy;
    policy.status = Broadcast::ViewOverlayPolicy::Status::Resolved;
    policy.watermark_enforce = watermark;
    policy.watermark_template = "{username}";
    policy.privacy_mask_enforce = privacy_mask;
    policy.privacy_mask_regions = "[[0,0,10,10]]";
    policy.username = "alice";
    policy.camera_name = "gate";
    return policy;
}

std::string derivedStream(const Broadcast::ViewOverlayPolicy &policy) {
    return std::string("main") + TRANSCODE_SUFFIX + "0123456789abcdef." + TestableHttpSession::makeViewOverlayPolicyTag(policy);
}

} // namespace

TEST(HlsOverlayGuardTest, TrustsOnlyRegisteredTranscodeOrigin) {
    const auto policy = makePolicy(true, true);
    const auto tag = TestableHttpSession::makeViewOverlayPolicyTag(policy);
    EXPECT_FALSE(TestableHttpSession::isTrustedViewOverlayDerivedSource(nullptr, tag));

    auto source = std::make_shared<OverlayGuardMediaSource>(derivedStream(policy));
    EXPECT_FALSE(TestableHttpSession::isTrustedViewOverlayDerivedSource(source, tag));

    auto pull_listener = std::make_shared<OriginMediaEvent>(MediaOriginType::pull);
    source->setListener(pull_listener);
    EXPECT_FALSE(TestableHttpSession::isTrustedViewOverlayDerivedSource(source, tag));

    auto transcode_listener = std::make_shared<OriginMediaEvent>(MediaOriginType::transcode);
    source->setListener(transcode_listener);
    EXPECT_TRUE(TestableHttpSession::isTrustedViewOverlayDerivedSource(source, tag));
}

TEST(HlsOverlayGuardTest, RejectsDerivedStreamOfAnotherPolicy) {
    auto transcode_listener = std::make_shared<OriginMediaEvent>(MediaOriginType::transcode);
    const auto viewer_tag = TestableHttpSession::makeViewOverlayPolicyTag(makePolicy(true, true));

    // Watermark-only derived stream built for a viewer excluded from the mask.
    auto watermark_only = std::make_shared<OverlayGuardMediaSource>(derivedStream(makePolicy(true, false)));
    watermark_only->setListener(transcode_listener);
    EXPECT_FALSE(TestableHttpSession::isTrustedViewOverlayDerivedSource(watermark_only, viewer_tag));

    // Same overlay rendered for another user.
    auto other_user_policy = makePolicy(true, true);
    other_user_policy.username = "bob";
    auto other_user = std::make_shared<OverlayGuardMediaSource>(derivedStream(other_user_policy));
    other_user->setListener(transcode_listener);
    EXPECT_FALSE(TestableHttpSession::isTrustedViewOverlayDerivedSource(other_user, viewer_tag));

    // Legacy suffix without a policy tag.
    auto legacy = std::make_shared<OverlayGuardMediaSource>(std::string("main") + TRANSCODE_SUFFIX + "0123456789abcdef");
    legacy->setListener(transcode_listener);
    EXPECT_FALSE(TestableHttpSession::isTrustedViewOverlayDerivedSource(legacy, viewer_tag));
}

TEST(HlsOverlayGuardTest, PolicyTagIgnoresFieldsThatAreNotEnforced) {
    auto excluded = makePolicy(true, true);
    excluded.watermark_excluded = true;
    excluded.privacy_mask_excluded = true;
    Broadcast::ViewOverlayPolicy none;
    EXPECT_EQ(TestableHttpSession::makeViewOverlayPolicyTag(excluded),
              TestableHttpSession::makeViewOverlayPolicyTag(none));
    EXPECT_NE(TestableHttpSession::makeViewOverlayPolicyTag(makePolicy(true, false)),
              TestableHttpSession::makeViewOverlayPolicyTag(makePolicy(false, true)));
}

namespace {

// Policy the manager resolves: the token lifts the viewer's exclusions, a
// request without token (HLS segment) gets the full camera policy.
struct PolicyResolver {
    bool mask_excluded = false;
    bool watermark_excluded = false;
    int calls = 0;

    Broadcast::ViewOverlayPolicy operator()(const std::string &token) {
        ++calls;
        auto policy = makePolicy(true, true);
        policy.username.clear();
        if (!token.empty()) {
            policy.privacy_mask_excluded = mask_excluded;
            policy.watermark_excluded = watermark_excluded;
        }
        return policy;
    }
};

Broadcast::ViewOverlayPolicy resolveHls(PolicyResolver &resolver, const std::string &key,
                                        const std::string &token, uint64_t now_ms) {
    return TestableHttpSession::resolveHlsViewOverlayPolicy(key, token, 60000, now_ms, [&]() {
        return resolver(token);
    });
}

} // namespace

TEST(HlsOverlayGuardTest, SegmentWithoutTokenUsesPlaylistPolicyOfTheSession) {
    PolicyResolver resolver;
    resolver.mask_excluded = true;
    const std::string key = "vhost|live|camera|mask-excluded-session";

    // Playlist carries the token: watermark-only derived stream.
    const auto playlist_policy = resolveHls(resolver, key, "token", 1000);
    EXPECT_TRUE(playlist_policy.requiresWatermark());
    EXPECT_FALSE(playlist_policy.requiresPrivacyMask());
    auto derived = std::make_shared<OverlayGuardMediaSource>(derivedStream(playlist_policy));
    auto transcode_listener = std::make_shared<OriginMediaEvent>(MediaOriginType::transcode);
    derived->setListener(transcode_listener);

    // Segment carries only the session id: same policy, derived stream trusted,
    // and the manager is not asked again.
    const auto segment_policy = resolveHls(resolver, key, "", 2000);
    EXPECT_EQ(1, resolver.calls);
    EXPECT_TRUE(TestableHttpSession::isTrustedViewOverlayDerivedSource(
        derived, TestableHttpSession::makeViewOverlayPolicyTag(segment_policy)));
}

TEST(HlsOverlayGuardTest, FullyExcludedViewerKeepsRawSegments) {
    PolicyResolver resolver;
    resolver.mask_excluded = true;
    resolver.watermark_excluded = true;
    const std::string key = "vhost|live|camera|fully-excluded-session";

    EXPECT_FALSE(resolveHls(resolver, key, "token", 1000).requiresOverlay());
    EXPECT_FALSE(resolveHls(resolver, key, "", 1500).requiresOverlay());
    EXPECT_EQ(1, resolver.calls);
}

TEST(HlsOverlayGuardTest, UnknownOrExpiredSessionFallsBackToFullPolicy) {
    PolicyResolver resolver;
    resolver.mask_excluded = true;
    const std::string key = "vhost|live|camera|expiring-session";

    // No session: resolved without token, full overlay.
    EXPECT_TRUE(resolveHls(resolver, "", "", 1000).requiresPrivacyMask());
    // Session never stored (e.g. forged session id).
    EXPECT_TRUE(resolveHls(resolver, "vhost|live|camera|forged", "", 1000).requiresPrivacyMask());

    resolveHls(resolver, key, "token", 1000);
    EXPECT_FALSE(resolveHls(resolver, key, "", 60000).requiresPrivacyMask());   // read extends to 120000
    EXPECT_FALSE(resolveHls(resolver, key, "", 119999).requiresPrivacyMask());
    EXPECT_TRUE(resolveHls(resolver, key, "", 300000).requiresPrivacyMask());   // expired

    // Another camera never reuses the session's policy.
    EXPECT_TRUE(resolveHls(resolver, "vhost|live|other|expiring-session", "", 1000).requiresPrivacyMask());
}

TEST(HlsOverlayGuardTest, TokenRequestRefreshesStoredPolicy) {
    PolicyResolver resolver;
    const std::string key = "vhost|record|camera|refresh-session";

    EXPECT_TRUE(resolveHls(resolver, key, "token", 1000).requiresPrivacyMask());
    resolver.mask_excluded = true;  // role changed, next playlist reload sees it
    EXPECT_FALSE(resolveHls(resolver, key, "token", 2000).requiresPrivacyMask());
    EXPECT_FALSE(resolveHls(resolver, key, "", 3000).requiresPrivacyMask());
    EXPECT_EQ(2, resolver.calls);
}

TEST(HlsViewerPolicyCacheTest, PurgesExpiredEntriesOnWrite) {
    HlsViewerPolicyCache cache;
    Broadcast::ViewOverlayPolicy policy;
    cache.put("a", policy, 1000, 0);
    cache.put("b", policy, 1000, 0);
    EXPECT_EQ(2u, cache.size());
    cache.put("c", policy, 1000, 20000);
    EXPECT_EQ(1u, cache.size());
    EXPECT_FALSE(cache.get("a", 20000, policy));
    EXPECT_TRUE(cache.get("c", 20500, policy));
}
