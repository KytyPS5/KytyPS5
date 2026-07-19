#include "graphics/host_gpu/renderer/descriptorCache.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ShaderIR.h"

#include <array>

namespace Libs::Graphics {
namespace {

using BindingKind = ShaderRecompiler::IR::DescriptorBindingKind;

vk::ShaderStageFlags StageFlags(DescriptorCache::Stage stage) {
	switch (stage) {
		case DescriptorCache::Stage::Vertex: return vk::ShaderStageFlagBits::eVertex;
		case DescriptorCache::Stage::Pixel: return vk::ShaderStageFlagBits::eFragment;
		case DescriptorCache::Stage::Compute: return vk::ShaderStageFlagBits::eCompute;
		default: EXIT("unknown descriptor stage\n");
	}
}

bool IsSampledImage(BindingKind kind) {
	switch (kind) {
		case BindingKind::Sampled2D:
		case BindingKind::Sampled2DArray:
		case BindingKind::Sampled3D:
		case BindingKind::SampledUint2D:
		case BindingKind::SampledUint2DArray:
		case BindingKind::SampledUint3D: return true;
		default: return false;
	}
}

bool IsStorageImage(BindingKind kind) {
	switch (kind) {
		case BindingKind::Storage2D:
		case BindingKind::Storage2DArray:
		case BindingKind::Storage3D:
		case BindingKind::StorageUint2D:
		case BindingKind::StorageUint2DArray:
		case BindingKind::StorageUint3D: return true;
		default: return false;
	}
}

vk::DescriptorType DescriptorType(BindingKind kind) {
	if (kind == BindingKind::Samplers) {
		return vk::DescriptorType::eSampler;
	}
	if (IsSampledImage(kind)) {
		return vk::DescriptorType::eSampledImage;
	}
	if (IsStorageImage(kind)) {
		return vk::DescriptorType::eStorageImage;
	}
	return vk::DescriptorType::eStorageBuffer;
}

uint32_t DescriptorCount(const ShaderRecompiler::IR::DescriptorBinding& binding) {
	return binding.resources.empty() ? 1u : static_cast<uint32_t>(binding.resources.size());
}

std::vector<uint32_t> LayoutKey(DescriptorCache::Stage               stage,
                                const ShaderRecompiler::IR::Program& program) {
	std::vector<uint32_t> key;
	key.reserve(1u + program.bindings.descriptors.size() * 4u);
	key.push_back(static_cast<uint32_t>(stage));
	for (const auto& binding: program.bindings.descriptors) {
		key.push_back(static_cast<uint32_t>(binding.kind));
		key.push_back(binding.binding);
		key.push_back(DescriptorCount(binding));
		key.push_back(static_cast<uint32_t>(DescriptorType(binding.kind)));
	}
	return key;
}

vk::ImageLayout SampledLayout(const VulkanImage* image) {
	EXIT_IF(image == nullptr);
	switch (image->layout) {
		case vk::ImageLayout::eTransferDstOptimal:
		case vk::ImageLayout::eTransferSrcOptimal:
		case vk::ImageLayout::eColorAttachmentOptimal: return vk::ImageLayout::eGeneral;
		default: return image->layout;
	}
}

vk::DescriptorBufferInfo BufferInfo(const BufferView& view) {
	EXIT_IF(view.buffer == nullptr || view.buffer->buffer == nullptr);
	return {view.buffer->buffer, view.offset, view.range};
}

vk::DescriptorImageInfo MakeDescriptorImageInfo(const DescriptorCache::TextureBinding& texture,
                                                vk::ImageLayout                        layout) {
	EXIT_IF(texture.image == nullptr || texture.view < 0 || texture.view >= VulkanImage::VIEW_MAX);
	const auto view = texture.image_view != nullptr ? texture.image_view
	                                                : texture.image->image_view[texture.view];
	EXIT_IF(view == nullptr);
	return {nullptr, view, layout};
}

} // namespace

vk::DescriptorSetLayout
DescriptorCache::GetDescriptorSetLayoutInternal(GraphicContext* gctx, Stage stage,
                                                const ShaderRecompiler::IR::Program& program) {
	EXIT_IF(gctx == nullptr);
	const auto key = LayoutKey(stage, program);
	if (const auto found = m_descriptor_set_layouts.find(key);
	    found != m_descriptor_set_layouts.end()) {
		return found->second;
	}

	std::vector<vk::DescriptorSetLayoutBinding> bindings;
	bindings.reserve(program.bindings.descriptors.size());
	for (const auto& descriptor: program.bindings.descriptors) {
		bindings.push_back({descriptor.binding, DescriptorType(descriptor.kind),
		                    DescriptorCount(descriptor), StageFlags(stage), nullptr});
	}
	if (bindings.empty()) {
		return nullptr;
	}

	vk::DescriptorSetLayoutCreateInfo info {};
	info.sType                     = vk::StructureType::eDescriptorSetLayoutCreateInfo;
	info.bindingCount              = static_cast<uint32_t>(bindings.size());
	info.pBindings                 = bindings.data();
	vk::DescriptorSetLayout layout = nullptr;
	const auto result = gctx->device.createDescriptorSetLayout(&info, nullptr, &layout);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || layout == nullptr);
	m_descriptor_set_layouts.emplace(key, layout);
	return layout;
}

void DescriptorCache::CreatePool(GraphicContext* gctx) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(gctx == nullptr);
	constexpr uint32_t           MaxSets = 512;
	const vk::DescriptorPoolSize sizes[] = {
	    {vk::DescriptorType::eStorageBuffer,
	     MaxSets * (ShaderRecompiler::IR::ShaderInfo::MaxBuffers +
	                ShaderRecompiler::IR::ShaderInfo::MaxAddresses + 3u)},
	    {vk::DescriptorType::eSampledImage, MaxSets * ShaderRecompiler::IR::ShaderInfo::MaxImages},
	    {vk::DescriptorType::eStorageImage, MaxSets * ShaderRecompiler::IR::ShaderInfo::MaxImages},
	    {vk::DescriptorType::eSampler, MaxSets * ShaderRecompiler::IR::ShaderInfo::MaxSamplers},
	};
	vk::DescriptorPoolCreateInfo info {};
	info.sType         = vk::StructureType::eDescriptorPoolCreateInfo;
	info.poolSizeCount = static_cast<uint32_t>(std::size(sizes));
	info.pPoolSizes    = sizes;
	info.maxSets       = MaxSets;
	const auto pool_id = static_cast<int>(m_pools.size());
	auto&      pool    = m_pools.emplace_back();
	const auto result  = gctx->device.createDescriptorPool(&info, nullptr, &pool.pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || pool.pool == nullptr);
	pool.next_free_pool = m_first_free_pool;
	m_first_free_pool   = pool_id;
}

VulkanDescriptorSet* DescriptorCache::Allocate(Stage                                stage,
                                               const ShaderRecompiler::IR::Program& program) {
	KYTY_PROFILER_FUNCTION();
	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);
	Common::LockGuard lock(m_mutex);
	const auto        layout = GetDescriptorSetLayoutInternal(gctx, stage, program);
	EXIT_IF(layout == nullptr);
	auto& free_sets = m_free_sets_by_layout[layout];
	if (!free_sets.empty()) {
		auto* result = free_sets.back();
		free_sets.pop_back();
		return result;
	}

	constexpr uint32_t Batch = 32;
	for (int attempt = 0; attempt < 2; attempt++) {
		for (int pool_id = m_first_free_pool; pool_id != -1;
		     pool_id     = m_pools[pool_id].next_free_pool) {
			auto&                                      pool = m_pools[pool_id];
			std::array<vk::DescriptorSetLayout, Batch> layouts;
			layouts.fill(layout);
			std::array<vk::DescriptorSet, Batch> sets {};
			vk::DescriptorSetAllocateInfo        info {};
			info.sType              = vk::StructureType::eDescriptorSetAllocateInfo;
			info.descriptorPool     = pool.pool;
			info.descriptorSetCount = Batch;
			info.pSetLayouts        = layouts.data();
			if (gctx->device.allocateDescriptorSets(&info, sets.data()) == vk::Result::eSuccess) {
				free_sets.reserve(free_sets.size() + Batch - 1u);
				for (uint32_t i = 0; i + 1u < Batch; i++) {
					free_sets.push_back(new VulkanDescriptorSet {sets[i], layout, pool_id});
				}
				return new VulkanDescriptorSet {sets.back(), layout, pool_id};
			}
			m_first_free_pool = pool.next_free_pool;
			break;
		}
		CreatePool(gctx);
	}
	return nullptr;
}

void DescriptorCache::Recycle(VulkanDescriptorSet* set) {
	EXIT_IF(set == nullptr || set->set == nullptr || set->layout == nullptr);
	Common::LockGuard lock(m_mutex);
	m_free_sets_by_layout[set->layout].push_back(set);
}

VulkanDescriptorSet* DescriptorCache::GetDescriptor(Stage                                stage,
                                                    const ShaderRecompiler::IR::Program& program,
                                                    const NativeDescriptors&             data) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(data.buffers.size() != program.info.buffers.size() ||
	        data.images.size() != program.info.images.size() ||
	        data.samplers.size() != program.info.samplers.size() ||
	        data.addresses.size() != program.info.addresses.size());
	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);
	auto* set = Allocate(stage, program);
	EXIT_NOT_IMPLEMENTED(set == nullptr);

	const auto descriptor_count = program.info.buffers.size() + program.info.images.size() +
	                              program.info.samplers.size() + program.info.addresses.size() + 3u;
	std::vector<vk::DescriptorBufferInfo> buffer_infos;
	std::vector<vk::DescriptorImageInfo>  image_infos;
	std::vector<vk::WriteDescriptorSet>   writes;
	buffer_infos.reserve(descriptor_count);
	image_infos.reserve(descriptor_count);
	writes.reserve(program.bindings.descriptors.size());

	for (const auto& binding: program.bindings.descriptors) {
		vk::WriteDescriptorSet write {};
		write.sType             = vk::StructureType::eWriteDescriptorSet;
		write.dstSet            = set->set;
		write.dstBinding        = binding.binding;
		write.descriptorType    = DescriptorType(binding.kind);
		write.descriptorCount   = DescriptorCount(binding);
		const auto buffer_start = buffer_infos.size();
		const auto image_start  = image_infos.size();
		switch (binding.kind) {
			case BindingKind::Buffers:
				for (const auto resource: binding.resources) {
					buffer_infos.push_back(BufferInfo(data.buffers.at(resource)));
				}
				break;
			case BindingKind::AddressMemory:
				for (const auto resource: binding.resources) {
					buffer_infos.push_back(BufferInfo(data.addresses.at(resource)));
				}
				break;
			case BindingKind::FlattenedSrt:
				buffer_infos.push_back(BufferInfo(data.flattened_srt));
				break;
			case BindingKind::UserData: buffer_infos.push_back(BufferInfo(data.user_data)); break;
			case BindingKind::Gds: buffer_infos.push_back(BufferInfo(data.gds)); break;
			case BindingKind::Samplers:
				for (const auto resource: binding.resources) {
					const auto sampler = data.samplers.at(resource);
					EXIT_IF(sampler == nullptr);
					image_infos.push_back({sampler, nullptr, vk::ImageLayout::eUndefined});
				}
				break;
			default: {
				const auto layout = IsStorageImage(binding.kind) ? vk::ImageLayout::eGeneral
				                                                 : vk::ImageLayout::eUndefined;
				for (const auto resource: binding.resources) {
					const auto& texture = data.images.at(resource);
					image_infos.push_back(MakeDescriptorImageInfo(
					    texture,
					    IsSampledImage(binding.kind) ? SampledLayout(texture.image) : layout));
				}
				break;
			}
		}
		if (buffer_infos.size() != buffer_start) {
			write.pBufferInfo = buffer_infos.data() + buffer_start;
		}
		if (image_infos.size() != image_start) {
			write.pImageInfo = image_infos.data() + image_start;
		}
		writes.push_back(write);
	}
	gctx->device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0,
	                                  nullptr);
	return set;
}

vk::DescriptorSetLayout
DescriptorCache::GetDescriptorSetLayout(Stage stage, const ShaderRecompiler::IR::Program& program) {
	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);
	Common::LockGuard lock(m_mutex);
	return GetDescriptorSetLayoutInternal(gctx, stage, program);
}

} // namespace Libs::Graphics
