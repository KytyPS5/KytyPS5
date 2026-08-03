#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

using Owners     = std::vector<uint32_t>;
using Table      = Libs::Graphics::MultiLevelPageTable<Owners>;
using PageOwners = Libs::Graphics::InlinePageOwnerList<uint32_t, 16>;
using OwnerTable = Libs::Graphics::MultiLevelPageTable<PageOwners, 20, 40, 10>;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ImagePageTableTests: failed: %s\n", text);
		std::abort();
	}
}

void TestMultiOwnerAndExactErase() {
	Table table;
	auto& owners = table.GetOrCreate(17);
	owners.push_back(11);
	owners.push_back(22);

	Check(table.Find(17) != nullptr && table.Find(17)->size() == 2,
	      "both page owners are retained");
	Check(Libs::Graphics::EraseExact(owners, 11U), "registered owner is erased");
	Check(owners.size() == 1 && owners.front() == 22, "erasing one owner preserves its neighbor");
	Check(!Libs::Graphics::EraseExact(owners, 33U), "missing owner is reported without mutation");
}

void TestCrossBucketRange() {
	Table::PageRange   range {};
	constexpr uint64_t bucket_boundary = uint64_t {Table::kBucketEntries} << Table::kPageBits;
	Check(Table::TryGetPageRange(bucket_boundary - 1, 2, range), "cross-bucket range is valid");
	Check(range.first == Table::kBucketEntries - 1 &&
	          range.last_exclusive == Table::kBucketEntries + 1,
	      "cross-bucket range covers both pages");

	Table table;
	table[range.first].push_back(1);
	table[range.last_exclusive - 1].push_back(2);
	Check(table.AllocatedBucketCount() == 2,
	      "pages across the L1 boundary use distinct sparse buckets");
}

void TestQueriesDoNotAllocate() {
	Table table;
	Check(table.Find(123) == nullptr, "unallocated page query is empty");
	Check(table.AllocatedBucketCount() == 0, "mutable query does not allocate");
	const Table& const_table = table;
	Check(const_table.Find(Table::kPageCount - 1) == nullptr, "const query is empty");
	Check(table.AllocatedBucketCount() == 0, "const query does not allocate");
	Check(table.Find(Table::kPageCount) == nullptr, "out-of-range query is empty");
	Check(table.AllocatedBucketCount() == 0, "out-of-range query does not allocate");
}

void TestAddressSpaceBoundaries() {
	Table::PageRange range {};
	Check(Table::TryGetPageRange(Table::kAddressSpaceSize - 1, 1, range),
	      "last guest byte is valid");
	Check(range.first == Table::kPageCount - 1 && range.last_exclusive == Table::kPageCount,
	      "last guest byte maps to the final page");
	Check(!Table::TryGetPageRange(0, 0, range), "empty ranges are rejected");
	Check(!Table::TryGetPageRange(Table::kAddressSpaceSize, 1, range),
	      "first out-of-range byte is rejected");
	Check(!Table::TryGetPageRange(Table::kAddressSpaceSize - 1, 2, range),
	      "crossing the address-space end is rejected");
	Check(!Table::TryGetPageRange(UINT64_MAX - 1, 4, range), "wrapping input is rejected");

	Table table;
	table.GetOrCreate(Table::kPageCount - 1).push_back(99);
	Check(table.Find(Table::kPageCount - 1) != nullptr &&
	          table.Find(Table::kPageCount - 1)->front() == 99,
	      "final page supports allocating and nonallocating access");
}

void TestInlineOwnerStorageAndOverflow() {
	PageOwners owners;
	for (uint32_t owner = 1; owner <= 18; ++owner) {
		owners.push_back(owner);
	}
	Check(owners.size() == 18 && owners.front() == 1,
	      "inline owner storage grows past its 16-owner capacity");
	uint32_t expected = 1;
	for (const uint32_t owner: owners) {
		Check(owner == expected++, "overflow preserves registration order");
	}
	Check(owners.Erase(5) && owners.size() == 17 && !owners.Contains(5),
	      "overflow erase removes only the requested owner");
	Check(owners.Erase(18) && owners.size() == 16,
	      "overflow storage shrinks back to inline capacity");
	const std::vector<uint32_t> remaining {1, 2, 3, 4, 6, 7, 8, 9,
	                                       10, 11, 12, 13, 14, 15, 16, 17};
	size_t                      remaining_index = 0;
	for (const uint32_t owner: owners) {
		Check(owner == remaining[remaining_index++],
		      "overflow-to-inline shrink preserves every owner");
	}
	Check(!owners.Erase(99), "missing inline owner is reported without mutation");

	PageOwners moved = std::move(owners);
	Check(owners.empty() && moved.size() == remaining.size() && moved.front() == 1,
	      "moving a full inline owner list leaves the source empty");
	PageOwners partial;
	partial.push_back(41);
	partial.push_back(42);
	PageOwners partial_moved = std::move(partial);
	Check(partial.empty() && partial_moved.size() == 2 && partial_moved[1] == 42,
	      "moving a partially populated list copies only live owners");
	PageOwners assigned;
	assigned.push_back(99);
	assigned = std::move(partial_moved);
	Check(partial_moved.empty() && assigned.size() == 2 && assigned.front() == 41,
	      "move assignment replaces an inline list without reading inactive slots");
	PageOwners empty;
	PageOwners empty_moved = std::move(empty);
	Check(empty.empty() && empty_moved.empty(), "moving an empty owner list is safe");
}

void TestOneMiBRegistrationGranularity() {
	static_assert(OwnerTable::kPageBits == 20);
	OwnerTable            table;
	OwnerTable::PageRange pages {};
	constexpr uint64_t    range_size = 64ull * 1024 * 1024;
	Check(OwnerTable::TryGetPageRange(0, range_size, pages), "large owner range is valid");
	Check(pages.first == 0 && pages.last_exclusive == 64,
	      "64 MiB registration touches exactly 64 one-MiB entries");
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		table[page].push_back(7);
	}
	Check(table.AllocatedBucketCount() == 1,
	      "large registration uses one sparse second-level bucket");
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		auto* owners = table.Find(page);
		Check(owners != nullptr && owners->size() == 1 && owners->front() == 7,
		      "each touched one-MiB entry retains the owner once");
		Check(owners->Erase(7), "large owner unregisters by coarse page");
	}
}

void TestSharedCoarsePageLifecycle() {
	OwnerTable table;
	auto&      owners = table[2];
	owners.push_back(11);
	owners.push_back(22);
	Check(owners.size() == 2, "two images share one coarse page");
	Check(owners.Erase(11) && owners.size() == 1 && owners.front() == 22,
	      "unregistering one image preserves its coarse-page neighbor");
	Check(!owners.Erase(11), "double unregister is rejected without mutation");
	Check(owners.Erase(22) && owners.empty(), "final coarse-page owner unregisters");
}

void TestOneMiBBoundaries() {
	OwnerTable::PageRange range {};
	Check(OwnerTable::TryGetPageRange(0x0fffff, 2, range),
	      "range crossing a one-MiB boundary is valid");
	Check(range.first == 0 && range.last_exclusive == 2,
	      "cross-boundary registration touches both coarse pages");
	Check(OwnerTable::TryGetPageRange(OwnerTable::kAddressSpaceSize - 1, 1, range),
	      "final guest byte maps to a coarse owner page");
	Check(range.first == OwnerTable::kPageCount - 1 &&
	          range.last_exclusive == OwnerTable::kPageCount,
	      "final guest byte uses the final one-MiB page");
}

} // namespace

int main() {
	TestMultiOwnerAndExactErase();
	TestCrossBucketRange();
	TestQueriesDoNotAllocate();
	TestAddressSpaceBoundaries();
	TestInlineOwnerStorageAndOverflow();
	TestOneMiBRegistrationGranularity();
	TestSharedCoarsePageLifecycle();
	TestOneMiBBoundaries();
	std::printf("ImagePageTableTests: all cases passed\n");
	return 0;
}
