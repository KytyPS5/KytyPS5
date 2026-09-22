#pragma once

#include "graphics/host_gpu/renderer/perVertexTransform.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include <cstddef>
#include <map>
#include <span>
#include <vector>

namespace Libs::Graphics {
bool PerVertexPrototypeEnabled();
bool HasPerVertexPrototypeInput(std::span<const uint32_t> words);
bool DecodePerVertexPrototypeAttribute(Prospero::BufferFormat   format,
                                       std::span<const uint8_t> bytes,
                                       std::span<uint32_t, 4>   components);
void RememberPerVertexPrototypeShader(ShaderType stage, const ShaderProgram& program,
                                      std::span<const uint32_t> words);

struct PerVertexPrototypePrograms {
	vk::ShaderModule                capture;
	PipelineCache::GraphicsPrograms graphics;
	vk::DescriptorSetLayout         extra_layout;
	PipelineCache::Pipeline         capture_pipeline;
	PerVertexLayout                 layout;
};

struct PerVertexUnpackPipeline {
	vk::ShaderModule        module;
	vk::DescriptorSetLayout set_layout;
	vk::PipelineLayout      pipeline_layout;
	vk::Pipeline            pipeline;
};

struct UnpackPushConstants {
	uint32_t total_invocations; // word 0
	uint32_t index_count;       // word 1
	uint32_t first_vertex;      // word 2
	int32_t  vertex_offset;     // word 3 (signed)
	uint32_t first_instance;    // word 4
	uint32_t vertex_stride;     // word 5
	uint32_t num_records;       // word 6: strict logical guest buffer bound
	uint32_t packed_flags; // word 7: [15:0] index_base_offset, [16] is_indexed, [18:17] index_type,
	                       // [22:19] num_attributes
	struct Attr {
		uint32_t meta;       // [15:0] byte offset, [19:17] component count, [22:20] format kind
		uint32_t bit_counts; // [cnt0, cnt1, cnt2, cnt3]
	} attrs[12];             // words 8..31 (24 uints)
};
static_assert(sizeof(UnpackPushConstants::Attr) == 8);
static_assert(offsetof(UnpackPushConstants, attrs) == 32);
static_assert(offsetof(UnpackPushConstants::Attr, bit_counts) == 4);
static_assert(sizeof(UnpackPushConstants) == 128);

const PerVertexPrototypePrograms*
GetPerVertexPrototypePrograms(GraphicContext&                        graphics,
                              const PipelineCache::GraphicsPrograms& programs,
                              const ShaderVertexInputInfo& vs_info, vk::PipelineCache driver_cache);
const PerVertexUnpackPipeline* GetPerVertexUnpackPipeline(GraphicContext&   graphics,
                                                          vk::PipelineCache driver_cache);

} // namespace Libs::Graphics
