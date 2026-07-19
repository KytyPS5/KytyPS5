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
#include "graphics/host_gpu/transfer.h"
#include "graphics/host_gpu/vma.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace Libs::Graphics {

namespace {
thread_local const void* g_cache_lock_owner = nullptr;

constexpr uint32_t READBACK_MAX_RANGES     = 256;
constexpr uint64_t READBACK_COPY_ALIGNMENT = 64;
constexpr uint64_t READBACK_CAPACITY =
    TRACKER_PAGE_SIZE + READBACK_MAX_RANGES * (READBACK_COPY_ALIGNMENT - 1);

constexpr uint64_t AlignReadbackCopySize(uint64_t size) noexcept {
	return (size + READBACK_COPY_ALIGNMENT - 1) & ~(READBACK_COPY_ALIGNMENT - 1);
}

struct SharedVulkanBufferOwner {
	explicit SharedVulkanBufferOwner(GraphicContext* context): ctx(context) {
		EXIT_IF(ctx == nullptr);
	}

	~SharedVulkanBufferOwner() {
		if (buffer.buffer != nullptr) {
			VulkanDeleteBuffer(ctx, &buffer);
		}
	}

	GraphicContext* ctx = nullptr;
	VulkanBuffer    buffer;
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

struct BufferCache::CachedBuffer {
	uint64_t                      vaddr      = 0;
	uint64_t                      size       = 0;
	uint64_t                      queue_mask = 0;
	GraphicContext*               ctx        = nullptr;
	std::shared_ptr<VulkanBuffer> buffer;
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
		       (current == State::Claimed &&
		        (command == nullptr || mapped == nullptr || readback.buffer == nullptr))) {
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
		const bool command_thread            = GraphicsRunIsCommandProcessorThread();
		const bool submissions_prepaused_now = GraphicsRunSubmissionLockHeld() || command_thread;
		const bool unsafe_gpu_lock = GraphicsRunGpuLockHeld() && !submissions_prepaused_now;
		if (unsafe_gpu_lock || LabelInCallback() || g_cache_lock_owner != nullptr ||
		    ctx == nullptr || command == nullptr || mapped == nullptr ||
		    readback.buffer == nullptr) {
			EXIT("BufferCache: unsafe readback request context, command_thread=%d "
			     "submission_lock=%d "
			     "gpu_lock=%d label_callback=%d cache_lock=%p ctx=%p command=%p mapped=%p "
			     "buffer=%p\n",
			     command_thread, GraphicsRunSubmissionLockHeld(), GraphicsRunGpuLockHeld(),
			     LabelInCallback(), static_cast<const void*>(g_cache_lock_owner),
			     static_cast<const void*>(ctx), static_cast<const void*>(command.get()),
			     static_cast<const void*>(mapped), static_cast<const void*>(readback.buffer));
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
		access                = fault_access;
		vaddr                 = fault_vaddr;
		size                  = fault_size;
		range_count           = 0;
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
				readback.usage           = vk::BufferUsageFlagBits::eTransferDst;
				readback.memory.property = vk::MemoryPropertyFlagBits::eHostVisible |
				                           vk::MemoryPropertyFlagBits::eHostCoherent |
				                           vk::MemoryPropertyFlagBits::eHostCached;
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
			auto               it = cache.m_buffers.upper_bound(vaddr);
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
			uint32_t                                        data_offset = 0;
			std::array<vk::BufferCopy, MAX_RANGES>          copies {};
			std::array<vk::BufferMemoryBarrier, MAX_RANGES> barriers {};
			cache.m_gpu_modified_ranges.ForEachIntersection(
			    page, TRACKER_PAGE_SIZE, [&](RangeSet::Range range) {
				    if (range_count == MAX_RANGES || (range.address - cached.vaddr) % 4 != 0 ||
				        range.size % 4 != 0) {
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
				    barrier.sType               = vk::StructureType::eBufferMemoryBarrier;
				    barrier.srcAccessMask       = vk::AccessFlagBits::eMemoryWrite;
				    barrier.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
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
			auto vk_buffer = command->Handle();
			command->Begin();
			vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
			                          vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags {},
			                          0, nullptr, range_count, barriers.data(), 0, nullptr);
			vk_buffer.copyBuffer(cached.buffer->buffer, readback.buffer, range_count,
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
	PageFaultAccess                        access                = PageFaultAccess::Unknown;
	uint64_t                               vaddr                 = 0;
	uint64_t                               size                  = 0;
	uint32_t                               range_count           = 0;
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
		Transfer::WaitForGraphicsIdle(m_buffers.begin()->second->ctx);
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
			return m_readback->Complete(access, vaddr, size) ||
			       m_memory_tracker.CompleteCpuFault(vaddr, size, access, false);
		case PageFaultPhase::Release: m_readback->Release(access, vaddr, size); return true;
		default:
			EXIT("BufferCache: unsupported page-fault phase %u\n", static_cast<uint32_t>(phase));
	}
	const auto action = m_memory_tracker.BeginCpuFault(vaddr, size, access);
	if (action != CpuFaultAction::Download) {
		return action == CpuFaultAction::Continue;
	}
	if (GraphicsRunIsCommandProcessorThread()) {
		GraphicsRunFinishCommandProcessors();
	}
	m_readback->Request(access, vaddr, size);
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
			    for (const auto& range: ranges) {
				    std::vector<uint8_t> data(range.size);
				    Transfer::DownloadBuffer(cached->ctx, cached->buffer.get(),
				                             range.address - begin, data.data(), range.size);
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
	const auto      texture_region = m_texture_cache->QueryRegion(vaddr, size);
	// Use the stream-buffer fast path before image/buffer alias handling. Clean image and metadata
	// views may coexist with a small CPU-current read; Kyty's separate image trackers require
	// GPU-dirty ownership guards here. Read physical backing so
	// host page protection is irrelevant.
	if (is_read && !is_written && size <= CACHING_PAGE_SIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size) && !texture_region.gpu_image_bytes &&
	    !texture_region.gpu_metadata_bytes) {
		std::array<uint8_t, CACHING_PAGE_SIZE> guest_data;
		if (Libs::LibKernel::Memory::TryReadBacking(vaddr, guest_data.data(), size)) {
			VulkanBuffer*  stream_buffer    = nullptr;
			vk::DeviceSize stream_offset    = 0;
			vk::DeviceSize stream_range     = 0;
			const auto     stream_alignment = std::max<uint64_t>(ctx->StorageMinAlignment(), 16);
			if (UploadHostData(command, ctx, guest_data.data(), size, stream_alignment,
			                   &stream_buffer, &stream_offset, &stream_range)) {
				return {stream_buffer, stream_offset};
			}
		}
	}
	const auto texture_pages = begin == vaddr && end - begin == size
	                               ? texture_region
	                               : m_texture_cache->QueryRegion(begin, end - begin);
	if (texture_pages.metadata_pages) {
		EXIT("BufferCache: buffer aliases metadata pages, addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     "\n",
		     begin, end - begin);
	}
	// Cache allocations are tracker-page aligned, but byte-disjoint buffers and images may share
	// an edge page. Clean read-only buffer and image views may coexist; Kyty retains a hard failure
	// when either cache owns newer GPU bytes. Writable buffers delegate the ownership transition
	// to TextureCache, which distinguishes raw texture-data writes from formatted target paths.
	if (texture_pages.image_pages && texture_region.image_bytes) {
		const bool coherent_read = is_read && !is_written &&
		                           !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
		                           !texture_region.gpu_image_bytes;
		if (!coherent_read) {
			if (!is_written) {
				EXIT("BufferCache: unsupported buffer/image alias, addr=0x%016" PRIx64
				     " size=0x%016" PRIx64 " read=%d written=%d formatted=%d\n",
				     vaddr, size, is_read, is_written, is_formatted);
			}
			(void)m_texture_cache->InvalidateMemoryFromGPU(vaddr, size, is_formatted);
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
			for (const auto& overlap: overlaps) {
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
						    Transfer::UploadBuffer(ctx, Transfer::StagingBufferType::Vertex,
						                           old.buffer.get(), upload_vaddr - old.vaddr,
						                           reinterpret_cast<const void*>(upload_vaddr),
						                           upload_size);
					    }
				    });
			}
		}
		auto cached    = std::make_unique<CachedBuffer>();
		cached->vaddr  = merged.address;
		cached->size   = merged.size;
		cached->ctx    = ctx;
		cached->buffer = MakeSharedVulkanBuffer(ctx);
		cached->buffer->usage =
		    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
		    vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
		    vk::BufferUsageFlagBits::eStorageBuffer;
		cached->buffer->memory.property = vk::MemoryPropertyFlagBits::eDeviceLocal;
		VulkanCreateBuffer(ctx, cached->size, cached->buffer.get());
		if (!overlaps.empty()) {
			auto                                 vk_buffer = command->Handle();
			std::vector<vk::BufferMemoryBarrier> before;
			before.reserve(overlaps.size() + 1);
			for (const auto& overlap: overlaps) {
				vk::BufferMemoryBarrier barrier {};
				barrier.sType               = vk::StructureType::eBufferMemoryBarrier;
				barrier.srcAccessMask       = vk::AccessFlagBits::eMemoryWrite;
				barrier.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.buffer              = overlap->second->buffer->buffer;
				barrier.offset              = 0;
				barrier.size                = overlap->second->size;
				before.push_back(barrier);
			}
			vk::BufferMemoryBarrier destination {};
			destination.sType               = vk::StructureType::eBufferMemoryBarrier;
			destination.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
			destination.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			destination.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			destination.buffer              = cached->buffer->buffer;
			destination.offset              = 0;
			destination.size                = cached->size;
			before.push_back(destination);
			vk_buffer.pipelineBarrier(
			    vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
			    vk::DependencyFlagBits::eByRegion, 0, nullptr, static_cast<uint32_t>(before.size()),
			    before.data(), 0, nullptr);
			for (const auto& overlap: overlaps) {
				const auto&          old = *overlap->second;
				const vk::BufferCopy copy {
				    .srcOffset = 0, .dstOffset = old.vaddr - cached->vaddr, .size = old.size};
				vk_buffer.copyBuffer(old.buffer->buffer, cached->buffer->buffer, 1, &copy);
			}
			vk::BufferMemoryBarrier after {};
			after.sType         = vk::StructureType::eBufferMemoryBarrier;
			after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
			after.dstAccessMask =
			    vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
			after.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			after.buffer              = cached->buffer->buffer;
			after.offset              = 0;
			after.size                = cached->size;
			vk_buffer.pipelineBarrier(
			    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
			    vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
			for (const auto& overlap: overlaps) {
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
			    Transfer::UploadBuffer(ctx, Transfer::StagingBufferType::Vertex,
			                           cached.buffer.get(), upload_vaddr - cached.vaddr,
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
	return command->m_host_stream.Copy(ctx, src, size, alignment, out_buffer, out_offset,
	                                   out_range);
}

VulkanBuffer* BufferCache::ObtainNullBuffer(CommandBuffer* command, GraphicContext* ctx) {
	if (command == nullptr || command->IsInvalid() || command->IsExecute() || ctx == nullptr) {
		EXIT("BufferCache: null buffer requires a graphics context\n");
	}
	FaultSafeCacheLock lock(this, m_mutex);
	if (m_null_buffer == nullptr) {
		// robustBufferAccess makes every fetch safe; TODO: Use a
		// persistent 16-byte fallback when Vulkan null vertex descriptors are unavailable.
		auto buffer             = MakeSharedVulkanBuffer(ctx);
		buffer->usage           = vk::BufferUsageFlagBits::eStorageBuffer |
		                          vk::BufferUsageFlagBits::eVertexBuffer |
		                          vk::BufferUsageFlagBits::eIndexBuffer;
		buffer->memory.property = vk::MemoryPropertyFlagBits::eHostVisible |
		                          vk::MemoryPropertyFlagBits::eHostCoherent |
		                          vk::MemoryPropertyFlagBits::eHostCached;
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
	const bool         cpu_modified = m_memory_tracker.IsRegionCpuModified(vaddr, size);
	const bool         gpu_modified = m_memory_tracker.IsRegionGpuModified(vaddr, size);
	const auto         dirty_ranges = m_gpu_modified_ranges.Intersections(vaddr, size);
	if (gpu_modified != !dirty_ranges.empty()) {
		EXIT("BufferCache: image-source tracker and byte ownership disagree, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " tracker_dirty=%d byte_ranges=%zu\n",
		     vaddr, size, gpu_modified, dirty_ranges.size());
	}
	auto find_owner = [&](uint64_t address, uint64_t bytes) noexcept -> CachedBuffer& {
		auto owner = m_buffers.upper_bound(address);
		if (owner == m_buffers.begin()) {
			EXIT("BufferCache: GPU image-source bytes have no cached owner\n");
		}
		--owner;
		auto&      cached = *owner->second;
		const auto offset = address - cached.vaddr;
		if (offset > cached.size || bytes > cached.size - offset || cached.ctx == nullptr ||
		    cached.buffer == nullptr || cached.buffer->buffer == nullptr) {
			EXIT("BufferCache: GPU image-source bytes have no containing native buffer\n");
		}
		return cached;
	};
	if (gpu_modified) {
		if (!GraphicsRunIsCommandProcessorThread() && !GraphicsRunSubmissionLockHeld() &&
		    !LabelInCallback()) {
			EXIT("BufferCache: GPU-dirty image source requires ordered GPU context, "
			     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
			     " command_thread=%d submission_lock=%d completion_callback=%d\n",
			     vaddr, size, GraphicsRunIsCommandProcessorThread(),
			     GraphicsRunSubmissionLockHeld(), LabelInCallback());
		}
		std::vector<GraphicContext*> waited;
		for (const auto& range: dirty_ranges) {
			auto& owner = find_owner(range.address, range.size);
			if (std::find(waited.begin(), waited.end(), owner.ctx) == waited.end()) {
				Transfer::WaitForGraphicsIdle(owner.ctx);
				waited.push_back(owner.ctx);
			}
		}
		auto     backing_writes = ReserveBackingWrites(m_page_manager, dirty_ranges);
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
				    auto&                owner         = find_owner(range.address, range.size);
				    const auto           buffer_offset = range.address - owner.vaddr;
				    std::vector<uint8_t> data(range.size);
				    Transfer::DownloadBuffer(owner.ctx, owner.buffer.get(), buffer_offset,
				                             data.data(), range.size);
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
	}
	auto owner = m_buffers.upper_bound(vaddr);
	if (owner == m_buffers.begin()) {
		return {nullptr, 0, vaddr, size, true, cpu_modified};
	}
	--owner;
	auto&      cached = *owner->second;
	const auto offset = vaddr - cached.vaddr;
	if (offset > cached.size || size > cached.size - offset) {
		return {nullptr, 0, vaddr, size, true, cpu_modified};
	}
	if (cached.ctx == nullptr || cached.buffer == nullptr || cached.buffer->buffer == nullptr) {
		EXIT("BufferCache: containing image source is inconsistent\n");
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
				    Transfer::UploadBuffer(cached.ctx, Transfer::StagingBufferType::Vertex,
				                           cached.buffer.get(), address - cached.vaddr,
				                           reinterpret_cast<const void*>(address), bytes);
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
	return {cached.buffer.get(), offset, vaddr, size, true, cpu_modified};
}

namespace {
vk::BufferMemoryBarrier MakeDmaBarrier(VulkanBuffer* buffer, uint64_t offset, uint64_t size,
                                       vk::AccessFlags source, vk::AccessFlags destination) {
	if (buffer == nullptr || buffer->buffer == nullptr || size == 0 ||
	    offset > buffer->buffer_size || size > buffer->buffer_size - offset) {
		EXIT("BufferCache: invalid DMA barrier, buffer=%p handle=%p offset=0x%016" PRIx64
		     " size=0x%016" PRIx64 " buffer_size=0x%016" PRIx64 "\n",
		     static_cast<const void*>(buffer),
		     buffer == nullptr ? nullptr : static_cast<const void*>(buffer->buffer), offset, size,
		     buffer == nullptr ? 0 : buffer->buffer_size);
	}
	vk::BufferMemoryBarrier barrier {};
	barrier.sType               = vk::StructureType::eBufferMemoryBarrier;
	barrier.srcAccessMask       = source;
	barrier.dstAccessMask       = destination;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = buffer->buffer;
	barrier.offset              = offset;
	barrier.size                = size;
	return barrier;
}

vk::CommandBuffer GetDmaCommandBuffer(CommandBuffer* command) {
	if (command == nullptr) {
		EXIT("BufferCache: invalid DMA command buffer, command=%p\n",
		     static_cast<const void*>(command));
	}
	return command->Handle();
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
		const auto      texture_region      = m_texture_cache->QueryRegion(vaddr, size);
		const bool      image_overlap       = texture_region.image_bytes;
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
		if (texture_region.metadata_bytes) {
			LOGF("BufferCache: GPU fill overlaps virtual metadata, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     vaddr, size);
			EXIT("BufferCache: GPU fill of virtual metadata is unsupported, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     vaddr, size);
		}
	}
	const auto [dst, dst_offset] = ObtainBuffer(command, ctx, vaddr, size, true, false);
	const auto before            = MakeDmaBarrier(
	    dst, dst_offset, size, vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
	    vk::AccessFlagBits::eTransferWrite);
	const auto vk_buffer = GetDmaCommandBuffer(command);
	vk_buffer.pipelineBarrier(
	    vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
	    vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
	vk_buffer.fillBuffer(dst->buffer, dst_offset, size, value);
	const auto after =
	    MakeDmaBarrier(dst, dst_offset, size, vk::AccessFlagBits::eTransferWrite,
	                   vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite);
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                          vk::PipelineStageFlagBits::eAllCommands,
	                          vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
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
	bool dst_image_transition = false;
	{
		std::lock_guard transaction(m_resource_mutex);
		const auto      src_region    = m_texture_cache->QueryRegion(src_vaddr, size);
		const auto      dst_region    = m_texture_cache->QueryRegion(dst_vaddr, size);
		const bool      src_image_gpu = src_region.gpu_image_bytes;
		const bool      src_meta      = src_region.metadata_bytes;
		if (src_region.non_sampled_pages) {
			EXIT("BufferCache: GPU copy aliases target pages, src=0x%016" PRIx64
			     " dst=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     src_vaddr, dst_vaddr, size);
		}
		if (!HasPageOverlap(dst_vaddr, size) && !IsRegionGpuModified(src_vaddr, size) &&
		    !IsRegionGpuModified(dst_vaddr, size) && !src_image_gpu) {
			if (src_region.gpu_metadata_bytes) {
				LOGF("BufferCache: host copy reads virtual metadata, src=0x%016" PRIx64
				     " size=0x%016" PRIx64 "\n",
				     src_vaddr, size);
				EXIT("BufferCache: host copy from GPU-modified metadata is unsupported, "
				     "src=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
				     src_vaddr, size);
			}
			if (dst_region.image_bytes) {
				m_texture_cache->PrepareHostWrite(dst_vaddr, size);
			}
			std::memcpy(reinterpret_cast<void*>(dst_vaddr),
			            reinterpret_cast<const void*>(src_vaddr), size);
			return;
		}
		if (src_image_gpu) {
			EXIT("BufferCache: GPU copy source aliases GPU-modified image, src=0x%016" PRIx64
			     " dst=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     src_vaddr, dst_vaddr, size);
		}
		dst_image_transition        = m_texture_cache->InvalidateMemoryFromGPU(dst_vaddr, size);
		const auto transitioned_dst = m_texture_cache->QueryRegion(dst_vaddr, size);
		// A clean target destination is handled above like a protected host write. Target
		// aliases that require an actual GPU buffer copy remain unsupported.
		if (transitioned_dst.non_sampled_pages) {
			EXIT("BufferCache: GPU copy aliases target pages, src=0x%016" PRIx64
			     " dst=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     src_vaddr, dst_vaddr, size);
		}
		if (src_meta || transitioned_dst.metadata_bytes) {
			LOGF("BufferCache: GPU copy overlaps virtual metadata, src=0x%016" PRIx64
			     " dst=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     src_vaddr, dst_vaddr, size);
			EXIT("BufferCache: GPU copy involving virtual metadata is unsupported, "
			     "src=0x%016" PRIx64 " dst=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     src_vaddr, dst_vaddr, size);
		}
	}
	const auto [src, src_offset] = ObtainBuffer(command, ctx, src_vaddr, size, false, true);
	const auto [dst, dst_offset] =
	    ObtainBuffer(command, ctx, dst_vaddr, size, true, false, dst_image_transition);
	if (src == dst && src_offset < dst_offset + size && dst_offset < src_offset + size) {
		EXIT("BufferCache: resolved Vulkan copy ranges overlap, src_offset=0x%016" PRIx64
		     " dst_offset=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     src_offset, dst_offset, size);
	}
	const vk::BufferMemoryBarrier before[] = {
	    MakeDmaBarrier(dst, dst_offset, size,
	                   vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
	                   vk::AccessFlagBits::eTransferWrite),
	    MakeDmaBarrier(src, src_offset, size, vk::AccessFlagBits::eMemoryWrite,
	                   vk::AccessFlagBits::eTransferRead),
	};
	const auto vk_buffer = GetDmaCommandBuffer(command);
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                          vk::PipelineStageFlagBits::eTransfer,
	                          vk::DependencyFlagBits::eByRegion, 0, nullptr, 2, before, 0, nullptr);
	const vk::BufferCopy copy {src_offset, dst_offset, size};
	vk_buffer.copyBuffer(src->buffer, dst->buffer, 1, &copy);
	const vk::BufferMemoryBarrier after[] = {
	    MakeDmaBarrier(dst, dst_offset, size, vk::AccessFlagBits::eTransferWrite,
	                   vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite),
	    MakeDmaBarrier(src, src_offset, size, vk::AccessFlagBits::eTransferRead,
	                   vk::AccessFlagBits::eMemoryWrite),
	};
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                          vk::PipelineStageFlagBits::eAllCommands,
	                          vk::DependencyFlagBits::eByRegion, 0, nullptr, 2, after, 0, nullptr);
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
	if ((owner != m_buffers.end() && m_memory_tracker.IsRegionCpuModified(vaddr, size)) ||
	    m_memory_tracker.IsRegionGpuModified(vaddr, size) ||
	    !m_gpu_modified_ranges.Intersections(vaddr, size).empty()) {
		EXIT("BufferCache: image backing requires clean buffer ownership, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " cached=%d\n",
		     vaddr, size, owner != m_buffers.end());
	}
	// A fresh tracker is CPU-dirty by construction when no cached buffer exists. Keep the range
	// CPU-dirty so subsequently-created buffer uploads the just-published image backing.
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

void BufferCache::ResetNullBuffer() {
	KYTY_PROFILER_BLOCK("BufferCache::ResetNullBuffer");

	FaultSafeCacheLock lock(this, m_mutex);
	m_null_buffer.reset();
}

} // namespace Libs::Graphics
