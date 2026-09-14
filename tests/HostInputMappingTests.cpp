#include "SDL_keycode.h"
#include "SDL_keyboard.h"
#include "common/emulatorConfig.h"
#include "graphics/presentation/window/hostInput.h"
#include "libs/controller.h"
#include "libs/padData.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "HostInputMappingTests: failed: %s\n", message);
    std::abort();
  }
}

}  // namespace

int main() {
  Config::Initialize();
  Libs::Controller::Initialize();
  Libs::Graphics::HostInputInit();

  Libs::Graphics::HostInputScancode(SDL_SCANCODE_A, SDLK_a, true);
  Libs::Controller::PadData state {};
  Libs::Controller::PadReadState(1, &state);
  Check(state.left_stick_x == 0,
        "scancode-based left-stick negative input was not translated to an analog stick value");

  Libs::Graphics::HostInputScancode(SDL_SCANCODE_A, SDLK_a, false);
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_D, SDLK_d, true);
  Libs::Controller::PadReadState(1, &state);
  Check(state.left_stick_x == 255,
        "scancode-based left-stick positive input was not translated to an analog stick value");

  Libs::Graphics::HostInputScancode(SDL_SCANCODE_D, SDLK_d, false);
  Libs::Controller::PadReadState(1, &state);
  Check(state.left_stick_x == 128,
        "releasing the positive direction should center the stick when no direction remains active");

  Libs::Graphics::HostInputScancode(SDL_SCANCODE_CAPSLOCK, SDLK_CAPSLOCK, true);
  Check(Libs::Graphics::HostInputCapsLockEnabled(),
        "caps lock should enable the analog lock state");

  Libs::Graphics::HostInputScancode(SDL_SCANCODE_CAPSLOCK, SDLK_CAPSLOCK, true);
  Check(!Libs::Graphics::HostInputCapsLockEnabled(),
        "a second caps lock press should toggle the analog lock state back off");

  Libs::Graphics::HostInputScancode(SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE, true);
  Libs::Controller::PadReadState(1, &state);
  Check((state.buttons & Libs::Controller::PAD_BUTTON_TOUCH_PAD) != 0,
        "backspace should trigger the DualSense touchpad button in the default profile");

  Libs::Graphics::HostInputScancode(SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE, false);
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_DELETE, SDLK_DELETE, true);
  Libs::Controller::PadReadState(1, &state);
  Check((state.buttons & Libs::Controller::PAD_BUTTON_PS) != 0,
        "delete should trigger the PS button in the default profile");

  Libs::Graphics::HostInputScancode(SDL_SCANCODE_I, SDLK_i, true);
  Libs::Controller::PadReadState(1, &state);
  Check((state.buttons & Libs::Controller::PAD_BUTTON_TRIANGLE) != 0,
        "the default profile should map I to Triangle");

  Libs::Graphics::HostInputScancode(SDL_SCANCODE_J, SDLK_j, true);
  Libs::Controller::PadReadState(1, &state);
  Check((state.buttons & Libs::Controller::PAD_BUTTON_SQUARE) != 0,
        "the default profile should map J to Square");

  Libs::Graphics::HostInputScancode(SDL_SCANCODE_K, SDLK_k, true);
  Libs::Controller::PadReadState(1, &state);
  Check((state.buttons & Libs::Controller::PAD_BUTTON_CROSS) != 0,
        "the default profile should map K to Cross");

  Libs::Graphics::HostInputScancode(SDL_SCANCODE_L, SDLK_l, true);
  Libs::Controller::PadReadState(1, &state);
  Check((state.buttons & Libs::Controller::PAD_BUTTON_CIRCLE) != 0,
        "the default profile should map L to Circle");

  Libs::Graphics::HostInputScancode(SDL_SCANCODE_2, SDLK_2, true);
  Libs::Controller::PadReadState(1, &state);
  Check((state.buttons & Libs::Controller::PAD_BUTTON_LEFT) != 0,
        "the always-active fallback should map 2 to D-pad Left");

  Libs::Graphics::HostInputScancode(SDL_SCANCODE_C, SDLK_c, true);
  Libs::Controller::PadReadState(1, &state);
  Check((state.buttons & Libs::Controller::PAD_BUTTON_TRIANGLE) != 0,
        "the always-active fallback should map C to Triangle");

  Libs::Controller::SetMotion(Libs::Controller::HOST_INPUT_CONTROLLER_ID, 0.0f, 0.0f, 1.0f);
  Libs::Controller::SetMicrophoneLevel(Libs::Controller::HOST_INPUT_CONTROLLER_ID, 75);
  Libs::Controller::PadReadState(1, &state);
  Check(state.angular_velocity_z == 1.0f,
        "host gyro input should be exposed through PadData angular velocity");
  Check(state.extension_unit_data_data_length == 1 && state.extension_unit_data_data[0] == 75,
        "host microphone level should be exposed through the controller extension payload");

  // 1. Check adjustment_active is false during regular gameplay inputs
  auto diag = Libs::Graphics::HostInputGetDiagnostics();
  Check(!diag.adjustment_active,
        "regular inputs should not mark analog adjustment as active");
  Check(!diag.analog_lock,
        "analog lock should be off initially");

  // 2. Pressing Tab activates analog adjustment mode
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_TAB, SDLK_TAB, true);
  diag = Libs::Graphics::HostInputGetDiagnostics();
  Check(diag.adjustment_active,
        "holding Tab should mark analog adjustment as active");

  // 3. Tab + Q targets L2 adjustment force and does NOT trigger L1 bumper
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_Q, SDLK_q, true);
  Libs::Controller::PadReadState(1, &state);
  Check((state.buttons & Libs::Controller::PAD_BUTTON_L1) == 0,
        "Tab + Q must not generate L1 bumper press");

  // Release Q and Tab
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_Q, SDLK_q, false);
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_TAB, SDLK_TAB, false);
  diag = Libs::Graphics::HostInputGetDiagnostics();
  Check(!diag.adjustment_active,
        "releasing Tab should exit adjustment active state");

  // Normal Q triggers L1 bumper
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_Q, SDLK_q, true);
  Libs::Controller::PadReadState(1, &state);
  Check((state.buttons & Libs::Controller::PAD_BUTTON_L1) != 0,
        "pressing Q without Tab must trigger L1 bumper");
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_Q, SDLK_q, false);

  // 4. Test Caps Lock / Analog Lock: freeze, modify, decrement without zeroing, and persist on release
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_CAPSLOCK, SDLK_CAPSLOCK, true);
  Check(Libs::Graphics::HostInputCapsLockEnabled(),
        "caps lock should enable analog lock");
  diag = Libs::Graphics::HostInputGetDiagnostics();
  Check(diag.analog_lock,
        "analog_lock in diagnostics must be true when caps lock is active");

  // Press Tab + R to adjust L2
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_TAB, SDLK_TAB, true);
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_R, SDLK_r, true);

  // Set step to 75% via PLUS
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_EQUALS, SDLK_PLUS, true);
  Libs::Controller::PadReadState(1, &state);
  Check(state.analog_buttons_l2 == 192,
        "Tab + PLUS step should set L2 to 192");

  // Release R and Tab: value must stay FROZEN at 192 because Analog Lock is active!
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_EQUALS, SDLK_PLUS, false);
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_R, SDLK_r, false);
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_TAB, SDLK_TAB, false);

  Libs::Controller::PadReadState(1, &state);
  Check(state.analog_buttons_l2 == 192,
        "releasing keys must NOT reset L2 while Analog Lock is active");

  // Decrement L2 using Tab + Mouse Wheel Down while locked: it should decrement but NOT reset to 0
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_TAB, SDLK_TAB, true);
  Libs::Graphics::HostInputMouseWheel(-1); // 192 - 25 = 167
  Libs::Controller::PadReadState(1, &state);
  Check(state.analog_buttons_l2 == 167,
        "wheel down in caps lock should decrement L2");

  // Repeatedly decrement to test minimum clamping in caps lock ("non si azzerano")
  for (int i = 0; i < 10; ++i) {
    Libs::Graphics::HostInputMouseWheel(-1);
  }
  Libs::Controller::PadReadState(1, &state);
  Check(state.analog_buttons_l2 > 0,
        "decrementing while in caps lock must not zero out the analog input");

  // Update value again with Tab + R + Step (50%)
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_R, SDLK_r, true);
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_EQUALS, SDLK_EQUALS, true);
  Libs::Controller::PadReadState(1, &state);
  Check(state.analog_buttons_l2 == 128,
        "updating step while locked should change value to 128");

  // Release keys: remains frozen at 128
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_EQUALS, SDLK_EQUALS, false);
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_R, SDLK_r, false);
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_TAB, SDLK_TAB, false);
  Libs::Controller::PadReadState(1, &state);
  Check(state.analog_buttons_l2 == 128,
        "L2 value must remain frozen at 128 after releasing keys");

  // 5. Unlocking Caps Lock resets all analog values to 0
  Libs::Graphics::HostInputScancode(SDL_SCANCODE_CAPSLOCK, SDLK_CAPSLOCK, true);
  Check(!Libs::Graphics::HostInputCapsLockEnabled(),
        "second caps lock press should disable analog lock");
  Libs::Controller::PadReadState(1, &state);
  Check(state.analog_buttons_l2 == 0,
        "toggling Analog Lock off must reset L2 to 0");
  Check(state.analog_buttons_r2 == 0,
        "toggling Analog Lock off must reset R2 to 0");
  diag = Libs::Graphics::HostInputGetDiagnostics();
  Check(!diag.analog_lock,
        "diagnostics analog_lock must be false after unlocking");
  Check(!diag.adjustment_active,
        "diagnostics adjustment_active must be false after unlocking");

  Libs::Controller::Shutdown();
  Config::Shutdown();
  return 0;
}
