#include "graphics/host_gpu/renderer/nativeRayTracing.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "gpu_tiler_shaders/rt_blas_walk_spv.h"
#include "gpu_tiler_shaders/rt_tlas_walk_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace Libs::Graphics {

namespace {

// QueueCapacity and MissingCapacity are kQueueCapacity and kMissingCapacity of rt_tlas_walk.comp.
constexpr uint32_t QueueCapacity    = 1u << 16u;
constexpr uint32_t InstanceCapacity = 1u << 16u;
constexpr uint32_t TableEntries     = 1u << 17u;
constexpr uint32_t SlotCapacity     = 1u << 15u;
constexpr uint32_t MissingCapacity  = 256;
constexpr uint32_t MaxJobs          = 256;
constexpr uint32_t ChangedCapacity  = 1024;
constexpr uint32_t PoolCapacity     = 1u << 23u;
// The largest queue one walk takes from the pool (kMaxQueue in rt_blas_walk.comp).
constexpr uint64_t PoolQueueMax     = uint64_t {1} << 20u;
constexpr uint64_t BuildScratchSize = uint64_t {64} << 20u;
constexpr uint32_t MaxRetries       = 8;
// Preparations between the reports of a BLAS that failed MaxRetries times (about 3 seconds).
constexpr uint64_t LateRetryPrepares = 64;
// Preparations a changed slot stays rebuilt before each use (about 30 frames).
constexpr uint32_t DynamicPrepares  = 120;
// A slot no TLAS referenced for this many frames is evicted (its BLAS left the scene or its
// memory was unmapped); one that comes back is counted and built again as a new BLAS, so objects
// the camera turns away from for a while keep their shadows when it turns back.
constexpr uint64_t EvictFrames      = 1800;
// Eviction scans run every this many preparations and evict at most MaxEvictions slots each
// (every one costs GPU updates in the stream).
constexpr uint64_t EvictScanPrepares = 8;
constexpr uint32_t MaxEvictions     = 256;
// The most triangles one slot holds (both geometries) and bytes its memory takes. The whole scene
// of GTA V holds under two million triangles in about 200 MiB; a BLAS counted bigger was read
// while the guest was rewriting it (a count of tens of millions once reserved 8 GB of VRAM).
constexpr uint64_t MaxSlotTriangles = uint64_t {1} << 21u;
constexpr uint64_t MaxSlotBytes     = uint64_t {256} << 20u;
// The slots used least recently are evicted when the arena passes 7/8 of its budget, down to
// 3/4 of it. A slot used in the last LruMinAge frames is never evicted that way.
constexpr uint64_t LruMinAge        = 60;
// Moving slots out of a nearly empty chunk: when the arena reserves more than DefragMinWaste
// (and half of what it uses) over what it uses, MaxMoves slots move per eviction scan; a pass
// that gives up, or takes longer than DefragMaxPrepares preparations, rests DefragRest ones.
constexpr uint64_t DefragMinWaste   = uint64_t {192} << 20u;
constexpr uint32_t MaxMoves         = 8;
constexpr uint64_t DefragMaxPrepares = 2048;
constexpr uint64_t DefragRest       = 1024;
// The default budget is this share of the largest device local heap, within these limits.
constexpr uint64_t BudgetMin        = uint64_t {512} << 20u;
constexpr uint64_t BudgetMax        = uint64_t {4096} << 20u;
// BLASes whose failed reports are remembered at most; the table entries of the exhausted ones
// are released when the map is full.
constexpr size_t   MaxRetryKeys     = 4096;
// A table entry whose BLAS was evicted (or forgotten): lookups probe past it and new keys reuse
// it (see FindSlot in rt_tlas_walk.comp). Real keys are multiples of 8.
constexpr uint64_t TableTombstone   = 1;

constexpr uint32_t StateSize      = 96;
constexpr uint32_t MissingStride  = 48;
constexpr uint32_t JobStride      = 56;
constexpr uint32_t SlotStride     = 64;
constexpr uint32_t TableStride    = 16;
constexpr uint32_t ContextStride  = 64;
constexpr uint32_t InstanceStride = 64;

constexpr uint32_t ModeCount  = 0;
constexpr uint32_t ModeBuild  = 1;
constexpr uint32_t ModeVerify = 2;

// RtState (rt_common.inc).
struct RtState {
	uint64_t header_va;
	uint64_t instance_base;
	uint32_t node_limit;
	uint32_t instance_count;
	uint32_t box_count;
	uint32_t overflow;
	uint32_t missing_count;
	uint32_t dispatch[3];
	uint32_t unknown_blas;
	uint32_t inactive;
	uint32_t max_depth;
	uint32_t verify_count;
	uint32_t verify[3];
	uint32_t changed_count;
	uint32_t pool_cursor;
	uint32_t pool_overflow;
	uint32_t pad[2];
};
static_assert(sizeof(RtState) == StateSize);
constexpr uint32_t StateDispatch = offsetof(RtState, dispatch);
constexpr uint32_t StateVerify   = offsetof(RtState, verify);

// The readback area: the state, the missing BLASes, the build jobs with their results and the
// slots whose BLAS changed.
constexpr uint64_t ReadbackMissing = StateSize;
constexpr uint64_t ReadbackJobs    = ReadbackMissing + uint64_t {MissingCapacity} * MissingStride;
constexpr uint64_t ReadbackChanged = ReadbackJobs + uint64_t {MaxJobs} * JobStride;
// The slots the preparation's TLAS referenced (the verify list: each once, SlotCapacity at most).
constexpr uint64_t ReadbackUsed    = ReadbackChanged + uint64_t {ChangedCapacity} * 4;
constexpr uint64_t ReadbackSize    = ReadbackUsed + uint64_t {SlotCapacity} * 4;

struct TlasPush {
	uint64_t user_ptr_va;
	uint64_t state;
	uint64_t queue;
	uint64_t instance_nodes;
	uint64_t instances;
	uint64_t table;
	uint64_t slots;
	uint64_t missing;
	uint64_t context;
	uint64_t tlas_address;
	uint64_t verify_list;
	uint32_t header_offset;
	uint32_t root;
	uint32_t instance_capacity;
	uint32_t table_mask;
	uint32_t prepare_id;
	uint32_t verify;
	// The layout of the instance records (Decoder::RayTracingLayout::Pack()).
	std::array<uint32_t, 4> layout;
};
static_assert(sizeof(TlasPush) == 128 && offsetof(TlasPush, layout) % 16 == 0);

struct BlasPush {
	uint64_t jobs;
	uint64_t pool;
	uint64_t slots;
	uint64_t state;
	uint64_t changed;
	uint32_t mode;
	uint32_t pool_capacity;
	uint32_t job_count;
	uint32_t changed_capacity;
};

constexpr uint32_t BlasNodeCountAt80 = 1;

// After the entry table of a slot (16 bytes per triangle), 32-byte aligned: the alpha records of
// its alpha tested triangles, 32 bytes each, that the RT passes fill (see AlphaTest() of the ray
// query emitter) and a walk build clears.
constexpr uint64_t AlphaRecordsOffset(uint32_t capacity0, uint32_t capacity1) {
	return ((uint64_t {capacity0} + capacity1) * 16 + 31u) & ~uint64_t {31};
}

struct Job {
	uint64_t blas_va;
	uint32_t root;
	uint32_t slot;
	uint64_t vertices0;
	uint64_t vertices1;
	uint64_t entries;
	uint32_t capacity0;
	uint32_t capacity1;
	uint32_t result;
	uint32_t pad;
};
static_assert(sizeof(Job) == JobStride);

struct SlotInfo {
	uint64_t as_address;
	uint64_t entry_table;
	uint32_t capacity0;
	uint32_t capacity1;
	uint32_t count0;
	uint32_t count1;
	uint32_t signature;
	uint32_t state;
	uint64_t blas_va;
	uint32_t root;
	uint32_t stamp;
	uint32_t leaf_count;
	uint32_t pad;
};
static_assert(sizeof(SlotInfo) == SlotStride);

struct MissingEntry {
	uint64_t blas_va;
	uint32_t root;
	uint32_t table_index;
	uint32_t leaf_count;
	uint32_t box_count;
	uint32_t opaque_count;
	uint32_t alpha_count;
	uint32_t node_limit;
	uint32_t flags;
	uint32_t pad[2];
};
static_assert(sizeof(MissingEntry) == MissingStride);

constexpr vk::BufferUsageFlags DataFlags =
    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress |
    vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc |
    vk::BufferUsageFlagBits::eIndirectBuffer;
constexpr vk::BufferUsageFlags AsFlags =
    DataFlags | vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
    vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;

constexpr vk::PipelineStageFlags2 AsBuildStage =
    vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;

void MemoryBarrier(const CommandRecorder& command, vk::PipelineStageFlags2 src_stages,
                   vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stages,
                   vk::AccessFlags2 dst_access) {
	vk::MemoryBarrier2 barrier {};
	barrier.srcStageMask  = src_stages;
	barrier.srcAccessMask = src_access;
	barrier.dstStageMask  = dst_stages;
	barrier.dstAccessMask = dst_access;
	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
}

// vkCmdUpdateBuffer of a small array (at most 64 KiB, dword multiples).
void UpdateBuffer(const CommandRecorder& command, vk::Buffer buffer, uint64_t offset,
                  const void* data, uint32_t size) {
	EXIT_IF(size == 0 || size > 65536 || (size & 3u) != 0 || (offset & 3u) != 0);
	const auto* copy = command.CopyArray(static_cast<const uint8_t*>(data), size);
	command.Custom([buffer, offset, copy, size](vk::CommandBuffer c) {
		c.updateBuffer(buffer, offset, size, copy);
	});
}

// Non-indexed triangles (3 floats per vertex): geometry 0 opaque, geometry 1 alpha tested (the
// guest alpha test runs once per candidate).
vk::AccelerationStructureGeometryKHR TriangleGeometry(uint64_t address, uint32_t capacity,
                                                      bool opaque) {
	vk::AccelerationStructureGeometryKHR geometry {};
	geometry.geometryType = vk::GeometryTypeKHR::eTriangles;
	geometry.flags        = opaque ? vk::GeometryFlagBitsKHR::eOpaque
	                               : vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation;
	auto& triangles                    = geometry.geometry.triangles;
	triangles.sType                    = vk::StructureType::eAccelerationStructureGeometryTrianglesDataKHR;
	triangles.vertexFormat             = vk::Format::eR32G32B32Sfloat;
	triangles.vertexData.deviceAddress = address;
	triangles.vertexStride             = 12;
	triangles.maxVertex                = capacity * 3 - 1;
	triangles.indexType                = vk::IndexType::eNoneKHR;
	return geometry;
}

} // namespace

namespace {
// Set when the device is created, before any shader compiles.
std::atomic<bool> g_device_support {false};
} // namespace

void NativeRayTracing::SetDeviceSupport(bool supported) noexcept {
	g_device_support.store(supported, std::memory_order_relaxed);
}

bool NativeRayTracing::PatchShaders() noexcept {
	return g_device_support.load(std::memory_order_relaxed);
}

bool NativeRayTracing::FindTlasSource(const ShaderRecompiler::IR::ShaderInfo& info,
                                      TlasSource& source) noexcept {
	// Only the programs the recompiler patched trace through the host structures.
	if (!info.ray_query || !PatchShaders()) {
		return false;
	}
	source = {info.ray_query_user_sgpr, info.ray_query_header_offset, info.ray_query_root, info.ray_query_layout};
	return true;
}

NativeRayTracing::Allocation NativeRayTracing::Arena::Allocate(uint64_t size, uint64_t alignment) {
	// Sizes and chunk starts are multiples of 256, so are all offsets.
	EXIT_IF(alignment == 0 || alignment > 256 || (256 % alignment) != 0);
	size = (size + 255u) & ~uint64_t {255};
	const auto carve = [&](Chunk& chunk, uint64_t offset, uint64_t length) {
		chunk.free_ranges.erase(offset);
		if (length > size) {
			chunk.free_ranges.emplace(offset + size, length - size);
		}
		chunk.free_bytes -= size;
		m_used += size;
		return Allocation {chunk.buffer.get(), offset, chunk.buffer->BufferDeviceAddress() + offset, size};
	};
	if (size <= ChunkSize / 2) {
		// The fullest chunk with a range that fits, the smallest such range in it: the other
		// chunks empty out and are released. The chunk being drained takes nothing.
		Chunk*   best        = nullptr;
		uint64_t best_offset = 0;
		uint64_t best_length = 0;
		for (auto& chunk: m_chunks) {
			if (chunk->dedicated || chunk->buffer.get() == m_draining || chunk->free_bytes < size ||
			    (best != nullptr && chunk->free_bytes >= best->free_bytes)) {
				continue;
			}
			uint64_t offset = 0;
			uint64_t length = UINT64_MAX;
			for (const auto& [range_offset, range_length]: chunk->free_ranges) {
				if (range_length >= size && range_length < length) {
					offset = range_offset;
					length = range_length;
				}
			}
			if (length != UINT64_MAX) {
				best        = chunk.get();
				best_offset = offset;
				best_length = length;
			}
		}
		if (best != nullptr) {
			return carve(*best, best_offset, best_length);
		}
	}
	auto chunk       = std::make_unique<Chunk>();
	chunk->dedicated = size > ChunkSize / 2;
	chunk->buffer    = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
	                                            AsFlags, chunk->dedicated ? size : ChunkSize);
	chunk->free_ranges.emplace(0, chunk->buffer->Size());
	chunk->free_bytes = chunk->buffer->Size();
	m_reserved += chunk->buffer->Size();
	m_chunks.push_back(std::move(chunk));
	return carve(*m_chunks.back(), 0, m_chunks.back()->buffer->Size());
}

void NativeRayTracing::Arena::Free(const Allocation& allocation) {
	if (allocation.buffer == nullptr) {
		return;
	}
	const auto it = std::find_if(m_chunks.begin(), m_chunks.end(), [&](const auto& chunk) {
		return chunk->buffer.get() == allocation.buffer;
	});
	EXIT_IF(it == m_chunks.end());
	auto& chunk  = **it;
	auto  offset = allocation.offset;
	auto  length = allocation.size;
	m_used -= length;
	chunk.free_bytes += length;
	// Merges with the free ranges around it.
	auto next = chunk.free_ranges.lower_bound(offset);
	if (next != chunk.free_ranges.end() && offset + length == next->first) {
		length += next->second;
		next = chunk.free_ranges.erase(next);
	}
	if (next != chunk.free_ranges.begin()) {
		const auto previous = std::prev(next);
		if (previous->first + previous->second == offset) {
			offset = previous->first;
			length += previous->second;
			chunk.free_ranges.erase(previous);
		}
	}
	chunk.free_ranges.emplace(offset, length);
	if (chunk.free_bytes == chunk.buffer->Size()) {
		// Nothing allocated: released, unless it is the only empty standard chunk.
		const auto empty = std::count_if(m_chunks.begin(), m_chunks.end(), [](const auto& c) {
			return !c->dedicated && c->free_bytes == c->buffer->Size();
		});
		if (chunk.dedicated || chunk.buffer.get() == m_draining || empty > 1) {
			if (chunk.buffer.get() == m_draining) {
				m_draining = nullptr;
			}
			m_reserved -= chunk.buffer->Size();
			m_chunks.erase(it);
		}
	}
}

bool NativeRayTracing::Arena::BeginDrain() {
	uint64_t total_free = 0;
	for (const auto& chunk: m_chunks) {
		if (!chunk->dedicated) {
			total_free += chunk->free_bytes;
		}
	}
	const Chunk* best      = nullptr;
	uint64_t     best_used = 0;
	for (const auto& chunk: m_chunks) {
		const auto used = chunk->buffer->Size() - chunk->free_bytes;
		if (chunk->dedicated || used == 0 || (best != nullptr && used >= best_used)) {
			continue;
		}
		// The other chunks must hold what it has, with some room to spare for fragmentation.
		if (total_free - chunk->free_bytes < used + ChunkSize / 4) {
			continue;
		}
		best      = chunk.get();
		best_used = used;
	}
	m_draining = best != nullptr ? best->buffer.get() : nullptr;
	return best != nullptr;
}

NativeRayTracing::NativeRayTracing(GraphicContext& graphics, CommandScheduler& scheduler,
                                   BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_buffer_cache(buffer_cache),
      m_arena(graphics, scheduler) {
	m_enabled = graphics.native_ray_tracing_enabled;
	if (!m_enabled) {
		return;
	}
	CreatePipelines();
	CreateBuffers();
	CreateTlas(InstanceCapacity);
	// The memory of the BLASes is limited: KYTY_RT_ARENA_MB (0: no limit), else a quarter of the
	// largest device local heap.
	if (const char* limit = std::getenv("KYTY_RT_ARENA_MB"); limit != nullptr && limit[0] != 0) {
		m_budget = std::strtoull(limit, nullptr, 10) << 20u;
	} else {
		const auto& memory = m_graphics.physical_device_memory_properties;
		uint64_t    heap   = 0;
		for (uint32_t i = 0; i < memory.memoryHeapCount; i++) {
			if (memory.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
				heap = std::max<uint64_t>(heap, memory.memoryHeaps[i].size);
			}
		}
		m_budget = std::clamp(heap / 4, BudgetMin, BudgetMax);
	}
	LOGF("NativeRayTracing: enabled, BLAS memory budget %llu MiB\n",
	     static_cast<unsigned long long>(m_budget >> 20u));
}

NativeRayTracing::~NativeRayTracing() {
	if (!m_enabled) {
		return;
	}
	auto device = m_graphics.device;
	for (auto& slot: m_slot_list) {
		if (slot && slot->handle) {
			device.destroyAccelerationStructureKHR(slot->handle);
		}
	}
	for (auto& retired: m_retired) {
		if (retired.handle) {
			device.destroyAccelerationStructureKHR(retired.handle);
		}
	}
	if (m_tlas) {
		device.destroyAccelerationStructureKHR(m_tlas);
	}
	device.destroyPipeline(m_tlas_pipeline);
	device.destroyPipeline(m_blas_pipeline);
	device.destroyPipelineLayout(m_tlas_layout);
	device.destroyPipelineLayout(m_blas_layout);
	device.destroyDescriptorSetLayout(m_descriptor_layout);
}

void NativeRayTracing::CreatePipelines() {
	auto device = m_graphics.device;
	const vk::DescriptorSetLayoutBinding binding {0, vk::DescriptorType::eStorageBuffer, 1,
	                                              vk::ShaderStageFlagBits::eCompute, nullptr};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = 1;
	layout_info.pBindings    = &binding;
	RequireVulkanSuccess(device.createDescriptorSetLayout(&layout_info, nullptr, &m_descriptor_layout),
	                     "create native ray tracing descriptor layout");
	const auto make = [&](std::span<const uint32_t> code, uint32_t push_size,
	                      vk::PipelineLayout& layout, vk::Pipeline& pipeline, const char* name) {
		const vk::PushConstantRange push {vk::ShaderStageFlagBits::eCompute, 0, push_size};
		vk::PipelineLayoutCreateInfo info {};
		info.setLayoutCount         = 1;
		info.pSetLayouts            = &m_descriptor_layout;
		info.pushConstantRangeCount = 1;
		info.pPushConstantRanges    = &push;
		RequireVulkanSuccess(device.createPipelineLayout(&info, nullptr, &layout),
		                     "create native ray tracing pipeline layout");
		const auto module = CompileSPV(code, device);
		vk::PipelineShaderStageCreateInfo stage {};
		stage.stage  = vk::ShaderStageFlagBits::eCompute;
		stage.module = module;
		stage.pName  = "main";
		vk::ComputePipelineCreateInfo pipeline_info {};
		pipeline_info.stage  = stage;
		pipeline_info.layout = layout;
		const auto result = device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &pipeline);
		device.destroyShaderModule(module, nullptr);
		RequireVulkanSuccess(result, "create native ray tracing pipeline");
		SetVulkanObjectNameF(device, pipeline, "%s", name);
	};
	make(RT_TLAS_WALK_SPV, sizeof(TlasPush), m_tlas_layout, m_tlas_pipeline, "RT TLAS walk");
	make(RT_BLAS_WALK_SPV, sizeof(BlasPush), m_blas_layout, m_blas_pipeline, "RT BLAS walk");
}

void NativeRayTracing::CreateBuffers() {
	const auto make = [&](uint64_t size, vk::BufferUsageFlags flags, const char* name) {
		auto buffer = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
		                                       flags, size);
		SetVulkanObjectNameF(m_graphics.device, buffer->Handle(), "%s", name);
		return buffer;
	};
	m_state          = make(StateSize, DataFlags, "RT state");
	m_queue          = make(uint64_t {QueueCapacity} * 4, DataFlags, "RT queue");
	m_instance_nodes = make(uint64_t {InstanceCapacity} * 4, DataFlags, "RT instance nodes");
	m_instances      = make(uint64_t {InstanceCapacity} * InstanceStride, AsFlags, "RT instances");
	m_table          = make(uint64_t {TableEntries} * TableStride, DataFlags, "RT BLAS table");
	m_slots          = make(uint64_t {SlotCapacity} * SlotStride, DataFlags, "RT slots");
	m_missing        = make(uint64_t {MissingCapacity} * MissingStride, DataFlags, "RT missing");
	m_context        = make(uint64_t {MaxContexts} * ContextStride, DataFlags, "RT context");
	m_pool           = make(uint64_t {PoolCapacity} * 4, DataFlags, "RT walk pool");
	m_verify_list    = make(uint64_t {SlotCapacity} * 4, DataFlags, "RT verify list");
	m_changed        = make(uint64_t {ChangedCapacity} * 4, DataFlags, "RT changed");
	m_jobs           = make(uint64_t {MaxJobs} * JobStride, DataFlags, "RT jobs");
	m_build_scratch  = make(BuildScratchSize, AsFlags, "RT build scratch");
	for (auto& readback: m_readbacks) {
		readback.buffer = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                           AllFlags, ReadbackSize);
	}
	m_context_address = m_context->BufferDeviceAddress();
}

void NativeRayTracing::CreateTlas(uint32_t capacity) {
	auto device = m_graphics.device;
	vk::AccelerationStructureGeometryKHR geometry {};
	geometry.geometryType                       = vk::GeometryTypeKHR::eInstances;
	geometry.geometry.instances.sType           = vk::StructureType::eAccelerationStructureGeometryInstancesDataKHR;
	geometry.geometry.instances.arrayOfPointers = VK_FALSE;
	vk::AccelerationStructureBuildGeometryInfoKHR info {};
	info.type          = vk::AccelerationStructureTypeKHR::eTopLevel;
	info.flags         = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
	info.mode          = vk::BuildAccelerationStructureModeKHR::eBuild;
	info.geometryCount = 1;
	info.pGeometries   = &geometry;
	vk::AccelerationStructureBuildSizesInfoKHR sizes {};
	device.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice, &info,
	                                             &capacity, &sizes);
	m_tlas_storage = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
	                                          AsFlags, sizes.accelerationStructureSize);
	vk::AccelerationStructureCreateInfoKHR create {};
	create.buffer = m_tlas_storage->Handle();
	create.size   = sizes.accelerationStructureSize;
	create.type   = vk::AccelerationStructureTypeKHR::eTopLevel;
	RequireVulkanSuccess(device.createAccelerationStructureKHR(&create, nullptr, &m_tlas),
	                     "create native TLAS");
	vk::AccelerationStructureDeviceAddressInfoKHR address_info {};
	address_info.accelerationStructure = m_tlas;
	m_tlas_address      = device.getAccelerationStructureAddressKHR(address_info);
	m_tlas_scratch_size = sizes.buildScratchSize;
	m_tlas_capacity     = capacity;
	EXIT_IF(m_tlas_scratch_size > BuildScratchSize);
}

bool NativeRayTracing::ComputeSlotSizes(uint32_t opaque, uint32_t alpha, SlotSizes& sizes, bool exact) {
	auto device = m_graphics.device;
	// Room to grow (unless the counts are capacities already); the 64-bit sums cannot overflow.
	const uint64_t capacity0 = exact ? opaque : uint64_t {opaque} + opaque / 8 + 16;
	const uint64_t capacity1 = exact ? alpha : uint64_t {alpha} + alpha / 8 + 16;
	if (capacity0 + capacity1 > MaxSlotTriangles) {
		return false;
	}
	sizes.capacity0 = static_cast<uint32_t>(capacity0);
	sizes.capacity1 = static_cast<uint32_t>(capacity1);
	const std::array geometries {TriangleGeometry(0, sizes.capacity0, true),
	                             TriangleGeometry(0, sizes.capacity1, false)};
	const std::array counts {sizes.capacity0, sizes.capacity1};
	vk::AccelerationStructureBuildGeometryInfoKHR info {};
	info.type          = vk::AccelerationStructureTypeKHR::eBottomLevel;
	info.flags         = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
	info.mode          = vk::BuildAccelerationStructureModeKHR::eBuild;
	info.geometryCount = static_cast<uint32_t>(geometries.size());
	info.pGeometries   = geometries.data();
	vk::AccelerationStructureBuildSizesInfoKHR build {};
	device.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice, &info,
	                                             counts.data(), &build);
	sizes.storage   = (build.accelerationStructureSize + 255u) & ~uint64_t {255};
	sizes.vertices0 = (uint64_t {sizes.capacity0} * 36 + 255u) & ~uint64_t {255};
	sizes.vertices1 = (uint64_t {sizes.capacity1} * 36 + 255u) & ~uint64_t {255};
	sizes.entries   = (AlphaRecordsOffset(sizes.capacity0, sizes.capacity1) + uint64_t {sizes.capacity1} * 32 + 255u) &
	                ~uint64_t {255};
	sizes.scratch   = build.buildScratchSize;
	// A build that does not fit in the scratch buffer could never run (and would stall the queue).
	if (sizes.scratch > BuildScratchSize || sizes.Total() > MaxSlotBytes) {
		return false;
	}
	return m_budget == 0 || m_arena.Used() + sizes.Total() <= m_budget;
}

uint64_t NativeRayTracing::SlotBytes(const Slot& slot) noexcept {
	return slot.storage.size + slot.vertices0.size + slot.vertices1.size + slot.entries.size;
}

void NativeRayTracing::AllocateSlot(Slot& slot, const SlotSizes& sizes) {
	auto device        = m_graphics.device;
	slot.capacity0     = sizes.capacity0;
	slot.capacity1     = sizes.capacity1;
	slot.storage       = m_arena.Allocate(sizes.storage, 256);
	slot.vertices0     = m_arena.Allocate(sizes.vertices0, 256);
	slot.vertices1     = m_arena.Allocate(sizes.vertices1, 256);
	slot.entries       = m_arena.Allocate(sizes.entries, 256);
	slot.build_scratch = sizes.scratch;
	slot.built         = false;
	vk::AccelerationStructureCreateInfoKHR create {};
	create.buffer = slot.storage.buffer->Handle();
	create.offset = slot.storage.offset;
	create.size   = sizes.storage;
	create.type   = vk::AccelerationStructureTypeKHR::eBottomLevel;
	RequireVulkanSuccess(device.createAccelerationStructureKHR(&create, nullptr, &slot.handle),
	                     "create native BLAS");
	vk::AccelerationStructureDeviceAddressInfoKHR address_info {};
	address_info.accelerationStructure = slot.handle;
	slot.as_address = device.getAccelerationStructureAddressKHR(address_info);
}

bool NativeRayTracing::CreateSlot(const PendingSlot& pending) {
	// Implausibly large (see MaxSlotTriangles) or over the budget: reported again later.
	SlotSizes sizes;
	if (!ComputeSlotSizes(pending.opaque, pending.alpha, sizes)) {
		return false;
	}
	uint32_t index = 0;
	if (!m_free_slots.empty()) {
		index = m_free_slots.back();
		m_free_slots.pop_back();
	} else if (m_slot_list.size() < SlotCapacity) {
		index = static_cast<uint32_t>(m_slot_list.size());
		m_slot_list.emplace_back();
	} else {
		return false;
	}
	auto slot            = std::make_unique<Slot>();
	slot->blas_va        = pending.blas_va;
	slot->root           = pending.root;
	slot->table_index    = pending.table_index;
	slot->leaf_count     = pending.leaf_count;
	slot->last_use_frame = m_frame;
	AllocateSlot(*slot, sizes);
	slot->needs_build = true;
	m_slots_by_blas[slot->blas_va].push_back(index);
	m_build_queue.push_back(index);
	m_slot_list[index] = std::move(slot);
	// Counted: a later failure of the same BLAS starts over.
	m_retries.erase(pending.blas_va ^ (uint64_t {pending.root} << 48u));
	return true;
}

void NativeRayTracing::RetireResources(Slot& slot, uint32_t slot_index) {
	Retired retired {};
	retired.tick        = m_scheduler.CurrentTick();
	retired.prepare_id  = m_prepare_id;
	retired.handle      = slot.handle;
	retired.allocations = {slot.storage, slot.vertices0, slot.vertices1, slot.entries};
	retired.slot        = slot_index;
	slot.handle         = nullptr;
	slot.as_address     = 0;
	slot.storage = slot.vertices0 = slot.vertices1 = slot.entries = {};
	for (const auto& allocation: retired.allocations) {
		m_retired_bytes += allocation.size;
	}
	m_retired.push_back(retired);
}

void NativeRayTracing::DestroyRetired() {
	auto device = m_graphics.device;
	while (!m_retired.empty()) {
		auto& retired = m_retired.front();
		// Every readback recorded before the release has been processed once the ring went round:
		// none can name the slot index any more.
		const bool readbacks_done = retired.slot == UINT32_MAX ||
		                            m_prepare_id > retired.prepare_id + m_readbacks.size();
		if (!readbacks_done || !m_scheduler.IsFree(retired.tick)) {
			break;
		}
		if (retired.handle) {
			device.destroyAccelerationStructureKHR(retired.handle);
		}
		for (const auto& allocation: retired.allocations) {
			m_retired_bytes -= allocation.size;
			m_arena.Free(allocation);
		}
		if (retired.slot != UINT32_MAX) {
			m_slot_list[retired.slot].reset();
			m_free_slots.push_back(retired.slot);
		}
		m_retired.pop_front();
	}
}

void NativeRayTracing::EvictSlot(uint32_t index) {
	auto& slot       = *m_slot_list[index];
	slot.evicted     = true;
	slot.needs_build = false;
	if (const auto it = m_slots_by_blas.find(slot.blas_va); it != m_slots_by_blas.end()) {
		std::erase(it->second, index);
		if (it->second.empty()) {
			m_slots_by_blas.erase(it);
		}
	}
	RetireResources(slot, index);
}

void NativeRayTracing::EvictUnused(std::vector<uint32_t>& evicted) {
	if (m_frame > EvictFrames) {
		const auto limit = m_frame - EvictFrames;
		for (uint32_t index = 0; index < m_slot_list.size() && evicted.size() < MaxEvictions; index++) {
			auto* slot = m_slot_list[index].get();
			if (slot == nullptr || slot->evicted || slot->last_use_frame >= limit) {
				continue;
			}
			EvictSlot(index);
			evicted.push_back(index);
		}
	}
	EvictOverBudget(evicted);
}

void NativeRayTracing::EvictOverBudget(std::vector<uint32_t>& evicted) {
	// What the retired slots hold is on its way out.
	const auto live = m_arena.Used() - m_retired_bytes;
	if (m_budget == 0 || live <= m_budget / 8 * 7 || m_frame <= LruMinAge) {
		return;
	}
	const auto                                 limit = m_frame - LruMinAge;
	std::vector<std::pair<uint64_t, uint32_t>> candidates;
	for (uint32_t index = 0; index < m_slot_list.size(); index++) {
		const auto* slot = m_slot_list[index].get();
		if (slot != nullptr && !slot->evicted && slot->last_use_frame < limit) {
			candidates.push_back({slot->last_use_frame, index});
		}
	}
	std::sort(candidates.begin(), candidates.end());
	auto current = live;
	for (const auto& [frame, index]: candidates) {
		if (current <= m_budget / 4 * 3 || evicted.size() >= MaxEvictions) {
			break;
		}
		current -= std::min(current, SlotBytes(*m_slot_list[index]));
		EvictSlot(index);
		evicted.push_back(index);
	}
}

void NativeRayTracing::Defragment(std::vector<uint32_t>& moved) {
	const auto used = m_arena.Used();
	if (m_arena.Draining() == nullptr) {
		if (m_prepare_id < m_defrag_after ||
		    m_arena.Reserved() < used + std::max(DefragMinWaste, used / 2) || !m_arena.BeginDrain()) {
			return;
		}
		m_defrag_started = m_prepare_id;
	}
	const Buffer* chunk   = m_arena.Draining();
	const auto    before  = moved.size();
	const auto    touches = [&](const Allocation& allocation) { return allocation.buffer == chunk; };
	for (uint32_t index = 0; index < m_slot_list.size() && moved.size() - before < MaxMoves; index++) {
		auto* slot = m_slot_list[index].get();
		if (slot == nullptr || slot->evicted || slot->handle == nullptr ||
		    !(touches(slot->storage) || touches(slot->vertices0) || touches(slot->vertices1) ||
		      touches(slot->entries))) {
			continue;
		}
		// The same capacities, in memory outside the chunk.
		SlotSizes sizes;
		if (!ComputeSlotSizes(slot->capacity0, slot->capacity1, sizes, true)) {
			continue;
		}
		RetireResources(*slot, UINT32_MAX);
		AllocateSlot(*slot, sizes);
		slot->needs_build = true;
		m_build_queue.push_back(index);
		moved.push_back(index);
	}
	if (moved.size() != before) {
		return;
	}
	// Nothing left to move: the chunk is released once the retired slots are destroyed; a pass
	// that cannot finish rests.
	const bool waiting = std::any_of(m_retired.begin(), m_retired.end(), [&](const Retired& retired) {
		return std::any_of(retired.allocations.begin(), retired.allocations.end(), touches);
	});
	if (!waiting || m_prepare_id - m_defrag_started > DefragMaxPrepares) {
		m_arena.EndDrain();
		m_defrag_after = m_prepare_id + DefragRest;
	}
}

void NativeRayTracing::ReportAgain(uint64_t blas_va, uint32_t root, uint32_t table_index) {
	if (m_retries.size() >= MaxRetryKeys) {
		// Forget the failures: the entries of the exhausted ones are released so they can be
		// reported again, the others are queued anyway.
		for (const auto& [key, state]: m_retries) {
			if (state.count > MaxRetries) {
				m_retry.push_back(state.table_index);
			}
		}
		m_retries.clear();
	}
	auto& state       = m_retries[blas_va ^ (uint64_t {root} << 48u)];
	state.table_index = table_index;
	if (++state.count <= MaxRetries) {
		m_retry.push_back(table_index);
	} else {
		// Not given up for good: it is reported again later.
		m_late_retries.push_back({m_prepare_id + LateRetryPrepares, table_index});
	}
}

void NativeRayTracing::RecordBuilds(const CommandRecorder& command) {
	auto&                 readback = m_readbacks[m_readback_index];
	std::vector<Job>      jobs;
	std::vector<uint32_t> slots;
	uint64_t              scratch   = 0;
	uint64_t              pool_use  = 0;
	constexpr uint64_t    Alignment = 256;
	{
		Common::LockGuard lock(m_mutex);
		// The oldest builds first (a slot never built counts as the oldest): the queue used to be
		// served last in, first out, and under a long backlog (a hundred or more animated BLASes
		// asking for a build every frame) the slots deep in it, such as a ped that just got out of
		// a vehicle, kept the shape of their last build for tens of frames.
		std::stable_sort(m_build_queue.begin(), m_build_queue.end(), [&](uint32_t a, uint32_t b) {
			const auto age = [&](uint32_t index) {
				const auto* slot = m_slot_list[index].get();
				return slot == nullptr || slot->built_frame == UINT64_MAX ? uint64_t {0} : slot->built_frame;
			};
			return age(a) > age(b);
		});
		while (!m_build_queue.empty() && jobs.size() < MaxJobs) {
			const auto index = m_build_queue.back();
			if (m_slot_list[index] == nullptr || m_slot_list[index]->evicted) {
				// Released since it was queued.
				m_build_queue.pop_back();
				continue;
			}
			auto&      slot  = *m_slot_list[index];
			const auto size  = (slot.build_scratch + Alignment - 1) & ~(Alignment - 1);
			if (scratch + size > BuildScratchSize) {
				break;
			}
			// The walks of the builds take a queue each from the pool, which the counts and the
			// checks of the preparation need too: the builds use up to half of it, the rest waits
			// for the next preparation (one build always goes).
			const uint64_t queue =
			    slot.leaf_count != 0
			        ? std::min<uint64_t>(uint64_t {slot.leaf_count} + slot.leaf_count / 2 + 64, PoolQueueMax)
			        : PoolQueueMax;
			if (!jobs.empty() && pool_use + queue > PoolCapacity / 2) {
				break;
			}
			pool_use += queue;
			m_build_queue.pop_back();
			if (!slot.needs_build) {
				continue;
			}
			slot.needs_build = false;
			scratch += size;
			Job job {slot.blas_va, slot.root, index, slot.vertices0.address,
			         slot.vertices1.address, slot.entries.address, slot.capacity0,
			         slot.capacity1, 0, 0, 0, 0, 0};
			if (!slot.layout.empty()) {
				job.layout       = m_layouts->BufferDeviceAddress() + uint64_t {layouts.size()} * 16;
				job.layout_count = static_cast<uint32_t>(slot.layout.size());
				layouts.insert(layouts.end(), slot.layout.begin(), slot.layout.end());
			}
			jobs.push_back(job);
			slots.push_back(index);
		}
	}
	if (jobs.empty()) {
		return;
	}
	// The slot infos (state 0 until built) and table entries (slot + 1) of the slots.
	for (const auto index: slots) {
		const auto&    slot = *m_slot_list[index];
		const SlotInfo info {.as_address  = slot.as_address,
		                     .entry_table = slot.entries.address,
		                     .capacity0   = slot.capacity0,
		                     .capacity1   = slot.capacity1,
		                     .blas_va     = slot.blas_va,
		                     .root        = slot.root,
		                     // The walk takes a queue of about as many nodes from the pool (0: as
		                     // many as the BLAS header allows).
		                     .leaf_count  = slot.leaf_count};
		UpdateBuffer(command, m_slots->Handle(), uint64_t {index} * SlotStride, &info, sizeof(info));
		// The walk may place other triangles: the alpha records start over.
		if (slot.capacity1 != 0) {
			command.fillBuffer(slot.entries.buffer->Handle(),
			                   slot.entries.offset + AlphaRecordsOffset(slot.capacity0, slot.capacity1),
			                   uint64_t {slot.capacity1} * 32, 0);
		}
		// A slot the host table holds has no entry in the walk's table.
		if (slot.table_index != UINT32_MAX) {
			const uint32_t encoded = index + 1;
			UpdateBuffer(command, m_table->Handle(), uint64_t {slot.table_index} * TableStride + 8,
			             &encoded, sizeof(encoded));
		}
		// A layout build writes only the positions its triangles take: the others stay inactive
		// (NaN vertices, entries of ~0).
		if (!slot.layout.empty()) {
			command.fillBuffer(slot.vertices0.buffer->Handle(), slot.vertices0.offset, slot.vertices0.size,
			                   0x7fc00000u);
			command.fillBuffer(slot.vertices1.buffer->Handle(), slot.vertices1.offset, slot.vertices1.size,
			                   0x7fc00000u);
			command.fillBuffer(slot.entries.buffer->Handle(), slot.entries.offset, slot.entries.size,
			                   0xffffffffu);
		}
	}
	for (size_t done = 0; done < layouts.size();) {
		const auto chunk = std::min<size_t>(layouts.size() - done, 4096);
		UpdateBuffer(command, m_layouts->Handle(), uint64_t {done} * 16, &layouts[done],
		             static_cast<uint32_t>(chunk * 16));
		done += chunk;
	}
	UpdateBuffer(command, m_jobs->Handle(), 0, jobs.data(),
	             static_cast<uint32_t>(jobs.size() * JobStride));
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
	              vk::PipelineStageFlagBits2::eComputeShader,
	              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
	const BlasPush push {m_jobs->BufferDeviceAddress(),    m_pool->BufferDeviceAddress(),
	                     m_slots->BufferDeviceAddress(),   m_state->BufferDeviceAddress(),
	                     m_changed->BufferDeviceAddress(), ModeBuild,
	                     PoolCapacity,                     static_cast<uint32_t>(jobs.size()),
	                     ChangedCapacity};
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_blas_pipeline);
	command.pushConstants(m_blas_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
	command.dispatch(static_cast<uint32_t>(jobs.size()), 1, 1);
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eComputeShader,
	              vk::AccessFlagBits2::eShaderWrite, AsBuildStage | vk::PipelineStageFlagBits2::eTransfer,
	              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead);

	// One build per slot, all in one command (distinct scratch ranges).
	struct BuildRecord {
		vk::AccelerationStructureKHR handle;
		uint64_t                     vertices0;
		uint64_t                     vertices1;
		uint64_t                     scratch;
		uint32_t                     capacity0;
		uint32_t                     capacity1;
	};
	std::vector<BuildRecord> records;
	uint64_t                 scratch_offset = 0;
	const auto               scratch_base   = m_build_scratch->BufferDeviceAddress();
	for (const auto index: slots) {
		const auto& slot = *m_slot_list[index];
		records.push_back({slot.handle, slot.vertices0.address, slot.vertices1.address,
		                   scratch_base + scratch_offset, slot.capacity0, slot.capacity1});
		scratch_offset += (slot.build_scratch + Alignment - 1) & ~(Alignment - 1);
	}
	const auto* copy  = command.CopyArray(records.data(), records.size());
	const auto  count = static_cast<uint32_t>(records.size());
	command.Custom([copy, count](vk::CommandBuffer c) {
		std::vector<vk::AccelerationStructureGeometryKHR>              geometries(count * 2);
		std::vector<vk::AccelerationStructureBuildGeometryInfoKHR>     infos(count);
		std::vector<vk::AccelerationStructureBuildRangeInfoKHR>        ranges(count * 2);
		std::vector<const vk::AccelerationStructureBuildRangeInfoKHR*> range_pointers(count);
		for (uint32_t i = 0; i < count; i++) {
			geometries[i * 2]     = TriangleGeometry(copy[i].vertices0, copy[i].capacity0, true);
			geometries[i * 2 + 1] = TriangleGeometry(copy[i].vertices1, copy[i].capacity1, false);
			auto& info                     = infos[i];
			info.type                      = vk::AccelerationStructureTypeKHR::eBottomLevel;
			info.flags                     = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
			info.mode                      = vk::BuildAccelerationStructureModeKHR::eBuild;
			info.dstAccelerationStructure  = copy[i].handle;
			info.geometryCount             = 2;
			info.pGeometries               = &geometries[i * 2];
			info.scratchData.deviceAddress = copy[i].scratch;
			ranges[i * 2].primitiveCount     = copy[i].capacity0;
			ranges[i * 2 + 1].primitiveCount = copy[i].capacity1;
			range_pointers[i]                = &ranges[i * 2];
		}
		c.buildAccelerationStructuresKHR(count, infos.data(), range_pointers.data());
	});
	MemoryBarrier(command, AsBuildStage, vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
	              AsBuildStage | vk::PipelineStageFlagBits2::eComputeShader |
	                  vk::PipelineStageFlagBits2::eTransfer,
	              vk::AccessFlagBits2::eAccelerationStructureReadKHR | vk::AccessFlagBits2::eShaderRead |
	                  vk::AccessFlagBits2::eAccelerationStructureWriteKHR |
	                  vk::AccessFlagBits2::eTransferWrite);
	// Built: the TLAS walk may reference them.
	constexpr uint32_t built = 1;
	for (const auto index: slots) {
		UpdateBuffer(command, m_slots->Handle(), uint64_t {index} * SlotStride + offsetof(SlotInfo, state),
		             &built, sizeof(built));
		m_slot_list[index]->built       = true;
		m_slot_list[index]->built_frame = m_frame;
	}
	readback.build_count = static_cast<uint32_t>(slots.size());
	readback.build_slots = std::move(slots);
}

void NativeRayTracing::Prepare(const CommandRecorder& command, uint64_t user_pointer,
                               const TlasSource& source, uint64_t frame) {
	if (!m_enabled) {
		return;
	}
	auto& readback = m_readbacks[m_readback_index];
	if (readback.tick != 0) {
		// The ring is full: the oldest readback must complete first (rare).
		m_scheduler.Wait(readback.tick);
		m_scheduler.PopPendingOperations();
	}
	readback.build_count = 0;
	readback.build_slots.clear();
	const auto source_key = source.user_sgpr << 16u | source.header_offset;
	readback.source_key   = source_key;
	readback.frame        = frame;
	uint32_t              tlas_count = 0;
	std::vector<uint32_t> retry;
	// Slots released this preparation (index and table entry), and slots that got new memory
	// (their infos must not point at the old structure any more).
	std::vector<std::pair<uint32_t, uint32_t>> evicted;
	std::vector<std::pair<uint32_t, SlotInfo>> reallocated;
	std::vector<uint32_t>                      moved;
	{
		Common::LockGuard lock(m_mutex);
		m_frame = frame;
		DestroyRetired();
		for (const auto& pending: m_new_slots) {
			if (!CreateSlot(pending)) {
				// No free slot: reported again later (a few times).
				ReportAgain(pending.blas_va, pending.root, pending.table_index);
			}
		}
		m_new_slots.clear();
		for (const auto& [index, opaque, alpha]: m_grow) {
			if (m_slot_list[index] == nullptr || m_slot_list[index]->evicted) {
				continue;
			}
			auto& slot = *m_slot_list[index];
			// An implausible or too large count (see ComputeSlotSizes) keeps the slot as it is:
			// its builds drop the triangles that do not fit.
			SlotSizes sizes;
			if (!ComputeSlotSizes(opaque, alpha, sizes)) {
				continue;
			}
			// The recorded work may still use the old structure: it is destroyed later.
			RetireResources(slot, UINT32_MAX);
			AllocateSlot(slot, sizes);
			slot.leaf_count  = 0;
			slot.needs_build = true;
			m_build_queue.push_back(index);
			moved.push_back(index);
		}
		m_grow.clear();
		if (m_prepare_id - m_last_scan_prepare >= EvictScanPrepares) {
			m_last_scan_prepare = m_prepare_id;
			std::vector<uint32_t> indices;
			EvictUnused(indices);
			for (const auto index: indices) {
				evicted.push_back({index, m_slot_list[index]->table_index});
			}
			Defragment(moved);
		}
		for (const auto index: moved) {
			const auto& slot = *m_slot_list[index];
			if (slot.evicted) {
				// Released by the scan above after it got new memory.
				continue;
			}
			reallocated.push_back({index, SlotInfo {.as_address  = slot.as_address,
			                                        .entry_table = slot.entries.address,
			                                        .capacity0   = slot.capacity0,
			                                        .capacity1   = slot.capacity1,
			                                        .blas_va     = slot.blas_va,
			                                        .root        = slot.root}});
		}
		std::erase_if(m_late_retries, [&](const std::pair<uint64_t, uint32_t>& late) {
			if (late.first > m_prepare_id) {
				return false;
			}
			m_retry.push_back(late.second);
			return true;
		});
		retry.swap(m_retry);
		// Slots the guest changed lately are rebuilt before each use: the verification finds a
		// change only after the TLAS of that preparation was built.
		std::erase_if(m_dynamic, [&](uint32_t index) {
			auto* released = m_slot_list[index].get();
			if (released == nullptr || released->evicted) {
				return true;
			}
			auto& slot = *released;
			if (slot.dynamic_until <= m_prepare_id) {
				slot.dynamic_until = 0;
				return true;
			}
			if (!slot.needs_build && slot.built_frame != frame) {
				slot.needs_build = true;
				m_build_queue.push_back(index);
			}
			return false;
		});
		const auto last = m_last_instance_counts[source_key];
		tlas_count      = std::min(m_tlas_capacity, std::max(1024u, (last + last / 4 + 255u) &
		                                                           ~255u));
	}
	m_prepare_id++;

	// Earlier guest work wrote the BVHs; earlier ray queries read the TLAS this rebuilds.
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eAllCommands,
	              vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
	              vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer |
	                  AsBuildStage,
	              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
	                  vk::AccessFlagBits2::eTransferWrite |
	                  vk::AccessFlagBits2::eAccelerationStructureWriteKHR);
	if (!m_cleared) {
		// The buffers start clear (empty table, invalid contexts).
		m_cleared = true;
		for (const auto* buffer: {m_table.get(), m_slots.get(), m_context.get()}) {
			command.fillBuffer(buffer->Handle(), 0, buffer->Size(), 0);
		}
		// Patched shaders find the context through the BDA page table.
		UpdateBuffer(command, m_buffer_cache.GetBdaPageTableBuffer()->Handle(),
		             BufferCache::NATIVE_RT_CONTEXT_PAGE * sizeof(uint64_t), &m_context_address,
		             sizeof(m_context_address));
	}
	// Table entries to forget become tombstones: the probe chains through them stay intact and
	// the walk reports the BLAS again (or reuses the entry for another key).
	const auto forget = [&](uint32_t table_index) {
		const std::array<uint64_t, 2> tombstone {TableTombstone, 0};
		UpdateBuffer(command, m_table->Handle(), uint64_t {table_index} * TableStride, tombstone.data(),
		             sizeof(tombstone));
	};
	for (const auto index: retry) {
		forget(index);
	}
	// Released slots: no table entry finds them and no TLAS may reference them (state 0).
	for (const auto& [index, table_index]: evicted) {
		constexpr SlotInfo cleared {};
		UpdateBuffer(command, m_slots->Handle(), uint64_t {index} * SlotStride, &cleared, sizeof(cleared));
		forget(table_index);
	}
	for (const auto& [index, info]: reallocated) {
		UpdateBuffer(command, m_slots->Handle(), uint64_t {index} * SlotStride, &info, sizeof(info));
	}
	command.fillBuffer(m_state->Handle(), 0, StateSize, 0);
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
	              vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
	              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
	                  vk::AccessFlagBits2::eTransferWrite);

	auto* page_table = m_buffer_cache.GetBdaPageTableBuffer();
	const vk::DescriptorBufferInfo page_info {page_table->Handle(), 0, page_table->Size()};
	vk::WriteDescriptorSet write {};
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eStorageBuffer;
	write.pBufferInfo     = &page_info;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_blas_layout, 0, 1, &write);

	// The BVHs of the traversal's layout (Decoder::RayTracingLayout::Pack() word 3).
	m_blas_flags = ((source.layout[3] >> 29u) & 1u) != 0 ? BlasNodeCountAt80 : 0u;
	RecordBuilds(command);

	command.fillBuffer(m_instances->Handle(), 0, uint64_t {tlas_count} * InstanceStride, 0);
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
	              vk::PipelineStageFlagBits2::eComputeShader,
	              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_tlas_layout, 0, 1, &write);
	const TlasPush tlas_push {
	    .user_ptr_va       = user_pointer,
	    .state             = m_state->BufferDeviceAddress(),
	    .queue             = m_queue->BufferDeviceAddress(),
	    .instance_nodes    = m_instance_nodes->BufferDeviceAddress(),
	    .instances         = m_instances->BufferDeviceAddress(),
	    .table             = m_table->BufferDeviceAddress(),
	    .slots             = m_slots->BufferDeviceAddress(),
	    .missing           = m_missing->BufferDeviceAddress(),
	    .context           = m_context_address,
	    .tlas_address      = m_tlas_address,
	    .verify_list       = m_verify_list->BufferDeviceAddress(),
	    .header_offset     = source.header_offset,
	    .root              = source.root,
	    .instance_capacity = tlas_count,
	    .table_mask        = TableEntries - 1,
	    .prepare_id        = m_prepare_id,
	    .verify            = 1u,
	    // The facing of each instance follows the game's triangle test (the layout's flip bit: GTA
	    // V 01.011's accepts, with flags bit 3 clear, the triangles whose test sign is set, the
	    // host's back faces; not flipping them culled the wrong face of every instance without both
	    // faces, and single sided geometry such as peds, roofs and walls cast no shadow).
	    .layout            = source.layout,
	};
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_tlas_pipeline);
	command.pushConstants(m_tlas_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(tlas_push),
	                      &tlas_push);
	command.dispatch(1, 1, 1);
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
	              vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eDrawIndirect |
	                  AsBuildStage,
	              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
	                  vk::AccessFlagBits2::eIndirectCommandRead);

	// Counts the leaves of the BLASes the walk found missing, and checks the built ones it uses.
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_blas_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_blas_layout, 0, 1, &write);
	BlasPush blas_push {m_missing->BufferDeviceAddress(), m_pool->BufferDeviceAddress(),
	                    m_slots->BufferDeviceAddress(),   m_state->BufferDeviceAddress(),
	                    m_changed->BufferDeviceAddress(), ModeCount,
	                    PoolCapacity,                     0,
	                    ChangedCapacity};
	command.pushConstants(m_blas_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(blas_push),
	                      &blas_push);
	command.dispatchIndirect(m_state->Handle(), StateDispatch);
	blas_push.jobs = m_verify_list->BufferDeviceAddress();
	blas_push.mode = ModeVerify;
	command.pushConstants(m_blas_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(blas_push),
	                      &blas_push);
	command.dispatchIndirect(m_state->Handle(), StateVerify);

	// The TLAS over the walk's instances (inactive ones have a null BLAS).
	{
		const auto handle    = m_tlas;
		const auto instances = m_instances->BufferDeviceAddress();
		const auto scratch   = m_build_scratch->BufferDeviceAddress();
		command.Custom([handle, instances, scratch, tlas_count](vk::CommandBuffer c) {
			vk::AccelerationStructureGeometryKHR geometry {};
			geometry.geometryType = vk::GeometryTypeKHR::eInstances;
			geometry.geometry.instances.sType =
			    vk::StructureType::eAccelerationStructureGeometryInstancesDataKHR;
			geometry.geometry.instances.arrayOfPointers    = VK_FALSE;
			geometry.geometry.instances.data.deviceAddress = instances;
			vk::AccelerationStructureBuildGeometryInfoKHR info {};
			info.type                      = vk::AccelerationStructureTypeKHR::eTopLevel;
			info.flags                     = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
			info.mode                      = vk::BuildAccelerationStructureModeKHR::eBuild;
			info.dstAccelerationStructure  = handle;
			info.geometryCount             = 1;
			info.pGeometries               = &geometry;
			info.scratchData.deviceAddress = scratch;
			vk::AccelerationStructureBuildRangeInfoKHR range {};
			range.primitiveCount      = tlas_count;
			const auto* range_pointer = &range;
			c.buildAccelerationStructuresKHR(1, &info, &range_pointer);
		});
	}
	MemoryBarrier(command, AsBuildStage | vk::PipelineStageFlagBits2::eComputeShader,
	              vk::AccessFlagBits2::eAccelerationStructureWriteKHR | vk::AccessFlagBits2::eShaderWrite,
	              vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
	              vk::AccessFlagBits2::eAccelerationStructureReadKHR | vk::AccessFlagBits2::eShaderRead |
	                  vk::AccessFlagBits2::eTransferRead);

	// Reads back the state, the counted missing BLASes, the build results and the changed slots.
	const std::array<vk::BufferCopy, 5> regions {{
	    {0, 0, StateSize},
	    {0, ReadbackMissing, uint64_t {MissingCapacity} * MissingStride},
	    {0, ReadbackJobs, uint64_t {MaxJobs} * JobStride},
	    {0, ReadbackChanged, uint64_t {ChangedCapacity} * 4},
	    {0, ReadbackUsed, uint64_t {SlotCapacity} * 4},
	}};
	command.copyBuffer(m_state->Handle(), readback.buffer->Handle(), 1, &regions[0]);
	command.copyBuffer(m_missing->Handle(), readback.buffer->Handle(), 1, &regions[1]);
	command.copyBuffer(m_jobs->Handle(), readback.buffer->Handle(), 1, &regions[2]);
	command.copyBuffer(m_changed->Handle(), readback.buffer->Handle(), 1, &regions[3]);
	command.copyBuffer(m_verify_list->Handle(), readback.buffer->Handle(), 1, &regions[4]);
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
	              vk::PipelineStageFlagBits2::eAllCommands,
	              vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
	readback.tick = m_scheduler.CurrentTick();
	m_scheduler.DeferHostOperation([this, &readback] { ProcessReadback(readback); });
	m_readback_index = (m_readback_index + 1) % m_readbacks.size();
}

void NativeRayTracing::ProcessReadback(Readback& readback) {
	readback.buffer->Invalidate(0, ReadbackSize);
	const auto* bytes = readback.buffer->Mapped().data();
	RtState     state {};
	std::memcpy(&state, bytes, sizeof(state));
	const auto               missing_count = std::min(state.missing_count, MissingCapacity);
	std::vector<PendingSlot> pending;
	std::vector<uint32_t>    retry;
	Common::LockGuard        lock(m_mutex);
	for (uint32_t i = 0; i < missing_count; i++) {
		MissingEntry entry {};
		std::memcpy(&entry, bytes + ReadbackMissing + uint64_t {i} * MissingStride, sizeof(entry));
		if (entry.flags != 0 || entry.leaf_count == 0) {
			// Not countable now (memory not written yet, a too large BLAS): forget the entry so
			// the walk reports it again, a few times.
			ReportAgain(entry.blas_va, entry.root, entry.table_index);
			continue;
		}
		m_new_slots.push_back({entry.blas_va, entry.root, entry.table_index, entry.leaf_count,
		                       entry.opaque_count, entry.alpha_count});
	}
	for (uint32_t i = 0; i < readback.build_count && i < readback.build_slots.size(); i++) {
		Job job {};
		std::memcpy(&job, bytes + ReadbackJobs + uint64_t {i} * JobStride, sizeof(job));
		if ((job.result & 1u) == 0 && ((job.result >> 1u) & 5u) != 0) {
			// The walk found no room: in the pool (other jobs of the preparation took it) or in
			// its queue (the BLAS has more nodes than when it was counted). The slot misses
			// triangles: built again, after a queue overflow with a queue of the size the BLAS
			// header allows.
			const auto index = readback.build_slots[i];
			if (m_slot_list[index] != nullptr && !m_slot_list[index]->evicted) {
				auto& slot = *m_slot_list[index];
				if (((job.result >> 1u) & 1u) != 0) {
					slot.leaf_count = 0;
				}
				if (!slot.needs_build) {
					slot.needs_build = true;
					m_build_queue.push_back(index);
				}
			}
		}
		if ((job.result & 1u) != 0) {
			// More triangles than the slot holds (the guest rebuilt the BLAS bigger): the job
			// came back with the counts. A slot released since needs nothing.
			const auto index = readback.build_slots[i];
			if (m_slot_list[index] != nullptr && !m_slot_list[index]->evicted) {
				m_grow.push_back({index, job.capacity0, job.capacity1});
			}
		}
	}
	const auto changed = std::min(state.changed_count, ChangedCapacity);
	for (uint32_t i = 0; i < changed; i++) {
		uint32_t index = 0;
		std::memcpy(&index, bytes + ReadbackChanged + uint64_t {i} * 4, sizeof(index));
		if (index >= m_slot_list.size() || m_slot_list[index] == nullptr ||
		    m_slot_list[index]->evicted) {
			continue;
		}
		auto& slot = *m_slot_list[index];
		if (slot.dynamic_until == 0) {
			m_dynamic.push_back(index);
		}
		slot.dynamic_until = m_prepare_id + DynamicPrepares;
		if (!slot.needs_build) {
			slot.needs_build = true;
			m_build_queue.push_back(index);
		}
	}
	// The slots the TLAS referenced were used in the preparation's frame.
	const auto used = std::min(state.verify_count, SlotCapacity);
	for (uint32_t i = 0; i < used; i++) {
		uint32_t index = 0;
		std::memcpy(&index, bytes + ReadbackUsed + uint64_t {i} * 4, sizeof(index));
		if (index < m_slot_list.size() && m_slot_list[index] != nullptr &&
		    !m_slot_list[index]->evicted) {
			auto& slot          = *m_slot_list[index];
			slot.last_use_frame = std::max(slot.last_use_frame, readback.frame);
		}
	}
	m_last_instance_counts[readback.source_key] = state.instance_count;
	readback.tick = 0;
}


} // namespace Libs::Graphics
