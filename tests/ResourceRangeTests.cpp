#include "graphics/host_gpu/renderer/resourceRange.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::ClassifyRangeRelation;
using Libs::Graphics::GuestRange;
using Libs::Graphics::HasByteOverlap;
using Libs::Graphics::HasPageOverlap;
using Libs::Graphics::RangeRelation;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "ResourceRangeTests: failed: %s\n", message);
		std::abort();
	}
}

void CheckRelations() {
	constexpr uint64_t page = 4096;
	Check(ClassifyRangeRelation({0x10020, 0x20}, {0x10020, 0x20}, page) ==
	          RangeRelation::Exact,
	      "equal ranges are exact");
	Check(ClassifyRangeRelation({0x10000, 0x100}, {0x10020, 0x20}, page) ==
	          RangeRelation::Contains,
	      "larger left range contains right range");
	Check(ClassifyRangeRelation({0x10020, 0x20}, {0x10000, 0x100}, page) ==
	          RangeRelation::ContainedBy,
	      "smaller left range is contained by right range");
	Check(ClassifyRangeRelation({0x10080, 0x100}, {0x10100, 0x100}, page) ==
	          RangeRelation::PartialOverlap,
	      "intersecting ranges partially overlap");
	Check(ClassifyRangeRelation({0x10000, 0x20}, {0x10800, 0x20}, page) ==
	          RangeRelation::SharedPageOnly,
	      "byte-disjoint ranges sharing a page are page neighbors");
	Check(ClassifyRangeRelation({0x10000, 0x20}, {0x11000, 0x20}, page) ==
	          RangeRelation::Disjoint,
	      "ranges on distinct pages are disjoint");
}

void CheckBoundaries() {
	constexpr uint64_t page = 4096;
	Check(ClassifyRangeRelation({0, 1}, {0, 1}, page) == RangeRelation::Exact,
	      "address zero is a valid generic byte range");
	Check(!ClassifyRangeRelation({UINT64_MAX, 1}, {UINT64_MAX, 1}, page).has_value(),
	      "overflowing ranges are invalid");
	Check(!ClassifyRangeRelation({0x1000, 0}, {0x1000, 1}, page).has_value(),
	      "empty ranges are invalid");
	Check(!ClassifyRangeRelation({0x1000, 1}, {0x1000, 1}, 0).has_value(),
	      "zero page size is invalid");
	Check(!ClassifyRangeRelation({0x1000, 1}, {0x1000, 1}, 3).has_value(),
	      "non-power-of-two page size is invalid");
	Check(ClassifyRangeRelation({0x1fff, 1}, {0x2000, 1}, page) ==
	          RangeRelation::Disjoint,
	      "adjacent ranges on distinct pages are disjoint");
	Check(ClassifyRangeRelation({0x1ffe, 1}, {0x1fff, 1}, page) ==
	          RangeRelation::SharedPageOnly,
	      "adjacent ranges within one page are page neighbors");
	Check(ClassifyRangeRelation({UINT64_MAX - 1, 1}, {UINT64_MAX - 1, 1}, page) ==
	          RangeRelation::Exact,
	      "the highest non-overflowing byte range is valid");
	Check(ClassifyRangeRelation({0, UINT64_MAX}, {UINT64_MAX - 1, 1}, page) ==
	          RangeRelation::Contains,
	      "a range ending exactly at UINT64_MAX is valid");
}

void CheckPredicates() {
	constexpr auto exact = RangeRelation::Exact;
	constexpr auto page  = RangeRelation::SharedPageOnly;
	Check(HasByteOverlap(exact) && HasPageOverlap(exact), "exact ranges overlap bytes and pages");
	Check(!HasByteOverlap(page) && HasPageOverlap(page),
	      "page neighbors overlap pages but not bytes");
}

[[nodiscard]] constexpr RangeRelation Reverse(RangeRelation relation) {
	switch (relation) {
		case RangeRelation::Contains: return RangeRelation::ContainedBy;
		case RangeRelation::ContainedBy: return RangeRelation::Contains;
		default: return relation;
	}
}

void CheckAlgebra() {
	constexpr GuestRange ranges[] {{0, 1},       {1, 1},       {0xfff, 2},
	                               {0x1000, 1},  {0x1000, 32}, {0x1010, 4},
	                               {0x1800, 64}, {0x2000, 8},  {0x2800, 0x1800}};
	constexpr uint64_t page_sizes[] {1, 2, 4, 4096};
	for (const auto page_size: page_sizes) {
		for (const auto left: ranges) {
			for (const auto right: ranges) {
				const auto forward = ClassifyRangeRelation(left, right, page_size);
				const auto reverse = ClassifyRangeRelation(right, left, page_size);
				Check(forward.has_value() && reverse.has_value(),
				      "valid ranges always produce a relation");
				Check(*reverse == Reverse(*forward), "range relation reverses consistently");
				Check(HasByteOverlap(*forward) == HasByteOverlap(*reverse),
				      "byte overlap is symmetric");
				Check(HasPageOverlap(*forward) == HasPageOverlap(*reverse),
				      "page overlap is symmetric");
				const auto expected_bytes =
				    left.address < *right.EndExclusive() && right.address < *left.EndExclusive();
				Check(HasByteOverlap(*forward) == expected_bytes,
				      "relation agrees with half-open byte intersection");
			}
		}
	}
}

static_assert(ClassifyRangeRelation({0x1000, 4}, {0x1000, 4}, 4096) ==
	              RangeRelation::Exact);
static_assert(ClassifyRangeRelation({0x1000, 4}, {0x1001, 1}, 4096) ==
	              RangeRelation::Contains);
static_assert(ClassifyRangeRelation({0x1001, 1}, {0x1000, 4}, 4096) ==
	              RangeRelation::ContainedBy);

} // namespace

int main() {
	CheckRelations();
	CheckBoundaries();
	CheckPredicates();
	CheckAlgebra();
	std::puts("ResourceRangeTests: all cases passed");
	return 0;
}
