# Input Translation Layer from Keyboard to DualSense PS5

| # | DualSense Input | Keyboard + Mouse | Fallback |
|---|---|---|---|
| 1 | D-pad Up | Arrow ↑ | 1 |
| 2 | D-pad Left | Arrow ← | 2 |
| 3 | D-pad Down | Arrow ↓ | 3 |
| 4 | D-pad Right | Arrow → | 4 |
| 5 | Create / Share | \ | 9 |
| 6 | Left Stick | WASD | WASD |
| 7 | Right Stick | Mouse (movement) | Numeric keys |
| 8 | Microphone | Real microphone input | M + mouse wheel (continuous) *or* M + N + `+/-/=` (step 0/25/50/75/100) |
| 9 | Touch press (click) | Backspace | Backspace / Touch input on trackpad (e.g. laptops) / Mouse wheel click |
| 10 | Options | Enter | Enter |
| 11 | Square | J | Z |
| 12 | Cross | K | Shift |
| 13 | Circle | L | X |
| 14 | Triangle | I | C |
| 15 | L1 | Q | Q |
| 16 | L2 (analog 0-255) | Right click = 100%; Right click+Tab+wheel = continuous; Right click+Tab+`+/-/=` = step (signal active as long as click is held) | R = 100% click / Tab+R or Tab+Q (hold) + wheel/`+/-/=` (does not generate L1, but adjusts L2 press force) |
| 17 | R1 | E | E |
| 18 | R2 (analog 0-255) | Left click = 100%; Left click+Tab+wheel = continuous; Left click+Tab+`+/-/=` = step (signal active as long as click is held) | F = 100% click / Tab+F or Tab+E (hold) + wheel/`+/-/=` (does not generate R1, but adjusts R2 press force) |
| 19 | Gyroscope | `/` + wheel down = rotate right; `/` + wheel up = rotate left | `/`+`+` = rotate right; `/`+`-` = rotate left; `/`+`=` = reset (signal active as long as `/` is held) |
| 20 | Touch (drag/swipe) | Touch/Trackpad, invoked with H, otherwise the trackpad acts as a mouse. | H+directional arrows = swipe direction; H + Y = swipe up, H + U = swipe right, H + G = swipe left, H + B = swipe down (shortcuts) |
| 21 | PS | Delete | 6 |

## General logic

- **Left hand**: WASD (movement) + Q/E (bumpers) + Shift/Z/X/C (alternative face buttons)
- **Right hand**: Mouse (camera/triggers) or IJKL (face buttons) + arrows/numbers (D-pad) in keyboard-only mode
- **Tab** acts as a common modifier to enter "fine-adjustment mode" on L2/R2
- **Physical position/scancode of the key:**
Keyboard bindings are based on the physical position of the keys, using the QWERTY layout as the reference. The action associated with a key depends on its physical position/scancode, not on the character produced by the operating system's language layout. This way, the ergonomic layout of the configuration stays consistent across QWERTY, AZERTY, QWERTZ, and other layouts.

  - Example: *the physical position occupied by the `W` key in the reference QWERTY layout corresponds to the `Move Forward` action.*

  - The configuration GUI automatically adapts to the layout and physical configuration of the keyboard in use, showing the position and/or symbol of the corresponding key.

- **Last Active Input Source**, meaning the emulator "remembers" the last input source received. Useful for UI and HUD.
- **The keyboard remains always active** and serves as a fallback in case the controller disconnects or issues commands not supported by the controller.
- **Microphone input**, if possible, is taken from a microphone connected to the PC; otherwise, it is taken from the M + mouse wheel key, or M + N + `-/+/=`.
- **"Adjustment Lock"**: pressing the "CAPS LOCK" key "locks" the analog adjustment until the "CAPS LOCK" key is pressed again. In other words, you can release the key combinations used for the analog adjustment and/or perform other analog adjustments; they will remain active until "CAPS LOCK" is pressed again, which will reset the analog adjustments.
  - This allows the analog adjustment to be "locked" globally. (Thus avoiding reset, and freeing up keys like Tab for further analog adjustments)
  - The reset returns all analog inputs to a Default state. Therefore, the R2 and L2 triggers return to 0, the gyroscope stops and returns flat, and the microphone returns to 0.
  - Conflict with the system's Caps Lock: This could occur when text needs to be entered. The system function is not suppressed, but when entering an input field, e.g., a virtual keyboard, Caps Lock behaves according to the system.
  - When Caps Lock captures the step value reached at that moment, it remains active until "CAPS LOCK" is pressed again. However, if while it is active you press a Tab + Wheel or Tab + `+/-/=` combination again, the step value is updated to the new value reached.
  - This freeze allows the analog adjustment to be "locked" for:
    - R2 / L2 triggers
    - Microphone
    - Gyroscope
  - When the Lock is active, the analog value remains set without needing to keep the adjustment keys held down. To modify it, the Tab + wheel / `+/-/=` combination must be pressed again, which updates the frozen value to the new level reached.

## The configuration prioritizes:

* WASD for movement.
* Mouse for the right stick and therefore for the camera.
* Mouse buttons for the analog triggers.
* Q/E for L1/R1.
* Shift/Z/X/C for the four face buttons in the Keyboard + Mouse configuration.
* 1/2/3/4 as the D-pad fallback, accessible from the left hand.
* Layering via modifiers for secondary or analog inputs.
* Use of the PC's real microphone, when available.
* Virtual fallbacks for microphone, gyroscope, adaptive triggers, and other inputs not directly available.
* If the keys used for analog adjustment are released and the Caps Lock is not active, the system resets that analog input.

## Compatibility notes:

* The configuration is designed for the QWERTY keyboard layout.
* Keys must be "ScanCode-based" so they are independent of keyboard format / adapt to the keyboard's format.
* The configuration supports the following formats: 100%, 80%, 75%, 65%, 60%, Laptop.
* In configurations <40%, the mapping may not be optimal and may require manual adjustments or the use of the Fn + .. key to access keys not present on the keyboard.
* Every key is customizable via a dedicated configuration menu in the emulator's settings.

## Notes on the PS and Create/Share functions:

* The PS (PlayStation) function is mapped and can be activated; however, it does not open the PlayStation menu but opens an emulator-exclusive menu where you can view game statistics, trophies, etc.
* The Create/Share function is mapped and can be activated; however, it does not open the Create/Share menu but opens an emulator-exclusive menu where you can create/share your gameplay.

## Compatibility with DS4, Xbox, and third-party controllers:

* The configuration is designed to be compatible with DS4, Xbox, and third-party controllers.
* Functions not present on the controller are mapped to the corresponding PC fallbacks.

## Feedback and Adaptive Triggers:
* When adjustment mode is active and you're using the keyboard to gauge how much input you're applying, a colored frame border appears around the game, changing color based on intensity. (All based on gradients.)
* If using a DS4 or Xbox controller, the same applies (as an activatable option for feedback given by vibration).
* For gyroscope input via mouse and keyboard, a frame overlay appears when activated, showing a kind of colored bubble that moves in the direction the gyroscope moves.
* Feedback can be enabled in the settings for the microphone, showing the corresponding input level.
* Text input in games. If a game requests text input, it can be given directly via the keyboard, or through a virtual on-screen keyboard overlay navigable with arrows/joystick commands and touch.
* Visual feedback elements, such as for the gyroscope, remain active only when in use/needed.
* Each feedback option can be disabled from the settings.
* Visual feedback for Caps Lock/analog adjustment lock. When the analog adjustment lock is active, the Caps Lock visual feedback is triggered — both as a light on the keyboard and on-screen, displaying (if enabled in the settings) the corresponding indicator and keeping it active until the analog input is stopped.

## Input Capability Detection:
On startup of the Launcher and the emulator, the system automatically detects available input capabilities to provide debug information to users.
It also adapts feedback to the type of input in use.

## Direct keyboard input option:
* Some PlayStation games support mouse and keyboard input. For this reason, there is a setting option to disable controller emulation and expose the keyboard/mouse directly as input devices.
* All logic described above would then be ignored and disabled.
