#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHINDIRECT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHINDIRECT_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/dispatchGuard.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shader.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <span>

namespace Libs::Graphics {

class RenderContext;

[[nodiscard]] inline MeshDrawShape
MeshShapeOf(const ShaderMeshInputInfo&                       mesh,
            const vk::PhysicalDeviceMeshShaderPropertiesEXT& limits) {
	return {.primitive_size       = mesh.InputPrimitiveSize(),
	        .primitive_step       = mesh.InputPrimitiveStep(),
	        .primitives_per_group = mesh.primitives_per_group,
	        .max_groups = {limits.maxMeshWorkGroupCount[0], limits.maxMeshWorkGroupCount[1],
	                       limits.maxMeshWorkGroupCount[2]},
	        .max_total  = limits.maxMeshWorkGroupTotalCount};
}

struct MeshIndirectVerifyPacket {
	MeshDrawShape shape;
	uint32_t      max_count          = 0;
	bool          counted            = false;
	bool          indexed            = false;
	uint32_t      index_element_size = 0;
	uint64_t      index_base         = 0;
	// INDEX_BUFFER_SIZE, in elements.
	uint32_t index_buffer_size = 0;
};

// What gpu_mesh_indirect.task reads through `records` when its packet is indexed or verified.
struct MeshIndirectHeader {
	vk::DeviceAddress records            = 0;
	vk::DeviceAddress count              = 0;
	uint32_t          max_count          = 0;
	uint32_t          stride             = 0;
	uint32_t          entries            = 0;
	uint32_t          index_element_size = 0;
	uint64_t          index_base         = 0;
	uint32_t          index_buffer_size  = 0;
	uint32_t          reserved           = 0;
};

inline constexpr uint32_t MeshIndirectVerifyHeaderDwords = sizeof(MeshIndirectHeader) / 4;
inline constexpr uint32_t MeshIndirectVerifyEntryDwords  = 20;
static_assert(sizeof(MeshIndirectHeader) == 48);

[[nodiscard]] inline MeshIndirectHeader MeshIndirectHeaderOf(const MeshIndirectVerifyPacket& packet,
                                                             vk::DeviceAddress records,
                                                             vk::DeviceAddress count,
                                                             uint32_t stride, uint32_t entries) {
	return {.records            = records,
	        .count              = count,
	        .max_count          = packet.max_count,
	        .stride             = stride,
	        .entries            = entries,
	        .index_element_size = packet.indexed ? packet.index_element_size : 0u,
	        .index_base         = packet.indexed ? packet.index_base : 0u,
	        .index_buffer_size  = packet.indexed ? packet.index_buffer_size : 0u};
}

// Whether one task workgroup's snapshot of what it read and launched matches the CPU replay.
[[nodiscard]] bool MeshIndirectEntryAgrees(const MeshIndirectVerifyPacket& packet, uint32_t index,
                                           std::span<const uint32_t> entry, bool& empty);

class MeshIndirectVerifier {
public:
	struct Totals {
		uint64_t checked    = 0;
		uint64_t mismatched = 0;
		uint64_t unverified = 0;
		uint64_t empty      = 0;
	};

	explicit MeshIndirectVerifier(RenderContext& context);
	~MeshIndirectVerifier();
	KYTY_CLASS_NO_COPY(MeshIndirectVerifier);

	// 0 when the arena is full; a returned header is committed once its draw is recorded.
	[[nodiscard]] vk::DeviceAddress Begin(const MeshIndirectVerifyPacket& packet,
	                                      vk::DeviceAddress records, vk::DeviceAddress count,
	                                      uint32_t stride, uint64_t args_addr);
	void                            Commit();
	[[nodiscard]] Totals            GetTotals() const;

private:
	struct Block {
		MeshIndirectVerifyPacket packet;
		uint64_t                 args_addr = 0;
		uint32_t                 offset    = 0;
		uint32_t                 dwords    = 0;
		uint32_t                 entries   = 0;
	};

	static constexpr uint32_t ArenaDwords = 1u << 21u;

	void Check();
	void Report(const char* what, const Block& block, uint32_t index,
	            std::span<const uint32_t> entry) const;

	RenderContext&     m_context;
	Buffer             m_arena;
	mutable std::mutex m_mutex;
	std::deque<Block>  m_blocks;
	uint32_t           m_head = 0;
	Totals             m_totals;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHINDIRECT_H_
