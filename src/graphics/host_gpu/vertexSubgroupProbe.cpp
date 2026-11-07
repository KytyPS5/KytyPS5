#include "graphics/host_gpu/vertexSubgroupProbe.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"

#include <array>
#include <cstring>
#include <spirv/unified1/GLSL.std.450.h>

namespace Libs::Graphics {

namespace {

constexpr uint32_t ProbeVertices    = 100;
constexpr uint64_t ProbeBufferBytes = uint64_t {ProbeVertices} * sizeof(VertexSubgroupSample);

// Everything the probe creates, destroyed in reverse order however the probe ends.
struct ProbeObjects {
	vk::Device              device = nullptr;
	vk::Buffer              buffer = nullptr;
	vk::DeviceMemory        memory = nullptr;
	vk::DescriptorSetLayout set_layout = nullptr;
	vk::DescriptorPool      pool = nullptr;
	vk::PipelineLayout      pipeline_layout = nullptr;
	vk::ShaderModule        module = nullptr;
	vk::Pipeline            pipeline = nullptr;
	vk::CommandPool         command_pool = nullptr;
	vk::Fence               fence = nullptr;

	ProbeObjects(const ProbeObjects&)            = delete;
	ProbeObjects& operator=(const ProbeObjects&) = delete;
	explicit ProbeObjects(vk::Device device_): device(device_) {}
	~ProbeObjects() {
		device.destroyFence(fence, nullptr);
		device.destroyCommandPool(command_pool, nullptr);
		device.destroyPipeline(pipeline, nullptr);
		device.destroyShaderModule(module, nullptr);
		device.destroyPipelineLayout(pipeline_layout, nullptr);
		device.destroyDescriptorPool(pool, nullptr);
		device.destroyDescriptorSetLayout(set_layout, nullptr);
		device.freeMemory(memory, nullptr);
		device.destroyBuffer(buffer, nullptr);
	}
};

bool HostVisibleMemoryType(const vk::PhysicalDeviceMemoryProperties& properties, uint32_t bits,
                           uint32_t& index) {
	const auto wanted =
	    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
		if ((bits & (1u << i)) != 0u && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
			index = i;
			return true;
		}
	}
	return false;
}

bool CreatePipeline(ProbeObjects& objects) {
	const auto device = objects.device;
	vk::DescriptorSetLayoutBinding binding {};
	binding.binding         = 0;
	binding.descriptorType  = vk::DescriptorType::eStorageBuffer;
	binding.descriptorCount = 1;
	binding.stageFlags      = vk::ShaderStageFlagBits::eVertex;
	vk::DescriptorSetLayoutCreateInfo set_layout_info {};
	set_layout_info.bindingCount = 1;
	set_layout_info.pBindings    = &binding;
	if (device.createDescriptorSetLayout(&set_layout_info, nullptr, &objects.set_layout) !=
	    vk::Result::eSuccess) {
		return false;
	}
	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts    = &objects.set_layout;
	if (device.createPipelineLayout(&layout_info, nullptr, &objects.pipeline_layout) !=
	    vk::Result::eSuccess) {
		return false;
	}
	const auto              code = VertexSubgroupProbeSpirv();
	vk::ShaderModuleCreateInfo module_info {};
	module_info.codeSize = code.size() * sizeof(uint32_t);
	module_info.pCode    = code.data();
	if (device.createShaderModule(&module_info, nullptr, &objects.module) != vk::Result::eSuccess) {
		return false;
	}
	// Only the vertex stage runs: points, no vertex buffers, rasterization discarded.
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eVertex;
	stage.module = objects.module;
	stage.pName  = "main";
	vk::PipelineVertexInputStateCreateInfo   vertex_input {};
	vk::PipelineInputAssemblyStateCreateInfo assembly {};
	assembly.topology = vk::PrimitiveTopology::ePointList;
	const vk::Viewport viewport {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
	const vk::Rect2D   scissor {{0, 0}, {1, 1}};
	vk::PipelineViewportStateCreateInfo viewport_state {};
	viewport_state.viewportCount = 1;
	viewport_state.pViewports    = &viewport;
	viewport_state.scissorCount  = 1;
	viewport_state.pScissors     = &scissor;
	vk::PipelineRasterizationStateCreateInfo raster {};
	raster.rasterizerDiscardEnable = VK_TRUE;
	raster.lineWidth               = 1.0f;
	vk::PipelineMultisampleStateCreateInfo multisample {};
	multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
	vk::PipelineRenderingCreateInfo rendering {};
	vk::GraphicsPipelineCreateInfo  pipeline_info {};
	pipeline_info.pNext               = &rendering;
	pipeline_info.stageCount          = 1;
	pipeline_info.pStages             = &stage;
	pipeline_info.pVertexInputState   = &vertex_input;
	pipeline_info.pInputAssemblyState = &assembly;
	pipeline_info.pViewportState      = &viewport_state;
	pipeline_info.pRasterizationState = &raster;
	pipeline_info.pMultisampleState   = &multisample;
	pipeline_info.layout              = objects.pipeline_layout;
	return device.createGraphicsPipelines(nullptr, 1, &pipeline_info, nullptr, &objects.pipeline) ==
	       vk::Result::eSuccess;
}

bool CreateBuffer(GraphicContext& graphics, ProbeObjects& objects, void** mapped) {
	const auto           device = objects.device;
	vk::BufferCreateInfo buffer_info {};
	buffer_info.size        = ProbeBufferBytes;
	buffer_info.usage       = vk::BufferUsageFlagBits::eStorageBuffer;
	buffer_info.sharingMode = vk::SharingMode::eExclusive;
	if (device.createBuffer(&buffer_info, nullptr, &objects.buffer) != vk::Result::eSuccess) {
		return false;
	}
	vk::MemoryRequirements requirements {};
	device.getBufferMemoryRequirements(objects.buffer, &requirements);
	vk::MemoryAllocateInfo allocate_info {};
	allocate_info.allocationSize = requirements.size;
	if (!HostVisibleMemoryType(graphics.physical_device_memory_properties,
	                           requirements.memoryTypeBits, allocate_info.memoryTypeIndex) ||
	    device.allocateMemory(&allocate_info, nullptr, &objects.memory) != vk::Result::eSuccess ||
	    device.bindBufferMemory(objects.buffer, objects.memory, 0) != vk::Result::eSuccess ||
	    device.mapMemory(objects.memory, 0, ProbeBufferBytes, {}, mapped) != vk::Result::eSuccess) {
		return false;
	}
	std::memset(*mapped, 0xff, ProbeBufferBytes);
	return true;
}

bool Draw(GraphicContext& graphics, ProbeObjects& objects) {
	const auto device = objects.device;
	const vk::DescriptorPoolSize   pool_size {vk::DescriptorType::eStorageBuffer, 1};
	vk::DescriptorPoolCreateInfo   pool_info {};
	pool_info.maxSets       = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes    = &pool_size;
	vk::DescriptorSet set = nullptr;
	vk::DescriptorSetAllocateInfo set_info {};
	set_info.descriptorSetCount = 1;
	set_info.pSetLayouts        = &objects.set_layout;
	if (device.createDescriptorPool(&pool_info, nullptr, &objects.pool) != vk::Result::eSuccess) {
		return false;
	}
	set_info.descriptorPool = objects.pool;
	if (device.allocateDescriptorSets(&set_info, &set) != vk::Result::eSuccess) {
		return false;
	}
	const vk::DescriptorBufferInfo buffer_info {objects.buffer, 0, ProbeBufferBytes};
	vk::WriteDescriptorSet         write {};
	write.dstSet          = set;
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eStorageBuffer;
	write.pBufferInfo     = &buffer_info;
	device.updateDescriptorSets(1, &write, 0, nullptr);

	vk::CommandPoolCreateInfo command_pool_info {};
	command_pool_info.queueFamilyIndex = graphics.queue_family;
	vk::CommandBuffer             command = nullptr;
	vk::CommandBufferAllocateInfo command_info {};
	command_info.level              = vk::CommandBufferLevel::ePrimary;
	command_info.commandBufferCount = 1;
	const vk::FenceCreateInfo fence_info {};
	if (device.createCommandPool(&command_pool_info, nullptr, &objects.command_pool) !=
	        vk::Result::eSuccess ||
	    device.createFence(&fence_info, nullptr, &objects.fence) != vk::Result::eSuccess) {
		return false;
	}
	command_info.commandPool = objects.command_pool;
	vk::CommandBufferBeginInfo begin_info {};
	begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	if (device.allocateCommandBuffers(&command_info, &command) != vk::Result::eSuccess ||
	    command.begin(&begin_info) != vk::Result::eSuccess) {
		return false;
	}
	vk::RenderingInfo rendering {};
	rendering.renderArea = vk::Rect2D {{0, 0}, {1, 1}};
	rendering.layerCount = 1;
	command.beginRendering(rendering);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, objects.pipeline);
	command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, objects.pipeline_layout, 0, 1,
	                           &set, 0, nullptr);
	command.draw(ProbeVertices, 1, 0, 0);
	command.endRendering();
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask       = vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask       = vk::AccessFlagBits::eHostRead;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = objects.buffer;
	barrier.size                = ProbeBufferBytes;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eVertexShader,
	                        vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &barrier, 0,
	                        nullptr);
	if (command.end() != vk::Result::eSuccess) {
		return false;
	}
	vk::SubmitInfo submit {};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &command;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		if (graphics.queue.submit(1, &submit, objects.fence) != vk::Result::eSuccess) {
			return false;
		}
	}
	constexpr uint64_t timeout_ns = 5'000'000'000ull;
	if (device.waitForFences(1, &objects.fence, VK_TRUE, timeout_ns) == vk::Result::eSuccess) {
		return true;
	}
	// The draw may still run: the objects it uses are destroyed only after the queue is idle, or
	// the device is lost. Without either, nothing may be destroyed.
	Common::LockGuard lock(graphics.queue_mutex);
	const auto        idle = graphics.queue.waitIdle();
	EXIT_IF(idle != vk::Result::eSuccess && idle != vk::Result::eErrorDeviceLost);
	return false;
}

} // namespace

std::vector<uint32_t> VertexSubgroupProbeSpirv() {
	ShaderRecompiler::Spirv::Builder b(0x00010300u);
	b.RequireCapability(spv::CapabilityShader);
	b.RequireCapability(spv::CapabilityGroupNonUniform);
	b.RequireCapability(spv::CapabilityGroupNonUniformBallot);
	b.RequireCapability(spv::CapabilityGroupNonUniformArithmetic);
	b.RequireCapability(spv::CapabilityGroupNonUniformShuffle);
	b.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
	const auto glsl        = b.Import("GLSL.std.450");
	const auto void_type   = b.Type(spv::OpTypeVoid);
	const auto function    = b.Type(spv::OpTypeFunction, void_type);
	const auto uint_type   = b.Type(spv::OpTypeInt, 32u, 0u);
	const auto bool_type   = b.Type(spv::OpTypeBool);
	const auto float_type  = b.Type(spv::OpTypeFloat, 32u);
	const auto vec4_type   = b.Type(spv::OpTypeVector, float_type, 4u);
	const auto uvec4_type  = b.Type(spv::OpTypeVector, uint_type, 4u);
	const auto words_type  = b.DecoratedType(
        spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}}, uint_type);
	const auto block_type = b.DecoratedType(spv::OpTypeStruct,
	                                        {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
	                                         {spv::OpDecorate, {spv::DecorationBlock}}},
	                                        words_type);
	const auto samples = b.DefineGlobalVariable(
	    b.Type(spv::OpTypePointer, spv::StorageClassStorageBuffer, block_type),
	    spv::StorageClassStorageBuffer);
	b.AddAnnotation(spv::OpDecorate, samples, spv::DecorationDescriptorSet, 0u);
	b.AddAnnotation(spv::OpDecorate, samples, spv::DecorationBinding, 0u);
	const auto vertex_index = b.DefineGlobalVariable(
	    b.Type(spv::OpTypePointer, spv::StorageClassInput, uint_type), spv::StorageClassInput);
	b.AddAnnotation(spv::OpDecorate, vertex_index, spv::DecorationBuiltIn,
	                spv::BuiltInVertexIndex);
	const auto position = b.DefineGlobalVariable(
	    b.Type(spv::OpTypePointer, spv::StorageClassOutput, vec4_type), spv::StorageClassOutput);
	b.AddAnnotation(spv::OpDecorate, position, spv::DecorationBuiltIn, spv::BuiltInPosition);
	const auto word_pointer = b.Type(spv::OpTypePointer, spv::StorageClassStorageBuffer, uint_type);
	const auto constant = [&](uint32_t value) { return b.Constant(spv::OpConstant, uint_type, value); };
	const auto subgroup = constant(spv::ScopeSubgroup);
	const auto op = [&](spv::Op opcode, uint32_t type, auto... operands) {
		const auto id = b.AllocateId();
		b.AddFunction(opcode, type, id, operands...);
		return id;
	};

	const auto main = b.AllocateId();
	b.AddFunction(spv::OpFunction, void_type, main, spv::FunctionControlMaskNone, function);
	b.AddFunction(spv::OpLabel, b.AllocateId());
	const auto vertex = op(spv::OpLoad, uint_type, vertex_index);
	const auto lane   = op(spv::OpGroupNonUniformIAdd, uint_type, subgroup,
	                       spv::GroupOperationExclusiveScan, constant(1));
	const auto count  = op(spv::OpGroupNonUniformIAdd, uint_type, subgroup,
	                       spv::GroupOperationReduce, constant(1));
	const auto ballot = op(spv::OpGroupNonUniformBallot, uvec4_type, subgroup,
	                       b.Constant(spv::OpConstantTrue, bool_type));
	const auto ballot_low   = op(spv::OpCompositeExtract, uint_type, ballot, 0u);
	const auto first_lane   = op(spv::OpExtInst, uint_type, glsl, GLSLstd450FindILsb, ballot_low);
	const auto first_vertex = op(spv::OpGroupNonUniformShuffle, uint_type, subgroup, vertex,
	                             first_lane);
	const auto base         = op(spv::OpIMul, uint_type, vertex, constant(4));
	const std::array values {lane, count, first_vertex, ballot_low};
	for (uint32_t word = 0; word < values.size(); word++) {
		const auto index   = op(spv::OpIAdd, uint_type, base, constant(word));
		const auto pointer = op(spv::OpAccessChain, word_pointer, samples, constant(0), index);
		b.AddFunction(spv::OpStore, pointer, values[word]);
	}
	const auto zero = b.Constant(spv::OpConstant, float_type, 0u);
	const auto one  = b.Constant(spv::OpConstant, float_type, 0x3f800000u);
	b.AddFunction(spv::OpStore, position,
	              b.Constant(spv::OpConstantComposite, vec4_type, zero, zero, zero, one));
	b.AddFunction(spv::OpReturn);
	b.AddFunction(spv::OpFunctionEnd);
	b.AddEntryPoint(spv::ExecutionModelVertex, main, "main", {vertex_index, position});
	return b.Build();
}

bool ProbeVertexSubgroups(GraphicContext& graphics) {
	ProbeObjects objects(graphics.device);
	void*        mapped = nullptr;
	if (!CreateBuffer(graphics, objects, &mapped) || !CreatePipeline(objects) ||
	    !Draw(graphics, objects)) {
		return false;
	}
	std::array<VertexSubgroupSample, ProbeVertices> samples {};
	std::memcpy(samples.data(), mapped, ProbeBufferBytes);
	return VertexSubgroupSamplesAreConsistent(samples);
}

} // namespace Libs::Graphics
