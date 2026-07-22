#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOURCERANGE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOURCERANGE_H_

#include <cstdint>
#include <optional>
#include <span>

namespace Libs::Graphics {

struct GuestRange final {
	uint64_t address = 0;
	uint64_t size    = 0;

	[[nodiscard]] constexpr bool IsValid() const noexcept {
		return size != 0 && size <= UINT64_MAX - address;
	}

	[[nodiscard]] constexpr std::optional<uint64_t> EndExclusive() const noexcept {
		return IsValid() ? std::optional<uint64_t> {address + size} : std::nullopt;
	}
};

// Describes the first range relative to the second range. SharedPageOnly means that the byte
// ranges are disjoint but share at least one page at the requested granularity.
enum class RangeRelation : uint8_t {
	Disjoint,
	SharedPageOnly,
	Exact,
	Contains,
	ContainedBy,
	PartialOverlap
};

enum class RangeSetCoverage : uint8_t { Disjoint, ContainsAll, Partial };

[[nodiscard]] constexpr bool IsPowerOfTwo(uint64_t value) noexcept {
	return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] constexpr std::optional<RangeRelation>
ClassifyRangeRelation(GuestRange left, GuestRange right, uint64_t page_size) noexcept {
	if (!left.IsValid() || !right.IsValid() || !IsPowerOfTwo(page_size)) {
		return std::nullopt;
	}

	const auto left_end  = *left.EndExclusive();
	const auto right_end = *right.EndExclusive();
	if (left.address >= right_end || right.address >= left_end) {
		const auto left_first  = left.address / page_size;
		const auto left_last   = (left_end - 1) / page_size;
		const auto right_first = right.address / page_size;
		const auto right_last  = (right_end - 1) / page_size;
		return left_first <= right_last && right_first <= left_last
		           ? RangeRelation::SharedPageOnly
		           : RangeRelation::Disjoint;
	}
	if (left.address == right.address && left.size == right.size) {
		return RangeRelation::Exact;
	}
	if (left.address <= right.address && left_end >= right_end) {
		return RangeRelation::Contains;
	}
	if (right.address <= left.address && right_end >= left_end) {
		return RangeRelation::ContainedBy;
	}
	return RangeRelation::PartialOverlap;
}

[[nodiscard]] constexpr bool HasByteOverlap(RangeRelation relation) noexcept {
	switch (relation) {
		case RangeRelation::Exact:
		case RangeRelation::Contains:
		case RangeRelation::ContainedBy:
		case RangeRelation::PartialOverlap: return true;
		case RangeRelation::Disjoint:
		case RangeRelation::SharedPageOnly: return false;
	}
	return false;
}

[[nodiscard]] constexpr bool HasPageOverlap(RangeRelation relation) noexcept {
	return relation == RangeRelation::SharedPageOnly || HasByteOverlap(relation);
}

// Classifies one range against an atomic set of owner ranges. A byte hit may retire the owner only
// when every range is contained; page-only neighbors remain disjoint at this layer.
[[nodiscard]] constexpr std::optional<RangeSetCoverage>
ClassifyRangeSetCoverage(GuestRange range, std::span<const GuestRange> owner_ranges) noexcept {
	if (!range.IsValid() || owner_ranges.empty()) {
		return std::nullopt;
	}
	bool overlaps = false;
	bool contains_all = true;
	for (const auto owner_range: owner_ranges) {
		const auto relation = ClassifyRangeRelation(range, owner_range, 1);
		if (!relation.has_value()) {
			return std::nullopt;
		}
		overlaps |= HasByteOverlap(*relation);
		contains_all &=
		    *relation == RangeRelation::Exact || *relation == RangeRelation::Contains;
	}
	if (!overlaps) {
		return RangeSetCoverage::Disjoint;
	}
	return contains_all ? RangeSetCoverage::ContainsAll : RangeSetCoverage::Partial;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOURCERANGE_H_
