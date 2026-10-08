#include <gtest/gtest.h>

#include "Control/VendorStage.h"
#include "ext-plugin/DeviceFactory.h"

using namespace managerkit;

namespace {

VendorStage::ProbeResult probeResult(DeviceBrand brand, bool rejected, bool answered) {
    VendorStage::ProbeResult result;
    result.brand = brand;
    result.rejected = rejected;
    result.answered = answered;
    return result;
}

} // namespace

TEST(VendorStageTest, RegistryDescribesTheBoschSpeakerPlugin) {
    VendorPluginInfo info;
    ASSERT_TRUE(DeviceFactory::findPlugin(DeviceType::Speaker, DeviceBrand::BOSCH, info));
    EXPECT_EQ("Bosch", info.name);
    EXPECT_TRUE(info.ownControlAccount);
    EXPECT_EQ("ONVIF", info.requiredProtocol);
    EXPECT_TRUE(info.sharedDsp);
    EXPECT_FALSE(DeviceFactory::findPlugin(DeviceType::Speaker, DeviceBrand::TOA, info));
    EXPECT_FALSE(DeviceFactory::findPlugin(DeviceType::Camera, DeviceBrand::BOSCH, info));
    EXPECT_TRUE(DeviceFactory::plugins(DeviceType::Camera).empty());
}

TEST(VendorStageTest, ConfiguredManufacturerSelectsThePlugin) {
    VendorStage stage;
    stage.configure(DeviceType::Speaker, "BOSCH Security Systems");
    EXPECT_FALSE(stage.detecting());
    EXPECT_EQ(DeviceBrand::BOSCH, stage.brand());
    ASSERT_TRUE(stage.hasPlugin());
    EXPECT_EQ("ONVIF", stage.plugin().requiredProtocol);

    // A brand without plugin only uses the control protocols.
    stage.configure(DeviceType::Speaker, "Hikvision");
    EXPECT_FALSE(stage.detecting());
    EXPECT_FALSE(stage.hasPlugin());
    EXPECT_EQ(DeviceBrand::UNKNOWN, stage.brand());
}

TEST(VendorStageTest, BlankManufacturerDetectsOnlyWhereThereArePlugins) {
    VendorStage stage;
    stage.configure(DeviceType::Speaker, "  ");
    EXPECT_TRUE(stage.detecting());
    EXPECT_FALSE(stage.hasPlugin());

    // No camera plugin yet: a camera without manufacturer stays a generic device.
    stage.configure(DeviceType::Camera, "");
    EXPECT_FALSE(stage.detecting());
    EXPECT_FALSE(stage.hasPlugin());
}

TEST(VendorStageTest, VendorApiLoginDetectsTheBrand) {
    VendorStage stage;
    stage.configure(DeviceType::Speaker, "");
    stage.startCycle();
    EXPECT_TRUE(stage.onVendorProbe(probeResult(DeviceBrand::BOSCH, false, true)));
    EXPECT_FALSE(stage.detecting());
    EXPECT_EQ(DeviceBrand::BOSCH, stage.brand());
    EXPECT_TRUE(stage.hasPlugin());
    EXPECT_EQ("Bosch", stage.name());
}

TEST(VendorStageTest, RejectedOrUnansweredLoginDetectsNothing) {
    VendorStage stage;
    stage.configure(DeviceType::Speaker, "");
    stage.startCycle();
    EXPECT_FALSE(stage.onVendorProbe(probeResult(DeviceBrand::UNKNOWN, true, true)));
    EXPECT_TRUE(stage.detecting());
    EXPECT_FALSE(stage.hasPlugin());
    EXPECT_TRUE(stage.name().empty());
}

TEST(VendorStageTest, ProtocolDeviceInfoNamesTheManufacturer) {
    VendorStage stage;
    stage.configure(DeviceType::Speaker, "");
    EXPECT_FALSE(stage.identify(" ")) << "no name: nothing to write back";
    EXPECT_FALSE(stage.detecting()) << "but no point in asking again";

    stage.configure(DeviceType::Speaker, "");
    EXPECT_TRUE(stage.identify("Hikvision"));
    EXPECT_FALSE(stage.hasPlugin());
    EXPECT_EQ("Hikvision", stage.name());

    stage.configure(DeviceType::Speaker, "");
    EXPECT_TRUE(stage.identify("BOSCH Security Systems"));
    EXPECT_TRUE(stage.hasPlugin());
    EXPECT_EQ(DeviceBrand::BOSCH, stage.brand());
    EXPECT_EQ("BOSCH Security Systems", stage.name());
}

TEST(VendorStageTest, CycleBacksOffOnlyWhileNothingAnswers) {
    VendorStage stage;
    stage.configure(DeviceType::Speaker, "");
    const int expected[] = {120, 240, 480, 600, 600};
    for (int want : expected) {
        stage.startCycle();
        stage.onVendorProbe(probeResult(DeviceBrand::UNKNOWN, false, false));
        stage.onCycleFailed(true);
        EXPECT_EQ(want, stage.cycleIntervalSec(600));
    }
    EXPECT_EQ(60, stage.cycleIntervalSec(0)) << "0 turns the back-off off";

    // The vendor API answered (HTTP 404), the protocols did not: the device is there.
    stage.startCycle();
    stage.onVendorProbe(probeResult(DeviceBrand::UNKNOWN, false, true));
    stage.onCycleFailed(true);
    EXPECT_EQ(60, stage.cycleIntervalSec(600));

    stage.startCycle();
    stage.onVendorProbe(probeResult(DeviceBrand::UNKNOWN, false, false));
    stage.onCycleFailed(true);
    EXPECT_EQ(120, stage.cycleIntervalSec(600));
    stage.resetBackoff();
    EXPECT_EQ(60, stage.cycleIntervalSec(600));

    // A protocol rejected the password: answered, no back-off.
    stage.startCycle();
    stage.onVendorProbe(probeResult(DeviceBrand::UNKNOWN, false, false));
    stage.onCycleFailed(false);
    EXPECT_EQ(60, stage.cycleIntervalSec(600));
}
