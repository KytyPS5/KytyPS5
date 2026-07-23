#include "graphics/host_gpu/gpuTiler.h"

#include "common/assert.h"
#include "common/threads.h"
#include "gpu_tiler_shaders/gpu_tiler_depth_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_prt_3d_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_prt_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_render_target_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard256_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard4_3d_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard4_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard64_3d_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard64_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <vector>

namespace Libs::Graphics {
namespace {

constexpr uint32_t FAMILY_COUNT            = static_cast<uint32_t>(TileBlockFamily::Count);
constexpr uint32_t BYTES_PER_ELEMENT_COUNT = 5;
constexpr uint32_t DIRECTION_COUNT         = 2;
constexpr uint32_t PIPELINE_COUNT = FAMILY_COUNT * BYTES_PER_ELEMENT_COUNT * DIRECTION_COUNT;
static_assert(FAMILY_COUNT == 9);

struct Push {
	uint32_t src_base;
	uint32_t dst_base;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t surface_z;
	uint32_t pitch_bytes;
	uint32_t slice_bytes;
	uint32_t blocks_per_row;
	uint32_t blocks_per_slice;
	uint32_t tail_x;
	uint32_t tail_y;
	uint32_t tail;
};
static_assert(sizeof(Push) == 52);

struct Shader {
	const uint32_t* code;
	size_t          words;
};

constexpr std::array<Shader, FAMILY_COUNT> SHADERS {{
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

struct Dispatch {
	Push     push {};
	uint32_t pipeline_slot = 0;
};

struct Resources {
	vk::DescriptorSetLayout                  descriptor_layout = nullptr;
	vk::PipelineLayout                       pipeline_layout   = nullptr;
	vk::DescriptorPool                       descriptor_pool   = nullptr;
	vk::DescriptorSet                        descriptor_set    = nullptr;
	std::array<vk::Pipeline, PIPELINE_COUNT> pipelines {};
	VulkanBuffer                             staging;
	VulkanBuffer                             linear;
	void*                                    mapped = nullptr;
};

bool CheckedAdd(uint64_t a, uint64_t b, uint64_t& result) {
	return b <= UINT64_MAX - a && (result = a + b, true);
}

bool CheckedMultiply(uint64_t a, uint64_t b, uint64_t& result) {
	return (a == 0 || b <= UINT64_MAX / a) && (result = a * b, true);
}

bool CheckedAddProduct(uint64_t& value, uint64_t count, uint64_t stride) {
	uint64_t bytes = 0;
	return CheckedMultiply(count, stride, bytes) && CheckedAdd(value, bytes, value);
}

bool IsRangeValid(uint64_t offset, uint64_t size, uint64_t capacity) {
	return size != 0 && offset <= capacity && size <= capacity - offset;
}

uint64_t AlignToDword(uint64_t value) {
	return (value + 3u) & ~uint64_t {3};
}

uint32_t GetPipelineSlot(bool to_tiled, TileBlockFamily family, uint32_t bytes_per_element) {
	const uint32_t direction_index    = to_tiled ? 1u : 0u;
	const uint32_t family_index       = static_cast<uint32_t>(family);
	const uint32_t element_size_index = std::countr_zero(bytes_per_element);
	return (direction_index * FAMILY_COUNT + family_index) * BYTES_PER_ELEMENT_COUNT +
	       element_size_index;
}

void Barrier(vk::CommandBuffer command, vk::Buffer buffer, vk::AccessFlags src_access,
             vk::AccessFlags dst_access, vk::PipelineStageFlags src_stage,
             vk::PipelineStageFlags dst_stage) {
	vk::BufferMemoryBarrier barrier {};
	barrier.sType               = vk::StructureType::eBufferMemoryBarrier;
	barrier.srcAccessMask       = src_access;
	barrier.dstAccessMask       = dst_access;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = buffer;
	barrier.size                = VK_WHOLE_SIZE;
	command.pipelineBarrier(src_stage, dst_stage, {}, 0, nullptr, 1, &barrier, 0, nullptr);
}

class TileCompute final {
public:
	explicit TileCompute(GraphicContext& graphics): graphics(graphics) {}
	void Run(bool to_tiled, const void* input, void* output, uint64_t tiled_capacity,
	         uint64_t linear_capacity, std::span<const GpuTileInfo> infos,
	         const GpuTileRecord& record);
	void Release();

private:
	void Prepare(bool to_tiled, uint64_t tiled_capacity, uint64_t linear_capacity,
	             std::span<const GpuTileInfo> infos, std::vector<Dispatch>& dispatches) const;
	void Init();
	void CreatePipelines(std::span<const Dispatch> dispatches);
	void CreatePipeline(uint32_t pipeline_slot);
	void Resize(uint64_t staging_size, uint64_t linear_size);
	void CreateBuffer(uint64_t size, bool mapped, VulkanBuffer& buffer, void** data) const;
	void Execute(bool to_tiled, const void* input, void* output, uint64_t tiled_capacity,
	             uint64_t linear_capacity, std::span<const Dispatch> dispatches,
	             const GpuTileRecord& record);
	void Destroy(Resources& target) const;

	GraphicContext& graphics;
	Resources       resources;
};

Common::Mutex                g_tiler_mutex;
std::unique_ptr<TileCompute> g_tiler;

void TileCompute::Prepare(bool to_tiled, uint64_t tiled_capacity, uint64_t linear_capacity,
                          std::span<const GpuTileInfo> infos,
                          std::vector<Dispatch>&       dispatches) const {
	EXIT_IF(infos.empty() || tiled_capacity == 0 || linear_capacity == 0);
	const auto& limits = graphics.GetPhysicalDeviceProperties().limits;
	EXIT_NOT_IMPLEMENTED(tiled_capacity > UINT32_MAX || linear_capacity > UINT32_MAX ||
	                     AlignToDword(tiled_capacity) > limits.maxStorageBufferRange ||
	                     AlignToDword(linear_capacity) > limits.maxStorageBufferRange);

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
		    !IsRangeValid(info.linear_offset, info.linear_size, linear_capacity) ||
		    !IsRangeValid(info.tiled_offset, info.tiled_size, tiled_capacity) ||
		    (block.block_depth == 1 && info.depth != 1));

		uint64_t pitch_bytes = 0;
		EXIT_NOT_IMPLEMENTED(!CheckedMultiply(info.pitch, info.bytes_per_element, pitch_bytes) ||
		                     pitch_bytes > UINT32_MAX);
		uint64_t slice_bytes = info.linear_slice_stride;
		EXIT_NOT_IMPLEMENTED(slice_bytes == 0 &&
		                     !CheckedMultiply(pitch_bytes, info.height, slice_bytes));
		uint64_t linear_used = 0, minimum_slice = 0;
		EXIT_NOT_IMPLEMENTED(!CheckedMultiply(pitch_bytes, info.height, minimum_slice) ||
		                     (info.depth > 1 && slice_bytes < minimum_slice) ||
		                     !CheckedAddProduct(linear_used, info.depth - 1u, slice_bytes) ||
		                     !CheckedAddProduct(linear_used, info.height - 1u, pitch_bytes) ||
		                     !CheckedAddProduct(linear_used, info.width, info.bytes_per_element) ||
		                     linear_used > info.linear_size || slice_bytes > UINT32_MAX);

		const uint64_t columns =
		    (static_cast<uint64_t>(tiled_width) + block.block_width - 1u) / block.block_width;
		const uint64_t rows =
		    (static_cast<uint64_t>(tiled_height) + block.block_height - 1u) / block.block_height;
		uint64_t blocks_per_slice = 0;
		EXIT_NOT_IMPLEMENTED(!CheckedMultiply(columns, rows, blocks_per_slice) ||
		                     columns > UINT32_MAX || blocks_per_slice > UINT32_MAX ||
		                     rows * block.block_height > UINT32_MAX);
		if (info.tail) {
			const bool supported = info.family != TileBlockFamily::Standard256B;
			EXIT_NOT_IMPLEMENTED(
			    !supported || info.depth > block.block_depth || info.tail_x >= block.block_width ||
			    info.width > block.block_width - info.tail_x || info.tail_y >= block.block_height ||
			    info.height > block.block_height - info.tail_y ||
			    info.tiled_size < block.block_size);
		} else {
			const uint64_t slices =
			    (static_cast<uint64_t>(info.depth) + block.block_depth - 1u) / block.block_depth;
			uint64_t tiled_used = 0;
			EXIT_NOT_IMPLEMENTED(!CheckedMultiply(blocks_per_slice, slices, tiled_used) ||
			                     !CheckedMultiply(tiled_used, block.block_size, tiled_used) ||
			                     tiled_used > info.tiled_size);
		}
		const uint32_t alignment = std::min(info.bytes_per_element, 4u);
		EXIT_NOT_IMPLEMENTED(((info.linear_offset | info.tiled_offset | pitch_bytes | slice_bytes) &
		                      (alignment - 1u)) != 0);

		Dispatch dispatch {};
		dispatch.pipeline_slot = GetPipelineSlot(to_tiled, info.family, info.bytes_per_element);
		dispatch.push.src_base =
		    static_cast<uint32_t>(to_tiled ? info.linear_offset : info.tiled_offset);
		dispatch.push.dst_base =
		    static_cast<uint32_t>(to_tiled ? info.tiled_offset : info.linear_offset);
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
}

void TileCompute::Destroy(Resources& target) const {
	if (target.mapped != nullptr) {
		graphics.UnmapMemory(target.staging.memory);
	}
	if (target.staging.buffer != nullptr) {
		graphics.DeleteBuffer(target.staging);
	}
	if (target.linear.buffer != nullptr) {
		graphics.DeleteBuffer(target.linear);
	}
	for (auto pipeline: target.pipelines) {
		if (pipeline != nullptr) {
			graphics.device.destroyPipeline(pipeline, nullptr);
		}
	}
	if (target.descriptor_pool != nullptr) {
		graphics.device.destroyDescriptorPool(target.descriptor_pool, nullptr);
	}
	if (target.pipeline_layout != nullptr) {
		graphics.device.destroyPipelineLayout(target.pipeline_layout, nullptr);
	}
	if (target.descriptor_layout != nullptr) {
		graphics.device.destroyDescriptorSetLayout(target.descriptor_layout, nullptr);
	}
	target = {};
}

void TileCompute::Init() {
	if (resources.pipeline_layout != nullptr) {
		return;
	}
	std::array<vk::DescriptorSetLayoutBinding, 2> bindings {};
	for (uint32_t i = 0; i < bindings.size(); i++) {
		bindings[i] = {i, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute,
		               nullptr};
	}
	vk::DescriptorSetLayoutCreateInfo descriptor_info {};
	descriptor_info.sType        = vk::StructureType::eDescriptorSetLayoutCreateInfo;
	descriptor_info.bindingCount = static_cast<uint32_t>(bindings.size());
	descriptor_info.pBindings    = bindings.data();
	RequireVulkanSuccess(graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr,
	                                                               &resources.descriptor_layout),
	                     "create GPU tiler descriptor layout");

	vk::PushConstantRange        push_range {vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.sType                  = vk::StructureType::ePipelineLayoutCreateInfo;
	layout_info.setLayoutCount         = 1;
	layout_info.pSetLayouts            = &resources.descriptor_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(
	    graphics.device.createPipelineLayout(&layout_info, nullptr, &resources.pipeline_layout),
	    "create GPU tiler pipeline layout");
	vk::DescriptorPoolSize       pool_size {vk::DescriptorType::eStorageBuffer, 2};
	vk::DescriptorPoolCreateInfo pool_info {};
	pool_info.sType         = vk::StructureType::eDescriptorPoolCreateInfo;
	pool_info.maxSets       = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes    = &pool_size;
	RequireVulkanSuccess(
	    graphics.device.createDescriptorPool(&pool_info, nullptr, &resources.descriptor_pool),
	    "create GPU tiler descriptor pool");
	vk::DescriptorSetAllocateInfo set_info {};
	set_info.sType              = vk::StructureType::eDescriptorSetAllocateInfo;
	set_info.descriptorPool     = resources.descriptor_pool;
	set_info.descriptorSetCount = 1;
	set_info.pSetLayouts        = &resources.descriptor_layout;
	RequireVulkanSuccess(
	    graphics.device.allocateDescriptorSets(&set_info, &resources.descriptor_set),
	    "allocate GPU tiler descriptor set");
}

void TileCompute::CreatePipeline(uint32_t pipeline_slot) {
	const uint32_t element_size_index     = pipeline_slot % BYTES_PER_ELEMENT_COUNT;
	const uint32_t family_direction_index = pipeline_slot / BYTES_PER_ELEMENT_COUNT;
	const uint32_t family_index           = family_direction_index % FAMILY_COUNT;
	const uint32_t direction_index        = family_direction_index / FAMILY_COUNT;
	const uint32_t specialization_values[] {1u << element_size_index, direction_index};
	const vk::SpecializationMapEntry entries[] {{0, 0, 4}, {1, 4, 4}};
	vk::SpecializationInfo           specialization {2, entries, sizeof(specialization_values),
	                                                 specialization_values};
	vk::ShaderModuleCreateInfo       module_info {};
	module_info.sType       = vk::StructureType::eShaderModuleCreateInfo;
	module_info.codeSize    = SHADERS[family_index].words * sizeof(uint32_t);
	module_info.pCode       = SHADERS[family_index].code;
	vk::ShaderModule module = nullptr;
	RequireVulkanSuccess(graphics.device.createShaderModule(&module_info, nullptr, &module),
	                     "create GPU tiler shader module");
	vk::PipelineShaderStageCreateInfo stage {};
	stage.sType               = vk::StructureType::ePipelineShaderStageCreateInfo;
	stage.stage               = vk::ShaderStageFlagBits::eCompute;
	stage.module              = module;
	stage.pName               = "main";
	stage.pSpecializationInfo = &specialization;
	vk::ComputePipelineCreateInfo info {};
	info.sType            = vk::StructureType::eComputePipelineCreateInfo;
	info.stage            = stage;
	info.layout           = resources.pipeline_layout;
	vk::Pipeline pipeline = nullptr;
	const auto   result =
	    graphics.device.createComputePipelines(nullptr, 1, &info, nullptr, &pipeline);
	graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create GPU tiler pipeline");
	resources.pipelines[pipeline_slot] = pipeline;
}

void TileCompute::CreatePipelines(std::span<const Dispatch> dispatches) {
	for (const auto& dispatch: dispatches) {
		if (resources.pipelines[dispatch.pipeline_slot] == nullptr) {
			CreatePipeline(dispatch.pipeline_slot);
		}
	}
}

void TileCompute::CreateBuffer(uint64_t size, bool mapped, VulkanBuffer& buffer,
                               void** data) const {
	buffer.usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
	               vk::BufferUsageFlagBits::eTransferDst;
	buffer.memory.property =
	    mapped
	        ? vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	        : vk::MemoryPropertyFlags(vk::MemoryPropertyFlagBits::eDeviceLocal);
	graphics.CreateBuffer(size, buffer);
	if (mapped) graphics.MapMemory(buffer.memory, *data);
}

void TileCompute::Resize(uint64_t staging_size, uint64_t linear_size) {
	if (resources.staging.buffer_size >= staging_size &&
	    resources.linear.buffer_size >= linear_size) {
		return;
	}
	staging_size = std::max(staging_size, resources.staging.buffer_size);
	linear_size  = std::max(linear_size, resources.linear.buffer_size);
	VulkanBuffer staging {}, linear {};
	void*        mapped = nullptr;
	CreateBuffer(staging_size, true, staging, &mapped);
	CreateBuffer(linear_size, false, linear, nullptr);
	if (resources.mapped != nullptr) graphics.UnmapMemory(resources.staging.memory);
	if (resources.staging.buffer != nullptr) graphics.DeleteBuffer(resources.staging);
	if (resources.linear.buffer != nullptr) graphics.DeleteBuffer(resources.linear);
	resources.staging = staging;
	resources.linear  = linear;
	resources.mapped  = mapped;
}

void TileCompute::Execute(bool to_tiled, const void* input, void* output, uint64_t tiled_capacity,
                          uint64_t linear_capacity, std::span<const Dispatch> dispatches,
                          const GpuTileRecord& record) {
	const uint64_t tiled_size  = AlignToDword(tiled_capacity);
	const uint64_t linear_size = AlignToDword(linear_capacity);
	const uint64_t input_size  = to_tiled ? linear_capacity : tiled_capacity;
	if (input != nullptr) {
		std::memcpy(resources.mapped, input, static_cast<size_t>(input_size));
		std::memset(static_cast<uint8_t*>(resources.mapped) + input_size, 0,
		            static_cast<size_t>(AlignToDword(input_size) - input_size));
	}

	std::array<vk::DescriptorBufferInfo, 2> buffer_info {{
	    {to_tiled ? resources.linear.buffer : resources.staging.buffer, 0,
	     to_tiled ? linear_size : tiled_size},
	    {to_tiled ? resources.staging.buffer : resources.linear.buffer, 0,
	     to_tiled ? tiled_size : linear_size},
	}};
	std::array<vk::WriteDescriptorSet, 2>   writes {};
	for (uint32_t i = 0; i < writes.size(); i++) {
		writes[i].sType           = vk::StructureType::eWriteDescriptorSet;
		writes[i].dstSet          = resources.descriptor_set;
		writes[i].dstBinding      = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[i].pBufferInfo     = &buffer_info[i];
	}
	graphics.device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0,
	                                     nullptr);

	CommandBuffer command;
	command.Begin();
	auto vk_command = command.Handle();
	if (input != nullptr) {
		Barrier(vk_command, resources.staging.buffer, vk::AccessFlagBits::eHostWrite,
		        vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
		        vk::PipelineStageFlagBits::eHost,
		        vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer);
	}
	if (to_tiled && input != nullptr) {
		const vk::BufferCopy copy {0, 0, linear_size};
		vk_command.copyBuffer(resources.staging.buffer, resources.linear.buffer, 1, &copy);
		Barrier(vk_command, resources.linear.buffer, vk::AccessFlagBits::eTransferWrite,
		        vk::AccessFlagBits::eShaderRead, vk::PipelineStageFlagBits::eTransfer,
		        vk::PipelineStageFlagBits::eComputeShader);
		Barrier(vk_command, resources.staging.buffer, vk::AccessFlagBits::eTransferRead,
		        vk::AccessFlagBits::eTransferWrite, vk::PipelineStageFlagBits::eTransfer,
		        vk::PipelineStageFlagBits::eTransfer);
	}
	if (to_tiled && record) {
		record(command, resources.linear);
		Barrier(vk_command, resources.linear.buffer,
		        vk::AccessFlagBits::eTransferWrite | vk::AccessFlagBits::eMemoryWrite,
		        vk::AccessFlagBits::eShaderRead, vk::PipelineStageFlagBits::eAllCommands,
		        vk::PipelineStageFlagBits::eComputeShader);
	}

	const auto output_buffer = to_tiled ? resources.staging.buffer : resources.linear.buffer;
	const auto output_size   = to_tiled ? tiled_size : linear_size;
	vk_command.fillBuffer(output_buffer, 0, output_size, 0);
	Barrier(vk_command, output_buffer, vk::AccessFlagBits::eTransferWrite,
	        vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
	        vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader);
	vk_command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, resources.pipeline_layout, 0, 1,
	                              &resources.descriptor_set, 0, nullptr);
	for (const auto& dispatch: dispatches) {
		vk_command.bindPipeline(vk::PipelineBindPoint::eCompute,
		                        resources.pipelines[dispatch.pipeline_slot]);
		vk_command.pushConstants(resources.pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
		                         sizeof(dispatch.push), &dispatch.push);
		vk_command.dispatch((dispatch.push.width + 7u) / 8u, (dispatch.push.height + 7u) / 8u,
		                    dispatch.push.depth);
	}
	Barrier(vk_command, output_buffer, vk::AccessFlagBits::eShaderWrite,
	        vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eHostRead,
	        vk::PipelineStageFlagBits::eComputeShader,
	        vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eHost);
	if (!to_tiled && record) {
		record(command, resources.linear);
	}
	if (!to_tiled && output != nullptr) {
		const vk::BufferCopy copy {0, 0, linear_size};
		vk_command.copyBuffer(resources.linear.buffer, resources.staging.buffer, 1, &copy);
		Barrier(vk_command, resources.staging.buffer, vk::AccessFlagBits::eTransferWrite,
		        vk::AccessFlagBits::eHostRead, vk::PipelineStageFlagBits::eTransfer,
		        vk::PipelineStageFlagBits::eHost);
	}
	command.End();
	command.Execute();
	command.WaitForFence();
	if (output != nullptr) {
		std::memcpy(output, resources.mapped,
		            static_cast<size_t>(to_tiled ? tiled_capacity : linear_capacity));
	}
}

void TileCompute::Run(bool to_tiled, const void* input, void* output, uint64_t tiled_capacity,
                      uint64_t linear_capacity, std::span<const GpuTileInfo> infos,
                      const GpuTileRecord& record) {
	EXIT_IF((to_tiled && (output == nullptr || (input == nullptr && !record))) ||
	        (!to_tiled && (input == nullptr || (output == nullptr && !record))));
	std::vector<Dispatch> dispatches;
	Prepare(to_tiled, tiled_capacity, linear_capacity, infos, dispatches);
	Init();
	CreatePipelines(dispatches);
	const uint64_t staging_size =
	    std::max(AlignToDword(tiled_capacity), AlignToDword(linear_capacity));
	const uint64_t linear_size = AlignToDword(linear_capacity);
	Resize(staging_size, linear_size);
	Execute(to_tiled, input, output, tiled_capacity, linear_capacity, dispatches, record);
}

void TileCompute::Release() {
	Destroy(resources);
}

} // namespace

void GpuDetile(const void* tiled, void* linear, uint64_t tiled_capacity, uint64_t linear_capacity,
               std::span<const GpuTileInfo> infos, const GpuTileRecord& after) {
	Common::LockGuard lock(g_tiler_mutex);
	if (!g_tiler) {
		g_tiler = std::make_unique<TileCompute>(GetRenderContext().GetGraphics());
	}
	g_tiler->Run(false, tiled, linear, tiled_capacity, linear_capacity, infos, after);
}

void GpuTile(const void* linear, void* tiled, uint64_t tiled_capacity, uint64_t linear_capacity,
             std::span<const GpuTileInfo> infos, const GpuTileRecord& before) {
	Common::LockGuard lock(g_tiler_mutex);
	if (!g_tiler) {
		g_tiler = std::make_unique<TileCompute>(GetRenderContext().GetGraphics());
	}
	g_tiler->Run(true, linear, tiled, tiled_capacity, linear_capacity, infos, before);
}

void GpuTileRelease() {
	Common::LockGuard lock(g_tiler_mutex);
	if (g_tiler) {
		g_tiler->Release();
		g_tiler.reset();
	}
}

} // namespace Libs::Graphics
