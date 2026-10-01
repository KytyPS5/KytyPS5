#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_AUDIO3D_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_AUDIO3D_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Audio {

namespace Audio3d {

struct Audio3dOpenParameters;

int KYTY_SYSV_ABI  Audio3dInitialize(int64_t reserved);
void KYTY_SYSV_ABI Audio3dGetDefaultOpenParameters(Audio3dOpenParameters* p);
int KYTY_SYSV_ABI  Audio3dPortOpen(int user_id, const Audio3dOpenParameters* parameters,
                                   uint32_t* id);
int KYTY_SYSV_ABI  Audio3dPortSetAttribute(uint32_t port_id, uint32_t attribute_id,
                                           const void* attribute, size_t attribute_size);
int KYTY_SYSV_ABI  Audio3dPortGetQueueLevel(uint32_t port_id, uint32_t* queue_level,
                                            uint32_t* queue_available);
int KYTY_SYSV_ABI  Audio3dPortAdvance(uint32_t port_id);
int KYTY_SYSV_ABI  Audio3dPortPush(uint32_t port_id, uint32_t blocking);

} // namespace Audio3d

} // namespace Libs::Audio

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_AUDIO3D_H_ */
