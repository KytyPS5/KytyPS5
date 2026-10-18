#include "graphics/host_gpu/renderer/cache/imagePageCoverage.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Owners     = std::vector<uint32_t>;
using Table      = Libs::Graphics::MultiLevelPageTable<Owners, 20, 44, 14>;
using PageOwners = Libs::Graphics::InlinePageOwnerList<uint32_t, 16>;
using OwnerTable = Libs::Graphics::MultiLevelPageTable<PageOwners, 20, 44, 14>;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ImagePageTableTests: failed: %s\n", text);
		std::abort();
	}
}

void TestMultiOwnerAndExactErase() {
	OwnerTable table;
	auto& owners = table.GetOrCreate(17);
	owners.push_back(11);
	owners.push_back(22);

	Check(table.Find(17) != nullptr && table.Find(17)->size() == 2,
	      "both page owners are retained");
	Check(owners.Erase(11U), "registered owner is erased");
	Check(owners.size() == 1 && owners.front() == 22, "erasing one owner preserves its neighbor");
	Check(!owners.Erase(33U), "missing owner is reported without mutation");
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

using Libs::Graphics::ImagePageCoverage;
using Libs::Graphics::LOWER_ADDRESS_SIZE;
using Libs::Graphics::TRACKER_PAGE_SIZE;
using Libs::Graphics::TRACKER_REGION_SIZE;
constexpr uint64_t ExtendedBase = Libs::LibKernel::Memory::kExtendedMemoryBase;
constexpr uint64_t ExtendedSize = Libs::LibKernel::Memory::kExtendedMemorySize;

void TestCoverageUsesTrackerPages() {
	auto coverage = std::make_unique<ImagePageCoverage>();
	coverage->Add(0x10000, 0x1000);
	Check(coverage->Covers(0x10000) && coverage->Covers(0x10fff), "exact page is covered");
	Check(!coverage->Covers(0xffff) && !coverage->Covers(0x11000), "neighbor pages are not");

	coverage->Add(0x21fff, 2);
	Check(coverage->Covers(0x21000) && coverage->Covers(0x22fff),
	      "an unaligned two-byte image covers both of its pages");
	Check(!coverage->Covers(0x20fff) && !coverage->Covers(0x23000),
	      "an unaligned image covers nothing past its pages");

	coverage->Remove(0x10000, 0x1000);
	coverage->Remove(0x21fff, 2);
	Check(!coverage->Covers(0x10000) && !coverage->Covers(0x21000) && !coverage->Covers(0x22000),
	      "removed images leave no coverage");
}

void TestCoverageCountsOverlappingImages() {
	auto coverage = std::make_unique<ImagePageCoverage>();
	coverage->Add(0x40000, 0x3000);
	coverage->Add(0x41800, 0x1000);
	coverage->Remove(0x40000, 0x3000);
	Check(!coverage->Covers(0x40000) && coverage->Covers(0x41000) &&
	          coverage->Covers(0x42000) && !coverage->Covers(0x43000),
	      "a page stays covered while another image still touches it");
	coverage->Remove(0x41800, 0x1000);
	Check(!coverage->Covers(0x41000) && !coverage->Covers(0x42000),
	      "the last image leaving a page uncovers it");
}

void TestCoverageCrossesRegions() {
	auto           coverage = std::make_unique<ImagePageCoverage>();
	const uint64_t boundary = 5 * TRACKER_REGION_SIZE;
	coverage->Add(boundary - 0x10, 0x20);
	Check(coverage->Covers(boundary - 1) && coverage->Covers(boundary),
	      "both sides of a region boundary are covered");
	Check(!coverage->Covers(boundary - TRACKER_PAGE_SIZE - 1) &&
	          !coverage->Covers(boundary + TRACKER_PAGE_SIZE),
	      "a boundary image covers only its two pages");

	coverage->Add(boundary, 3 * TRACKER_REGION_SIZE);
	Check(coverage->Covers(boundary + 2 * TRACKER_REGION_SIZE + 0x1234) &&
	          !coverage->Covers(boundary + 3 * TRACKER_REGION_SIZE),
	      "a multi-region image covers every region it spans and no more");
	coverage->Remove(boundary, 3 * TRACKER_REGION_SIZE);
	coverage->Remove(boundary - 0x10, 0x20);
	Check(!coverage->Covers(boundary) && !coverage->Covers(boundary + TRACKER_REGION_SIZE),
	      "multi-region removal clears every region");
}

void TestCoverageAddressRanges() {
	auto coverage = std::make_unique<ImagePageCoverage>();
	coverage->Add(LOWER_ADDRESS_SIZE - TRACKER_PAGE_SIZE, 2 * TRACKER_PAGE_SIZE);
	Check(coverage->Covers(LOWER_ADDRESS_SIZE - 1) && !coverage->Covers(LOWER_ADDRESS_SIZE),
	      "the lower range ends at its last page");

	coverage->Add(ExtendedBase + 0x123456789, 0x10);
	coverage->Add(ExtendedBase + ExtendedSize - 1, 1);
	Check(coverage->Covers(ExtendedBase + 0x123456000) &&
	          coverage->Covers(ExtendedBase + ExtendedSize - TRACKER_PAGE_SIZE),
	      "extended memory is covered");
	Check(!coverage->Covers(ExtendedBase - 1) && !coverage->Covers(ExtendedBase + ExtendedSize) &&
	          !coverage->Covers(UINT64_MAX),
	      "addresses past the extended range are never covered");

	coverage->Add(ExtendedBase - TRACKER_PAGE_SIZE, TRACKER_PAGE_SIZE);
	Check(!coverage->Covers(ExtendedBase - TRACKER_PAGE_SIZE) && !coverage->Covers(ExtendedBase),
	      "an image in the guest address gap covers no reachable page");
	coverage->Remove(ExtendedBase - TRACKER_PAGE_SIZE, TRACKER_PAGE_SIZE);
	coverage->Remove(ExtendedBase + ExtendedSize - 1, 1);
	coverage->Remove(ExtendedBase + 0x123456789, 0x10);
	coverage->Remove(LOWER_ADDRESS_SIZE - TRACKER_PAGE_SIZE, 2 * TRACKER_PAGE_SIZE);
	Check(!coverage->Covers(LOWER_ADDRESS_SIZE - 1) && !coverage->Covers(ExtendedBase + 0x123456000),
	      "removal clears both address ranges");
}

void TestCoverageMatchesImageList() {
	struct Range {
		uint64_t address;
		uint64_t size;
	};
	constexpr uint64_t Span     = 3 * TRACKER_REGION_SIZE;
	constexpr uint64_t Base     = 7 * TRACKER_REGION_SIZE - TRACKER_REGION_SIZE / 2;
	auto               coverage = std::make_unique<ImagePageCoverage>();
	std::vector<Range> images;
	uint64_t           state = 0x9e3779b97f4a7c15ull;
	const auto         next  = [&state] {
		state ^= state << 13u;
		state ^= state >> 7u;
		state ^= state << 17u;
		return state;
	};
	for (int step = 0; step < 4000; step++) {
		if (images.empty() || next() % 3 != 0) {
			const Range image {Base + next() % Span, 1 + next() % (TRACKER_REGION_SIZE / 2)};
			coverage->Add(image.address, image.size);
			images.push_back(image);
		} else {
			const auto index = next() % images.size();
			coverage->Remove(images[index].address, images[index].size);
			images.erase(images.begin() + static_cast<std::ptrdiff_t>(index));
		}
		const uint64_t probe   = Base - TRACKER_PAGE_SIZE + next() % (Span + TRACKER_REGION_SIZE);
		const uint64_t page    = probe / TRACKER_PAGE_SIZE;
		const bool     touched = std::any_of(images.begin(), images.end(), [&](const Range& image) {
			return image.address / TRACKER_PAGE_SIZE <= page &&
			       page <= (image.address + image.size - 1) / TRACKER_PAGE_SIZE;
		});
		Check(coverage->Covers(probe) == touched, "coverage matches the registered image list");
	}
}

void TestCoveragePublishesAcrossThreads() {
	constexpr uint64_t Regions  = 256;
	constexpr uint64_t Base     = 64 * TRACKER_REGION_SIZE;
	constexpr uint64_t Pinned   = Base - TRACKER_PAGE_SIZE;
	auto               coverage = std::make_unique<ImagePageCoverage>();
	coverage->Add(Pinned, TRACKER_PAGE_SIZE);
	std::atomic<uint64_t> published {0};
	std::atomic<bool>     failed {false};
	std::thread           reader([&] {
		uint64_t seen = 0;
		while (seen < Regions) {
			seen = published.load(std::memory_order_acquire);
			for (uint64_t region = 0; region < seen; region++) {
				if (!coverage->Covers(Base + region * TRACKER_REGION_SIZE + 0x2345)) {
					failed.store(true, std::memory_order_relaxed);
				}
			}
			if (!coverage->Covers(Pinned)) {
				failed.store(true, std::memory_order_relaxed);
			}
		}
	});
	for (uint64_t region = 0; region < Regions; region++) {
		coverage->Add(Base + region * TRACKER_REGION_SIZE, TRACKER_REGION_SIZE);
		coverage->Add(Pinned, TRACKER_PAGE_SIZE);
		coverage->Remove(Pinned, TRACKER_PAGE_SIZE);
		published.store(region + 1, std::memory_order_release);
	}
	reader.join();
	Check(!failed.load(), "a reader sees every published registration and no transient gap");
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
	TestCoverageUsesTrackerPages();
	TestCoverageCountsOverlappingImages();
	TestCoverageCrossesRegions();
	TestCoverageAddressRanges();
	TestCoverageMatchesImageList();
	TestCoveragePublishesAcrossThreads();
	std::printf("ImagePageTableTests: all cases passed\n");
	return 0;
}
