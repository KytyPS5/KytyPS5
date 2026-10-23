#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_IMAGEPAGECOVERAGE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_IMAGEPAGECOVERAGE_H_

#include "common/assert.h"
#include "common/common.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace Libs::Graphics {

// Registered images per tracker page; Covers takes no lock and may run on any thread.
class ImagePageCoverage final {
public:
	ImagePageCoverage(): m_regions(std::make_unique<std::atomic<Region*>[]>(RegionCount)) {}

	KYTY_CLASS_NO_COPY(ImagePageCoverage);

	void Add(uint64_t address, uint64_t size) {
		Update<true>(address, size);
		// The counts must be visible before the caller's page watch can make a store fault.
		std::atomic_thread_fence(std::memory_order_seq_cst);
	}

	void Remove(uint64_t address, uint64_t size) { Update<false>(address, size); }

	[[nodiscard]] bool Covers(uint64_t address) const noexcept {
		const auto index = RegionIndex(address);
		if (index == RegionCount) {
			return false;
		}
		const auto* region = m_regions[index].load(std::memory_order_acquire);
		return region != nullptr &&
		       region->pages[PageIndex(address)].load(std::memory_order_acquire) != 0;
	}

private:
	static constexpr uint64_t ExtendedBase = LibKernel::Memory::kExtendedMemoryBase;
	static constexpr uint64_t ExtendedSize = LibKernel::Memory::kExtendedMemorySize;
	static constexpr uint64_t LowerRegions = LOWER_ADDRESS_SIZE / TRACKER_REGION_SIZE;
	static constexpr uint64_t RegionCount  = LowerRegions + ExtendedSize / TRACKER_REGION_SIZE;
	static_assert(LOWER_ADDRESS_SIZE % TRACKER_REGION_SIZE == 0 &&
	              ExtendedBase % TRACKER_REGION_SIZE == 0 &&
	              ExtendedSize % TRACKER_REGION_SIZE == 0 && LOWER_ADDRESS_SIZE <= ExtendedBase &&
	              ExtendedBase <= UINT64_MAX - ExtendedSize);

	struct Region {
		std::array<std::atomic<uint32_t>, TRACKER_REGION_PAGES> pages {};
	};

	// RegionCount for an address outside every range a valid GuestRange can name.
	[[nodiscard]] static constexpr uint64_t RegionIndex(uint64_t address) noexcept {
		if (address < LOWER_ADDRESS_SIZE) {
			return address / TRACKER_REGION_SIZE;
		}
		if (address >= ExtendedBase && address - ExtendedBase < ExtendedSize) {
			return LowerRegions + (address - ExtendedBase) / TRACKER_REGION_SIZE;
		}
		return RegionCount;
	}

	[[nodiscard]] static constexpr size_t PageIndex(uint64_t address) noexcept {
		return static_cast<size_t>((address % TRACKER_REGION_SIZE) / TRACKER_PAGE_SIZE);
	}

	template <bool add>
	void Update(uint64_t address, uint64_t size) {
		if (size == 0 || size > UINT64_MAX - address) {
			EXIT("ImagePageCoverage: invalid range\n");
		}
		const uint64_t last_byte = address + size - 1;
		for (uint64_t chunk = address;;) {
			const uint64_t region_last = chunk | (TRACKER_REGION_SIZE - 1);
			const uint64_t chunk_last  = std::min(last_byte, region_last);
			if (const auto index = RegionIndex(chunk); index != RegionCount) {
				auto& region = add ? GetOrCreate(index) : Get(index);
				for (auto page = PageIndex(chunk); page <= PageIndex(chunk_last); ++page) {
					auto&      count = region.pages[page];
					const auto value = count.load(std::memory_order_relaxed);
					if (add ? value == UINT32_MAX : value == 0) {
						EXIT("ImagePageCoverage: page count %s\n", add ? "overflow" : "underflow");
					}
					count.store(add ? value + 1 : value - 1, std::memory_order_relaxed);
				}
			}
			if (chunk_last == last_byte) {
				return;
			}
			chunk = chunk_last + 1;
		}
	}

	Region& GetOrCreate(uint64_t index) {
		if (auto* region = m_regions[index].load(std::memory_order_relaxed); region != nullptr) {
			return *region;
		}
		auto* region = m_storage.emplace_back(std::make_unique<Region>()).get();
		m_regions[index].store(region, std::memory_order_release);
		return *region;
	}

	Region& Get(uint64_t index) {
		auto* region = m_regions[index].load(std::memory_order_relaxed);
		if (region == nullptr) {
			EXIT("ImagePageCoverage: page count underflow\n");
		}
		return *region;
	}

	std::unique_ptr<std::atomic<Region*>[]> m_regions;
	std::vector<std::unique_ptr<Region>>    m_storage;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_IMAGEPAGECOVERAGE_H_
