#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include "platform/input.hpp"
#include "platform/input_script.hpp"

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
