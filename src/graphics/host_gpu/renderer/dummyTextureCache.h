#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DUMMYTEXTURECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DUMMYTEXTURECACHE_H_

#include "common/abi.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"

#include <array>
#include <cstdint>

namespace Libs::Graphics {

class DummyTextureCache final {
public:
	enum class Usage : uint8_t { Sampled, Storage };

	DummyTextureCache() = default;
	~DummyTextureCache();
	KYTY_CLASS_NO_COPY(DummyTextureCache);

	[[nodiscard]] VulkanImage* Get(GraphicContext* ctx, Usage usage, bool uint_format,
	                               bool image_3d);

private:
	struct Slot {
		GpuTextureVulkanImage* image = nullptr;
	};

	Common::Mutex       m_mutex;
	std::array<Slot, 4> m_sampled {};
	std::array<Slot, 4> m_storage {};
	GraphicContext*     m_ctx = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DUMMYTEXTURECACHE_H_
