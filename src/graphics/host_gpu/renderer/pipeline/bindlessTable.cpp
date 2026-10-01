#include "graphics/host_gpu/renderer/pipeline/bindlessTable.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/samplerCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <array>
#include <atomic>
#include <fmt/format.h>
#include <string>
#include <cstdlib>

namespace Libs::Graphics {

BindlessTable::BindlessTable(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics) {
	if (!graphics.bindless_enabled) {
		return;
	}
	m_images_per_array =
	    std::min(MaxImagesPerArray, graphics.bindless_max_sampled_images / ImageArrays);
	if (m_images_per_array == 0) {
		LOGF("Bindless table: the device allows no update-after-bind sampled images\n");
		return;
	}

	constexpr auto image_flags = vk::DescriptorBindingFlagBits::ePartiallyBound |
	                             vk::DescriptorBindingFlagBits::eUpdateAfterBind |
	                             vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending;
	// The buffers are written once, here, so they need no update-after-bind.
	constexpr auto buffer_flags = vk::DescriptorBindingFlags {};
	// A device without update-after-bind samplers still gets a one-entry array, holding the
	// default sampler, so every bindless pipeline sees the same layout.
	m_samplers_per_array = std::min(MaxSamplers, graphics.bindless_max_samplers);
	const std::array<vk::DescriptorSetLayoutBinding, 7> bindings {{
	    {Images2D, vk::DescriptorType::eSampledImage, m_images_per_array, vk::ShaderStageFlagBits::eAll,
	     nullptr},
	    {Images2DArray, vk::DescriptorType::eSampledImage, m_images_per_array,
	     vk::ShaderStageFlagBits::eAll, nullptr},
	    {ImagesCube, vk::DescriptorType::eSampledImage, m_images_per_array,
	     vk::ShaderStageFlagBits::eAll, nullptr},
	    {Images3D, vk::DescriptorType::eSampledImage, m_images_per_array, vk::ShaderStageFlagBits::eAll,
	     nullptr},
	    {Translation, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr},
	    {Feedback, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr},
	    {Samplers, vk::DescriptorType::eSampler, std::max(m_samplers_per_array, 1u),
	     vk::ShaderStageFlagBits::eAll, nullptr},
	}};
	const std::array<vk::DescriptorBindingFlags, 7> binding_flags {
	    image_flags, image_flags, image_flags, image_flags, buffer_flags, buffer_flags, image_flags};
	vk::DescriptorSetLayoutBindingFlagsCreateInfo flags_info {};
	flags_info.bindingCount  = static_cast<uint32_t>(binding_flags.size());
	flags_info.pBindingFlags = binding_flags.data();
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.pNext        = &flags_info;
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
	layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
	layout_info.pBindings    = bindings.data();
	RequireVulkanSuccess(graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_layout),
	                     "create bindless descriptor layout");

	const std::array<vk::DescriptorPoolSize, 3> sizes {{
	    {vk::DescriptorType::eSampledImage, m_images_per_array * ImageArrays},
	    {vk::DescriptorType::eStorageBuffer, 2},
	    {vk::DescriptorType::eSampler, std::max(m_samplers_per_array, 1u)},
	}};
	vk::DescriptorPoolCreateInfo pool_info {};
	pool_info.flags         = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
	pool_info.maxSets       = 1;
	pool_info.poolSizeCount = static_cast<uint32_t>(sizes.size());
	pool_info.pPoolSizes    = sizes.data();
	RequireVulkanSuccess(graphics.device.createDescriptorPool(&pool_info, nullptr, &m_pool),
	                     "create bindless descriptor pool");

	vk::DescriptorSetAllocateInfo allocate_info {};
	allocate_info.descriptorPool     = m_pool;
	allocate_info.descriptorSetCount = 1;
	allocate_info.pSetLayouts        = &m_layout;
	RequireVulkanSuccess(graphics.device.allocateDescriptorSets(&allocate_info, &m_set),
	                     "allocate bindless descriptor set");

	// Both live in host-visible device memory: the host writes translations and clears feedback
	// flags directly, while shaders access them at device speed. The host reads the flags from
	// a host-cached snapshot (RecordFeedbackSnapshot).
	m_translation = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Stream, 0,
	                                         AllFlags, TranslationEntries * sizeof(uint32_t));
	m_feedback    = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Stream, 0,
	                                         AllFlags, TranslationEntries * sizeof(uint32_t));
	m_feedback_snapshot = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Download, 0,
	                                               AllFlags, TranslationEntries * sizeof(uint32_t));
	EXIT_IF(m_translation->Mapped().empty() || m_feedback->Mapped().empty() ||
	        m_feedback_snapshot->Mapped().empty());
	// A region allocated after a snapshot was recorded reads as "no requests" until the next.
	std::fill(m_feedback_snapshot->Mapped().begin(), m_feedback_snapshot->Mapped().end(),
	          uint8_t {0});
	m_feedback_snapshot->Flush(0, m_feedback_snapshot->Size());
	SetVulkanObjectNameF(graphics.device, m_translation->Handle(), "Bindless Translation");
	SetVulkanObjectNameF(graphics.device, m_feedback->Handle(), "Bindless Feedback");

	const std::array<vk::DescriptorBufferInfo, 2> buffer_infos {{
	    {m_translation->Handle(), 0, VK_WHOLE_SIZE},
	    {m_feedback->Handle(), 0, VK_WHOLE_SIZE},
	}};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t i = 0; i < writes.size(); i++) {
		writes[i].dstSet          = m_set;
		writes[i].dstBinding      = Translation + i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[i].pBufferInfo     = &buffer_infos[i];
	}
	graphics.device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0,
	                                     nullptr);
	CreatePlaceholders(scheduler);
	graphics.bindless_layout = m_layout;
	graphics.bindless_set    = m_set;
	LOGF("Bindless table: %u images per array, %u translation entries, %u samplers\n",
	     m_images_per_array, TranslationEntries, m_samplers_per_array);
}

BindlessTable::SamplerHeap* BindlessTable::FindOrCreateSamplerHeap(uint64_t base,
                                                                   uint32_t table_offset,
                                                                   uint32_t flags) {
	for (auto& heap: m_sampler_heaps) {
		if (heap.base == base && heap.table_offset == table_offset && heap.flags == flags) {
			return &heap;
		}
	}
	auto& heap        = m_sampler_heaps.emplace_back();
	heap.base         = base;
	heap.table_offset = table_offset;
	heap.flags        = flags;
	return &heap;
}

void BindlessTable::WriteDefaultSampler(vk::Sampler sampler) {
	if (m_default_sampler_written || m_set == nullptr) {
		return;
	}
	vk::DescriptorImageInfo info {sampler, nullptr, vk::ImageLayout::eUndefined};
	vk::WriteDescriptorSet  write {};
	write.dstSet          = m_set;
	write.dstBinding      = Samplers;
	write.dstArrayElement = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eSampler;
	write.pImageInfo      = &info;
	m_graphics.device.updateDescriptorSets(1, &write, 0, nullptr);
	m_default_sampler_written = true;
}

bool BindlessTable::MirrorSamplerHeap(SamplerHeap&                              heap,
                                      std::span<const std::array<uint32_t, 4>> records,
                                      SamplerCache&                             cache) {
	const auto same_prefix =
	    records.size() >= heap.records.size() &&
	    std::equal(heap.records.begin(), heap.records.end(), records.begin());
	if (same_prefix && records.size() == heap.records.size()) {
		return true;
	}
	uint32_t first = static_cast<uint32_t>(heap.records.size());
	if (!same_prefix || records.size() > heap.capacity || heap.region == 0) {
		const auto capacity = std::max<uint32_t>(64u, static_cast<uint32_t>(records.size()) * 2u);
		if (m_samplers_per_array <= 1u || capacity > m_samplers_per_array - m_next_sampler_slot) {
			static std::atomic<uint32_t> reported = 0;
			if (reported.fetch_add(1) < 8) {
				LOGF("Bindless samplers: array full (%u of %u slots), heap 0x%016" PRIx64
				     " keeps %zu records\n",
				     m_next_sampler_slot, m_samplers_per_array, heap.base, heap.records.size());
			}
			return false;
		}
		heap.region   = m_next_sampler_slot;
		heap.capacity = capacity;
		m_next_sampler_slot += capacity;
		first = 0;
	}
	std::vector<vk::DescriptorImageInfo> infos;
	infos.reserve(records.size() - first);
	for (size_t key = first; key < records.size(); key++) {
		ShaderSamplerResource descriptor;
		std::copy(records[key].begin(), records[key].end(), descriptor.fields);
		if ((heap.flags & SamplerDepthCompare) == 0u) {
			descriptor.fields[0] &= ~(0x7u << 12u);
		}
		if ((heap.flags & SamplerPointFiltering) != 0u) {
			descriptor.SetPointFiltering();
		}
		infos.push_back({cache.GetSampler(descriptor, (heap.flags & SamplerIntegerBorder) != 0u),
		                 nullptr, vk::ImageLayout::eUndefined});
	}
	if (!infos.empty()) {
		vk::WriteDescriptorSet write {};
		write.dstSet          = m_set;
		write.dstBinding      = Samplers;
		write.dstArrayElement = heap.region + first;
		write.descriptorCount = static_cast<uint32_t>(infos.size());
		write.descriptorType  = vk::DescriptorType::eSampler;
		write.pImageInfo      = infos.data();
		m_graphics.device.updateDescriptorSets(1, &write, 0, nullptr);
	}
	static std::atomic<uint32_t> logged = 0;
	if (logged.fetch_add(1) < 32) {
		LOGF("Bindless samplers: heap 0x%016" PRIx64 "+0x%x flags=%u region=%u records=%zu"
		     " (%u new)\n",
		     heap.base, heap.table_offset, heap.flags, heap.region, records.size(),
		     static_cast<uint32_t>(infos.size()));
	}
	heap.records.assign(records.begin(), records.end());
	return true;
}

void BindlessTable::CreatePlaceholders(CommandScheduler& /*scheduler*/) {
	struct Kind {
		vk::ImageType        type;
		vk::ImageViewType    view;
		uint32_t             layers;
		vk::ImageCreateFlags flags;
	};
	const std::array<Kind, ImageArrays> kinds {{
	    {vk::ImageType::e2D, vk::ImageViewType::e2D, 1, {}},
	    {vk::ImageType::e2D, vk::ImageViewType::e2DArray, 1, {}},
	    {vk::ImageType::e2D, vk::ImageViewType::eCube, 6, vk::ImageCreateFlagBits::eCubeCompatible},
	    {vk::ImageType::e3D, vk::ImageViewType::e3D, 1, {}},
	}};
	for (uint32_t i = 0; i < Placeholders; i++) {
		vk::ImageCreateInfo info {};
		info.flags         = kinds[i % ImageArrays].flags;
		info.imageType     = kinds[i % ImageArrays].type;
		info.format        = vk::Format::eR8G8B8A8Unorm;
		info.extent        = vk::Extent3D {1, 1, 1};
		info.mipLevels     = 1;
		info.arrayLayers   = kinds[i % ImageArrays].layers;
		info.samples       = vk::SampleCountFlagBits::e1;
		info.tiling        = vk::ImageTiling::eOptimal;
		info.usage         = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
		info.initialLayout = vk::ImageLayout::eUndefined;
		EXIT_IF(!m_graphics.CreateImage(info, m_placeholders[i]));
		vk::ImageViewCreateInfo view {};
		view.image            = m_placeholders[i].image;
		view.viewType         = kinds[i % ImageArrays].view;
		view.format           = info.format;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, kinds[i % ImageArrays].layers};
		RequireVulkanSuccess(
		    m_graphics.device.createImageView(&view, nullptr, &m_placeholder_views[i]),
		    "create bindless placeholder view");
	}

	// One fenced submission, before any rendering: clears and a buffer fill are not allowed
	// inside the render passes draws record into.
	vk::CommandPoolCreateInfo pool_info {};
	pool_info.flags            = vk::CommandPoolCreateFlagBits::eTransient;
	pool_info.queueFamilyIndex = m_graphics.queue_family;
	vk::CommandPool pool       = nullptr;
	RequireVulkanSuccess(m_graphics.device.createCommandPool(&pool_info, nullptr, &pool),
	                     "create bindless init pool");
	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = 1;
	vk::CommandBuffer command   = nullptr;
	RequireVulkanSuccess(m_graphics.device.allocateCommandBuffers(&allocate, &command),
	                     "allocate bindless init command buffer");
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	RequireVulkanSuccess(command.begin(&begin), "begin bindless init command buffer");

	std::array<vk::ImageMemoryBarrier, Placeholders> to_transfer {};
	std::array<vk::ImageMemoryBarrier, Placeholders> to_read {};
	for (uint32_t i = 0; i < Placeholders; i++) {
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0,
		                                       kinds[i % ImageArrays].layers};
		to_transfer[i].srcAccessMask       = {};
		to_transfer[i].dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		to_transfer[i].oldLayout           = vk::ImageLayout::eUndefined;
		to_transfer[i].newLayout           = vk::ImageLayout::eTransferDstOptimal;
		to_transfer[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_transfer[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_transfer[i].image               = m_placeholders[i].image;
		to_transfer[i].subresourceRange    = range;
		to_read[i]                         = to_transfer[i];
		to_read[i].srcAccessMask           = vk::AccessFlagBits::eTransferWrite;
		to_read[i].dstAccessMask           = vk::AccessFlagBits::eShaderRead;
		to_read[i].oldLayout               = vk::ImageLayout::eTransferDstOptimal;
		to_read[i].newLayout               = vk::ImageLayout::eShaderReadOnlyOptimal;
	}
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 0, nullptr,
	                        Placeholders, to_transfer.data());
	// Transparent black by default. KYTY_BINDLESS_DEBUG_COLORS=1: slot 0 grey (not resident), slot
	// 1 red (key outside its heap), slot 2 blue (pending), to see which case a surface is in.
	const bool debug_colors = std::getenv("KYTY_BINDLESS_DEBUG_COLORS") != nullptr;
	const auto color        = [debug_colors](float r, float g, float b) {
        return debug_colors ? vk::ClearColorValue {std::array<float, 4> {r, g, b, 1.0f}}
		                           : vk::ClearColorValue {std::array<float, 4> {0.0f, 0.0f, 0.0f, 0.0f}};
	};
	const std::array<vk::ClearColorValue, PlaceholderColors> colors {
	    color(0.5f, 0.5f, 0.5f), color(0.8f, 0.1f, 0.1f), color(0.1f, 0.2f, 0.8f)};
	for (uint32_t i = 0; i < Placeholders; i++) {
		command.clearColorImage(m_placeholders[i].image, vk::ImageLayout::eTransferDstOptimal,
		                        &colors[i / ImageArrays], 1, &to_transfer[i].subresourceRange);
	}
	// translation[0], the entry of keys outside their heap, is the red slot.
	command.fillBuffer(m_translation->Handle(), 4, VK_WHOLE_SIZE, 0u);
	command.fillBuffer(m_translation->Handle(), 0, 4, 1u);
	command.fillBuffer(m_feedback->Handle(), 0, VK_WHOLE_SIZE, 0u);
	vk::BufferMemoryBarrier translation_ready {};
	translation_ready.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	translation_ready.dstAccessMask       = vk::AccessFlagBits::eShaderRead;
	translation_ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	translation_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	translation_ready.buffer              = m_translation->Handle();
	translation_ready.offset              = 0;
	translation_ready.size                = VK_WHOLE_SIZE;
	auto feedback_ready                   = translation_ready;
	feedback_ready.dstAccessMask          = vk::AccessFlagBits::eShaderRead |
	                                        vk::AccessFlagBits::eShaderWrite |
	                                        vk::AccessFlagBits::eHostRead;
	feedback_ready.buffer                 = m_feedback->Handle();
	const std::array<vk::BufferMemoryBarrier, 2> buffers_ready {translation_ready, feedback_ready};
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eAllCommands |
	                            vk::PipelineStageFlagBits::eHost,
	                        {}, 0, nullptr, static_cast<uint32_t>(buffers_ready.size()),
	                        buffers_ready.data(), Placeholders, to_read.data());
	RequireVulkanSuccess(command.end(), "end bindless init command buffer");

	vk::FenceCreateInfo fence_info {};
	vk::Fence           fence = nullptr;
	RequireVulkanSuccess(m_graphics.device.createFence(&fence_info, nullptr, &fence),
	                     "create bindless init fence");
	vk::SubmitInfo submit {};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &command;
	{
		Common::LockGuard lock(m_graphics.queue_mutex);
		RequireVulkanSuccess(m_graphics.queue.submit(1, &submit, fence), "submit bindless init");
	}
	RequireVulkanSuccess(m_graphics.device.waitForFences(1, &fence, VK_TRUE, UINT64_MAX),
	                     "wait for bindless init");
	m_graphics.device.destroyFence(fence, nullptr);
	m_graphics.device.destroyCommandPool(pool, nullptr);

	std::array<vk::DescriptorImageInfo, Placeholders> infos {};
	std::array<vk::WriteDescriptorSet, Placeholders> writes {};
	for (uint32_t i = 0; i < Placeholders; i++) {
		infos[i]                  = {nullptr, m_placeholder_views[i],
		                             vk::ImageLayout::eShaderReadOnlyOptimal};
		writes[i].dstSet          = m_set;
		writes[i].dstBinding      = Images2D + i % ImageArrays;
		writes[i].dstArrayElement = i / ImageArrays;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eSampledImage;
		writes[i].pImageInfo      = &infos[i];
	}
	m_graphics.device.updateDescriptorSets(Placeholders, writes.data(), 0, nullptr);
}

BindlessTable::~BindlessTable() {
	for (uint32_t i = 0; i < Placeholders; i++) {
		if (m_placeholder_views[i] != nullptr) {
			m_graphics.device.destroyImageView(m_placeholder_views[i], nullptr);
		}
		if (m_placeholders[i].image != nullptr) {
			m_graphics.DeleteImage(m_placeholders[i]);
		}
	}
	m_translation.reset();
	m_feedback.reset();
	m_feedback_snapshot.reset();
	if (m_pool != nullptr) {
		m_graphics.device.destroyDescriptorPool(m_pool, nullptr);
	}
	if (m_layout != nullptr) {
		m_graphics.device.destroyDescriptorSetLayout(m_layout, nullptr);
	}
}

bool BindlessTable::AllocateRegion(Heap& heap, uint32_t entries) {
	// Headroom so a growing heap rarely moves; a move re-writes the keys resolved so far.
	const auto capacity = std::max(entries, std::min<uint32_t>(entries * 2u, 1u << 16u));
	if (capacity > ShaderRecompiler::IR::WatchdogReportBase - m_next_region) {
		return false;
	}
	const auto old_region  = heap.region;
	const auto old_entries = heap.entries;
	heap.region            = m_next_region;
	heap.entries           = capacity;
	m_next_region += capacity;
	heap.slots.resize(capacity, 0u);
	heap.settled.resize(capacity, 0u);
	auto* translation = reinterpret_cast<uint32_t*>(m_translation->Mapped().data());
	auto* feedback    = reinterpret_cast<uint32_t*>(m_feedback->Mapped().data());
	for (uint32_t key = 0; key < capacity; key++) {
		uint32_t value = ShaderRecompiler::IR::BindlessPending;
		if (key < old_entries && heap.settled[key] != 0) {
			value = heap.slots[key];
		}
		translation[heap.region + key] = value;
		feedback[heap.region + key]    = 0;
	}
	if (!m_translation->IsCoherent()) {
		m_translation->Flush(heap.region * sizeof(uint32_t), capacity * sizeof(uint32_t));
	}
	LOGF("Bindless heap region: base=0x%016" PRIx64 " binding=%u region=%u entries=%u (was %u at "
	     "%u)\n",
	     heap.base, heap.binding, heap.region, capacity, old_entries, old_region);
	return true;
}

BindlessTable::Heap* BindlessTable::FindOrCreateHeap(
    uint64_t base, uint32_t table_offset, uint32_t binding, uint32_t entries,
    const ShaderRecompiler::IR::ImageResource& resource) {
	if (entries == 0) {
		return nullptr;
	}
	for (auto& heap: m_heaps) {
		if (heap.base == base && heap.table_offset == table_offset && heap.binding == binding) {
			if (entries > heap.entries && !AllocateRegion(heap, entries)) {
				return nullptr;
			}
			return &heap;
		}
	}
	auto& heap        = m_heaps.emplace_back();
	heap.base         = base;
	heap.table_offset = table_offset;
	heap.binding      = binding;
	heap.resource     = resource;
	if (!AllocateRegion(heap, std::max(entries, 1u << 14u))) {
		m_heaps.pop_back();
		return nullptr;
	}
	return &heap;
}

uint32_t BindlessTable::AllocateSlot(uint32_t binding) {
	if (binding >= ImageArrays || m_next_slot[binding] >= m_images_per_array) {
		return 0;
	}
	return m_next_slot[binding]++;
}

void BindlessTable::WriteSlot(uint32_t binding, uint32_t slot, vk::ImageView view,
                              vk::ImageLayout layout) {
	const vk::DescriptorImageInfo info {nullptr, view, layout};
	vk::WriteDescriptorSet        write {};
	write.dstSet          = m_set;
	write.dstBinding      = binding;
	write.dstArrayElement = slot;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eSampledImage;
	write.pImageInfo      = &info;
	m_graphics.device.updateDescriptorSets(1, &write, 0, nullptr);
}

void BindlessTable::SetTranslation(const Heap& heap, uint32_t key, uint32_t slot) {
	auto* translation = reinterpret_cast<uint32_t*>(m_translation->Mapped().data());
	translation[heap.region + key] = slot;
	if (!m_translation->IsCoherent()) {
		m_translation->Flush((heap.region + key) * sizeof(uint32_t), sizeof(uint32_t));
	}
}

void BindlessTable::RecordFeedbackSnapshot(CommandScheduler& scheduler) {
	// Regions are allocated contiguously after word 0, so one copy covers every heap.
	const vk::DeviceSize size = vk::DeviceSize {m_next_region} * sizeof(uint32_t);
	scheduler.EndRendering();
	auto                    command = scheduler.Current().Handle();
	vk::BufferMemoryBarrier flags_written {};
	flags_written.srcAccessMask       = vk::AccessFlagBits::eShaderWrite;
	flags_written.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	flags_written.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	flags_written.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	flags_written.buffer              = m_feedback->Handle();
	flags_written.offset              = 0;
	flags_written.size                = size;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1,
	                        &flags_written, 0, nullptr);
	const vk::BufferCopy copy {0, 0, size};
	command.copyBuffer(m_feedback->Handle(), m_feedback_snapshot->Handle(), 1, &copy);
	auto snapshot_written          = flags_written;
	snapshot_written.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	snapshot_written.dstAccessMask = vk::AccessFlagBits::eHostRead;
	snapshot_written.buffer        = m_feedback_snapshot->Handle();
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1,
	                        &snapshot_written, 0, nullptr);
	m_snapshot_tick = scheduler.CurrentTick();
}

bool BindlessTable::SnapshotReady(CommandScheduler& scheduler) {
	return m_snapshot_tick != 0 && scheduler.IsFree(m_snapshot_tick);
}

void BindlessTable::TakeRequests(const Heap& heap, std::vector<uint32_t>& keys) {
	const auto* snapshot = reinterpret_cast<const uint32_t*>(m_feedback_snapshot->Mapped().data());
	auto*       feedback = reinterpret_cast<uint32_t*>(m_feedback->Mapped().data());
	m_feedback_snapshot->Invalidate(heap.region * sizeof(uint32_t),
	                                heap.entries * sizeof(uint32_t));
	bool cleared = false;
	for (uint32_t key = 0; key < heap.entries; key++) {
		if (snapshot[heap.region + key] != 0) {
			// A flag the GPU sets again after the snapshot may be lost here; a key that is
			// still pending is flagged again by the next draw that samples it.
			feedback[heap.region + key] = 0;
			keys.push_back(key);
			cleared = true;
		}
	}
	if (cleared) {
		m_feedback->Flush(heap.region * sizeof(uint32_t), heap.entries * sizeof(uint32_t));
	}
}

void BindlessTable::DrainWatchdogReports(uint64_t frame) {
	using ShaderRecompiler::IR::WatchdogReportBase;
	using ShaderRecompiler::IR::WatchdogReportPhis;
	using ShaderRecompiler::IR::WatchdogReportSlots;
	using ShaderRecompiler::IR::WatchdogReportWords;
	static_assert(WatchdogReportBase + WatchdogReportWords * (WatchdogReportSlots + 1) <=
	              TranslationEntries);
	auto* feedback = reinterpret_cast<uint32_t*>(m_feedback->Mapped().data());
	if (!m_feedback->IsCoherent()) {
		m_feedback->Invalidate(WatchdogReportBase * sizeof(uint32_t),
		                       WatchdogReportWords * (WatchdogReportSlots + 1) * sizeof(uint32_t));
	}
	const auto count = feedback[WatchdogReportBase];
	if (count == 0) {
		return;
	}
	const auto stored = std::min(count, WatchdogReportSlots);
	// Per (shader, loop): how many tripped, and the first report in full.
	std::unordered_map<uint64_t, std::pair<uint32_t, const uint32_t*>> loops;
	for (uint32_t n = 0; n < stored; n++) {
		const auto* report = feedback + WatchdogReportBase + WatchdogReportWords * (n + 1);
		if ((report[0] >> 16u) != 0x5744u) {
			continue;
		}
		const auto key = (static_cast<uint64_t>(report[2]) << 32u | report[1]) ^
		                 (static_cast<uint64_t>(report[0] & 0xffffu) << 48u);
		auto& entry = loops[key];
		if (entry.first++ == 0) {
			entry.second = report;
		}
	}
	static std::atomic<uint32_t> logged = 0;
	LOGF("WatchdogTrips: frame=%" PRIu64 " reports=%u loops=%zu\n", frame, count, loops.size());
	for (const auto& [key, entry]: loops) {
		if (logged.fetch_add(1) >= 256) {
			break;
		}
		const auto* r = entry.second;
		std::string phis;
		for (uint32_t half = 0; half < 2; half++) {
			phis += half == 0 ? " lo:" : " hi:";
			for (uint32_t i = 0; i < WatchdogReportPhis; i++) {
				phis += fmt::format(" {:x}", r[16 + half * WatchdogReportPhis + i]);
			}
		}
		LOGF("WatchdogTrip: hash=0x%08x%08x loop_block=%u trips=%u counter=%u group=%u lane=%u "
		     "ud=[%08x %08x %08x %08x %08x %08x %08x %08x]%s\n",
		     r[2], r[1], r[0] & 0xffffu, entry.first, r[3], r[4], r[5], r[6], r[7], r[8], r[9],
		     r[10], r[11], r[12], r[13], phis.c_str());
	}
	feedback[WatchdogReportBase] = 0;
	if (!m_feedback->IsCoherent()) {
		m_feedback->Flush(WatchdogReportBase * sizeof(uint32_t), sizeof(uint32_t));
	}
}

void BindlessTable::AddImageReference(ImageId id, Heap& heap, uint32_t key) {
	m_image_refs[ImageKey(id)].emplace_back(&heap, key);
}

void BindlessTable::OnImageUnregistered(ImageId id) {
	const auto found = m_image_refs.find(ImageKey(id));
	if (found == m_image_refs.end()) {
		return;
	}
	for (const auto& [heap, key]: found->second) {
		if (heap->slots[key] != 0) {
			WriteSlot(heap->binding, heap->slots[key], m_placeholder_views[heap->binding],
			          vk::ImageLayout::eShaderReadOnlyOptimal);
		}
		heap->slots[key]   = 0;
		heap->settled[key] = 0;
		SetTranslation(*heap, key, ShaderRecompiler::IR::BindlessPending);
		std::erase(heap->resolved, id);
	}
	m_image_refs.erase(found);
}

} // namespace Libs::Graphics
