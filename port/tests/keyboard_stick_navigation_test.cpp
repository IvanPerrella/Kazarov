#include <gtest/gtest.h>
#include <SDL3/SDL.h>

#include "platform/input.hpp"

TEST(KeyboardStickNavigation, ControllerDeflectionWinsPerAxis) {
    InputResetBindings();
    InputKeyboardMouse held;
    held.keys = {SDL_SCANCODE_D, SDL_SCANCODE_W};
    auto mixed = InputApplyKeyboardMouse({}, held);
    EXPECT_GT(mixed.left_x, 128);
    EXPECT_LT(mixed.left_y, 128);

    InputPadState controller;
    controller.left_x = 0;
    mixed = InputApplyKeyboardMouse(controller, held);
    EXPECT_EQ(mixed.left_x, 0);

    controller.left_y = 255;
    mixed = InputApplyKeyboardMouse(controller, held);
    EXPECT_EQ(mixed.left_y, 255);
    mixed = InputApplyKeyboardMouse({}, {});
    EXPECT_EQ(mixed.left_x, 128);
    EXPECT_EQ(mixed.left_y, 128);
}

TEST(KeyboardStickNavigation, ReboundAndOpposingKeysUseEffectiveAxes) {
    InputResetBindings();
    const std::string_view binding[] = {"H"};
    ASSERT_TRUE(InputBindKeys("lx-", binding));
    InputKeyboardMouse held;
    held.keys = {SDL_SCANCODE_H};
    auto state = InputApplyKeyboardMouse({}, held);
    EXPECT_EQ(state.left_x, 0);
    held.keys.push_back(SDL_SCANCODE_D);
    state = InputApplyKeyboardMouse({}, held);
    EXPECT_EQ(state.left_x, 128);
    InputResetBindings();
}

TEST(KeyboardStickNavigation, NavigationPolicyPreservesExplicitDpadAndStickAxes) {
    InputResetBindings();
    InputPadState base;
    base.connected = true;
    InputKeyboardMouse held;
    held.keys = {SDL_SCANCODE_D, SDL_SCANCODE_UP};
    const auto state = InputApplyKeyboardMouse(base, held);
    InputSetOverride(0, &state);
    InputSetKeyboardStickNavigation(false);
    // Host menu state must not re-enable fallback: the game owns conversion.
    for (bool menu : {false, true, false}) {
        InputSetMenuNavigation(menu);
        EXPECT_EQ(InputGetPad(0).buttons, kInputUp);
        EXPECT_EQ(InputGetPad(0).left_x, 255);
    }
    // The policy is opt-in: other games retain their fallback navigation.
    InputSetKeyboardStickNavigation(true);
    if (!InputLeftStickLive()) EXPECT_EQ(InputGetPad(0).buttons, kInputUp | kInputRight);
    InputSetOverride(0, nullptr);
}
