#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MULTILEVELPAGETABLE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MULTILEVELPAGETABLE_H_

#include "common/assert.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace Libs::Graphics {

// Sparse two-level lookup for texture-cache page ownership.
// Entries are selected by guest page number; queries never allocate L1 buckets.
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
		if (!IsValidPage(page)) {
			return nullptr;
		}
		auto& bucket = m_first_level[FirstLevelIndex(page)];
		return bucket == nullptr ? nullptr : &(*bucket)[SecondLevelIndex(page)];
	}

	[[nodiscard]] const Entry* Find(size_t page) const {
		if (!IsValidPage(page)) {
			return nullptr;
		}
		const auto& bucket = m_first_level[FirstLevelIndex(page)];
		return bucket == nullptr ? nullptr : &(*bucket)[SecondLevelIndex(page)];
	}

	[[nodiscard]] Entry& GetOrCreate(size_t page) {
		if (!IsValidPage(page)) {
			EXIT("MultiLevelPageTable page is outside the guest address space");
		}
		auto& bucket = m_first_level[FirstLevelIndex(page)];
		if (bucket == nullptr) {
			bucket = std::make_unique<Bucket>();
			++m_allocated_buckets;
		}
		return (*bucket)[SecondLevelIndex(page)];
	}

	[[nodiscard]] Entry& operator[](size_t page) { return GetOrCreate(page); }

	[[nodiscard]] static constexpr bool IsValidPage(size_t page) { return page < kPageCount; }

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

	[[nodiscard]] size_t AllocatedBucketCount() const { return m_allocated_buckets; }

private:
	using Bucket = std::array<Entry, kBucketEntries>;

	[[nodiscard]] static constexpr size_t FirstLevelIndex(size_t page) {
		return page >> kSecondLevelBits;
	}
	[[nodiscard]] static constexpr size_t SecondLevelIndex(size_t page) {
		return page & (kBucketEntries - 1);
	}

	std::vector<std::unique_ptr<Bucket>> m_first_level;
	size_t                               m_allocated_buckets = 0;
};

// Inline owner storage for page-table entries. Texture pages normally have only a
// handful of owners, so avoid a heap allocation on the first 16 registrations.
//
// Owners are packed into live uint64_t objects instead of being constructed in
// raw storage. This keeps sparse bucket construction cheap without relying on
// subtle implicit-lifetime or pointer-provenance rules.
template <typename OwnerT, size_t InlineCapacity = 16>
class InlinePageOwnerList final {
public:
	using value_type = OwnerT;
	using size_type  = size_t;

	class const_iterator final {
	public:
		[[nodiscard]] OwnerT operator*() const noexcept { return m_owners->At(m_index); }
		const_iterator&      operator++() noexcept {
			++m_index;
			return *this;
		}
		bool operator==(const const_iterator&) const = default;

	private:
		friend class InlinePageOwnerList;
		const_iterator(const InlinePageOwnerList* owners, size_type index) noexcept
		    : m_owners(owners), m_index(index) {}

		const InlinePageOwnerList* m_owners = nullptr;
		size_type                  m_index  = 0;
	};

	static_assert(InlineCapacity > 0);
	static_assert(std::is_default_constructible_v<OwnerT>);
	static_assert(std::is_trivially_copyable_v<OwnerT>);
	static_assert(std::has_unique_object_representations_v<OwnerT>);
	static_assert(sizeof(OwnerT) <= sizeof(uint64_t));

	InlinePageOwnerList() noexcept {}
	InlinePageOwnerList(const InlinePageOwnerList&)            = delete;
	InlinePageOwnerList& operator=(const InlinePageOwnerList&) = delete;
	InlinePageOwnerList(InlinePageOwnerList&& other) noexcept
	    : m_overflow(std::move(other.m_overflow)),
	      m_inline_size(std::exchange(other.m_inline_size, 0)) {
		if (m_overflow == nullptr) {
			std::copy_n(other.m_inline.begin(), m_inline_size, m_inline.begin());
		}
	}
	InlinePageOwnerList& operator=(InlinePageOwnerList&& other) noexcept {
		if (this != &other) {
			m_overflow    = std::move(other.m_overflow);
			m_inline_size = std::exchange(other.m_inline_size, 0);
			if (m_overflow == nullptr) {
				std::copy_n(other.m_inline.begin(), m_inline_size, m_inline.begin());
			}
		}
		return *this;
	}

	[[nodiscard]] const_iterator begin() const noexcept { return {this, 0}; }
	[[nodiscard]] const_iterator end() const noexcept { return {this, size()}; }

	[[nodiscard]] bool      empty() const noexcept { return size() == 0; }
	[[nodiscard]] size_type size() const noexcept {
		return m_overflow == nullptr ? m_inline_size : m_overflow->size();
	}
	[[nodiscard]] OwnerT front() const noexcept { return At(0); }
	[[nodiscard]] OwnerT operator[](size_type index) const noexcept { return At(index); }

	void push_back(const OwnerT& owner) {
		const uint64_t packed = Pack(owner);
		if (m_overflow != nullptr) {
			m_overflow->push_back(packed);
			return;
		}
		if (m_inline_size < InlineCapacity) {
			m_inline[m_inline_size] = packed;
			++m_inline_size;
			return;
		}
		auto overflow = std::make_unique<std::vector<uint64_t>>();
		overflow->reserve(InlineCapacity * 2);
		overflow->assign(m_inline.begin(), m_inline.end());
		overflow->push_back(packed);
		m_overflow = std::move(overflow);
	}

	[[nodiscard]] bool Contains(const OwnerT& owner) const noexcept {
		const uint64_t packed = Pack(owner);
		if (m_overflow != nullptr) {
			return std::find(m_overflow->begin(), m_overflow->end(), packed) != m_overflow->end();
		}
		return std::find(m_inline.begin(), m_inline.begin() + static_cast<ptrdiff_t>(m_inline_size),
		                 packed) != m_inline.begin() + static_cast<ptrdiff_t>(m_inline_size);
	}

	[[nodiscard]] bool Erase(const OwnerT& owner) noexcept {
		const uint64_t packed = Pack(owner);
		if (m_overflow != nullptr) {
			const auto found = std::find(m_overflow->begin(), m_overflow->end(), packed);
			if (found == m_overflow->end()) {
				return false;
			}
			m_overflow->erase(found);
			if (m_overflow->size() == InlineCapacity) {
				std::copy(m_overflow->begin(), m_overflow->end(), m_inline.begin());
				m_inline_size = InlineCapacity;
				m_overflow.reset();
			}
			return true;
		}
		const auto end   = m_inline.begin() + static_cast<ptrdiff_t>(m_inline_size);
		const auto found = std::find(m_inline.begin(), end, packed);
		if (found == end) {
			return false;
		}
		std::move(found + 1, end, found);
		--m_inline_size;
		return true;
	}

	template <typename Func>
	void ForEach(Func&& func) const {
		if (m_overflow != nullptr) {
			for (const uint64_t owner: *m_overflow) {
				func(Unpack(owner));
			}
			return;
		}
		for (size_type index = 0; index < m_inline_size; ++index) {
			func(Unpack(m_inline[index]));
		}
	}

private:
	[[nodiscard]] OwnerT At(size_type index) const noexcept {
		return Unpack(m_overflow == nullptr ? m_inline[index] : (*m_overflow)[index]);
	}
	[[nodiscard]] static uint64_t Pack(const OwnerT& owner) noexcept {
		uint64_t packed = 0;
		std::memcpy(&packed, &owner, sizeof(owner));
		return packed;
	}
	[[nodiscard]] static OwnerT Unpack(uint64_t packed) noexcept {
		OwnerT owner {};
		std::memcpy(&owner, &packed, sizeof(owner));
		return owner;
	}

	std::array<uint64_t, InlineCapacity>   m_inline;
	std::unique_ptr<std::vector<uint64_t>> m_overflow;
	size_type                              m_inline_size = 0;
};

// Removes only the requested owner. Other owners in the same page entry remain intact.
template <typename Container, typename Value>
[[nodiscard]] bool EraseExact(Container& owners, const Value& owner) {
	const auto it = std::find(owners.begin(), owners.end(), owner);
	if (it == owners.end()) {
		return false;
	}
	owners.erase(it);
	return true;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MULTILEVELPAGETABLE_H_
