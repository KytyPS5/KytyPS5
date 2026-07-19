#include "graphics/host_gpu/renderer/multiLevelPageTable.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace {

using Owners = std::vector<uint32_t>;
using Table  = Libs::Graphics::MultiLevelPageTable<Owners>;
using OwnerIndex = Libs::Graphics::MultiRangePageOwnerIndex<uint32_t>;

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

	Check(table.Find(17) != nullptr && table.Find(17)->size() == 2, "both page owners are retained");
	Check(Libs::Graphics::EraseExact(owners, 11U), "registered owner is erased");
	Check(owners.size() == 1 && owners.front() == 22, "erasing one owner preserves its neighbor");
	Check(!Libs::Graphics::EraseExact(owners, 33U), "missing owner is reported without mutation");
}

void TestCrossBucketRange() {
	Table::PageRange range{};
	constexpr uint64_t bucket_boundary = uint64_t{Table::kBucketEntries} << Table::kPageBits;
	Check(Table::TryGetPageRange(bucket_boundary - 1, 2, range), "cross-bucket range is valid");
	Check(range.first == Table::kBucketEntries - 1 && range.last_exclusive == Table::kBucketEntries + 1,
	      "cross-bucket range covers both pages");

	Table table;
	table[range.first].push_back(1);
	table[range.last_exclusive - 1].push_back(2);
	Check(table.AllocatedBucketCount() == 2, "pages across the L1 boundary use distinct sparse buckets");
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
	Table::PageRange range{};
	Check(Table::TryGetPageRange(Table::kAddressSpaceSize - 1, 1, range), "last guest byte is valid");
	Check(range.first == Table::kPageCount - 1 && range.last_exclusive == Table::kPageCount,
	      "last guest byte maps to the final page");
	Check(!Table::TryGetPageRange(0, 0, range), "empty ranges are rejected");
	Check(!Table::TryGetPageRange(Table::kAddressSpaceSize, 1, range), "first out-of-range byte is rejected");
	Check(!Table::TryGetPageRange(Table::kAddressSpaceSize - 1, 2, range), "crossing the address-space end is rejected");
	Check(!Table::TryGetPageRange(UINT64_MAX - 1, 4, range), "wrapping input is rejected");

	Table table;
	table.GetOrCreate(Table::kPageCount - 1).push_back(99);
	Check(table.Find(Table::kPageCount - 1) != nullptr && table.Find(Table::kPageCount - 1)->front() == 99,
	      "final page supports allocating and nonallocating access");
	bool  threw = false;
	try {
		(void)table.GetOrCreate(Table::kPageCount);
	} catch (const std::out_of_range&) {
		threw = true;
	}
	Check(threw, "allocating access hard-fails outside the guest address space");
}

void TestMultiRangeRegistrationDeduplicatesPages() {
	OwnerIndex index;
	// Depth and stencil-like planes overlap tracking pages and share one 1 MiB bucket.
	Check(index.Register(7, {{0x101000, 0x2800}, {0x102000, 0x3000}}), "multi-range owner registers");
	Check(index.CoarseMembershipCount(1) == 1, "one owner is inserted once in a shared 1 MiB bucket");
	Check(index.TrackingMembershipCount(0x102) == 1, "overlapping planes insert one 4 KiB membership");
	Check(!index.Register(7, {{0x101000, 0x1000}}), "duplicate owner registration hard-fails");

	const auto owners = index.Query(0x100000, 0x10000);
	Check(owners.size() == 1 && owners.front() == 7, "multi-page query returns an owner once");
}

void TestSharedPageUnregisterLifecycle() {
	OwnerIndex index;
	const std::vector<OwnerIndex::ByteRange> ranges{{0x202000, 0x2000}};
	Check(index.Register(11, ranges) && index.Register(22, ranges), "two owners register on identical pages");
	Check(index.CoarseMembershipCount(2) == 2 && index.TrackingMembershipCount(0x202) == 2,
	      "coarse and tracking pages retain both owners");

	std::vector<OwnerIndex::ByteRange> releases;
	Check(index.Unregister(11, releases), "first owner unregisters");
	Check(releases.empty(), "shared tracking pages are not released with one owner remaining");
	Check(index.Query(0x202000, 1).size() == 1 && index.Query(0x202000, 1).front() == 22,
	      "unregistering one owner preserves the other");
	Check(!index.Unregister(11, releases), "missing membership hard-fails without mutation");

	Check(index.Unregister(22, releases), "final owner unregisters");
	Check(releases.size() == 1 && releases.front().address == 0x202000 && releases.front().size == 0x2000,
	      "adjacent final-owner tracking pages return one contiguous release");
}

void TestStrictByteFilteringAndPredicate() {
	OwnerIndex index;
	Check(index.Register(31, {{0x300100, 0x100}}), "first byte-disjoint owner registers");
	Check(index.Register(32, {{0x300800, 0x100}}), "second byte-disjoint owner registers");
	Check(index.TrackingMembershipCount(0x300) == 2, "byte-disjoint owners share one tracking page");
	Check(index.Query(0x300400, 0x40).empty(), "page hit without byte overlap is filtered out");
	const auto page_candidates = index.QueryCandidates(0x300400, 0x40);
	Check(page_candidates.size() == 2, "fault candidate query retains byte-disjoint owners on the touched page");
	const auto first = index.Query(0x300180, 0x10);
	Check(first.size() == 1 && first.front() == 31, "strict byte overlap selects only the matching owner");
	const auto predicate_filtered = index.Query(0x300000, 0x1000, [](uint32_t owner) { return owner == 32; });
	Check(predicate_filtered.size() == 1 && predicate_filtered.front() == 32, "supplied predicate filters query owners");
}

} // namespace

int main() {
	TestMultiOwnerAndExactErase();
	TestCrossBucketRange();
	TestQueriesDoNotAllocate();
	TestAddressSpaceBoundaries();
	TestMultiRangeRegistrationDeduplicatesPages();
	TestSharedPageUnregisterLifecycle();
	TestStrictByteFilteringAndPredicate();
	std::printf("ImagePageTableTests: all cases passed\n");
	return 0;
}
