#include "graphics/guest_gpu/pm4.h"
#include "graphics/guest_gpu/pssr.h"
#include "graphics/shader/shaderBindings.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace Libs::LibPsml {

LIB_VERSION("Psml", 1, "Psml", 1, 1);

namespace {

namespace Pm4  = Graphics::Pm4;
namespace Pssr = Graphics::Pssr;

constexpr int      NOT_INITIALIZED           = static_cast<int>(0x8a810001u);
constexpr int      ALREADY_INITIALIZED       = static_cast<int>(0x8a810002u);
constexpr int      TOO_MANY_CONTEXTS         = static_cast<int>(0x8a810004u);
constexpr int      INVALID_CONTEXT           = static_cast<int>(0x8a810005u);
constexpr int      COMMAND_BUFFER_FULL       = static_cast<int>(0x8a810006u);
constexpr int      COMMAND_BUFFER_BAD        = static_cast<int>(0x8a810007u);
constexpr int      INSUFFICIENT_BUFFER       = static_cast<int>(0x8a810008u);
constexpr int      ARGUMENT_NULL             = static_cast<int>(0x8a810009u);
constexpr int      TEXTURE_NULL              = static_cast<int>(0x8a81000au);
constexpr int      INVALID_TEXTURE           = static_cast<int>(0x8a81000bu);
constexpr int      INVALID_PARAMETER         = static_cast<int>(0x8a81000du);
constexpr int      UNSUPPORTED_RENDER_SIZE   = static_cast<int>(0x8a81000eu);
constexpr int      INVALID_OUTPUT_TEXTURE    = static_cast<int>(0x8a81000fu);
constexpr int      NOT_TRINITY_MODE          = static_cast<int>(0x8a810012u);
constexpr int      UNSUPPORTED_OUTPUT_SIZE   = static_cast<int>(0x8a810013u);
constexpr int      INVALID_SHARED_RESOURCES  = static_cast<int>(0x8a810014u);
constexpr int      TOO_MANY_SHARED_RESOURCES = static_cast<int>(0x8a810015u);
constexpr uint64_t MEMORY_BLOCK_SIZE         = 0x200000;
constexpr uint32_t PACKET_DWORDS             = 1 + Pssr::RequestDwords;

struct MemoryRequirements {
	uint64_t block_size;
	uint64_t alignment;
	uint64_t block_count;
};

struct DirectMemoryBlock {
	uint64_t offset;
	uint64_t size;
};

struct MemoryParameters {
	uint32_t                 profile;
	uint32_t                 flags;
	const DirectMemoryBlock* blocks;
	uint64_t                 block_count;
};

struct SharedParameters {
	MemoryParameters memory;
	uint64_t         virtual_address;
};

struct ContextParameters {
	MemoryParameters memory;
	uint16_t         width;
	uint16_t         height;
	uint32_t         reserved;
	const void*      depth_copy;
	const void*      motion_copy;
	const char*      name;
	void*            shared;
};
static_assert(offsetof(ContextParameters, width) == 0x18);
static_assert(offsetof(ContextParameters, shared) == 0x38);

struct DispatchParameters {
	const uint32_t* output;
	const uint32_t* color;
	const uint32_t* motion;
	const uint32_t* previous_depth;
	const uint32_t* depth;
	const uint32_t* exposure;
	uint8_t         unused0[0x34];
	float           pre_exposure;
	uint8_t         unused1[0x88];
	uint32_t        width;
	uint32_t        height;
	uint32_t        x;
	uint32_t        y;
	float           jitter[2];
	float           motion_scale[2];
	float           motion_offset[2];
	float           motion_gamma;
	uint8_t         reset;
	uint32_t        flags;
	const void*     vr_lookup;
};
static_assert(offsetof(DispatchParameters, pre_exposure) == 0x64);
static_assert(offsetof(DispatchParameters, width) == 0xf0);
static_assert(offsetof(DispatchParameters, vr_lookup) == 0x128);

// The older layout: no render offset or VR lookup, and only two flags.
struct LegacyDispatchParameters {
	uint8_t  render[offsetof(DispatchParameters, x)];
	uint8_t  motion[offsetof(DispatchParameters, flags) - offsetof(DispatchParameters, jitter)];
	uint32_t flags;
};
static_assert(offsetof(LegacyDispatchParameters, flags) == 0x118);

constexpr uint32_t VIEW_DEPTH    = 1u << 14;
constexpr uint32_t REVERSE_DEPTH = 1u << 15;
constexpr uint32_t PASS_THROUGH  = 1u << 31;

struct SharedResources {
	uint32_t contexts = 0;
};

struct Context {
	uint32_t         width;
	uint32_t         height;
	SharedResources* shared;
};

struct CommandSpace {
	uint32_t* cmd;
	uint32_t  dwords;
};

std::mutex                                                g_mutex;
bool                                                      g_initialized = false;
std::unique_ptr<SharedResources>                          g_shared;
std::unordered_map<const void*, std::unique_ptr<Context>> g_contexts;

uint64_t SharedBlockCount(uint32_t profile) {
	constexpr uint64_t bytes[] = {0x06200000, 0x18200000, 0x12200000};
	return profile < 3 ? (bytes[profile] + 0x600000) / MEMORY_BLOCK_SIZE : 0;
}

int CheckContext(const void* context) {
	if (!g_initialized) {
		return NOT_INITIALIZED;
	}
	return g_contexts.contains(context) ? OK : INVALID_CONTEXT;
}

void CopyDescriptor(uint32_t (&out)[8], const uint32_t* descriptor) {
	if (descriptor != nullptr) {
		std::memcpy(out, descriptor, sizeof(out));
	}
}

// Both dispatch parameter structs name their common fields alike.
template <typename Parameters>
Pssr::Request MakeRequest(const void* context, const Parameters& params, uint32_t flags,
                          float pre_exposure) {
	Pssr::Request request {.width        = params.width,
	                       .height       = params.height,
	                       .context      = reinterpret_cast<uintptr_t>(context),
	                       .x            = params.x,
	                       .y            = params.y,
	                       .jitter       = {params.jitter[0], params.jitter[1]},
	                       .motion_scale = {params.motion_scale[0], params.motion_scale[1]},
	                       .pre_exposure = pre_exposure};
	CopyDescriptor(request.output, params.output);
	CopyDescriptor(request.input, params.color);
	CopyDescriptor(request.depth, params.depth);
	CopyDescriptor(request.motion, params.motion);
	CopyDescriptor(request.exposure, params.exposure);
	if (params.reset != 0) {
		flags |= Pssr::kReset;
	}
	if (params.motion_offset[0] != 0.0f || params.motion_offset[1] != 0.0f) {
		flags |= Pssr::kSpatialOnly;
	}
	request.flags = flags;
	return request;
}

void WritePacket(const CommandSpace& space, const Pssr::Request& request) {
	space.cmd[0] = KYTY_PM4(space.dwords, Pm4::IT_NOP, Pm4::R_PSSR);
	std::memcpy(space.cmd + 1, &request, sizeof(request));
	std::memset(space.cmd + PACKET_DWORDS, 0, (space.dwords - PACKET_DWORDS) * sizeof(uint32_t));
}

int KYTY_SYSV_ABI Initialize() {
	std::lock_guard lock(g_mutex);
	if (!Loader::SystemContentIsTrinityMode()) {
		return NOT_TRINITY_MODE;
	}
	if (g_initialized) {
		return ALREADY_INITIALIZED;
	}
	g_initialized = true;
	return OK;
}

int KYTY_SYSV_ABI GetSharedRequirements(MemoryRequirements* out, const MemoryParameters* params) {
	std::lock_guard lock(g_mutex);
	if (!g_initialized) {
		return NOT_INITIALIZED;
	}
	if (out == nullptr || params == nullptr) {
		return ARGUMENT_NULL;
	}
	const auto blocks = SharedBlockCount(params->profile);
	if (blocks == 0) {
		return INVALID_PARAMETER;
	}
	*out = {MEMORY_BLOCK_SIZE, MEMORY_BLOCK_SIZE, blocks};
	return OK;
}

int KYTY_SYSV_ABI CreateSharedResources(void** out, const SharedParameters* params) {
	std::lock_guard lock(g_mutex);
	if (!g_initialized) {
		return NOT_INITIALIZED;
	}
	if (out == nullptr || params == nullptr || params->memory.blocks == nullptr) {
		return ARGUMENT_NULL;
	}
	const auto required = SharedBlockCount(params->memory.profile);
	if (required == 0 || params->memory.flags != 0) {
		return INVALID_PARAMETER;
	}
	if (params->memory.block_count < required) {
		return INSUFFICIENT_BUFFER;
	}
	if (g_shared) {
		return TOO_MANY_SHARED_RESOURCES;
	}
	g_shared = std::make_unique<SharedResources>();
	*out     = g_shared.get();
	return OK;
}

int KYTY_SYSV_ABI ReleaseSharedResources(void* resources) {
	std::lock_guard lock(g_mutex);
	if (!g_initialized) {
		return NOT_INITIALIZED;
	}
	if (!g_shared || resources != g_shared.get()) {
		return INVALID_SHARED_RESOURCES;
	}
	if (g_shared->contexts != 0) {
		return INVALID_CONTEXT;
	}
	g_shared.reset();
	return OK;
}

int KYTY_SYSV_ABI GetContextRequirements(MemoryRequirements* out, const void* params) {
	std::lock_guard lock(g_mutex);
	if (!g_initialized) {
		return NOT_INITIALIZED;
	}
	if (out == nullptr || params == nullptr) {
		return ARGUMENT_NULL;
	}
	*out = {MEMORY_BLOCK_SIZE, MEMORY_BLOCK_SIZE, 1};
	return OK;
}

int CreateContext(void** out, const MemoryParameters* memory, uint32_t width, uint32_t height,
                  void* shared, bool use_shared) {
	std::lock_guard lock(g_mutex);
	if (!g_initialized) {
		return NOT_INITIALIZED;
	}
	if (out == nullptr || memory == nullptr || memory->blocks == nullptr) {
		return ARGUMENT_NULL;
	}
	if (memory->profile > 2) {
		return INVALID_PARAMETER;
	}
	if (memory->block_count == 0) {
		return INSUFFICIENT_BUFFER;
	}
	if (width == 0 || height == 0 || width > 16384 || height > 16384) {
		return UNSUPPORTED_OUTPUT_SIZE;
	}
	if (use_shared && (!g_shared || shared != g_shared.get())) {
		return INVALID_SHARED_RESOURCES;
	}
	if (g_contexts.size() == 4) {
		return TOO_MANY_CONTEXTS;
	}
	auto record =
	    std::make_unique<Context>(Context {width, height, use_shared ? g_shared.get() : nullptr});
	auto* handle = record.get();
	g_contexts.emplace(handle, std::move(record));
	if (handle->shared != nullptr) {
		++handle->shared->contexts;
	}
	*out = handle;
	return OK;
}

int KYTY_SYSV_ABI CreateFixedContext(void** out, const MemoryParameters* params) {
	const bool large = params != nullptr && params->profile == 1;
	return CreateContext(out, params, large ? 7680 : 3840, large ? 4320 : 2160, nullptr, false);
}

int KYTY_SYSV_ABI CreatePrivateContext(void** out, const ContextParameters* params) {
	return CreateContext(out, params == nullptr ? nullptr : &params->memory,
	                     params == nullptr ? 0 : params->width,
	                     params == nullptr ? 0 : params->height, nullptr, false);
}

int KYTY_SYSV_ABI CreateSharedContext(void** out, const ContextParameters* params) {
	return CreateContext(
	    out, params == nullptr ? nullptr : &params->memory, params == nullptr ? 0 : params->width,
	    params == nullptr ? 0 : params->height, params == nullptr ? nullptr : params->shared, true);
}

int KYTY_SYSV_ABI ReleaseContext(void* context) {
	std::lock_guard lock(g_mutex);
	if (const int error = CheckContext(context); error != OK) {
		return error;
	}
	auto& record = g_contexts.at(context);
	if (record->shared != nullptr) {
		--record->shared->contexts;
	}
	g_contexts.erase(context);
	return OK;
}

int KYTY_SYSV_ABI GetPacketSize(const void* context, uint32_t* out_size) {
	std::lock_guard lock(g_mutex);
	if (const int error = CheckContext(context); error != OK) {
		return error;
	}
	if (out_size == nullptr) {
		return ARGUMENT_NULL;
	}
	*out_size = PACKET_DWORDS;
	return OK;
}

int KYTY_SYSV_ABI Dispatch(const void* context, const CommandSpace* space,
                           const DispatchParameters* params) {
	std::lock_guard lock(g_mutex);
	if (const int error = CheckContext(context); error != OK) {
		return error;
	}
	if (space == nullptr || params == nullptr || space->cmd == nullptr) {
		return ARGUMENT_NULL;
	}
	if (space->dwords < PACKET_DWORDS) {
		return COMMAND_BUFFER_FULL;
	}
	if (space->dwords > 0x4001) {
		return COMMAND_BUFFER_BAD;
	}
	if (params->output == nullptr || params->color == nullptr) {
		return TEXTURE_NULL;
	}
	Graphics::ShaderTextureResource input, output;
	std::memcpy(input.fields, params->color, sizeof(input.fields));
	std::memcpy(output.fields, params->output, sizeof(output.fields));
	namespace Gpu    = Graphics::Prospero;
	const auto is_2d = [](const Graphics::ShaderTextureResource& descriptor) {
		return !descriptor.IsNull() &&
		       (descriptor.Type() == Gpu::ImageType::kColor2D ||
		        descriptor.Type() == Gpu::ImageType::kColor2DArray) &&
		       descriptor.TileMode() != Gpu::TileMode::kDepth &&
		       descriptor.BaseLevel() <= descriptor.LastLevel() &&
		       descriptor.LastLevel() <= descriptor.MaxMip() &&
		       descriptor.BaseArray5() <= descriptor.Depth();
	};
	if (!is_2d(input) || !Pssr::IsColorFormat(input.Format())) {
		return INVALID_TEXTURE;
	}
	if (!is_2d(output) || output.Format() != Gpu::BufferFormat::k11_11_10Float ||
	    output.TileMode() != Gpu::TileMode::kStandard256B || output.BaseLevel() != 0 ||
	    output.LastLevel() != 0 || output.MaxMip() != 0 || output.BaseArray5() != 0 ||
	    output.Depth() != 0 || input.Base40() == output.Base40()) {
		return INVALID_OUTPUT_TEXTURE;
	}
	const uint32_t input_width  = std::max(1u, (input.Width5() + 1u) >> input.BaseLevel());
	const uint32_t input_height = std::max(1u, (input.Height5() + 1u) >> input.BaseLevel());
	if (params->width == 0 || params->height == 0 || params->width > input_width ||
	    params->height > input_height) {
		return UNSUPPORTED_RENDER_SIZE;
	}
	const auto& record = *g_contexts.at(context);
	if (uint32_t(output.Width5()) + 1 != record.width ||
	    uint32_t(output.Height5()) + 1 != record.height) {
		return INVALID_OUTPUT_TEXTURE;
	}
	// Reverse depth and auto exposure are bits 15 and 16.
	uint32_t flags = (params->flags >> 15) & (Pssr::kReverseDepth | Pssr::kAutoExposure);
	if ((params->flags & (VIEW_DEPTH | PASS_THROUGH)) != 0 || params->vr_lookup != nullptr ||
	    (params->motion_gamma != 0.0f && params->motion_gamma != 1.0f)) {
		flags |= Pssr::kSpatialOnly;
	}
	WritePacket(*space, MakeRequest(context, *params, flags,
	                                params->pre_exposure == 0.0f ? 1.0f : params->pre_exposure));
	return OK;
}

int KYTY_SYSV_ABI LegacyDispatch(const void* context, const CommandSpace* space,
                                 const LegacyDispatchParameters* legacy) {
	if (legacy == nullptr) {
		return Dispatch(context, space, nullptr);
	}
	DispatchParameters params {};
	auto*              bytes = reinterpret_cast<uint8_t*>(&params);
	std::memcpy(bytes, legacy->render, sizeof(legacy->render));
	std::memcpy(bytes + offsetof(DispatchParameters, jitter), legacy->motion,
	            sizeof(legacy->motion));
	params.flags = legacy->flags & (REVERSE_DEPTH | PASS_THROUGH);
	return Dispatch(context, space, &params);
}

int KYTY_SYSV_ABI GetMipmapBias(const void* context, float* bias, uint32_t width, uint32_t height) {
	std::lock_guard lock(g_mutex);
	if (const int error = CheckContext(context); error != OK) {
		return error;
	}
	if (bias == nullptr) {
		return ARGUMENT_NULL;
	}
	if (width == 0 || height == 0) {
		return UNSUPPORTED_RENDER_SIZE;
	}
	*bias = std::log2(float(width) / float(g_contexts.at(context)->width)) - 1.0f;
	return OK;
}

int KYTY_SYSV_ABI ValidateContext(const void* context) {
	std::lock_guard lock(g_mutex);
	return CheckContext(context);
}

} // namespace

namespace Mfsr2 {

LIB_VERSION("Psml_mfsr2", 1, "Psml", 1, 1);

namespace {

constexpr int      NOT_INITIALIZED           = static_cast<int>(0x8a8a0001u);
constexpr int      ALREADY_INITIALIZED       = static_cast<int>(0x8a8a0002u);
constexpr int      INVALID_PROFILE           = static_cast<int>(0x8a8a0003u);
constexpr int      INVALID_CONTEXT           = static_cast<int>(0x8a8a0005u);
constexpr int      ARGUMENT_NULL             = static_cast<int>(0x8a8a0009u);
constexpr int      TEXTURE_NULL              = static_cast<int>(0x8a8a000au);
constexpr int      INVALID_TEXTURE           = static_cast<int>(0x8a8a000bu);
constexpr int      INVALID_PARAMETER         = static_cast<int>(0x8a8a000du);
constexpr int      UNSUPPORTED_RENDER_SIZE   = static_cast<int>(0x8a8a000eu);
constexpr int      NOT_AVAILABLE             = static_cast<int>(0x8a8a0012u);
constexpr int      INVALID_SHARED_RESOURCES  = static_cast<int>(0x8a8a0014u);
constexpr int      TOO_MANY_SHARED_RESOURCES = static_cast<int>(0x8a8a0015u);
constexpr uint64_t SHARED_BLOCK_COUNT        = 123;
constexpr uint32_t DISPATCH_DWORDS           = 0x800;

struct SharedParameters {
	MemoryParameters memory;
	uint16_t         max_width;
	uint16_t         max_height;
	uint32_t         reserved;
};

struct ContextParameters {
	uint32_t                 profile;
	uint32_t                 reserved;
	const void*              shared;
	const DirectMemoryBlock* blocks;
	uint64_t                 block_count;
	const char*              name;
	uint16_t                 width;
	uint16_t                 height;
	uint32_t                 color_space;
};
static_assert(offsetof(ContextParameters, width) == 0x28);

struct DispatchParameters {
	const uint32_t* output;
	const uint32_t* color;
	const uint32_t* motion;
	const uint32_t* depth;
	const uint32_t* exposure;
	const uint32_t* particle_mask;
	float           pre_exposure;
	uint32_t        x;
	uint32_t        y;
	uint32_t        width;
	uint32_t        height;
	float           jitter[2];
	float           motion_scale[2];
	float           motion_offset[2];
	float           depth_range[2];
	uint8_t         reset;
	uint8_t         reserved;
	uint8_t         encoding; // 0 is linear
	uint8_t         particle_bits;
	uint32_t        flags;
};
static_assert(offsetof(DispatchParameters, x) == 0x34);
static_assert(offsetof(DispatchParameters, flags) == 0x68);

struct Context {
	bool     used;
	uint32_t width;
	uint32_t height;
};

bool    g_initialized = false;
bool    g_shared      = false;
Context g_contexts[4] {};

int CheckSharedParameters(const void* out, const SharedParameters* params) {
	if (out == nullptr || params == nullptr) {
		return ARGUMENT_NULL;
	}
	if (params->memory.profile != 0) {
		return INVALID_PROFILE;
	}
	return params->max_width < 960 || params->max_width > 7680 || params->max_height < 540 ||
	               params->max_height > 4320
	           ? INVALID_PARAMETER
	           : OK;
}

int CheckContextParameters(const void* out, const ContextParameters* params) {
	if (out == nullptr || params == nullptr) {
		return ARGUMENT_NULL;
	}
	if (params->profile != 0) {
		return INVALID_PROFILE;
	}
	if (params->shared == nullptr) {
		return INVALID_SHARED_RESOURCES;
	}
	return params->reserved != 0 || params->width == 0 || params->height == 0 ||
	               params->color_space >= 2
	           ? INVALID_PARAMETER
	           : OK;
}

uint64_t ContextBlockCount(const ContextParameters& params) {
	const uint64_t bytes =
	    uint64_t {8} * ((params.width + 7u) & ~7u) * ((params.height + 7u) & ~7u) + 0x7f00;
	return (bytes + MEMORY_BLOCK_SIZE - 1) / MEMORY_BLOCK_SIZE;
}

int CheckDispatch(const Context* context, const DispatchParameters* params) {
	if (context == nullptr) {
		return INVALID_CONTEXT;
	}
	if (params == nullptr) {
		return ARGUMENT_NULL;
	}
	if (params->output == nullptr) {
		return TEXTURE_NULL;
	}
	if (params->width == 0 || params->height == 0) {
		return UNSUPPORTED_RENDER_SIZE;
	}
	Graphics::ShaderTextureResource output, color;
	std::memcpy(output.fields, params->output, sizeof(output.fields));
	if (output.Width5() + 1u < context->width || output.Height5() + 1u < context->height) {
		return INVALID_TEXTURE;
	}
	const auto is_valid = [](const Graphics::ShaderTextureResource& descriptor) {
		return Pssr::IsColorFormat(descriptor.Format()) &&
		       descriptor.Type() == Graphics::Prospero::ImageType::kColor2D &&
		       descriptor.BaseLevel() == 0 && descriptor.LastLevel() == 0 &&
		       descriptor.BaseArray5() == 0 && descriptor.Depth() == 0;
	};
	if (!is_valid(output)) {
		return INVALID_TEXTURE;
	}
	if (params->color == nullptr) {
		return TEXTURE_NULL;
	}
	std::memcpy(color.fields, params->color, sizeof(color.fields));
	return !is_valid(color) || params->x + params->width > color.Width5() + 1u ||
	               params->y + params->height > color.Height5() + 1u
	           ? INVALID_TEXTURE
	           : OK;
}

int KYTY_SYSV_ABI Initialize() {
	std::lock_guard lock(g_mutex);
	if (g_initialized) {
		return ALREADY_INITIALIZED;
	}
	if (!Loader::SystemContentIsTrinityMode()) {
		return NOT_AVAILABLE;
	}
	g_initialized = true;
	return OK;
}

int KYTY_SYSV_ABI GetSharedRequirements(MemoryRequirements* out, const SharedParameters* params) {
	std::lock_guard lock(g_mutex);
	if (!g_initialized) {
		return NOT_INITIALIZED;
	}
	if (const int error = CheckSharedParameters(out, params); error != OK) {
		return error;
	}
	*out = {MEMORY_BLOCK_SIZE, MEMORY_BLOCK_SIZE, SHARED_BLOCK_COUNT};
	return OK;
}

int KYTY_SYSV_ABI CreateSharedResources(void** out, const SharedParameters* params) {
	std::lock_guard lock(g_mutex);
	if (!g_initialized) {
		return NOT_INITIALIZED;
	}
	if (g_shared) {
		return TOO_MANY_SHARED_RESOURCES;
	}
	if (const int error = CheckSharedParameters(out, params); error != OK) {
		return error;
	}
	if (params->reserved != 0 || params->memory.block_count != SHARED_BLOCK_COUNT) {
		return INVALID_PARAMETER;
	}
	g_shared = true;
	*out     = &g_shared;
	return OK;
}

int KYTY_SYSV_ABI ReleaseSharedResources(void* /*resources*/) {
	std::lock_guard lock(g_mutex);
	if (!g_initialized) {
		return NOT_INITIALIZED;
	}
	g_shared = false;
	return OK;
}

int KYTY_SYSV_ABI GetContextRequirements(MemoryRequirements* out, const ContextParameters* params) {
	std::lock_guard lock(g_mutex);
	if (!g_initialized) {
		return NOT_INITIALIZED;
	}
	if (const int error = CheckContextParameters(out, params); error != OK) {
		return error;
	}
	*out = {MEMORY_BLOCK_SIZE, MEMORY_BLOCK_SIZE, ContextBlockCount(*params)};
	return OK;
}

int KYTY_SYSV_ABI CreateContext(Context** out, const ContextParameters* params) {
	std::lock_guard lock(g_mutex);
	if (!g_initialized) {
		return NOT_INITIALIZED;
	}
	auto* slot = std::ranges::find(g_contexts, false, &Context::used);
	if (slot == std::end(g_contexts)) {
		return TOO_MANY_SHARED_RESOURCES;
	}
	if (const int error = CheckContextParameters(out, params); error != OK) {
		return error;
	}
	if (params->block_count != ContextBlockCount(*params)) {
		return INVALID_PARAMETER;
	}
	*slot = {true, params->width, params->height};
	*out  = slot;
	return OK;
}

int KYTY_SYSV_ABI ReleaseContext(Context* context) {
	std::lock_guard lock(g_mutex);
	if (!g_initialized) {
		return NOT_INITIALIZED;
	}
	context->used = false;
	return OK;
}

int KYTY_SYSV_ABI GetDispatchDwords(const Context* /*context*/, uint32_t* out) {
	std::lock_guard lock(g_mutex);
	if (!g_initialized) {
		return NOT_INITIALIZED;
	}
	*out = DISPATCH_DWORDS;
	return OK;
}

int KYTY_SYSV_ABI GetDispatchPackets(const Context* context, const CommandSpace* space,
                                     const DispatchParameters* params) {
	std::lock_guard lock(g_mutex);
	if (!g_initialized) {
		return NOT_INITIALIZED;
	}
	if (const int error = CheckDispatch(context, params); error != OK) {
		space->cmd[0] = KYTY_PM4(space->dwords, Pm4::IT_NOP, Pm4::R_ZERO);
		return error;
	}
	constexpr uint32_t DILATED_MOTION = 1u << 2;
	// Reverse depth and auto exposure are bits 0 and 1.
	uint32_t flags = params->flags & (Pssr::kReverseDepth | Pssr::kAutoExposure);
	if (params->encoding != 0 || (params->flags & DILATED_MOTION) != 0) {
		flags |= Pssr::kSpatialOnly;
	}
	WritePacket(*space, MakeRequest(context, *params, flags, params->pre_exposure));
	return OK;
}

} // namespace

LIB_DEFINE(InitPsmlMfsr2_1) {
	LIB_FUNC("o+NM86gEwFE", Initialize);
	LIB_FUNC("1ic5q-kdOsc", GetSharedRequirements);
	LIB_FUNC("gMduXCLYrNg", CreateSharedResources);
	LIB_FUNC("0GAw7SmkwII", ReleaseSharedResources);
	LIB_FUNC("5z2gBlqxJ+0", GetContextRequirements);
	LIB_FUNC("m9JLPc3wOQw", CreateContext);
	LIB_FUNC("Kyy1baXgaVU", ReleaseContext);
	LIB_FUNC("ZLL31lzzxr4", GetDispatchDwords);
	LIB_FUNC("lrJwpLjKXRc", GetDispatchPackets);
}

} // namespace Mfsr2

LIB_DEFINE(InitPsml_1) {
	LIB_FUNC("3WVD91e12ZQ", Initialize);
	LIB_FUNC("+2KpvixvL6E", GetSharedRequirements);
	LIB_FUNC("eWoKNeB6V-k", CreateSharedResources);
	LIB_FUNC("jEevBXmagOQ", ReleaseSharedResources);
	LIB_FUNC("EO9YQXmJEN8", GetContextRequirements);
	LIB_FUNC("IMlj247LdTo", GetContextRequirements);
	LIB_FUNC("sjTyYheKTrU", GetContextRequirements);
	LIB_FUNC("ArakEpzsZo0", GetContextRequirements);
	LIB_FUNC("VGjrQa-WqdU", GetContextRequirements);
	LIB_FUNC("2ecEbQaf9VU", CreateFixedContext);
	LIB_FUNC("vUk2pWMx3KQ", CreatePrivateContext);
	LIB_FUNC("gxv3i+MTEzU", CreateSharedContext);
	LIB_FUNC("fccGInHrj8A", CreateSharedContext);
	LIB_FUNC("JaLBe0P3jSU", ReleaseContext);
	LIB_FUNC("AHalTX9wFZY", GetPacketSize);
	LIB_FUNC("RUNLFro+qok", LegacyDispatch);
	LIB_FUNC("94iBp3KvIuI", Dispatch);
	LIB_FUNC("GHna9-DvnUk", GetMipmapBias);
	LIB_FUNC("GJY0MvuTcs8", ValidateContext);
	LIB_FUNC("LXq+6mIxpCw", ValidateContext);
	LIB_FUNC("FSGaTQze0UY", ValidateContext);
	Mfsr2::InitPsmlMfsr2_1(s);
}

} // namespace Libs::LibPsml
