#include "graphics/host_gpu/renderer/bufferCache.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/resourceMutex.h"
#include "graphics/host_gpu/renderer/textureCache.h"
#include "graphics/host_gpu/utils.h"
#include "graphics/host_gpu/vma.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>

namespace Libs::Graphics {

namespace {
thread_local const void* g_cache_lock_owner = nullptr;

constexpr uint32_t READBACK_MAX_RANGES    = 256;
constexpr uint64_t READBACK_COPY_ALIGNMENT = 64;
constexpr uint64_t READBACK_CAPACITY =
    TRACKER_PAGE_SIZE + READBACK_MAX_RANGES * (READBACK_COPY_ALIGNMENT - 1);

constexpr uint64_t AlignReadbackCopySize(uint64_t size) noexcept {
	return (size + READBACK_COPY_ALIGNMENT - 1) & ~(READBACK_COPY_ALIGNMENT - 1);
}

constexpr bool ShouldLogCommandProcessorReadback(uint64_t serial) noexcept {
	return serial <= 32 || (serial % 256) == 0;
}

struct CommandProcessorReadbackState {
	BufferCache*                          cache       = nullptr;
	PageFaultAccess                       access      = PageFaultAccess::Unknown;
	uint64_t                              vaddr       = 0;
	uint64_t                              size        = 0;
	uint64_t                              page        = 0;
	uint64_t                              serial      = 0;
	uint64_t                              dirty_bytes = 0;
	uint64_t                              packed_bytes = 0;
	uint32_t                              dirty_count = 0;
	std::chrono::steady_clock::time_point started {};
	bool                                  installed = false;
};

thread_local CommandProcessorReadbackState g_command_processor_readback;
std::atomic<uint64_t>                       g_command_processor_readback_serial {0};

struct SharedVulkanBufferOwner {
	explicit SharedVulkanBufferOwner(GraphicContext* context): ctx(context) { EXIT_IF(ctx == nullptr); }

	~SharedVulkanBufferOwner() {
		if (buffer.buffer != nullptr) {
			VulkanDeleteBuffer(ctx, &buffer);
		}
	}

	GraphicContext* ctx = nullptr;
	VulkanBuffer   buffer;
};

std::shared_ptr<VulkanBuffer> MakeSharedVulkanBuffer(GraphicContext* ctx) {
	auto owner = std::make_shared<SharedVulkanBufferOwner>(ctx);
	return {owner, &owner->buffer};
}

std::vector<std::unique_ptr<PageManager::BackingWrite>>
ReserveBackingWrites(PageManager& page_manager, const std::vector<RangeSet::Range>& ranges) {
	if (ranges.empty()) {
		EXIT("BufferCache: cannot reserve empty backing-write ranges\n");
	}
	std::vector<std::unique_ptr<PageManager::BackingWrite>> writes;
	writes.reserve(ranges.size());
	uint64_t begin = 0;
	uint64_t end   = 0;
	for (const auto& range: ranges) {
		if (range.address == 0 || range.size == 0 || range.size > UINT64_MAX - range.address ||
		    range.address + range.size > UINT64_MAX - (TRACKER_PAGE_SIZE - 1)) {
			EXIT("BufferCache: invalid backing-write range, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     range.address, range.size);
		}
		const auto page_begin = range.address & ~(TRACKER_PAGE_SIZE - 1);
		const auto page_end =
		    (range.address + range.size + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
		if (begin != 0 && page_begin > end) {
			writes.push_back(
			    std::make_unique<PageManager::BackingWrite>(page_manager, begin, end - begin));
			begin = 0;
		}
		if (begin == 0) {
			begin = page_begin;
			end   = page_end;
		} else {
			end = std::max(end, page_end);
		}
	}
	writes.push_back(std::make_unique<PageManager::BackingWrite>(page_manager, begin, end - begin));
	return writes;
}

void ValidateDirtyPages(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
                        const char* operation) noexcept {
	if (vaddr == 0 || size == 0 || size > UINT64_MAX - vaddr ||
	    (vaddr & (TRACKER_PAGE_SIZE - 1)) != 0 || (size & (TRACKER_PAGE_SIZE - 1)) != 0) {
		EXIT("BufferCache: invalid dirty-page validation range\n");
	}
	for (auto page = vaddr; page < vaddr + size; page += TRACKER_PAGE_SIZE) {
		bool found = false;
		dirty.ForEachIntersection(page, TRACKER_PAGE_SIZE,
		                          [&found](RangeSet::Range) { found = true; });
		if (!found) {
			EXIT("BufferCache: GPU-dirty tracker page has no dirty bytes, operation=%s "
			     "addr=0x%016" PRIx64 "\n",
			     operation, page);
		}
	}
}

class FaultSafeCacheLock final {
public:
	FaultSafeCacheLock(const void* owner, Common::Mutex& mutex): m_mutex(mutex) {
		if (g_cache_lock_owner != nullptr) {
			EXIT("BufferCache: recursive cache lock acquisition, current=%p\n",
			     static_cast<const void*>(g_cache_lock_owner));
		}
		g_cache_lock_owner = owner;
		m_mutex.Lock();
	}
	~FaultSafeCacheLock() {
		m_mutex.Unlock();
		g_cache_lock_owner = nullptr;
	}

private:
	Common::Mutex& m_mutex;
};

uint64_t AlignDown(uint64_t value) {
	return value & ~(BufferCache::CACHING_PAGE_SIZE - 1);
}

uint64_t AlignUp(uint64_t value) {
	if (value > UINT64_MAX - (BufferCache::CACHING_PAGE_SIZE - 1)) {
		EXIT("BufferCache: address alignment overflow, value=0x%016" PRIx64 "\n", value);
	}
	return (value + BufferCache::CACHING_PAGE_SIZE - 1) & ~(BufferCache::CACHING_PAGE_SIZE - 1);
}

bool PageOverlaps(uint64_t left, uint64_t left_size, uint64_t right, uint64_t right_size) {
	const auto left_begin  = left & ~(TRACKER_PAGE_SIZE - 1);
	const auto left_end    = (left + left_size + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
	const auto right_begin = right & ~(TRACKER_PAGE_SIZE - 1);
	const auto right_end = (right + right_size + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
	return left_begin < right_end && right_begin < left_end;
}
} // namespace

bool MergeOverlappingBufferCacheRange(BufferCacheRange* merged,
                                      BufferCacheRange  candidate) noexcept {
	if (merged == nullptr || merged->address == 0 || merged->size == 0 || candidate.address == 0 ||
	    candidate.size == 0 || merged->size > UINT64_MAX - merged->address ||
	    candidate.size > UINT64_MAX - candidate.address) {
		EXIT("BufferCache: invalid overlap-merge range\n");
	}
	const auto merged_end    = merged->address + merged->size;
	const auto candidate_end = candidate.address + candidate.size;
	if (merged->address >= candidate_end || candidate.address >= merged_end) {
		return false;
	}
	const auto address = std::min(merged->address, candidate.address);
	const auto end     = std::max(merged_end, candidate_end);
	*merged            = {.address = address, .size = end - address};
	return true;
}

bool CanMergeBufferCacheQueueMask(uint64_t queue_mask, uint32_t queue) noexcept {
	return queue < 64 && (queue_mask & ~(uint64_t {1} << queue)) == 0;
}

bool CanReadbackBufferCacheQueueMask(uint64_t queue_mask, uint32_t queue) noexcept {
	return queue < 64 && queue_mask == (uint64_t {1} << queue);
}

struct BufferCache::CachedBuffer {
	uint64_t                      vaddr      = 0;
	uint64_t                      size       = 0;
	uint64_t                      queue_mask = 0;
	GraphicContext*               ctx        = nullptr;
	std::shared_ptr<VulkanBuffer> buffer;
};

struct BufferCache::CommandProcessorReadbackResources {
	GraphicContext*                ctx    = nullptr;
	int                            queue  = -1;
	VulkanBuffer                   buffer {};
	void*                          mapped = nullptr;
	std::unique_ptr<CommandBuffer> command;
};

struct BufferCache::ReadbackWorker {
	static constexpr uint32_t MAX_RANGES = READBACK_MAX_RANGES;
	enum class State : uint32_t {
		Uninitialized,
		InitRequested,
		Idle,
		Claimed,
		Requested,
		Ready,
		Installed,
		Completed,
		Stopping,
		Stopped
	};
	static_assert(std::atomic<State>::is_always_lock_free);

	struct Range {
		uint64_t address = 0;
		uint32_t size    = 0;
		uint32_t offset  = 0;
	};

	explicit ReadbackWorker(BufferCache& owner): cache(owner), thread([this] { Run(); }) {}

	~ReadbackWorker() {
		auto expected = State::Idle;
		if (!state.compare_exchange_strong(expected, State::Stopping, std::memory_order_acq_rel)) {
			expected = State::Uninitialized;
			if (!state.compare_exchange_strong(expected, State::Stopping,
			                                   std::memory_order_acq_rel)) {
				EXIT("BufferCache: cannot stop readback worker from state %u\n",
				     static_cast<uint32_t>(expected));
			}
		}
		state.notify_all();
		thread.join();
		if (state.load(std::memory_order_acquire) != State::Stopped) {
			EXIT("BufferCache: readback worker did not reach stopped state\n");
		}
	}

	void Prepare(GraphicContext* graphic_context) {
		if (graphic_context == nullptr) {
			EXIT("BufferCache: cannot prepare readback without a graphics context\n");
		}
		auto current = state.load(std::memory_order_acquire);
		while (current == State::InitRequested ||
		       (current == State::Claimed && (command == nullptr || mapped == nullptr ||
		                                      readback.buffer == nullptr))) {
			state.wait(current, std::memory_order_acquire);
			current = state.load(std::memory_order_acquire);
		}
		if (current != State::Uninitialized) {
			if (current == State::Stopping || current == State::Stopped) {
				EXIT("BufferCache: cannot prepare a stopping readback worker, state=%u\n",
				     static_cast<uint32_t>(current));
			}
			if (ctx != graphic_context || command == nullptr || mapped == nullptr ||
			    readback.buffer == nullptr) {
				EXIT("BufferCache: initialized readback worker has invalid resources, state=%u "
				     "ctx=%p requested=%p "
				     "command=%p mapped=%p buffer=%p\n",
				     static_cast<uint32_t>(current), static_cast<const void*>(ctx),
				     static_cast<const void*>(graphic_context),
				     static_cast<const void*>(command.get()), static_cast<const void*>(mapped),
				     static_cast<const void*>(readback.buffer));
			}
			return;
		}
		auto expected = State::Uninitialized;
		if (!state.compare_exchange_strong(expected, State::Claimed, std::memory_order_acq_rel)) {
			EXIT("BufferCache: readback prepare requires uninitialized state, state=%u\n",
			     static_cast<uint32_t>(expected));
		}
		ctx = graphic_context;
		state.store(State::InitRequested, std::memory_order_release);
		state.notify_all();
		while ((current = state.load(std::memory_order_acquire)) != State::Idle) {
			if (current != State::InitRequested) {
				EXIT("BufferCache: invalid readback initialization state %u\n",
				     static_cast<uint32_t>(current));
			}
			state.wait(current, std::memory_order_acquire);
		}
		if (command == nullptr || mapped == nullptr || readback.buffer == nullptr) {
			EXIT("BufferCache: readback initialization produced invalid resources, command=%p "
			     "mapped=%p buffer=%p\n",
			     static_cast<const void*>(command.get()), static_cast<const void*>(mapped),
			     static_cast<const void*>(readback.buffer));
		}
	}

	void Request(PageFaultAccess fault_access, uint64_t fault_vaddr, uint64_t fault_size) noexcept {
		const bool submissions_prepaused_now = GraphicsRunSubmissionLockHeld();
		const bool unsafe_gpu_lock = GraphicsRunGpuLockHeld() && !submissions_prepaused_now;
		if (GraphicsRunIsCommandProcessorThread() || unsafe_gpu_lock || LabelInCallback() ||
		    g_cache_lock_owner != nullptr ||
		    ctx == nullptr || command == nullptr || mapped == nullptr ||
		    readback.buffer == nullptr) {
			EXIT("BufferCache: unsafe readback request context, command_thread=%d "
			     "submission_lock=%d "
			     "gpu_lock=%d label_callback=%d cache_lock=%p ctx=%p command=%p mapped=%p "
			     "buffer=%p\n",
			     GraphicsRunIsCommandProcessorThread(), GraphicsRunSubmissionLockHeld(),
			     GraphicsRunGpuLockHeld(), LabelInCallback(),
			     static_cast<const void*>(g_cache_lock_owner), static_cast<const void*>(ctx),
			     static_cast<const void*>(command.get()), static_cast<const void*>(mapped),
			     static_cast<const void*>(readback.buffer));
		}
		State expected = State::Idle;
		while (!state.compare_exchange_weak(expected, State::Claimed, std::memory_order_acq_rel)) {
			if (expected == State::Stopping || expected == State::Stopped) {
				EXIT("BufferCache: readback requested while worker is stopping, state=%u\n",
				     static_cast<uint32_t>(expected));
			}
			state.wait(expected, std::memory_order_acquire);
			expected = State::Idle;
		}
		access      = fault_access;
		vaddr       = fault_vaddr;
		size        = fault_size;
		range_count = 0;
		submissions_prepaused = submissions_prepaused_now;
		state.store(State::Requested, std::memory_order_release);
		state.notify_all();
		while (true) {
			const auto current = state.load(std::memory_order_acquire);
			if (current == State::Ready) {
				break;
			}
			if (current != State::Requested) {
				EXIT("BufferCache: invalid state while waiting for readback, state=%u\n",
				     static_cast<uint32_t>(current));
			}
			state.wait(current, std::memory_order_acquire);
		}
	}

	[[nodiscard]] bool Complete(PageFaultAccess fault_access, uint64_t fault_vaddr,
	                            uint64_t fault_size) noexcept {
		const auto current = state.load(std::memory_order_acquire);
		if (current == State::Uninitialized || current == State::InitRequested ||
		    current == State::Idle || current == State::Stopping || current == State::Stopped) {
			return false;
		}
		if (current != State::Ready) {
			EXIT("BufferCache: active readback has invalid completion state %u\n",
			     static_cast<uint32_t>(current));
		}
		if (access != fault_access || vaddr != fault_vaddr || size != fault_size) {
			EXIT("BufferCache: mismatched active readback completion\n");
		}
		if (range_count == 0 || range_count > MAX_RANGES) {
			EXIT("BufferCache: invalid completed readback range count %u\n", range_count);
		}
		for (uint32_t i = 0; i < range_count; i++) {
			const auto& range = ranges[i];
			Libs::LibKernel::Memory::WriteBacking(range.address, data.data() + range.offset,
			                                       range.size);
		}
		if (!cache.m_memory_tracker.CompleteCpuFault(vaddr, size, access, true)) {
			EXIT("BufferCache: failed to complete downloaded CPU fault, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " access=%u\n",
			     vaddr, size, static_cast<uint32_t>(access));
		}
		state.store(State::Installed, std::memory_order_release);
		state.notify_all();
		return true;
	}

	void Release(PageFaultAccess fault_access, uint64_t fault_vaddr, uint64_t fault_size) noexcept {
		const auto current = state.load(std::memory_order_acquire);
		if (current == State::Uninitialized || current == State::InitRequested ||
		    current == State::Idle || current == State::Stopping || current == State::Stopped) {
			return;
		}
		if (current != State::Installed) {
			EXIT("BufferCache: active readback has invalid release state %u\n",
			     static_cast<uint32_t>(current));
		}
		if (access != fault_access || vaddr != fault_vaddr || size != fault_size) {
			EXIT("BufferCache: mismatched active readback release\n");
		}
		state.store(State::Completed, std::memory_order_release);
		state.notify_all();
		while (true) {
			const auto current = state.load(std::memory_order_acquire);
			if (current == State::Idle) {
				break;
			}
			if (current != State::Completed) {
				EXIT("BufferCache: invalid state while releasing readback, state=%u\n",
				     static_cast<uint32_t>(current));
			}
			state.wait(current, std::memory_order_acquire);
		}
	}

	void Run() noexcept {
		while (true) {
			auto current = state.load(std::memory_order_acquire);
			while (current == State::Uninitialized || current == State::Idle) {
				state.wait(current, std::memory_order_acquire);
				current = state.load(std::memory_order_acquire);
			}
			if (current == State::Stopping) {
				command.reset();
				if (mapped != nullptr) {
					VulkanUnmapMemory(ctx, &readback.memory);
					mapped = nullptr;
				}
				if (readback.buffer != nullptr) {
					VulkanDeleteBuffer(ctx, &readback);
				}
				state.store(State::Stopped, std::memory_order_release);
				state.notify_all();
				return;
			}
			if (current == State::InitRequested) {
				if (ctx == nullptr || command != nullptr || mapped != nullptr ||
				    readback.buffer != nullptr) {
					EXIT("BufferCache: invalid resources before readback initialization, ctx=%p "
					     "command=%p mapped=%p buffer=%p\n",
					     static_cast<const void*>(ctx), static_cast<const void*>(command.get()),
					     static_cast<const void*>(mapped),
					     static_cast<const void*>(readback.buffer));
				}
				const auto family = ctx->queues[GraphicContext::QUEUE_UTIL].family;
				if (family == static_cast<uint32_t>(-1) ||
				    ctx->queues[GraphicContext::QUEUE_GFX].family != family) {
					EXIT("BufferCache: utility and graphics queues must share a valid family, "
					     "util=%u "
					     "gfx=%u\n",
					     family, ctx->queues[GraphicContext::QUEUE_GFX].family);
				}
				for (int i = GraphicContext::QUEUE_COMPUTE_START;
				     i < GraphicContext::QUEUE_COMPUTE_START + GraphicContext::QUEUE_COMPUTE_NUM;
				     i++) {
					if (ctx->queues[i].family != family) {
						EXIT("BufferCache: compute queue %d family mismatch, expected=%u "
						     "actual=%u\n",
						     i, family, ctx->queues[i].family);
					}
				}
				readback.usage           = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
				readback.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
				                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
				VulkanCreateBuffer(ctx, READBACK_CAPACITY, &readback);
				VulkanMapMemory(ctx, &readback.memory, &mapped);
				command = std::make_unique<CommandBuffer>(GraphicContext::QUEUE_UTIL);
				state.store(State::Idle, std::memory_order_release);
				state.notify_all();
				continue;
			}
			if (current != State::Requested) {
				EXIT("BufferCache: worker received unsupported state %u\n",
				     static_cast<uint32_t>(current));
			}

			std::optional<GraphicsRunSubmissionLock> submissions;
			if (!submissions_prepaused) {
				submissions.emplace();
			}
			const auto         page = vaddr & ~(TRACKER_PAGE_SIZE - 1);
			FaultSafeCacheLock lock(&cache, cache.m_mutex);
			auto                      it = cache.m_buffers.upper_bound(vaddr);
			if (it == cache.m_buffers.begin()) {
				EXIT("BufferCache: readback address has no cached buffer, addr=0x%016" PRIx64 "\n",
				     vaddr);
			}
			--it;
			auto& cached = *it->second;
			if (vaddr < cached.vaddr || vaddr >= cached.vaddr + cached.size ||
			    page < cached.vaddr || TRACKER_PAGE_SIZE > cached.size - (page - cached.vaddr)) {
				EXIT("BufferCache: readback page is outside cached buffer, fault=0x%016" PRIx64
				     " page=0x%016" PRIx64 " buffer=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
				     vaddr, page, cached.vaddr, cached.size);
			}
			uint32_t                                      data_offset = 0;
			std::array<VkBufferCopy, MAX_RANGES>          copies {};
			std::array<VkBufferMemoryBarrier, MAX_RANGES> barriers {};
			cache.m_gpu_modified_ranges.ForEachIntersection(
			    page, TRACKER_PAGE_SIZE, [&](RangeSet::Range range) {
				    if (range.size > UINT32_MAX || data_offset > data.size() ||
				        range.size > data.size() - data_offset || range.address < cached.vaddr ||
				        range.size > cached.size - (range.address - cached.vaddr) ||
				        range_count == MAX_RANGES || (range.address - cached.vaddr) % 4 != 0 ||
				        data_offset % 4 != 0 || range.size % 4 != 0) {
					    EXIT("BufferCache: invalid GPU-modified readback range, addr=0x%016" PRIx64
					         " size=0x%016" PRIx64 " offset=%u count=%u\n",
					         range.address, range.size, data_offset, range_count);
				    }
				    auto& out                   = ranges[range_count];
				    out.address                 = range.address;
				    out.size                    = static_cast<uint32_t>(range.size);
				    out.offset                  = data_offset;
				    copies[range_count]         = {.srcOffset = range.address - cached.vaddr,
				                                   .dstOffset = data_offset,
				                                   .size      = range.size};
				    auto& barrier               = barriers[range_count];
				    barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
				    barrier.srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT;
				    barrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
				    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				    barrier.buffer              = cached.buffer->buffer;
				    barrier.offset              = copies[range_count].srcOffset;
				    barrier.size                = range.size;
				    range_count++;
				    const auto slot_size = AlignReadbackCopySize(out.size);
				    if (slot_size > data.size() - data_offset) {
					    EXIT("BufferCache: aligned readback range exceeds persistent capacity, "
					         "offset=%u size=%" PRIu64 " capacity=%" PRIu64 "\n",
					         data_offset, slot_size, static_cast<uint64_t>(data.size()));
				    }
				    data_offset += static_cast<uint32_t>(slot_size);
			    });
			if (range_count == 0 || data_offset == 0) {
				EXIT(
				    "BufferCache: GPU-modified page produced no readback ranges, page=0x%016" PRIx64
				    " count=%u bytes=%u\n",
				    page, range_count, data_offset);
			}
			auto* vk_buffer = command->GetPool()->buffers[command->GetIndex()];
			command->Begin();
			vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, range_count,
			                     barriers.data(), 0, nullptr);
			vkCmdCopyBuffer(vk_buffer, cached.buffer->buffer, readback.buffer, range_count,
			                copies.data());
			command->End();
			command->Execute();
			command->WaitForFenceAndReset();
			std::memcpy(data.data(), mapped, data_offset);
			state.store(State::Ready, std::memory_order_release);
			state.notify_all();
			while ((current = state.load(std::memory_order_acquire)) != State::Completed) {
				if (current != State::Ready && current != State::Installed) {
					EXIT("BufferCache: invalid readback completion state %u\n",
					     static_cast<uint32_t>(current));
				}
				state.wait(current, std::memory_order_acquire);
			}
			cache.m_gpu_modified_ranges.Subtract(page, TRACKER_PAGE_SIZE);
			submissions_prepaused = false;
			state.store(State::Idle, std::memory_order_release);
			state.notify_all();
		}
	}

	BufferCache&                           cache;
	std::atomic<State>                     state {State::Uninitialized};
	GraphicContext*                        ctx = nullptr;
	VulkanBuffer                           readback {};
	void*                                  mapped = nullptr;
	std::unique_ptr<CommandBuffer>         command;
	PageFaultAccess                        access      = PageFaultAccess::Unknown;
	uint64_t                               vaddr       = 0;
	uint64_t                               size        = 0;
	uint32_t                               range_count = 0;
	bool                                   submissions_prepaused = false;
	std::array<Range, MAX_RANGES>          ranges {};
	std::array<uint8_t, READBACK_CAPACITY> data {};
	std::thread                            thread;
};

BufferCache::BufferCache(PageManager& page_manager, ResourceMutex& resource_mutex)
    : m_memory_tracker(page_manager), m_page_manager(page_manager),
      m_resource_mutex(resource_mutex) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	m_readback = std::make_unique<ReadbackWorker>(*this);
}

BufferCache::~BufferCache() {
	m_readback.reset();
	if (m_cp_readback_resources != nullptr) {
		auto& resources = *m_cp_readback_resources;
		resources.command.reset();
		if (resources.mapped != nullptr) {
			VulkanUnmapMemory(resources.ctx, &resources.buffer.memory);
			resources.mapped = nullptr;
		}
		if (resources.buffer.buffer != nullptr) {
			VulkanDeleteBuffer(resources.ctx, &resources.buffer);
		}
		m_cp_readback_resources.reset();
	}
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, cached]: m_buffers) {
		(void)vaddr;
		if (m_memory_tracker.IsRegionGpuModified(cached->vaddr, cached->size)) {
			EXIT("BufferCache: destroyed with GPU-modified buffer, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     cached->vaddr, cached->size);
		}
	}
	if (!m_buffers.empty()) {
		VulkanDeviceWaitIdle(m_buffers.begin()->second->ctx);
	}
	m_buffers.clear();
}

bool BufferCache::InvalidateMemory(PageFaultAccess access, uint64_t vaddr, uint64_t size,
                                   PageFaultPhase phase) noexcept {
	const auto page = vaddr & ~(TRACKER_PAGE_SIZE - 1);
	if (size == 0 || size > page + TRACKER_PAGE_SIZE - vaddr) {
		EXIT("BufferCache: invalid page-fault range, addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	switch (phase) {
		case PageFaultPhase::Invalidate: break;
		case PageFaultPhase::Complete:
			return CompleteCommandProcessorReadback(access, vaddr, size) ||
			       m_readback->Complete(access, vaddr, size) ||
			       m_memory_tracker.CompleteCpuFault(vaddr, size, access, false);
		case PageFaultPhase::Release:
			if (!ReleaseCommandProcessorReadback(access, vaddr, size)) {
				m_readback->Release(access, vaddr, size);
			}
			return true;
		default:
			EXIT("BufferCache: unsupported page-fault phase %u\n", static_cast<uint32_t>(phase));
	}
	const auto action = m_memory_tracker.BeginCpuFault(vaddr, size, access);
	if (action != CpuFaultAction::Download) {
		return action == CpuFaultAction::Continue;
	}
	if (GraphicsRunIsCommandProcessorThread()) {
		if (!GraphicsRunIsCommandProcessorGuestAccess()) {
			EXIT("BufferCache: command-processor fault outside a coherent guest-memory access, "
			     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vaddr, size);
		}
		RequestCommandProcessorReadback(access, vaddr, size);
	} else {
		m_readback->Request(access, vaddr, size);
	}
	return true;
}

void BufferCache::RequestCommandProcessorReadback(PageFaultAccess access, uint64_t vaddr,
                                                  uint64_t size) {
	if (!GraphicsRunIsCommandProcessorThread() || g_command_processor_readback.cache != nullptr) {
		EXIT("BufferCache: invalid nested command-processor readback\n");
	}
	const auto serial =
	    g_command_processor_readback_serial.fetch_add(1, std::memory_order_relaxed) + 1;
	if (serial == 0) {
		EXIT("BufferCache: command-processor readback serial overflow\n");
	}
	const bool log_detail = ShouldLogCommandProcessorReadback(serial);
	if (log_detail) {
		LOGF("BufferCache: command-processor readback request serial=%" PRIu64
		     " addr=0x%016" PRIx64 " size=0x%016" PRIx64 " access=%u poll=%d\n",
		     serial, vaddr, size, static_cast<uint32_t>(access), GraphicsRunIsGuestMemoryPoll());
		Log::Flush();
	}
	const auto page  = vaddr & ~(TRACKER_PAGE_SIZE - 1);
	const auto queue = static_cast<uint32_t>(GraphicsRunBeginCommandProcessorReadback());
	if (log_detail) {
		LOGF("BufferCache: command-processor queue synchronized serial=%" PRIu64
		     " queue=%u page=0x%016" PRIx64 "\n",
		     serial, queue, page);
		Log::Flush();
	}
	if (g_cache_lock_owner != nullptr) {
		EXIT("BufferCache: command-processor readback entered with a cache lock\n");
	}
	g_cache_lock_owner = this;
	m_mutex.Lock();
	if (log_detail) {
		LOGF("BufferCache: command-processor cache locked serial=%" PRIu64
		     " page=0x%016" PRIx64 "\n",
		     serial, page);
		Log::Flush();
	}

	auto it = m_buffers.upper_bound(vaddr);
	if (it == m_buffers.begin()) {
		EXIT("BufferCache: command-processor readback address has no cached buffer, "
		     "addr=0x%016" PRIx64 "\n",
		     vaddr);
	}
	--it;
	auto& cached = *it->second;
	if (vaddr < cached.vaddr || vaddr >= cached.vaddr + cached.size || page < cached.vaddr ||
	    TRACKER_PAGE_SIZE > cached.size - (page - cached.vaddr) || cached.ctx == nullptr ||
	    cached.buffer == nullptr || cached.buffer->buffer == nullptr) {
		EXIT("BufferCache: invalid command-processor readback owner, fault=0x%016" PRIx64
		     " page=0x%016" PRIx64 " buffer=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
		     vaddr, page, cached.vaddr, cached.size);
	}
	if (!CanReadbackBufferCacheQueueMask(cached.queue_mask, queue)) {
		EXIT("BufferCache: command-processor readback cannot synchronize cross-queue ownership, "
		     "addr=0x%016" PRIx64 " used_queues=0x%016" PRIx64 " current_queue=%u\n",
		     vaddr, cached.queue_mask, queue);
	}
	const auto dirty = m_gpu_modified_ranges.Intersections(page, TRACKER_PAGE_SIZE);
	if (dirty.empty()) {
		EXIT("BufferCache: command-processor GPU-dirty page has no dirty byte ranges, "
		     "page=0x%016" PRIx64 "\n",
		     page);
	}
	ValidateDirtyPages(m_gpu_modified_ranges, page, TRACKER_PAGE_SIZE, "command-processor");
	if (m_cp_readback_resources == nullptr) {
		auto resources       = std::make_unique<CommandProcessorReadbackResources>();
		resources->ctx       = cached.ctx;
		resources->buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		resources->buffer.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
		                                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		VulkanCreateBuffer(cached.ctx, READBACK_CAPACITY, &resources->buffer);
		VulkanMapMemory(cached.ctx, &resources->buffer.memory, &resources->mapped);
		m_cp_readback_resources = std::move(resources);
	}
	auto& resources = *m_cp_readback_resources;
	if (resources.ctx != cached.ctx || resources.mapped == nullptr ||
	    resources.buffer.buffer == nullptr) {
		EXIT("BufferCache: invalid command-processor readback resources\n");
	}
	if (queue >= GraphicContext::QUEUES_NUM || cached.ctx->queues[queue].family == UINT32_MAX ||
	    cached.ctx->queues[queue].vk_queue == nullptr) {
		EXIT("BufferCache: command-processor readback has invalid Vulkan queue %u\n", queue);
	}
	if (resources.command == nullptr || resources.queue != static_cast<int>(queue)) {
		resources.command.reset();
		resources.command = std::make_unique<CommandBuffer>(static_cast<int>(queue));
		resources.queue   = static_cast<int>(queue);
	}
	if (resources.command->IsInvalid() || resources.command->GetQueue() != static_cast<int>(queue)) {
		EXIT("BufferCache: failed to initialize owning-queue readback command buffer, queue=%u\n",
		     queue);
	}
	if (dirty.size() > ReadbackWorker::MAX_RANGES) {
		EXIT("BufferCache: too many command-processor dirty ranges, page=0x%016" PRIx64
		     " count=%" PRIu64 " limit=%u\n",
		     page, static_cast<uint64_t>(dirty.size()), ReadbackWorker::MAX_RANGES);
	}
	std::array<VkBufferCopy, ReadbackWorker::MAX_RANGES>          copies {};
	std::array<VkBufferMemoryBarrier, ReadbackWorker::MAX_RANGES> barriers {};
	uint64_t                                                       dirty_bytes  = 0;
	uint64_t                                                       packed_bytes = 0;
	uint32_t                                                       dirty_count = 0;
	for (const auto& range: dirty) {
		if (range.address < page || range.size == 0 ||
		    range.size > TRACKER_PAGE_SIZE - (range.address - page) ||
		    range.address < cached.vaddr ||
		    range.size > cached.size - (range.address - cached.vaddr) ||
		    dirty_bytes > TRACKER_PAGE_SIZE || range.size > TRACKER_PAGE_SIZE - dirty_bytes ||
		    (range.address - cached.vaddr) % 4 != 0 || packed_bytes % 4 != 0 ||
		    range.size % 4 != 0) {
			EXIT("BufferCache: invalid command-processor dirty range, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " page=0x%016" PRIx64 " packed=0x%016" PRIx64 "\n",
			     range.address, range.size, page, dirty_bytes);
		}
		copies[dirty_count] = {.srcOffset = range.address - cached.vaddr,
		                       .dstOffset = packed_bytes,
		                       .size      = range.size};
		auto& barrier               = barriers[dirty_count];
		barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		barrier.srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.buffer              = cached.buffer->buffer;
		barrier.offset              = copies[dirty_count].srcOffset;
		barrier.size                = range.size;
		const auto slot_size = AlignReadbackCopySize(range.size);
		if (packed_bytes > READBACK_CAPACITY || slot_size > READBACK_CAPACITY - packed_bytes) {
			EXIT("BufferCache: aligned command-processor readback exceeds persistent capacity, "
			     "packed=%" PRIu64 " size=%" PRIu64 " capacity=%" PRIu64 "\n",
			     packed_bytes, slot_size, READBACK_CAPACITY);
		}
		packed_bytes += slot_size;
		dirty_bytes += range.size;
		dirty_count++;
	}
	if (dirty_count == 0 || dirty_bytes == 0) {
		EXIT("BufferCache: command-processor readback produced no packed ranges\n");
	}
	const auto started = std::chrono::steady_clock::now();
	if (log_detail) {
		LOGF("BufferCache: command-processor readback begin serial=%" PRIu64
		     " queue=%u page=0x%016" PRIx64 " ranges=%u bytes=%" PRIu64
		     " packed=%" PRIu64 "\n",
		     serial, queue, page, dirty_count, dirty_bytes, packed_bytes);
		Log::Flush();
	}
	auto* vk_command = resources.command->GetPool()->buffers[resources.command->GetIndex()];
	resources.command->Begin();
	vkCmdPipelineBarrier(vk_command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, dirty_count,
	                     barriers.data(), 0, nullptr);
	vkCmdCopyBuffer(vk_command, cached.buffer->buffer, resources.buffer.buffer, dirty_count,
	                copies.data());
	resources.command->End();
	resources.command->Execute();
	resources.command->WaitForFenceAndReset();
	if (log_detail) {
		LOGF("BufferCache: command-processor GPU copy complete serial=%" PRIu64 "\n", serial);
		Log::Flush();
	}
	for (uint32_t i = 0; i < dirty_count; i++) {
		Libs::LibKernel::Memory::WriteBacking(
		    dirty[i].address,
		    static_cast<const uint8_t*>(resources.mapped) + copies[i].dstOffset, dirty[i].size);
	}
	g_command_processor_readback = {
	    .cache       = this,
	    .access      = access,
	    .vaddr       = vaddr,
	    .size        = size,
	    .page        = page,
	    .serial      = serial,
	    .dirty_bytes = dirty_bytes,
	    .packed_bytes = packed_bytes,
	    .dirty_count = dirty_count,
	    .started     = started};
}

bool BufferCache::CompleteCommandProcessorReadback(PageFaultAccess access, uint64_t vaddr,
                                                   uint64_t size) noexcept {
	auto& pending = g_command_processor_readback;
	if (pending.cache == nullptr) {
		return false;
	}
	if (pending.cache != this || pending.access != access || pending.vaddr != vaddr ||
	    pending.size != size || pending.installed) {
		EXIT("BufferCache: mismatched active command-processor readback completion\n");
	}
	if (!m_memory_tracker.CompleteCpuFault(vaddr, size, access, true)) {
		EXIT("BufferCache: failed to complete command-processor readback, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " access=%u\n",
		     vaddr, size, static_cast<uint32_t>(access));
	}
	m_gpu_modified_ranges.Subtract(pending.page, TRACKER_PAGE_SIZE);
	pending.installed = true;
	if (ShouldLogCommandProcessorReadback(pending.serial)) {
		LOGF("BufferCache: command-processor readback complete serial=%" PRIu64
		     " page=0x%016" PRIx64 " ranges=%u bytes=%" PRIu64 "\n",
		     pending.serial, pending.page, pending.dirty_count, pending.dirty_bytes);
	}
	return true;
}

bool BufferCache::ReleaseCommandProcessorReadback(PageFaultAccess access, uint64_t vaddr,
                                                  uint64_t size) noexcept {
	auto& pending = g_command_processor_readback;
	if (pending.cache == nullptr) {
		return false;
	}
	if (pending.cache != this || pending.access != access || pending.vaddr != vaddr ||
	    pending.size != size || !pending.installed) {
		EXIT("BufferCache: mismatched active command-processor readback release\n");
	}
	const auto serial        = pending.serial;
	const auto page          = pending.page;
	const auto dirty_count   = pending.dirty_count;
	const auto dirty_bytes   = pending.dirty_bytes;
	const auto elapsed_micro = std::chrono::duration_cast<std::chrono::microseconds>(
	                               std::chrono::steady_clock::now() - pending.started)
	                               .count();
	pending = {};
	m_mutex.Unlock();
	g_cache_lock_owner = nullptr;
	GraphicsRunEndCommandProcessorReadback();
	if (ShouldLogCommandProcessorReadback(serial)) {
		LOGF("BufferCache: command-processor readback release serial=%" PRIu64
		     " page=0x%016" PRIx64 " ranges=%u bytes=%" PRIu64 " elapsed_us=%" PRIi64 "\n",
		     serial, page, dirty_count, dirty_bytes, static_cast<int64_t>(elapsed_micro));
	}
	return true;
}

void BufferCache::UnmapMemory(uint64_t vaddr, uint64_t size) {
	GraphicsRunSubmissionLock submissions;
	FaultSafeCacheLock        lock(this, m_mutex);
	for (const auto& [begin, cached]: m_buffers) {
		if (vaddr < begin + cached->size && begin < vaddr + size &&
		    (vaddr > begin || size < cached->size || vaddr + size < begin + cached->size)) {
			EXIT("BufferCache: partial buffer unmap is unsupported, unmap=0x%016" PRIx64
			     "+0x%016" PRIx64 " buffer=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
			     vaddr, size, begin, cached->size);
		}
	}
	for (const auto& [begin, cached]: m_buffers) {
		const auto offset = begin >= vaddr ? begin - vaddr : UINT64_MAX;
		if (offset > size || cached->size > size - offset ||
		    !m_memory_tracker.IsRegionGpuModified(begin, cached->size)) {
			continue;
		}
		const auto dirty = m_gpu_modified_ranges.Intersections(begin, cached->size);
		if (dirty.empty()) {
			EXIT("BufferCache: GPU-modified buffer has no dirty ranges, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     begin, cached->size);
		}
		auto     backing_writes = ReserveBackingWrites(m_page_manager, dirty);
		uint64_t downloaded     = 0;
		m_memory_tracker.ForEachDownloadRange<true>(
		    begin, cached->size,
		    [&](uint64_t address, uint64_t bytes) noexcept {
			    ValidateDirtyPages(m_gpu_modified_ranges, address, bytes, "unmap");
		    },
		    [&](uint64_t address, uint64_t bytes) noexcept {
			    const auto ranges = m_gpu_modified_ranges.Intersections(address, bytes);
			    if (ranges.empty()) {
				    EXIT("BufferCache: GPU-dirty pages have no dirty byte ranges, "
				         "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
				         address, bytes);
			    }
			    for (const auto& range: ranges) {
				    if (range.address < begin || range.address - begin > cached->size ||
				        range.size > cached->size - (range.address - begin) ||
				        downloaded > UINT64_MAX - range.size) {
					    EXIT("BufferCache: invalid unmap download range, addr=0x%016" PRIx64
					         " size=0x%016" PRIx64 " buffer=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
					         range.address, range.size, begin, cached->size);
				    }
				    std::vector<uint8_t> data(range.size);
				    UtilDownloadBuffer(cached->ctx, cached->buffer.get(), range.address - begin,
				                       data.data(), range.size);
				    Libs::LibKernel::Memory::WriteBacking(range.address, data.data(), data.size());
				    downloaded += range.size;
			    }
		    });
		if (downloaded == 0) {
			EXIT("BufferCache: GPU-modified buffer downloaded no bytes, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     begin, cached->size);
		}
		m_memory_tracker.MarkRegionAsCpuModified(begin, cached->size);
		m_gpu_modified_ranges.Subtract(begin, cached->size);
	}
	if (!m_gpu_modified_ranges.Intersections(vaddr, size).empty()) {
		EXIT("BufferCache: unmap retained dirty byte ranges, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	m_memory_tracker.UntrackMemory(vaddr, size);
	for (auto it = m_buffers.begin(); it != m_buffers.end();) {
		const auto offset = (it->first >= vaddr ? it->first - vaddr : UINT64_MAX);
		if (offset <= size && it->second->size <= size - offset) {
			it = m_buffers.erase(it);
		} else {
			++it;
		}
	}
}

std::pair<VulkanBuffer*, uint64_t> BufferCache::ObtainBuffer(CommandBuffer*  command,
                                                             GraphicContext* ctx, uint64_t vaddr,
                                                             uint64_t size, bool is_written,
                                                             bool is_read, bool is_formatted) {
	if (command == nullptr || command->IsInvalid() || command->IsExecute() || ctx == nullptr ||
	    vaddr == 0 || size == 0 || size > UINT64_MAX - vaddr || command->GetQueue() < 0 ||
	    command->GetQueue() >= 64) {
		EXIT("BufferCache: invalid buffer request, command=%p queue=%d ctx=%p addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     static_cast<const void*>(command), command == nullptr ? -1 : command->GetQueue(),
		     static_cast<const void*>(ctx), vaddr, size);
	}
	const auto queue      = static_cast<uint32_t>(command->GetQueue());
	const auto queue_mask = uint64_t {1} << queue;
	ValidateGpuAccess(vaddr, size, is_read, is_written);
	const auto      begin = AlignDown(vaddr);
	const auto      end   = AlignUp(vaddr + size);
	std::lock_guard transaction(m_resource_mutex);
	// Use the stream-buffer fast path before image/buffer alias handling. Clean image and metadata
	// views may coexist with a small CPU-current read; Kyty's separate image trackers require
	// GPU-dirty ownership guards here. Read physical backing so
	// host page protection is irrelevant.
	if (is_read && !is_written && size <= CACHING_PAGE_SIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size) &&
	    !m_texture_cache->HasGpuModifiedRangeOverlap(vaddr, size) &&
	    !m_texture_cache->IsMetaGpuModified(vaddr, size)) {
		std::array<uint8_t, CACHING_PAGE_SIZE> guest_data;
		if (Libs::LibKernel::Memory::TryReadBacking(vaddr, guest_data.data(), size)) {
			VulkanBuffer* stream_buffer = nullptr;
			VkDeviceSize  stream_offset = 0;
			VkDeviceSize  stream_range  = 0;
			const auto stream_alignment = std::max<uint64_t>(ctx->StorageMinAlignment(), 16);
			if (UploadHostData(command, ctx, guest_data.data(), size, stream_alignment,
			                   &stream_buffer, &stream_offset, &stream_range)) {
				return {stream_buffer, stream_offset};
			}
		}
	}
	if (m_texture_cache->HasMetaOverlap(begin, end - begin)) {
		EXIT("BufferCache: buffer aliases metadata pages, addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     "\n",
		     begin, end - begin);
	}
	// Cache allocations are tracker-page aligned, but byte-disjoint buffers and images may share
	// an edge page. Clean read-only buffer and image views may coexist; Kyty retains a hard failure
	// when either cache owns newer GPU bytes. Only a formatted buffer write
	// performs the image ownership transition below.
	if (m_texture_cache->HasPageOverlap(begin, end - begin) &&
	    m_texture_cache->HasRangeOverlap(vaddr, size)) {
		const bool coherent_read =
		    is_read && !is_written && !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
		    !m_texture_cache->HasGpuModifiedRangeOverlap(vaddr, size);
		if (!coherent_read) {
			if (!is_written || !is_formatted) {
				EXIT("BufferCache: unsupported buffer/image alias, addr=0x%016" PRIx64
				     " size=0x%016" PRIx64 " read=%d written=%d formatted=%d\n",
				     vaddr, size, is_read, is_written, is_formatted);
			}
			m_texture_cache->InvalidateMemoryFromGPU(vaddr, size, true);
		}
	}
	if (is_written) {
		m_readback->Prepare(ctx);
	}
	FaultSafeCacheLock lock(this, m_mutex);

	auto it = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		auto       previous = std::prev(it);
		const auto offset   = vaddr - previous->second->vaddr;
		if (offset <= previous->second->size && size <= previous->second->size - offset) {
			it = previous;
		}
	}
	if (it == m_buffers.end() || it->second->vaddr > vaddr ||
	    size > it->second->size - (vaddr - it->second->vaddr)) {
		BufferCacheRange merged {.address = begin, .size = end - begin};
		using BufferIterator = decltype(m_buffers.begin());
		std::vector<BufferIterator> overlaps;
		auto                        first = m_buffers.lower_bound(begin);
		if (first != m_buffers.begin()) {
			auto previous = std::prev(first);
			if (MergeOverlappingBufferCacheRange(
			        &merged, {previous->second->vaddr, previous->second->size})) {
				first = previous;
			}
		}
		for (auto candidate = first; candidate != m_buffers.end(); ++candidate) {
			if (candidate->first >= merged.address + merged.size) {
				break;
			}
			if (MergeOverlappingBufferCacheRange(
			        &merged, {candidate->second->vaddr, candidate->second->size})) {
				overlaps.push_back(candidate);
			}
		}

		if (!overlaps.empty()) {
			for (const auto overlap: overlaps) {
				auto& old = *overlap->second;
				if (old.ctx != ctx || old.buffer == nullptr || old.buffer->buffer == nullptr) {
					EXIT("BufferCache: invalid overlapping buffer owner, addr=0x%016" PRIx64
					     " size=0x%016" PRIx64 " owner_ctx=%p requested_ctx=%p buffer=%p\n",
					     old.vaddr, old.size, static_cast<const void*>(old.ctx),
					     static_cast<const void*>(ctx), static_cast<const void*>(old.buffer.get()));
				}
				if (!CanMergeBufferCacheQueueMask(old.queue_mask, queue)) {
					EXIT("BufferCache: cross-queue overlap merge is unsupported, "
					     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 " used_queues=0x%016" PRIx64
					     " requested_queue=%u\n",
					     old.vaddr, old.size, old.queue_mask, queue);
				}
				std::vector<std::pair<uint64_t, uint64_t>> uploads;
				m_memory_tracker.ForEachUploadRange(
				    old.vaddr, old.size, false,
				    [&](uint64_t upload_vaddr, uint64_t upload_size) noexcept {
					    uploads.emplace_back(upload_vaddr, upload_size);
				    },
				    [&]() noexcept {
					    for (const auto& [upload_vaddr, upload_size]: uploads) {
						    UtilUploadBuffer(ctx, StagingBufferType::Vertex, old.buffer.get(),
						                     upload_vaddr - old.vaddr,
						                     reinterpret_cast<const void*>(upload_vaddr),
						                     upload_size);
					    }
				    });
			}
		}
		auto cached   = std::make_unique<CachedBuffer>();
		cached->vaddr = merged.address;
		cached->size  = merged.size;
		cached->ctx   = ctx;
		cached->buffer = MakeSharedVulkanBuffer(ctx);
		cached->buffer->usage =
		    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
		    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		cached->buffer->memory.property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		VulkanCreateBuffer(ctx, cached->size, cached->buffer.get());
		if (!overlaps.empty()) {
			auto* vk_buffer = command->GetPool()->buffers[command->GetIndex()];
			std::vector<VkBufferMemoryBarrier> before;
			before.reserve(overlaps.size() + 1);
			for (const auto overlap: overlaps) {
				VkBufferMemoryBarrier barrier {};
				barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
				barrier.srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT;
				barrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.buffer              = overlap->second->buffer->buffer;
				barrier.offset              = 0;
				barrier.size                = overlap->second->size;
				before.push_back(barrier);
			}
			VkBufferMemoryBarrier destination {};
			destination.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			destination.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
			destination.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			destination.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			destination.buffer              = cached->buffer->buffer;
			destination.offset              = 0;
			destination.size                = cached->size;
			before.push_back(destination);
			vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0,
			                     nullptr, static_cast<uint32_t>(before.size()), before.data(), 0,
			                     nullptr);
			for (const auto overlap: overlaps) {
				const auto&        old = *overlap->second;
				const VkBufferCopy copy {
				    .srcOffset = 0, .dstOffset = old.vaddr - cached->vaddr, .size = old.size};
				vkCmdCopyBuffer(vk_buffer, old.buffer->buffer, cached->buffer->buffer, 1, &copy);
			}
			VkBufferMemoryBarrier after {};
			after.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			after.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
			after.dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			after.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			after.buffer              = cached->buffer->buffer;
			after.offset              = 0;
			after.size                = cached->size;
			vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0,
			                     nullptr, 1, &after, 0, nullptr);
			for (const auto overlap: overlaps) {
				command->RetainResourceUntilFence(overlap->second->buffer);
				m_buffers.erase(overlap);
			}
		}
		it = m_buffers.emplace(cached->vaddr, std::move(cached)).first;
	}

	auto&                                      cached = *it->second;
	std::vector<std::pair<uint64_t, uint64_t>> ranges;
	ranges.reserve((size + 2 * TRACKER_PAGE_SIZE - 2) / TRACKER_PAGE_SIZE);
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t upload_vaddr, uint64_t upload_size) noexcept {
		    ranges.emplace_back(upload_vaddr, upload_size);
	    },
	    [&]() noexcept {
		    for (const auto& [upload_vaddr, upload_size]: ranges) {
			    UtilUploadBuffer(ctx, StagingBufferType::Vertex, cached.buffer.get(),
			                     upload_vaddr - cached.vaddr,
			                     reinterpret_cast<const void*>(upload_vaddr), upload_size);
		    }
	    });
	if (is_written) {
		m_gpu_modified_ranges.Add(vaddr, size);
	}
	cached.queue_mask |= queue_mask;
	command->RetainResourceUntilFence(cached.buffer);
	return {cached.buffer.get(), vaddr - cached.vaddr};
}

bool BufferCache::UploadHostData(CommandBuffer* command, GraphicContext* ctx, const void* src,
                                 uint64_t size, uint64_t alignment, VulkanBuffer** out_buffer,
                                 uint64_t* out_offset, uint64_t* out_range) {
	if (command == nullptr || command->IsInvalid() || command->IsExecute() || ctx == nullptr) {
		EXIT("BufferCache: host stream upload requires a recording command buffer\n");
	}
	return command->m_host_stream.Copy(ctx, src, size, alignment, out_buffer, out_offset, out_range);
}

VulkanBuffer* BufferCache::ObtainNullBuffer(CommandBuffer* command, GraphicContext* ctx) {
	if (command == nullptr || command->IsInvalid() || command->IsExecute() || ctx == nullptr) {
		EXIT("BufferCache: null buffer requires a graphics context\n");
	}
	FaultSafeCacheLock lock(this, m_mutex);
	if (m_null_buffer == nullptr) {
		// robustBufferAccess makes every fetch safe; keeping all 16 backing bytes zero also makes
		// both in-range and implementation-selected out-of-range values deterministic. Use a
		// persistent 16-byte fallback when Vulkan null vertex descriptors are unavailable.
		auto buffer = MakeSharedVulkanBuffer(ctx);
		buffer->usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
		                VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		buffer->memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) |
		                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
		                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		VulkanCreateBuffer(ctx, 16, buffer.get());
		void* data = nullptr;
		VulkanMapMemory(ctx, &buffer->memory, &data);
		std::memset(data, 0, 16);
		VulkanUnmapMemory(ctx, &buffer->memory);
		m_null_buffer = std::move(buffer);
	}

	// The buffer remains cache-persistent, while every recorded consumer keeps the allocation
	// alive until its own fence even if an unrelated command processor resets the global cache.
	command->RetainResourceUntilFence(m_null_buffer);
	return m_null_buffer.get();
}

BufferImageCopySource BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr || (vaddr & (TRACKER_PAGE_SIZE - 1)) != 0 ||
	    (size & (TRACKER_PAGE_SIZE - 1)) != 0) {
		EXIT("BufferCache: invalid image source, addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " page_aligned=%d\n",
		     vaddr, size,
		     (vaddr & (TRACKER_PAGE_SIZE - 1)) == 0 && (size & (TRACKER_PAGE_SIZE - 1)) == 0);
	}
	FaultSafeCacheLock lock(this, m_mutex);
	auto use_coherent_guest_backing = [&](const CachedBuffer& overlap) {
		const bool gpu_modified = m_memory_tracker.IsRegionGpuModified(vaddr, size);
		const auto dirty_ranges = m_gpu_modified_ranges.Intersections(vaddr, size);
		if (gpu_modified || !dirty_ranges.empty()) {
			EXIT("BufferCache: GPU-modified image source requires a containing cached buffer, "
			     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
			     " buffer=0x%016" PRIx64 "+0x%016" PRIx64 " tracker_dirty=%d byte_ranges=%zu\n",
			     vaddr, size, overlap.vaddr, overlap.size, gpu_modified, dirty_ranges.size());
		}
		// A clean/CPU-current image range may coexist with a partial cached-buffer view because
		// guest backing is authoritative. The PS5 detiler consumes that backing at the existing
		// Tiler seam.
		return BufferImageCopySource {nullptr, 0, vaddr, size, true};
	};
	auto               it = m_buffers.upper_bound(vaddr);
	if (it == m_buffers.begin()) {
		if (it != m_buffers.end() &&
		    PageOverlaps(vaddr, size, it->second->vaddr, it->second->size)) {
			return use_coherent_guest_backing(*it->second);
		}
		return {nullptr, 0, vaddr, size, true};
	}
	--it;
	auto&      cached = *it->second;
	const auto offset = vaddr - cached.vaddr;
	if (offset > cached.size || size > cached.size - offset) {
		const auto next = std::next(it);
		if (PageOverlaps(vaddr, size, cached.vaddr, cached.size)) {
			return use_coherent_guest_backing(cached);
		}
		if (next != m_buffers.end() &&
		    PageOverlaps(vaddr, size, next->second->vaddr, next->second->size)) {
			return use_coherent_guest_backing(*next->second);
		}
		return {nullptr, 0, vaddr, size, true};
	}
	if (cached.ctx == nullptr || cached.buffer == nullptr || cached.buffer->buffer == nullptr) {
		EXIT("BufferCache: inconsistent image source, addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " buffer=0x%016" PRIx64 "+0x%016" PRIx64 " ctx=%p native=%p\n",
		     vaddr, size, cached.vaddr, cached.size, static_cast<const void*>(cached.ctx),
		     static_cast<const void*>(cached.buffer.get()));
	}
	const bool gpu_modified = m_memory_tracker.IsRegionGpuModified(vaddr, size);
	const bool cpu_modified = m_memory_tracker.IsRegionCpuModified(vaddr, size);
	if (gpu_modified && cpu_modified) {
		EXIT("BufferCache: image source is both CPU- and GPU-modified, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto ranges = m_gpu_modified_ranges.Intersections(vaddr, size);
	if (gpu_modified) {
		if (!GraphicsRunIsCommandProcessorThread() && !GraphicsRunSubmissionLockHeld() &&
		    !LabelInCallback()) {
			EXIT("BufferCache: GPU-dirty image source requires ordered GPU context, "
			     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
			     " command_thread=%d submission_lock=%d completion_callback=%d\n",
			     vaddr, size, GraphicsRunIsCommandProcessorThread(),
			     GraphicsRunSubmissionLockHeld(), LabelInCallback());
		}
		if (ranges.empty()) {
			EXIT("BufferCache: GPU-dirty image read has no dirty byte ranges, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     vaddr, size);
		}
		VulkanDeviceWaitIdle(cached.ctx);
		auto     backing_writes = ReserveBackingWrites(m_page_manager, ranges);
		uint64_t downloaded     = 0;
		m_memory_tracker.ForEachDownloadRange<true>(
		    vaddr, size,
		    [&](uint64_t address, uint64_t bytes) noexcept {
			    ValidateDirtyPages(m_gpu_modified_ranges, address, bytes, "image");
		    },
		    [&](uint64_t address, uint64_t bytes) noexcept {
			    const auto dirty = m_gpu_modified_ranges.Intersections(address, bytes);
			    if (dirty.empty()) {
				    EXIT("BufferCache: GPU-dirty image pages have no dirty byte ranges, "
				         "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
				         address, bytes);
			    }
			    for (const auto& range: dirty) {
				    const auto buffer_offset = range.address - cached.vaddr;
				    if (range.address < cached.vaddr || buffer_offset > cached.size ||
				        range.size > cached.size - buffer_offset ||
				        downloaded > UINT64_MAX - range.size) {
					    EXIT("BufferCache: invalid image download range, addr=0x%016" PRIx64
					         " size=0x%016" PRIx64 " buffer=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
					         range.address, range.size, cached.vaddr, cached.size);
				    }
				    std::vector<uint8_t> data(range.size);
				    UtilDownloadBuffer(cached.ctx, cached.buffer.get(), buffer_offset, data.data(),
				                       range.size);
				    Libs::LibKernel::Memory::WriteBacking(range.address, data.data(), data.size());
				    downloaded += range.size;
			    }
		    });
		if (downloaded == 0) {
			EXIT("BufferCache: image source cleared no tracked GPU pages, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     vaddr, size);
		}
		m_gpu_modified_ranges.Subtract(vaddr, size);
	} else if (!ranges.empty()) {
		EXIT("BufferCache: CPU-current image source retains GPU-dirty bytes, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	if (cpu_modified) {
		std::vector<std::pair<uint64_t, uint64_t>> uploads;
		m_memory_tracker.ForEachUploadRange(
		    vaddr, size, false,
		    [&](uint64_t address, uint64_t bytes) noexcept {
			    uploads.emplace_back(address, bytes);
		    },
		    [&]() noexcept {
			    for (const auto& [address, bytes]: uploads) {
				    UtilUploadBuffer(cached.ctx, StagingBufferType::Vertex, cached.buffer.get(),
				                     address - cached.vaddr, reinterpret_cast<const void*>(address),
				                     bytes);
			    }
		    });
		if (uploads.empty()) {
			EXIT("BufferCache: CPU-modified image source produced no uploads, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     vaddr, size);
		}
	}
	if (m_memory_tracker.IsRegionCpuModified(vaddr, size) ||
	    m_memory_tracker.IsRegionGpuModified(vaddr, size) ||
	    !m_gpu_modified_ranges.Intersections(vaddr, size).empty()) {
		EXIT("BufferCache: image source did not become coherent, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	return {cached.buffer.get(), offset, vaddr, size, true};
}

namespace {
VkBufferMemoryBarrier MakeDmaBarrier(VulkanBuffer* buffer, uint64_t offset, uint64_t size,
                                     VkAccessFlags source, VkAccessFlags destination) {
	if (buffer == nullptr || buffer->buffer == nullptr || size == 0 ||
	    offset > buffer->buffer_size || size > buffer->buffer_size - offset) {
		EXIT("BufferCache: invalid DMA barrier, buffer=%p handle=%p offset=0x%016" PRIx64
		     " size=0x%016" PRIx64 " buffer_size=0x%016" PRIx64 "\n",
		     static_cast<const void*>(buffer),
		     buffer == nullptr ? nullptr : static_cast<const void*>(buffer->buffer), offset, size,
		     buffer == nullptr ? 0 : buffer->buffer_size);
	}
	VkBufferMemoryBarrier barrier {};
	barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask       = source;
	barrier.dstAccessMask       = destination;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = buffer->buffer;
	barrier.offset              = offset;
	barrier.size                = size;
	return barrier;
}

VkCommandBuffer GetDmaCommandBuffer(CommandBuffer* command) {
	if (command == nullptr || command->IsInvalid() || command->GetPool() == nullptr ||
	    command->GetIndex() >= command->GetPool()->buffers_count) {
		EXIT("BufferCache: invalid DMA command buffer, command=%p\n",
		     static_cast<const void*>(command));
	}
	const auto vk_buffer = command->GetPool()->buffers[command->GetIndex()];
	if (vk_buffer == nullptr) {
		EXIT("BufferCache: DMA command buffer handle is null, index=%u\n", command->GetIndex());
	}
	return vk_buffer;
}
} // namespace

void BufferCache::FillBuffer(CommandBuffer* command, GraphicContext* ctx, uint64_t vaddr,
                             uint64_t size, uint32_t value) {
	if ((vaddr & 3u) != 0 || (size & 3u) != 0) {
		EXIT("BufferCache: fill range must be dword aligned, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	ValidateGpuAccess(vaddr, size, false, true);
	{
		std::lock_guard transaction(m_resource_mutex);
		const bool      image_overlap       = m_texture_cache->HasPageOverlap(vaddr, size);
		const bool      buffer_overlap      = HasPageOverlap(vaddr, size);
		const bool      buffer_gpu_modified = IsRegionGpuModified(vaddr, size);
		if (!buffer_overlap && !buffer_gpu_modified) {
			if (image_overlap) {
				m_texture_cache->PrepareHostWrite(vaddr, size);
			}
			auto* dst = reinterpret_cast<uint32_t*>(vaddr);
			std::fill(dst, dst + size / sizeof(uint32_t), value);
			return;
		}
		if (image_overlap) {
			EXIT("BufferCache: GPU fill aliases image pages, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     vaddr, size);
		}
		if (m_texture_cache->HasMetaOverlap(vaddr, size)) {
			LOGF("BufferCache: GPU fill overlaps virtual metadata, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     vaddr, size);
			EXIT("BufferCache: GPU fill of virtual metadata is unsupported, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     vaddr, size);
		}
	}
	const auto [dst, dst_offset] = ObtainBuffer(command, ctx, vaddr, size, true, false);
	const auto before    = MakeDmaBarrier(dst, dst_offset, size,
	                                      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	                                      VK_ACCESS_TRANSFER_WRITE_BIT);
	const auto vk_buffer = GetDmaCommandBuffer(command);
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, 1,
	                     &before, 0, nullptr);
	vkCmdFillBuffer(vk_buffer, dst->buffer, dst_offset, size, value);
	const auto after = MakeDmaBarrier(dst, dst_offset, size, VK_ACCESS_TRANSFER_WRITE_BIT,
	                                  VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0,
	                     nullptr, 1, &after, 0, nullptr);
}

void BufferCache::CopyBuffer(CommandBuffer* command, GraphicContext* ctx, uint64_t dst_vaddr,
                             uint64_t src_vaddr, uint64_t size) {
	if (dst_vaddr == 0 || src_vaddr == 0 || size == 0 ||
	    ((dst_vaddr | src_vaddr | size) & 3u) != 0 || size > UINT64_MAX - dst_vaddr ||
	    size > UINT64_MAX - src_vaddr ||
	    (src_vaddr < dst_vaddr + size && dst_vaddr < src_vaddr + size)) {
		EXIT("BufferCache: invalid or overlapping copy range, src=0x%016" PRIx64
		     " dst=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     src_vaddr, dst_vaddr, size);
	}
	ValidateGpuAccess(src_vaddr, size, true, false);
	ValidateGpuAccess(dst_vaddr, size, false, true);
	{
		std::lock_guard transaction(m_resource_mutex);
		const bool      src_image = m_texture_cache->HasPageOverlap(src_vaddr, size);
		const bool      dst_image = m_texture_cache->HasPageOverlap(dst_vaddr, size);
		if (m_texture_cache->HasGpuTargetPageOverlap(src_vaddr, size)) {
			EXIT("BufferCache: GPU copy aliases target pages, src=0x%016" PRIx64
			     " dst=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     src_vaddr, dst_vaddr, size);
		}
		if (!HasPageOverlap(dst_vaddr, size) && !IsRegionGpuModified(src_vaddr, size) &&
		    !IsRegionGpuModified(dst_vaddr, size)) {
			if (m_texture_cache->IsMetaGpuModified(src_vaddr, size)) {
				LOGF("BufferCache: host copy reads virtual metadata, src=0x%016" PRIx64
				     " size=0x%016" PRIx64 "\n",
				     src_vaddr, size);
				EXIT("BufferCache: host copy from GPU-modified metadata is unsupported, "
				     "src=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
				     src_vaddr, size);
			}
			if (dst_image) {
				m_texture_cache->PrepareHostWrite(dst_vaddr, size);
			}
			std::memcpy(reinterpret_cast<void*>(dst_vaddr),
			            reinterpret_cast<const void*>(src_vaddr), size);
			return;
		}
		// A clean target destination is handled above like a protected host write. Target
		// aliases that require an actual GPU buffer copy remain unsupported.
		if (m_texture_cache->HasGpuTargetPageOverlap(dst_vaddr, size)) {
			EXIT("BufferCache: GPU copy aliases target pages, src=0x%016" PRIx64
			     " dst=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     src_vaddr, dst_vaddr, size);
		}
		if (src_image || dst_image) {
			EXIT("BufferCache: GPU copy requires image-buffer synchronization, src=0x%016" PRIx64
			     " dst=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     src_vaddr, dst_vaddr, size);
		}
		if (m_texture_cache->HasMetaOverlap(src_vaddr, size) ||
		    m_texture_cache->HasMetaOverlap(dst_vaddr, size)) {
			LOGF("BufferCache: GPU copy overlaps virtual metadata, src=0x%016" PRIx64
			     " dst=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     src_vaddr, dst_vaddr, size);
			EXIT("BufferCache: GPU copy involving virtual metadata is unsupported, "
			     "src=0x%016" PRIx64 " dst=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     src_vaddr, dst_vaddr, size);
		}
	}
	const auto [src, src_offset] = ObtainBuffer(command, ctx, src_vaddr, size, false, true);
	const auto [dst, dst_offset] = ObtainBuffer(command, ctx, dst_vaddr, size, true, false);
	if (src == dst && src_offset < dst_offset + size && dst_offset < src_offset + size) {
		EXIT("BufferCache: resolved Vulkan copy ranges overlap, src_offset=0x%016" PRIx64
		     " dst_offset=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     src_offset, dst_offset, size);
	}
	const VkBufferMemoryBarrier before[] = {
	    MakeDmaBarrier(dst, dst_offset, size,
	                   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	                   VK_ACCESS_TRANSFER_WRITE_BIT),
	    MakeDmaBarrier(src, src_offset, size, VK_ACCESS_MEMORY_WRITE_BIT,
	                   VK_ACCESS_TRANSFER_READ_BIT),
	};
	const auto vk_buffer = GetDmaCommandBuffer(command);
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, 2,
	                     before, 0, nullptr);
	const VkBufferCopy copy {src_offset, dst_offset, size};
	vkCmdCopyBuffer(vk_buffer, src->buffer, dst->buffer, 1, &copy);
	const VkBufferMemoryBarrier after[] = {
	    MakeDmaBarrier(dst, dst_offset, size, VK_ACCESS_TRANSFER_WRITE_BIT,
	                   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
	    MakeDmaBarrier(src, src_offset, size, VK_ACCESS_TRANSFER_READ_BIT,
	                   VK_ACCESS_MEMORY_WRITE_BIT),
	};
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0,
	                     nullptr, 2, after, 0, nullptr);
}

bool BufferCache::HasPageOverlap(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("BufferCache: invalid page-overlap query, addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     "\n",
		     vaddr, size);
	}
	FaultSafeCacheLock lock(this, m_mutex);
	for (const auto& [address, cached]: m_buffers) {
		if (PageOverlaps(vaddr, size, address, cached->size)) {
			return true;
		}
	}
	return false;
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::PublishImageBacking(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("BufferCache: invalid image-backing publication, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	FaultSafeCacheLock lock(this, m_mutex);
	auto               owner = m_buffers.end();
	for (auto it = m_buffers.begin(); it != m_buffers.end(); ++it) {
		if (!PageOverlaps(vaddr, size, it->second->vaddr, it->second->size)) {
			continue;
		}
		const auto offset = vaddr >= it->second->vaddr ? vaddr - it->second->vaddr : UINT64_MAX;
		if (owner != m_buffers.end() || offset > it->second->size ||
		    size > it->second->size - offset) {
			EXIT("BufferCache: image backing aliases a non-containing cached buffer, "
			     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 " buffer=0x%016" PRIx64 "+0x%016" PRIx64
			     "\n",
			     vaddr, size, it->second->vaddr, it->second->size);
		}
		owner = it;
	}
	if ((owner != m_buffers.end() &&
	     (owner->second->ctx == nullptr || owner->second->buffer == nullptr ||
	      owner->second->buffer->buffer == nullptr ||
	      m_memory_tracker.IsRegionCpuModified(vaddr, size))) ||
	    m_memory_tracker.IsRegionGpuModified(vaddr, size) ||
	    !m_gpu_modified_ranges.Intersections(vaddr, size).empty()) {
		EXIT("BufferCache: image backing requires clean buffer ownership, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " cached=%d\n",
		     vaddr, size, owner != m_buffers.end());
	}
	// A fresh tracker is CPU-dirty by construction when no cached buffer exists. Keep the range
	// CPU-dirty so a subsequently-created buffer uploads the just-published image backing.
	m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
}

void BufferCache::ValidateGpuAccess(uint64_t vaddr, uint64_t size, bool is_read,
                                    bool is_written) const {
	if ((!is_read && !is_written) || vaddr == 0 || size == 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: invalid GPU access request, addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " read=%d write=%d\n",
		     vaddr, size, is_read, is_written);
	}
	if (is_read && !m_page_manager.HasGpuAccess(vaddr, size, GpuAccess::Read)) {
		LOGF("BufferCache: GPU-read access denied, addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
		EXIT("BufferCache: GPU-read access denied, addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	if (is_written && !m_page_manager.HasGpuAccess(vaddr, size, GpuAccess::Write)) {
		LOGF("BufferCache: GPU-write access denied, addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
		EXIT("BufferCache: GPU-write access denied, addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
}

void BufferCache::SetTextureCache(TextureCache& texture_cache) {
	if (m_texture_cache != nullptr) {
		EXIT("BufferCache: texture cache already connected\n");
	}
	m_texture_cache = &texture_cache;
}

void BufferCache::RegisterForDelete(VulkanBuffer* buffer) {
	FaultSafeCacheLock lock(this, m_mutex);

	m_delete_later.push_back(buffer);
}

void BufferCache::DeleteAll(GraphicContext* ctx) {
	KYTY_PROFILER_BLOCK("BufferCache::DeleteAll");

	FaultSafeCacheLock lock(this, m_mutex);

	for (auto* buffer: m_delete_later) {
		EXIT_IF(buffer == nullptr);
		EXIT_IF(buffer->buffer == nullptr);
		EXIT_IF(ctx == nullptr);

		VulkanDeleteBuffer(ctx, buffer);
		delete buffer;
	}
	m_delete_later.clear();
	m_null_buffer.reset();
}

} // namespace Libs::Graphics
