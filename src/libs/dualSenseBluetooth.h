#ifndef KYTY_LIBS_DUALSENSE_BLUETOOTH_H_
#define KYTY_LIBS_DUALSENSE_BLUETOOTH_H_

#include <cstdint>

namespace Libs::Controller::DualSenseBluetooth {
struct Stream;
Stream*  Open(uint32_t frequency, bool speaker, int controller, void (*tick)());
uint64_t Queue(Stream* stream, const float* samples, uint32_t frames);
void     Close(Stream* stream);
void     Shutdown();
} // namespace Libs::Controller::DualSenseBluetooth

#endif
