#ifndef EMULATOR_INCLUDE_EMULATOR_DUALSENSE_HAPTICS_H_
#define EMULATOR_INCLUDE_EMULATOR_DUALSENSE_HAPTICS_H_

#include <cstdint>

namespace Libs::Controller::DualSenseHaptics {

struct Stream;

Stream* Open(uint32_t freq);
void    Close(Stream* stream);
void    Queue(Stream* stream, int controller, const void* data, uint32_t frames, uint32_t channels,
              bool is_float, const int* volume);
// Returns false for other gamepad types, which retain the normal rumble path.
bool SetVibration(int controller, uint8_t large_motor, uint8_t small_motor);
void Shutdown();

} // namespace Libs::Controller::DualSenseHaptics

#endif // EMULATOR_INCLUDE_EMULATOR_DUALSENSE_HAPTICS_H_
