#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MULTILEVELPAGETABLE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MULTILEVELPAGETABLE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Libs::Graphics {

// Sparse two-level lookup for texture-cache page ownership.
// Entries are selected by guest page number below kPageCount; queries never allocate L1 buckets.
template <typename EntryT, size_t PageBits = 20, size_t AddressSpaceBits = 40,
          size_t FirstLevelBits = 10>
class MultiLevelPageTable final {
public:
	using Entry = EntryT;

	static_assert(AddressSpaceBits < 64);
	static_assert(PageBits < AddressSpaceBits);
	static_assert(FirstLevelBits > 0 && FirstLevelBits <= AddressSpaceBits - PageBits);

	static constexpr size_t   kPageBits          = PageBits;
	static constexpr size_t   kAddressSpaceBits  = AddressSpaceBits;
	static constexpr size_t   kFirstLevelBits    = FirstLevelBits;
	static constexpr size_t   kSecondLevelBits   = AddressSpaceBits - FirstLevelBits - PageBits;
	static constexpr size_t   kFirstLevelEntries = size_t {1} << FirstLevelBits;
	static constexpr size_t   kBucketEntries     = size_t {1} << kSecondLevelBits;
	static constexpr size_t   kPageCount         = size_t {1} << (AddressSpaceBits - PageBits);
	static constexpr uint64_t kAddressSpaceSize  = uint64_t {1} << AddressSpaceBits;

	struct PageRange final {
		size_t first          = 0;
		size_t last_exclusive = 0;
	};

	MultiLevelPageTable(): m_first_level(kFirstLevelEntries) {}

	[[nodiscard]] Entry* Find(size_t page) {
		auto& bucket = m_first_level[FirstLevelIndex(page)];
		return bucket == nullptr ? nullptr : &(*bucket)[SecondLevelIndex(page)];
	}

	[[nodiscard]] const Entry* Find(size_t page) const {
		const auto& bucket = m_first_level[FirstLevelIndex(page)];
		return bucket == nullptr ? nullptr : &(*bucket)[SecondLevelIndex(page)];
	}

	[[nodiscard]] Entry& operator[](size_t page) {
		auto& bucket = m_first_level[FirstLevelIndex(page)];
		if (bucket == nullptr) {
			bucket = std::make_unique<Bucket>();
		}
		return (*bucket)[SecondLevelIndex(page)];
	}

	// Returns the half-open page interval touched by a non-empty byte range.
	// An end exactly at 2^AddressSpaceBits is valid; wrapping or crossing it is not.
	[[nodiscard]] static constexpr bool TryGetPageRange(uint64_t address, uint64_t size,
	                                                    PageRange& range) {
		if (size == 0 || address >= kAddressSpaceSize || size > kAddressSpaceSize - address) {
			return false;
		}
		const uint64_t end   = address + size;
		range.first          = static_cast<size_t>(address >> PageBits);
		range.last_exclusive = static_cast<size_t>(((end - 1) >> PageBits) + 1);
		return true;
	}

private:
	using Bucket = std::array<Entry, kBucketEntries>;

	[[nodiscard]] static constexpr size_t FirstLevelIndex(size_t page) {
		return page >> kSecondLevelBits;
	}
	[[nodiscard]] static constexpr size_t SecondLevelIndex(size_t page) {
		return page & (kBucketEntries - 1);
	}

	std::vector<std::unique_ptr<Bucket>> m_first_level;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MULTILEVELPAGETABLE_H_
