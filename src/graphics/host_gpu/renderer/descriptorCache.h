#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORCACHE_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/textureCache.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shaderBindings.h"

#include <map>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

namespace ShaderRecompiler::IR {
struct DescriptorValue;
struct ImageResource;
struct Program;
struct ResourceSnapshot;
} // namespace ShaderRecompiler::IR

class CommandBuffer;
struct DescriptorCacheTestAccess;
struct ShaderStageRuntime;

struct VulkanDescriptorSet {
	vk::DescriptorSet       set     = nullptr;
	vk::DescriptorSetLayout layout  = nullptr;
	int                     pool_id = -1;
};

struct BufferView {
	std::shared_ptr<void> owner;
	vk::Buffer            buffer = nullptr;
	vk::DeviceSize        offset = 0;
	vk::DeviceSize        range  = VK_WHOLE_SIZE;
};

class DescriptorCache {
public:
	enum class Stage { Unknown, Vertex, Pixel, Compute };

	struct TextureBinding {
		ImageId                 image_id;
		vk::ImageView           image_view = nullptr;
		TextureCache::ImageDesc desc;
		vk::ImageLayout         layout = vk::ImageLayout::eUndefined;
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

	struct PreparedBindings {
		std::shared_ptr<const ShaderRecompiler::IR::Program>          program;
		std::shared_ptr<const ShaderRecompiler::IR::ResourceSnapshot> snapshot;
		NativeDescriptors                                             resources;
		std::vector<uint32_t>                                         flattened_srt;
		std::vector<uint32_t>                                         user_data;
		vk::ShaderStageFlags                                          shader_stage;
		Stage                                                         stage     = Stage::Unknown;
		bool                                                          committed = false;
	};

	explicit DescriptorCache(GraphicContext& graphics): m_graphics(graphics) {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	}
	~DescriptorCache();
	KYTY_CLASS_NO_COPY(DescriptorCache);

	vk::DescriptorSetLayout GetDescriptorSetLayout(Stage                                stage,
	                                               const ShaderRecompiler::IR::Program& program);
	void                    Recycle(VulkanDescriptorSet& set);
	VulkanDescriptorSet&    GetDescriptor(Stage stage, const ShaderRecompiler::IR::Program& program,
	                                      const NativeDescriptors& descriptors);

private:
	friend struct DescriptorCacheTestAccess;

	struct Pool {
		vk::DescriptorPool pool           = nullptr;
		int                next_free_pool = -1;
	};

	static vk::DescriptorImageInfo MakeImageInfo(const TextureBinding& texture);
	void                 CreatePool();
	VulkanDescriptorSet* Allocate(Stage stage, const ShaderRecompiler::IR::Program& program);
	vk::DescriptorSetLayout
	GetDescriptorSetLayoutInternal(Stage stage, const ShaderRecompiler::IR::Program& program);

	GraphicContext&   m_graphics;
	Common::Mutex     m_mutex;
	std::vector<Pool> m_pools;
	int               m_first_free_pool = -1;
	std::unordered_map<vk::DescriptorSetLayout, std::vector<VulkanDescriptorSet*>>
	                                                         m_free_sets_by_layout;
	std::map<std::vector<uint32_t>, vk::DescriptorSetLayout> m_descriptor_set_layouts;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORCACHE_H_
