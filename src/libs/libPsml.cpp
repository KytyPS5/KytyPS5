#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <atomic>
#include <cstdint>

namespace Libs {

namespace LibPsml {

LIB_VERSION("Psml", 1, "Psml", 1, 1);

namespace Psml {

constexpr int Error(uint32_t value) {
	return static_cast<int>(value);
}

constexpr int PSML_ERROR_NOT_INITIALIZED = Error(0x8a810001u);
constexpr int PSML_ERROR_INVALID_OBJECT  = Error(0x8a810005u);
constexpr int PSML_ERROR_INVALID_POINTER = Error(0x8a810009u);
constexpr int PSML_ERROR_INVALID_VALUE   = Error(0x8a81000du);
constexpr int PSML_ERROR_NULL_OBJECT     = Error(0x8a810014u);

constexpr uint32_t SHARED_RESOURCES_MAGIC = 0xa9c4;
constexpr uint32_t CONTEXT_MAGIC          = 0x9231;

constexpr uint64_t MAIN_MEMORY_BLOCK_SIZE = 0x200000;
constexpr uint64_t MAIN_MEMORY_ALIGNMENT  = 0x200000;
constexpr uint64_t EXTRA_VA_BYTES         = 0x600000;

struct MainMemoryRequirements {
	uint64_t block_size;
	uint64_t alignment;
	uint64_t block_count;
};

struct MainMemoryParameters {
	uint32_t type;
	uint32_t reserved;
};

struct DirectMemoryBlock {
	uint64_t address;
	uint64_t size;
};

struct SharedResourcesInitParameters {
	uint32_t                 type;
	uint32_t                 reserved;
	const DirectMemoryBlock* blocks;
	uint64_t                 block_count;
	uint64_t                 virtual_address_start;
};

static_assert(sizeof(MainMemoryRequirements) == 24);
static_assert(sizeof(DirectMemoryBlock) == 16);

static bool                 g_initialized      = false;
static uint32_t             g_supported_modes  = 0x3;
static std::atomic<int32_t> g_shared_ref_count = 0;

static uint64_t BaseVaBytes(uint32_t type) {
	// The native implementation indexes this table before adding one 6 MiB range.
	switch (type) {
		case 0: return 0x06200000;
		case 1: return 0x18200000;
		case 2: return 0x12200000;
		default: return 0;
	}
}

static bool IsSupportedType(uint32_t type) {
	switch (type) {
		case 0: return true;
		case 1: return (g_supported_modes & 0x1u) != 0;
		case 2: return (g_supported_modes & 0x2u) != 0;
		default: return false;
	}
}

static uint64_t RequiredBlockCount(uint32_t type) {
	const auto base = BaseVaBytes(type);
	if (base == 0) {
		return 0;
	}
	return (base + EXTRA_VA_BYTES) / MAIN_MEMORY_BLOCK_SIZE;
}

static int CheckInitialized() {
	return (g_initialized ? OK : PSML_ERROR_NOT_INITIALIZED);
}

static void MarkSharedResources(void* resources, const SharedResourcesInitParameters& params) {
	auto* bytes = static_cast<uint8_t*>(resources);

	*reinterpret_cast<uint32_t*>(bytes)                        = SHARED_RESOURCES_MAGIC;
	*reinterpret_cast<const DirectMemoryBlock**>(bytes + 0x08) = params.blocks;
	*reinterpret_cast<uint64_t*>(bytes + 0x18)                 = params.block_count;
	*reinterpret_cast<uint32_t*>(bytes + 0x20)                 = params.type;
	*reinterpret_cast<uint64_t*>(bytes + 0x28)                 = params.virtual_address_start;
}

static bool HasKnownObjectMagic(const void* object) {
	if (object == nullptr) {
		return false;
	}
	const auto magic = *static_cast<const uint32_t*>(object);
	return magic == SHARED_RESOURCES_MAGIC || magic == CONTEXT_MAGIC;
}

static int KYTY_SYSV_ABI PsmlInitialize() {
	PRINT_NAME();

	g_initialized      = true;
	g_supported_modes  = 0x3;
	g_shared_ref_count = 0;

	return OK;
}

static int KYTY_SYSV_ABI PsmlGetMainMemoryRequirements(MainMemoryRequirements*     out,
                                                       const MainMemoryParameters* params) {
	PRINT_NAME();

	const auto init_error = CheckInitialized();
	if (init_error != OK) {
		return init_error;
	}
	if (out == nullptr || params == nullptr) {
		return PSML_ERROR_INVALID_POINTER;
	}
	if (!IsSupportedType(params->type)) {
		return PSML_ERROR_INVALID_VALUE;
	}

	out->block_size  = MAIN_MEMORY_BLOCK_SIZE;
	out->alignment   = MAIN_MEMORY_ALIGNMENT;
	out->block_count = RequiredBlockCount(params->type);

	return OK;
}

static int KYTY_SYSV_ABI
PsmlSharedResourcesInitialize(void* resources, const SharedResourcesInitParameters* params) {
	PRINT_NAME();

	const auto init_error = CheckInitialized();
	if (init_error != OK) {
		return init_error;
	}
	if (resources == nullptr || params == nullptr || params->blocks == nullptr) {
		return PSML_ERROR_INVALID_POINTER;
	}
	if (params->reserved != 0 || !IsSupportedType(params->type) ||
	    params->block_count < RequiredBlockCount(params->type)) {
		return PSML_ERROR_INVALID_VALUE;
	}

	MarkSharedResources(resources, *params);
	g_shared_ref_count.fetch_add(1, std::memory_order_relaxed);

	return OK;
}

static int KYTY_SYSV_ABI PsmlSharedResourcesFinalize(void* resources) {
	PRINT_NAME();

	const auto init_error = CheckInitialized();
	if (init_error != OK) {
		return init_error;
	}
	if (resources == nullptr) {
		return PSML_ERROR_NULL_OBJECT;
	}

	g_shared_ref_count.fetch_sub(1, std::memory_order_relaxed);

	return OK;
}

static int KYTY_SYSV_ABI PsmlGetContextMemoryRequirements(MainMemoryRequirements* out,
                                                          uint64_t /*params*/) {
	PRINT_NAME();

	const auto init_error = CheckInitialized();
	if (init_error != OK) {
		return init_error;
	}
	if (out == nullptr) {
		return PSML_ERROR_INVALID_POINTER;
	}

	out->block_size  = MAIN_MEMORY_BLOCK_SIZE;
	out->alignment   = MAIN_MEMORY_ALIGNMENT;
	out->block_count = 1;

	return OK;
}

static int KYTY_SYSV_ABI PsmlContextInitialize(void* context, const void* params) {
	PRINT_NAME();

	const auto init_error = CheckInitialized();
	if (init_error != OK) {
		return init_error;
	}
	if (context == nullptr || params == nullptr) {
		return PSML_ERROR_INVALID_POINTER;
	}

	auto* bytes            = static_cast<uint8_t*>(context);
	auto* params_bytes     = static_cast<const uint8_t*>(params);
	auto* shared_resources = *reinterpret_cast<void* const*>(params_bytes + 0x08);
	if (shared_resources == nullptr) {
		return PSML_ERROR_INVALID_POINTER;
	}

	*reinterpret_cast<uint32_t*>(bytes)        = CONTEXT_MAGIC;
	*reinterpret_cast<void**>(bytes + 0x360)   = shared_resources;
	*reinterpret_cast<uint8_t*>(bytes + 0x368) = 0;

	return OK;
}

static int KYTY_SYSV_ABI PsmlContextFinalize(void* context) {
	PRINT_NAME();

	const auto init_error = CheckInitialized();
	if (init_error != OK) {
		return init_error;
	}
	if (!HasKnownObjectMagic(context)) {
		return PSML_ERROR_INVALID_OBJECT;
	}

	*static_cast<uint32_t*>(context) = 0;

	return OK;
}

static int KYTY_SYSV_ABI PsmlGetWorkAreaSize(const void* object, uint32_t* out_size) {
	PRINT_NAME();

	const auto init_error = CheckInitialized();
	if (init_error != OK) {
		return init_error;
	}
	if (!HasKnownObjectMagic(object)) {
		return PSML_ERROR_INVALID_OBJECT;
	}
	if (out_size == nullptr) {
		return PSML_ERROR_INVALID_POINTER;
	}

	*out_size = 0x600;

	return OK;
}

static int KYTY_SYSV_ABI PsmlDispatch(void* context, uint64_t command, uint64_t params) {
	PRINT_NAME();

	const auto init_error = CheckInitialized();
	if (init_error != OK) {
		return init_error;
	}
	if (!HasKnownObjectMagic(context)) {
		return PSML_ERROR_INVALID_OBJECT;
	}
	if (command == 0 || params == 0) {
		return PSML_ERROR_INVALID_POINTER;
	}

	return OK;
}

static int KYTY_SYSV_ABI PsmlGetProgress(const void* object, float* out_progress,
                                         uint32_t /*frame_index*/) {
	PRINT_NAME();

	const auto init_error = CheckInitialized();
	if (init_error != OK) {
		return init_error;
	}
	if (!HasKnownObjectMagic(object)) {
		return PSML_ERROR_INVALID_OBJECT;
	}
	if (out_progress == nullptr) {
		return PSML_ERROR_INVALID_POINTER;
	}

	*out_progress = 0.0f;

	return OK;
}

static int KYTY_SYSV_ABI PsmlRequestCapture(const void* object) {
	PRINT_NAME();

	const auto init_error = CheckInitialized();
	if (init_error != OK) {
		return init_error;
	}
	if (!HasKnownObjectMagic(object)) {
		return PSML_ERROR_INVALID_OBJECT;
	}

	return OK;
}

static int KYTY_SYSV_ABI PsmlValidateObject(const void* object) {
	PRINT_NAME();

	const auto init_error = CheckInitialized();
	if (init_error != OK) {
		return init_error;
	}
	if (!HasKnownObjectMagic(object)) {
		return PSML_ERROR_INVALID_OBJECT;
	}

	return OK;
}

} // namespace Psml

LIB_DEFINE(InitPsml_1) {
	LIB_FUNC("3WVD91e12ZQ", Psml::PsmlInitialize);
	LIB_FUNC("+2KpvixvL6E", Psml::PsmlGetMainMemoryRequirements);
	LIB_FUNC("eWoKNeB6V-k", Psml::PsmlSharedResourcesInitialize);
	LIB_FUNC("jEevBXmagOQ", Psml::PsmlSharedResourcesFinalize);
	LIB_FUNC("EO9YQXmJEN8", Psml::PsmlGetContextMemoryRequirements);
	LIB_FUNC("IMlj247LdTo", Psml::PsmlGetContextMemoryRequirements);
	LIB_FUNC("sjTyYheKTrU", Psml::PsmlGetContextMemoryRequirements);
	LIB_FUNC("ArakEpzsZo0", Psml::PsmlGetContextMemoryRequirements);
	LIB_FUNC("VGjrQa-WqdU", Psml::PsmlGetContextMemoryRequirements);
	LIB_FUNC("2ecEbQaf9VU", Psml::PsmlContextInitialize);
	LIB_FUNC("vUk2pWMx3KQ", Psml::PsmlContextInitialize);
	LIB_FUNC("gxv3i+MTEzU", Psml::PsmlContextInitialize);
	LIB_FUNC("JaLBe0P3jSU", Psml::PsmlContextFinalize);
	LIB_FUNC("AHalTX9wFZY", Psml::PsmlGetWorkAreaSize);
	LIB_FUNC("RUNLFro+qok", Psml::PsmlDispatch);
	LIB_FUNC("GHna9-DvnUk", Psml::PsmlGetProgress);
	LIB_FUNC("GJY0MvuTcs8", Psml::PsmlRequestCapture);
	LIB_FUNC("LXq+6mIxpCw", Psml::PsmlValidateObject);
	LIB_FUNC("FSGaTQze0UY", Psml::PsmlValidateObject);
}

} // namespace LibPsml

} // namespace Libs
