#ifndef EMULATOR_INCLUDE_EMULATOR_DUALSENSE_HAPTICS_H_
#define EMULATOR_INCLUDE_EMULATOR_DUALSENSE_HAPTICS_H_

#include <cstdint>

namespace Libs::Controller::DualSenseHaptics {

struct Stream;

// A speaker stream plays a pad speaker port and routes the speaker to it; the others play vibration
// ports on the actuators.
Stream* Open(uint32_t freq, bool speaker);
void    Close(Stream* stream);
// True while this stream is routed through the asynchronous Bluetooth HID sender.
bool UsesBluetooth(const Stream* stream);
// Returns how long the queued audio still plays, in microseconds, or 0 when no DualSense took it
// (the active pad has no usable controller audio device or its speaker cannot be routed).
// Ambiguous USB or Bluetooth endpoints are not selected.
uint64_t Queue(Stream* stream, int controller, const void* data, uint32_t frames, uint32_t channels,
               bool is_float, const int* volume, float gain = 1.0f);
// Returns false for other gamepad types, which retain the normal rumble path.
bool SetVibration(int controller, uint8_t large_motor, uint8_t small_motor,
                  uint32_t duration_ms = 0xffff);
// Also restores the headphone routing.
void Shutdown();

} // namespace Libs::Controller::DualSenseHaptics

#endif // EMULATOR_INCLUDE_EMULATOR_DUALSENSE_HAPTICS_H_
