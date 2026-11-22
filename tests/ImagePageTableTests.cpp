#include "common/smallVector.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using Owners     = std::vector<uint32_t>;
using Table      = Libs::Graphics::MultiLevelPageTable<Owners, 20, 44, 14>;
using PageOwners = Common::SmallVector<uint32_t, 3, uint32_t>;
using OwnerTable = Libs::Graphics::MultiLevelPageTable<PageOwners, 20, 44, 14>;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ImagePageTableTests: failed: %s\n", text);
		std::abort();
	}
}

bool Erase(PageOwners& owners, uint32_t owner) {
	const auto found = std::ranges::find(owners, owner);
	if (found == owners.end()) {
		return false;
	}
	owners.erase(found);
	return true;
}

void TestMultiOwnerAndExactErase() {
	OwnerTable table;
	auto& owners = table[17];
	owners.push_back(11);
	owners.push_back(22);

	Check(table.Find(17) != nullptr && table.Find(17)->size() == 2,
	      "both page owners are retained");
	Check(Erase(owners, 11), "registered owner is erased");
	Check(owners.size() == 1 && owners.front() == 22, "erasing one owner preserves its neighbor");
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
	Check(table.Find(range.first - 1) != nullptr && table.Find(range.last_exclusive) != nullptr,
	      "pages across the L1 boundary allocate their sparse buckets");
}

void TestQueriesDoNotAllocate() {
	Table table;
	Check(table.Find(123) == nullptr, "unallocated page query is empty");
	Check(table.Find(123) == nullptr, "mutable query does not allocate");
	const Table& const_table = table;
	Check(const_table.Find(Table::kPageCount - 1) == nullptr, "const query is empty");
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
	table[Table::kPageCount - 1].push_back(99);
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
	      "inline owner storage grows past its 3-owner capacity");
	uint32_t expected = 1;
	for (const uint32_t owner: owners) {
		Check(owner == expected++, "overflow preserves registration order");
	}
	Check(Erase(owners, 5) && owners.size() == 17,
	      "overflow erase removes only the requested owner");
	Check(Erase(owners, 18) && owners.size() == 16, "erasing the last owner keeps the rest");
	const std::vector<uint32_t> remaining {1, 2, 3, 4, 6, 7, 8, 9,
	                                       10, 11, 12, 13, 14, 15, 16, 17};
	Check(std::ranges::equal(owners, remaining), "erasing preserves every other owner in order");
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
	Check(table.Find(OwnerTable::kBucketEntries) == nullptr,
	      "large registration uses one sparse second-level bucket");
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		auto* owners = table.Find(page);
		Check(owners != nullptr && owners->size() == 1 && owners->front() == 7,
		      "each touched one-MiB entry retains the owner once");
		Check(Erase(*owners, 7), "large owner unregisters by coarse page");
	}
}

void TestSharedCoarsePageLifecycle() {
	OwnerTable table;
	auto&      owners = table[2];
	owners.push_back(11);
	owners.push_back(22);
	Check(owners.size() == 2, "two images share one coarse page");
	Check(Erase(owners, 11) && owners.size() == 1 && owners.front() == 22,
	      "unregistering one image preserves its coarse-page neighbor");
	Check(Erase(owners, 22) && owners.size() == 0, "final coarse-page owner unregisters");
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
