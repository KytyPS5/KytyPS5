#pragma once

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Audio::Audio3d {

struct Audio3dOpenParameters {
	size_t   size        = 0x20;
	uint32_t granularity = 256;
	uint32_t rate        = 0;
	uint32_t max_objects = 512;
	uint32_t queue_depth = 2;
	uint32_t buffer_mode = 2;
	uint32_t pad         = 0;
};

struct Audio3dAttribute {
	uint32_t    id;
	uint32_t    reserved;
	const void* value;
	size_t      size;
};

struct Audio3dPcm {
	uint32_t    format;
	const void* samples;
	uint32_t    count;
};

void               Shutdown();
int KYTY_SYSV_ABI  Audio3dInitialize(int64_t reserved);
void KYTY_SYSV_ABI Audio3dGetDefaultOpenParameters(Audio3dOpenParameters* params);
int KYTY_SYSV_ABI  Audio3dPortOpen(int user_id, const Audio3dOpenParameters* params, uint32_t* id);
int KYTY_SYSV_ABI  Audio3dPortClose(uint32_t id);
int KYTY_SYSV_ABI  Audio3dPortSetAttribute(uint32_t id, uint32_t attribute, const void* value,
                                           size_t size);
int KYTY_SYSV_ABI  Audio3dPortGetAttributesSupported(uint32_t id, uint32_t* capabilities,
                                                     uint32_t* count);
int KYTY_SYSV_ABI  Audio3dPortGetQueueLevel(uint32_t id, uint32_t* level, uint32_t* available);
int KYTY_SYSV_ABI  Audio3dPortAdvance(uint32_t id);
int KYTY_SYSV_ABI  Audio3dPortPush(uint32_t id, uint32_t blocking);
int KYTY_SYSV_ABI  Audio3dObjectReserve(uint32_t port, uint32_t* object);
int KYTY_SYSV_ABI  Audio3dObjectUnreserve(uint32_t port, uint32_t object);
int KYTY_SYSV_ABI  Audio3dObjectSetAttribute(uint32_t port, uint32_t object, uint32_t attribute,
                                             const void* value, size_t size);
int KYTY_SYSV_ABI  Audio3dObjectSetAttributes(uint32_t port, uint32_t object, size_t count,
                                              const Audio3dAttribute* attributes);

} // namespace Libs::Audio::Audio3d
