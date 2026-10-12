#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/fenceRetirement.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <span>

namespace Libs::Graphics {

struct GraphicContext;

class MasterSemaphore {
public:
	explicit MasterSemaphore(GraphicContext& graphics);
	~MasterSemaphore();
	KYTY_CLASS_NO_COPY(MasterSemaphore);

	[[nodiscard]] uint64_t CurrentTick() const noexcept {
		return m_current_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] uint64_t KnownGpuTick() const noexcept {
		return m_gpu_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] bool     IsFree(uint64_t tick) const noexcept { return KnownGpuTick() >= tick; }
	[[nodiscard]] uint64_t NextTick() noexcept {
		return m_current_tick.fetch_add(1, std::memory_order_release);
	}
	[[nodiscard]] vk::Semaphore Handle() const noexcept { return m_semaphore; }

	// The timeline semaphore says the GPU executed a submission. On MoltenVK it is signalled by an
	// event encoded at the end of the Metal command buffer, before Metal finishes with the buffer
	// and the resources it uses (MVKQueue.mm, MVKQueueCommandBufferSubmission::execute). A resource
	// may be destroyed only when the submission is retired: its VkFence, which MoltenVK signals
	// from the command buffer completion handler, is signalled.
	//
	// Every submission passes the fence AcquireFence returns for its tick.
	[[nodiscard]] vk::Fence AcquireFence(uint64_t tick);
	[[nodiscard]] bool      IsRetired(uint64_t tick);
	void                    WaitRetired(uint64_t tick);

	void Refresh();
	void Wait(uint64_t tick);
	// The same for a thread that does not own the device: false when the device no longer answers,
	// as it does while the emulator shuts down. Such a thread must not stop the emulator for that.
	[[nodiscard]] bool TryWait(uint64_t tick) noexcept;

private:
	GraphicContext&       m_graphics;
	vk::Semaphore         m_semaphore = nullptr;
	std::atomic<uint64_t> m_gpu_tick {0};
	std::atomic<uint64_t> m_current_tick {1};

	struct FenceOps {
		GraphicContext* graphics = nullptr;

		[[nodiscard]] vk::Fence Create() const;
		[[nodiscard]] bool      IsSignaled(vk::Fence fence) const;
		void                    Reset(std::span<const vk::Fence> fences) const;
		void                    Wait(vk::Fence fence) const;
		void                    Destroy(vk::Fence fence) const;
	};
	FenceRetirement<vk::Fence, FenceOps> m_fences;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_
