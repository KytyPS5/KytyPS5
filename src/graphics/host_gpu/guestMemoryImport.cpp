#include "graphics/host_gpu/guestMemoryImport.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/trace.h"
#include "graphics/host_gpu/graphicContext.h"
#include "kernel/memory.h"

#include <cinttypes>
#include <fstream>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define KYTY_GUEST_MEMORY_IMPORT 1
#endif

namespace Libs::Graphics {

namespace {

// udmabuf's limit on the size of one dma-buf, in MiB.
uint64_t UdmabufSizeLimit() {
	std::ifstream file("/sys/module/udmabuf/parameters/size_limit_mb");
	uint64_t      mib = 64;
	if (file) {
		file >> mib;
	}
	return mib * 1024 * 1024;
}

} // namespace

GuestMemoryImport::GuestMemoryImport(GraphicContext& graphics): m_graphics(graphics) {
#if defined(KYTY_GUEST_MEMORY_IMPORT)
	if (!Config::GetGpuGuestMemoryImport() || !graphics.dma_buf_import_enabled) {
		return;
	}
	const auto file = LibKernel::Memory::GetDirectMemoryFile();
	if (file.fd < 0 || file.size == 0) {
		return;
	}
	m_udmabuf = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	if (m_udmabuf < 0) {
		Log::WriteToConsoleAndLog(
		    "GPU: guest memory is copied for the GPU (/dev/udmabuf cannot be opened).\n");
		return;
	}
	m_file      = file.fd;
	m_file_size = file.size;
	m_window    = std::min(UdmabufSizeLimit(), m_file_size);
	m_windows.resize((m_file_size + m_window / 2 - 1) / (m_window / 2));
	m_states.resize(m_windows.size(), WindowState::None);
	// The first window shows whether the GPU takes the memory at all.
	if (Window(0) == nullptr) {
		close(m_udmabuf);
		m_udmabuf = -1;
		m_windows.clear();
		m_states.clear();
		Log::WriteToConsoleAndLog("GPU: guest memory is copied for the GPU (import failed).\n");
		return;
	}
	m_enabled = true;
	m_worker  = std::thread([this] { BuildAhead(); });
	Log::WriteToConsoleAndLog(fmt::format(
	    "GPU: the GPU uses guest memory in place ({} MiB windows).\n", m_window >> 20u));
#endif
}

GuestMemoryImport::~GuestMemoryImport() {
	{
		std::lock_guard lock(m_mutex);
		m_stop = true;
	}
	m_changed.notify_all();
	if (m_worker.joinable()) {
		m_worker.join();
	}
	for (auto& memory: m_windows) {
		if (memory != nullptr) {
			m_graphics.device.freeMemory(memory);
		}
	}
#if defined(KYTY_GUEST_MEMORY_IMPORT)
	if (m_udmabuf >= 0) {
		close(m_udmabuf);
	}
#endif
}

bool GuestMemoryImport::CanPlace(uint64_t vaddr, uint64_t size) const {
	uint64_t offset = 0;
	return m_enabled && LibKernel::Memory::TranslateDirectMemory(vaddr, size, &offset) &&
	       GuestMemoryWindowIndex(offset, size, m_window, m_file_size).has_value();
}

std::optional<GuestMemoryImport::Placement> GuestMemoryImport::Place(uint64_t vaddr,
                                                                     uint64_t size) {
	uint64_t offset = 0;
	if (!m_enabled || !LibKernel::Memory::TranslateDirectMemory(vaddr, size, &offset)) {
		return std::nullopt;
	}
	const auto index = GuestMemoryWindowIndex(offset, size, m_window, m_file_size);
	if (!index) {
		return std::nullopt;
	}
	const auto memory = Window(*index);
	if (memory == nullptr) {
		return std::nullopt;
	}
	return Placement {memory, offset - *index * (m_window / 2)};
}

void GuestMemoryImport::Prefetch(uint64_t file_offset, uint64_t size) {
	if (!m_enabled) {
		return;
	}
	const auto covering = GuestMemoryWindowsCovering(file_offset, size, m_window, m_file_size);
	if (!covering) {
		return;
	}
	{
		std::lock_guard lock(m_mutex);
		for (auto index = covering->first; index <= covering->second && index < m_windows.size();
		     index++) {
			if (m_states[index] == WindowState::None) {
				m_states[index] = WindowState::Queued;
				m_queue.push_back(index);
			}
		}
	}
	m_changed.notify_all();
}

void GuestMemoryImport::BuildAhead() {
#if defined(KYTY_GUEST_MEMORY_IMPORT)
	pthread_setname_np(pthread_self(), "GuestMemImport");
#endif
	std::unique_lock lock(m_mutex);
	while (true) {
		m_changed.wait(lock, [this] { return m_stop || !m_queue.empty(); });
		if (m_stop) {
			return;
		}
		const auto index = m_queue.front();
		m_queue.pop_front();
		if (m_states[index] != WindowState::Queued) {
			continue;
		}
		m_states[index] = WindowState::Building;
		lock.unlock();
		const auto memory = Build(index);
		lock.lock();
		Finish(index, memory);
	}
}

void GuestMemoryImport::Finish(uint64_t index, vk::DeviceMemory memory) {
	m_windows[index] = memory;
	// A window that cannot be built is not tried again ahead of use; Place tries it itself.
	m_states[index] = memory != nullptr ? WindowState::Ready : WindowState::None;
	m_changed.notify_all();
}

vk::DeviceMemory GuestMemoryImport::Window(uint64_t index) {
	EXIT_IF(index >= m_windows.size());
	std::unique_lock lock(m_mutex);
	if (m_states[index] == WindowState::Ready) {
		return m_windows[index];
	}
	KYTY_TRACE_COLUMN(ImportWait);
	// A window that the worker builds is waited for, not built twice.
	m_changed.wait(lock, [&] { return m_states[index] != WindowState::Building; });
	if (m_states[index] == WindowState::Ready) {
		return m_windows[index];
	}
	m_states[index] = WindowState::Building;
	lock.unlock();
	const auto memory = Build(index);
	lock.lock();
	Finish(index, memory);
	return memory;
}

vk::DeviceMemory GuestMemoryImport::Build(uint64_t index) {
#if defined(KYTY_GUEST_MEMORY_IMPORT)
	const auto     begin = index * (m_window / 2);
	const auto     size  = std::min(m_window, m_file_size - begin);
	udmabuf_create create {};
	create.memfd     = static_cast<uint32_t>(m_file);
	create.flags     = UDMABUF_FLAGS_CLOEXEC;
	create.offset    = begin;
	create.size      = size;
	const int dmabuf = ioctl(m_udmabuf, UDMABUF_CREATE, &create);
	if (dmabuf < 0) {
		LOGF("GuestMemoryImport: UDMABUF_CREATE failed for window %" PRIu64 ": errno %d\n", index,
		     errno);
		return nullptr;
	}
	std::lock_guard type_lock(m_memory_type_mutex);
	if (m_memory_type == UINT32_MAX) {
		vk::MemoryFdPropertiesKHR properties {};
		if (m_graphics.device.getMemoryFdPropertiesKHR(
		        vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT, dmabuf, &properties) !=
		        vk::Result::eSuccess ||
		    properties.memoryTypeBits == 0) {
			close(dmabuf);
			return nullptr;
		}
		// Any type the dma-buf allows: they are the same system memory, seen through the GPU's
		// snooping caches.
		m_memory_type = static_cast<uint32_t>(__builtin_ctz(properties.memoryTypeBits));
	}
	vk::ImportMemoryFdInfoKHR import {};
	import.handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
	import.fd         = dmabuf;
	vk::MemoryAllocateFlagsInfo flags {};
	flags.pNext = &import;
	flags.flags = vk::MemoryAllocateFlagBits::eDeviceAddress;
	vk::MemoryAllocateInfo allocate {};
	allocate.pNext           = &flags;
	allocate.allocationSize  = size;
	allocate.memoryTypeIndex = m_memory_type;
	vk::DeviceMemory memory  = nullptr;
	if (m_graphics.device.allocateMemory(&allocate, nullptr, &memory) != vk::Result::eSuccess) {
		// The import did not take the descriptor.
		close(dmabuf);
		return nullptr;
	}
	return memory;
#else
	return nullptr;
#endif
}

} // namespace Libs::Graphics
