#include "graphics/host_gpu/renderer/image/tiler.h"

#include "common/assert.h"
#include "gpu_tiler_shaders/gpu_tiler_demote_d16_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_depth_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_promote_d16_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_prt_3d_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_prt_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_render_target_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard256_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard4_3d_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard4_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard64_3d_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard64_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_swap_bgra16_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>

namespace Libs::Graphics {

TileManager::TileManager(GraphicContext& graphics, CommandScheduler& scheduler,
                         StreamBuffer& stream_buffer)
    : m_graphics(graphics), m_scheduler(scheduler), m_stream_buffer(stream_buffer) {
	static_assert(FamilyCount == 9);
	static_assert(sizeof(Push) == 52);
	std::array<vk::DescriptorSetLayoutBinding, 3> bindings {};
	for (uint32_t index = 0; index < 2; index++) {
		bindings[index] = {index, vk::DescriptorType::eStorageBuffer, 1,
		                   vk::ShaderStageFlagBits::eCompute, nullptr};
	}
	bindings[2] = {2, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute,
	               nullptr};

	vk::DescriptorSetLayoutCreateInfo descriptor_info {};
	descriptor_info.sType        = vk::StructureType::eDescriptorSetLayoutCreateInfo;
	descriptor_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	descriptor_info.bindingCount = static_cast<uint32_t>(bindings.size());
	descriptor_info.pBindings    = bindings.data();
	RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr,
	                                                                 &m_descriptor_layout),
	                     "create TileManager descriptor layout");

	const vk::PushConstantRange  push_range {vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.sType                  = vk::StructureType::ePipelineLayoutCreateInfo;
	layout_info.setLayoutCount         = 1;
	layout_info.pSetLayouts            = &m_descriptor_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&layout_info, nullptr, &m_pipeline_layout),
	    "create TileManager pipeline layout");
}

TileManager::~TileManager() {
	for (auto pipeline: m_pipelines) {
		if (pipeline != nullptr) {
			m_graphics.device.destroyPipeline(pipeline, nullptr);
		}
	}
	if (m_d16_to_d24 != nullptr) {
		m_graphics.device.destroyPipeline(m_d16_to_d24, nullptr);
	}
	if (m_d16_to_d32 != nullptr) {
		m_graphics.device.destroyPipeline(m_d16_to_d32, nullptr);
	}
	if (m_d24_to_d16 != nullptr) {
		m_graphics.device.destroyPipeline(m_d24_to_d16, nullptr);
	}
	if (m_d32_to_d16 != nullptr) {
		m_graphics.device.destroyPipeline(m_d32_to_d16, nullptr);
	}
	if (m_swap_bgra16 != nullptr) {
		m_graphics.device.destroyPipeline(m_swap_bgra16, nullptr);
	}
	if (m_pipeline_layout != nullptr) {
		m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	}
	if (m_descriptor_layout != nullptr) {
		m_graphics.device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
	}
}

TileManager::Scratch TileManager::AllocateScratch(uint64_t size) {
	EXIT_IF(size == 0);
	vk::BufferCreateInfo create {};
	create.sType = vk::StructureType::eBufferCreateInfo;
	create.size  = size;
	create.usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
	               vk::BufferUsageFlagBits::eTransferDst;
	create.sharingMode = vk::SharingMode::eExclusive;

	VmaAllocationCreateInfo allocate {};
	allocate.usage       = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	VkBuffer      buffer = VK_NULL_HANDLE;
	VmaAllocation memory = nullptr;
	const auto    raw    = static_cast<VkBufferCreateInfo>(create);
	RequireVulkanSuccess(static_cast<vk::Result>(vmaCreateBuffer(
	                         m_graphics.allocator, &raw, &allocate, &buffer, &memory, nullptr)),
	                     "allocate TileManager scratch buffer");
	return {buffer, memory, size};
}

void TileManager::DeferDestroy(Scratch scratch) {
	auto allocator = m_graphics.allocator;
	m_scheduler.DeferOperation(
	    [allocator, scratch] { vmaDestroyBuffer(allocator, scratch.buffer, scratch.allocation); });
}

void TileManager::Prepare(bool tile, uint64_t tiled_capacity, uint64_t linear_capacity,
                          std::span<const GpuTileInfo> infos, uint64_t source_base,
                          uint64_t target_base, std::vector<Dispatch>& dispatches) {
	EXIT_IF(infos.empty() || tiled_capacity == 0 || linear_capacity == 0);
	const auto& limits = m_graphics.GetPhysicalDeviceProperties().limits;
	EXIT_NOT_IMPLEMENTED(tiled_capacity > UINT32_MAX || linear_capacity > UINT32_MAX);

	const auto checked_multiply = [](uint64_t left, uint64_t right, uint64_t& result) {
		return (left == 0 || right <= UINT64_MAX / left) && (result = left * right, true);
	};
	const auto checked_add = [](uint64_t left, uint64_t right, uint64_t& result) {
		return right <= UINT64_MAX - left && (result = left + right, true);
	};
	const auto valid_range = [](uint64_t offset, uint64_t size, uint64_t capacity) {
		return size != 0 && offset <= capacity && size <= capacity - offset;
	};

	dispatches.clear();
	dispatches.reserve(infos.size());
	for (const auto& info: infos) {
		TileBlockLayout block {};
		const uint32_t  tiled_width  = info.tiled_width != 0 ? info.tiled_width : info.pitch;
		const uint32_t  tiled_height = info.tiled_height != 0 ? info.tiled_height : info.height;
		const uint64_t  groups_x     = (static_cast<uint64_t>(info.width) + 7u) / 8u;
		const uint64_t  groups_y     = (static_cast<uint64_t>(info.height) + 7u) / 8u;
		EXIT_NOT_IMPLEMENTED(
		    !TileGetBlockLayout(info.family, info.bytes_per_element, block) || info.width == 0 ||
		    info.height == 0 || info.depth == 0 || info.pitch < info.width ||
		    groups_x > limits.maxComputeWorkGroupCount[0] ||
		    groups_y > limits.maxComputeWorkGroupCount[1] ||
		    info.depth > limits.maxComputeWorkGroupCount[2] ||
		    (!info.tail && (tiled_width < info.width || tiled_height < info.height)) ||
		    !valid_range(info.linear_offset, info.linear_size, linear_capacity) ||
		    !valid_range(info.tiled_offset, info.tiled_size, tiled_capacity) ||
		    (block.block_depth == 1 && info.depth != 1));

		uint64_t pitch_bytes = 0;
		EXIT_NOT_IMPLEMENTED(!checked_multiply(info.pitch, info.bytes_per_element, pitch_bytes) ||
		                     pitch_bytes > UINT32_MAX);
		uint64_t slice_bytes   = info.linear_slice_stride;
		uint64_t minimum_slice = 0;
		EXIT_NOT_IMPLEMENTED(!checked_multiply(pitch_bytes, info.height, minimum_slice));
		if (slice_bytes == 0) {
			slice_bytes = minimum_slice;
		}
		uint64_t linear_used = 0;
		uint64_t bytes       = 0;
		EXIT_NOT_IMPLEMENTED((info.depth > 1 && slice_bytes < minimum_slice) ||
		                     !checked_multiply(info.depth - 1u, slice_bytes, bytes) ||
		                     !checked_add(linear_used, bytes, linear_used) ||
		                     !checked_multiply(info.height - 1u, pitch_bytes, bytes) ||
		                     !checked_add(linear_used, bytes, linear_used) ||
		                     !checked_multiply(info.width, info.bytes_per_element, bytes) ||
		                     !checked_add(linear_used, bytes, linear_used) ||
		                     linear_used > info.linear_size || slice_bytes > UINT32_MAX);

		const uint64_t columns =
		    (static_cast<uint64_t>(tiled_width) + block.block_width - 1u) / block.block_width;
		const uint64_t rows =
		    (static_cast<uint64_t>(tiled_height) + block.block_height - 1u) / block.block_height;
		uint64_t blocks_per_slice = 0;
		EXIT_NOT_IMPLEMENTED(!checked_multiply(columns, rows, blocks_per_slice) ||
		                     columns > UINT32_MAX || blocks_per_slice > UINT32_MAX);
		if (info.tail) {
			EXIT_NOT_IMPLEMENTED(
			    info.family == TileBlockFamily::Standard256B || info.depth > block.block_depth ||
			    info.tail_x >= block.block_width || info.width > block.block_width - info.tail_x ||
			    info.tail_y >= block.block_height ||
			    info.height > block.block_height - info.tail_y ||
			    info.tiled_size < block.block_size);
		} else {
			const uint64_t slices =
			    (static_cast<uint64_t>(info.depth) + block.block_depth - 1u) / block.block_depth;
			uint64_t tiled_used = 0;
			EXIT_NOT_IMPLEMENTED(!checked_multiply(blocks_per_slice, slices, tiled_used) ||
			                     !checked_multiply(tiled_used, block.block_size, tiled_used) ||
			                     tiled_used > info.tiled_size);
		}

		const uint32_t alignment = std::min(info.bytes_per_element, 4u);
		EXIT_NOT_IMPLEMENTED(((info.linear_offset | info.tiled_offset | pitch_bytes | slice_bytes) &
		                      (alignment - 1u)) != 0);
		const uint64_t src = source_base + (tile ? info.linear_offset : info.tiled_offset);
		const uint64_t dst = target_base + (tile ? info.tiled_offset : info.linear_offset);
		EXIT_NOT_IMPLEMENTED(src > UINT32_MAX || dst > UINT32_MAX);

		const uint32_t family_index  = static_cast<uint32_t>(info.family);
		const uint32_t element_index = std::countr_zero(info.bytes_per_element);
		EXIT_NOT_IMPLEMENTED(family_index >= FamilyCount || element_index >= BytesPerElementCount);
		Dispatch dispatch {};
		dispatch.pipeline_slot =
		    ((tile ? FamilyCount : 0u) + family_index) * BytesPerElementCount + element_index;
		dispatch.push.src_base         = static_cast<uint32_t>(src);
		dispatch.push.dst_base         = static_cast<uint32_t>(dst);
		dispatch.push.width            = info.width;
		dispatch.push.height           = info.height;
		dispatch.push.depth            = info.depth;
		dispatch.push.surface_z        = info.surface_z;
		dispatch.push.pitch_bytes      = static_cast<uint32_t>(pitch_bytes);
		dispatch.push.slice_bytes      = static_cast<uint32_t>(slice_bytes);
		dispatch.push.blocks_per_row   = static_cast<uint32_t>(columns);
		dispatch.push.blocks_per_slice = static_cast<uint32_t>(blocks_per_slice);
		dispatch.push.tail_x           = info.tail_x;
		dispatch.push.tail_y           = info.tail_y;
		dispatch.push.tail             = info.tail;
		dispatches.push_back(dispatch);
	}

	const uint64_t uniform_alignment =
	    std::max<uint64_t>(limits.minUniformBufferOffsetAlignment, 1);
	const uint64_t stride = (sizeof(Push) + uniform_alignment - 1) & ~(uniform_alignment - 1);
	EXIT_NOT_IMPLEMENTED(dispatches.size() > UINT64_MAX / stride);
	const uint64_t bytes  = dispatches.size() * stride;
	auto [mapped, offset] = m_stream_buffer.Map(bytes, uniform_alignment);
	EXIT_IF(mapped == nullptr);
	for (size_t index = 0; index < dispatches.size(); index++) {
		std::memcpy(mapped + index * stride, &dispatches[index].push, sizeof(Push));
		dispatches[index].params_offset = offset + index * stride;
	}
	m_stream_buffer.Commit();
}

vk::Pipeline TileManager::GetPipeline(uint32_t slot) {
	EXIT_IF(slot >= m_pipelines.size());
	if (m_pipelines[slot] != nullptr) {
		return m_pipelines[slot];
	}
	struct Shader {
		const uint32_t* code;
		size_t          words;
	};
	static constexpr std::array<Shader, FamilyCount> shaders {{
	    {GPU_TILER_STANDARD256_SPV, std::size(GPU_TILER_STANDARD256_SPV)},
	    {GPU_TILER_STANDARD4_SPV, std::size(GPU_TILER_STANDARD4_SPV)},
	    {GPU_TILER_STANDARD4_3D_SPV, std::size(GPU_TILER_STANDARD4_3D_SPV)},
	    {GPU_TILER_STANDARD64_SPV, std::size(GPU_TILER_STANDARD64_SPV)},
	    {GPU_TILER_STANDARD64_3D_SPV, std::size(GPU_TILER_STANDARD64_3D_SPV)},
	    {GPU_TILER_PRT_SPV, std::size(GPU_TILER_PRT_SPV)},
	    {GPU_TILER_PRT_3D_SPV, std::size(GPU_TILER_PRT_3D_SPV)},
	    {GPU_TILER_RENDER_TARGET_SPV, std::size(GPU_TILER_RENDER_TARGET_SPV)},
	    {GPU_TILER_DEPTH_SPV, std::size(GPU_TILER_DEPTH_SPV)},
	}};
	const uint32_t                                   element_index = slot % BytesPerElementCount;
	const uint32_t                   direction_index = slot / (FamilyCount * BytesPerElementCount);
	const uint32_t                   family_index    = (slot / BytesPerElementCount) % FamilyCount;
	const uint32_t                   values[] {1u << element_index, direction_index};
	const vk::SpecializationMapEntry entries[] {{0, 0, 4}, {1, 4, 4}};
	const vk::SpecializationInfo     specialization {2, entries, sizeof(values), values};
	vk::ShaderModuleCreateInfo       module_info {};
	module_info.sType       = vk::StructureType::eShaderModuleCreateInfo;
	module_info.codeSize    = shaders[family_index].words * sizeof(uint32_t);
	module_info.pCode       = shaders[family_index].code;
	vk::ShaderModule module = nullptr;
	RequireVulkanSuccess(m_graphics.device.createShaderModule(&module_info, nullptr, &module),
	                     "create TileManager shader module");
	vk::PipelineShaderStageCreateInfo stage {};
	stage.sType               = vk::StructureType::ePipelineShaderStageCreateInfo;
	stage.stage               = vk::ShaderStageFlagBits::eCompute;
	stage.module              = module;
	stage.pName               = "main";
	stage.pSpecializationInfo = &specialization;
	vk::ComputePipelineCreateInfo create {};
	create.sType  = vk::StructureType::eComputePipelineCreateInfo;
	create.stage  = stage;
	create.layout = m_pipeline_layout;
	const auto result =
	    m_graphics.device.createComputePipelines(nullptr, 1, &create, nullptr, &m_pipelines[slot]);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create TileManager pipeline");
	return m_pipelines[slot];
}

void TileManager::Record(bool tile, vk::Buffer source, uint64_t source_offset,
                         uint64_t source_capacity, vk::Buffer target, uint64_t target_offset,
                         uint64_t target_capacity, std::span<Dispatch> dispatches,
                         bool clear_target) {
	const auto&    limits = m_graphics.GetPhysicalDeviceProperties().limits;
	const uint64_t descriptor_alignment =
	    std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const uint64_t source_descriptor_offset = source_offset & ~(descriptor_alignment - 1);
	const uint64_t target_descriptor_offset = target_offset & ~(descriptor_alignment - 1);
	const uint64_t source_base              = source_offset - source_descriptor_offset;
	const uint64_t target_base              = target_offset - target_descriptor_offset;
	const uint64_t source_range             = (source_base + source_capacity + 3u) & ~uint64_t {3};
	const uint64_t target_range             = (target_base + target_capacity + 3u) & ~uint64_t {3};
	EXIT_NOT_IMPLEMENTED(source_range > limits.maxStorageBufferRange ||
	                     target_range > limits.maxStorageBufferRange || target_offset % 4 != 0 ||
	                     target_capacity % 4 != 0);

	m_scheduler.EndRendering();
	auto                    command = m_scheduler.Current().Handle();
	vk::BufferMemoryBarrier barriers[3] {};
	barriers[0].sType         = vk::StructureType::eBufferMemoryBarrier;
	barriers[0].srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eHostWrite;
	barriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].buffer              = source;
	barriers[0].offset              = source_offset;
	barriers[0].size                = source_capacity;
	barriers[1]                     = barriers[0];
	barriers[1].srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eHostWrite;
	barriers[1].dstAccessMask =
	    clear_target ? vk::AccessFlagBits::eTransferWrite
	                 : vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	barriers[1].buffer        = target;
	barriers[1].offset        = target_offset;
	barriers[1].size          = target_capacity;
	barriers[2]               = barriers[0];
	barriers[2].srcAccessMask = vk::AccessFlagBits::eHostWrite;
	barriers[2].dstAccessMask = vk::AccessFlagBits::eUniformRead;
	barriers[2].buffer        = m_stream_buffer.Handle();
	barriers[2].offset        = dispatches.front().params_offset;
	barriers[2].size =
	    dispatches.back().params_offset - dispatches.front().params_offset + sizeof(Push);
	command.pipelineBarrier(
	    vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost,
	    vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer, {}, 0,
	    nullptr, 3, barriers, 0, nullptr);
	if (clear_target) {
		command.fillBuffer(target, target_offset, target_capacity, 0);
		barriers[1].srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		barriers[1].dstAccessMask =
		    vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                        vk::PipelineStageFlagBits::eComputeShader, {}, 0, nullptr, 1,
		                        &barriers[1], 0, nullptr);
	}

	const vk::DescriptorBufferInfo source_info {source, source_descriptor_offset, source_range};
	const vk::DescriptorBufferInfo target_info {target, target_descriptor_offset, target_range};
	for (auto& dispatch: dispatches) {
		const vk::DescriptorBufferInfo        params_info {m_stream_buffer.Handle(),
		                                                   dispatch.params_offset, sizeof(Push)};
		const vk::DescriptorBufferInfo        infos[] {source_info, target_info, params_info};
		std::array<vk::WriteDescriptorSet, 3> writes {};
		for (uint32_t index = 0; index < writes.size(); index++) {
			writes[index].sType           = vk::StructureType::eWriteDescriptorSet;
			writes[index].dstBinding      = index;
			writes[index].descriptorCount = 1;
			writes[index].descriptorType  = index == 2 ? vk::DescriptorType::eUniformBuffer
			                                           : vk::DescriptorType::eStorageBuffer;
			writes[index].pBufferInfo     = &infos[index];
		}
		command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
		                             static_cast<uint32_t>(writes.size()), writes.data());
		command.bindPipeline(vk::PipelineBindPoint::eCompute, GetPipeline(dispatch.pipeline_slot));
		command.dispatch((dispatch.push.width + 7u) / 8u, (dispatch.push.height + 7u) / 8u,
		                 dispatch.push.depth);
	}

	barriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	barriers[1].dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eMemoryRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eAllCommands, {}, 0, nullptr, 1,
	                        &barriers[1], 0, nullptr);
}

TileManager::Result TileManager::Detile(vk::Buffer tiled, uint64_t tiled_offset,
                                        uint64_t tiled_capacity, uint64_t linear_capacity,
                                        std::span<const GpuTileInfo> infos) {
	const auto&    limits = m_graphics.GetPhysicalDeviceProperties().limits;
	const uint64_t descriptor_alignment =
	    std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const uint64_t        source_base = tiled_offset & (descriptor_alignment - 1);
	std::vector<Dispatch> dispatches;
	Prepare(false, tiled_capacity, linear_capacity, infos, source_base, 0, dispatches);
	auto scratch = AllocateScratch((linear_capacity + 3u) & ~uint64_t {3});
	DeferDestroy(scratch);
	Record(false, tiled, tiled_offset, tiled_capacity, scratch.buffer, 0, scratch.size, dispatches,
	       true);
	return {scratch.buffer, 0, linear_capacity};
}

void TileManager::Tile(vk::Buffer linear, uint64_t linear_offset, uint64_t linear_capacity,
                       vk::Buffer tiled, uint64_t tiled_offset, uint64_t tiled_capacity,
                       std::span<const GpuTileInfo> infos) {
	const auto&    limits = m_graphics.GetPhysicalDeviceProperties().limits;
	const uint64_t descriptor_alignment =
	    std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const uint64_t        source_base = linear_offset & (descriptor_alignment - 1);
	const uint64_t        target_base = tiled_offset & (descriptor_alignment - 1);
	std::vector<Dispatch> dispatches;
	Prepare(true, tiled_capacity, linear_capacity, infos, source_base, target_base, dispatches);
	Record(true, linear, linear_offset, linear_capacity, tiled, tiled_offset, tiled_capacity,
	       dispatches, false);
}

void TileManager::TileImage(Image& image, std::span<const vk::BufferImageCopy> regions,
                            vk::Buffer tiled, uint64_t tiled_offset, uint64_t tiled_capacity,
                            uint64_t linear_capacity, std::span<const GpuTileInfo> infos,
                            ColorTransform transform) {
	EXIT_IF(regions.empty());
	const auto&    limits = m_graphics.GetPhysicalDeviceProperties().limits;
	const uint64_t descriptor_alignment =
	    std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const uint64_t        target_base = tiled_offset & (descriptor_alignment - 1);
	std::vector<Dispatch> dispatches;
	// Reserve all stream parameters before creating a scheduler-lived scratch dependency:
	// StreamBuffer::Map is allowed to submit the current tick when it wraps.
	Prepare(true, tiled_capacity, linear_capacity, infos, 0, target_base, dispatches);
	auto linear = AllocateScratch((linear_capacity + 3u) & ~uint64_t {3});
	DeferDestroy(linear);
	image.Download(regions, linear.buffer, 0, linear.size);
	Result source {linear.buffer, 0, linear.size};
	if (transform == ColorTransform::SwapBgra16) {
		source = SwapBgra16(source);
	}
	Record(true, source.buffer, source.offset, linear_capacity, tiled, tiled_offset, tiled_capacity,
	       dispatches, false);
}

TileManager::Result TileManager::GetScratchBuffer(uint64_t size) {
	auto scratch = AllocateScratch((size + 3u) & ~uint64_t {3});
	DeferDestroy(scratch);
	return {scratch.buffer, 0, scratch.size};
}

TileManager::StorageBinding TileManager::BindStorage(Result buffer, uint64_t size) const {
	const auto& limits            = m_graphics.GetPhysicalDeviceProperties().limits;
	const auto  alignment         = std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const auto  descriptor_offset = buffer.offset - buffer.offset % alignment;
	const auto  base              = buffer.offset - descriptor_offset;
	EXIT_IF(buffer.buffer == nullptr || size == 0 || buffer.size < size || base > UINT32_MAX ||
	        size > UINT64_MAX - base || base + size > UINT64_MAX - 3);
	const auto range = (base + size + 3) & ~uint64_t {3};
	EXIT_IF(range > limits.maxStorageBufferRange || range > UINT32_MAX);
	return {{buffer.buffer, descriptor_offset, range}, static_cast<uint32_t>(base)};
}

uint32_t TileManager::ConversionRows(uint64_t offset, uint64_t row_stride, uint64_t active,
                                     uint32_t remaining, uint64_t alignment, uint64_t max_range,
                                     uint32_t max_groups) noexcept {
	if (row_stride == 0 || active == 0 || remaining == 0 || alignment == 0 || max_groups == 0) {
		return 0;
	}
	const auto prefix = offset % alignment;
	if (prefix >= max_range || active > max_range - prefix) {
		return 0;
	}
	const auto descriptor_rows = 1 + (max_range - prefix - active) / row_stride;
	return static_cast<uint32_t>(std::min<uint64_t>({remaining, descriptor_rows, max_groups}));
}

void TileManager::ConvertD16(Result source, Result target, D16Direction direction, bool d32,
                             const D16Layout& layout) {
	vk::Pipeline* pipeline_pointer = nullptr;
	if (direction == D16Direction::Promote) {
		pipeline_pointer = d32 ? &m_d16_to_d32 : &m_d16_to_d24;
	} else {
		pipeline_pointer = d32 ? &m_d32_to_d16 : &m_d24_to_d16;
	}
	auto& pipeline = *pipeline_pointer;
	if (pipeline == nullptr) {
		const uint32_t                   value = d32 ? 1u : 0u;
		const vk::SpecializationMapEntry entry {0, 0, sizeof(value)};
		const vk::SpecializationInfo     specialization {1, &entry, sizeof(value), &value};
		const uint32_t*                  code  = nullptr;
		size_t                           words = 0;
		if (direction == D16Direction::Promote) {
			code  = GPU_TILER_PROMOTE_D16_SPV;
			words = std::size(GPU_TILER_PROMOTE_D16_SPV);
		} else {
			code  = GPU_TILER_DEMOTE_D16_SPV;
			words = std::size(GPU_TILER_DEMOTE_D16_SPV);
		}
		vk::ShaderModuleCreateInfo module_info {};
		module_info.sType       = vk::StructureType::eShaderModuleCreateInfo;
		module_info.codeSize    = words * sizeof(uint32_t);
		module_info.pCode       = code;
		vk::ShaderModule module = nullptr;
		RequireVulkanSuccess(m_graphics.device.createShaderModule(&module_info, nullptr, &module),
		                     "create D16 conversion shader module");
		vk::PipelineShaderStageCreateInfo stage {};
		stage.sType               = vk::StructureType::ePipelineShaderStageCreateInfo;
		stage.stage               = vk::ShaderStageFlagBits::eCompute;
		stage.module              = module;
		stage.pName               = "main";
		stage.pSpecializationInfo = &specialization;
		vk::ComputePipelineCreateInfo create {};
		create.sType  = vk::StructureType::eComputePipelineCreateInfo;
		create.stage  = stage;
		create.layout = m_pipeline_layout;
		const auto result =
		    m_graphics.device.createComputePipelines(nullptr, 1, &create, nullptr, &pipeline);
		m_graphics.device.destroyShaderModule(module, nullptr);
		RequireVulkanSuccess(result, "create D16 conversion pipeline");
	}

	const uint64_t source_element =
	    direction == D16Direction::Promote ? sizeof(uint16_t) : sizeof(uint32_t);
	const uint64_t target_element =
	    direction == D16Direction::Promote ? sizeof(uint32_t) : sizeof(uint16_t);
	const uint64_t source_active = static_cast<uint64_t>(layout.width) * source_element;
	const uint64_t target_active = static_cast<uint64_t>(layout.width) * target_element;
	const auto     required      = [](uint32_t height, uint32_t layers, uint64_t row_stride,
	                                  uint64_t slice_stride, uint64_t active) {
		EXIT_IF(height == 0 || layers == 0 || row_stride < active ||
		        (height - 1) > (UINT64_MAX - active) / row_stride);
		const auto slice = static_cast<uint64_t>(height - 1) * row_stride + active;
		EXIT_IF(slice_stride < slice || (layers - 1) > (UINT64_MAX - slice) / slice_stride);
		return static_cast<uint64_t>(layers - 1) * slice_stride + slice;
	};
	EXIT_IF(layout.width == 0 || layout.source_row_stride > UINT32_MAX ||
	        layout.target_row_stride > UINT32_MAX);
	const auto source_required = required(layout.height, layout.layers, layout.source_row_stride,
	                                      layout.source_slice_stride, source_active);
	const auto target_required = required(layout.height, layout.layers, layout.target_row_stride,
	                                      layout.target_slice_stride, target_active);
	EXIT_IF(source_required > UINT64_MAX - 3 || target_required > UINT64_MAX - 3);
	const auto source_barrier_size = (source_required + 3) & ~uint64_t {3};
	const auto target_barrier_size = (target_required + 3) & ~uint64_t {3};
	EXIT_IF(source.size < source_barrier_size || target.size < target_barrier_size);

	m_scheduler.EndRendering();
	auto                    command = m_scheduler.Current().Handle();
	vk::BufferMemoryBarrier barriers[2] {};
	barriers[0].sType               = vk::StructureType::eBufferMemoryBarrier;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].buffer              = source.buffer;
	barriers[0].offset              = source.offset;
	barriers[0].size                = source_barrier_size;
	barriers[0].srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eHostWrite |
	                            vk::AccessFlagBits::eTransferWrite |
	                            vk::AccessFlagBits::eShaderWrite;
	barriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;
	barriers[1].sType         = vk::StructureType::eBufferMemoryBarrier;
	barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].buffer              = target.buffer;
	barriers[1].offset              = target.offset;
	barriers[1].size                = target_barrier_size;
	barriers[1].srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
	                            vk::AccessFlagBits::eHostWrite |
	                            vk::AccessFlagBits::eTransferWrite |
	                            vk::AccessFlagBits::eShaderWrite;
	barriers[1].dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	command.pipelineBarrier(
	    vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost,
	    vk::PipelineStageFlagBits::eComputeShader, {}, 0, nullptr, 2, barriers, 0, nullptr);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
	const auto& limits              = m_graphics.GetPhysicalDeviceProperties().limits;
	const auto descriptor_alignment = std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const auto rows_for = [&](Result buffer, uint64_t relative, uint64_t stride, uint64_t active,
	                          uint32_t remaining) {
		EXIT_IF(relative > buffer.size || buffer.offset > UINT64_MAX - relative);
		const auto offset = buffer.offset + relative;
		return ConversionRows(offset, stride, active, remaining, descriptor_alignment,
		                      limits.maxStorageBufferRange, limits.maxComputeWorkGroupCount[1]);
	};
	const auto groups_x = (static_cast<uint64_t>(layout.width) + 63u) / 64u;
	EXIT_IF(groups_x == 0 || groups_x > limits.maxComputeWorkGroupCount[0]);
	for (uint32_t layer = 0; layer < layout.layers; layer++) {
		for (uint32_t row = 0; row < layout.height;) {
			const auto source_relative =
			    layout.source_slice_stride * layer + layout.source_row_stride * row;
			const auto target_relative =
			    layout.target_slice_stride * layer + layout.target_row_stride * row;
			const auto remaining = layout.height - row;
			const auto rows = std::min(rows_for(source, source_relative, layout.source_row_stride,
			                                    source_active, remaining),
			                           rows_for(target, target_relative, layout.target_row_stride,
			                                    target_active, remaining));
			EXIT_IF(rows == 0);
			const auto source_span =
			    static_cast<uint64_t>(rows - 1) * layout.source_row_stride + source_active;
			const auto target_span =
			    static_cast<uint64_t>(rows - 1) * layout.target_row_stride + target_active;
			const auto source_binding = BindStorage(
			    {source.buffer, source.offset + source_relative, source.size - source_relative},
			    source_span);
			const auto target_binding = BindStorage(
			    {target.buffer, target.offset + target_relative, target.size - target_relative},
			    target_span);
			const vk::DescriptorBufferInfo infos[] {
			    source_binding.info,
			    target_binding.info,
			};
			std::array<vk::WriteDescriptorSet, 2> writes {};
			for (uint32_t index = 0; index < writes.size(); index++) {
				writes[index].sType           = vk::StructureType::eWriteDescriptorSet;
				writes[index].dstBinding      = index;
				writes[index].descriptorCount = 1;
				writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
				writes[index].pBufferInfo     = &infos[index];
			}
			command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
			                             static_cast<uint32_t>(writes.size()), writes.data());
			Push push {};
			push.src_base    = source_binding.base;
			push.dst_base    = target_binding.base;
			push.width       = layout.width;
			push.height      = rows;
			push.pitch_bytes = static_cast<uint32_t>(layout.source_row_stride);
			push.slice_bytes = static_cast<uint32_t>(layout.target_row_stride);
			command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
			                      sizeof(push), &push);
			command.dispatch(static_cast<uint32_t>(groups_x), rows, 1);
			row += rows;
		}
	}
	barriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	barriers[1].dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eMemoryRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eAllCommands, {}, 0, nullptr, 1,
	                        &barriers[1], 0, nullptr);
}

void TileManager::SwapBgra16(Result input, Result output, uint32_t pixels) {
	if (m_swap_bgra16 == nullptr) {
		vk::ShaderModuleCreateInfo module_info {};
		module_info.sType       = vk::StructureType::eShaderModuleCreateInfo;
		module_info.codeSize    = std::size(GPU_TILER_SWAP_BGRA16_SPV) * sizeof(uint32_t);
		module_info.pCode       = GPU_TILER_SWAP_BGRA16_SPV;
		vk::ShaderModule module = nullptr;
		RequireVulkanSuccess(m_graphics.device.createShaderModule(&module_info, nullptr, &module),
		                     "create BGRA16 swap shader module");
		vk::PipelineShaderStageCreateInfo stage {};
		stage.sType  = vk::StructureType::ePipelineShaderStageCreateInfo;
		stage.stage  = vk::ShaderStageFlagBits::eCompute;
		stage.module = module;
		stage.pName  = "main";
		vk::ComputePipelineCreateInfo create {};
		create.sType  = vk::StructureType::eComputePipelineCreateInfo;
		create.stage  = stage;
		create.layout = m_pipeline_layout;
		const auto result =
		    m_graphics.device.createComputePipelines(nullptr, 1, &create, nullptr, &m_swap_bgra16);
		m_graphics.device.destroyShaderModule(module, nullptr);
		RequireVulkanSuccess(result, "create BGRA16 swap pipeline");
	}
	const uint64_t bytes = static_cast<uint64_t>(pixels) * 8u;
	EXIT_IF(pixels == 0);
	const auto input_binding  = BindStorage(input, bytes);
	const auto output_binding = BindStorage(output, bytes);

	const vk::DescriptorBufferInfo infos[] {
	    input_binding.info,
	    output_binding.info,
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); index++) {
		writes[index].sType           = vk::StructureType::eWriteDescriptorSet;
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	vk::BufferMemoryBarrier barriers[2] {};
	for (uint32_t index = 0; index < 2; index++) {
		barriers[index].sType               = vk::StructureType::eBufferMemoryBarrier;
		barriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[index].buffer              = infos[index].buffer;
		barriers[index].offset              = infos[index].offset;
		barriers[index].size                = infos[index].range;
	}
	barriers[0].srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eHostWrite |
	                            vk::AccessFlagBits::eShaderWrite;
	barriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;
	barriers[1].srcAccessMask = vk::AccessFlagBits::eMemoryRead;
	barriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;
	m_scheduler.EndRendering();
	auto command = m_scheduler.Current().Handle();
	command.pipelineBarrier(
	    vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost,
	    vk::PipelineStageFlagBits::eComputeShader, {}, 0, nullptr, 2, barriers, 0, nullptr);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_swap_bgra16);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());
	Push push {};
	push.src_base = input_binding.base;
	push.dst_base = output_binding.base;
	push.width    = pixels;
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
	                      &push);
	command.dispatch((pixels + 63u) / 64u, 1, 1);
	barriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	barriers[1].dstAccessMask = vk::AccessFlagBits::eTransferRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &barriers[1],
	                        0, nullptr);
}

TileManager::Result TileManager::SwapBgra16(Result input) {
	EXIT_NOT_IMPLEMENTED(input.size == 0 || input.size % 8u != 0 || input.size / 8u > UINT32_MAX);
	auto output = AllocateScratch(input.size);
	DeferDestroy(output);
	Result result {output.buffer, 0, output.size};
	SwapBgra16(input, result, static_cast<uint32_t>(input.size / 8u));
	return result;
}

void TileManager::SwapBgra16(Result input, Result output) {
	EXIT_NOT_IMPLEMENTED(input.size == 0 || input.size % 8u != 0 || input.size / 8u > UINT32_MAX ||
	                     output.size < input.size);
	SwapBgra16(input, output, static_cast<uint32_t>(input.size / 8u));
}

} // namespace Libs::Graphics
