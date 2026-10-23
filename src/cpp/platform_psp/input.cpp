// input.hpp on the PSP: its own buttons and analog nub as pad 1, with no keyboard, mouse, rebinding,
// rumble or second pad.
//
// The PSP lacks L2, R2, L3, R3 and the right stick. Select is a shift: Select with L is L2 (the
// camera behind the character), Select with R is R2 (first-person look), and Select pressed and let
// go alone is Select (switch character). The camera also turns with L1 and R1, as in retail.

#include "platform/input.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include <pspctrl.h>

#include "platform/settings.hpp"

namespace {

// The ticks a lone Select is held for after it is let go, so the game sees a press.
constexpr int kSelectTapPolls = 3;

InputPadState                g_state[kInputPadCount];
InputPadState                g_view[kInputPadCount];
std::optional<InputPadState> g_override[kInputPadCount];
InputRumble                  g_rumble[kInputPadCount];
InputMouseLook               g_mouse_look;
InputMouseSettings           g_mouse_settings;
float                        g_stick_sensitivity = 1.33f;
bool                         g_invert_x = false;
bool                         g_invert_y = false;
bool                         g_look_left = false;
bool                         g_keyboard_stick_navigation = false;
bool                         g_developer_menu = false;
bool                         g_select_shifted = false;
bool                         g_select_held = false;
int                          g_select_tap = 0;
std::uint64_t                g_latch_serial = 0;
std::uint64_t                g_stick_read_serial = 0;
bool                         g_stick_live = false;

// The nub's circle onto the DualShock 2's square, as the PC port does for a modern stick
// (input.stick_sensitivity), so the game's run threshold is within reach.
void NubToBytes(unsigned char raw_x, unsigned char raw_y, std::uint8_t &x, std::uint8_t &y) {
    float fx = std::clamp((static_cast<float>(raw_x) - 128.0f) / 127.0f, -1.0f, 1.0f);
    float fy = std::clamp((static_cast<float>(raw_y) - 128.0f) / 127.0f, -1.0f, 1.0f);
    float radius = std::min(1.0f, std::hypot(fx, fy) * g_stick_sensitivity);
    float edge = std::max(std::fabs(fx), std::fabs(fy));
    if (edge > 0.0f) {
        fx *= radius / edge;
        fy *= radius / edge;
    }
    if (g_invert_x) {
        fx = -fx;
    }
    if (g_invert_y) {
        fy = -fy;
    }
    x = static_cast<std::uint8_t>(std::clamp(std::lround(128.0f + fx * 127.0f), 0L, 255L));
    y = static_cast<std::uint8_t>(std::clamp(std::lround(128.0f + fy * 127.0f), 0L, 255L));
}

bool Deflected(std::uint8_t byte) {
    int offset = static_cast<int>(byte) - kInputAxisCentre;
    return offset > 49 || offset < -50;
}

} // namespace

void InputInit() {
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
}

void InputApplyConfig(const HostSettings &config) {
    g_stick_sensitivity = config.stick_sensitivity;
    g_invert_x = config.stick_invert_x;
    g_invert_y = config.stick_invert_y;
}

void InputShutdown() {}

void InputPoll() {
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(&pad, 1);
    const unsigned held = pad.Buttons;
    InputPadState state;
    state.connected = true;
    auto press = [&](unsigned psp, std::uint16_t ps2) {
        if (held & psp) {
            state.buttons |= ps2;
        }
    };
    press(PSP_CTRL_UP, kInputUp);
    press(PSP_CTRL_DOWN, kInputDown);
    press(PSP_CTRL_LEFT, kInputLeft);
    press(PSP_CTRL_RIGHT, kInputRight);
    press(PSP_CTRL_CROSS, kInputCross);
    press(PSP_CTRL_CIRCLE, kInputCircle);
    press(PSP_CTRL_SQUARE, kInputSquare);
    press(PSP_CTRL_TRIANGLE, kInputTriangle);
    press(PSP_CTRL_START, kInputStart);
    const bool select = (held & PSP_CTRL_SELECT) != 0;
    const bool shoulder = (held & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) != 0;
    if (select) {
        if (shoulder) {
            g_select_shifted = true;
        }
        press(PSP_CTRL_LTRIGGER, kInputL2);
        press(PSP_CTRL_RTRIGGER, kInputR2);
    } else {
        press(PSP_CTRL_LTRIGGER, kInputL1);
        press(PSP_CTRL_RTRIGGER, kInputR1);
        if (g_select_held && !g_select_shifted) {
            g_select_tap = kSelectTapPolls;
        }
        g_select_shifted = false;
    }
    g_select_held = select;
    if (g_select_tap > 0) {
        state.buttons |= kInputSelect;
        g_select_tap--;
    }
    NubToBytes(pad.Lx, pad.Ly, state.left_x, state.left_y);
    if (state.left_y < kInputAxisCentre - 50) {
        state.stick_dpad |= kInputUp;
    } else if (state.left_y > kInputAxisCentre + 49) {
        state.stick_dpad |= kInputDown;
    }
    if (state.left_x < kInputAxisCentre - 50) {
        state.stick_dpad |= kInputLeft;
    } else if (state.left_x > kInputAxisCentre + 49) {
        state.stick_dpad |= kInputRight;
    }
    g_state[0] = state;
    g_state[1] = InputPadState{};
}

void InputHandleEvent(const SDL_Event &) {}

void InputLatchPad(int pad) {
    if (pad != 0) {
        return;
    }
    g_stick_live = g_stick_read_serial == g_latch_serial;
    ++g_latch_serial;
}

const InputMouseLook &InputGetMouseLook() { return g_mouse_look; }
InputMouseLookTotal   InputPeekMouseLookTotal() { return {}; }
void                  InputNoteLeftStickRead() { g_stick_read_serial = g_latch_serial; }
bool                  InputLeftStickLive() { return g_stick_live; }

const InputPadState &InputGetPad(int pad) {
    static const InputPadState kAbsent;
    if (pad < 0 || pad >= kInputPadCount) {
        return kAbsent;
    }
    g_view[pad] = g_override[pad] ? *g_override[pad] : g_state[pad];
    if (g_keyboard_stick_navigation && !g_stick_live) {
        g_view[pad].buttons |= g_view[pad].stick_dpad;
    }
    return g_view[pad];
}

InputPadState InputGetLookPad(int pad) {
    InputPadState state = InputGetPad(pad);
    if (pad == 0 && g_look_left && !Deflected(state.left_x) && !Deflected(state.left_y)) {
        state.left_x = state.right_x;
        state.left_y = state.right_y;
    }
    return state;
}

void InputSetLookOnLeftStick(bool left) { g_look_left = left; }

void InputSetRumble(int pad, InputRumble rumble) {
    if (pad >= 0 && pad < kInputPadCount) {
        g_rumble[pad] = rumble;
    }
}

void InputSetLightbar(int, std::uint8_t, std::uint8_t, std::uint8_t) {}

InputRumble InputGetRumble(int pad) { return pad >= 0 && pad < kInputPadCount ? g_rumble[pad] : InputRumble{}; }

void InputSetOverride(int pad, const InputPadState *state) {
    if (pad < 0 || pad >= kInputPadCount) {
        return;
    }
    if (state != nullptr) {
        g_override[pad] = *state;
    } else {
        g_override[pad].reset();
    }
}

InputPadState InputApplyKeyboardMouse(InputPadState base, const InputKeyboardMouse &) { return base; }

std::uint8_t InputStickByte(float deflection) {
    if (!std::isfinite(deflection)) {
        return kInputAxisCentre;
    }
    return static_cast<std::uint8_t>(std::clamp(std::lround(128.0f + std::clamp(deflection, -1.0f, 1.0f) * 127.0f), 0L, 255L));
}

bool InputBindKeys(std::string_view, std::span<const std::string_view>) { return true; }
void InputResetBindings() {}
void InputSetMouseSettings(const InputMouseSettings &settings) { g_mouse_settings = settings; }
const InputMouseSettings &InputGetMouseSettings() { return g_mouse_settings; }

void           InputSetMenuMouse(bool) {}
void           InputSetMenuNavigation(bool) {}
void           InputSetKeyboardStickNavigation(bool on) { g_keyboard_stick_navigation = on; }
void           InputSetDeveloperMenu(bool on) { g_developer_menu = on; }
bool           InputDeveloperMenu() { return g_developer_menu; }
InputMenuMouse InputTakeMenuMouse() { return {}; }

void InputPeekMenuMouseMotion(float &dx, float &dy) {
    dx = 0.0f;
    dy = 0.0f;
}

InputGlyphFamily InputGlyphFamilyForGamepad(int, unsigned, unsigned) { return InputGlyphFamily::Ps3; }
InputGlyphFamily InputActiveGlyphFamily() { return InputGlyphFamily::Ps3; }
std::string      InputPrimaryBindingName(std::string_view) { return {}; }
int              InputScancodeFromName(std::string_view) { return -1; }
bool             InputHostHeld(InputHostAction) { return false; }
bool             InputHostPressed(InputHostAction) { return false; }
void             InputSetScriptedDevices(const InputKeyboardMouse &) {}
InputKeyboardMovement InputGetKeyboardMovement() { return {}; }
void                  InputSetMovementLocked(bool) {}
// Nothing to rebind: the Options screen lists no bindings.
std::span<const InputActionInfo> InputActions() { return {}; }
bool                             InputActionTakesGamepad(std::string_view) { return false; }
std::string                      InputBindingLabel(std::string_view) { return {}; }
void                             InputBeginBindCapture() {}
std::string                      InputTakeBindKey(bool) { return {}; }
bool                             InputBindSourceHeld(std::string_view) { return false; }
