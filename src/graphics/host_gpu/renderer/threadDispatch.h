#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_THREADDISPATCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_THREADDISPATCH_H_

#include "common/abi.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <cstdint>

namespace Libs::Graphics {

class RenderContext;

class ThreadDispatcher {
public:
	struct Record {
		vk::Buffer     buffer;
		vk::DeviceSize groups_offset = 0;
		uint64_t       limit_address = 0;
	};

	explicit ThreadDispatcher(RenderContext& context);
	~ThreadDispatcher();
	KYTY_CLASS_NO_COPY(ThreadDispatcher);

	[[nodiscard]] Record Write(vk::CommandBuffer command, std::array<uint32_t, 3> threads);
	[[nodiscard]] Record Convert(vk::CommandBuffer command, vk::Buffer source,
	                             vk::DeviceSize source_offset, std::array<uint32_t, 3> local_size,
	                             std::array<uint32_t, 3> max_groups);

private:
	static constexpr uint32_t RecordBytes = 32;
	static constexpr uint32_t RecordCount = 4096;

	[[nodiscard]] Record Next();

	RenderContext&          m_context;
	Buffer                  m_records;
	uint32_t                m_next            = 0;
	vk::DescriptorSetLayout m_set_layout      = nullptr;
	vk::PipelineLayout      m_pipeline_layout = nullptr;
	vk::Pipeline            m_pipeline        = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_THREADDISPATCH_H_
