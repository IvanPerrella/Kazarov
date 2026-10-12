#include <gtest/gtest.h>
#include <libpad.h>

#include "sce/sdk_params.hpp"
#include "sce/libpad_extensions.hpp"

TEST(PlatformPadLifecycle, ClosingAPortAndEndingTheLibraryReleaseOpenedPorts) {
    using Buffer = SdkParams<decltype(scePadPortOpen)>::At<2>;
    scePadInit(0);
    EXPECT_EQ(scePadPortOpen(0, 0, Buffer{}), 1);
    EXPECT_EQ(scePadPortOpen(1, 0, Buffer{}), 1);
    EXPECT_EQ(scePadPortClose(-1, 0), 0);
    EXPECT_EQ(scePadPortClose(0, 1), 0);
    EXPECT_EQ(scePadPortClose(0, 0), 1);
    EXPECT_EQ(scePadPortClose(0, 0), 0);
    EXPECT_EQ(scePadGetState(0, 0), scePadStateDiscon);
    EXPECT_EQ(scePadEnd(), 1);
    EXPECT_EQ(scePadPortClose(1, 0), 0);
    EXPECT_EQ(scePadEnd(), 1);
    EXPECT_EQ(scePadPortOpen(0, 0, Buffer{}), 1);
    EXPECT_EQ(scePadPortClose(0, 0), 1);
}
