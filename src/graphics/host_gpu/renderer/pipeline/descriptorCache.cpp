#include "graphics/host_gpu/renderer/pipeline/descriptorCache.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

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
		case BindingKind::Sampled1D:
		case BindingKind::Sampled1DArray:
		case BindingKind::Sampled2D:
		case BindingKind::Sampled2DArray:
		case BindingKind::Sampled2DMsaa:
		case BindingKind::Sampled2DMsaaArray:
		case BindingKind::Sampled3D:
		case BindingKind::SampledUint1D:
		case BindingKind::SampledUint1DArray:
		case BindingKind::SampledUint2D:
		case BindingKind::SampledUint2DArray:
		case BindingKind::SampledUint2DMsaa:
		case BindingKind::SampledUint2DMsaaArray:
		case BindingKind::SampledUint3D: return true;
		default: return false;
	}
}

bool IsStorageImage(BindingKind kind) {
	switch (kind) {
		case BindingKind::Storage1D:
		case BindingKind::Storage1DArray:
		case BindingKind::Storage2D:
		case BindingKind::Storage2DArray:
		case BindingKind::Storage3D:
		case BindingKind::StorageUint1D:
		case BindingKind::StorageUint1DArray:
		case BindingKind::StorageUint2D:
		case BindingKind::StorageUint2DArray:
		case BindingKind::StorageUint3D:
		case BindingKind::StorageAtomic1D:
		case BindingKind::StorageAtomic1DArray:
		case BindingKind::StorageAtomic2D:
		case BindingKind::StorageAtomic2DArray:
		case BindingKind::StorageAtomic3D: return true;
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

vk::DescriptorBufferInfo BufferInfo(const BufferView& view) {
	EXIT_IF(view.buffer == nullptr);
	return {view.buffer, view.offset, view.range};
}

} // namespace

vk::DescriptorImageInfo DescriptorCache::MakeImageInfo(const TextureBinding& texture,
                                                       uint32_t              element) {
	const auto view =
	    texture.mip_views.empty()
	        ? (element == 0u ? texture.image_view : vk::ImageView {})
	        : (element < texture.mip_views.size() ? texture.mip_views[element] : vk::ImageView {});
	EXIT_IF(!texture.image_id || view == nullptr || texture.layout == vk::ImageLayout::eUndefined);
	return {nullptr, view, texture.layout};
}

DescriptorCache::~DescriptorCache() {
	for (auto& [layout, sets]: m_free_sets_by_layout) {
		(void)layout;
		for (auto* set: sets) {
			delete set;
		}
	}
	for (const auto& [key, layout]: m_descriptor_set_layouts) {
		(void)key;
		m_graphics.device.destroyDescriptorSetLayout(layout, nullptr);
	}
	for (const auto& pool: m_pools) {
		m_graphics.device.destroyDescriptorPool(pool.pool, nullptr);
	}
}

vk::DescriptorSetLayout
DescriptorCache::GetDescriptorSetLayoutInternal(Stage                                stage,
                                                const ShaderRecompiler::IR::Program& program) {
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
	const auto result = m_graphics.device.createDescriptorSetLayout(&info, nullptr, &layout);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || layout == nullptr);
	m_descriptor_set_layouts.emplace(key, layout);
	return layout;
}

void DescriptorCache::CreatePool() {
	KYTY_PROFILER_FUNCTION();
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
	const auto result  = m_graphics.device.createDescriptorPool(&info, nullptr, &pool.pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || pool.pool == nullptr);
	pool.next_free_pool = m_first_free_pool;
	m_first_free_pool   = pool_id;
}

VulkanDescriptorSet* DescriptorCache::Allocate(Stage                                stage,
                                               const ShaderRecompiler::IR::Program& program) {
	KYTY_PROFILER_FUNCTION();
	Common::LockGuard lock(m_mutex);
	const auto        layout = GetDescriptorSetLayoutInternal(stage, program);
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
			if (m_graphics.device.allocateDescriptorSets(&info, sets.data()) ==
			    vk::Result::eSuccess) {
				free_sets.reserve(free_sets.size() + Batch - 1u);
				for (uint32_t i = 0; i + 1u < Batch; i++) {
					free_sets.push_back(new VulkanDescriptorSet {sets[i], layout, pool_id});
				}
				return new VulkanDescriptorSet {sets.back(), layout, pool_id};
			}
			m_first_free_pool = pool.next_free_pool;
			break;
		}
		CreatePool();
	}
	return nullptr;
}

void DescriptorCache::Recycle(VulkanDescriptorSet& set) {
	EXIT_IF(set.set == nullptr || set.layout == nullptr);
	Common::LockGuard lock(m_mutex);
	m_free_sets_by_layout[set.layout].push_back(&set);
}

VulkanDescriptorSet& DescriptorCache::GetDescriptor(Stage                                stage,
                                                    const ShaderRecompiler::IR::Program& program,
                                                    const NativeDescriptors&             data) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(data.buffers.size() != program.info.buffers.size() ||
	        data.images.size() != program.info.images.size() ||
	        data.samplers.size() != program.info.samplers.size() ||
	        data.addresses.size() != program.info.addresses.size());
	auto* set = Allocate(stage, program);
	EXIT_NOT_IMPLEMENTED(set == nullptr);

	size_t descriptor_count = 0;
	for (const auto& binding: program.bindings.descriptors) {
		descriptor_count += DescriptorCount(binding);
	}
	std::vector<vk::DescriptorBufferInfo> buffer_infos;
	std::vector<vk::DescriptorImageInfo>  image_infos;
	std::vector<vk::WriteDescriptorSet>   writes;
	buffer_infos.reserve(descriptor_count);
	image_infos.reserve(descriptor_count);
	writes.reserve(program.bindings.descriptors.size());
	std::vector<uint32_t> image_occurrences(data.images.size());

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
				for (const auto resource: binding.resources) {
					const auto& texture = data.images.at(resource);
					image_infos.push_back(MakeImageInfo(texture, image_occurrences.at(resource)++));
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
	for (uint32_t i = 0; i < data.images.size(); i++) {
		const auto expected = data.images[i].mip_views.empty()
		                          ? 1u
		                          : static_cast<uint32_t>(data.images[i].mip_views.size());
		EXIT_IF(image_occurrences[i] != expected);
	}
	m_graphics.device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0,
	                                       nullptr);
	return *set;
}

vk::DescriptorSetLayout
DescriptorCache::GetDescriptorSetLayout(Stage stage, const ShaderRecompiler::IR::Program& program) {
	Common::LockGuard lock(m_mutex);
	return GetDescriptorSetLayoutInternal(stage, program);
}

} // namespace Libs::Graphics
