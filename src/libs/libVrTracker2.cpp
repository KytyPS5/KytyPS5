#include "common/abi.h"
#include "common/logging/log.h"
#include "graphics/presentation/vrInput.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/hmd2.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Libs {

LIB_VERSION("VrTracker2", 1, "VrTracker2", 1, 1);

namespace VrTracker2 {

constexpr int32_t ERROR_NOT_INIT            = -1972109311; // 0x8a740001
constexpr int32_t ERROR_ALREADY_INITIALIZED = -1972109310; // 0x8a740002
constexpr int32_t ERROR_ARGUMENT_INVALID    = -1972109306; // 0x8a740006

constexpr uint32_t MEMORY_SIZE      = 32u << 20u;
constexpr uint32_t MEMORY_ALIGNMENT = 0x10000;
// Device types: the headset, then the right and the left controller.
constexpr int32_t  DEVICE_TYPE_HMD                = 0;
constexpr int32_t  DEVICE_TYPE_VR_CONTROLLER_LEFT = 2;
constexpr uint32_t STATUS_TRACKING                = 1;
constexpr uint32_t QUALITY_FULL                   = 9;
constexpr uint8_t  MOTION_SENSOR_DATA_VALID       = 3; // Velocity and acceleration.

struct SceFQuaternion {
	float x;
	float y;
	float z;
	float w;
};

struct SceVrTracker2QueryMemoryResult {
	uint32_t sizeOfThis;
	uint32_t memorySize;
	uint32_t memoryAlignment;
	uint32_t reserved[5];
};

struct SceVrTracker2InitializeParam {
	uint32_t sizeOfThis;
	uint32_t profile;
	int32_t  highThreadPriority;
	int32_t  lowThreadPriority;
	uint64_t highThreadAffinity;
	uint64_t lowThreadAffinity;
	void*    memoryPointer;
	uint8_t  options[32]; // Memory size and tracking options; nothing here depends on them.
};

struct SceVrTracker2GetResultParam {
	uint32_t sizeOfThis;
	int32_t  handle;
	uint32_t reserved00;
	uint64_t predictionTime;
	uint32_t reserved01;
	int64_t  flipArg;
	uint32_t reserved02[2];
};

struct SceVrTracker2PoseData {
	Hmd2::SceFVector3 position;
	uint32_t          reserved00;
	SceFQuaternion    orientation;
	uint32_t          reserved01[8];
};

struct SceVrTracker2ResultData {
	int32_t           handle;
	uint32_t          connected;
	uint32_t          playAreaUpdateCounter;
	uint32_t          playAreaMode;
	uint64_t          timestamp;
	uint32_t          status;
	uint32_t          positionQuality;
	uint32_t          orientationQuality;
	Hmd2::SceFVector3 velocity;
	Hmd2::SceFVector3 acceleration;
	Hmd2::SceFVector3 angularVelocity;
	uint32_t          reserved01[3];
	uint8_t           motionSensorDataValidFlags;
	uint8_t           reserved010[3];
	int64_t           flipArg;
	union {
		struct {
			SceVrTracker2PoseData devicePose;
			SceVrTracker2PoseData leftEyePose;
			SceVrTracker2PoseData rightEyePose;
			SceVrTracker2PoseData headPose;
			SceVrTracker2PoseData gazeOriginPose;
		} hmdInfo;
		struct {
			SceVrTracker2PoseData devicePose;
			uint64_t              lastValidVisionTimestamp;
			uint64_t              lastValidSensorTimestamp;
			uint32_t              reserved[60];
		} vrControllerInfo;
	};
	uint32_t reserved02;
};

static_assert(sizeof(SceVrTracker2QueryMemoryResult) == 0x20);
static_assert(sizeof(SceVrTracker2InitializeParam) == 0x48);
static_assert(offsetof(SceVrTracker2InitializeParam, memoryPointer) == 0x20);
static_assert(sizeof(SceVrTracker2GetResultParam) == 0x30);
static_assert(offsetof(SceVrTracker2GetResultParam, flipArg) == 0x20);
static_assert(sizeof(SceVrTracker2PoseData) == 0x40);
static_assert(sizeof(SceVrTracker2ResultData) == 0x1a8);
static_assert(offsetof(SceVrTracker2ResultData, status) == 0x18);
static_assert(offsetof(SceVrTracker2ResultData, motionSensorDataValidFlags) == 0x54);
static_assert(offsetof(SceVrTracker2ResultData, flipArg) == 0x58);
static_assert(offsetof(SceVrTracker2ResultData, hmdInfo.devicePose) == 0x60);
static_assert(offsetof(SceVrTracker2ResultData, hmdInfo.gazeOriginPose) == 0x160);
static_assert(offsetof(SceVrTracker2ResultData, vrControllerInfo.lastValidVisionTimestamp) == 0xa0);

static std::atomic<bool> g_initialized = false;
// Handle registered for each device type.
static std::atomic<int32_t> g_devices[DEVICE_TYPE_VR_CONTROLLER_LEFT + 1] = {-1, -1, -1};

// Writes a tracking-space pose moved along its own x axis by shift metres. w stays non-negative so
// that a rotation always has one representation, whatever sign the runtime reports.
static void SetPose(SceVrTracker2PoseData& out, const Vr::VrPose& pose, float shift = 0.0f) {
	const auto [x, y, z, w] = pose.orientation;
	const float s           = w < 0.0f ? -1.0f : 1.0f;
	out.position            = {pose.position[0] + shift * (1.0f - 2.0f * (y * y + z * z)),
	                           pose.position[1] + shift * 2.0f * (x * y + w * z),
	                           pose.position[2] + shift * 2.0f * (x * z - w * y)};
	out.orientation         = {s * x, s * y, s * z, s * w};
}

static int32_t KYTY_SYSV_ABI QueryMemory(const void*                     param,
                                         SceVrTracker2QueryMemoryResult* result) {
	PRINT_NAME();
	if (param == nullptr || result == nullptr) {
		return ERROR_ARGUMENT_INVALID;
	}
	result->memorySize      = MEMORY_SIZE;
	result->memoryAlignment = MEMORY_ALIGNMENT;
	return OK;
}

static int32_t KYTY_SYSV_ABI Initialize(const SceVrTracker2InitializeParam* param) {
	PRINT_NAME();
	if (param == nullptr || param->memoryPointer == nullptr) {
		return ERROR_ARGUMENT_INVALID;
	}
	return g_initialized.exchange(true) ? ERROR_ALREADY_INITIALIZED : OK;
}

static int32_t KYTY_SYSV_ABI RegisterDevice(int32_t device_type, int32_t handle) {
	PRINT_NAME();
	LOGF("\t device_type = %d\n"
	     "\t handle      = %d\n",
	     device_type, handle);
	if (!g_initialized.load()) {
		return ERROR_NOT_INIT;
	}
	if (device_type < DEVICE_TYPE_HMD || device_type > DEVICE_TYPE_VR_CONTROLLER_LEFT ||
	    handle < 0) {
		return ERROR_ARGUMENT_INVALID;
	}
	g_devices[device_type].store(handle);
	return OK;
}

// Without a headset runtime every device reports the identity pose at the tracking origin.
static int32_t KYTY_SYSV_ABI GetResult(const SceVrTracker2GetResultParam* param,
                                       SceVrTracker2ResultData*           result) {
	PRINT_NAME();
	if (param == nullptr || result == nullptr) {
		return ERROR_ARGUMENT_INVALID;
	}
	if (!g_initialized.load()) {
		return ERROR_NOT_INIT;
	}
	std::memset(result, 0, sizeof(*result));
	result->handle  = param->handle;
	result->flipArg = param->flipArg;
	result->timestamp =
	    param->predictionTime != 0 ? param->predictionTime : LibKernel::KernelGetProcessTime();
	int32_t type = DEVICE_TYPE_HMD;
	while (type <= DEVICE_TYPE_VR_CONTROLLER_LEFT && g_devices[type].load() != param->handle) {
		type++;
	}
	if (param->handle < 0 || type > DEVICE_TYPE_VR_CONTROLLER_LEFT) {
		return OK; // Not registered: not connected, tracking not started.
	}
	result->connected                  = 1;
	result->status                     = STATUS_TRACKING;
	result->positionQuality            = QUALITY_FULL;
	result->orientationQuality         = QUALITY_FULL;
	result->motionSensorDataValidFlags = MOTION_SENSOR_DATA_VALID;
	if (type == DEVICE_TYPE_HMD) {
		const auto pose = Vr::OpenXrGetHeadPose();
		auto&      hmd  = result->hmdInfo;
		SetPose(hmd.devicePose, pose);
		SetPose(hmd.leftEyePose, pose, -Hmd2::EYE_OFFSET_M);
		SetPose(hmd.rightEyePose, pose, Hmd2::EYE_OFFSET_M);
		hmd.headPose       = hmd.devicePose;
		hmd.gazeOriginPose = hmd.devicePose;
		return OK;
	}
	auto& controller = result->vrControllerInfo;
	SetPose(controller.devicePose,
	        Vr::OpenXrGetController(type == DEVICE_TYPE_VR_CONTROLLER_LEFT ? 0 : 1).pose);
	controller.lastValidVisionTimestamp = result->timestamp;
	controller.lastValidSensorTimestamp = result->timestamp;
	return OK;
}

} // namespace VrTracker2

LIB_DEFINE(InitVrTracker2_1) {
	LIB_FUNC("TwqZnaIjWv4", VrTracker2::QueryMemory);
	LIB_FUNC("6Jy73SRfG-o", VrTracker2::Initialize);
	LIB_FUNC("Dog+g25QYjw", VrTracker2::RegisterDevice);
	LIB_FUNC("J4Vh3VVX0iU", VrTracker2::GetResult);
}

} // namespace Libs
