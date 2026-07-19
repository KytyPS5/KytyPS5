#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORCACHE_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shaderBindings.h"

#include <map>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

namespace ShaderRecompiler::IR {
struct Program;
}

class CommandBuffer;
struct ShaderStageRuntime;

struct VulkanDescriptorSet {
	vk::DescriptorSet       set     = nullptr;
	vk::DescriptorSetLayout layout  = nullptr;
	int                     pool_id = -1;
};

struct BufferView {
	VulkanBuffer*  buffer = nullptr;
	vk::DeviceSize offset = 0;
	vk::DeviceSize range  = VK_WHOLE_SIZE;
};

class DescriptorCache {
public:
	enum class Stage { Unknown, Vertex, Pixel, Compute };

	struct TextureBinding {
		VulkanImage*  image      = nullptr;
		int           view       = VulkanImage::VIEW_DEFAULT;
		vk::ImageView image_view = nullptr;
	};

	enum class TextureVariant : int {
		Float2D = 0,
		Uint2D,
		FloatArray,
		UintArray,
		Float3D,
		Uint3D,
	};

	struct NativeDescriptors {
		std::vector<BufferView>     buffers;
		std::vector<TextureBinding> images;
		std::vector<vk::Sampler>    samplers;
		std::vector<BufferView>     addresses;
		BufferView                  gds;
		BufferView                  flattened_srt;
		BufferView                  user_data;
	};

	DescriptorCache() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	~DescriptorCache() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(DescriptorCache);

	vk::DescriptorSetLayout GetDescriptorSetLayout(Stage                                stage,
	                                               const ShaderRecompiler::IR::Program& program);
	void                    Recycle(VulkanDescriptorSet* set);
	VulkanDescriptorSet*    GetDescriptor(Stage stage, const ShaderRecompiler::IR::Program& program,
	                                      const NativeDescriptors& descriptors);

private:
	struct Pool {
		vk::DescriptorPool pool           = nullptr;
		int                next_free_pool = -1;
	};

	void                 CreatePool(GraphicContext* gctx);
	VulkanDescriptorSet* Allocate(Stage stage, const ShaderRecompiler::IR::Program& program);
	vk::DescriptorSetLayout
	GetDescriptorSetLayoutInternal(GraphicContext* gctx, Stage stage,
	                               const ShaderRecompiler::IR::Program& program);

	Common::Mutex     m_mutex;
	std::vector<Pool> m_pools;
	int               m_first_free_pool = -1;
	std::unordered_map<vk::DescriptorSetLayout, std::vector<VulkanDescriptorSet*>>
	                                                         m_free_sets_by_layout;
	std::map<std::vector<uint32_t>, vk::DescriptorSetLayout> m_descriptor_set_layouts;
};

void BindDescriptors(uint64_t submit_id, CommandBuffer* buffer,
                     vk::PipelineBindPoint pipeline_bind_point, vk::PipelineLayout layout,
                     const ShaderStageRuntime& runtime, vk::ShaderStageFlags vk_stage,
                     DescriptorCache::Stage stage);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORCACHE_H_
