#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MULTILEVELPAGETABLE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MULTILEVELPAGETABLE_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <stdexcept>
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
			throw std::out_of_range("MultiLevelPageTable page is outside the guest address space");
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

// Multi-range ownership with 1 MiB candidate buckets and precise 4 KiB
// lifetime accounting. OwnerT only needs equality.
template <typename OwnerT>
class MultiRangePageOwnerIndex final {
private:
	struct Registration;
	using MembershipList = std::vector<const Registration*>;

public:
	struct ByteRange final {
		uint64_t address = 0;
		uint64_t size    = 0;
	};

	using CoarseTable   = MultiLevelPageTable<MembershipList, 20, 40, 10>;
	using TrackingTable = MultiLevelPageTable<MembershipList, 12, 40, 18>;

	[[nodiscard]] bool Register(const OwnerT& owner, const std::vector<ByteRange>& ranges) {
		if (FindRegistration(owner) != m_registrations.end()) {
			return false;
		}
		auto normalized = Normalize(ranges);
		if (normalized.empty()) {
			return false;
		}
		m_registrations.push_back({owner, std::move(normalized)});
		const Registration* registration = &m_registrations.back();
		for (const size_t page: CollectPages<20>(registration->ranges)) {
			m_coarse_pages[page].push_back(registration);
		}
		for (const size_t page: CollectPages<12>(registration->ranges)) {
			m_tracking_pages[page].push_back(registration);
		}
		return true;
	}

	// No state changes if an expected membership is absent. Final releases are
	// sorted and coalesced 4 KiB spans whose final owner disappeared.
	[[nodiscard]] bool Unregister(const OwnerT& owner, std::vector<ByteRange>& final_releases) {
		final_releases.clear();
		auto registration = FindRegistration(owner);
		if (registration == m_registrations.end()) {
			return false;
		}
		const auto          coarse_pages     = CollectPages<20>(registration->ranges);
		const auto          tracking_pages   = CollectPages<12>(registration->ranges);
		const Registration* registration_ptr = &*registration;
		if (!HasAllMemberships(m_coarse_pages, coarse_pages, registration_ptr) ||
		    !HasAllMemberships(m_tracking_pages, tracking_pages, registration_ptr)) {
			return false;
		}
		std::vector<size_t> final_pages;
		for (const size_t page: coarse_pages) {
			(void)EraseExact(*m_coarse_pages.Find(page), registration_ptr);
		}
		for (const size_t page: tracking_pages) {
			auto& owners = *m_tracking_pages.Find(page);
			if (owners.size() == 1) {
				final_pages.push_back(page);
			}
			(void)EraseExact(owners, registration_ptr);
		}
		m_registrations.erase(registration);
		final_releases = CoalesceTrackingPages(final_pages);
		return true;
	}

	[[nodiscard]] std::vector<OwnerT> Query(uint64_t address, uint64_t size) const {
		return Query(address, size, [](const OwnerT&) { return true; });
	}

	template <typename Predicate>
	[[nodiscard]] std::vector<OwnerT> Query(uint64_t address, uint64_t size,
	                                        Predicate&& predicate) const {
		return QueryImpl(address, size, true, std::forward<Predicate>(predicate));
	}

	// Fault paths use page candidates: exact byte-disjoint owners sharing a
	// touched 4 KiB page are intentionally retained.
	[[nodiscard]] std::vector<OwnerT> QueryCandidates(uint64_t address, uint64_t size) const {
		return QueryCandidates(address, size, [](const OwnerT&) { return true; });
	}

	template <typename Predicate>
	[[nodiscard]] std::vector<OwnerT> QueryCandidates(uint64_t address, uint64_t size,
	                                                  Predicate&& predicate) const {
		return QueryImpl(address, size, false, std::forward<Predicate>(predicate));
	}

	[[nodiscard]] size_t CoarseMembershipCount(size_t page) const {
		const auto* owners = m_coarse_pages.Find(page);
		return owners == nullptr ? 0 : owners->size();
	}
	[[nodiscard]] size_t TrackingMembershipCount(size_t page) const {
		const auto* owners = m_tracking_pages.Find(page);
		return owners == nullptr ? 0 : owners->size();
	}

private:
	template <typename Predicate>
	[[nodiscard]] std::vector<OwnerT> QueryImpl(uint64_t address, uint64_t size, bool strict_bytes,
	                                            Predicate&& predicate) const {
		typename CoarseTable::PageRange   coarse_range {};
		typename TrackingTable::PageRange tracking_range {};
		if (!CoarseTable::TryGetPageRange(address, size, coarse_range) ||
		    !TrackingTable::TryGetPageRange(address, size, tracking_range)) {
			return {};
		}
		MembershipList candidates;
		for (size_t page = coarse_range.first; page < coarse_range.last_exclusive; ++page) {
			if (const auto* owners = m_coarse_pages.Find(page); owners != nullptr) {
				AppendUnique(candidates, *owners);
			}
		}
		std::vector<OwnerT> result;
		for (const Registration* registration: candidates) {
			if ((!strict_bytes || Overlaps(registration->ranges, address, size)) &&
			    HasTrackingMembership(registration, tracking_range) &&
			    predicate(registration->owner)) {
				result.push_back(registration->owner);
			}
		}
		return result;
	}

	struct Registration final {
		OwnerT                 owner;
		std::vector<ByteRange> ranges;
	};
	using RegistrationIterator      = typename std::list<Registration>::iterator;
	using ConstRegistrationIterator = typename std::list<Registration>::const_iterator;

	[[nodiscard]] RegistrationIterator FindRegistration(const OwnerT& owner) {
		return std::find_if(m_registrations.begin(), m_registrations.end(),
		                    [&](const Registration& item) { return item.owner == owner; });
	}
	[[nodiscard]] ConstRegistrationIterator FindRegistration(const OwnerT& owner) const {
		return std::find_if(m_registrations.begin(), m_registrations.end(),
		                    [&](const Registration& item) { return item.owner == owner; });
	}

	[[nodiscard]] static std::vector<ByteRange> Normalize(const std::vector<ByteRange>& ranges) {
		std::vector<ByteRange> sorted;
		for (const auto& range: ranges) {
			typename CoarseTable::PageRange ignored {};
			if (!CoarseTable::TryGetPageRange(range.address, range.size, ignored)) {
				return {};
			}
			sorted.push_back(range);
		}
		std::sort(sorted.begin(), sorted.end(), [](const ByteRange& lhs, const ByteRange& rhs) {
			return lhs.address < rhs.address;
		});
		std::vector<ByteRange> merged;
		for (const auto& range: sorted) {
			if (merged.empty() || range.address > merged.back().address + merged.back().size) {
				merged.push_back(range);
			} else {
				const uint64_t end = std::max(merged.back().address + merged.back().size,
				                              range.address + range.size);
				merged.back().size = end - merged.back().address;
			}
		}
		return merged;
	}

	template <size_t Bits>
	[[nodiscard]] static std::vector<size_t> CollectPages(const std::vector<ByteRange>& ranges) {
		std::vector<size_t> pages;
		for (const auto& range: ranges) {
			const size_t first = static_cast<size_t>(range.address >> Bits);
			const size_t last  = static_cast<size_t>((range.address + range.size - 1) >> Bits);
			for (size_t page = first; page <= last; ++page) {
				pages.push_back(page);
			}
		}
		std::sort(pages.begin(), pages.end());
		pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
		return pages;
	}

	template <typename Table>
	[[nodiscard]] static bool HasAllMemberships(const Table&               table,
	                                            const std::vector<size_t>& pages,
	                                            const Registration*        registration) {
		for (const size_t page: pages) {
			const auto* owners = table.Find(page);
			if (owners == nullptr ||
			    std::find(owners->begin(), owners->end(), registration) == owners->end()) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] bool HasTrackingMembership(const Registration*                      registration,
	                                         const typename TrackingTable::PageRange& range) const {
		for (size_t page = range.first; page < range.last_exclusive; ++page) {
			const auto* owners = m_tracking_pages.Find(page);
			if (owners != nullptr &&
			    std::find(owners->begin(), owners->end(), registration) != owners->end()) {
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] static bool Overlaps(const std::vector<ByteRange>& ranges, uint64_t address,
	                                   uint64_t size) {
		const uint64_t end = address + size;
		return std::any_of(ranges.begin(), ranges.end(), [&](const ByteRange& range) {
			return range.address < end && address < range.address + range.size;
		});
	}
	static void AppendUnique(MembershipList& destination, const MembershipList& source) {
		for (const Registration* registration: source) {
			if (std::find(destination.begin(), destination.end(), registration) ==
			    destination.end()) {
				destination.push_back(registration);
			}
		}
	}
	[[nodiscard]] static std::vector<ByteRange>
	CoalesceTrackingPages(const std::vector<size_t>& pages) {
		std::vector<ByteRange> result;
		for (const size_t page: pages) {
			const uint64_t address = static_cast<uint64_t>(page) << 12;
			if (!result.empty() && result.back().address + result.back().size == address) {
				result.back().size += uint64_t {1} << 12;
			} else {
				result.push_back({address, uint64_t {1} << 12});
			}
		}
		return result;
	}

	CoarseTable             m_coarse_pages;
	TrackingTable           m_tracking_pages;
	std::list<Registration> m_registrations;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MULTILEVELPAGETABLE_H_
