#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_PAGEDIFF_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_PAGEDIFF_H_

#include "common/abi.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Libs::Graphics {

// Finds the tracker pages of a buffer range that GPU work changed: the range is copied aside
// before the work is recorded and compared with its contents after it, page by page.
class PageDiff {
public:
	static constexpr uint64_t PageSize = TRACKER_PAGE_SIZE;
	// Larger ranges are not compared: their owners keep whole-range GPU ownership. A range of
	// this size can still straddle one more tracker page.
	static constexpr uint64_t MaxRangeBytes = 256ull << 20u;
	static constexpr uint64_t MaxPages      = MaxRangeBytes / PageSize + 1;
	static constexpr uint32_t MaxPending    = 128;

	struct Ticket {
		vk::Buffer source;
		uint64_t   source_offset = 0;
		vk::Buffer copy;
		uint64_t   copy_offset = 0;
		uint64_t   pages       = 0;
		uint32_t   slot        = UINT32_MAX;
	};

	PageDiff(GraphicContext& graphics, CommandScheduler& scheduler);
	~PageDiff();
	KYTY_CLASS_NO_COPY(PageDiff);

	// Records a copy of `pages` tracker pages of `source` from `offset`. Returns nothing when the
	// range is too large or every result slot is in use.
	[[nodiscard]] std::optional<Ticket> Snapshot(const Buffer& source, uint64_t offset,
	                                             uint64_t pages);
	// Records the comparison of the range with its copy. The result is ready once the work
	// recorded so far completes.
	void Compare(const Ticket& ticket);
	// Ends a batch of snapshots and comparisons: later snapshots may reuse the copy memory.
	void EndBatch();
	// One bit per page of the ticket, set when the page changed. Valid after the comparison
	// completed; until then every bit is set.
	[[nodiscard]] std::span<const uint32_t> Result(const Ticket& ticket);
	void                                    Free(const Ticket& ticket);
	[[nodiscard]] bool                      HasFreeSlot() const noexcept;

private:
	static constexpr uint64_t SlotWords = (MaxPages + 31u) / 32u;
	// Result slots are bound at their offset: keep them at the largest storage-buffer offset
	// alignment Vulkan allows.
	static constexpr uint64_t SlotBytes = (SlotWords * sizeof(uint32_t) + 255u) / 256u * 256u;

	GraphicContext&                      m_graphics;
	CommandScheduler&                    m_scheduler;
	Buffer                               m_results;
	std::array<bool, MaxPending>         m_slot_used {};
	std::unique_ptr<Buffer>              m_copies;
	std::vector<std::unique_ptr<Buffer>> m_retired;
	uint64_t                             m_copy_used         = 0;
	vk::DescriptorSetLayout              m_descriptor_layout = nullptr;
	vk::PipelineLayout                   m_pipeline_layout   = nullptr;
	vk::Pipeline                         m_pipeline          = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_PAGEDIFF_H_
