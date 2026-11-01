#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TILER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TILER_H_

#include "common/common.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/image/refreshRegions.h"
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
	// A window [lo, hi) of an image's tiled bytes, held in `buffer` from `offset` on.
	struct RegionSource {
		vk::Buffer buffer = nullptr;
		uint64_t   offset = 0;
		uint64_t   lo     = 0;
		uint64_t   hi     = 0;
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
	// Detiles only the given regions of an image into the full linear layout in `target`. Texels
	// outside the regions, and texels of a region whose tiled bytes lie outside the window, are
	// left as they are. The target is never cleared.
	void DetileRegions(const RegionSource& source, const ImageInfo& info,
	                   std::span<const RefreshRegion> regions, Result target);
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
	// Color-clear keys are multiples of 0x20, so key >> 5 selects one of these slots.
	static constexpr uint32_t ColorClearSlots = 8;
	// metadata spans `slices` equal runs `stride` bytes apart. Returns the buffer whose dword
	// slice * ColorClearSlots + (key >> 5) is set when every byte of that run equals a key whose
	// bit `keys` selects; a hit expands to 0xff when `expand` is set.
	[[nodiscard]] vk::Buffer CheckColorClear(Result metadata, uint64_t stride, uint32_t slices,
	                                         uint32_t keys, bool expand);

private:
	friend struct TileManagerTestAccess;

	static constexpr uint32_t FamilyCount          = static_cast<uint32_t>(TileBlockFamily::Count);
	static constexpr uint32_t BytesPerElementCount = 5;
	// Detile and tile run over whole mips. Detile regions run over rectangles of a mip.
	enum class Direction : uint32_t { Detile, Tile, DetileRegion };
	static constexpr uint32_t DirectionCount       = 3;
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
		uint32_t x0;
		uint32_t y0;
		uint32_t z0;
		uint32_t spare;
	};
	struct TilingParams {
		uint32_t                      num_texels      = 0;
		uint32_t                      color_transform = 0;
		uint32_t                      window_start    = 0;
		uint32_t                      window_len      = 0;
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
	// The part of a mip that one Mip record covers, in storage elements.
	struct MipWindow {
		uint32_t width;
		uint32_t height;
		uint32_t depth;
		uint32_t x0 = 0;
		uint32_t y0 = 0;
		uint32_t z0 = 0;
	};
	// What BuildMip needs besides the mip and its window.
	struct MipBuildContext {
		const ImageInfo& info;
		bool             tile;
		uint64_t         source_base;
		uint64_t         target_base;
		uint64_t         tiled_capacity;
		uint64_t         linear_capacity;
		// Region records bias their source by the window start, which can wrap in 32 bits.
		bool     modular_source = false;
		uint64_t window_lo      = 0;
	};
	static void ValidateImageForTiling(const ImageInfo& info);
	[[nodiscard]] static MipWindow FullMipWindow(const ImageInfo& info, uint32_t level);
	[[nodiscard]] static Mip       BuildMip(const MipBuildContext& context, uint32_t level,
	                                        const MipWindow& window, uint32_t used_texels);
	[[nodiscard]] static TilingParams
	BuildRegionParams(const ImageInfo& info, std::span<const RefreshRegion> regions,
	                  const RegionSource& source, uint64_t source_base, uint64_t target_base,
	                  uint64_t linear_capacity);
	[[nodiscard]] Tiling UploadParams(const TilingParams& params, uint32_t records,
	                                  Direction direction, const TileBlockLayout& block);
	void RecordBarriers(vk::CommandBuffer command, vk::Buffer source, uint64_t source_offset,
	                    uint64_t source_capacity, vk::Buffer target, uint64_t target_offset,
	                    uint64_t target_capacity, const Tiling& tiling, bool clear_target);
	void BindTilerDescriptors(vk::CommandBuffer command,
	                          const std::array<vk::DescriptorBufferInfo, 3>& infos);
	void DispatchTexels(vk::CommandBuffer command, uint32_t num_texels, vk::Pipeline pipeline,
	                    vk::Buffer target, uint64_t target_offset, uint64_t target_capacity);
	void                 Record(vk::Buffer source, uint64_t source_offset, uint64_t source_capacity,
	                            vk::Buffer target, uint64_t target_offset, uint64_t target_capacity,
	                            const Tiling& tiling, bool clear_target);
	[[nodiscard]] vk::Pipeline GetPipeline(uint32_t slot);
	[[nodiscard]] vk::Pipeline CreatePipeline(std::span<const uint32_t>     code,
	                                          const vk::SpecializationInfo* specialization) const;

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
	vk::Pipeline                            m_color_clear     = nullptr;
	std::unique_ptr<Buffer>                 m_clear_predicates;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TILER_H_
