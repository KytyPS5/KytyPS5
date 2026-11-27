#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_PATCH_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_PATCH_H_

#include <concepts>
#include <cstddef>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Libs::Graphics::SystemUi {

/// The most fields Patch can take apart in one struct. Raise it by adding a case to Fields.
inline constexpr size_t MAX_PATCH_FIELDS = 16;

namespace PatchDetail {

/// Converts to any type, so a struct can be brace-initialised with N of these exactly when it has
/// at least N fields. A value that converts to a nested struct initialises it whole, rather than
/// its first field, so a nested struct counts as one field.
struct AnyField {
	/// Converting implicitly is the point. Never called, only named while counting, but
	/// std::optional instantiates it, so it needs a body.
	template <class Type>
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	constexpr operator Type() const noexcept {
		return Type {};
	}
};

/// Whether `Type` can be brace-initialised with as many AnyFields as `count` holds.
template <class Type, size_t... Index>
[[nodiscard]] constexpr bool BuildsFrom(std::index_sequence<Index...> /*count*/) {
	return requires { Type {(static_cast<void>(Index), AnyField {})...}; };
}

/// The number of fields of the aggregate `Type`, the most AnyFields it can be built from.
template <class Type, size_t Count = 0>
[[nodiscard]] constexpr size_t FieldCount() {
	if constexpr (Count < MAX_PATCH_FIELDS + 1 &&
	              BuildsFrom<Type>(std::make_index_sequence<Count + 1> {})) {
		return FieldCount<Type, Count + 1>();
	} else {
		return Count;
	}
}

/// Whether `Type` is a std::optional, whose field overrides when it holds a value.
template <class Type>
inline constexpr bool IS_OPTIONAL = false;
template <class Value>
inline constexpr bool IS_OPTIONAL<std::optional<Value>> = true;

/// References to every field of `object`, in declaration order.
template <class Type>
[[nodiscard]] constexpr auto Fields(Type& object) {
	constexpr size_t COUNT = FieldCount<std::remove_const_t<Type>>();
	if constexpr (COUNT == 1) {
		auto& [f0] = object;
		return std::tie(f0);
	} else if constexpr (COUNT == 2) {
		auto& [f0, f1] = object;
		return std::tie(f0, f1);
	} else if constexpr (COUNT == 3) {
		auto& [f0, f1, f2] = object;
		return std::tie(f0, f1, f2);
	} else if constexpr (COUNT == 4) {
		auto& [f0, f1, f2, f3] = object;
		return std::tie(f0, f1, f2, f3);
	} else if constexpr (COUNT == 5) {
		auto& [f0, f1, f2, f3, f4] = object;
		return std::tie(f0, f1, f2, f3, f4);
	} else if constexpr (COUNT == 6) {
		auto& [f0, f1, f2, f3, f4, f5] = object;
		return std::tie(f0, f1, f2, f3, f4, f5);
	} else if constexpr (COUNT == 7) {
		auto& [f0, f1, f2, f3, f4, f5, f6] = object;
		return std::tie(f0, f1, f2, f3, f4, f5, f6);
	} else if constexpr (COUNT == 8) {
		auto& [f0, f1, f2, f3, f4, f5, f6, f7] = object;
		return std::tie(f0, f1, f2, f3, f4, f5, f6, f7);
	} else if constexpr (COUNT == 9) {
		auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8] = object;
		return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8);
	} else if constexpr (COUNT == 10) {
		auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9] = object;
		return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9);
	} else if constexpr (COUNT == 11) {
		auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10] = object;
		return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10);
	} else if constexpr (COUNT == 12) {
		auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11] = object;
		return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11);
	} else if constexpr (COUNT == 13) {
		auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12] = object;
		return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12);
	} else if constexpr (COUNT == 14) {
		auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13] = object;
		return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13);
	} else if constexpr (COUNT == 15) {
		auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14] = object;
		return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14);
	} else if constexpr (COUNT == 16) {
		auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15] = object;
		return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15);
	} else {
		// None found means the struct is empty or a field can't be counted, and patching it would
		// silently do nothing.
		static_assert(COUNT >= 1, "No fields found to patch.");
		static_assert(COUNT <= MAX_PATCH_FIELDS, "Too many fields to patch. Add a case to Fields.");
		return std::tuple<> {};
	}
}

/// Declared ahead, as patching a field that is a struct patches into it in turn.
template <class Type>
constexpr void PatchInto(Type& patched, const Type& overrides, const Type& blank);

/// One field. An optional overrides when it holds a value, even zero. A nested struct with no == is
/// patched field by field. Any other field overrides when it differs from `blank`, the field as an
/// empty struct leaves it, so a field set back to that keeps the default, which is the price of
/// plain fields. Comparing with the empty struct rather than the type's zero keeps a field with a
/// default of its own, such as a line count of 3, from overriding whenever it is left out.
template <class Field>
constexpr void PatchField(Field& patched, const Field& overrides, const Field& blank) {
	if constexpr (IS_OPTIONAL<Field>) {
		if (overrides.has_value()) {
			patched = overrides;
		}
	} else if constexpr (std::equality_comparable<Field>) {
		if (!(overrides == blank)) {
			patched = overrides;
		}
	} else {
		static_assert(std::is_aggregate_v<Field>,
		              "A field must be optional, comparable with ==, or a struct to patch into.");
		PatchInto(patched, overrides, blank);
	}
}

/// Patches every field of `patched` from the same field of `overrides`, judged against `blank`.
template <class Type>
constexpr void PatchInto(Type& patched, const Type& overrides, const Type& blank) {
	auto       to    = Fields(patched);
	const auto from  = Fields(overrides);
	const auto empty = Fields(blank);
	[&]<size_t... Index>(std::index_sequence<Index...>) {
		(PatchField(std::get<Index>(to), std::get<Index>(from), std::get<Index>(empty)), ...);
	}(std::make_index_sequence<std::tuple_size_v<decltype(to)>> {});
}

} // namespace PatchDetail

/// `defaults` with each field `overrides` sets laid over it, so a caller restyling a component
/// names only what it changes, as in `Patch(defaults, {.radius = Dp(4.0f)})`. An optional field is
/// set when it holds a value, so it can override to zero, transparent or none, and should default
/// to empty. Any other field is set when it differs from an empty `Type {}`, its zero or its own
/// default, so a plain field can't override back to that, and a field that must be able to is made
/// optional. Nested structs with no == are patched field by field. `Type` must be an aggregate of
/// at most MAX_PATCH_FIELDS fields, each optional, comparable with ==, or such a struct itself.
template <class Type>
[[nodiscard]] constexpr Type Patch(const Type& defaults, const Type& overrides) {
	static_assert(std::is_aggregate_v<Type>, "Patch takes apart aggregates only.");
	const Type blank {};
	Type       patched = defaults;
	PatchDetail::PatchInto(patched, overrides, blank);
	return patched;
}

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_PATCH_H_
