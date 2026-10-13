#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include "platform/input.hpp"
#include "platform/input_script.hpp"
#include "platform/mouse.hpp"

TEST(KeyboardLook, MovementDoesNotHideSimultaneousControllerLook) {
    InputResetBindings();
    for (int axis : {0, 128, 220, 255}) {
        InputPadState pad{.connected = true, .left_x = static_cast<std::uint8_t>(axis)};
        auto          mixed = InputApplyKeyboardMouse(pad, {
                                                               .keys = {SDL_SCANCODE_W, SDL_SCANCODE_D}
        });
        InputSetOverride(0, &mixed);
        EXPECT_EQ(InputGetLookPad(0).left_x, axis);
        EXPECT_EQ(InputGetLookPad(0).left_y, 128);
        EXPECT_NE(InputGetPad(0).left_y, 128);
    }
    InputSetOverride(0, nullptr);
}

TEST(KeyboardLook, ScriptRetainsControllerAndMouseAxisProvenance) {
    InputResetBindings();
    const std::string_view binding[] = {"MouseY"};
    ASSERT_TRUE(InputBindKeys("ly", binding));
    InputScript script;
    std::string error;
    ASSERT_TRUE(InputScriptParse("0 key:w mouse:0,5 220 128 128 128\n", script, error)) << error;
    auto state = InputScriptStateAt(script, 0, 0);
    InputSetOverride(0, &state);
    EXPECT_EQ(InputGetLookPad(0).left_x, 220);
    EXPECT_GT(InputGetLookPad(0).left_y, 128);
    InputSetOverride(0, nullptr);
    InputResetBindings();
}

TEST(KeyboardLook, DisplayTotalRetainsMultipleLogicSamplesAndDoesNotDrain) {
    InputResetBindings();
    InputPadState pad{.connected = true};
    InputSetOverride(0, &pad);
    InputSetScriptedDevices({.mouse_dx = 5, .mouse_dy = 3});
    const auto before = InputPeekMouseLookTotal();
    InputLatchPad(0);
    const auto one = InputGetMouseLook();
    InputLatchPad(0);
    const auto total = InputPeekMouseLookTotal();
    EXPECT_NEAR(total.yaw - before.yaw, 2 * one.yaw, 1e-8);
    EXPECT_NEAR(total.pitch - before.pitch, 2 * one.pitch, 1e-8);
    EXPECT_EQ(InputPeekMouseLookTotal().yaw, total.yaw);
    EXPECT_EQ(InputGetMouseLook().yaw, one.yaw);
    InputSetMenuMouse(true);
    EXPECT_NE(InputPeekMouseLookTotal().epoch, total.epoch);
    InputSetMenuMouse(false);
    InputSetOverride(0, nullptr);
    InputSetScriptedDevices({});
}

TEST(KeyboardLook, DisplayTotalPeeksLiveMotionBeforeLatchWithoutDoubleApplying) {
    InputResetBindings();
    MouseConfigure(false, {});
    const auto before = InputPeekMouseLookTotal();
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.xrel = 12;
    motion.motion.yrel = 7;
    InputHandleEvent(motion);
    const auto display = InputPeekMouseLookTotal();
    EXPECT_NE(display.yaw, before.yaw);
    EXPECT_NE(display.pitch, before.pitch);
    EXPECT_EQ(InputPeekMouseLookTotal().yaw, display.yaw);
    InputLatchPad(0);
    EXPECT_NEAR(InputPeekMouseLookTotal().yaw, display.yaw, 1e-8);
    EXPECT_NEAR(InputPeekMouseLookTotal().pitch, display.pitch, 1e-8);
    MouseConfigure(true, {});
}
