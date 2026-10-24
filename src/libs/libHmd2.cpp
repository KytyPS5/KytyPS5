#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/shader/shaderBindings.h"
#include "kernel/pthread.h"
#include "libs/agc.h"
#include "libs/errno.h"
#include "libs/hmd2.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>

namespace Libs {

LIB_VERSION("Hmd2", 1, "Hmd2", 1, 1);

namespace Hmd2 {

constexpr int32_t ERROR_ALREADY_INITIALIZED = -1972240383; // 0x8a720001
constexpr int32_t ERROR_NOT_INITIALIZED     = -1972240382; // 0x8a720002
constexpr int32_t ERROR_HANDLE_INVALID                   = -1972240381; // 0x8a720003
constexpr int32_t ERROR_PARAMETER_NULL      = -1972240376; // 0x8a720008
constexpr int32_t ERROR_PARAMETER_INVALID                = -1972240375; // 0x8a720009
constexpr int32_t ERROR_REPROJECTION_ALREADY_INITIALIZED = -1972240372; // 0x8a72000c
constexpr int32_t ERROR_REPROJECTION_IN_VR_MODE = -1972240356; // 0x8a72001c

// Any non-negative value is a valid headset handle.
constexpr int32_t  HMD_HANDLE                    = 0x0f000001;
constexpr uint32_t REPROJECTION_LAYER_TYPE_VR3D  = 0x10;
constexpr uint32_t REPROJECTION_LAYER_TYPE_VR2D  = 0x12;
constexpr uint32_t REPROJECTION_MAX_SOURCE_COUNT = 5;
constexpr int32_t  GAZE_TRACKING_STATUS_DISABLE  = -1;

struct SceHmd2InitializeParam {
	void*   reserved0;
	uint8_t reserved[8];
};

struct SceHmd2DeviceInformation {
	uint32_t status;
	uint32_t reserve0;
	struct {
		struct {
			uint32_t width;
			uint32_t height;
		} panelResolution;
		struct {
			uint16_t refreshRate90Hz;
			uint16_t refreshRate120Hz;
		} flipToDisplayLatency;
	} deviceInfo;
	uint8_t hmuMount;
	uint8_t lensSeparationDistance;
	uint8_t reserve1[2];
	float   virtualImageDistance;
};

struct SceHmd2FieldOfView {
	float tanOut;
	float tanIn;
	float tanTop;
	float tanBottom;
};

struct SceHmd2ReprojectionInitializeParam {
	void*    pReprojectionBuff;
	void*    pDisplayBuff;
	int32_t  threadPriority;
	int32_t  cpuAffinityMask;
	uint32_t pipeId;
	uint32_t queueId;
	uint32_t type;
	void*    pSeeThroughBuff;
	int32_t  reprojectionTiming;
	uint32_t reserved[5];
};

struct SceHmd2ReprojectionSource {
	uint32_t    layerType;
	uint32_t    data[73]; // VR3D: srcL texture, srcR texture, ...; VR2D: src texture, ...
	const void* option;
};

struct SceHmd2ReprojectionBuffer {
	const SceHmd2ReprojectionSource* src;
	uint32_t                         srcCount;
};

struct SceHmd2ReprojectionRenderConfig {
	uint32_t syncMode;
	uint32_t phase;
	uint32_t reserved[2];
};

struct SceHmd2ReprojectionParam {
	const void* settings;
	int32_t     settingCount;
	const void* option;
};

struct SceHmd2GazeGetResultParam {
	uint32_t sizeOfThis;
	int32_t  handle;
	int64_t  flipArg;
};

struct SceHmd2GazeEyeResult {
	int32_t isPupilDiameterValid;
	float   pupilDiameter;
	int32_t isBlink;
};

struct SceHmd2GazeResult {
	uint32_t             sizeOfThis;
	int32_t              status;
	uint64_t             timestamp;
	int32_t              isGazeValid;
	SceFVector3          gazeDirection;
	SceHmd2GazeEyeResult left;
	SceHmd2GazeEyeResult right;
};

struct SceHmd2GazeResultForFoveatedRendering {
	uint32_t    sizeOfThis;
	int32_t     status;
	SceFVector3 gazeDirection;
	uint32_t    reserved;
};

static_assert(sizeof(SceHmd2InitializeParam) == 16);
static_assert(offsetof(SceHmd2InitializeParam, reserved) == 8);
static_assert(sizeof(SceHmd2DeviceInformation) == 28);
static_assert(offsetof(SceHmd2DeviceInformation, deviceInfo) == 8);
static_assert(offsetof(SceHmd2DeviceInformation, hmuMount) == 20);
static_assert(offsetof(SceHmd2DeviceInformation, virtualImageDistance) == 24);
static_assert(sizeof(SceHmd2FieldOfView) == 16);
static_assert(alignof(SceHmd2FieldOfView) == 4);
static_assert(sizeof(SceHmd2ReprojectionInitializeParam) == 72);
static_assert(alignof(SceHmd2ReprojectionInitializeParam) == 8);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, threadPriority) == 16);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, type) == 32);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, pSeeThroughBuff) == 40);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, reprojectionTiming) == 48);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, reserved) == 52);
static_assert(sizeof(SceHmd2ReprojectionSource) == 304);
static_assert(offsetof(SceHmd2ReprojectionSource, option) == 296);
static_assert(sizeof(SceHmd2ReprojectionBuffer) == 16);
static_assert(sizeof(SceHmd2ReprojectionRenderConfig) == 16);
static_assert(sizeof(SceHmd2ReprojectionParam) == 24);
static_assert(offsetof(SceHmd2ReprojectionParam, option) == 16);
static_assert(sizeof(SceHmd2GazeGetResultParam) == 16);
static_assert(offsetof(SceHmd2GazeGetResultParam, flipArg) == 8);
static_assert(sizeof(SceHmd2GazeResult) == 56);
static_assert(offsetof(SceHmd2GazeResult, gazeDirection) == 20);
static_assert(offsetof(SceHmd2GazeResult, right) == 44);
static_assert(sizeof(SceHmd2GazeResultForFoveatedRendering) == 24);
static_assert(offsetof(SceHmd2GazeResultForFoveatedRendering, gazeDirection) == 8);

static std::atomic<bool> g_initialized = false;
static std::mutex g_reprojection_mutex;
static std::atomic<ReprojectionState*> g_reprojection_state {nullptr};

ReprojectionState* GetReprojectionState() {
	return g_reprojection_state.load(std::memory_order_acquire);
}

static Graphics::SizeAlign ReprojectionDisplaySize() {
	// The host display target is one uncompressed tiled 10-bit panel surface.
	Graphics::TileSizeAlign size {};
	Graphics::TileGetTextureTotalSize(Graphics::Prospero::BufferFormat::k10_10_10_2UNorm,
	                                  PANEL_WIDTH, PANEL_HEIGHT, 1, 1,
	                                  Graphics::Prospero::TileMode::kRenderTarget, false, size);
	EXIT_IF(size.size == 0 || size.align == 0);
	return {size.size, size.align};
}

static int32_t KYTY_SYSV_ABI Hmd2Initialize(const SceHmd2InitializeParam* param) {
	PRINT_NAME();
	if (param == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	if (g_initialized.exchange(true)) {
		return ERROR_ALREADY_INITIALIZED;
	}
	LOGF("Hmd2: initialized, virtual headset=%s\n", Config::VrEnabled() ? "enabled" : "disabled");
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2GetDeviceInformation(SceHmd2DeviceInformation* info) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INITIALIZED;
	}
	if (info == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	*info = {};
	if (Config::VrEnabled()) {
		// Virtual headset defaults: READY, MOUNT, 63 mm lens separation.
		info->hmuMount               = 1;
		info->lensSeparationDistance = LENS_SEPARATION_MM;
		// Headset characteristics: 4000x2040 panel and 2 m virtual image distance.
		info->deviceInfo.panelResolution = {PANEL_WIDTH, PANEL_HEIGHT};
		info->virtualImageDistance       = 2.0f;
		// Host presentation has no emulated headset scan-out delay; latency remains zero.
	} else {
		info->status = 2; // SCE_HMD2_DEVICE_STATUS_NOT_DETECTED
	}
	LOGF("Hmd2: device status=%u panel=%ux%u mounted=%u\n", info->status,
	     info->deviceInfo.panelResolution.width, info->deviceInfo.panelResolution.height,
	     info->hmuMount);
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2GetFieldOfViewWithoutHandle(SceHmd2FieldOfView* fov) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INITIALIZED;
	}
	if (fov == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	*fov = {FOV_TAN_OUT, FOV_TAN_IN, FOV_TAN_TOP, FOV_TAN_BOTTOM};
	LOGF("Hmd2: FOV tangents out=%f in=%f top=%f bottom=%f\n", fov->tanOut, fov->tanIn,
	     fov->tanTop, fov->tanBottom);
	return OK;
}

static Graphics::SizeAlign KYTY_SYSV_ABI Hmd2ReprojectionQueryBufferSizeAlign() {
	const Graphics::SizeAlign size {sizeof(ReprojectionState), alignof(ReprojectionState)};
	LOGF("Hmd2: reprojection work size=%" PRIu64 " alignment=%zu\n", size.m_size, size.m_align);
	return size;
}

static Graphics::SizeAlign KYTY_SYSV_ABI Hmd2ReprojectionQueryDisplayBufferSizeAlign() {
	const auto size = ReprojectionDisplaySize();
	LOGF("Hmd2: reprojection display size=%" PRIu64 " alignment=%zu\n", size.m_size,
	     size.m_align);
	return size;
}

static int32_t KYTY_SYSV_ABI
Hmd2ReprojectionInitialize(const SceHmd2ReprojectionInitializeParam* param, void*) {
	std::scoped_lock lock {g_reprojection_mutex};
	if (GetReprojectionState() != nullptr) {
		return ERROR_REPROJECTION_ALREADY_INITIALIZED;
	}
	if (param == nullptr || param->pReprojectionBuff == nullptr || param->pDisplayBuff == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	EXIT_NOT_IMPLEMENTED(param->pSeeThroughBuff != nullptr);
	const auto display_size = ReprojectionDisplaySize();
	auto* state = new (param->pReprojectionBuff) ReprojectionState;
	if (param->reprojectionTiming != 0) {
		state->timing_us.store(static_cast<uint32_t>(param->reprojectionTiming),
		                      std::memory_order_relaxed);
	}
	g_reprojection_state.store(state, std::memory_order_release);
	LOGF("Hmd2: reprojection initialized work=%p display=%p size=%" PRIu64 " timing=%u\n",
	     static_cast<void*>(state), param->pDisplayBuff, display_size.m_size,
	     state->timing_us.load(std::memory_order_relaxed));
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2ReprojectionEnableVrMode(uint64_t output_mode) {
	auto* state = GetReprojectionState();
	if (state == nullptr) {
		return ERROR_REPROJECTION_NOT_INITIALIZED;
	}
	if (output_mode != OUTPUT_MODE_89_91HZ && output_mode != OUTPUT_MODE_119_88HZ) {
		return VideoOut::VIDEO_OUT_ERROR_INVALID_VALUE;
	}
	if (!Config::VrEnabled()) {
		return VideoOut::VIDEO_OUT_ERROR_NO_DEVICE;
	}
	uint64_t expected = 0;
	if (!state->output_mode.compare_exchange_strong(expected, output_mode)) {
		return ERROR_REPROJECTION_IN_VR_MODE;
	}
	LOGF("Hmd2: VR output mode=0x%" PRIx64 "\n", output_mode);
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2Open(int32_t user_id, int32_t type, int32_t index,
                                      const void* param) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INITIALIZED;
	}
	if (user_id != Config::GetUserId() || type != 0 || index != 0 || param != nullptr) {
		return ERROR_PARAMETER_INVALID;
	}
	return HMD_HANDLE;
}

static int32_t KYTY_SYSV_ABI Hmd2ReprojectionBeginFrame(int64_t flip_arg) {
	PRINT_NAME();
	LOGF("\t flip_arg = %" PRId64 "\n", flip_arg);
	return OK;
}

// Microseconds of process time, the clock VrTracker2 predicts poses for. The headset shows a frame
// one refresh after it begins.
static int32_t KYTY_SYSV_ABI Hmd2ReprojectionGetPredictedDisplayTime(int64_t   flip_arg,
                                                                     uint64_t* timestamp) {
	PRINT_NAME();
	if (timestamp == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	const auto* state = GetReprojectionState();
	const bool  fast  = state != nullptr && state->output_mode.load() == OUTPUT_MODE_119_88HZ;
	*timestamp        = LibKernel::KernelGetProcessTime() + (fast ? 8342 : 11122);
	LOGF("\t flip_arg = %" PRId64 ", time = %" PRIu64 "\n", flip_arg, *timestamp);
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2ReprojectionSetRenderConfig(SceHmd2ReprojectionRenderConfig config,
                                                             const void* option) {
	PRINT_NAME();
	LOGF("\t sync_mode = %u, phase = %u\n", config.syncMode, config.phase);
	if (GetReprojectionState() == nullptr) {
		return ERROR_REPROJECTION_NOT_INITIALIZED;
	}
	return option != nullptr ? ERROR_PARAMETER_INVALID : OK;
}

static int32_t KYTY_SYSV_ABI Hmd2ReprojectionSetParam(int32_t index, int64_t flip_arg,
                                                      const SceHmd2ReprojectionParam* param,
                                                      const void*                     option) {
	PRINT_NAME();
	if (GetReprojectionState() == nullptr) {
		return ERROR_REPROJECTION_NOT_INITIALIZED;
	}
	if (param == nullptr || param->settings == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	LOGF("\t index = %d, flip_arg = %" PRId64 ", layers = %d\n", index, flip_arg,
	     param->settingCount);
	return option != nullptr ? ERROR_PARAMETER_INVALID : OK;
}

// No eye tracker: report DISABLE, which a PS5 also returns when the user turned eye tracking off.
template <typename Result>
static int32_t GazeResult(const SceHmd2GazeGetResultParam* param, Result* result) {
	if (param == nullptr || result == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	if (param->sizeOfThis != sizeof(*param) || result->sizeOfThis != sizeof(*result)) {
		return ERROR_PARAMETER_INVALID;
	}
	if (param->handle != HMD_HANDLE) {
		return ERROR_HANDLE_INVALID;
	}
	*result               = {};
	result->sizeOfThis    = sizeof(*result);
	result->status        = GAZE_TRACKING_STATUS_DISABLE;
	result->gazeDirection = {0.0f, 0.0f,
	                         -1.0f}; // The display center; +z points away from the lens.
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2GazeGetResult(const SceHmd2GazeGetResultParam* param,
                                               SceHmd2GazeResult*               result) {
	PRINT_NAME();
	const auto error = GazeResult(param, result);
	if (error == OK) {
		result->timestamp = LibKernel::KernelGetProcessTime();
	}
	return error;
}

static int32_t KYTY_SYSV_ABI Hmd2GazeGetResultForFoveatedRendering(
    const SceHmd2GazeGetResultParam* param, SceHmd2GazeResultForFoveatedRendering* result) {
	PRINT_NAME();
	return GazeResult(param, result);
}

bool GetReprojectionSurface(const void* reprojection_buffer, ReprojectionSurface& out) {
	const auto* buffer = static_cast<const SceHmd2ReprojectionBuffer*>(reprojection_buffer);
	if (buffer == nullptr || buffer->src == nullptr || buffer->srcCount == 0 ||
	    buffer->srcCount > REPROJECTION_MAX_SOURCE_COUNT ||
	    (buffer->src->layerType != REPROJECTION_LAYER_TYPE_VR3D &&
	     buffer->src->layerType != REPROJECTION_LAYER_TYPE_VR2D)) {
		return false;
	}
	Graphics::ShaderTextureResource texture;
	std::memcpy(texture.fields, buffer->src->data, sizeof(texture.fields));
	// Eye textures are RGBA ordered render targets; flip them through the matching SDR format.
	const auto& policies = Graphics::VIDEO_OUT_FORMAT_POLICIES;
	const auto  policy   = std::find_if(policies.begin(), policies.end(), [&](const auto& p) {
		return p.info.guest_format == texture.Format() &&
		       (p.pixel_format & 0xff000000u) == 0x22000000u &&
		       !Graphics::IsHdrVideoOutPixelFormat(p.pixel_format);
	});
	if (texture.IsNull() || texture.TileMode() != Graphics::Prospero::TileMode::kRenderTarget ||
	    policy == policies.end()) {
		LOGF("Hmd2: unsupported eye texture format=%u tile=%u\n",
		     static_cast<uint32_t>(texture.Format()), static_cast<uint32_t>(texture.TileMode()));
		return false;
	}
	if (buffer->src->layerType == REPROJECTION_LAYER_TYPE_VR3D &&
	    std::memcmp(buffer->src->data, buffer->src->data + std::size(texture.fields),
	                sizeof(texture.fields)) != 0) {
		Log::WriteToConsoleAndLog(
		    "Hmd2: separate eye textures are not supported; showing the left one\n");
	}
	out = {texture.Base40(), texture.Width5() + 1u, texture.Height5() + 1u, policy->pixel_format};
	return true;
}

static int32_t KYTY_SYSV_ABI Hmd2ReprojectionSetTiming(int32_t timing) {
	auto* state = GetReprojectionState();
	if (state == nullptr) {
		return ERROR_REPROJECTION_NOT_INITIALIZED;
	}
	EXIT_NOT_IMPLEMENTED(timing < 2000 || timing > 7000);
	const auto rounded = static_cast<uint32_t>((timing + 99) / 100 * 100);
	state->timing_us.store(rounded, std::memory_order_release);
	LOGF("Hmd2: reprojection timing=%u us\n", rounded);
	return OK;
}

} // namespace Hmd2

LIB_DEFINE(InitHmd2_1) {
	LIB_FUNC("c812oYs7Vsc", Hmd2::Hmd2Initialize);
	LIB_FUNC("bIi4YUfSRys", Hmd2::Hmd2GetDeviceInformation);
	LIB_FUNC("gF8+lvc7GuQ", Hmd2::Hmd2GetFieldOfViewWithoutHandle);
	LIB_FUNC("U-CnbmeyYaA", Hmd2::Hmd2ReprojectionQueryBufferSizeAlign);
	LIB_FUNC("-C2nkoEYOnU", Hmd2::Hmd2ReprojectionQueryDisplayBufferSizeAlign);
	LIB_FUNC("C0rPwER-yxg", Hmd2::Hmd2ReprojectionInitialize);
	LIB_FUNC("VVvFh51o20s", Hmd2::Hmd2ReprojectionEnableVrMode);
	LIB_FUNC("FkQX7rjFomk", Hmd2::Hmd2ReprojectionSetTiming);
	LIB_FUNC("f3kPeoTZnIE", Hmd2::Hmd2Open);
	LIB_FUNC("Ocf081WpBpA", Hmd2::Hmd2ReprojectionBeginFrame);
	LIB_FUNC("SVEG+1D7qHA", Hmd2::Hmd2ReprojectionGetPredictedDisplayTime);
	LIB_FUNC("hA9LshbSkzw", Hmd2::Hmd2ReprojectionSetRenderConfig);
	LIB_FUNC("xMo9ENEu2E0", Hmd2::Hmd2ReprojectionSetParam);
	LIB_FUNC("lAoFUedcfqA", Hmd2::Hmd2GazeGetResult);
	LIB_FUNC("retc+-uRMhk", Hmd2::Hmd2GazeGetResultForFoveatedRendering);
}

} // namespace Libs
