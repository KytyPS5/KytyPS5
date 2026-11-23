#ifndef EMULATOR_LIBS_SYSTEMSERVICE_H_
#define EMULATOR_LIBS_SYSTEMSERVICE_H_

#include <cstdint>

namespace Libs::SystemService {

constexpr int32_t SYSTEM_SERVICE_EVENT_ENTITLEMENT_UPDATE = 0x10000003;
constexpr int32_t SYSTEM_SERVICE_EVENT_GAME_INTENT        = 0x10000017;

void NotifyEvent(int32_t event_type);

} // namespace Libs::SystemService

#endif
