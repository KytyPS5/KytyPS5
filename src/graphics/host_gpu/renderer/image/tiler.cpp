#include "graphics/host_gpu/renderer/image/tiler.h"

#include "common/alignment.h"
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
#include "gpu_tiler_shaders/gpu_tiler_color_transform_spv.h"
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
	static_assert(sizeof(Mip) == 48);
	static_assert(offsetof(TilingParams, mips) == 16);
	static_assert(sizeof(TilingParams) == 784);
	static_assert(sizeof(TilingParams) <= UINT16_MAX && PipelineCount <= UINT16_MAX);
	static_assert(sizeof(Tiling) == 16);
	static_assert(sizeof(Conversion) == 32);
	std::array<vk::DescriptorSetLayoutBinding, 3> bindings {};
	for (uint32_t index = 0; index < 2; index++) {
		bindings[index] = {index, vk::DescriptorType::eStorageBuffer, 1,
		                   vk::ShaderStageFlagBits::eCompute, nullptr};
	}
	bindings[2] = {2, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute,
	               nullptr};

	vk::DescriptorSetLayoutCreateInfo descriptor_info {};
	descriptor_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	descriptor_info.bindingCount = static_cast<uint32_t>(bindings.size());
	descriptor_info.pBindings    = bindings.data();
	RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr,
	                                                                 &m_descriptor_layout),
	                     "create TileManager descriptor layout");

	const vk::PushConstantRange  push_range {vk::ShaderStageFlagBits::eCompute, 0,
	                                         sizeof(Conversion)};
	vk::PipelineLayoutCreateInfo layout_info {};
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
	if (m_color_transform != nullptr) {
		m_graphics.device.destroyPipeline(m_color_transform, nullptr);
	}
	if (m_pipeline_layout != nullptr) {
		m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	}
	if (m_descriptor_layout != nullptr) {
		m_graphics.device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
	}
}

TileManager::Tiling TileManager::Prepare(bool tile, uint64_t tiled_capacity,
                                         uint64_t linear_capacity, const ImageInfo& info,
                                         uint32_t levels, uint64_t source_base,
                                         uint64_t target_base) {
	levels = levels != 0 ? levels : info.resources.levels;
	EXIT_IF(levels == 0 || levels > MaxMipLevels || levels > info.resources.levels ||
	        tiled_capacity == 0 || linear_capacity == 0);
	EXIT_NOT_IMPLEMENTED(tiled_capacity > UINT32_MAX || linear_capacity > UINT32_MAX);

	const auto& block     = info.tiling.block;
	const auto  transform = info.GetColorTransform();
	EXIT_NOT_IMPLEMENTED(
	    !info.IsTiled() || info.samples != 1 || Prospero::IsFmaskTextureFormat(info.guest_format) ||
	    block.family >= TileBlockFamily::Count || !std::has_single_bit(block.bytes_per_element) ||
	    block.bytes_per_element > 16 || block.block_width == 0 || block.block_height == 0 ||
	    block.block_depth == 0 || block.block_size == 0 || info.tiling.texel_width == 0 ||
	    info.tiling.texel_height == 0 || info.tiled_slice_stride == 0);
	EXIT_IF((transform == ColorTransform::SwapBgra16 && block.bytes_per_element != 8u) ||
	        (transform == ColorTransform::Reverse10_11_11 && block.bytes_per_element != 4u));
	const auto checked_multiply = [](uint64_t left, uint64_t right, uint64_t& result) {
		return (left == 0 || right <= UINT64_MAX / left) && (result = left * right, true);
	};
	const auto checked_add = [](uint64_t left, uint64_t right, uint64_t& result) {
		return right <= UINT64_MAX - left && (result = left + right, true);
	};
	const auto valid_range = [](uint64_t offset, uint64_t size, uint64_t capacity) {
		return size != 0 && offset <= capacity && size <= capacity - offset;
	};

	// ImageInfo owns the layout. Upload only the header and active mip records.
	TilingParams params;
	params.color_transform = static_cast<uint32_t>(transform);
	for (uint32_t level = 0; level < levels; ++level) {
		const auto& mip    = info.mip_layout[level];
		const auto  extent = info.MipExtent(level);
		const auto  width  = static_cast<uint32_t>(
		    (uint64_t {extent.width} + info.tiling.texel_width - 1u) / info.tiling.texel_width);
		const auto height = static_cast<uint32_t>(
		    (uint64_t {extent.height} + info.tiling.texel_height - 1u) / info.tiling.texel_height);
		const auto depth = extent.depth;
		const bool tail  = level >= info.first_tail_level;
		EXIT_NOT_IMPLEMENTED(width == 0 || height == 0 || depth == 0 || mip.linear_pitch < width ||
		                     mip.linear_size == 0 || mip.size == 0 ||
		                     (!tail && (mip.pitch < width || mip.height < height)) ||
		                     (!info.IsVolume() && mip.size % depth != 0));

		uint64_t pitch_bytes = 0;
		EXIT_NOT_IMPLEMENTED(
		    !checked_multiply(mip.linear_pitch, block.bytes_per_element, pitch_bytes) ||
		    pitch_bytes > UINT32_MAX);
		const uint64_t slice_bytes =
		    info.linear_slice_stride != 0 ? info.linear_slice_stride : mip.linear_size;
		uint64_t minimum_slice = 0;
		uint64_t linear_used = 0;
		uint64_t bytes         = 0;
		uint64_t linear_span   = 0;
		EXIT_NOT_IMPLEMENTED(!checked_multiply(pitch_bytes, height, minimum_slice) ||
		                     (depth > 1 && slice_bytes < minimum_slice) ||
		                     !checked_multiply(height - 1u, pitch_bytes, linear_used) ||
		                     !checked_multiply(width, block.bytes_per_element, bytes) ||
		                     !checked_add(linear_used, bytes, linear_used) ||
		                     linear_used > mip.linear_size || slice_bytes > UINT32_MAX ||
		                     !checked_multiply(depth - 1u, slice_bytes, linear_span) ||
		                     !checked_add(linear_span, mip.linear_size, linear_span) ||
		                     !valid_range(mip.linear_offset, linear_span, linear_capacity));

		const uint64_t columns =
		    (uint64_t {mip.pitch} + block.block_width - 1u) / block.block_width;
		if (tail) {
			EXIT_NOT_IMPLEMENTED(
			    block.family == TileBlockFamily::Standard256B || mip.tail_x >= block.block_width ||
			    width > block.block_width - mip.tail_x || mip.tail_y >= block.block_height ||
			    height > block.block_height - mip.tail_y);
		}
		const uint64_t tiled_plane = info.IsVolume() ? mip.size : mip.size / depth;
		const uint64_t tiled_slice = info.tiled_slice_stride;
		const uint64_t slices     = (uint64_t {depth} + block.block_depth - 1u) / block.block_depth;
		uint64_t       tiled_span = 0;
		EXIT_NOT_IMPLEMENTED(columns > UINT32_MAX || tiled_slice > UINT32_MAX ||
		                     (slices > 1 && tiled_slice < tiled_plane) ||
		                     !checked_multiply(slices - 1u, tiled_slice, tiled_span) ||
		                     !checked_add(tiled_span, tiled_plane, tiled_span) ||
		                     !valid_range(mip.offset, tiled_span, tiled_capacity));

		const uint32_t alignment = std::min(block.bytes_per_element, 4u);
		EXIT_NOT_IMPLEMENTED(
		    ((mip.linear_offset | mip.offset | pitch_bytes | slice_bytes | tiled_slice) &
		     (alignment - 1u)) != 0);
		uint64_t src = 0, dst = 0, texels = 0;
		EXIT_NOT_IMPLEMENTED(
		    !checked_add(source_base, tile ? mip.linear_offset : mip.offset, src) ||
		    !checked_add(target_base, tile ? mip.offset : mip.linear_offset, dst) ||
		    src > UINT32_MAX || dst > UINT32_MAX || !checked_multiply(width, height, texels) ||
		    !checked_multiply(texels, depth, texels) || texels > UINT32_MAX - params.num_texels);
		params.mips[level] = {static_cast<uint32_t>(src),
		                      static_cast<uint32_t>(dst),
		                      width,
		                      height,
		                      static_cast<uint32_t>(pitch_bytes),
		                      static_cast<uint32_t>(slice_bytes),
		                      static_cast<uint32_t>(tiled_slice),
		                      static_cast<uint32_t>(columns),
		                      mip.surface_z,
		                      tail ? mip.tail_x : 0u,
		                      tail ? mip.tail_y : 0u,
		                      static_cast<uint32_t>(texels)};
		params.num_texels += static_cast<uint32_t>(texels);
	}

	const auto& limits = m_graphics.GetPhysicalDeviceProperties().limits;
	const auto  params_size =
	    static_cast<uint16_t>(offsetof(TilingParams, mips) + levels * sizeof(Mip));
	const auto offset = m_stream_buffer.Copy(
	    &params, params_size,
	    std::max<uint64_t>(limits.minUniformBufferOffsetAlignment, alignof(TilingParams)));
	const auto slot =
	    ((tile ? FamilyCount : 0u) + static_cast<uint32_t>(block.family)) * BytesPerElementCount +
	    std::countr_zero(block.bytes_per_element);
	return {offset, params.num_texels, params_size, static_cast<uint16_t>(slot)};
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
	const auto module =
	    CompileSPV({shaders[family_index].code, shaders[family_index].words}, m_graphics.device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage               = vk::ShaderStageFlagBits::eCompute;
	stage.module              = module;
	stage.pName               = "main";
	stage.pSpecializationInfo = &specialization;
	vk::ComputePipelineCreateInfo create {};
	create.stage  = stage;
	create.layout = m_pipeline_layout;
	const auto result =
	    m_graphics.device.createComputePipelines(nullptr, 1, &create, nullptr, &m_pipelines[slot]);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create TileManager pipeline");
	return m_pipelines[slot];
}

void TileManager::Record(vk::Buffer source, uint64_t source_offset, uint64_t source_capacity,
                         vk::Buffer target, uint64_t target_offset, uint64_t target_capacity,
                         const Tiling& tiling, bool clear_target) {
	const auto&    limits = m_graphics.GetPhysicalDeviceProperties().limits;
	const uint64_t descriptor_alignment =
	    std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const uint64_t source_descriptor_offset = Common::AlignDown(source_offset, descriptor_alignment);
	const uint64_t target_descriptor_offset = Common::AlignDown(target_offset, descriptor_alignment);
	const uint64_t source_base              = source_offset - source_descriptor_offset;
	const uint64_t target_base              = target_offset - target_descriptor_offset;
	const uint64_t source_range             = Common::AlignUp(source_base + source_capacity, 4);
	const uint64_t target_range             = Common::AlignUp(target_base + target_capacity, 4);
	EXIT_NOT_IMPLEMENTED(source_range > limits.maxStorageBufferRange ||
	                     target_range > limits.maxStorageBufferRange || target_offset % 4 != 0 ||
	                     target_capacity % 4 != 0);

	m_scheduler.EndRendering();
	auto                    command = m_scheduler.Current().Handle();
	vk::BufferMemoryBarrier barriers[3] {};
	barriers[0].srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eHostWrite;
	barriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].buffer              = source;
	barriers[0].offset              = source_offset;
	barriers[0].size                = source_capacity;
	barriers[1]                     = barriers[0];
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
	barriers[2].offset        = tiling.params_offset;
	barriers[2].size          = tiling.params_size;
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
	const vk::DescriptorBufferInfo params_info {m_stream_buffer.Handle(), tiling.params_offset,
	                                            tiling.params_size};
	const vk::DescriptorBufferInfo infos[] {source_info, target_info, params_info};
	std::array<vk::WriteDescriptorSet, 3> writes {};
	for (uint32_t index = 0; index < writes.size(); index++) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType =
		    index == 2 ? vk::DescriptorType::eUniformBuffer : vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo = &infos[index];
	}
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());
	command.bindPipeline(vk::PipelineBindPoint::eCompute, GetPipeline(tiling.pipeline_slot));
	const uint64_t groups   = (uint64_t {tiling.num_texels} + 63u) / 64u;
	const uint32_t groups_y = static_cast<uint32_t>(
	    (groups + limits.maxComputeWorkGroupCount[0] - 1u) / limits.maxComputeWorkGroupCount[0]);
	const uint32_t groups_x = static_cast<uint32_t>((groups + groups_y - 1u) / groups_y);
	EXIT_NOT_IMPLEMENTED(groups_y > limits.maxComputeWorkGroupCount[1]);
	command.dispatch(groups_x, groups_y, 1);

	barriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	barriers[1].dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eMemoryRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eAllCommands, {}, 0, nullptr, 1,
	                        &barriers[1], 0, nullptr);
}

TileManager::Result TileManager::Detile(vk::Buffer tiled, uint64_t tiled_offset,
                                        const ImageInfo& info) {
	if (!info.IsTiled()) {
		const Result source {tiled, tiled_offset, info.data.size};
		return info.GetColorTransform() == ColorTransform::None
		           ? source
		           : TransformColor(source, info.GetColorTransform(), true);
	}
	const auto&    limits = m_graphics.GetPhysicalDeviceProperties().limits;
	const uint64_t descriptor_alignment =
	    std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const uint64_t source_base     = tiled_offset & (descriptor_alignment - 1);
	const auto     linear_capacity = info.LinearSize();
	const auto tiling  = Prepare(false, info.data.size, linear_capacity, info, 0, source_base, 0);
	auto scratch = GetScratchBuffer(linear_capacity, tiled);
	Record(tiled, tiled_offset, info.data.size, scratch.buffer, 0, scratch.size, tiling, true);
	return {scratch.buffer, 0, linear_capacity};
}

void TileManager::Tile(vk::Buffer linear, uint64_t linear_offset, uint64_t linear_capacity,
                       vk::Buffer tiled, uint64_t tiled_offset, uint64_t tiled_capacity,
                       const ImageInfo& info, uint32_t levels) {
	const auto&    limits = m_graphics.GetPhysicalDeviceProperties().limits;
	const uint64_t descriptor_alignment =
	    std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const uint64_t source_base = linear_offset & (descriptor_alignment - 1);
	const uint64_t target_base = tiled_offset & (descriptor_alignment - 1);
	const auto     tiling =
	    Prepare(true, tiled_capacity, linear_capacity, info, levels, source_base, target_base);
	Record(linear, linear_offset, linear_capacity, tiled, tiled_offset, tiled_capacity, tiling,
	       false);
}

void TileManager::TileImage(Image& image, std::span<const vk::BufferImageCopy> regions,
                            vk::Buffer tiled, uint64_t tiled_offset, uint64_t tiled_capacity,
                            uint32_t levels) {
	EXIT_IF(regions.empty());
	const auto&    limits = m_graphics.GetPhysicalDeviceProperties().limits;
	const uint64_t descriptor_alignment =
	    std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const uint64_t target_base     = tiled_offset & (descriptor_alignment - 1);
	const auto     linear_capacity = image.info.LinearSize(levels);
	// Reserve stream parameters before recording the image download.
	const auto tiling =
	    Prepare(true, tiled_capacity, linear_capacity, image.info, levels, 0, target_base);
	auto linear = GetScratchBuffer(linear_capacity, tiled);
	image.Download(regions, linear.buffer, 0, linear.size);
	Record(linear.buffer, 0, linear_capacity, tiled, tiled_offset, tiled_capacity, tiling, false);
}

TileManager::Result TileManager::GetScratchBuffer(uint64_t size, vk::Buffer input) {
	EXIT_IF(size == 0);
	size = Common::AlignUp(size, 4);
	auto& buffer = m_scratch[m_scratch[0] && m_scratch[0]->Handle() == input ? 1 : 0];
	if (!buffer || buffer->Size() < size) {
		if (buffer) {
			m_scheduler.DeferOperation([old = std::move(buffer)]() mutable { old.reset(); });
		}
		buffer = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
		    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
		        vk::BufferUsageFlagBits::eTransferDst, size);
	}
	return {buffer->Handle(), 0, size};
}

TileManager::StorageBinding TileManager::BindStorage(Result buffer, uint64_t size) const {
	const auto& limits            = m_graphics.GetPhysicalDeviceProperties().limits;
	const auto  alignment         = std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const auto  descriptor_offset = Common::AlignDown(buffer.offset, alignment);
	const auto  base              = buffer.offset - descriptor_offset;
	EXIT_IF(buffer.buffer == nullptr || size == 0 || buffer.size < size || base > UINT32_MAX ||
	        size > UINT64_MAX - base || base + size > UINT64_MAX - 3);
	const auto range = Common::AlignUp(base + size, 4);
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
		const auto module = CompileSPV({code, words}, m_graphics.device);
		vk::PipelineShaderStageCreateInfo stage {};
		stage.stage               = vk::ShaderStageFlagBits::eCompute;
		stage.module              = module;
		stage.pName               = "main";
		stage.pSpecializationInfo = &specialization;
		vk::ComputePipelineCreateInfo create {};
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
	const auto source_barrier_size = Common::AlignUp(source_required, 4);
	const auto target_barrier_size = Common::AlignUp(target_required, 4);
	EXIT_IF(source.size < source_barrier_size || target.size < target_barrier_size);

	m_scheduler.EndRendering();
	auto                    command = m_scheduler.Current().Handle();
	vk::BufferMemoryBarrier barriers[2] {};
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].buffer              = source.buffer;
	barriers[0].offset              = source.offset;
	barriers[0].size                = source_barrier_size;
	barriers[0].srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eHostWrite |
	                            vk::AccessFlagBits::eTransferWrite |
	                            vk::AccessFlagBits::eShaderWrite;
	barriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;
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
				writes[index].dstBinding      = index;
				writes[index].descriptorCount = 1;
				writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
				writes[index].pBufferInfo     = &infos[index];
			}
			command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
			                             static_cast<uint32_t>(writes.size()), writes.data());
			Conversion push {};
			push.src_base    = source_binding.base;
			push.dst_base    = target_binding.base;
			push.width       = layout.width;
			push.height      = rows;
			push.source_pitch = static_cast<uint32_t>(layout.source_row_stride);
			push.target_pitch = static_cast<uint32_t>(layout.target_row_stride);
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

void TileManager::TransformColor(Result input, Result output, ColorTransform transform,
                                 bool to_host) {
	uint32_t element_bytes = 0;
	switch (transform) {
		case ColorTransform::SwapBgra16: element_bytes = 8; break;
		case ColorTransform::Reverse10_11_11: element_bytes = 4; break;
		default: EXIT("invalid color transform\n");
	}
	EXIT_IF(input.size == 0 || input.size % element_bytes != 0 ||
	        input.size / element_bytes > UINT32_MAX ||
	        output.size < input.size);
	const auto pixels = static_cast<uint32_t>(input.size / element_bytes);
	if (m_color_transform == nullptr) {
		const auto module = CompileSPV(GPU_TILER_COLOR_TRANSFORM_SPV, m_graphics.device);
		vk::PipelineShaderStageCreateInfo stage {};
		stage.stage  = vk::ShaderStageFlagBits::eCompute;
		stage.module = module;
		stage.pName  = "main";
		vk::ComputePipelineCreateInfo create {};
		create.stage  = stage;
		create.layout = m_pipeline_layout;
		const auto result =
		    m_graphics.device.createComputePipelines(nullptr, 1, &create, nullptr, &m_color_transform);
		m_graphics.device.destroyShaderModule(module, nullptr);
		RequireVulkanSuccess(result, "create color transform pipeline");
	}
	const auto input_binding  = BindStorage(input, input.size);
	const auto output_binding = BindStorage(output, input.size);

	const vk::DescriptorBufferInfo infos[] {
	    input_binding.info,
	    output_binding.info,
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); index++) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	vk::BufferMemoryBarrier barriers[2] {};
	for (uint32_t index = 0; index < 2; index++) {
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
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_color_transform);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());
	Conversion push {};
	push.src_base        = input_binding.base;
	push.dst_base        = output_binding.base;
	push.width           = pixels;
	push.color_transform = static_cast<uint32_t>(transform);
	push.to_host         = to_host;
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
	                      &push);
	command.dispatch((pixels + 63u) / 64u, 1, 1);
	barriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	barriers[1].dstAccessMask = vk::AccessFlagBits::eTransferRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &barriers[1],
	                        0, nullptr);
}

TileManager::Result TileManager::TransformColor(Result input, ColorTransform transform,
                                                bool to_host) {
	auto result = GetScratchBuffer(input.size, input.buffer);
	TransformColor(input, result, transform, to_host);
	return result;
}

} // namespace Libs::Graphics
