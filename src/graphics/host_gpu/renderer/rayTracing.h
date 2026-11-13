#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RAYTRACING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RAYTRACING_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class BufferCache;
class CommandScheduler;
class RenderContext;
struct GraphicContext;

// Mirrors guest BVHs (header version 1) into host acceleration structures. The guest builds and
// refits them on the GPU, so a GPU job reads them there; the host reads back only what sizes the
// builds. A guest TLAS instance names a (BLAS, subtree root) pair, mirrored as one BLAS over the
// subtree's prim-record range.
class RayTracing {
public:
	explicit RayTracing(RenderContext& context);
	~RayTracing();
	KYTY_CLASS_NO_COPY(RayTracing);

	// Records the mirror of the guest TLAS header at `tlas`. Returns the device address of its
	// context {u32 valid, u32 guest instance count, u32, u64 TLAS address at +16, u64 guest
	// instance array at +24}, valid once every visible instance has a mirror of its pair as the
	// guest holds it at the dispatch. New or changed BVHs get mirrors a few calls later. Instance i
	// has custom index i and SBT offset equal to the first prim record of its range, so a hit's
	// record is that plus the primitive. The context and the mirrors serve the commands recorded
	// before the next call. The guest page table must be synchronized first (PrepareBda).
	[[nodiscard]] vk::DeviceAddress Prepare(uint64_t tlas);

private:
	static constexpr uint32_t Areas = 8;

	struct Structure {
		std::unique_ptr<Buffer>      buffer;
		vk::AccelerationStructureKHR handle         = nullptr;
		vk::DeviceAddress            address        = 0;
		vk::DeviceSize               build_scratch  = 0;
		vk::DeviceSize               update_scratch = 0;
		uint32_t                     capacity       = 0; // Primitives or instances.
	};

	struct Blas {
		uint64_t                address;
		std::unique_ptr<Buffer> triangles;
		uint32_t                capacity = 0; // Records `triangles` holds.
		uint32_t                begin    = 0; // Records and node count of the last readback.
		uint32_t                end      = 0;
		uint32_t                nodes    = 0;
		uint32_t                topology = 0; // Hashes of the last complete readback.
		uint32_t                geometry = 0;
		bool                    known    = false;
		bool                    dynamic  = false; // Its geometry changed: updated every call.
	};

	struct Pair {
		uint32_t                blas;
		uint32_t                root;
		Structure               structure;
		std::unique_ptr<Buffer> boxes; // Box nodes the last walk visited.
		uint32_t                box_capacity = 0;
		uint32_t                box_count    = 0;
		uint32_t                links        = 0; // Their link hash.
		// Range of the last walk, under BLAS topology `topology`.
		uint32_t count          = 0;
		uint32_t first          = 0;
		uint32_t topology       = 0;
		uint32_t geometry       = 0; // BLAS geometry at the last build or update.
		uint64_t update_serial  = 0;
		bool     built          = false; // The structure holds the walked range.
		bool     walked         = false;
		bool     walking        = false;
		bool     geometry_known = false;
	};

	struct Tlas {
		Structure               structure;
		std::unique_ptr<Buffer> instances;
		uint32_t                instance_slots = 64;
	};

	// A dispatch's readback.
	struct Area {
		std::unique_ptr<Buffer> buffer;
		uint64_t                tick           = 0;
		uint64_t                serial         = 0;
		uint64_t                tlas           = 0;
		uint32_t                instance_slots = 0;
		size_t                  pairs          = 0;
		std::vector<uint32_t>   walks;    // Pair per walk result.
		std::vector<uint32_t>   verified; // Pairs whose boxes were hashed again.
		std::vector<uint32_t>   covered;  // Records hashed per BLAS.
	};

	void Collect();
	void Apply(Area& area);
	void AddPair(uint64_t blas, uint32_t root);
	void Create(Structure& structure, const vk::AccelerationStructureGeometryKHR& geometry,
	            vk::AccelerationStructureTypeKHR type, vk::BuildAccelerationStructureFlagsKHR flags,
	            uint32_t capacity);
	void Reserve(std::unique_ptr<Buffer>& buffer, vk::BufferUsageFlags usage, uint64_t size,
	             MemoryUsage memory = MemoryUsage::DeviceLocal);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	BufferCache&                                      m_buffer_cache;
	RenderContext&                                    m_context;
	vk::DescriptorSetLayout                           m_descriptor_layout = nullptr;
	vk::PipelineLayout                                m_layout            = nullptr;
	vk::Pipeline                                      m_walk_pipeline     = nullptr;
	vk::Pipeline                                      m_pipeline          = nullptr;
	uint64_t                                          m_scratch_alignment = 0;
	uint64_t                                          m_max_instances     = 0;
	std::vector<Blas>                                 m_blases;
	std::map<uint64_t, uint32_t>                      m_blas_index;
	std::vector<Pair>                                 m_pairs;
	std::map<std::pair<uint64_t, uint32_t>, uint32_t> m_pair_index;
	std::vector<std::array<uint32_t, 4>>              m_keys; // {BLAS low, high, root, pair}.
	std::map<uint64_t, Tlas>                          m_tlases;
	std::unique_ptr<Buffer>                           m_state;
	std::unique_ptr<Buffer>                           m_scratch;
	std::array<Area, Areas>                           m_areas;
	uint32_t                                          m_next_area = 0;
	uint64_t                                          m_serial    = 0;
	uint64_t                                          m_applied = 0; // Serial of the last readback.
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RAYTRACING_H_
