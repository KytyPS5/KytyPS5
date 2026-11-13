#include "graphics/host_gpu/renderer/rayTracing.h"

#include "common/alignment.h"
#include "gpu_blit_shaders/gpu_bvh_mirror_spv.h"
#include "gpu_blit_shaders/gpu_bvh_walk_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <span>

namespace Libs::Graphics {
namespace {

enum Mode : uint32_t { Walk, Triangles, Boxes, Instances };

constexpr uint32_t PairGeometry = 1; // The geometry hash of its last build or update is known.
constexpr uint32_t PairUpdated  = 2;
constexpr uint32_t StateAbsent  = 1; // The job met an unsynchronized page.
constexpr uint32_t GroupSize    = 64;
constexpr uint64_t TriangleSize = 36;

constexpr auto BuildInput = vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
constexpr auto BlasFlags  = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace |
                            vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate;

struct Parameters {
	uint64_t          tlas;
	uint64_t          extended_base;
	uint64_t          fold;
	vk::DeviceAddress blases;
	vk::DeviceAddress pairs;
	vk::DeviceAddress keys;
	vk::DeviceAddress walks;
	vk::DeviceAddress triangle_chunks;
	vk::DeviceAddress box_chunks;
	vk::DeviceAddress instances;
	vk::DeviceAddress context;
	vk::DeviceAddress blas_states;
	vk::DeviceAddress pair_states;
	vk::DeviceAddress walk_results;
	vk::DeviceAddress missing;
	uint32_t          instance_slots;
	uint32_t          key_count;
	uint32_t          page_bits;
};
static_assert(sizeof(Parameters) == 136);

struct Push {
	vk::DeviceAddress parameters;
	uint32_t          mode;
};

struct GpuBlas {
	uint64_t          address;
	vk::DeviceAddress triangles;
	uint32_t          capacity;
};
static_assert(sizeof(GpuBlas) == 24);

struct GpuPair {
	vk::DeviceAddress structure;
	vk::DeviceAddress boxes;
	uint32_t          blas;
	uint32_t          root;
	uint32_t          first;
	uint32_t          topology;
	uint32_t          links;
	uint32_t          box_count;
	uint32_t          box_capacity;
	uint32_t          geometry;
	uint32_t          flags;
};
static_assert(sizeof(GpuPair) == 56);

struct BlasState {
	uint32_t topology;
	uint32_t geometry;
	uint32_t begin;
	uint32_t end;
	uint32_t nodes;
	uint32_t fail;
};

struct PairState {
	uint32_t links;
	uint32_t fail;
	uint32_t used; // A visible instance named the pair.
};

// The BLAS states follow it, at the 16-byte alignment of a buffer reference.
struct alignas(16) Context {
	uint32_t valid;
	uint32_t instances;
	uint32_t missing_count;
	uint64_t structure;
	uint64_t records;
};
static_assert(sizeof(Context) == 32);

// The job's state, copied to a readback area: the context, a BlasState per BLAS, a PairState per
// pair, a {count, first, boxes, links} per walk, and the {BLAS low, BLAS high, root, 0} per
// visible instance without a pair.
struct StateLayout {
	uint64_t pair_states;
	uint64_t walk_results;
	uint64_t missing;
	uint64_t size;
};

StateLayout Layout(size_t blases, size_t pairs, size_t walks, uint32_t instance_slots) {
	const auto pair_states  = Common::AlignUp(sizeof(Context) + sizeof(BlasState) * blases, 16);
	const auto walk_results = Common::AlignUp(pair_states + sizeof(PairState) * pairs, 16);
	const auto missing      = walk_results + 16 * walks;
	return {pair_states, walk_results, missing, missing + 16ull * instance_slots};
}

// Each step consumes what the one before wrote, the last one on the host.
void Barrier(vk::CommandBuffer command) {
	const vk::MemoryBarrier2 barrier {.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands,
	                                  .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
	                                  .dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands |
	                                                   vk::PipelineStageFlagBits2::eHost,
	                                  .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
	                                                   vk::AccessFlagBits2::eMemoryWrite |
	                                                   vk::AccessFlagBits2::eHostRead};
	command.pipelineBarrier2(
	    vk::DependencyInfo {.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

vk::AccelerationStructureGeometryKHR TriangleGeometry(vk::DeviceAddress vertices, uint32_t count) {
	vk::AccelerationStructureGeometryKHR geometry {};
	geometry.geometryType              = vk::GeometryTypeKHR::eTriangles;
	geometry.flags                     = vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation;
	auto& triangles                    = geometry.geometry.triangles;
	triangles.vertexFormat             = vk::Format::eR32G32B32Sfloat;
	triangles.vertexData.deviceAddress = vertices;
	triangles.vertexStride             = 12;
	triangles.maxVertex                = 3 * count - 1;
	triangles.indexType                = vk::IndexType::eNoneKHR;
	return geometry;
}

vk::AccelerationStructureGeometryKHR InstanceGeometry(vk::DeviceAddress instances) {
	vk::AccelerationStructureGeometryKHR geometry {};
	geometry.geometryType       = vk::GeometryTypeKHR::eInstances;
	geometry.geometry.instances = vk::AccelerationStructureGeometryInstancesDataKHR {};
	geometry.geometry.instances.data.deviceAddress = instances;
	return geometry;
}

} // namespace

RayTracing::RayTracing(RenderContext& context)
    : m_graphics(context.GetGraphics()), m_scheduler(context.GetCommandScheduler()),
      m_buffer_cache(context.GetBufferCache()), m_context(context) {
	vk::PhysicalDeviceAccelerationStructurePropertiesKHR limits {};
	vk::PhysicalDeviceProperties2                        properties {};
	properties.pNext = &limits;
	m_graphics.physical_device.getProperties2(&properties);
	m_scratch_alignment = limits.minAccelerationStructureScratchOffsetAlignment;
	// A slot is a 24-bit custom index.
	m_max_instances = std::min(limits.maxInstanceCount, uint64_t {1} << 24);

	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = std::size(bindings);
	layout_info.pBindings    = bindings;
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_descriptor_layout),
	    "create BVH mirror descriptor layout");
	const vk::PushConstantRange  range {vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_descriptor_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &range;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr, &m_layout),
	    "create BVH mirror pipeline layout");
	m_walk_pipeline = CreateComputePipeline(m_graphics.device, GPU_BVH_WALK_SPV, m_layout,
	                                        "create BVH walk pipeline");
	m_pipeline      = CreateComputePipeline(m_graphics.device, GPU_BVH_MIRROR_SPV, m_layout,
	                                        "create BVH mirror pipeline");
}

RayTracing::~RayTracing() {
	const auto device = m_graphics.device;
	for (const auto& pair: m_pairs) {
		device.destroyAccelerationStructureKHR(pair.structure.handle, nullptr);
	}
	for (const auto& [address, tlas]: m_tlases) {
		device.destroyAccelerationStructureKHR(tlas.structure.handle, nullptr);
	}
	device.destroyPipeline(m_walk_pipeline, nullptr);
	device.destroyPipeline(m_pipeline, nullptr);
	device.destroyPipelineLayout(m_layout, nullptr);
	device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
}

void RayTracing::Reserve(std::unique_ptr<Buffer>& buffer, vk::BufferUsageFlags usage, uint64_t size,
                         MemoryUsage memory) {
	if (buffer && buffer->Size() >= size) {
		return;
	}
	Retire(buffer);
	buffer = std::make_unique<Buffer>(m_graphics, m_scheduler, memory, 0,
	                                  usage | vk::BufferUsageFlagBits::eShaderDeviceAddress,
	                                  std::bit_ceil(size));
}

void RayTracing::Create(Structure& structure, const vk::AccelerationStructureGeometryKHR& geometry,
                        vk::AccelerationStructureTypeKHR       type,
                        vk::BuildAccelerationStructureFlagsKHR flags, uint32_t capacity) {
	Retire(structure);
	vk::AccelerationStructureBuildGeometryInfoKHR info {};
	info.type          = type;
	info.flags         = flags;
	info.geometryCount = 1;
	info.pGeometries   = &geometry;
	vk::AccelerationStructureBuildSizesInfoKHR sizes {};
	m_graphics.device.getAccelerationStructureBuildSizesKHR(
	    vk::AccelerationStructureBuildTypeKHR::eDevice, &info, &capacity, &sizes);
	Reserve(structure.buffer, vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR,
	        sizes.accelerationStructureSize);
	vk::AccelerationStructureCreateInfoKHR create {};
	create.buffer = structure.buffer->Handle();
	create.size   = sizes.accelerationStructureSize;
	create.type   = type;
	RequireVulkanSuccess(
	    m_graphics.device.createAccelerationStructureKHR(&create, nullptr, &structure.handle),
	    "create acceleration structure");
	const vk::AccelerationStructureDeviceAddressInfoKHR address {.accelerationStructure =
	                                                                 structure.handle};
	structure.address        = m_graphics.device.getAccelerationStructureAddressKHR(&address);
	structure.build_scratch  = sizes.buildScratchSize;
	structure.update_scratch = sizes.updateScratchSize;
	structure.capacity       = capacity;
}

// Retired buffers and structures outlive the commands recorded so far.
void RayTracing::Retire(std::unique_ptr<Buffer>& buffer) {
	if (buffer) {
		m_scheduler.DeferOperation([retired = std::move(buffer)] {});
	}
}

void RayTracing::Retire(Structure& structure) {
	if (structure.handle) {
		m_scheduler.DeferOperation([device = m_graphics.device, handle = structure.handle,
		                            retired = std::move(structure.buffer)] {
			device.destroyAccelerationStructureKHR(handle, nullptr);
		});
	}
}

void RayTracing::AddPair(uint64_t blas, uint32_t root) {
	if (!m_pair_index.try_emplace({blas, root}, static_cast<uint32_t>(m_pairs.size())).second) {
		return;
	}
	const auto [it, added] = m_blas_index.try_emplace(blas, static_cast<uint32_t>(m_blases.size()));
	if (added) {
		m_blases.push_back({.address = blas});
	}
	m_pairs.push_back({.blas = it->second, .root = root});
}

void RayTracing::Collect() {
	for (uint32_t i = 0; i < Areas; ++i) {
		auto& area = m_areas[(m_next_area + i) % Areas];
		if (area.tick == 0) {
			continue;
		}
		if (!m_scheduler.IsFree(area.tick)) {
			return;
		}
		Apply(area);
	}
}

void RayTracing::Apply(Area& area) {
	area.tick = 0;
	m_applied = area.serial;
	const auto layout =
	    Layout(area.covered.size(), area.pairs, area.walks.size(), area.instance_slots);
	area.buffer->Invalidate(0, layout.size);
	const auto* data = area.buffer->Mapped().data();
	Context     context {};
	std::memcpy(&context, data, sizeof(context));
	// Instance counts past the device limit keep the mirror invalid.
	const auto it = m_tlases.find(area.tlas);
	if (it != m_tlases.end() && context.instances > it->second.instance_slots &&
	    context.instances <= m_max_instances) {
		it->second.instance_slots = static_cast<uint32_t>(
		    std::min(std::bit_ceil(uint64_t {context.instances}), m_max_instances));
	}
	for (uint32_t i = 0; i < std::min(context.missing_count, area.instance_slots); ++i) {
		std::array<uint32_t, 4> key {};
		std::memcpy(key.data(), data + layout.missing + sizeof(key) * i, sizeof(key));
		AddPair(key[0] | uint64_t {key[1]} << 32, key[2]);
	}

	std::vector<BlasState> states(area.covered.size());
	std::memcpy(states.data(), data + sizeof(Context), sizeof(BlasState) * states.size());
	// Hashes cover a BLAS once every record of it was hashed.
	const auto complete = [&](uint32_t index) {
		return states[index].fail == 0 &&
		       states[index].end - states[index].begin <= area.covered[index];
	};
	for (uint32_t index = 0; index < states.size(); ++index) {
		auto&       blas  = m_blases[index];
		const auto& state = states[index];
		if (state.fail == 0) {
			blas.begin = state.begin;
			blas.end   = state.end;
			blas.nodes = state.nodes;
		}
		if (!complete(index)) {
			continue;
		}
		if (blas.known && blas.topology == state.topology && blas.geometry != state.geometry) {
			blas.dynamic = true;
		}
		blas.topology = state.topology;
		blas.geometry = state.geometry;
		blas.known    = true;
	}
	// Links that changed under the same records need a new walk.
	for (const auto index: area.verified) {
		PairState state {};
		std::memcpy(&state, data + layout.pair_states + sizeof(state) * index, sizeof(state));
		if (state.fail == 0 && state.links != m_pairs[index].links) {
			m_pairs[index].walked = false;
		}
	}
	for (size_t index = 0; index < area.pairs; ++index) {
		PairState state {};
		std::memcpy(&state, data + layout.pair_states + sizeof(state) * index, sizeof(state));
		if (state.used != 0) {
			m_pairs[index].used = true;
		}
	}
	for (auto& pair: m_pairs) {
		if (pair.update_serial == area.serial) {
			pair.geometry       = states[pair.blas].geometry;
			pair.geometry_known = complete(pair.blas);
		}
	}
	for (size_t walk = 0; walk < area.walks.size(); ++walk) {
		auto&                   pair  = m_pairs[area.walks[walk]];
		const auto&             state = states[pair.blas];
		std::array<uint32_t, 4> result {};
		std::memcpy(result.data(), data + layout.walk_results + sizeof(result) * walk,
		            sizeof(result));
		pair.walking = false;
		// Absent pages and a box list the BLAS outgrew get a new walk; a malformed subtree waits
		// for the BLAS to change.
		const bool retry =
		    result[0] == 0 && ((result[1] & StateAbsent) != 0 || state.nodes > pair.box_capacity);
		if (retry || !complete(pair.blas)) {
			continue;
		}
		const auto first = result[0] != 0 ? result[1] : 0;
		// Other records or another range need a rebuilt mirror.
		if (pair.topology != state.topology || pair.count != result[0] || pair.first != first) {
			pair.built = false;
		}
		pair.walked    = true;
		pair.count     = result[0];
		pair.first     = first;
		pair.box_count = result[2];
		pair.links     = result[3];
		pair.topology  = state.topology;
	}
}

vk::DeviceAddress RayTracing::Prepare(uint64_t tlas) {
	Collect();
	Area       unread;
	const bool readback = m_areas[m_next_area].tick == 0;
	auto&      area     = readback ? m_areas[m_next_area] : unread;
	if (readback) {
		m_next_area = (m_next_area + 1) % Areas;
	}
	const auto serial = ++m_serial;

	auto& top = m_tlases[tlas];
	top.used  = true;
	if (top.structure.capacity != top.instance_slots) {
		Reserve(top.instances, BuildInput, 64ull * top.instance_slots);
		Create(top.structure, InstanceGeometry(top.instances->BufferDeviceAddress()),
		       vk::AccelerationStructureTypeKHR::eTopLevel,
		       vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace, top.instance_slots);
	}

	// Triangle buffers for records [begin, end), which the header bound keeps within the minimum
	// primitive limit of a device. Only BLASes the call builds from get theirs written.
	std::vector<GpuBlas>                 blases(m_blases.size());
	std::vector<std::array<uint32_t, 2>> triangle_chunks;
	area.covered.assign(m_blases.size(), 0);
	for (uint32_t index = 0; index < m_blases.size(); ++index) {
		auto&      blas    = m_blases[index];
		const auto records = blas.end - blas.begin;
		if (records > blas.capacity) {
			Reserve(blas.triangles, BuildInput, TriangleSize * records);
			blas.capacity = static_cast<uint32_t>(blas.triangles->Size() / TriangleSize);
		}
		blases[index] = {blas.address, 0, blas.capacity};
		// A BLAS of unknown size gets one chunk, which reads its header.
		do {
			triangle_chunks.push_back({index, area.covered[index]});
			area.covered[index] += GroupSize;
		} while (area.covered[index] < records);
	}

	// Pairs without a walk under the BLAS topology last seen get one. Walked ranges get a mirror,
	// updated every call once the BLAS geometry changed, and their visited boxes hashed again.
	// Builds take scratch offsets relative to the scratch buffer until it is sized.
	std::vector<vk::AccelerationStructureGeometryKHR>              geometries;
	std::vector<vk::AccelerationStructureBuildGeometryInfoKHR>     builds;
	std::vector<vk::AccelerationStructureBuildRangeInfoKHR>        ranges;
	std::vector<const vk::AccelerationStructureBuildRangeInfoKHR*> range_pointers;
	geometries.reserve(m_pairs.size());
	ranges.reserve(m_pairs.size());
	uint64_t                             scratch_size = 0;
	std::vector<GpuPair>                 pairs(m_pairs.size());
	std::vector<std::array<uint32_t, 2>> box_chunks;
	area.walks.clear();
	area.verified.clear();
	for (uint32_t index = 0; index < m_pairs.size(); ++index) {
		auto&       pair    = m_pairs[index];
		const auto& blas    = m_blases[pair.blas];
		const bool  current = pair.walked && blas.known && pair.topology == blas.topology;
		if (!current && !pair.walking && blas.end > blas.begin) {
			Reserve(pair.boxes, {}, 4ull * std::max(blas.nodes, 1u));
			pair.box_capacity = static_cast<uint32_t>(pair.boxes->Size() / 4);
			area.walks.push_back(index);
			pair.walking = true;
		}
		const bool ready   = current && pair.count != 0 && pair.first >= blas.begin &&
		                     pair.first - blas.begin + uint64_t {pair.count} <= blas.capacity &&
		                     pair.first < (1u << 24);
		const bool rebuild = ready && !pair.built;
		const bool update =
		    ready && !rebuild &&
		    (blas.dynamic || (!pair.geometry_known && m_applied >= pair.update_serial));
		if (rebuild && pair.structure.capacity < pair.count) {
			Create(pair.structure, TriangleGeometry(0, pair.count),
			       vk::AccelerationStructureTypeKHR::eBottomLevel, BlasFlags, pair.count);
		}
		if (rebuild || update) {
			pair.built                  = true;
			pair.update_serial          = serial;
			pair.geometry_known         = false;
			blases[pair.blas].triangles = blas.triangles->BufferDeviceAddress();
			geometries.push_back(TriangleGeometry(blases[pair.blas].triangles +
			                                          TriangleSize * (pair.first - blas.begin),
			                                      pair.count));
			vk::AccelerationStructureBuildGeometryInfoKHR info {};
			info.type                      = vk::AccelerationStructureTypeKHR::eBottomLevel;
			info.flags                     = BlasFlags;
			info.mode                      = update ? vk::BuildAccelerationStructureModeKHR::eUpdate
			                                        : vk::BuildAccelerationStructureModeKHR::eBuild;
			info.srcAccelerationStructure  = update ? pair.structure.handle : nullptr;
			info.dstAccelerationStructure  = pair.structure.handle;
			info.geometryCount             = 1;
			info.pGeometries               = &geometries.back();
			info.scratchData.deviceAddress = scratch_size;
			scratch_size += Common::AlignUp(update ? pair.structure.update_scratch
			                                       : pair.structure.build_scratch,
			                                m_scratch_alignment);
			builds.push_back(info);
			ranges.push_back({pair.count, 0, 0, 0});
			range_pointers.push_back(&ranges.back());
		}
		if (ready) {
			area.verified.push_back(index);
		}
		for (uint32_t box = 0; ready && box < pair.box_count; box += GroupSize) {
			box_chunks.push_back({index, box});
		}
		pairs[index] = {
		    .structure    = ready ? pair.structure.address : 0,
		    .boxes        = pair.boxes ? pair.boxes->BufferDeviceAddress() : 0,
		    .blas         = pair.blas,
		    .root         = pair.root,
		    .first        = pair.first,
		    .topology     = pair.topology,
		    .links        = pair.links,
		    .box_count    = pair.box_count,
		    .box_capacity = pair.box_capacity,
		    .geometry     = pair.geometry,
		    .flags =
		        (pair.geometry_known ? PairGeometry : 0) | ((rebuild || update) ? PairUpdated : 0),
		};
	}

	// The instance job looks pairs up by key.
	if (m_keys.size() != m_pairs.size()) {
		m_keys.clear();
		for (const auto& [key, index]: m_pair_index) {
			m_keys.push_back({static_cast<uint32_t>(key.first),
			                  static_cast<uint32_t>(key.first >> 32), key.second, index});
		}
	}

	const auto layout =
	    Layout(m_blases.size(), m_pairs.size(), area.walks.size(), top.instance_slots);
	Reserve(m_state, vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
	        layout.size);
	const auto state = m_state->BufferDeviceAddress();

	// One upload: the parameters, then the tables they point to.
	std::vector<std::byte> upload(sizeof(Parameters));
	const auto             append = [&](const auto& table) {
		upload.resize(Common::AlignUp(upload.size(), 16));
		const auto offset = upload.size();
		const auto bytes  = std::as_bytes(std::span(table));
		upload.insert(upload.end(), bytes.begin(), bytes.end());
		return offset;
	};
	const auto blases_offset          = append(blases);
	const auto pairs_offset           = append(pairs);
	const auto keys_offset            = append(m_keys);
	const auto walks_offset           = append(area.walks);
	const auto triangle_chunks_offset = append(triangle_chunks);
	const auto box_chunks_offset      = append(box_chunks);
	auto&      stream                 = m_buffer_cache.GetUtilityBuffer(MemoryUsage::Stream);
	const auto [mapped, base]         = stream.Map(upload.size(), 16);
	EXIT_IF(mapped == nullptr);
	const auto       table = stream.BufferDeviceAddress() + base;
	const Parameters parameters {
	    .tlas            = tlas,
	    .extended_base   = LibKernel::Memory::kExtendedMemoryBase,
	    .fold            = LibKernel::Memory::kExtendedMemoryBase - LOWER_ADDRESS_SIZE,
	    .blases          = table + blases_offset,
	    .pairs           = table + pairs_offset,
	    .keys            = table + keys_offset,
	    .walks           = table + walks_offset,
	    .triangle_chunks = table + triangle_chunks_offset,
	    .box_chunks      = table + box_chunks_offset,
	    .instances       = top.instances->BufferDeviceAddress(),
	    .context         = state,
	    .blas_states     = state + sizeof(Context),
	    .pair_states     = state + layout.pair_states,
	    .walk_results    = state + layout.walk_results,
	    .missing         = state + layout.missing,
	    .instance_slots  = top.instance_slots,
	    .key_count       = static_cast<uint32_t>(m_keys.size()),
	    .page_bits       = BufferCache::CACHING_PAGEBITS,
	};
	std::memcpy(upload.data(), &parameters, sizeof(parameters));
	std::memcpy(mapped, upload.data(), upload.size());
	stream.Commit();

	m_scheduler.EndRendering();
	const auto command = m_scheduler.Current().Handle();
	Barrier(command);
	const Context header {.valid = 1, .structure = top.structure.address};
	command.updateBuffer(m_state->Handle(), 0, sizeof(header), &header);
	command.fillBuffer(m_state->Handle(), sizeof(header), layout.size - sizeof(header), 0);
	Barrier(command);

	const auto*                    page_table = m_buffer_cache.GetBdaPageTableBuffer();
	const auto*                    faults     = m_buffer_cache.GetFaultBuffer();
	const vk::DescriptorBufferInfo infos[] {
	    {page_table->Handle(), 0, page_table->Size()},
	    {faults->Handle(), 0, faults->Size()},
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_layout, 0, writes);
	const auto dispatch = [&](Mode mode, size_t groups) {
		const Push push {table, mode};
		command.pushConstants(m_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
		command.dispatch(static_cast<uint32_t>(groups), 1, 1);
	};
	if (!area.walks.empty()) {
		command.bindPipeline(vk::PipelineBindPoint::eCompute, m_walk_pipeline);
		dispatch(Walk, area.walks.size());
	}
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	dispatch(Triangles, triangle_chunks.size());
	if (!box_chunks.empty()) {
		dispatch(Boxes, box_chunks.size());
	}
	Barrier(command);
	dispatch(Instances, (top.instance_slots + GroupSize - 1) / GroupSize);

	// BLAS builds in one call over disjoint scratch ranges, then the TLAS over the first.
	Reserve(m_scratch, vk::BufferUsageFlagBits::eStorageBuffer,
	        std::max(scratch_size, top.structure.build_scratch) + m_scratch_alignment);
	const auto scratch = Common::AlignUp(m_scratch->BufferDeviceAddress(), m_scratch_alignment);
	for (auto& info: builds) {
		info.scratchData.deviceAddress += scratch;
	}
	if (!builds.empty()) {
		command.buildAccelerationStructuresKHR(static_cast<uint32_t>(builds.size()), builds.data(),
		                                       range_pointers.data());
	}
	Barrier(command);

	const auto instances = InstanceGeometry(parameters.instances);
	vk::AccelerationStructureBuildGeometryInfoKHR info {};
	info.type                      = vk::AccelerationStructureTypeKHR::eTopLevel;
	info.flags                     = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
	info.dstAccelerationStructure  = top.structure.handle;
	info.geometryCount             = 1;
	info.pGeometries               = &instances;
	info.scratchData.deviceAddress = scratch;
	const vk::AccelerationStructureBuildRangeInfoKHR  range {top.instance_slots, 0, 0, 0};
	const vk::AccelerationStructureBuildRangeInfoKHR* range_pointer = &range;
	command.buildAccelerationStructuresKHR(1, &info, &range_pointer);
	Barrier(command);

	if (readback) {
		Reserve(area.buffer, vk::BufferUsageFlagBits::eTransferDst, layout.size,
		        MemoryUsage::Download);
		const vk::BufferCopy copy {0, 0, layout.size};
		command.copyBuffer(m_state->Handle(), area.buffer->Handle(), 1, &copy);
		Barrier(command);
	}
	area.tick           = m_scheduler.CurrentTick();
	area.serial         = serial;
	area.tlas           = tlas;
	area.instance_slots = top.instance_slots;
	area.pairs          = m_pairs.size();
	return state;
}

void RayTracing::RunGarbageCollector() {
	if (++m_frame % EvictFrames != 0) {
		return;
	}
	Collect();
	std::erase_if(m_tlases, [&](auto& entry) {
		auto& top = entry.second;
		if (top.used) {
			top.used = false;
			return false;
		}
		Retire(top.structure);
		Retire(top.instances);
		return true;
	});
	size_t live = 0;
	for (auto& pair: m_pairs) {
		if (!pair.used) {
			Retire(pair.structure);
			Retire(pair.boxes);
			continue;
		}
		pair.used       = false;
		m_pairs[live++] = std::move(pair);
	}
	if (live == m_pairs.size()) {
		return;
	}
	m_pairs.resize(live);

	// BLASes left without pairs go too. The readbacks in flight, with the walks they carry, hold
	// the old indices and go as well.
	std::vector<bool> named(m_blases.size());
	for (const auto& pair: m_pairs) {
		named[pair.blas] = true;
	}
	std::vector<uint32_t> remap(m_blases.size());
	uint32_t              kept = 0;
	m_blas_index.clear();
	for (uint32_t index = 0; index < m_blases.size(); ++index) {
		if (!named[index]) {
			Retire(m_blases[index].triangles);
			continue;
		}
		remap[index] = kept;
		m_blas_index.emplace(m_blases[index].address, kept);
		m_blases[kept++] = std::move(m_blases[index]);
	}
	m_blases.resize(kept);
	m_pair_index.clear();
	for (uint32_t index = 0; index < m_pairs.size(); ++index) {
		auto& pair   = m_pairs[index];
		pair.blas    = remap[pair.blas];
		pair.walking = false;
		m_pair_index.emplace(std::pair {m_blases[pair.blas].address, pair.root}, index);
	}
	m_keys.clear();
	for (auto& area: m_areas) {
		area.tick = 0;
	}
}

} // namespace Libs::Graphics
