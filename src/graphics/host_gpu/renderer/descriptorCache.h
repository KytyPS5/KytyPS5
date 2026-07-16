#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORCACHE_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/shader/shaderBindings.h"

#include <map>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace Libs::Graphics {

namespace ShaderRecompiler::IR {
struct Program;
}

class CommandBuffer;
struct ShaderStageRuntime;

struct VulkanDescriptor {
	VkDescriptorSet descriptor_set = nullptr;
};

struct VulkanDescriptorSet {
	VkDescriptorSet       set     = nullptr;
	VkDescriptorSetLayout layout  = nullptr;
	int                   pool_id = -1;
};

struct BufferView {
	VulkanBuffer* buffer = nullptr;
	VkDeviceSize  offset = 0;
	VkDeviceSize  range  = VK_WHOLE_SIZE;
};

struct ShaderAddressWriteRange {
	VulkanBuffer* buffer  = nullptr;
	uint64_t      address = 0;
	uint64_t      size    = 0;
};

class DescriptorCache {
public:
	enum class Stage { Unknown, Vertex, Pixel, Compute };

	struct TextureBinding {
		VulkanImage* image      = nullptr;
		int          view       = VulkanImage::VIEW_DEFAULT;
		VkImageView  image_view = nullptr;
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
		std::vector<VkSampler>      samplers;
		std::vector<BufferView>     addresses;
		BufferView                  gds;
		BufferView                  flattened_srt;
		BufferView                  user_data;
	};

	DescriptorCache() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	virtual ~DescriptorCache() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(DescriptorCache);

	VkDescriptorSetLayout GetDescriptorSetLayout(Stage                                stage,
	                                             const ShaderRecompiler::IR::Program& program);
	void                  Recycle(VulkanDescriptorSet* set);
	VulkanDescriptorSet*  GetDescriptor(Stage stage, const ShaderRecompiler::IR::Program& program,
	                                    const NativeDescriptors& descriptors);

private:
	struct Pool {
		VkDescriptorPool pool           = nullptr;
		int              next_free_pool = -1;
		bool             free           = true;
	};

	void                 Init();
	void                 CreatePool(GraphicContext* gctx);
	VulkanDescriptorSet* Allocate(Stage stage, const ShaderRecompiler::IR::Program& program);
	VkDescriptorSetLayout
	GetDescriptorSetLayoutInternal(GraphicContext* gctx, Stage stage,
	                               const ShaderRecompiler::IR::Program& program);

	Common::Mutex     m_mutex;
	std::vector<Pool> m_pools;
	int               m_first_free_pool = -1;
	std::unordered_map<VkDescriptorSetLayout, std::vector<VulkanDescriptorSet*>>
	                                                       m_free_sets_by_layout;
	std::map<std::vector<uint32_t>, VkDescriptorSetLayout> m_descriptor_set_layouts;
	bool                                                   m_initialized = false;
};

const char*        storage_usage_name(ShaderStorageUsage usage);
VkImageAspectFlags DepthStencilAspectMask(VkFormat format);
std::vector<ShaderAddressWriteRange>
BindDescriptors(uint64_t submit_id, CommandBuffer* buffer, VkPipelineBindPoint pipeline_bind_point,
                VkPipelineLayout layout, const ShaderStageRuntime& runtime,
                VkShaderStageFlags vk_stage, DescriptorCache::Stage stage);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORCACHE_H_
