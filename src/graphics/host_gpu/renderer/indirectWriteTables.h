#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTWRITETABLES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTWRITETABLES_H_

#include "common/abi.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/shader/shaderBindings.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

class RenderContext;

// Compute shaders may store through V#s they read from a table at an index only the GPU knows
// (IR::BufferResource::indirect_write_table). Such a store writes guest memory by device address,
// which reaches only memory that a cached buffer holds, so every target the table names must be
// cached before the dispatch. The tables are large (GTA V's is its 2 MiB descriptor heap) and
// rarely change, so each is kept as a snapshot that only the pages the CPU wrote since refresh.
// Which pages the stores really wrote comes back from the shaders' write bitmap once they have
// completed; only those bytes become GPU modified.
class IndirectWriteTables {
public:
	explicit IndirectWriteTables(RenderContext& context): m_context(context) {}
	KYTY_CLASS_NO_COPY(IndirectWriteTables);

	// Caches every target of the table of V#s at [address, address + size) that a store can
	// write. Returns whether the table names any.
	bool Prepare(uint64_t address, uint64_t size);
	// Whether a table was prepared since the last call.
	[[nodiscard]] bool TakePendingWrites() noexcept {
		const bool pending = m_pending_writes;
		m_pending_writes   = false;
		return pending;
	}
	// Records the caching pages that V#-table stores wrote.
	void NoteWrites(std::span<const uint64_t> pages);

	// The guest range a store through `target` can write, or an empty range when the shader drops
	// every store through it: a T#, S# or stale table entry, or a buffer without a data format.
	[[nodiscard]] static GuestRange TargetRange(const ShaderBufferResource& target);
	// Coalesces ranges that share or touch a BufferCache caching page into their page-aligned
	// union, sorted by address: the buffers that cover them.
	[[nodiscard]] static std::vector<GuestRange> CoalesceTargets(std::vector<GuestRange> ranges);

private:
	// A game binds few tables; the least recently prepared one beyond this is forgotten.
	static constexpr size_t MaxTables = 8;

	struct Table {
		uint64_t                          address = 0;
		uint64_t                          size    = 0;
		std::vector<ShaderBufferResource> entries;
		// Targets skipped because their memory was not mapped, revisited when the mappings change.
		std::vector<GuestRange> unmapped;
		uint64_t                mapped_version = 0;
		uint64_t                target_count   = 0;
		uint64_t                last_use       = 0;
	};

	void Scan(Table& table, uint64_t first, uint64_t last, bool changed_only,
	          std::vector<GuestRange>& targets);
	void Cover(Table& table, std::vector<GuestRange>& targets);

	RenderContext&     m_context;
	std::vector<Table> m_tables;
	// Every target prepared so far: a store can only have written these bytes.
	RangeSet m_targets;
	uint64_t m_use_count      = 0;
	bool     m_pending_writes = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTWRITETABLES_H_
