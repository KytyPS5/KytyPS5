#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <fmt/format.h>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask            = 0x0000ffffffffffffull;
constexpr uint64_t MaxIndirectDescriptorProbes = 65536u;

bool SpecializationFail(std::string_view message) {
	std::fprintf(stderr, "shader resource specialization failed: %.*s\n",
	             static_cast<int>(message.size()), message.data());
	return false;
}

Decoder::ImageDimension DescriptorDimension(const DescriptorValue&  descriptor,
                                            Decoder::ImageDimension requested) {
	const bool is_array = requested == Decoder::ImageDimension::Dim1DArray ||
	                      requested == Decoder::ImageDimension::Dim2DArray ||
	                      requested == Decoder::ImageDimension::Dim2DMsaaArray;
	switch (static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu)) {
		case Prospero::ImageType::kColor1D: return Decoder::ImageDimension::Dim1D;
		case Prospero::ImageType::kColor1DArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim1DArray;
			}
			return Decoder::ImageDimension::Dim1D;
		case Prospero::ImageType::kColor3D: return Decoder::ImageDimension::Dim3D;
		case Prospero::ImageType::kCube: return Decoder::ImageDimension::Dim2DArray;
		case Prospero::ImageType::kColor2DArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim2DArray;
			}
			return Decoder::ImageDimension::Dim2D;
		case Prospero::ImageType::kColor2DMsaaArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim2DMsaaArray;
			}
			return Decoder::ImageDimension::Dim2DMsaa;
		case Prospero::ImageType::kColor2D: return Decoder::ImageDimension::Dim2D;
		case Prospero::ImageType::kColor2DMsaa: return Decoder::ImageDimension::Dim2DMsaa;
		default: return Decoder::ImageDimension::Unknown;
	}
}

bool NullImageDescriptor(const DescriptorValue& descriptor) {
	return descriptor.dwords[0] == 0 && (descriptor.dwords[1] & 0xffu) == 0;
}

bool ValidImageDescriptor(const DescriptorValue& descriptor, bool r128 = false) {
	const auto& words = descriptor.dwords;
	// Reject texture descriptors with nonzero reserved bits.
	if ((words[1] & 0x20000000u) != 0u || (words[2] & 0x70003000u) != 0u ||
	    (!r128 && ((words[4] & 0xe000e000u) != 0u || (words[5] & 0xf9000000u) != 0u ||
	               (words[6] & 0x00007b00u) != 0u))) {
		return false;
	}
	const auto type   = static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu);
	const auto format = static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
	if (type < Prospero::ImageType::kColor1D || format == Prospero::BufferFormat::kInvalid ||
	    format > Prospero::BufferFormat::kBc7Srgb) {
		return false;
	}
	if (r128 && type != Prospero::ImageType::kColor1D && type != Prospero::ImageType::kColor2D &&
	    type != Prospero::ImageType::kColor2DMsaa) {
		return false;
	}
	const bool array = type == Prospero::ImageType::kColor1DArray ||
	                   type == Prospero::ImageType::kColor2DArray ||
	                   type == Prospero::ImageType::kColor2DMsaaArray ||
	                   type == Prospero::ImageType::kCube;
	if (array && ((words[4] >> 16u) & 0x1fffu) > (words[4] & 0x1fffu)) {
		return false;
	}
	if (type == Prospero::ImageType::kColor2DMsaa ||
	    type == Prospero::ImageType::kColor2DMsaaArray) {
		const auto base_level = (descriptor.dwords[3] >> 12u) & 0xfu;
		const auto fragments  = (descriptor.dwords[3] >> 16u) & 0xfu;
		const auto max_mip    = (descriptor.dwords[5] >> 4u) & 0xfu;
		return base_level == 0 && fragments >= 1 && fragments <= 3 &&
		       (r128 || max_mip == fragments);
	}
	return true;
}

uint32_t DescriptorImageSwizzle(const DescriptorValue& descriptor) {
	return descriptor.dwords[3] & 0xfffu;
}

Prospero::BufferFormat ImageConversionFormat(Prospero::BufferFormat format) {
	return Prospero::RemapTextureFormat(format) != format ? format
	                                                      : Prospero::BufferFormat::kInvalid;
}

enum class SamplerClass : uint8_t { Float, Integer, PointInteger };

SamplerClass ClassifySampler(const ImageResource& image) {
	if (image.numeric_class == Prospero::TextureNumericClass::Sint ||
	    (image.numeric_class == Prospero::TextureNumericClass::Uint &&
	     image.conversion_format != Prospero::BufferFormat::kInvalid)) {
		return SamplerClass::PointInteger;
	}
	return image.numeric_class == Prospero::TextureNumericClass::Uint ? SamplerClass::Integer
	                                                               : SamplerClass::Float;
}

bool DescriptorIsCube(const DescriptorValue& descriptor) {
	return static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu) ==
	       Prospero::ImageType::kCube;
}

uint32_t ImageMipCount(const ImageResource& image, const DescriptorValue& descriptor) {
	if (image.mip_mode != ImageMipMode::Dynamic || NullImageDescriptor(descriptor)) {
		return 1;
	}
	const auto base = (descriptor.dwords[3] >> 12u) & 0xfu;
	const auto last = (descriptor.dwords[3] >> 16u) & 0xfu;
	return base <= last ? last - base + 1u : 0u;
}

bool DecodeBufferDescriptor(const DescriptorValue& descriptor, ShaderBufferResource& result) {
	if (descriptor.dword_count != std::size(result.fields)) {
		return false;
	}
	std::copy_n(descriptor.dwords.begin(), std::size(result.fields), result.fields);
	return true;
}

const DescriptorSource* Source(const ResourcePlan& program, uint32_t source) {
	if (source >= program.descriptor_sources.size()) {
		return nullptr;
	}
	return &program.descriptor_sources[source];
}

void MarkCleanReads(const std::unordered_set<const Inst*>& planned_reads,
                    const DescriptorSource* source, Value extra = {}) {
	if (source == nullptr && extra.IsEmpty()) {
		return;
	}
	std::vector<Value> pending;
	if (source != nullptr) {
		pending.assign(source->dwords.begin(), source->dwords.begin() + source->dword_count);
	}
	if (!extra.IsEmpty()) pending.push_back(extra);
	std::vector<const Inst*> visited;
	while (!pending.empty()) {
		auto* inst = pending.back().Resolve().TryInstruction();
		pending.pop_back();
		if (inst == nullptr || std::ranges::find(visited, inst) != visited.end()) {
			continue;
		}
		visited.push_back(inst);
		if (planned_reads.contains(inst)) {
			auto flags = inst->Flags<SrtReadFlags>();
			flags.clean = 1u;
			inst->SetFlags(flags);
		}
		for (size_t arg = 0; arg < inst->NumArgs(); arg++) {
			pending.push_back(inst->Arg(arg));
		}
	}
}

bool ReadScalarTable(uint64_t base, uint64_t size, uint64_t dynamic_offset,
                     const SrtRuntime& runtime, std::span<uint32_t> words) {
	const auto offset = dynamic_offset & ~uint64_t {3};
	const auto count = std::min<uint64_t>(words.size(), offset < size ? (size - offset) / 4u : 0u);
	std::ranges::fill(words.subspan(count), 0u);
	if (count == 0u) {
		return true;
	}
	base &= AddressMask & ~uint64_t {3};
	if (offset > AddressMask - base) {
		return false;
	}
	const auto address = base + offset;
	const auto prefix = words.first(count);
	return prefix.size_bytes() - 1u <= AddressMask - address &&
	       runtime.read_specialization_memory != nullptr &&
	       runtime.read_specialization_memory(runtime.userdata, address, prefix);
}

bool NormalizeIndirectStoreBuffer(DescriptorValue& value) {
	ShaderBufferResource descriptor;
	if (!DecodeBufferDescriptor(value, descriptor)) return false;
	if (descriptor.Type() != 0u || descriptor.Format() == Prospero::BufferFormat::kInvalid ||
	    descriptor.Base48() == 0u || descriptor.GetSize() == 0u) {
		value.dwords.fill(0u);
	} else if (descriptor.SwizzleEnabled() || descriptor.AddTid()) {
		// Each candidate is bound as its own buffer with its own stride and bounds; only the
		// lane-dependent address modes are shared across the selection switch.
		return SpecializationFail("indirect buffer stores require unswizzled addressing");
	}
	return true;
}

// Enumerates every value a GPU-computed key can take. Host-evaluable operands are evaluated
// exactly. A GPU-only operand (shared memory, a lane value) is unknown: masks bound it, and a
// scalar buffer read at an affine offset of it enumerates that buffer's in-bounds entries.
// Offsets are assumed not to wrap; an index that large addresses no guest table.
class KeySetEvaluator {
public:
	KeySetEvaluator(const ResourcePlan& program, const SrtRuntime& runtime, SrtWalker& clean)
	    : m_program(program), m_runtime(runtime), m_clean(clean) {}

	bool Run(Value value, std::span<const DescriptorSource::IndirectDescriptor::InductionRange> ranges,
	         std::vector<uint32_t>& keys) {
		for (const auto& range: ranges) {
			uint32_t begin = 0, end = 0;
			const auto* counter = range.counter.Resolve().TryInstruction();
			if (counter == nullptr || !m_clean.Evaluate(range.begin, begin) ||
			    !m_clean.Evaluate(range.end, end)) {
				return false;
			}
			std::vector<uint32_t> values;
			if (begin < end) {
				if (end - begin > MaxValues) return false;
				values.resize(end - begin);
				std::iota(values.begin(), values.end(), begin);
			}
			m_counters[counter] = {.values = std::move(values)};
		}
		auto result = Evaluate(value, 0);
		if (!result.values) return false;
		keys = std::move(*result.values);
		return true;
	}

private:
	struct Abstract {
		// Sorted, unique values when they are known.
		std::optional<std::vector<uint32_t>> values;
		// Otherwise scale * unknown + bias, while the value is affine in a single unknown.
		const Inst* unknown = nullptr;
		uint32_t    scale   = 0;
		uint32_t    bias    = 0;

		[[nodiscard]] bool Known() const { return values.has_value(); }
		[[nodiscard]] bool Affine() const { return !values && unknown != nullptr; }
		[[nodiscard]] std::optional<uint32_t> Single() const {
			return values && values->size() == 1u ? std::optional((*values)[0]) : std::nullopt;
		}
	};

	static constexpr size_t   MaxValues     = 4096;
	static constexpr size_t   MaxProducts   = size_t {1} << 16u;
	static constexpr uint32_t MaxSubsetBits = 12;
	static constexpr uint32_t MaxDepth      = 64;
	static constexpr uint64_t MaxTableBytes = 1024 * 1024;

	static Abstract Known(std::vector<uint32_t> values) {
		std::ranges::sort(values);
		values.erase(std::unique(values.begin(), values.end()), values.end());
		if (values.size() > MaxValues) return {};
		return {.values = std::move(values)};
	}

	static Abstract Affine(const Inst* unknown, uint32_t scale, uint32_t bias) {
		return {.unknown = unknown, .scale = scale, .bias = bias};
	}

	// Every value whose set bits are a subset of mask.
	static Abstract Subsets(uint32_t mask) {
		if (static_cast<uint32_t>(std::popcount(mask)) > MaxSubsetBits) return {};
		std::vector<uint32_t> values;
		for (uint32_t subset = mask;; subset = (subset - 1u) & mask) {
			values.push_back(subset);
			if (subset == 0u) break;
		}
		return Known(std::move(values));
	}

	template <typename Op>
	static Abstract Combine(const Abstract& a, const Abstract& b, Op&& op) {
		if (!a.Known() || !b.Known() || a.values->size() * b.values->size() > MaxProducts) return {};
		std::vector<uint32_t> values;
		values.reserve(a.values->size() * b.values->size());
		for (const auto x: *a.values) {
			for (const auto y: *b.values) values.push_back(op(x, y));
		}
		return Known(std::move(values));
	}

	static Abstract Union(const Abstract& a, const Abstract& b) {
		if (a.Affine() && b.Affine() && a.unknown == b.unknown && a.scale == b.scale &&
		    a.bias == b.bias) {
			return a;
		}
		if (!a.Known() || !b.Known()) return {};
		auto values = *a.values;
		values.insert(values.end(), b.values->begin(), b.values->end());
		return Known(std::move(values));
	}

	static uint32_t BitsOf(const std::vector<uint32_t>& values) {
		uint32_t bits = 0;
		for (const auto value: values) bits |= value;
		return bits;
	}

	// Reads one DWORD per address with a single guest copy of the spanned table.
	bool ReadWords(std::span<const uint64_t> addresses, std::vector<uint32_t>& values) const {
		if (addresses.empty()) return true;
		const auto [first, last] = std::ranges::minmax(addresses);
		if (last - first >= MaxTableBytes || m_runtime.read_specialization_memory == nullptr) {
			return false;
		}
		std::vector<uint32_t> table((last - first) / 4u + 1u);
		if (!m_runtime.read_specialization_memory(m_runtime.userdata, first, table)) return false;
		for (const auto address: addresses) values.push_back(table[(address - first) / 4u]);
		return true;
	}

	Abstract ReadAddress(const Inst& inst, const MemoryInfo& memory, uint32_t depth) {
		const auto* handle = inst.Arg(0).ResolveInstruction();
		uint32_t    low = 0, high = 0;
		if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetAddressResource ||
		    handle->NumArgs() != 2u || !m_clean.Evaluate(handle->Arg(0), low) ||
		    !m_clean.Evaluate(handle->Arg(1), high)) {
			return {};
		}
		const auto offsets = Evaluate(inst.Arg(1), depth + 1u);
		if (!offsets.Known()) return {};
		// SMEM aligns the base and both offsets to DWORDs independently.
		const auto base = ((uint64_t {high} << 32u) | low) & AddressMask & ~uint64_t {3};
		const auto immediate =
		    static_cast<int64_t>(static_cast<int32_t>(memory.offset)) & ~int64_t {3};
		std::vector<uint64_t> addresses;
		for (const auto offset: *offsets.values) {
			const auto relative = immediate + static_cast<int64_t>(offset & ~3u);
			if (relative < 0 && static_cast<uint64_t>(-relative) > base) return {};
			const auto address = base + static_cast<uint64_t>(relative);
			if (address > AddressMask - 3u) return {};
			addresses.push_back(address);
		}
		std::vector<uint32_t> values;
		if (!ReadWords(addresses, values)) return {};
		return Known(std::move(values));
	}

	Abstract ReadBuffer(const Inst& inst, const MemoryInfo& memory, uint32_t depth) {
		const auto*     handle = inst.Arg(0).ResolveInstruction();
		DescriptorValue value {.dword_count = 4u};
		if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetBufferResource ||
		    handle->NumArgs() != 4u || static_cast<int32_t>(memory.offset) < 0) {
			return {};
		}
		for (uint32_t word = 0; word < 4u; ++word) {
			if (!m_clean.Evaluate(handle->Arg(word), value.dwords[word])) return {};
		}
		ShaderBufferResource descriptor;
		if (!DecodeBufferDescriptor(value, descriptor)) return {};
		const auto stride = descriptor.Stride();
		const auto size   = stride == 0u ? uint64_t {descriptor.NumRecords()}
		                                 : uint64_t {stride} * descriptor.NumRecords();
		const auto base   = descriptor.Base48() & ~uint64_t {3};
		const auto immediate = uint64_t {memory.offset} & ~uint64_t {3};
		const auto offsets   = Evaluate(inst.Arg(1), depth + 1u);
		std::vector<uint64_t> addresses;
		// Scalar buffer reads outside NUM_RECORDS return zero.
		bool out_of_bounds = false;
		const auto add = [&](uint64_t byte_offset) {
			if (byte_offset + 4u > size) {
				out_of_bounds = true;
			} else {
				addresses.push_back(base + byte_offset);
			}
		};
		if (offsets.Known()) {
			for (const auto offset: *offsets.values) add(immediate + (offset & ~3u));
		} else if (offsets.Affine() && offsets.scale != 0u && offsets.scale <= INT32_MAX) {
			// Every in-bounds entry the unknown index can select, plus the zero of an OOB read.
			out_of_bounds    = true;
			const auto bias  = int64_t {static_cast<int32_t>(offsets.bias)};
			const auto scale = int64_t {offsets.scale};
			for (auto index = bias < 0 ? (scale - 1 - bias) / scale : int64_t {0};; ++index) {
				const auto offset = static_cast<uint64_t>(bias + index * scale);
				if (offset > UINT32_MAX || immediate + (offset & ~uint64_t {3}) + 4u > size) break;
				if (addresses.size() >= MaxProducts) return {};
				add(immediate + (offset & ~uint64_t {3}));
			}
		} else {
			return {};
		}
		std::vector<uint32_t> values;
		if (out_of_bounds) values.push_back(0u);
		if (!ReadWords(addresses, values)) return {};
		return Known(std::move(values));
	}

	// Whether EXEC at the observing ReadFirstLane implies this condition: it is a conjunct.
	bool ImpliedByActive(Value condition) const {
		condition = condition.Resolve();
		std::vector<Value> pending {m_active};
		for (uint32_t visited = 0; !pending.empty() && visited < 64u; ++visited) {
			const auto value = pending.back().Resolve();
			pending.pop_back();
			if (value.IsEmpty()) continue;
			if (value == condition) return true;
			const auto* inst = value.TryInstruction();
			if (inst != nullptr && inst->GetOpcode() == ValueOpcode::LogicalAnd) {
				pending.push_back(inst->Arg(0));
				pending.push_back(inst->Arg(1));
			}
		}
		return false;
	}

	// Booleans are the value sets {0}, {1} or {0, 1}.
	static Abstract Boolean(const Abstract& value) {
		return value.Known() ? value : Abstract {.values = std::vector<uint32_t> {0u, 1u}};
	}

	Abstract Evaluate(Value value, uint32_t depth) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			if (value.GetType() == Type::U32) return {.values = std::vector<uint32_t> {value.U32()}};
			if (value.GetType() == Type::U1) {
				return {.values = std::vector<uint32_t> {value.U1() ? 1u : 0u}};
			}
			return {};
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || depth > MaxDepth ||
		    (value.GetType() != Type::U32 && value.GetType() != Type::U1)) {
			return {};
		}
		if (const auto counter = m_counters.find(inst); counter != m_counters.end()) {
			return counter->second;
		}
		if (!m_active.IsEmpty() && IsSelect(*inst) && inst->Arg(0).Resolve() == m_active) {
			// ReadFirstLane observes a lane where its EXEC mask holds.
			return Evaluate(inst->Arg(1), depth + 1u);
		}
		const auto key = std::pair(inst, m_active.TryInstruction());
		if (const auto found = m_memo.find(key); found != m_memo.end()) return found->second;
		// A cycle through a loop-carried value is unbounded.
		m_memo.emplace(key, Abstract {});
		auto result = EvaluateInst(*inst, depth);
		m_memo[key] = result;
		return result;
	}

	static bool IsSelect(const Inst& inst) {
		return inst.GetOpcode() == ValueOpcode::SelectU32 && inst.NumArgs() == 3u;
	}

	Abstract EvaluateInst(const Inst& inst, uint32_t depth) {
		if (uint32_t exact = 0; m_clean.Evaluate(Value(const_cast<Inst*>(&inst)), exact)) {
			return {.values = std::vector<uint32_t> {exact}};
		}
		const auto arg = [&](uint32_t index) { return Evaluate(inst.Arg(index), depth + 1u); };
		const auto memory_index = inst.Flags<SrtReadFlags>().index;
		switch (inst.GetOpcode()) {
			case ValueOpcode::ReadFirstLane: {
				const auto previous = m_active;
				m_active            = inst.Arg(1).Resolve();
				auto result         = arg(0);
				m_active            = previous;
				return result;
			}
			case ValueOpcode::Phi: {
				Abstract result {.values = std::vector<uint32_t> {}};
				for (uint32_t index = 0; index < inst.NumArgs(); ++index) {
					result = index == 0u ? arg(index) : Union(result, arg(index));
				}
				return result;
			}
			case ValueOpcode::SelectU32: {
				if (ImpliedByActive(inst.Arg(0))) return arg(1);
				if (const auto condition = arg(0).Single()) return arg(*condition != 0u ? 1u : 2u);
				return Union(arg(1), arg(2));
			}
			case ValueOpcode::ConditionRef: return Boolean(arg(0));
			case ValueOpcode::LogicalNot:
				return Boolean(Combine(Boolean(arg(0)), {.values = std::vector<uint32_t> {1u}},
				                       [](uint32_t x, uint32_t y) { return x ^ y; }));
			case ValueOpcode::LogicalAnd:
			case ValueOpcode::LogicalOr:
			case ValueOpcode::LogicalXor: {
				const auto op = inst.GetOpcode();
				const auto a = Boolean(arg(0)), b = Boolean(arg(1));
				// One decided operand can decide the result without the other.
				const auto absorbing = op == ValueOpcode::LogicalAnd ? 0u : 1u;
				if (op != ValueOpcode::LogicalXor &&
				    (a.Single() == absorbing || b.Single() == absorbing)) {
					return {.values = std::vector<uint32_t> {absorbing}};
				}
				return Combine(a, b, [op](uint32_t x, uint32_t y) {
					return op == ValueOpcode::LogicalAnd ? x & y
					       : op == ValueOpcode::LogicalOr ? x | y
					                                      : x ^ y;
				});
			}
			case ValueOpcode::IEqual32:
			case ValueOpcode::INotEqual32:
			case ValueOpcode::ULessThan32:
			case ValueOpcode::ULessThanEqual32:
			case ValueOpcode::UGreaterThan32:
			case ValueOpcode::UGreaterThanEqual32:
			case ValueOpcode::SLessThan32:
			case ValueOpcode::SLessThanEqual32:
			case ValueOpcode::SGreaterThan32:
			case ValueOpcode::SGreaterThanEqual32: {
				const auto op = inst.GetOpcode();
				return Boolean(Combine(arg(0), arg(1), [op](uint32_t x, uint32_t y) -> uint32_t {
					const auto sx = std::bit_cast<int32_t>(x), sy = std::bit_cast<int32_t>(y);
					switch (op) {
						case ValueOpcode::IEqual32: return x == y;
						case ValueOpcode::INotEqual32: return x != y;
						case ValueOpcode::ULessThan32: return x < y;
						case ValueOpcode::ULessThanEqual32: return x <= y;
						case ValueOpcode::UGreaterThan32: return x > y;
						case ValueOpcode::UGreaterThanEqual32: return x >= y;
						case ValueOpcode::SLessThan32: return sx < sy;
						case ValueOpcode::SLessThanEqual32: return sx <= sy;
						case ValueOpcode::SGreaterThan32: return sx > sy;
						default: return sx >= sy;
					}
				}));
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer: {
				if (memory_index >= m_program.memory_info.size()) return {};
				const auto& memory = m_program.memory_info[memory_index];
				auto result = inst.GetOpcode() == ValueOpcode::LoadAddressU32 &&
				                      memory.kind == ResourceKind::ScalarAddress
				                  ? ReadAddress(inst, memory, depth)
				              : inst.GetOpcode() == ValueOpcode::ReadConstBuffer &&
				                      memory.kind == ResourceKind::ScalarBuffer
				                  ? ReadBuffer(inst, memory, depth)
				                  : Abstract {};
				return result.Known() ? result : Affine(&inst, 1u, 0u);
			}
			case ValueOpcode::BitwiseAnd32: {
				const auto a = arg(0), b = arg(1);
				// Unknown bits can only survive where the other operand may be set.
				if (a.Known() && !b.Known()) return Subsets(BitsOf(*a.values));
				if (b.Known() && !a.Known()) return Subsets(BitsOf(*b.values));
				return Combine(a, b, [](uint32_t x, uint32_t y) { return x & y; });
			}
			case ValueOpcode::ShiftRightLogical32: {
				const auto a = arg(0), b = arg(1);
				if (!a.Known() && b.Known()) {
					uint32_t shift = 31;
					for (const auto value: *b.values) shift = std::min(shift, value & 31u);
					return Subsets(UINT32_MAX >> shift);
				}
				return Combine(a, b, [](uint32_t x, uint32_t y) { return x >> (y & 31u); });
			}
			case ValueOpcode::BitFieldUExtract: {
				const auto offset = arg(1);
				const auto width  = arg(2).Single();
				if (!offset.Known() || !width || *width > 32u) return {};
				const auto mask   = *width == 32u ? UINT32_MAX : (uint32_t {1} << *width) - 1u;
				const auto source = arg(0);
				if (!source.Known()) return Subsets(mask);
				return Combine(source, offset, [mask](uint32_t x, uint32_t y) {
					return y >= 32u ? 0u : (x >> y) & mask;
				});
			}
			case ValueOpcode::UMin32: {
				const auto a = arg(0), b = arg(1);
				if (a.Known() != b.Known()) {
					const auto& bound = *(a.Known() ? a : b).values;
					if (bound.empty() || bound.back() >= MaxValues) return {};
					std::vector<uint32_t> values(bound.back() + 1u);
					std::iota(values.begin(), values.end(), 0u);
					return {.values = std::move(values)};
				}
				return Combine(a, b, [](uint32_t x, uint32_t y) { return std::min(x, y); });
			}
			case ValueOpcode::IAdd32: {
				const auto a = arg(0), b = arg(1);
				const auto ca = a.Single(), cb = b.Single();
				if (a.Affine() && cb) return Affine(a.unknown, a.scale, a.bias + *cb);
				if (b.Affine() && ca) return Affine(b.unknown, b.scale, b.bias + *ca);
				if (a.Affine() && b.Affine() && a.unknown == b.unknown) {
					return Affine(a.unknown, a.scale + b.scale, a.bias + b.bias);
				}
				return Combine(a, b, [](uint32_t x, uint32_t y) { return x + y; });
			}
			case ValueOpcode::ISub32: {
				const auto a = arg(0), b = arg(1);
				if (const auto cb = b.Single(); a.Affine() && cb) {
					return Affine(a.unknown, a.scale, a.bias - *cb);
				}
				return Combine(a, b, [](uint32_t x, uint32_t y) { return x - y; });
			}
			case ValueOpcode::IMul32: {
				const auto a = arg(0), b = arg(1);
				const auto ca = a.Single(), cb = b.Single();
				if (a.Affine() && cb) return Affine(a.unknown, a.scale * *cb, a.bias * *cb);
				if (b.Affine() && ca) return Affine(b.unknown, b.scale * *ca, b.bias * *ca);
				return Combine(a, b, [](uint32_t x, uint32_t y) { return x * y; });
			}
			case ValueOpcode::ShiftLeftLogical32: {
				const auto a = arg(0), b = arg(1);
				if (const auto cb = b.Single(); a.Affine() && cb) {
					const auto shift = *cb & 31u;
					return Affine(a.unknown, a.scale << shift, a.bias << shift);
				}
				return Combine(a, b, [](uint32_t x, uint32_t y) { return x << (y & 31u); });
			}
			case ValueOpcode::FindUMsb32:
			case ValueOpcode::FindUMsb64:
			case ValueOpcode::FindILsb32:
			case ValueOpcode::BitCount32:
			case ValueOpcode::BitCount64: {
				// A bit position, or the all-ones result of a scan that finds no bit.
				const auto op   = inst.GetOpcode();
				const auto bits = op == ValueOpcode::FindUMsb64 || op == ValueOpcode::BitCount64 ? 64u : 32u;
				const bool count = op == ValueOpcode::BitCount32 || op == ValueOpcode::BitCount64;
				std::vector<uint32_t> values(count ? bits + 1u : bits);
				std::iota(values.begin(), values.end(), 0u);
				if (!count) values.push_back(UINT32_MAX);
				return {.values = std::move(values)};
			}
			case ValueOpcode::BitwiseOr32:
				return Combine(arg(0), arg(1), [](uint32_t x, uint32_t y) { return x | y; });
			case ValueOpcode::BitwiseXor32:
				return Combine(arg(0), arg(1), [](uint32_t x, uint32_t y) { return x ^ y; });
			default:
				// A GPU-only value: the unknown that affine offsets are expressed in.
				if (inst.GetType() != Type::U32) return {};
				return Affine(&inst, 1u, 0u);
		}
	}

	const ResourcePlan& m_program;
	const SrtRuntime&   m_runtime;
	SrtWalker&          m_clean;
	Value               m_active;
	std::unordered_map<const Inst*, Abstract>               m_counters;
	std::map<std::pair<const Inst*, const Inst*>, Abstract> m_memo;
};

bool IsBoundedDescriptorTable(const ResourcePlan& program,
                              const DescriptorSource::IndirectDescriptor& indirect) {
	const auto* table = Source(program, indirect.table_source);
	return indirect.sources.empty() && !indirect.selector && indirect.selector_value.IsEmpty() &&
	       indirect.workgroup_axis == UINT32_MAX && table != nullptr && table->dword_count == 4u;
}

// Candidates read from a descriptor table can include entries the GPU never selects (unused
// slots, other operations' resources): a bounded table offers every DWORD offset, and a key the
// GPU computes is enumerated as a superset of its values. Keys read from a selector, counted keys,
// workgroup keys and finite descriptor choices are all selected.
bool IsSpeculativeImageTable(const ResourcePlan& program, const DescriptorSource* source) {
	if (source == nullptr || !source->indirect_descriptor) return false;
	const auto& indirect = *source->indirect_descriptor;
	return indirect.sources.empty() &&
	       (!indirect.selector_value.IsEmpty() || IsBoundedDescriptorTable(program, indirect));
}

// The candidates of a speculative table follow whatever guest memory holds from dispatch to
// dispatch, so the table is laid out canonically and the compiled shader does not follow them. A
// table the shader writes through keeps an exact layout: a store target is never padded or
// selected through a root.
bool HasStableImageLayout(const ResourcePlan& program, uint32_t root) {
	if (root >= program.info.images.size()) return false;
	const auto& image = program.info.images[root];
	return IsSpeculativeImageTable(program, Source(program, image.source)) && !image.written &&
	       !image.atomic;
}

template <typename Specialization, typename Normalize>
bool MaterializeIndirectDescriptor(const ResourcePlan&                         program,
                                   const DescriptorSource::IndirectDescriptor& indirect,
                                   uint32_t resource_index, uint32_t dword_count,
                                   const SrtRuntime& runtime, SrtWalker& clean,
                                   ResourceSnapshot&             snapshot,
                                   std::vector<DescriptorValue>& descriptors,
                                   std::vector<Specialization>&  specializations,
                                   uint32_t maximum_resources, uint32_t padded_iterations,
                                   Normalize&& normalize) {
	const auto  descriptor_bytes = dword_count * sizeof(uint32_t);
	const auto& sources = indirect.sources;
	const auto* selector = indirect.selector ? &*indirect.selector : nullptr;
	auto& keys = program.material_keys;
	const auto children_begin = descriptors.size();
	const auto root_resource = specializations[resource_index];
	const auto fail = [&](std::string_view reason) {
		return SpecializationFail(fmt::format("shader 0x{:016x} resource {} descriptor table: {}",
		                                      program.shader_hash, resource_index, reason));
	};
	const auto intern_candidate = [&](const DescriptorValue& candidate, uint32_t& ordinal) {
		ordinal = 0u;
		if (candidate == descriptors[resource_index]) return true;
		const auto found = std::find(descriptors.begin() + children_begin, descriptors.end(), candidate);
		ordinal = static_cast<uint32_t>(found - descriptors.begin() - children_begin + 1u);
		if (found == descriptors.end()) {
			if (descriptors.size() >= maximum_resources) return false;
			descriptors.push_back(candidate);
			auto child = root_resource;
			child.indirect_root = resource_index;
			specializations.push_back(child);
		}
		return true;
	};
	const auto read_keys = [&](const ShaderBufferResource& material, uint64_t first,
	                           uint64_t step, uint64_t count) {
		keys.resize(count);
		if (step == 4u) {
			return ReadScalarTable(material.Base48(), material.GetSize(), first, runtime, keys);
		}
		for (auto& key: keys) {
			if (!ReadScalarTable(material.Base48(), material.GetSize(), first, runtime, {&key, 1}))
				return false;
			first += step;
		}
		return true;
	};
	uint64_t table_base = 0;
	uint64_t table_size = UINT64_MAX; // Scalar addresses have no buffer descriptor bounds.
	if (sources.empty()) {
		keys.clear();
		DescriptorValue material_value;
		DescriptorValue table_value;
		if (selector != nullptr && !clean.EvaluateDescriptor(selector->source, material_value)) {
			return fail("its selector descriptor could not be read");
		}
		if (!clean.EvaluateDescriptor(indirect.table_source, table_value)) {
			return fail("its table descriptor could not be read");
		}
		ShaderBufferResource table;
		if (table_value.dword_count == 2u) {
			table_base = (static_cast<uint64_t>(table_value.dwords[1]) << 32u) | table_value.dwords[0];
		} else if (DecodeBufferDescriptor(table_value, table)) {
			table_base = table.Base48();
			table_size = table.GetSize();
		} else {
			return fail("its table descriptor has an invalid width");
		}
		if (IsBoundedDescriptorTable(program, indirect)) {
			if (!indirect.table_scalar && table.Format() == Prospero::BufferFormat::kInvalid) {
				descriptors[resource_index] = {.dword_count = dword_count};
				return true;
			}
			// A V# read on a path the GPU does not take can be any data. One whose table is not
			// mapped memory cannot be what the GPU uses: like a stale one, it provides no
			// descriptor.
			if (runtime.is_mapped != nullptr && !runtime.is_mapped(table_base, sizeof(uint32_t))) {
				descriptors[resource_index] = {.dword_count = dword_count};
				return true;
			}
			if (table_size > UINT32_MAX ||
			    (indirect.table_record_bytes != 0u &&
			     (table.Type() != 0u || table.SwizzleEnabled() || table.AddTid() ||
			      table.OutOfBounds() != 0u || (table_base & 3u) != 0u ||
			      (table.Stride() & 3u) != 0u || table.Stride() < indirect.table_record_bytes))) {
				return SpecializationFail(fmt::format(
				    "shader 0x{:016x}: unsupported descriptor table V# {:08x},{:08x},{:08x},{:08x}",
				    program.shader_hash, table_value.dwords[0], table_value.dwords[1],
				    table_value.dwords[2], table_value.dwords[3]));
			}
			// Descriptor bytes are refreshed together. The GPU selects the DWORD address;
			// no material selector or unused table field is interpreted on the CPU.
			keys.resize(table_size / 4u);
			if (!ReadScalarTable(table_base, table_size, 0u, runtime, keys)) {
				return SpecializationFail(fmt::format(
				    "shader 0x{:016x}: cannot read descriptor table at 0x{:x}, size 0x{:x}",
				    program.shader_hash, table_base, table_size));
			}
			const auto mapping_offset = snapshot.flattened_srt.size();
			descriptors[resource_index] = {.dword_count = dword_count};
			snapshot.flattened_srt.resize(mapping_offset + 2u, 0u);
			for (uint32_t word = 0; word < keys.size(); ++word) {
				DescriptorValue candidate {.dword_count = dword_count};
				std::copy_n(keys.begin() + word, std::min<size_t>(dword_count, keys.size() - word),
				            candidate.dwords.begin());
				if (!normalize(candidate)) return fail("a bounded-table candidate is invalid");
				uint32_t ordinal;
				if (!intern_candidate(candidate, ordinal)) {
					return fail("a bounded table has more candidates than bindings");
				}
				if (ordinal == 0u) continue;
				snapshot.flattened_srt.resize(mapping_offset + 2u + word, 0u);
				snapshot.flattened_srt[mapping_offset + 1u + word] = ordinal;
			}
			const bool selects_nothing = descriptors.size() == children_begin;
			if (selects_nothing && padded_iterations == 0u) {
				snapshot.flattened_srt.resize(mapping_offset);
			} else {
				snapshot.flattened_srt[mapping_offset] =
				    selects_nothing ? 0u
				                    : static_cast<uint32_t>(snapshot.flattened_srt.size() - mapping_offset - 1u);
				if (padded_iterations != 0u) {
					// A stable layout keeps the whole table's region, also when it selects nothing,
					// so neither the root nor the offsets of later mappings follow its contents.
					snapshot.flattened_srt.resize(
					    std::max<size_t>(snapshot.flattened_srt.size(), mapping_offset + 1u + keys.size()),
					    0u);
				}
				auto& root = specializations[resource_index];
				root.indirect_root = resource_index;
				root.indirect_mapping_offset = static_cast<uint32_t>(mapping_offset);
				root.indirect_search_iterations = 0u;
			}
			return true;
		}
		if (!indirect.selector_value.IsEmpty()) {
			// The GPU computes the key; its value set is enumerated from the guest memory it reads.
			// A key derived from data the GPU writes earlier in the frame has no such value set.
			// Sampled entries are validated on the host, so an image table is then scanned from its
			// start instead; unused and foreign entries become null candidates. Only tables behind a
			// raw pointer take GPU-computed image keys (a table with a V# is planned as a bounded
			// table), so nothing bounds the scan but a fixed prefix.
			constexpr uint32_t ScannedImageKeys = 1024;
			if (indirect.table_stride == 0u) return fail("a GPU-selected table has no stride");
			if (!KeySetEvaluator(program, runtime, clean)
			         .Run(indirect.selector_value, indirect.selector_ranges, keys)) {
				if (dword_count != 8u) {
					return SpecializationFail(fmt::format(
					    "shader 0x{:016x} resource {}: GPU-selected descriptor key has no bounded "
					    "value set",
					    program.shader_hash, resource_index));
				}
				keys.resize(ScannedImageKeys);
				std::iota(keys.begin(), keys.end(), 0u);
			}
		} else if (selector == nullptr) {
			uint32_t key_count = 0;
			if (indirect.workgroup_axis != UINT32_MAX) {
				if (indirect.workgroup_axis >= runtime.workgroup_counts.size()) {
					return fail("its workgroup axis is out of range");
				}
				key_count = runtime.workgroup_counts[indirect.workgroup_axis];
				if (key_count == 0u) return fail("the dispatch has no workgroups on its key axis");
			} else {
				if (table_value.dword_count != 2u || !clean.Evaluate(indirect.key_count, key_count))
					return fail("its key count could not be read");
				if (std::bit_cast<int32_t>(key_count) <= 0) key_count = 0;
			}
			if (indirect.table_stride == 0u || key_count > MaxIndirectDescriptorProbes ||
			    (key_count != 0u && uint64_t {indirect.table_offset} + indirect.table_immediate +
			         uint64_t {key_count - 1u} * indirect.table_stride + descriptor_bytes >
			             UINT32_MAX + 1ull)) {
				return fail(fmt::format("{} keys exceed the table or the probe limit", key_count));
			}
			keys.resize(key_count);
			std::iota(keys.begin(), keys.end(), 0u);
		} else if (!indirect.selector_first.IsEmpty()) {
			ShaderBufferResource material;
			uint32_t             first = 0, count = 0;
			if (!DecodeBufferDescriptor(material_value, material) ||
			    !clean.Evaluate(indirect.selector_first, first) ||
			    !clean.Evaluate(indirect.key_count, count) || count > MaxIndirectDescriptorProbes)
				return fail("its selector range could not be read");
			// SMEM uses its instruction's byte stride; V# stride only supplies its bounds.
			const auto step = selector->stride != 0u ? selector->stride : material.Stride();
			if (selector->stride == 0u &&
			    (material.Type() != 0u || material.SwizzleEnabled() || material.AddTid() ||
			     material.OutOfBounds() != 0u || uint64_t {selector->offset} + 4u > step ||
			     uint64_t {first} + count > material.NumRecords()))
				return fail("its selector buffer is unsupported or too small");
			const auto end = count != 0u
			    ? (uint64_t {first} + count - 1u) * step + selector->offset + 4u : 0u;
			if (end > uint64_t {UINT32_MAX} + 1u || end > material.GetSize())
				return fail("its selector range exceeds the selector buffer");
			if (!read_keys(material, uint64_t {first} * step + selector->offset, step, count))
				return fail("its selector keys could not be read");
		} else if (!indirect.selector_mask.IsEmpty()) {
			uint32_t mask = 0;
			uint32_t count = 0;
			if (material_value.dword_count != 2u || table_value.dword_count != 2u ||
			    !clean.Evaluate(indirect.selector_mask, mask) ||
			    !clean.Evaluate(indirect.key_count, count) || count == 0u || count > 32u) {
				return fail("its selector mask could not be read");
			}
			if (count < 32u) mask &= (1u << count) - 1u;
			const auto material_base =
			    (static_cast<uint64_t>(material_value.dwords[1]) << 32u) | material_value.dwords[0];
			keys.reserve(std::popcount(mask));
			while (mask != 0u) {
				const auto index = std::countr_zero(mask);
				const auto offset = static_cast<uint64_t>(selector->offset) +
				                    static_cast<uint64_t>(index) * selector->stride;
				if (offset > UINT32_MAX) return fail("a masked selector offset is out of range");
				uint32_t key = 0;
				if (!ReadScalarTable(material_base, UINT64_MAX, static_cast<uint32_t>(offset),
				                     runtime, {&key, 1}))
					return fail("a masked selector key could not be read");
				keys.push_back(key);
				mask &= mask - 1u;
			}
		} else return fail("its selector form is unsupported");
		if (selector != nullptr) {
			std::ranges::sort(keys);
			keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
		}
	}

	const auto mapping_offset = snapshot.flattened_srt.size();
	const auto key_count = sources.empty() ? keys.size() : sources.size();
	snapshot.flattened_srt.resize(mapping_offset + 1u + key_count * 2u);
	snapshot.flattened_srt[mapping_offset] = static_cast<uint32_t>(key_count);
	if (key_count == 0u) {
		// No key selects anything; the root the shader falls back to must be null.
		descriptors[resource_index] = {.dword_count = dword_count};
	}
	for (uint32_t entry = 0; entry < key_count; ++entry) {
		const auto key = sources.empty() ? keys[entry] : entry;
		DescriptorValue candidate;
		candidate.dword_count = dword_count;
		if (sources.empty()) {
			const uint64_t table_offset =
			    uint64_t {(key * indirect.table_stride + indirect.table_offset) & ~3u} +
			    (indirect.table_immediate & ~3u);
			if (!ReadScalarTable(table_base, table_size, table_offset, runtime,
			                     std::span(candidate.dwords).first(dword_count))) {
				// An enumerated key set is a superset; a key that addresses no guest memory
				// cannot be the one the GPU selects with a valid table.
				if (indirect.selector_value.IsEmpty()) {
					return fail(fmt::format("key {} addresses unreadable table memory", key));
				}
				candidate.dwords.fill(0u);
			}
		} else if (!clean.EvaluateDescriptor(sources[entry], candidate)) {
			return fail(fmt::format("candidate {} could not be read", entry));
		}
		if (!normalize(candidate)) return fail(fmt::format("candidate {} is invalid", entry));
		uint32_t ordinal = 0;
		if (entry == 0) {
			descriptors[resource_index] = candidate;
		} else if (!intern_candidate(candidate, ordinal)) {
			return fail("the table has more candidates than bindings");
		}
		snapshot.flattened_srt[mapping_offset + 1u + entry * 2u] = key;
		snapshot.flattened_srt[mapping_offset + 2u + entry * 2u] = ordinal;
	}
	if (descriptors.size() == children_begin && padded_iterations == 0u) {
		snapshot.flattened_srt.resize(mapping_offset);
	} else {
		auto& root                      = specializations[resource_index];
		root.indirect_root              = resource_index;
		root.indirect_mapping_offset = static_cast<uint32_t>(mapping_offset);
		root.indirect_search_iterations = std::bit_width(key_count);
		if (padded_iterations != 0u) {
			// A speculative key set changes with guest data. Its mapping keeps a capacity that
			// does not follow it, so neither its search nor the offsets after it change per dispatch.
			// Each growth is a new shader, so the capacity grows four times at a time.
			const uint32_t needed = (root.indirect_search_iterations + 1u) & ~1u;
			root.indirect_search_iterations = std::max<uint32_t>(needed, padded_iterations);
			const auto capacity = (size_t {1} << root.indirect_search_iterations) - 1u;
			snapshot.flattened_srt.resize(mapping_offset + 1u + capacity * 2u, 0u);
		}
	}
	return true;
}

} // namespace

struct SamplerPlan {
	struct Binding {
		uint32_t     source;
		SamplerClass type;
	};
	std::array<std::array<uint32_t, 3>, ShaderInfo::MaxSamplers> mapping;
	std::array<Binding, ShaderInfo::MaxSamplers>                bindings;
	uint32_t                                                  sampler_count = 0;
};

template <typename T, typename Keep>
void CompactImages(std::vector<T>& images, Keep&& keep) {
	size_t count = 0;
	for (size_t index = 0; index < images.size(); ++index) {
		if (!keep(index)) continue;
		if (count != index) images[count] = std::move(images[index]);
		++count;
	}
	images.resize(count);
}

struct ImageRemap {
	explicit ImageRemap(const ResourceSpecialization& specialization)
	    : indices(specialization.images.size()),
	      source_count(static_cast<uint32_t>(specialization.images.size())) {
		for (uint32_t index = 0; index < source_count; index++) {
			indices[index] = specialization.images[index].fmask ? UINT32_MAX : count++;
		}
	}

	uint32_t operator[](uint32_t index) const {
		EXIT_IF(index >= source_count);
		return indices[index];
	}

	template <typename T>
	void Apply(std::vector<T>& images) const {
		EXIT_IF(images.size() != source_count);
		if (count == source_count) {
			return;
		}
		CompactImages(images, [&](size_t index) { return indices[index] != UINT32_MAX; });
	}

private:
	std::vector<uint32_t> indices;
	uint32_t                                    source_count;
	uint32_t                                    count = 0;
};

// What the candidates one image operation selects must share. 2D and 2D-array images share
// coordinates.
struct ImageSampleClass {
	Prospero::TextureNumericClass numeric_class;
	uint32_t                      mip_count;
	Prospero::BufferFormat        conversion_format;
	uint32_t                      shader_swizzle;
	Decoder::ImageDimension       coordinates;
	bool                          cube;

	auto operator<=>(const ImageSampleClass&) const = default;
};

static ImageSampleClass SampleClassOf(const ResourceSpecialization::Image& image) {
	const bool planar = image.dimension == Decoder::ImageDimension::Dim2D ||
	                    image.dimension == Decoder::ImageDimension::Dim2DArray;
	return {image.numeric_class,     image.mip_count,
	        image.conversion_format, image.shader_swizzle,
	        planar ? Decoder::ImageDimension::Dim2D : image.dimension, !planar && image.cube};
}

// Derives an image's specialization from its descriptor, or returns why it cannot be bound.
static std::optional<std::string> ClassifyImage(const ResourcePlan& program, uint32_t i,
                                                const ImageResource&      base,
                                                const DescriptorValue&    descriptor,
                                                ResourceSpecialization::Image& image) {
	image.mip_count = ImageMipCount(base, descriptor);
	if (image.mip_count == 0u) {
		return fmt::format("image descriptor {} has an invalid mip range", i);
	}
	if (NullImageDescriptor(descriptor)) {
		image.numeric_class = base.atomic ? Prospero::TextureNumericClass::Uint
		                                  : Prospero::TextureNumericClass::Float;
		image.dimension     = Decoder::ImageDimension::Dim2D;
		image.cube          = false;
		return std::nullopt;
	}
	const auto descriptor_dimension = DescriptorDimension(descriptor, base.dimension);
	if (descriptor_dimension == Decoder::ImageDimension::Unknown) {
		return fmt::format(
		    "image descriptor {} has unsupported type {}: {:08x},{:08x},{:08x},{:08x},"
		    "{:08x},{:08x},{:08x},{:08x}",
		    i, (descriptor.dwords[3] >> 28u) & 0xfu, descriptor.dwords[0], descriptor.dwords[1],
		    descriptor.dwords[2], descriptor.dwords[3], descriptor.dwords[4], descriptor.dwords[5],
		    descriptor.dwords[6], descriptor.dwords[7]);
	}
	image.dimension = descriptor_dimension;
	image.cube      = DescriptorIsCube(descriptor);
	const auto format =
	    static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
	if (base.atomic &&
	    (base.atomic64 ? format != Prospero::BufferFormat::k32_32UInt
	                   : format != Prospero::BufferFormat::k32UInt &&
	                         format != Prospero::BufferFormat::k32Float)) {
		return fmt::format("atomic image descriptor {} uses unsupported format {}", i,
		                   static_cast<uint32_t>(format));
	}
	const bool storage = base.resource_class == ImageResourceClass::Storage;
	image.fmask        = Prospero::IsFmaskTextureFormat(format);
	if (image.fmask) {
		if (storage || base.depth_compare || image.indirect_root != ImageResource::NoIndirectImage ||
		    std::ranges::any_of(program.info.sampled_pairs,
		                        [&](const auto& pair) { return pair.image == i; })) {
			return std::string("FMASK requires a direct image load");
		}
	}
	image.conversion_format = ImageConversionFormat(format);
	if (storage || image.conversion_format != Prospero::BufferFormat::kInvalid) {
		image.shader_swizzle = DescriptorImageSwizzle(descriptor);
	}
	const bool raw_sint_storage = storage && format == Prospero::BufferFormat::k32SInt &&
	                              base.written && !base.read && !base.atomic;
	image.numeric_class         = Prospero::SampledTextureNumericClass(format);
	if (storage) {
		if ((!raw_sint_storage && image.numeric_class == Prospero::TextureNumericClass::Sint) ||
		    image.numeric_class == Prospero::TextureNumericClass::Unsupported) {
			return fmt::format("storage image descriptor {} uses unsupported format {}", i,
			                   static_cast<uint32_t>(format));
		}
		if (raw_sint_storage || base.atomic) {
			image.numeric_class = Prospero::TextureNumericClass::Uint;
		}
	} else if (image.numeric_class == Prospero::TextureNumericClass::Unsupported ||
	           (base.depth_compare && image.numeric_class != Prospero::TextureNumericClass::Float)) {
		return fmt::format("sampled image descriptor {} uses unsupported format {}", i,
		                   static_cast<uint32_t>(format));
	}
	return std::nullopt;
}

// Speculative candidates follow table contents that change from dispatch to dispatch; the compiled
// shader must not. Lays them out canonically: a null root, then one run per coordinate kind in a
// fixed order, each padded with null entries to a power of four that does not shrink below the
// previous layout. Rewrites the key mapping to the new ordinals.
static void LayOutSpeculativeImages(const ResourcePlan&                                program,
                                    std::span<const ResourceSpecialization::Image> previous,
                                    ResourceSnapshot&                                  snapshot,
                                    ResourceSpecialization& specialization) {
	using Image = ResourceSpecialization::Image;
	using Kind  = std::pair<Decoder::ImageDimension, bool>;
	const auto base_count = static_cast<uint32_t>(program.info.images.size());
	// Other tables keep their candidates where materialization put them, in their own storage.
	bool has_stable = false;
	for (uint32_t root_index = 0; root_index < base_count; root_index++) {
		has_stable |= specialization.images[root_index].indirect_root == root_index &&
		              HasStableImageLayout(program, root_index);
	}
	if (!has_stable) {
		return;
	}
	std::vector<DescriptorValue> descriptors(snapshot.images.begin(),
	                                         snapshot.images.begin() + base_count);
	std::vector<Image> images(specialization.images.begin(),
	                          specialization.images.begin() + base_count);
	for (uint32_t root_index = 0; root_index < base_count; root_index++) {
		std::vector<uint32_t> members {root_index};
		for (uint32_t index = base_count; index < specialization.images.size(); index++) {
			if (specialization.images[index].indirect_root == root_index) members.push_back(index);
		}
		const auto& plan = program.info.images[root_index];
		if (images[root_index].indirect_root != root_index ||
		    !HasStableImageLayout(program, root_index)) {
			for (size_t member = 1; member < members.size(); member++) {
				descriptors.push_back(snapshot.images[members[member]]);
				images.push_back(specialization.images[members[member]]);
			}
			continue;
		}
		std::map<Kind, std::vector<uint32_t>> runs; // Ordinals of each kind's candidates.
		for (uint32_t ordinal = 0; ordinal < members.size(); ordinal++) {
			const auto& image = specialization.images[members[ordinal]];
			if (!NullImageDescriptor(snapshot.images[members[ordinal]])) {
				runs[{image.dimension, image.cube}].push_back(ordinal);
			}
		}
		const bool had_layout =
		    root_index < previous.size() && previous[root_index].indirect_root == root_index;
		// The remaining candidates share one class. Without any, the previous layout stands.
		const auto exemplar = !runs.empty()
		                          ? specialization.images[members[runs.begin()->second.front()]]
		                      : had_layout ? previous[root_index]
		                                   : specialization.images[root_index];
		// Slots only grow, also across a change of class: they are null padding. Each growth is a new
		// shader, so a run grows by powers of four (1, 4, 16, 64, ...).
		std::map<Kind, uint32_t> capacity;
		if (had_layout) {
			for (uint32_t index = base_count; index < previous.size(); index++) {
				if (previous[index].indirect_root == root_index) {
					capacity[{previous[index].dimension, previous[index].cube}]++;
				}
			}
		}
		for (const auto& [kind, ordinals]: runs) {
			auto& slots = capacity[kind];
			uint32_t bucket = 1u;
			while (bucket < ordinals.size()) bucket *= 4u;
			slots = std::max(slots, bucket);
		}
		if (capacity.empty()) {
			capacity[{exemplar.dimension, exemplar.cube}] = 1u;
		}
		auto& root                   = images[root_index];
		root.numeric_class           = exemplar.numeric_class;
		root.mip_count               = exemplar.mip_count;
		root.conversion_format       = exemplar.conversion_format;
		root.shader_swizzle          = exemplar.shader_swizzle;
		root.fmask                   = false;
		root.speculative             = IsSpeculativeImageTable(program, Source(program, plan.source));
		root.dimension               = capacity.begin()->first.first;
		root.cube                    = capacity.begin()->first.second;
		descriptors[root_index]      = {.dword_count = 8u};
		// Appending the children reallocates images; keep the root by value.
		const auto layout = root;
		std::vector<uint32_t> remap(members.size(), 0u); // Null and dropped entries select the root.
		uint32_t              ordinal = 1;
		for (const auto& [kind, slots]: capacity) {
			const auto run = runs.find(kind);
			for (uint32_t slot = 0; slot < slots; slot++, ordinal++) {
				auto child                       = layout;
				child.dimension                  = kind.first;
				child.cube                       = kind.second;
				child.indirect_mapping_offset    = plan.indirect_mapping_offset;
				child.indirect_search_iterations = plan.indirect_search_iterations;
				if (run != runs.end() && slot < run->second.size()) {
					remap[run->second[slot]] = ordinal;
					descriptors.push_back(snapshot.images[members[run->second[slot]]]);
				} else {
					descriptors.push_back({.dword_count = 8u});
				}
				images.push_back(child);
			}
		}
		auto       mapping = std::span(snapshot.flattened_srt).subspan(layout.indirect_mapping_offset);
		const bool direct  = layout.indirect_search_iterations == 0u;
		for (uint32_t entry = 0; entry < mapping[0]; entry++) {
			auto& selected = mapping[direct ? 1u + entry : 2u + entry * 2u];
			selected       = selected < remap.size() ? remap[selected] : 0u;
		}
	}
	snapshot.images       = std::move(descriptors);
	specialization.images = std::move(images);
}

static bool BuildResourceSpecialization(const ResourcePlan& program,
                                        std::span<const ResourceSpecialization::Image> previous,
                                        ResourceSnapshot&       snapshot,
                                        ResourceSpecialization& specialization) {
	for (uint32_t i = 0; i < specialization.images.size(); i++) {
		auto&      descriptor = snapshot.images[i];
		auto&      image      = specialization.images[i];
		const auto base_index = i < program.info.images.size() ? i : image.indirect_root;
		if (base_index >= program.info.images.size()) {
			return SpecializationFail(fmt::format("image resource {} has an invalid root", i));
		}
		const auto& base = program.info.images[base_index];
		if (base.resource_class == ImageResourceClass::None ||
		    (base.atomic && base.resource_class != ImageResourceClass::Storage)) {
			return SpecializationFail(fmt::format("image resource {} has an invalid class", i));
		}
		const auto initial = image;
		auto       error   = ClassifyImage(program, i, base, descriptor, image);
		if (error && IsSpeculativeImageTable(program, Source(program, base.source))) {
			// The GPU never selects a table entry this operation cannot use.
			descriptor.dwords.fill(0u);
			image = initial;
			error = ClassifyImage(program, i, base, descriptor, image);
		}
		if (error) {
			return SpecializationFail(*error);
		}
	}
	for (uint32_t root_index = 0; root_index < specialization.images.size(); root_index++) {
		auto& root = specialization.images[root_index];
		if (root.indirect_root != root_index) {
			continue;
		}
		const auto key_count = root.indirect_mapping_offset < snapshot.flattened_srt.size()
		                           ? snapshot.flattened_srt[root.indirect_mapping_offset]
		                           : 0u;
		const auto* source    = Source(program, program.info.images[root_index].source);
		const bool speculative = IsSpeculativeImageTable(program, source);
		const bool stable      = HasStableImageLayout(program, root_index);
		// A stable table keeps its padded mapping when the table selects nothing.
		if (root.indirect_mapping_offset >= snapshot.flattened_srt.size() ||
		    (key_count == 0u && !stable) ||
		    static_cast<size_t>(root.indirect_mapping_offset) + 1u +
		            static_cast<size_t>(key_count) * (root.indirect_search_iterations == 0u ? 1u : 2u) >
		        snapshot.flattened_srt.size()) {
			return SpecializationFail("indirect image specialization has an invalid key mapping");
		}
		const auto compatible = [](const auto& a, const auto& b) {
			return SampleClassOf(a) == SampleClassOf(b);
		};
		const auto candidates = [&](auto&& visit) {
			for (uint32_t resource = 0; resource < specialization.images.size(); resource++) {
				if (specialization.images[resource].indirect_root == root_index &&
				    !NullImageDescriptor(snapshot.images[resource])) {
					visit(resource);
				}
			}
		};
		if (speculative) {
			// Table entries over-approximate the descriptors the GPU selects. Keep one class of
			// entries one operation can sample alike and null the others: the previous dispatch's
			// class while it still has entries, so the shader does not alternate, else the largest.
			std::map<ImageSampleClass, uint32_t> classes;
			candidates([&](uint32_t resource) {
				classes[SampleClassOf(specialization.images[resource])]++;
			});
			if (!classes.empty()) {
				auto chosen = std::ranges::max_element(classes, {}, [](const auto& entry) {
					              return entry.second;
				              })->first;
				if (root_index < previous.size() && previous[root_index].indirect_root == root_index &&
				    classes.contains(SampleClassOf(previous[root_index]))) {
					chosen = SampleClassOf(previous[root_index]);
				}
				candidates([&](uint32_t resource) {
					if (SampleClassOf(specialization.images[resource]) != chosen) {
						snapshot.images[resource].dwords.fill(0u);
					}
				});
			}
			// A stable table is laid out below in the class it kept. A table the shader writes
			// through types its remaining candidates like any other table's.
			if (stable) continue;
		}
		uint32_t exemplar       = ImageResource::NoIndirectImage;
		uint32_t resource_count = 0;
		for (uint32_t resource = 0; resource < specialization.images.size(); resource++) {
			if (specialization.images[resource].indirect_root != root_index) {
				continue;
			}
			resource_count++;
			if (exemplar == ImageResource::NoIndirectImage &&
			    !NullImageDescriptor(snapshot.images[resource])) {
				exemplar = resource;
			}
		}
		if (resource_count < 2u || exemplar == ImageResource::NoIndirectImage) {
			// A stable table that selects nothing keeps its layout; its slots are null.
			if (stable) continue;
			return SpecializationFail("indirect image specialization has no typed candidate");
		}
		const auto image_class = specialization.images[exemplar];
		for (uint32_t candidate = 0; candidate < specialization.images.size(); candidate++) {
			auto& image = specialization.images[candidate];
			if (image.indirect_root != root_index) {
				continue;
			}
			if (NullImageDescriptor(snapshot.images[candidate])) {
				image.numeric_class     = image_class.numeric_class;
				image.dimension         = image_class.dimension;
				image.mip_count         = image_class.mip_count;
				image.conversion_format = image_class.conversion_format;
				image.shader_swizzle    = image_class.shader_swizzle;
				image.cube              = image_class.cube;
			}
			if (!compatible(image, image_class)) {
				return SpecializationFail(
				    fmt::format("indirect image table at pc 0x{:08x} has incompatible candidates",
				                program.info.images[root_index].first_use_pc));
			}
		}
	}
	LayOutSpeculativeImages(program, previous, snapshot, specialization);
	CompactImages(snapshot.images, [&](size_t index) { return !specialization.images[index].fmask; });
	return true;
}

bool BuildSamplerPlan(const ShaderInfo& base, SamplerPlan& plan) {
	if (base.samplers.size() > plan.mapping.size()) {
		return false;
	}
	std::array<uint8_t, ShaderInfo::MaxSamplers> usage {};
	plan.sampler_count = static_cast<uint32_t>(base.samplers.size());
	for (const auto& pair: base.sampled_pairs) {
		if (pair.image >= base.images.size() || pair.sampler >= base.samplers.size()) {
			return false;
		}
		usage[pair.sampler] |= 1u << static_cast<uint32_t>(ClassifySampler(base.images[pair.image]));
		for (const auto candidate: base.samplers[pair.sampler].indirect_resources) {
			if (candidate >= base.samplers.size()) return false;
			usage[candidate] |= usage[pair.sampler];
		}
	}
	for (uint32_t index = 0; index < base.samplers.size(); index++) {
		auto& mapping = plan.mapping[index];
		mapping.fill(UINT32_MAX);
		const auto classes = usage[index] == 0u ? 1u : usage[index];
		bool       first   = true;
		for (uint32_t type = 0; type < mapping.size(); type++) {
			if ((classes & (1u << type)) == 0u) continue;
			const auto target = first ? index : plan.sampler_count++;
			if (target >= ShaderInfo::MaxSamplers) return false;
			mapping[type]         = target;
			plan.bindings[target] = {index, static_cast<SamplerClass>(type)};
			first                = false;
		}
	}
	return true;
}

template <typename Predicate>
static std::vector<ResourceBlock> ResourceControlFlow(const Program& program, const Predicate& predicate) {
	// The renderer checks captured scalar reads against final buffer and image write ranges.
	if (program.has_address_writes) {
		return {};
	}
	std::unordered_map<const Block*, uint32_t> indices;
	for (uint32_t i = 0; i < program.blocks.size(); i++) {
		if (!indices.emplace(program.blocks[i], i).second) {
			return {};
		}
	}
	std::vector<ResourceBlock> blocks(program.blocks.size());
	for (uint32_t i = 0; i < blocks.size(); i++) {
		auto&                 block      = blocks[i];
		const auto&             info       = *program.blocks[i];
		const auto&           terminator = info.terminator;
		const std::array        targets {terminator.true_block, terminator.false_block};
		std::span<Block* const> successors;
		switch (terminator.kind) {
			case CFG::TerminatorKind::Branch: successors = std::span(targets).first(1); break;
			case CFG::TerminatorKind::ConditionalBranch:
				successors      = targets;
				block.condition = info.condition;
				break;
			case CFG::TerminatorKind::IndirectBranch: successors = info.ImmSuccessors(); break;
			case CFG::TerminatorKind::Return: break;
			default: return {};
		}
		for (const auto successor: successors) {
			const auto found = indices.find(successor);
			if (found == indices.end()) {
				return {};
			}
			block.successors.push_back(found->second);
		}
		for (const auto& inst: *program.blocks[i]) {
			const auto op     = inst.GetOpcode();
			if (op == ValueOpcode::ReadConst) {
				block.srt_reads.push_back(inst.Arg(1).Resolve().U32());
				continue;
			}
			const auto buffer = BufferAccessOf(op);
			const auto image  = ImageOpcodeInfoOf(op);
			if (buffer == BufferAccess::None && image.access == ImageAccess::None) {
				continue;
			}
			const auto& memory = program.memory_info.at(inst.Flags<MemoryFlags>().index);
			if (memory.planning_only || memory.kind == ResourceKind::IndirectBuffer) {
				continue;
			}
			if (buffer != BufferAccess::None) {
				block.sources.push_back(program.info.buffers.at(memory.resource).source);
			} else {
				block.sources.push_back(program.info.images.at(memory.resource).source);
				if (image.needs_sampler) {
					block.sources.push_back(program.info.samplers.at(memory.sampler).source);
				}
			}
		}
		std::ranges::sort(block.sources);
		block.sources.erase(std::unique(block.sources.begin(), block.sources.end()),
		                    block.sources.end());
	}
	for (auto& block: blocks) block.condition = predicate(block.condition);
	if (std::ranges::none_of(
	        blocks, [](const ResourceBlock& block) { return !block.condition.IsEmpty(); })) {
		return {};
	}
	return blocks;
}

// ResolveComputeBufferFill proves complete workgroups and matching requested extents, so every
// dispatch-bound predicate accepted here is true for every invocation covered by the fill.
static bool IsFullDispatchPredicate(Value value, uint32_t depth = 0) {
	value = value.Resolve();
	if (value == Value(true)) return true;
	const auto* inst = value.TryInstruction();
	if (inst == nullptr || depth > 32) return false;
	if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
		return IsFullDispatchPredicate(inst->Arg(0), depth + 1) &&
		       IsFullDispatchPredicate(inst->Arg(1), depth + 1);
	}
	if (inst->GetOpcode() != ValueOpcode::ULessThan32) return false;
	const auto* global = inst->Arg(0).Resolve().TryInstruction();
	const auto* extent = inst->Arg(1).Resolve().TryInstruction();
	if (global == nullptr || global->GetOpcode() != ValueOpcode::GetBuiltin ||
	    global->Arg(0).Resolve() != Value(static_cast<uint32_t>(StageInputKind::GlobalInvocationId)) ||
	    extent == nullptr || extent->GetOpcode() != ValueOpcode::GetDispatchThreadExtent)
		return false;
	const auto axis = global->Arg(1).Resolve();
	return axis.IsImmediate() && axis.U32() < 3 && extent->Arg(0).Resolve() == axis;
}

// Nonnegative affine coefficients for constant, local and workgroup coordinates. Reject modular
// arithmetic that could wrap; runtime coverage also bounds the largest invocation index.
static std::optional<std::array<uint64_t, 3>> FillIndex(Value value, uint32_t axis, Value guard,
                                                      uint32_t depth = 0) {
	value = ResolveActiveU32(value, guard);
	if (depth > 32 || value.GetType() != Type::U32) {
		return {};
	}
	if (value.IsImmediate()) {
		return std::array<uint64_t, 3> {value.U32(), 0, 0};
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) {
		return {};
	}
	const auto op = inst->GetOpcode();
	if (op == ValueOpcode::GetBuiltin && inst->Arg(1) == Value(axis)) {
		if (inst->Arg(0) == Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId))) {
			return std::array<uint64_t, 3> {0, 1, 0};
		}
		if (inst->Arg(0) == Value(static_cast<uint32_t>(StageInputKind::WorkgroupId))) {
			return std::array<uint64_t, 3> {0, 0, 1};
		}
	}
	if (op != ValueOpcode::IAdd32 && op != ValueOpcode::IMul32 &&
	    op != ValueOpcode::ShiftLeftLogical32) {
		return {};
	}
	auto left  = FillIndex(inst->Arg(0), axis, guard, depth + 1);
	auto right = FillIndex(inst->Arg(1), axis, guard, depth + 1);
	if (!left || !right) {
		return {};
	}
	if (op == ValueOpcode::IMul32 && ((*right)[1] != 0 || (*right)[2] != 0)) {
		std::swap(left, right);
	}
	if (op != ValueOpcode::IAdd32 && ((*right)[1] != 0 || (*right)[2] != 0)) {
		return {};
	}
	if (op == ValueOpcode::ShiftLeftLogical32) {
		if ((*right)[0] >= 32) return {};
		(*right)[0] = uint64_t {1} << (*right)[0];
	}
	for (uint32_t i = 0; i < left->size(); ++i) {
		(*left)[i] =
		    op == ValueOpcode::IAdd32 ? (*left)[i] + (*right)[i] : (*left)[i] * (*right)[0];
		if ((*left)[i] > UINT32_MAX) return {};
	}
	return left;
}

// Whether a conditional branch only skips the store's block when no lane would store: the EXEC test
// a compiler places around a predicated region, "some lane passes the store guard".
static bool IsStoreGuardSkip(const Block& block, const Block* store_block, Value guard) {
	auto        value = block.condition.Resolve();
	bool        outer = false;
	const Inst* test  = nullptr;
	while (test == nullptr) {
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return false;
		if (inst->GetOpcode() == ValueOpcode::ConditionRef) {
			test = inst;
		} else if (inst->GetOpcode() == ValueOpcode::LogicalNot) {
			outer = !outer;
			value = inst->Arg(0).Resolve();
		} else {
			return false;
		}
	}
	bool inner = false;
	value = test->Arg(0).Resolve();
	for (guard = guard.Resolve(); value != guard;) {
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::LogicalNot) return false;
		inner = !inner;
		value = inst->Arg(0).Resolve();
	}
	// ExecNonZero tests whether any lane holds its operand; ExecZero whether all lanes do.
	bool any = false;
	const auto kind = test->Flags<CFG::BranchCondition>();
	if (kind == CFG::BranchCondition::ExecNonZero && !inner) {
		any = true; // any(guard)
	} else if (kind != CFG::BranchCondition::ExecZero || !inner) {
		return false; // all(!guard) is !any(guard); other forms are not a skip.
	}
	if (outer) any = !any;
	return (any ? block.terminator.true_block : block.terminator.false_block) == store_block;
}

static UniformFillPlan AnalyzeUniformFill(const Program& program) {
	if (program.stage != ShaderType::Compute || program.blocks.empty() || program.info.uses_dma ||
	    !program.info.samplers.empty()) {
		return {};
	}
	// One store and no other effect in acyclic control flow; branches are checked below.
	std::unordered_map<const Block*, size_t> order;
	for (size_t index = 0; index < program.blocks.size(); ++index) {
		order.emplace(program.blocks[index], index);
	}
	const auto forward = [&](const Block* from, const Block* to) {
		const auto found = order.find(to);
		return to != nullptr && found != order.end() && found->second > order.at(from);
	};
	const Inst*  store       = nullptr;
	const Block* store_block = nullptr;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (AddressOpcodeInfoOf(inst.GetOpcode()).access != AddressAccess::None) return {};
			// References only keep a branch condition alive.
			if (!inst.MayHaveSideEffects() || inst.GetOpcode() == ValueOpcode::Reference ||
			    inst.GetOpcode() == ValueOpcode::ReferenceU32)
				continue;
			if (store != nullptr || (BufferAccessOf(inst.GetOpcode()) != BufferAccess::Write &&
			                         inst.GetOpcode() != ValueOpcode::ImageWrite))
				return {};
			store       = &inst;
			store_block = block;
		}
		const auto& term = block->terminator;
		if (term.loop_header) return {};
		if (term.kind == CFG::TerminatorKind::Return) continue;
		if ((term.kind != CFG::TerminatorKind::Branch &&
		     term.kind != CFG::TerminatorKind::ConditionalBranch) ||
		    !forward(block, term.true_block) ||
		    (term.kind == CFG::TerminatorKind::ConditionalBranch &&
		     !forward(block, term.false_block)))
			return {};
	}
	if (store == nullptr) return {};
	for (const auto* block: program.blocks) {
		if (block->terminator.kind == CFG::TerminatorKind::ConditionalBranch &&
		    (store->GetOpcode() == ValueOpcode::ImageWrite ||
		     !IsStoreGuardSkip(*block, store_block, store->Arg(5))))
			return {};
	}
	for (const auto& buffer: program.info.buffers) {
		if (buffer.read && (!buffer.scalar || buffer.written)) return {};
	}
	const auto& memory = program.memory_info.at(store->Flags<MemoryFlags>().index);
	UniformFillPlan result;
	result.fill.resource = memory.resource;
	Value data;
	Value guard(true);
	if (store->GetOpcode() == ValueOpcode::ImageWrite) {
		if (program.info.images.size() != 1 || memory.dmask != 1 || memory.data_bits != 32 ||
		    memory.image_has_mip || memory.image_sample_flags != 0 || memory.image_r128 ||
		    memory.image_dimension != Decoder::ImageDimension::Dim2DArray ||
		    store->Arg(3).Resolve() != Value(true)) return {};
		const auto& image = program.info.images[memory.resource];
		if (image.read || image.atomic || image.mip_mode != ImageMipMode::None) return {};
		const auto* address = store->Arg(1).ResolveInstruction();
		if (address == nullptr || address->GetOpcode() != ValueOpcode::MakeImageAddress) return {};
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const auto index = FillIndex(address->Arg(axis), axis, guard);
			if (!index || (*index)[0] != 0) return {};
			if (axis < 2) {
				if ((*index)[1] != 1 || (*index)[2] == 0) return {};
			} else if ((*index)[1] != 0 || (*index)[2] != 1) {
				return {};
			}
			result.fill.group_stride[axis] = static_cast<uint32_t>((*index)[2]);
		}
		const auto* values = store->Arg(2).ResolveInstruction();
		if (values == nullptr || values->GetOpcode() != ValueOpcode::CompositeConstructU32x4)
			return {};
		result.fill.kind  = UniformFillKind::Image;
		result.fill.words = 1;
		data = values->Arg(0);
	} else {
		if (!program.info.images.empty()) return {};
		const auto           op = store->GetOpcode();
		constexpr std::array stores {ValueOpcode::StoreBufferU32, ValueOpcode::StoreBufferU32x2,
		                             ValueOpcode::StoreBufferU32x3, ValueOpcode::StoreBufferU32x4};
		const auto           store_op = std::ranges::find(stores, op);
		if (store_op == stores.end() || store->Arg(2).Resolve() != Value(0u) ||
		    store->Arg(3).Resolve() != Value(0u))
			return {};
		guard = store->Arg(5).Resolve();
		if (!IsFullDispatchPredicate(guard)) {
			// A memset kernel bounds its index by a host-provided element count.
			const auto* bound = guard.TryInstruction();
			const auto  index = store->Arg(1).Resolve();
			if (bound != nullptr && bound->GetOpcode() == ValueOpcode::UGreaterThan32 &&
			    bound->Arg(1).Resolve() == index) {
				result.count = bound->Arg(0).Resolve();
			} else if (bound != nullptr && bound->GetOpcode() == ValueOpcode::ULessThan32 &&
			           bound->Arg(0).Resolve() == index) {
				result.count = bound->Arg(1).Resolve();
			}
			if (result.count.IsEmpty() || result.count.GetType() != Type::U32 ||
			    !ValidateRuntimeValue(program, result.count, RuntimeValueType::Integer))
				return {};
		}
		if (!memory.formatted || memory.typed || !memory.idxen || memory.offen || memory.offset != 0 ||
		    memory.data_bits != 32 ||
		    memory.data_dwords != static_cast<uint32_t>(store_op - stores.begin() + 1))
			return {};
		const auto address = FillIndex(store->Arg(1), 0, guard);
		if (!address || (*address)[0] != 0 || (*address)[1] != 1 || (*address)[2] == 0) return {};
		result.fill.kind = UniformFillKind::Buffer;
		result.fill.group_stride[0] = static_cast<uint32_t>((*address)[2]);
		result.fill.words = memory.data_dwords;
		data = store->Arg(4);
	}
	data = ResolveActiveU32(data, guard);
	const auto*          vector = data.TryInstruction();
	constexpr std::array composites {ValueOpcode::CompositeConstructU32x2,
	                                 ValueOpcode::CompositeConstructU32x3,
	                                 ValueOpcode::CompositeConstructU32x4};
	if (result.fill.words > 1 &&
	    (vector == nullptr || vector->GetOpcode() != composites[result.fill.words - 2]))
		return {};
	for (uint32_t i = 0; i < result.fill.words; ++i) {
		const auto word = ResolveActiveU32(result.fill.words == 1 ? data : vector->Arg(i), guard);
		if (word.GetType() != Type::U32 ||
		    !ValidateRuntimeValue(program, word, RuntimeValueType::Integer))
			return {};
		result.values[i] = word;
	}
	return result;
}

// Whether every lane the dispatch launches holds `value`: EXEC at entry, EXEC restored to all ones
// (after an S_MOV of -1, each lane tests its bit of 0xffffffff), dispatch-extent predicates (the renderer
// bounds the lanes it copies by the dispatch), and masks merging only these. A mask a loop carries
// holds on every iteration when it holds on entry and its back edge keeps it.
static bool IsAllLanes(Value value, std::vector<const Inst*>& carried, uint32_t depth = 0) {
	value = value.Resolve();
	if (value == Value(true) || IsFullDispatchPredicate(value)) return true;
	const auto* inst = value.TryInstruction();
	if (inst == nullptr || depth > 32) return false;
	const auto immediate = [](Value operand, uint32_t expected) {
		operand = operand.Resolve();
		return operand.IsImmediate() && operand.GetType() == Type::U32 && operand.U32() == expected;
	};
	switch (inst->GetOpcode()) {
		case ValueOpcode::LogicalAnd:
			return IsAllLanes(inst->Arg(0), carried, depth + 1) &&
			       IsAllLanes(inst->Arg(1), carried, depth + 1);
		case ValueOpcode::Phi:
			if (std::ranges::find(carried, inst) != carried.end()) return true;
			carried.push_back(inst);
			for (size_t i = 0; i < inst->NumArgs(); ++i) {
				if (!IsAllLanes(inst->Arg(i), carried, depth + 1)) return false;
			}
			return true;
		case ValueOpcode::INotEqual32: {
			const auto* bit   = inst->Arg(0).ResolveInstruction();
			const auto* shift = bit != nullptr && bit->GetOpcode() == ValueOpcode::BitwiseAnd32
			                        ? bit->Arg(0).ResolveInstruction()
			                        : nullptr;
			const auto* lane  = shift != nullptr && shift->GetOpcode() == ValueOpcode::ShiftRightLogical32
			                        ? shift->Arg(1).ResolveInstruction()
			                        : nullptr;
			if (lane == nullptr || !immediate(inst->Arg(1), 0u) || !immediate(bit->Arg(1), 1u) ||
			    !immediate(shift->Arg(0), UINT32_MAX) || lane->GetOpcode() != ValueOpcode::BitwiseAnd32)
				return false;
			const auto mask = lane->Arg(1).Resolve();
			return mask.IsImmediate() && mask.GetType() == Type::U32 && mask.U32() < 32u;
		}
		default: return false;
	}
}

// Splits a store guard into all-lanes terms and at most one bound, index < count.
static bool SplitCopyGuard(Value guard, Value index, Value& count, uint32_t depth = 0) {
	guard = guard.Resolve();
	std::vector<const Inst*> carried;
	if (IsAllLanes(guard, carried)) return true;
	const auto* inst = guard.TryInstruction();
	if (inst == nullptr || depth > 32) return false;
	if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
		return SplitCopyGuard(inst->Arg(0), index, count, depth + 1) &&
		       SplitCopyGuard(inst->Arg(1), index, count, depth + 1);
	}
	if (!count.IsEmpty()) return false;
	if (inst->GetOpcode() == ValueOpcode::UGreaterThan32 && inst->Arg(1).Resolve() == index) {
		count = inst->Arg(0).Resolve();
	} else if (inst->GetOpcode() == ValueOpcode::ULessThan32 && inst->Arg(0).Resolve() == index) {
		count = inst->Arg(1).Resolve();
	}
	return !count.IsEmpty();
}

// A kernel whose only effect is copying buffer elements (see UniformCopyPlan). Each store writes,
// at the invocation's global index, what a load of the same width, form and guard read at that
// index. Branches are uniform except EXEC tests that skip a store block no lane would store in.
// One counted loop may surround the copies; descriptors, counts and the trip count are evaluated
// per iteration from memory, so they must not depend on anything the loop computes but the counter.
static UniformCopyPlan AnalyzeUniformCopy(const Program& program) {
	const auto& info = program.info;
	if (program.stage != ShaderType::Compute || program.blocks.empty() || program.has_address_writes ||
	    program.dispatcher_fallback || !info.images.empty() || !info.samplers.empty() ||
	    info.uses_lds || info.uses_gds || info.function_lds || info.function_scratch) {
		return {};
	}
	std::unordered_map<const Block*, size_t> order;
	for (size_t index = 0; index < program.blocks.size(); ++index) {
		order.emplace(program.blocks[index], index);
	}
	std::vector<const Inst*> stores;
	const Block*             header = nullptr;
	const Block*             latch  = nullptr;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto op = inst.GetOpcode();
			// Scalar reads of guest memory feed counts and descriptors; nothing else addresses it.
			if (AddressOpcodeInfoOf(op).access != AddressAccess::None &&
			    (op != ValueOpcode::LoadAddressU32 ||
			     program.memory_info.at(inst.Flags<MemoryFlags>().index).kind !=
			         ResourceKind::ScalarAddress))
				return {};
			if (!inst.MayHaveSideEffects() || op == ValueOpcode::Reference ||
			    op == ValueOpcode::ReferenceU32)
				continue;
			if (BufferAccessOf(op) != BufferAccess::Write) return {};
			stores.push_back(&inst);
		}
		const auto& term = block->terminator;
		if (term.kind == CFG::TerminatorKind::Return) continue;
		if (term.kind != CFG::TerminatorKind::Branch &&
		    term.kind != CFG::TerminatorKind::ConditionalBranch)
			return {};
		const std::array targets {term.true_block, term.false_block};
		const auto       count = term.kind == CFG::TerminatorKind::ConditionalBranch ? 2u : 1u;
		for (uint32_t i = 0; i < count; ++i) {
			const auto found = order.find(targets[i]);
			if (found == order.end()) return {};
			if (found->second > order.at(block)) continue;
			// An unconditional back edge closes the only loop.
			if (latch != nullptr || term.kind != CFG::TerminatorKind::Branch) return {};
			header = targets[i];
			latch  = block;
		}
	}
	if (stores.empty()) return {};
	UniformCopyPlan result;
	std::unordered_set<const Block*> body;
	const Inst*  counter = nullptr;
	const Block* test    = nullptr;
	if (latch != nullptr) {
		body.insert(header);
		for (std::vector<const Block*> pending {latch}; !pending.empty();) {
			const auto* block = pending.back();
			pending.pop_back();
			if (!body.insert(block).second) continue;
			for (const auto* predecessor: block->ImmPredecessors()) pending.push_back(predecessor);
		}
		// The loop tests its counter at the top, in a straight run of blocks without stores, and
		// leaves only there; code outside it is straight-line and stores nothing.
		test = header;
		for (uint32_t steps = 0; test->terminator.kind == CFG::TerminatorKind::Branch; ++steps) {
			test = test->terminator.true_block;
			if (steps > 8 || !body.contains(test) || test == header) return {};
		}
		if (test->terminator.kind != CFG::TerminatorKind::ConditionalBranch) return {};
		for (const auto* block: program.blocks) {
			const auto& term = block->terminator;
			const bool inside = body.contains(block);
			if (term.kind == CFG::TerminatorKind::ConditionalBranch && block != test &&
			    (!inside || body.contains(term.true_block) != body.contains(term.false_block)))
				return {};
			if (!inside || block == header) continue;
			for (const auto& inst: *block) {
				if (inst.GetOpcode() == ValueOpcode::Phi && inst.HasUses()) return {};
			}
		}
		for (const auto* store: stores) {
			if (!body.contains(store->Parent())) return {};
		}
		for (const Block* block = program.blocks[0]; block != header; block = block->terminator.true_block) {
			if (block->terminator.kind != CFG::TerminatorKind::Branch || body.contains(block)) return {};
		}
		for (const Block* block = header;; block = block->terminator.true_block) {
			for (const auto* store: stores) {
				if (store->Parent() == block) return {};
			}
			if (block == test) break;
		}
		// The exit test, through scalar branch conditions only: counter < end stays in the loop.
		auto condition = test->condition.Resolve();
		bool negated   = false;
		for (uint32_t depth = 0;; ++depth) {
			const auto* inst = condition.TryInstruction();
			if (inst == nullptr || depth > 8) return {};
			if (inst->GetOpcode() == ValueOpcode::LogicalNot) {
				negated = !negated;
			} else if (inst->GetOpcode() == ValueOpcode::ConditionRef) {
				const auto kind = inst->Flags<CFG::BranchCondition>();
				if (kind != CFG::BranchCondition::SccZero && kind != CFG::BranchCondition::SccNonZero &&
				    kind != CFG::BranchCondition::ScalarInstruction)
					return {};
			} else {
				break;
			}
			condition = inst->Arg(0).Resolve();
		}
		const auto* compare = condition.TryInstruction();
		if (compare == nullptr) return {};
		Value      count_value;
		Value      end;
		bool       stays_if_true = true;
		switch (compare->GetOpcode()) {
			case ValueOpcode::ULessThan32: count_value = compare->Arg(0); end = compare->Arg(1); break;
			case ValueOpcode::UGreaterThan32: count_value = compare->Arg(1); end = compare->Arg(0); break;
			case ValueOpcode::UGreaterThanEqual32:
				count_value = compare->Arg(0), end = compare->Arg(1), stays_if_true = false;
				break;
			case ValueOpcode::ULessThanEqual32:
				count_value = compare->Arg(1), end = compare->Arg(0), stays_if_true = false;
				break;
			default: return {};
		}
		const auto& term = test->terminator;
		const auto* stay = stays_if_true != negated ? term.true_block : term.false_block;
		const auto* exit = stay == term.true_block ? term.false_block : term.true_block;
		counter = count_value.Resolve().TryInstruction();
		if (!body.contains(stay) || body.contains(exit) || counter == nullptr ||
		    counter->GetOpcode() != ValueOpcode::Phi || counter->Parent() != header ||
		    counter->GetType() != Type::U32 || counter->NumArgs() != 2u ||
		    !ValidateRuntimeValue(program, end, RuntimeValueType::Integer))
			return {};
		const Value self(const_cast<Inst*>(counter));
		bool        entered = false;
		bool        stepped = false;
		for (size_t i = 0; i < 2u; ++i) {
			const auto operand = counter->Arg(i).Resolve();
			if (!body.contains(counter->PhiBlock(i))) {
				entered = operand.IsImmediate() && operand.GetType() == Type::U32;
				if (entered) result.begin = operand.U32();
				continue;
			}
			const auto* step = operand.TryInstruction();
			stepped = step != nullptr && step->GetOpcode() == ValueOpcode::IAdd32 &&
			          ((step->Arg(0).Resolve() == self && step->Arg(1).Resolve() == Value(1u)) ||
			           (step->Arg(1).Resolve() == self && step->Arg(0).Resolve() == Value(1u)));
		}
		if (!entered || !stepped) return {};
		// Every other value the loop carries is an all-lanes EXEC mask.
		for (const auto& inst: *header) {
			std::vector<const Inst*> carried;
			if (inst.GetOpcode() == ValueOpcode::Phi && &inst != counter && inst.HasUses() &&
			    (inst.GetType() != Type::U1 || !IsAllLanes(Value(const_cast<Inst*>(&inst)), carried)))
				return {};
		}
		result.counter = self;
		result.end     = end.Resolve();
	} else {
		for (const auto* block: program.blocks) {
			for (const auto& inst: *block) {
				if (inst.GetOpcode() == ValueOpcode::Phi && inst.HasUses()) return {};
			}
		}
	}
	const Value counter_value = counter != nullptr ? Value(const_cast<Inst*>(counter)) : Value {};
	const auto  runtime_value = [&](Value value) {
		return value.GetType() == Type::U32 &&
		       ValidateRuntimeValue(program, value, RuntimeValueType::Integer, counter_value);
	};
	// A V# the copy reads or writes: its four DWORDs, or a tracked source (one table entry when the
	// key is computed on the GPU).
	const auto descriptor = [&](const Inst& access, UniformCopyPlan::Descriptor& out) {
		const auto& memory = program.memory_info.at(access.Flags<MemoryFlags>().index);
		if (memory.kind == ResourceKind::IndirectBuffer) {
			const auto* handle = access.Arg(0).ResolveInstruction();
			if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetBufferResource ||
			    handle->NumArgs() != 4u)
				return false;
			for (uint32_t i = 0; i < 4u; ++i) {
				out.dwords[i] = handle->Arg(i).Resolve();
				if (!runtime_value(out.dwords[i])) return false;
			}
			return true;
		}
		if (memory.kind != ResourceKind::Buffer || memory.resource >= info.buffers.size()) return false;
		const auto& buffer = info.buffers[memory.resource];
		const auto* source = Source(program, buffer.source);
		if (source == nullptr || buffer.indirect_root != UINT32_MAX || (buffer.read && buffer.written) ||
		    source->dword_count != 4u)
			return false;
		out.source = buffer.source;
		if (!source->indirect_descriptor) return true;
		const auto& indirect = *source->indirect_descriptor;
		const auto* table    = Source(program, indirect.table_source);
		if (!indirect.sources.empty() || indirect.selector || !indirect.selector_first.IsEmpty() ||
		    !indirect.selector_mask.IsEmpty() || indirect.workgroup_axis != UINT32_MAX ||
		    indirect.table_stride == 0u || indirect.selector_value.IsEmpty() || table == nullptr ||
		    table->indirect_descriptor || (table->dword_count != 2u && table->dword_count != 4u) ||
		    !runtime_value(indirect.selector_value))
			return false;
		out.key = indirect.selector_value.Resolve();
		return true;
	};
	constexpr std::array store_ops {ValueOpcode::StoreBufferU32, ValueOpcode::StoreBufferU32x2,
	                                ValueOpcode::StoreBufferU32x3, ValueOpcode::StoreBufferU32x4};
	constexpr std::array load_ops {ValueOpcode::LoadBufferU32, ValueOpcode::LoadBufferU32x2,
	                               ValueOpcode::LoadBufferU32x3, ValueOpcode::LoadBufferU32x4};
	constexpr std::array composites {ValueOpcode::CompositeConstructU32x2,
	                                 ValueOpcode::CompositeConstructU32x3,
	                                 ValueOpcode::CompositeConstructU32x4};
	constexpr std::array extracts {ValueOpcode::CompositeExtractU32x2,
	                               ValueOpcode::CompositeExtractU32x3,
	                               ValueOpcode::CompositeExtractU32x4};
	// Linear element addressing at the global index: idxen, no offsets, unswizzled DWORD data.
	const auto element_access = [&](const Inst& access, uint32_t words) {
		const auto& memory = program.memory_info.at(access.Flags<MemoryFlags>().index);
		return !memory.typed && memory.idxen && !memory.offen && memory.offset == 0u &&
		       memory.data_bits == 32u && memory.data_dwords == words &&
		       access.Arg(2).Resolve() == Value(0u) && access.Arg(3).Resolve() == Value(0u);
	};
	for (const auto* store: stores) {
		const auto words_found = std::ranges::find(store_ops, store->GetOpcode());
		if (words_found == store_ops.end()) return {};
		const auto  words     = static_cast<uint32_t>(words_found - store_ops.begin() + 1);
		const auto& memory    = program.memory_info.at(store->Flags<MemoryFlags>().index);
		const auto  guard     = store->Arg(5).Resolve();
		const auto  index     = store->Arg(1).Resolve();
		if (!element_access(*store, words)) return {};
		UniformCopyPlan::Copy copy {.words = words, .formatted = memory.formatted};
		if (!SplitCopyGuard(guard, index, copy.count) ||
		    (!copy.count.IsEmpty() && !runtime_value(copy.count)))
			return {};
		const auto address = FillIndex(index, 0, guard);
		if (!address || (*address)[0] != 0 || (*address)[1] != 1 || (*address)[2] == 0 ||
		    (result.group_stride != 0 && result.group_stride != (*address)[2]))
			return {};
		result.group_stride = static_cast<uint32_t>((*address)[2]);
		// The stored data is the loaded element, whole and in order.
		auto        data = words == 1u ? ResolveActiveU32(store->Arg(4), guard) : store->Arg(4).Resolve();
		const auto* load = data.TryInstruction();
		if (load != nullptr && words > 1u && load->GetOpcode() == composites[words - 2]) {
			const Inst* parts = nullptr;
			for (uint32_t i = 0; i < words; ++i) {
				const auto* part = load->Arg(i).ResolveInstruction();
				if (part == nullptr || part->GetOpcode() != extracts[words - 2] ||
				    part->Arg(1).Resolve() != Value(i) ||
				    (parts != nullptr && part->Arg(0).ResolveInstruction() != parts))
					return {};
				parts = part->Arg(0).ResolveInstruction();
			}
			load = parts;
		}
		std::vector<const Inst*> carried;
		if (load == nullptr || load->GetOpcode() != load_ops[words - 1] || !element_access(*load, words) ||
		    program.memory_info.at(load->Flags<MemoryFlags>().index).formatted != memory.formatted ||
		    load->Arg(1).Resolve() != index ||
		    (load->Arg(4).Resolve() != guard && !IsAllLanes(load->Arg(4), carried)) ||
		    !descriptor(*load, copy.source) || !descriptor(*store, copy.destination))
			return {};
		result.copies.push_back(std::move(copy));
	}
	// Every other conditional branch only skips a block whose stores no lane performs, and rejoins
	// where that block continues.
	for (const auto* block: program.blocks) {
		const auto& term = block->terminator;
		if (term.kind != CFG::TerminatorKind::ConditionalBranch || block == test) continue;
		bool skip = false;
		for (const auto* stored: {term.true_block, term.false_block}) {
			const auto* rejoin = stored == term.true_block ? term.false_block : term.true_block;
			const Inst* first  = nullptr;
			bool        same   = true;
			for (const auto* store: stores) {
				if (store->Parent() != stored) continue;
				same &= first == nullptr || store->Arg(5).Resolve() == first->Arg(5).Resolve();
				if (first == nullptr) first = store;
			}
			skip |= first != nullptr && same && stored->terminator.kind == CFG::TerminatorKind::Branch &&
			        stored->terminator.true_block == rejoin &&
			        IsStoreGuardSkip(*block, stored, first->Arg(5));
		}
		if (!skip) return {};
	}
	return result;
}

ResourcePlan ExtractResourcePlan(const Program& program) {
	ResourcePlan plan;
	plan.stage                      = program.stage;
	plan.shader_hash                = program.shader_hash;
	plan.user_data_base             = program.user_data_base;
	plan.user_data_count            = program.user_data_count;
	plan.info                       = program.info;
	plan.memory_info                = program.memory_info;
	plan.source_reads               = program.source_reads;
	plan.srt_plan_complete          = program.srt_plan_complete;
	plan.resource_tracking_complete = program.resource_tracking_complete;

	std::unordered_map<const Inst*, Inst*> cloned;
	std::function<Value(Value)>            Clone = [&](Value value) -> Value {
		value              = value.Resolve();
		const auto* source = value.TryInstruction();
		if (source == nullptr) {
			return value;
		}
		if (source->GetOpcode() == ValueOpcode::ReadConst) {
			const auto slot = source->Arg(1).Resolve();
			EXIT_IF(!slot.IsImmediate() || slot.GetType() != Type::U32 ||
			        slot.U32() >= program.srt_reads.size());
			return Clone(program.srt_reads[slot.U32()].value);
		}
		if (source->GetOpcode() == ValueOpcode::ReadFirstLane &&
		    ValidateRuntimeValue(program, source->Arg(0))) {
			// Uniform values need no new EXEC context; preserve their shared evaluation memo.
			return Clone(source->Arg(0));
		}
		if (source->GetOpcode() == ValueOpcode::Phi) {
			const auto invariant = ResolveInvariantPhi(program, value);
			if (!invariant.IsEmpty() && invariant != value) {
				return Clone(invariant);
			}
		}
		if (const auto found = cloned.find(source); found != cloned.end()) {
			return Value(found->second);
		}
		if (source->GetOpcode() == ValueOpcode::LoadBufferU32) {
			plan.requires_specialization_memory = true;
			plan.capture_specialization_reads   = true;
		}
		auto& target =
		    plan.value_storage.emplace_back(source->GetOpcode(), source->Flags<uint64_t>());
		if (source->GetOpcode() == ValueOpcode::LoadAddressU32 ||
		    source->GetOpcode() == ValueOpcode::ReadConstBuffer ||
		    source->GetOpcode() == ValueOpcode::LoadBufferU32) {
			target.SetFlags(SrtReadFlags {.index = source->Flags<MemoryFlags>().index});
		}
		cloned.emplace(source, &target);
		if (source->GetOpcode() == ValueOpcode::Phi) {
			for (size_t index = 0; index < source->NumArgs(); index++) {
				target.AddPhiOperand(nullptr, Clone(source->Arg(index)));
			}
		} else {
			for (size_t index = 0; index < source->NumArgs(); index++) {
				target.SetArg(index, Clone(source->Arg(index)));
			}
		}
		return Value(&target);
	};

	bool capture_indirect_reads = false;
	plan.descriptor_sources.reserve(program.descriptor_sources.size());
	for (const auto& source: program.descriptor_sources) {
		auto& target          = plan.descriptor_sources.emplace_back();
		target.dword_count    = source.dword_count;
		target.indirect_descriptor = source.indirect_descriptor;
		if (target.indirect_descriptor.has_value()) {
			plan.requires_specialization_memory = true;
			capture_indirect_reads = true;
			target.indirect_descriptor->key_count = Clone(target.indirect_descriptor->key_count);
			target.indirect_descriptor->selector_first =
			    Clone(target.indirect_descriptor->selector_first);
			target.indirect_descriptor->selector_mask =
			    Clone(target.indirect_descriptor->selector_mask);
			target.indirect_descriptor->selector_value =
			    Clone(target.indirect_descriptor->selector_value);
			for (auto& range: target.indirect_descriptor->selector_ranges) {
				range = {Clone(range.counter), Clone(range.begin), Clone(range.end)};
			}
		}
		for (uint32_t dword = 0; dword < source.dword_count; dword++) {
			target.dwords[dword] = Clone(source.dwords[dword]);
		}
	}
	plan.srt_reads.reserve(program.srt_reads.size());
	for (const auto& read: program.srt_reads) {
		plan.srt_reads.push_back({Clone(read.value), read.flat_offset});
	}
	for (const auto& [low, high]: program.address_bases) {
		if (ValidateRuntimeValue(program, low, RuntimeValueType::Integer) &&
		    ValidateRuntimeValue(program, high, RuntimeValueType::Integer)) {
			plan.address_bases.push_back({Clone(low), Clone(high)});
		}
	}
	// A proven uniform factor can decide a branch even when its other lanes are unknown.
	// Keep only that Boolean structure, never the varying shader dependency graph.
	Value unknown;
	bool capture_control_reads = false;
	std::unordered_map<const Inst*, Value> predicates;
	std::function<Value(Value)> ClonePredicate = [&](Value value) -> Value {
		value = value.Resolve();
		if (value.IsEmpty()) return {};
		const auto* inst = value.TryInstruction();
		if (inst == nullptr)
			return ValidateRuntimeValue(program, value, RuntimeValueType::Integer) ? value : Value {};
		// Preserve shared EXEC predicates, including subgraphs with no uniform factor.
		const auto [found, inserted] = predicates.try_emplace(inst);
		if (!inserted) return found->second;
		auto& result = found->second;
		if (ValidateRuntimeValue(program, value, RuntimeValueType::Integer)) {
			capture_control_reads |=
			    !ValidateRuntimeValue(program, value, RuntimeValueType::ImmutableInteger);
			return result = Clone(value);
		}
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::ConditionRef || op == ValueOpcode::LogicalNot) {
			const auto operand = ClonePredicate(inst->Arg(0));
			if (operand.IsEmpty() || op == ValueOpcode::ConditionRef) return result = operand;
			auto& node = plan.value_storage.emplace_back(op);
			node.SetArg(0, operand);
			return result = Value(&node);
		}
		if (op != ValueOpcode::LogicalAnd && op != ValueOpcode::LogicalOr) return {};
		auto left = ClonePredicate(inst->Arg(0));
		auto right = ClonePredicate(inst->Arg(1));
		if (left.IsEmpty() && right.IsEmpty()) return {};
		if (left.IsEmpty() || right.IsEmpty()) {
			if (unknown.IsEmpty()) unknown = Value(&plan.value_storage.emplace_back(ValueOpcode::UndefU1));
			if (left.IsEmpty()) left = unknown;
			if (right.IsEmpty()) right = unknown;
		}
		auto& node = plan.value_storage.emplace_back(op);
		node.SetArg(0, left);
		node.SetArg(1, right);
		return result = Value(&node);
	};
	plan.control_flow = ResourceControlFlow(program, ClonePredicate);
	plan.uniform_fill = AnalyzeUniformFill(program);
	for (uint32_t i = 0; i < plan.uniform_fill.fill.words; ++i) {
		plan.uniform_fill.values[i] = Clone(plan.uniform_fill.values[i]);
	}
	if (!plan.uniform_fill.count.IsEmpty()) {
		plan.uniform_fill.count = Clone(plan.uniform_fill.count);
	}
	plan.uniform_copy = AnalyzeUniformCopy(program);
	const auto clone_descriptor = [&](UniformCopyPlan::Descriptor& descriptor) {
		if (descriptor.source == UINT32_MAX) {
			for (auto& dword: descriptor.dwords) dword = Clone(dword);
		} else if (!descriptor.key.IsEmpty()) {
			descriptor.key = Clone(descriptor.key);
		}
	};
	for (auto& copy: plan.uniform_copy.copies) {
		clone_descriptor(copy.source);
		clone_descriptor(copy.destination);
		if (!copy.count.IsEmpty()) copy.count = Clone(copy.count);
	}
	if (!plan.uniform_copy.counter.IsEmpty()) {
		plan.uniform_copy.counter = Clone(plan.uniform_copy.counter);
		plan.uniform_copy.end     = Clone(plan.uniform_copy.end);
	}
	plan.capture_specialization_reads |=
	    capture_indirect_reads || capture_control_reads || !plan.source_reads.empty();
	if (plan.capture_specialization_reads) {
		// Every former flat alias now targets its retained raw read. Mark those reads
		// on the normalized graph, so discarded Phi/EXEC dependencies stay discarded.
		std::unordered_set<const Inst*> planned_reads;
		for (const auto& read: plan.srt_reads) {
			auto* inst = read.value.ResolveInstruction();
			EXIT_IF(inst == nullptr || (inst->GetOpcode() != ValueOpcode::LoadAddressU32 &&
			                           inst->GetOpcode() != ValueOpcode::ReadConstBuffer));
			planned_reads.insert(inst);
		}
		for (const auto& source: plan.descriptor_sources) {
			if (!source.indirect_descriptor.has_value()) continue;
			const auto& indirect = *source.indirect_descriptor;
			if (indirect.selector)
				MarkCleanReads(planned_reads, Source(plan, indirect.selector->source));
			MarkCleanReads(planned_reads, nullptr, indirect.selector_mask);
			MarkCleanReads(planned_reads, nullptr, indirect.selector_first);
			MarkCleanReads(planned_reads, nullptr, indirect.key_count);
			MarkCleanReads(planned_reads, nullptr, indirect.selector_value);
			for (const auto& range: indirect.selector_ranges) {
				MarkCleanReads(planned_reads, nullptr, range.begin);
				MarkCleanReads(planned_reads, nullptr, range.end);
			}
			if (indirect.sources.empty()) {
				MarkCleanReads(planned_reads, Source(plan, indirect.table_source));
			} else {
				for (const auto candidate: indirect.sources) {
					MarkCleanReads(planned_reads, Source(plan, candidate));
				}
			}
		}
		// Clean writable addresses prove that shader writes cannot overlap captured reads.
		for (const auto& buffer: plan.info.buffers) {
			if (buffer.written) MarkCleanReads(planned_reads, Source(plan, buffer.source));
		}
		for (const auto& image: plan.info.images) {
			if (image.written) MarkCleanReads(planned_reads, Source(plan, image.source));
		}
	}
	if (capture_indirect_reads || !plan.source_reads.empty())
		plan.resource_tracking_complete &= !program.has_address_writes;
	CompileSrtWalker(plan);
	return plan;
}

// Every copy a proven copy kernel performs in this dispatch. Each loop iteration binds the counter
// and reads the descriptors and counts from the memory the kernel reads them from.
static bool EvaluateUniformCopies(const ResourcePlan& program, const SrtRuntime& runtime,
                                  ResourceSnapshot& snapshot) {
	constexpr uint32_t MaxIterations = 64;
	const auto&        plan          = program.uniform_copy;
	SrtReadCapture     capture {runtime, snapshot.uniform_copy_reads};
	const auto         clean = CleanRuntime(capture.ObservedRuntime());
	uint32_t           end   = plan.begin + 1u;
	if (!plan.counter.IsEmpty()) {
		SrtWalker walker(program, clean);
		if (!walker.Evaluate(plan.end, end)) return false;
	}
	if (end > plan.begin && end - plan.begin > MaxIterations) return false;
	const auto read = [&](SrtWalker& walker, const UniformCopyPlan::Descriptor& recipe,
	                      std::array<uint32_t, 4>& words) {
		if (recipe.source == UINT32_MAX) {
			for (uint32_t i = 0; i < 4u; ++i) {
				if (!walker.Evaluate(recipe.dwords[i], words[i])) return false;
			}
			return true;
		}
		const auto&     source = program.descriptor_sources[recipe.source];
		DescriptorValue value;
		if (!source.indirect_descriptor) {
			if (!walker.EvaluateDescriptor(recipe.source, value) || value.dword_count != 4u) return false;
			std::copy_n(value.dwords.begin(), 4, words.begin());
			return true;
		}
		// The table entry at the key, read as MaterializeIndirectDescriptor reads candidates.
		const auto& indirect = *source.indirect_descriptor;
		uint32_t    key      = 0;
		if (!walker.Evaluate(recipe.key, key) ||
		    !walker.EvaluateDescriptor(indirect.table_source, value))
			return false;
		uint64_t             base = 0;
		uint64_t             size = UINT64_MAX;
		ShaderBufferResource table;
		if (value.dword_count == 2u) {
			base = (static_cast<uint64_t>(value.dwords[1]) << 32u) | value.dwords[0];
		} else if (DecodeBufferDescriptor(value, table)) {
			base = table.Base48();
			size = table.GetSize();
		} else {
			return false;
		}
		const uint64_t offset =
		    uint64_t {(key * indirect.table_stride + indirect.table_offset) & ~3u} +
		    (indirect.table_immediate & ~3u);
		return ReadScalarTable(base, size, offset, clean, words);
	};
	for (uint32_t iteration = plan.begin; iteration < end; ++iteration) {
		SrtWalker walker(program, clean);
		if (!plan.counter.IsEmpty()) walker.Bind(plan.counter, iteration);
		for (const auto& recipe: plan.copies) {
			auto& copy = snapshot.uniform_copies.emplace_back(
			    UniformCopy {.count = UINT32_MAX, .words = recipe.words, .formatted = recipe.formatted});
			if ((!recipe.count.IsEmpty() && !walker.Evaluate(recipe.count, copy.count)) ||
			    !read(walker, recipe.source, copy.source) ||
			    !read(walker, recipe.destination, copy.destination))
				return false;
		}
	}
	snapshot.copy_group_stride = plan.group_stride;
	return true;
}

bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization) {
	if (!program.resource_tracking_complete) {
		return SpecializationFail("resource tracking did not complete");
	}
	if ((program.requires_specialization_memory || !program.source_reads.empty()) &&
	    runtime.read_specialization_memory == nullptr) {
		return SpecializationFail("specialization memory is required but cannot be read");
	}
	const bool capture_reads = program.capture_specialization_reads || !program.source_reads.empty();
	auto& reads = snapshot.specialization_reads;
	reads = program.source_reads;
	SrtReadCapture capture {runtime, reads};
	const auto observed = capture_reads ? capture.ObservedRuntime() : runtime;
	SrtWalker clean(program, CleanRuntime(observed));
	SrtWalker walker(program, observed,
	                 capture_reads || program.requires_specialization_memory ? &clean : nullptr);
	if (!walker.RefreshFlatBuffer(snapshot.flattened_srt)) {
		return SpecializationFail("the flattened SRT could not be read");
	}
	const auto active = std::span<const uint8_t>(program.active_sources);
	snapshot.uniform_fill = {};
	const auto& fill = program.uniform_fill;
	const auto words = fill.fill.words;
	std::array<uint32_t, 4> stored {};
	bool uniform_fill = words != 0;
	for (uint32_t i = 0; i < words && uniform_fill; ++i) {
		uniform_fill = clean.Evaluate(fill.values[i], stored[i]) && stored[i] == stored[0];
	}
	uint32_t fill_count = UINT32_MAX;
	if (uniform_fill && !fill.count.IsEmpty()) {
		uniform_fill = clean.Evaluate(fill.count, fill_count);
	}
	if (uniform_fill) {
		snapshot.uniform_fill = fill.fill;
		snapshot.uniform_fill.value = stored[0];
		snapshot.uniform_fill.count = fill_count;
	}
	snapshot.uniform_copies.clear();
	snapshot.uniform_copy_reads.clear();
	snapshot.copies_proven = !program.uniform_copy.copies.empty() &&
	                         runtime.read_specialization_memory != nullptr &&
	                         EvaluateUniformCopies(program, runtime, snapshot);
	const auto evaluate = [&](uint32_t source, DescriptorValue& value, bool written = false) {
		if (source >= program.descriptor_sources.size()) {
			return false;
		}
		if (active.empty() || active[source]) {
			return (capture_reads && written ? clean : walker).EvaluateDescriptor(source, value);
		}
		value = {};
		value.dword_count = program.descriptor_sources[source].dword_count;
		return true;
	};
	snapshot.buffers.resize(program.info.buffers.size());
	specialization.buffers.assign(program.info.buffers.size(), {});
	for (uint32_t i = 0; i < snapshot.buffers.size(); ++i) {
		const auto base_index =
		    i < program.info.buffers.size() ? i : specialization.buffers[i].indirect_root;
		if (base_index >= program.info.buffers.size()) {
			return SpecializationFail(fmt::format("buffer {} has an invalid root", i));
		}
		const auto& base = program.info.buffers[base_index];
		if (i < program.info.buffers.size()) {
			const auto* source = Source(program, base.source);
			if (source == nullptr) {
				return SpecializationFail(fmt::format("buffer {} has no descriptor source", i));
			}
			if (source->indirect_descriptor.has_value()) {
				snapshot.buffers[i] = {.dword_count = 4u};
				if (active.empty() || active[base.source]) {
					const bool speculative = !source->indirect_descriptor->selector_value.IsEmpty();
					if (!MaterializeIndirectDescriptor(
					        program, *source->indirect_descriptor, i, 4u, observed, clean, snapshot,
					        snapshot.buffers, specialization.buffers, ShaderInfo::MaxBuffers, 0u,
					        [&](DescriptorValue& value) {
						        // An enumerated key set can reach unused entries. One whose memory
						        // is not mapped cannot be the destination the GPU writes.
						        ShaderBufferResource descriptor;
						        if (speculative && runtime.is_mapped != nullptr &&
						            DecodeBufferDescriptor(value, descriptor) &&
						            !runtime.is_mapped(descriptor.Base48(),
						                               std::max<uint64_t>(descriptor.GetSize(), 1u))) {
							        value.dwords.fill(0u);
						        }
						        return NormalizeIndirectStoreBuffer(value);
					        }))
						return SpecializationFail(
						    fmt::format("buffer {} descriptor table could not be enumerated", i));
				}
			} else if (!evaluate(base.source, snapshot.buffers[i], base.written)) {
				return SpecializationFail(fmt::format("buffer {} descriptor could not be read", i));
			}
		}
		auto&                descriptor_value = snapshot.buffers[i];
		ShaderBufferResource descriptor;
		if (!DecodeBufferDescriptor(descriptor_value, descriptor)) {
			return SpecializationFail(fmt::format("buffer descriptor {} has invalid width", i));
		}
		if (descriptor.Type() != 0) {
			descriptor_value.dwords.fill(0);
			descriptor = {};
		}
		auto       packed_stride = descriptor.PackedStride();
		const auto stride        = packed_stride & 0x3fffu;
		const bool swizzle       = stride != 0u && ((packed_stride >> 14u) & 1u) != 0u;
		if (stride == 0u) {
			packed_stride &= ~((1u << 14u) | (3u << 16u));
		} else if (!swizzle) {
			packed_stride &= ~(3u << 16u);
		}
		auto& buffer         = specialization.buffers[i];
		buffer.packed_stride = packed_stride;
		buffer.descriptor_format =
		    base.formatted ? descriptor.Format() : Prospero::BufferFormat::kInvalid;
		buffer.descriptor_swizzle = base.formatted ? descriptor.DstSelXYZW() : DstSel(4, 5, 6, 7);
		buffer.zero_stride_oob    = descriptor.OutOfBounds() == 0u && stride == 0u;
	}
	// The previous dispatch's layout, which speculative tables keep where they can.
	// Swapped with a retained scratch vector so neither layout is reallocated per dispatch.
	static thread_local std::vector<ResourceSpecialization::Image> previous_images;
	previous_images.swap(specialization.images);
	specialization.images.clear();
	snapshot.images.resize(program.info.images.size());
	specialization.images.resize(program.info.images.size());
	for (uint32_t i = 0; i < program.info.images.size(); ++i) {
		const auto& image = program.info.images[i];
		specialization.images[i] = {
		    .numeric_class = image.numeric_class,
		    .dimension = image.dimension,
		    .mip_count = image.mip_count,
		    .conversion_format = image.conversion_format,
		    .shader_swizzle = image.shader_swizzle,
		    .indirect_root = image.indirect_root,
		    .indirect_mapping_offset = image.indirect_mapping_offset,
		    .indirect_search_iterations = image.indirect_search_iterations,
		    .cube = image.cube,
		};
		const auto* source = Source(program, image.source);
		if (source == nullptr) {
			return SpecializationFail(fmt::format("image {} has no descriptor source", i));
		}
		if (source->indirect_descriptor.has_value()) {
			snapshot.images[i] = {.dword_count = 8u};
			if (!active.empty() && !active[image.source]) {
				continue;
			}
			const auto& indirect = *source->indirect_descriptor;
			// A table entry whose image is not mapped and aligned for its tiling cannot be the one
			// the GPU samples.
			const bool speculative = IsSpeculativeImageTable(program, source);
			const auto unmapped    = [&](const DescriptorValue& value) {
				return speculative && runtime.is_placeable_texture != nullptr &&
				       !runtime.is_placeable_texture(std::span<const uint32_t, 8>(value.dwords));
			};
			constexpr uint32_t MinimumSpeculativeIterations = 4;
			const auto         padded_iterations =
			    !HasStableImageLayout(program, i) ? 0u
			    : i < previous_images.size() && previous_images[i].indirect_root == i
			        ? std::max(MinimumSpeculativeIterations,
			                   previous_images[i].indirect_search_iterations)
			        : MinimumSpeculativeIterations;
			if (!MaterializeIndirectDescriptor(
			        program, indirect, i, 8u, observed, clean, snapshot, snapshot.images,
			        specialization.images, UINT32_MAX, padded_iterations, [&](DescriptorValue& value) {
				        // A broad heap also contains resources for other typed image operations, and an
				        // operation only samples an image of its own dimension.
				        if (NullImageDescriptor(value) || !ValidImageDescriptor(value, image.r128) ||
				            (speculative && DescriptorDimension(value, image.dimension) != image.dimension) ||
				            unmapped(value))
					        value.dwords.fill(0u);
				        return true;
			        })) {
				return SpecializationFail(
				    fmt::format("image {} descriptor table could not be enumerated", i));
			}
		} else {
			if (!evaluate(image.source, snapshot.images[i], image.written)) {
				return SpecializationFail(fmt::format("image {} descriptor could not be read", i));
			}
			if (!ValidImageDescriptor(snapshot.images[i], image.r128)) {
				snapshot.images[i].dwords.fill(0);
			}
		}
	}
	snapshot.samplers.resize(program.info.samplers.size());
	for (uint32_t i = 0; i < program.info.samplers.size(); ++i) {
		auto source = program.info.samplers[i].source;
		if (source >= program.descriptor_sources.size()) {
			return SpecializationFail(fmt::format("sampler {} has no descriptor source", i));
		}
		const auto& descriptor = program.descriptor_sources[source];
		if (descriptor.indirect_descriptor) {
			if (descriptor.indirect_descriptor->sources.empty()) {
				return SpecializationFail(fmt::format("sampler {} table has no candidates", i));
			}
			source = descriptor.indirect_descriptor->sources[0];
		}
		if (!evaluate(source, snapshot.samplers[i])) {
			return SpecializationFail(fmt::format("sampler {} descriptor could not be read", i));
		}
		if (program.info.samplers[i].gather_lod) {
			const auto control = snapshot.samplers[i].dwords[2];
			const auto filter = (control >> 26u) & 3u;
			// MipNone always selects the base level. Explicit point gathers currently require
			// encoded-zero primary and secondary bias; linear primary-mip selection is unsupported.
			if (filter > 1u || (filter == 1u && (control & 0xfffffu) != 0u)) {
				return SpecializationFail(
				    "explicit-LOD gather requires mip filtering None or Point with zero LOD biases");
			}
		}
	}
	snapshot.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	snapshot.address_bases.clear();
	if (!program.address_bases.empty()) {
		// Read with the plain runtime: these addresses only steer buffer residency and must not
		// join the captured reads that decide specialization.
		SrtWalker bases(program, runtime);
		for (const auto& [low, high]: program.address_bases) {
			uint32_t low_value  = 0;
			uint32_t high_value = 0;
			if (bases.Evaluate(low, low_value) && bases.Evaluate(high, high_value)) {
				snapshot.address_bases.push_back(
				    (uint64_t {high_value} << 32u | low_value) & 0xffffffffffffull);
			}
		}
	}
	return BuildResourceSpecialization(program, previous_images, snapshot, specialization);
}

void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization) {
	EXIT_IF(!program.resource_tracking_complete || program.shader_info_complete);
	EXIT_IF(program.info.buffers.size() > specialization.buffers.size() ||
	        program.info.images.size() > specialization.images.size());

	auto& buffers = program.info.buffers;
	const auto original_buffer_count = buffers.size();
	buffers.reserve(specialization.buffers.size());
	for (uint32_t index = 0; index < specialization.buffers.size(); ++index) {
		const auto& source = specialization.buffers[index];
		if (index >= buffers.size()) {
			EXIT_IF(source.indirect_root >= original_buffer_count);
			buffers.push_back(buffers[source.indirect_root]);
		}
		auto& buffer                      = buffers[index];
		buffer.packed_stride              = source.packed_stride;
		buffer.descriptor_format          = source.descriptor_format;
		buffer.descriptor_swizzle         = source.descriptor_swizzle;
		buffer.indirect_root              = source.indirect_root;
		buffer.indirect_mapping_offset    = source.indirect_mapping_offset;
		buffer.indirect_search_iterations = source.indirect_search_iterations;
		buffer.indirect_resources.clear();
	}
	for (uint32_t index = 0; index < buffers.size(); ++index) {
		const auto root = buffers[index].indirect_root;
		if (root != BufferResource::NoIndirectBuffer) {
			EXIT_IF(root >= buffers.size());
			buffers[root].indirect_resources.push_back(index);
		}
	}
	auto& images = program.info.images;
	const auto original_image_count = images.size();
	images.reserve(specialization.images.size());
	for (uint32_t index = 0; index < specialization.images.size(); index++) {
		const auto& source = specialization.images[index];
		if (index >= images.size()) {
			EXIT_IF(source.indirect_root >= original_image_count);
			images.push_back(images[source.indirect_root]);
		}
		auto& image                      = images[index];
		image.numeric_class              = source.numeric_class;
		image.dimension                  = source.dimension;
		image.mip_count                  = source.mip_count;
		image.conversion_format          = source.conversion_format;
		image.shader_swizzle             = source.shader_swizzle;
		image.indirect_root              = source.indirect_root;
		image.indirect_mapping_offset    = source.indirect_mapping_offset;
		image.indirect_search_iterations = source.indirect_search_iterations;
		image.cube                       = source.cube;
		image.speculative                = source.speculative;
		image.indirect_resources.clear();
	}
	for (uint32_t index = 0; index < images.size(); index++) {
		const auto root = images[index].indirect_root;
		if (root != ImageResource::NoIndirectImage) {
			EXIT_IF(root >= images.size());
			images[root].indirect_resources.push_back(index);
		}
	}

	SamplerPlan sampler_plan;
	EXIT_IF(!BuildSamplerPlan(program.info, sampler_plan));
	auto& samplers      = program.info.samplers;
	auto& sampled_pairs = program.info.sampled_pairs;
	const auto original_sampler_count = samplers.size();
	samplers.reserve(sampler_plan.sampler_count);
	for (uint32_t index = 0; index < sampler_plan.sampler_count; index++) {
		const auto& binding = sampler_plan.bindings[index];
		if (index >= original_sampler_count) {
			samplers.push_back(samplers[binding.source]);
		}
		samplers[index].snapshot_index = binding.source;
		samplers[index].force_point_filtering = binding.type == SamplerClass::PointInteger;
		samplers[index].integer_border        = binding.type != SamplerClass::Float;
	}
	for (uint32_t index = 0; index < sampler_plan.sampler_count; index++) {
		const auto& binding = sampler_plan.bindings[index];
		for (auto& candidate: samplers[index].indirect_resources) {
			candidate = sampler_plan.mapping[candidate][static_cast<uint32_t>(binding.type)];
			EXIT_IF(candidate == UINT32_MAX);
		}
	}
	for (auto& pair: sampled_pairs) {
		const auto type = static_cast<uint32_t>(ClassifySampler(images[pair.image]));
		pair.sampler = sampler_plan.mapping[pair.sampler][type];
		EXIT_IF(pair.sampler == UINT32_MAX);
		samplers[pair.sampler].depth_compare |= images[pair.image].depth_compare;
		for (const auto candidate: samplers[pair.sampler].indirect_resources)
			samplers[candidate].depth_compare |= images[pair.image].depth_compare;
	}

	auto& memory_info = program.memory_info;
	const ImageRemap image_remap(specialization);
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto& inst = *it;
			if (inst.GetOpcode() == ValueOpcode::GetImageResource) {
				inst.SetFlags(image_remap[inst.Flags<uint32_t>()]);
				continue;
			}
			if (BufferAccessOf(inst.GetOpcode()) == BufferAccess::Read) {
				const auto& memory = memory_info[inst.Flags<MemoryFlags>().index];
				if (memory.kind == ResourceKind::Buffer &&
				    specialization.buffers[memory.resource].zero_stride_oob) {
					// Bounds mode 0 checks offset >= stride, so zero stride
					// makes every vector read out of bounds regardless of its address.
					const auto count = BufferComponentCount(inst.GetOpcode());
					std::array<Value, 4> values {Value(0u), Value(0u), Value(0u), Value(0u)};
					if (memory.formatted && !memory.typed) {
						const auto& buffer = buffers[memory.resource];
						const auto format = Format::GetFormatInfo(buffer.descriptor_format);
						for (uint32_t component = 0; component < count; component++) {
							if (format.type == Format::ComponentType::Unknown ||
							    GetDstSel(buffer.descriptor_swizzle, component) != 1u) continue;
							const auto one = Format::FormattedConstantBits(
							    format, Format::FormattedSourceKind::One);
							values[component] = Value(&*block->PrependNewInst(
							    it, ValueOpcode::SelectU32,
							    {inst.Arg(inst.NumArgs() - 1), Value(one), Value(0u)}));
						}
					}
					Value result = values[0];
					switch (inst.GetType()) {
						case Type::U8: result = Value(uint8_t {0}); break;
						case Type::U16: result = Value(uint16_t {0}); break;
						case Type::U32x2:
							result = Value(&*block->PrependNewInst(it, ValueOpcode::CompositeConstructU32x2,
							                                      {values[0], values[1]}));
							break;
						case Type::U32x3:
							result = Value(&*block->PrependNewInst(it, ValueOpcode::CompositeConstructU32x3,
							                                      {values[0], values[1], values[2]}));
							break;
						case Type::U32x4:
							result = Value(&*block->PrependNewInst(it, ValueOpcode::CompositeConstructU32x4,
							                                      {values[0], values[1], values[2], values[3]}));
							break;
						default: break;
					}
					inst.ReplaceUsesWith(result);
				}
				continue;
			}
			const auto image_opcode = ImageOpcodeInfoOf(inst.GetOpcode());
			if (image_opcode.access == ImageAccess::None) {
				continue;
			}
			const auto index = inst.Flags<MemoryFlags>().index;
			EXIT_IF(index >= memory_info.size());
			auto& memory = memory_info[index];
			EXIT_IF(memory.resource >= images.size());
			const auto& image = images[memory.resource];
			if (specialization.images[memory.resource].fmask) {
				EXIT_IF(inst.GetOpcode() != ValueOpcode::ImageRead || memory.data_bits != 32u);
				// Vulkan MSAA stores each sample directly; FMASK's four-bit fragment indices
				// therefore map each coverage sample to the same host sample.
				constexpr uint32_t indices[] = {0x76543210u, 0xfedcba98u};
				std::array<Value, 2> fragments;
				for (uint32_t component = 0; component < fragments.size(); component++) {
					const auto selected = block->PrependNewInst(
					    it, ValueOpcode::SelectU32, {inst.Arg(2), Value(indices[component]), Value(0u)});
					fragments[component] = Value(&*selected);
				}
				const auto result = block->PrependNewInst(
				    it, ValueOpcode::CompositeConstructU32x4,
				    {fragments[0], fragments[1], Value(0u), Value(0u)});
				inst.ReplaceUsesWith(Value(&*result));
				continue;
			}
			if (image_opcode.needs_sampler &&
			    memory.sampler < original_sampler_count) {
				const auto type = static_cast<uint32_t>(ClassifySampler(image));
				memory.sampler = sampler_plan.mapping[memory.sampler][type];
				EXIT_IF(memory.sampler == UINT32_MAX);
			}
			EXIT_IF(image.indirect_root == memory.resource &&
			        inst.GetOpcode() != ValueOpcode::ImageSampleRaw &&
			        inst.GetOpcode() != ValueOpcode::ImageGatherRaw &&
			        inst.GetOpcode() != ValueOpcode::ImageRead);
		}
	}
	for (auto& memory: memory_info) {
		if (memory.kind == ResourceKind::Image && !memory.planning_only) {
			memory.resource = image_remap[memory.resource];
		}
	}
	for (auto& buffer: buffers) {
		if (buffer.image_alias != BufferResource::NoImageAlias) {
			buffer.image_alias = image_remap[buffer.image_alias];
		}
	}
	for (auto& pair: sampled_pairs) {
		pair.image = image_remap[pair.image];
	}
	for (auto& image: images) {
		if (image.indirect_root != ImageResource::NoIndirectImage) {
			image.indirect_root = image_remap[image.indirect_root];
		}
		for (auto& resource: image.indirect_resources) {
			resource = image_remap[resource];
		}
	}
	image_remap.Apply(images);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
