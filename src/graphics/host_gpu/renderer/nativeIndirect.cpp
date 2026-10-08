#include "graphics/host_gpu/renderer/nativeIndirect.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "gpu_tiler_shaders/gpu_tiler_native_indirect_spv.h"
#include "common/assert.h"
namespace Libs::Graphics {
NativeIndirectPrep::NativeIndirectPrep(GraphicContext& graphics): m_graphics(graphics) {
 const vk::PushConstantRange range {vk::ShaderStageFlagBits::eCompute, 0, 24};
 vk::PipelineLayoutCreateInfo layout {};
 layout.pushConstantRangeCount = 1;
 layout.pPushConstantRanges = &range;
 RequireVulkanSuccess(graphics.device.createPipelineLayout(&layout, nullptr, &m_layout), "native indirect layout");
 const auto module = CompileSPV(GPU_TILER_NATIVE_INDIRECT_SPV, graphics.device);
 vk::ComputePipelineCreateInfo info {};
 info.layout = m_layout;
 info.stage.stage = vk::ShaderStageFlagBits::eCompute;
 info.stage.module = module;
 info.stage.pName = "main";
 const auto result = graphics.device.createComputePipelines(nullptr, 1, &info, nullptr, &m_pipeline);
 graphics.device.destroyShaderModule(module, nullptr);
 RequireVulkanSuccess(result, "native indirect count preparation");
}
NativeIndirectPrep::~NativeIndirectPrep() {
 m_graphics.device.destroyPipeline(m_pipeline, nullptr);
 m_graphics.device.destroyPipelineLayout(m_layout, nullptr);
}
void NativeIndirectPrep::Record(CommandBuffer& buffer, const NativeIndirectDraw& source) {
 const auto command = buffer.Handle();
 vk::MemoryBarrier ready {};
 ready.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
 ready.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
 command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
                         {}, 1, &ready, 0, nullptr, 0, nullptr);
 struct Params { uint64_t address; uint32_t stride, maximum, limit, reserved; };
 static_assert(sizeof(Params) == 24);
 const Params params {source.snapshot->BufferDeviceAddress(), source.stride / 4,
                      source.max_count, source.index_count_limit, 0};
 command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
 command.pushConstants(m_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
 command.dispatch((source.max_count + 63u) / 64u, 1, 1);
}
}