#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDRAW_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDRAW_H_

#include "common/abi.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <cstdint>
#include <optional>

namespace Libs::Graphics {

// Turns guest indirect draw arguments into host draw commands on the GPU, so that arguments
// written by earlier GPU work are never read back by the CPU.
class IndirectDrawPrepare {
public:
	static constexpr uint32_t IndexedCommandDwords = 5;
	static constexpr uint32_t AutoCommandDwords    = 4;

	struct Request {
		const Buffer* arguments        = nullptr;
		uint64_t      arguments_offset = 0;
		uint64_t      arguments_size   = 0;
		// Null when the packet carries the draw count itself.
		const Buffer* count        = nullptr;
		uint64_t      count_offset = 0;
		uint32_t      max_count    = 0;
		uint32_t      stride       = 0;
		bool          indexed      = false;
		// Keep the base vertex and first instance: rewritten vertex fetches consume them.
		bool keep_offsets = false;
		// NUM_INSTANCES known by the CPU, kept on the GPU when no draw runs.
		std::optional<uint32_t> instances;
	};

	struct Commands {
		vk::Buffer buffer          = nullptr;
		uint64_t   count_offset    = 0;
		uint64_t   commands_offset = 0;
	};

	IndirectDrawPrepare(GraphicContext& graphics, CommandScheduler& scheduler);
	~IndirectDrawPrepare();
	KYTY_CLASS_NO_COPY(IndirectDrawPrepare);

	// False when the request does not fit the command ring.
	[[nodiscard]] bool     Fits(const Request& request) const;
	[[nodiscard]] Commands Record(vk::CommandBuffer command, const Request& request);
	// Waits for the recorded draws, then returns the NUM_INSTANCES they leave.
	[[nodiscard]] uint32_t ReadInstances();

private:
	GraphicContext&         m_graphics;
	CommandScheduler&       m_scheduler;
	Buffer                  m_commands;
	Buffer                  m_states;
	uint64_t                m_cursor          = 0;
	uint64_t                m_state_tick      = 0;
	vk::DescriptorSetLayout m_set_layout      = nullptr;
	vk::PipelineLayout      m_pipeline_layout = nullptr;
	vk::Pipeline            m_pipeline        = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDRAW_H_
