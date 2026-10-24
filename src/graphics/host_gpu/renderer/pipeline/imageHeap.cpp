#include "graphics/host_gpu/renderer/pipeline/imageHeap.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "kernel/memory.h"

#include <algorithm>
#include <xxhash.h>

namespace Libs::Graphics {
namespace {

namespace IR = ShaderRecompiler::IR;

// Smaller tables are cheap to materialize on each draw and are often transient.
constexpr uint64_t MinTableBytes = 64ull * 1024ull;
constexpr uint32_t ElementMask   = (1u << IR::ImageHeapKindShift) - 1u;

template <typename Pred, typename Fn>
void ForEachRun(size_t begin, size_t end, Pred&& pred, Fn&& fn) {
	while (begin < end) {
		if (!pred(begin)) {
			++begin;
			continue;
		}
		auto run = begin + 1u;
		while (run < end && pred(run)) ++run;
		fn(begin, run);
		begin = run;
	}
}

// Grows [begin, end) to cover [first, last).
void Widen(size_t& begin, size_t& end, size_t first, size_t last) {
	const bool empty = begin == end;
	begin            = empty ? first : std::min(begin, first);
	end              = empty ? last : std::max(end, last);
}

// Guest bytes of a table that its tracker pages [begin, end) cover.
std::pair<uint64_t, uint64_t> PageRange(uint64_t base, uint64_t size, size_t begin, size_t end) {
	const auto origin = Common::AlignDown(base, TRACKER_PAGE_SIZE);
	return {std::max(base, origin + begin * TRACKER_PAGE_SIZE),
	        std::min(base + size, origin + end * TRACKER_PAGE_SIZE)};
}

} // namespace

size_t ImageHeap::DescriptorHash::operator()(const DescriptorValue& value) const noexcept {
	return XXH3_64bits(value.dwords.data(), sizeof(value.dwords));
}

ImageHeap::ImageHeap(RenderContext& context, PageManager& page_manager)
    : m_context(context), m_page_manager(page_manager) {
	auto& graphics = context.GetGraphics();
	vk::PhysicalDeviceVulkan12Properties limits {};
	vk::PhysicalDeviceProperties2        properties {};
	properties.pNext = &limits;
	graphics.physical_device.getProperties2(&properties);
	EXIT_NOT_IMPLEMENTED(std::min({limits.maxDescriptorSetUpdateAfterBindSampledImages,
	                               limits.maxPerStageDescriptorUpdateAfterBindSampledImages,
	                               limits.maxPerStageUpdateAfterBindResources}) <
	                     IR::ImageHeapCapacity);

	const vk::DescriptorBindingFlags binding_flags =
	    vk::DescriptorBindingFlagBits::ePartiallyBound |
	    vk::DescriptorBindingFlagBits::eUpdateAfterBind |
	    vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending;
	vk::DescriptorSetLayoutBindingFlagsCreateInfo flags_info {};
	flags_info.bindingCount  = 1;
	flags_info.pBindingFlags = &binding_flags;
	const vk::DescriptorSetLayoutBinding binding {0, vk::DescriptorType::eSampledImage,
	                                              IR::ImageHeapCapacity,
	                                              vk::ShaderStageFlagBits::eAll, nullptr};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.pNext        = &flags_info;
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
	layout_info.bindingCount = 1;
	layout_info.pBindings    = &binding;
	EXIT_IF(graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_layout) !=
	        vk::Result::eSuccess);
	const vk::DescriptorPoolSize pool_size {vk::DescriptorType::eSampledImage,
	                                        IR::ImageHeapCapacity};
	vk::DescriptorPoolCreateInfo pool_info {};
	pool_info.flags         = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
	pool_info.maxSets       = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes    = &pool_size;
	EXIT_IF(graphics.device.createDescriptorPool(&pool_info, nullptr, &m_pool) !=
	        vk::Result::eSuccess);
	vk::DescriptorSetAllocateInfo allocate {};
	allocate.descriptorPool     = m_pool;
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts        = &m_layout;
	EXIT_IF(graphics.device.allocateDescriptorSets(&allocate, &m_set) != vk::Result::eSuccess);
	graphics.image_heap_layout = m_layout;
	// Slot word 0 selects element 0, which stays a null image.
	m_elements.push_back({.binding = {.layout = vk::ImageLayout::eShaderReadOnlyOptimal}});
	m_writes.push_back(0u);
	WriteDescriptors();
}

ImageHeap::~ImageHeap() {
	for (const auto& memory: m_memories) Unwatch(*memory);
	auto& graphics             = m_context.GetGraphics();
	graphics.image_heap_layout = nullptr;
	graphics.device.destroyDescriptorPool(m_pool, nullptr);
	graphics.device.destroyDescriptorSetLayout(m_layout, nullptr);
}

bool ImageHeap::Acquire(const IR::ImageHeapRequest& request, IR::ImageHeapView& result) {
	KYTY_PROFILER_FUNCTION();
	const auto size = request.size & ~uint64_t {3};
	if (size < MinTableBytes || request.count == 0u || request.step == 0u ||
	    ((request.base | request.first | request.step) & 3u) != 0u) {
		return false;
	}
	auto memory = std::ranges::find_if(m_memories, [&](const auto& candidate) {
		return candidate->base == request.base && candidate->size == size;
	});
	if (memory == m_memories.end()) {
		if (!m_context.IsMapped(request.base, size)) return false;
		auto created   = std::make_unique<Memory>();
		created->base  = request.base;
		created->size  = size;
		created->words.resize(size / 4u);
		created->dead.resize(size / 4u);
		created->dirty_pages.assign((Common::AlignUp(request.base + size, TRACKER_PAGE_SIZE) -
		                             Common::AlignDown(request.base, TRACKER_PAGE_SIZE)) /
		                                TRACKER_PAGE_SIZE,
		                            uint8_t {1});
		std::scoped_lock lock {m_mutex};
		memory = m_memories.insert(m_memories.end(), std::move(created));
	}
	// A draw refreshes a table and a view takes its changes once, so all programs of the draw see
	// one snapshot and one set of kinds.
	if ((*memory)->draw != m_draw) {
		if (!Refresh(**memory)) return false;
		(*memory)->draw = m_draw;
	}
	auto&      views   = (*memory)->views;
	const auto profile = IR::ImageHeapResource(*request.root, 0u);
	auto       view    = std::ranges::find_if(views, [&](const auto& candidate) {
		return candidate->step == request.step && candidate->first == request.first &&
		       candidate->count == request.count && candidate->profile == profile;
	});
	if (view == views.end()) {
		const auto epoch   = Image::heap_epoch.load(std::memory_order_relaxed);
		auto       created = std::make_unique<View>(View {.memory        = **memory,
		                                                  .step          = request.step,
		                                                  .first         = request.first,
		                                                  .count         = request.count,
		                                                  .profile       = profile,
		                                                  .resolve_epoch = epoch,
		                                                  .layout_epoch  = epoch});
		created->slots.resize(size / 4u + 1u);
		created->buffer = std::make_unique<Buffer>(
		    m_context.GetGraphics(), m_context.GetCommandScheduler(), MemoryUsage::DeviceLocal, 0,
		    vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer |
		        vk::BufferUsageFlagBits::eShaderDeviceAddress,
		    size + 4u);
		UpdateProbes(*created, 0u, size);
		// The first upload also clears the slots no probe selects.
		created->dirty_begin = 0;
		created->dirty_end   = created->slots.size();
		view = views.insert(views.end(), std::move(created));
	} else {
		// Empty after the first acquire in a draw, since the table refreshes once per draw.
		UpdateProbes(**view, (*view)->changed_begin, (*view)->changed_end);
		(*view)->changed_begin = (*view)->changed_end = 0;
	}
	if (std::ranges::find(m_active, view->get()) == m_active.end()) m_active.push_back(view->get());
	const auto& selected = **view;
	result = {.slots      = selected.buffer->BufferDeviceAddress(),
	          .slot_count = static_cast<uint32_t>(selected.slots.size() - 1u),
	          .kinds      = selected.kinds};
	return true;
}

bool ImageHeap::Refresh(Memory& memory) {
	// GPU writes leave no fault; reading the table downloads them.
	const bool gpu_dirty = m_context.GetBufferCache().HasGpuDirtyBytes(memory.base, memory.size);
	const auto pages     = memory.dirty_pages.size();
	m_runs.clear();
	{
		std::scoped_lock lock {m_mutex};
		// Watch before reading, so a write during the read faults the page again.
		ForEachRun(
		    0, pages, [&](size_t page) { return memory.dirty_pages[page] != 0u; },
		    [&](size_t begin, size_t end) {
			    std::fill(memory.dirty_pages.begin() + begin, memory.dirty_pages.begin() + end,
			              uint8_t {0});
			    Watch<true>(memory, begin, end);
			    m_runs.emplace_back(begin, end);
		    });
	}
	if (gpu_dirty) m_runs.assign(1u, {0u, pages});
	for (size_t run = 0; run < m_runs.size(); ++run) {
		const auto [address, end] = PageRange(memory.base, memory.size, m_runs[run].first,
		                                      m_runs[run].second);
		m_scratch.resize((end - address) / 4u);
		if (!LibKernel::Memory::TryReadBufferBacking(address, m_scratch.data(), end - address)) {
			std::scoped_lock lock {m_mutex};
			for (; run < m_runs.size(); ++run) {
				MarkDirty(memory, m_runs[run].first, m_runs[run].second);
			}
			return false;
		}
		const auto words = std::span(memory.words).subspan((address - memory.base) / 4u,
		                                                   m_scratch.size());
		size_t     first = 0;
		size_t     last  = words.size();
		while (first < last && words[first] == m_scratch[first]) ++first;
		while (first < last && words[last - 1u] == m_scratch[last - 1u]) --last;
		if (first == last) continue;
		std::copy(m_scratch.begin() + first, m_scratch.begin() + last, words.begin() + first);
		const auto offset = address - memory.base;
		for (auto& view: memory.views) {
			Widen(view->changed_begin, view->changed_end, offset + first * 4u, offset + last * 4u);
		}
	}
	return true;
}

void ImageHeap::MarkDirty(Memory& memory, size_t begin, size_t end) {
	ForEachRun(
	    begin, end, [&](size_t page) { return memory.dirty_pages[page] == 0u; },
	    [&](size_t first, size_t last) {
		    std::fill(memory.dirty_pages.begin() + first, memory.dirty_pages.begin() + last,
		              uint8_t {1});
		    Watch<false>(memory, first, last);
	    });
}

template <bool track>
void ImageHeap::Watch(const Memory& memory, size_t begin, size_t end) {
	const auto [address, limit] = PageRange(memory.base, memory.size, begin, end);
	m_page_manager.UpdatePageWatchers<track>(address, limit - address);
}

void ImageHeap::Unwatch(const Memory& memory) {
	ForEachRun(
	    0, memory.dirty_pages.size(), [&](size_t page) { return memory.dirty_pages[page] == 0u; },
	    [&](size_t begin, size_t end) { Watch<false>(memory, begin, end); });
}

void ImageHeap::InvalidateMemory(uint64_t address, uint64_t size) {
	std::scoped_lock lock {m_mutex};
	for (auto& memory: m_memories) {
		const auto begin = std::max(address, memory->base);
		const auto end   = std::min(address + size, memory->base + memory->size);
		if (begin >= end) continue;
		const auto origin = Common::AlignDown(memory->base, TRACKER_PAGE_SIZE);
		MarkDirty(*memory, (begin - origin) / TRACKER_PAGE_SIZE,
		          (end - 1u - origin) / TRACKER_PAGE_SIZE + 1u);
	}
}

void ImageHeap::UpdateProbes(View& view, uint64_t begin, uint64_t end) {
	// A probe reads its descriptor from first + probe * step.
	const uint64_t bytes = view.profile.r128 ? 16u : 32u;
	auto probe = begin < view.first + bytes ? 0u : (begin - view.first - bytes) / view.step + 1u;
	for (; probe < view.count && view.first + probe * view.step < end; ++probe) {
		UpdateProbe(view, probe);
	}
}

void ImageHeap::UpdateProbe(View& view, uint64_t probe) {
	const auto&     words  = view.memory.words;
	const auto      offset = (view.first + probe * view.step) / 4u;
	DescriptorValue value {.dword_count = 8u};
	std::copy_n(words.begin() + offset,
	            std::min<size_t>(view.profile.r128 ? 4u : 8u, words.size() - offset),
	            value.dwords.begin());
	// A buried word stays null in every view until its descriptor changes.
	auto& dead = view.memory.dead[offset];
	if (dead != 0u && dead != DescriptorHash {}(value)) dead = 0u;
	const auto kind = dead != 0u ? 0u : IR::ImageHeapKind(view.profile, value);
	Assign(view, offset, kind == 0u ? 0u : Intern(view, kind, value));
}

void ImageHeap::Assign(View& view, size_t slot, uint32_t entry) {
	const auto previous = std::exchange(view.slots[slot], entry);
	Release(view, previous);
	if (previous != entry) Widen(view.dirty_begin, view.dirty_end, slot, slot + 1u);
}

void ImageHeap::Bury() {
	if (m_unmapped.empty()) return;
	for (const auto& memory: m_memories) {
		for (const auto& view: memory->views) {
			bool buried = false;
			for (const auto& [value, index]: view->elements) {
				const auto& binding = m_elements[index].binding;
				if (binding.image_id && std::ranges::any_of(m_unmapped, [&](GuestRange range) {
					    return ImageRangeOverlaps(binding.desc.info.data, range);
				    })) {
					m_elements[index].stale = buried = true;
				}
			}
			if (!buried) continue;
			for (uint64_t probe = 0; probe < view->count; ++probe) {
				const auto  slot    = (view->first + probe * view->step) / 4u;
				const auto& element = m_elements[view->slots[slot] & ElementMask];
				if (!element.stale) continue;
				memory->dead[slot] = DescriptorHash {}(element.value);
				Assign(*view, slot, 0u);
			}
		}
	}
	m_unmapped.clear();
}

uint32_t ImageHeap::Intern(View& view, uint32_t kind, const DescriptorValue& value) {
	const auto arm = static_cast<uint32_t>(std::ranges::find(view.kind_keys, kind) -
	                                       view.kind_keys.begin());
	if (arm == view.kind_keys.size()) {
		view.kind_keys.push_back(kind);
		view.kinds.push_back(value);
	}
	auto [found, inserted] = view.elements.try_emplace(value, 0u);
	if (inserted) {
		found->second    = Allocate();
		auto& element    = m_elements[found->second];
		element.value    = value;
		element.kind     = kind;
		view.pending.push_back(found->second);
	}
	++m_elements[found->second].refs;
	return arm << IR::ImageHeapKindShift | found->second;
}

void ImageHeap::Release(View& view, uint32_t entry) {
	const auto index = entry & ElementMask;
	if (index == 0u || --m_elements[index].refs != 0u) return;
	auto& element = m_elements[index];
	if (const auto found = view.elements.find(element.value);
	    found != view.elements.end() && found->second == index) {
		view.elements.erase(found);
	}
	Unreference(element);
	view.released.push_back(index);
}

void ImageHeap::Unreference(Element& element) {
	if (auto* image = ImageOf(element)) {
		image->heap_refs.fetch_sub(1u, std::memory_order_relaxed);
		image->tick_accessed_last = m_context.GetCommandScheduler().CurrentTick();
	}
}

Image* ImageHeap::ImageOf(const Element& element) const {
	auto* image = m_context.GetTextureCache().m_slot_images.try_get(element.binding.image_id);
	return image != nullptr && image->serial == element.serial ? image : nullptr;
}

uint32_t ImageHeap::Allocate() {
	auto& scheduler = m_context.GetCommandScheduler();
	while (!m_retired.empty() && scheduler.IsFree(m_retired.front().first)) {
		m_free.push_back(m_retired.front().second);
		m_retired.pop_front();
	}
	if (m_free.empty()) {
		if (m_elements.size() == IR::ImageHeapCapacity) {
			EXIT("image heaps exceed %u resident images\n", IR::ImageHeapCapacity);
		}
		m_elements.emplace_back();
		return static_cast<uint32_t>(m_elements.size() - 1u);
	}
	const auto index = m_free.back();
	m_free.pop_back();
	m_elements[index] = {};
	return index;
}

void ImageHeap::Retire(View& view) {
	const auto tick = m_context.GetCommandScheduler().CurrentTick();
	for (const auto index: view.released) m_retired.emplace_back(tick, index);
	view.released.clear();
}

void ImageHeap::Rebind(RenderExecutor& executor) {
	KYTY_PROFILER_FUNCTION();
	auto&      textures = m_context.GetTextureCache();
	const auto epoch    = Image::heap_epoch.load(std::memory_order_relaxed);
	for (auto* view: m_active) {
		bool       stale = false;
		const auto mark  = [&](uint32_t index) { m_elements[index].stale = stale = true; };
		if (view->resolve_epoch != epoch) {
			view->resolve_epoch = epoch;
			for (const auto& [value, index]: view->elements) {
				const auto& binding = m_elements[index].binding;
				if (!binding.image_id) continue;
				const auto* image = ImageOf(m_elements[index]);
				if (image == nullptr || !image->registered || image->depth_id ||
				    image->binding.needs_rebind) {
					mark(index);
				} else {
					(void)textures.FindTexture(binding.image_id, binding.desc);
				}
			}
		}
		// Discovery applies the color metadata clears written since the last draw.
		std::erase_if(view->dcc, [&](uint32_t index) {
			const auto& element = m_elements[index];
			if (element.refs == 0u || element.stale) return true;
			if (executor.ResolveTexture(IR::ImageHeapResource(view->profile, element.kind),
			                            element.value).image_id == element.binding.image_id) {
				return false;
			}
			mark(index);
			return true;
		});
		if (stale) {
			// A changed image needs a new element; pending draws still read the old one. The new
			// element keeps the old value, since a view takes table changes only when acquired.
			std::erase_if(view->elements,
			              [&](const auto& item) { return m_elements[item.second].stale; });
			for (uint64_t probe = 0; probe < view->count; ++probe) {
				const auto  slot    = (view->first + probe * view->step) / 4u;
				const auto& element = m_elements[view->slots[slot] & ElementMask];
				if (!element.stale) continue;
				const auto value = element.value; // Intern may grow m_elements.
				Assign(*view, slot, Intern(*view, element.kind, value));
			}
		}
		for (const auto index: view->pending) {
			if (m_elements[index].refs == 0u) continue;
			auto binding = executor.ResolveTexture(
			    IR::ImageHeapResource(view->profile, m_elements[index].kind),
			    m_elements[index].value);
			if (binding.image_id) {
				auto& image = textures.GetImage(binding.image_id);
				image.heap_refs.fetch_add(1u, std::memory_order_relaxed);
				m_elements[index].serial = image.serial;
				binding.layout     = image.ReadLayout();
				binding.image_view = textures.FindTexture(binding.image_id, binding.desc);
				if (binding.desc.info.metadata.kind == ImageMetadataKind::Dcc) {
					view->dcc.push_back(index);
				}
				view->away.push_back(index);
			}
			m_elements[index].binding = std::move(binding);
			m_writes.push_back(index);
		}
		view->pending.clear();
	}
	WriteDescriptors();
}

void ImageHeap::WriteDescriptors() {
	if (m_writes.empty()) return;
	std::vector<vk::DescriptorImageInfo> infos;
	std::vector<vk::WriteDescriptorSet>  writes(m_writes.size());
	infos.reserve(m_writes.size());
	for (size_t write = 0; write < m_writes.size(); ++write) {
		const auto& binding = m_elements[m_writes[write]].binding;
		infos.push_back({nullptr, binding.image_view, binding.layout});
		writes[write].dstSet          = m_set;
		writes[write].dstArrayElement = m_writes[write];
		writes[write].descriptorCount = 1;
		writes[write].descriptorType  = vk::DescriptorType::eSampledImage;
		writes[write].pImageInfo      = &infos.back();
	}
	m_context.GetGraphics().device.updateDescriptorSets(static_cast<uint32_t>(writes.size()),
	                                                    writes.data(), 0, nullptr);
	m_writes.clear();
}

void ImageHeap::Commit(CommandBuffer& buffer, vk::PipelineBindPoint bind_point,
                       vk::PipelineLayout layout) {
	KYTY_PROFILER_FUNCTION();
	const auto epoch = Image::heap_epoch.load(std::memory_order_relaxed);
	for (auto* view: m_active) {
		if (view->dirty_begin != view->dirty_end) {
			const auto slots = std::span(view->slots).subspan(view->dirty_begin,
			                                                  view->dirty_end - view->dirty_begin);
			auto& stream = m_context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
			const auto offset = stream.Copy(slots.data(), slots.size_bytes(), sizeof(uint32_t));
			view->buffer->CopyFrom(buffer, stream, offset, view->dirty_begin * sizeof(uint32_t),
			                       slots.size_bytes());
			view->dirty_begin = view->dirty_end = 0;
		}
		Retire(*view);
		if (view->layout_epoch != epoch) {
			view->layout_epoch = epoch;
			view->away.clear();
			for (const auto& [value, index]: view->elements) view->away.push_back(index);
		}
		// This draw's own bindings own the layouts of its attachments and storage images. An image
		// this draw's discovery replaced lives until the tick ends, and its slots still select it.
		std::erase_if(view->away, [&](uint32_t index) {
			const auto& element = m_elements[index];
			auto*       image   = ImageOf(element);
			if (element.refs == 0u || image == nullptr) return true;
			if (image->binding.is_target || image->binding.shader_write ||
			    image->binding.force_general) {
				return false;
			}
			const auto& view_info = element.binding.desc.view_info;
			image->Transit(image->ReadLayout(), vk::AccessFlagBits2::eShaderRead,
			               ImageSubresourceRange {view_info.base_level, view_info.level_count,
			                                      view_info.base_layer, view_info.layer_count},
			               buffer.Handle());
			return true;
		});
	}
	m_active.clear();
	buffer.Handle().bindDescriptorSets(bind_point, layout, IR::ImageHeapSet, 1, &m_set, 0,
	                                   nullptr);
}

void ImageHeap::UnmapMemory(uint64_t address, uint64_t size) {
	// Descriptors of textures in the range stay null even once it is mapped again.
	if (!m_unmapped.empty() && m_unmapped.back().End() == address) {
		m_unmapped.back().size += size;
	} else {
		m_unmapped.push_back({address, size});
	}
	auto&           scheduler = m_context.GetCommandScheduler();
	std::scoped_lock lock {m_mutex};
	std::erase_if(m_memories, [&](const std::unique_ptr<Memory>& memory) {
		if (address >= memory->base + memory->size || memory->base >= address + size) {
			return false;
		}
		Unwatch(*memory);
		for (auto& view: memory->views) {
			std::erase(m_active, view.get());
			for (const auto& [value, index]: view->elements) {
				Unreference(m_elements[index]);
				view->released.push_back(index);
			}
			Retire(*view);
			if (scheduler.Active()) {
				scheduler.DeferOperation([buffer = std::move(view->buffer)] {});
			}
		}
		return true;
	});
}

} // namespace Libs::Graphics
