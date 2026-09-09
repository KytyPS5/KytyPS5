#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TILER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TILER_H_

#include "common/common.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <span>
#include <vector>
#include <vk_mem_alloc.h>

namespace Libs::Graphics {

class CommandScheduler;
class Image;
class StreamBuffer;
struct GraphicContext;
struct TileManagerTestAccess;

struct GpuTileInfo {
	TileBlockFamily family              = TileBlockFamily::Count;
	uint32_t        bytes_per_element   = 0;
	uint64_t        linear_offset       = 0;
	uint64_t        linear_size         = 0;
	uint64_t        tiled_offset        = 0;
	uint64_t        tiled_size          = 0;
	uint64_t        linear_slice_stride = 0;
	uint32_t        width               = 0;
	uint32_t        height              = 0;
	uint32_t        depth               = 1;
	uint32_t        pitch               = 0;
	uint32_t        tail_x              = 0;
	uint32_t        tail_y              = 0;
	bool            tail                = false;
	uint32_t        tiled_width         = 0;
	uint32_t        tiled_height        = 0;
	uint32_t        surface_z           = 0;
};

class TileManager final {
public:
	enum class D16Direction { Promote, Demote };
	enum class ColorTransform { None, SwapBgra16 };

	struct Result {
		vk::Buffer buffer = nullptr;
		uint64_t   offset = 0;
		uint64_t   size   = 0;
	};
	struct D16Layout {
		uint32_t width               = 0;
		uint32_t height              = 0;
		uint32_t layers              = 0;
		uint64_t source_row_stride   = 0;
		uint64_t target_row_stride   = 0;
		uint64_t source_slice_stride = 0;
		uint64_t target_slice_stride = 0;
	};

	TileManager(GraphicContext& graphics, CommandScheduler& scheduler, StreamBuffer& stream_buffer);
	~TileManager();
	KYTY_CLASS_NO_COPY(TileManager);

	// The returned device-local buffer remains alive through the current scheduler tick.
	[[nodiscard]] Result Detile(vk::Buffer tiled, uint64_t tiled_offset, uint64_t tiled_capacity,
	                            uint64_t linear_capacity, std::span<const GpuTileInfo> infos);
	void Tile(vk::Buffer linear, uint64_t linear_offset, uint64_t linear_capacity, vk::Buffer tiled,
	          uint64_t tiled_offset, uint64_t tiled_capacity, std::span<const GpuTileInfo> infos);
	void TileImage(Image& image, std::span<const vk::BufferImageCopy> regions, vk::Buffer tiled,
	               uint64_t tiled_offset, uint64_t tiled_capacity, uint64_t linear_capacity,
	               std::span<const GpuTileInfo> infos,
	               ColorTransform               transform = ColorTransform::None);
	[[nodiscard]] Result GetScratchBuffer(uint64_t size);
	void                 ConvertD16(Result source, Result target, D16Direction direction, bool d32,
	                                const D16Layout& layout);
	[[nodiscard]] Result SwapBgra16(Result input);
	void                 SwapBgra16(Result input, Result output);

private:
	friend struct TileManagerTestAccess;

	static constexpr uint32_t FamilyCount          = static_cast<uint32_t>(TileBlockFamily::Count);
	static constexpr uint32_t BytesPerElementCount = 5;
	static constexpr uint32_t DirectionCount       = 2;
	static constexpr uint32_t PipelineCount = FamilyCount * BytesPerElementCount * DirectionCount;

	struct Push {
		uint32_t src_base;
		uint32_t dst_base;
		uint32_t width;
		uint32_t height;
		uint32_t depth;
		uint32_t surface_z;
		uint32_t pitch_bytes;
		uint32_t slice_bytes;
		uint32_t blocks_per_row;
		uint32_t blocks_per_slice;
		uint32_t tail_x;
		uint32_t tail_y;
		uint32_t tail;
	};
	struct Dispatch {
		Push     push {};
		uint32_t pipeline_slot = 0;
		uint64_t params_offset = 0;
	};
	struct Scratch {
		vk::Buffer    buffer     = nullptr;
		VmaAllocation allocation = nullptr;
		uint64_t      size       = 0;
	};
	struct StorageBinding {
		vk::DescriptorBufferInfo info;
		uint32_t                 base = 0;
	};

	[[nodiscard]] Scratch         AllocateScratch(uint64_t size);
	[[nodiscard]] StorageBinding  BindStorage(Result buffer, uint64_t size) const;
	[[nodiscard]] static uint32_t ConversionRows(uint64_t offset, uint64_t row_stride,
	                                             uint64_t active, uint32_t remaining,
	                                             uint64_t alignment, uint64_t max_range,
	                                             uint32_t max_groups) noexcept;
	void                          DeferDestroy(Scratch scratch);
	void Prepare(bool tile, uint64_t tiled_capacity, uint64_t linear_capacity,
	             std::span<const GpuTileInfo> infos, uint64_t source_base, uint64_t target_base,
	             std::vector<Dispatch>& dispatches);
	void Record(bool tile, vk::Buffer source, uint64_t source_offset, uint64_t source_capacity,
	            vk::Buffer target, uint64_t target_offset, uint64_t target_capacity,
	            std::span<Dispatch> dispatches, bool clear_target);
	[[nodiscard]] vk::Pipeline GetPipeline(uint32_t slot);
	void                       SwapBgra16(Result input, Result output, uint32_t pixels);

	GraphicContext&                         m_graphics;
	CommandScheduler&                       m_scheduler;
	StreamBuffer&                           m_stream_buffer;
	vk::DescriptorSetLayout                 m_descriptor_layout = nullptr;
	vk::PipelineLayout                      m_pipeline_layout   = nullptr;
	std::array<vk::Pipeline, PipelineCount> m_pipelines {};
	vk::Pipeline                            m_d16_to_d24  = nullptr;
	vk::Pipeline                            m_d16_to_d32  = nullptr;
	vk::Pipeline                            m_d24_to_d16  = nullptr;
	vk::Pipeline                            m_d32_to_d16  = nullptr;
	vk::Pipeline                            m_swap_bgra16 = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TILER_H_
