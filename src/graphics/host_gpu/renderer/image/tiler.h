#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TILER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TILER_H_

#include "common/common.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <memory>
#include <span>

namespace Libs::Graphics {

class CommandScheduler;
class Buffer;
class Image;
class StreamBuffer;
struct GraphicContext;
struct TileManagerTestAccess;

class TileManager final {
public:
	enum class D16Direction { Promote, Demote };

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

	// Consume scratch results before the next acquisition, or pass their buffer as input.
	[[nodiscard]] Result Detile(vk::Buffer tiled, uint64_t tiled_offset, const ImageInfo& info);
	void Tile(vk::Buffer linear, uint64_t linear_offset, uint64_t linear_capacity, vk::Buffer tiled,
	          uint64_t tiled_offset, uint64_t tiled_capacity, const ImageInfo& info,
	          uint32_t levels = 0);
	void TileImage(Image& image, std::span<const vk::BufferImageCopy> regions, vk::Buffer tiled,
	               uint64_t tiled_offset, uint64_t tiled_capacity, uint32_t levels = 0);
	[[nodiscard]] Result GetScratchBuffer(uint64_t size, vk::Buffer input = nullptr);
	void                 ConvertD16(Result source, Result target, D16Direction direction, bool d32,
	                                const D16Layout& layout);
	[[nodiscard]] Result TransformColor(Result input, ColorTransform transform, bool to_host);
	void TransformColor(Result input, Result output, ColorTransform transform, bool to_host);

private:
	friend struct TileManagerTestAccess;

	static constexpr uint32_t FamilyCount          = static_cast<uint32_t>(TileBlockFamily::Count);
	static constexpr uint32_t BytesPerElementCount = 5;
	static constexpr uint32_t DirectionCount       = 2;
	static constexpr uint32_t PipelineCount = FamilyCount * BytesPerElementCount * DirectionCount;
	static constexpr uint32_t MaxMipLevels         = 16;

	struct alignas(16) Mip {
		uint32_t src_base;
		uint32_t dst_base;
		uint32_t width;
		uint32_t height;
		uint32_t pitch_bytes;
		uint32_t slice_bytes;
		uint32_t tiled_slice_bytes;
		uint32_t blocks_per_row;
		uint32_t surface_z;
		uint32_t tail_x;
		uint32_t tail_y;
		uint32_t num_texels;
	};
	struct TilingParams {
		uint32_t                      num_texels      = 0;
		uint32_t                      color_transform = 0;
		std::array<uint32_t, 2>        padding {};
		std::array<Mip, MaxMipLevels> mips;
	};
	struct Tiling {
		uint64_t params_offset;
		uint32_t num_texels;
		uint16_t params_size;
		uint16_t pipeline_slot;
	};
	struct Conversion {
		uint32_t src_base;
		uint32_t dst_base;
		uint32_t width;
		uint32_t height;
		uint32_t source_pitch;
		uint32_t target_pitch;
		uint32_t color_transform;
		uint32_t to_host;
	};
	struct StorageBinding {
		vk::DescriptorBufferInfo info;
		uint32_t                 base = 0;
	};

	[[nodiscard]] StorageBinding  BindStorage(Result buffer, uint64_t size) const;
	[[nodiscard]] static uint32_t ConversionRows(uint64_t offset, uint64_t row_stride,
	                                             uint64_t active, uint32_t remaining,
	                                             uint64_t alignment, uint64_t max_range,
	                                             uint32_t max_groups) noexcept;
	[[nodiscard]] Tiling Prepare(bool tile, uint64_t tiled_capacity, uint64_t linear_capacity,
	                             const ImageInfo& info, uint32_t levels, uint64_t source_base,
	                             uint64_t target_base);
	void                 Record(vk::Buffer source, uint64_t source_offset, uint64_t source_capacity,
	                            vk::Buffer target, uint64_t target_offset, uint64_t target_capacity,
	                            const Tiling& tiling, bool clear_target);
	[[nodiscard]] vk::Pipeline GetPipeline(uint32_t slot);

	GraphicContext&                         m_graphics;
	CommandScheduler&                       m_scheduler;
	StreamBuffer&                           m_stream_buffer;
	std::array<std::unique_ptr<Buffer>, 2>   m_scratch;
	vk::DescriptorSetLayout                 m_descriptor_layout = nullptr;
	vk::PipelineLayout                      m_pipeline_layout   = nullptr;
	std::array<vk::Pipeline, PipelineCount> m_pipelines {};
	vk::Pipeline                            m_d16_to_d24      = nullptr;
	vk::Pipeline                            m_d16_to_d32      = nullptr;
	vk::Pipeline                            m_d24_to_d16      = nullptr;
	vk::Pipeline                            m_d32_to_d16      = nullptr;
	vk::Pipeline                            m_color_transform = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TILER_H_
