#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <map>
#include <numeric>
#include <tuple>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask            = 0x0000ffffffffffffull;
constexpr uint64_t MaxIndirectImageProbes = 65536u;

void SortUniqueOffsets(std::vector<uint32_t>& values) {
	std::ranges::sort(values);
	values.erase(std::unique(values.begin(), values.end()), values.end());
}

struct IndirectDescriptorTable {
	uint32_t                     resource = 0;
	std::vector<uint32_t>        keys;
	std::vector<uint32_t>        candidates;
	std::vector<DescriptorValue> descriptors;
};

struct MaterializedSnapshot {
	ResourceSnapshot                     resources;
	std::vector<IndirectDescriptorTable> indirect_images;
	std::vector<IndirectDescriptorTable> indirect_buffers;
};

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
	// DST_SEL encodings 2 and 3 are reserved. Speculative table candidates may
	// point at non-image data and must not reach native image-view creation.
	for (uint32_t channel = 0; channel < 4; ++channel) {
		const auto select = (descriptor.dwords[3] >> (channel * 3u)) & 7u;
		if (select == 2u || select == 3u) return false;
	}
	if (!r128) {
		// RDNA2 ISA 8.2.6: word 4 has depth[12:0], pitch[13], and base_array[28:16].
		// Reserved bits must be zero. Finite table enumeration can encounter adjacent
		// non-image data; accepting its address bits here creates invalid host images.
		if ((descriptor.dwords[4] & 0xe000c000u) != 0u ||
		    (descriptor.dwords[6] & 0x00007800u) != 0u) {
			return false;
		}
		const bool array = type == Prospero::ImageType::kColor1DArray ||
		                   type == Prospero::ImageType::kColor2DArray ||
		                   type == Prospero::ImageType::kColor2DMsaaArray ||
		                   type == Prospero::ImageType::kCube;
		if (array && ((descriptor.dwords[4] >> 16u) & 0x1fffu) > (descriptor.dwords[4] & 0x1fffu)) {
			return false;
		}
	}
	if (type == Prospero::ImageType::kColor2DMsaa ||
	    type == Prospero::ImageType::kColor2DMsaaArray) {
		const auto base_level = (descriptor.dwords[3] >> 12u) & 0xfu;
		const auto fragments  = (descriptor.dwords[3] >> 16u) & 0xfu;
		const auto max_mip    = (descriptor.dwords[5] >> 4u) & 0xfu;
		return base_level == 0 && fragments >= 1 && fragments <= 3 &&
		       (r128 || max_mip == fragments);
	}
	const auto base_level = (descriptor.dwords[3] >> 12u) & 15u;
	const auto last_level = (descriptor.dwords[3] >> 16u) & 15u;
	const auto max_mip    = (descriptor.dwords[5] >> 4u) & 15u;
	return base_level <= last_level && (r128 || base_level <= max_mip);
}

uint32_t DescriptorImageSwizzle(const DescriptorValue& descriptor) {
	return descriptor.dwords[3] & 0xfffu;
}

Prospero::BufferFormat ImageConversionFormat(Prospero::BufferFormat format) {
	return Prospero::RemapTextureFormat(format) != format ? format
	                                                      : Prospero::BufferFormat::kInvalid;
}

bool RequiresPointSampler(const ImageResource& image) {
	return image.numeric_class == Prospero::TextureNumericClass::Sint ||
	       image.numeric_class == Prospero::TextureNumericClass::Uint ||
	       image.conversion_format != Prospero::BufferFormat::kInvalid;
}

bool RequiresPointSampler(const ResourceSpecialization::Image& image) {
	return image.numeric_class == Prospero::TextureNumericClass::Sint ||
	       image.numeric_class == Prospero::TextureNumericClass::Uint ||
	       image.conversion_format != Prospero::BufferFormat::kInvalid;
}

bool DescriptorIsCube(const DescriptorValue& descriptor) {
	return static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu) ==
	       Prospero::ImageType::kCube;
}

uint32_t StorageMipCount(const ImageResource& image, const DescriptorValue& descriptor) {
	if (image.mip_mode != ImageMipMode::DynamicStorage || NullImageDescriptor(descriptor)) {
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

void MarkCleanFlatSlots(const ResourcePlan& program, const DescriptorSource* source,
                        std::span<uint8_t> slots) {
	if (source == nullptr) {
		return;
	}
	std::vector<Value>       pending(source->dwords.begin(),
	                                 source->dwords.begin() + source->dword_count);
	std::vector<const Inst*> visited;
	while (!pending.empty()) {
		auto value = pending.back().Resolve();
		pending.pop_back();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || std::ranges::find(visited, inst) != visited.end()) {
			continue;
		}
		visited.push_back(inst);
		if (inst->GetOpcode() == ValueOpcode::ReadConst) {
			const auto slot = inst->Arg(1).Resolve();
			if (slot.IsImmediate() && slot.GetType() == Type::U32 && slot.U32() < slots.size()) {
				slots[slot.U32()] = 1u;
				pending.push_back(program.srt_reads[slot.U32()].value);
			}
			continue;
		}
		for (size_t arg = 0; arg < inst->NumArgs(); arg++) {
			pending.push_back(inst->Arg(arg));
		}
	}
}

uint64_t ScalarBufferSize(const ShaderBufferResource& descriptor) {
	return descriptor.Stride() == 0u
	           ? descriptor.NumRecords()
	           : static_cast<uint64_t>(descriptor.Stride()) * descriptor.NumRecords();
}

bool ReadSpecializationWord(const SrtRuntime& runtime, uint64_t address, uint32_t& word) {
	return runtime.read_specialization_memory != nullptr &&
	       runtime.read_specialization_memory(runtime.userdata, address, &word);
}

// Enumerate a conservative, finite set of GPU-selected table offsets. Memory used to
// narrow the set must be CPU-known for this dispatch, just like the descriptors themselves.
bool ReadScalarBufferWord(const ShaderBufferResource& descriptor, uint32_t dynamic_offset,
                          uint32_t immediate_offset, const SrtRuntime& runtime, uint32_t& word);

class BufferOffsets {
public:
	BufferOffsets(const ResourcePlan& program, const SrtRuntime& runtime,
	              std::span<const DescriptorSource::IndexRange> ranges = {})
	    : m_program(program), m_runtime(runtime), m_ranges(ranges) {
		m_runtime.read_memory = runtime.read_specialization_memory;
	}

	bool EvaluateDispatch(Value value, std::vector<uint32_t>& out) {
		if (EvaluateCountedRows(value, out, true)) return true;
		// Most masked selectors already have a small conservative domain. Only
		// partition by workgroup when that domain cannot be resolved as a whole.

		const bool finite = Evaluate(value, out);

		if (finite) return true;
		if (EvaluateCountedRows(value, out)) return true;
		// Keep a tile's address and its loop bound correlated. Combining the bounds
		// of different workgroups would also read unused slots in each tile's list.
		std::array<bool, 3>             axes {};
		std::vector<Value>              pending {value};
		std::unordered_set<const Inst*> visited;
		for (const auto& range: m_ranges) {
			pending.push_back(range.begin);
			pending.push_back(range.end);
		}
		while (!pending.empty()) {
			const auto* inst = pending.back().Resolve().TryInstruction();
			pending.pop_back();
			if (inst == nullptr || !visited.insert(inst).second) continue;
			if (inst->GetOpcode() == ValueOpcode::GetBuiltin &&
			    inst->Arg(0) == Value(static_cast<uint32_t>(StageInputKind::WorkgroupId))) {
				const auto axis = inst->Arg(1).Resolve();
				if (!axis.IsImmediate() || axis.U32() >= axes.size()) return false;
				axes[axis.U32()] = true;
			}
			// GPU-only operations are evaluated through a finite mask or a buffer
			// domain fallback. Their own operands do not specialize that domain.
			switch (inst->GetOpcode()) {
				case ValueOpcode::Phi:
				case ValueOpcode::ReadFirstLane:
				case ValueOpcode::SelectU32:
				case ValueOpcode::ReadConstBuffer:
				case ValueOpcode::LoadAddressU32:
				case ValueOpcode::IAdd32:
				case ValueOpcode::ISub32:
				case ValueOpcode::IMul32:
				case ValueOpcode::BitwiseAnd32:
				case ValueOpcode::BitwiseOr32:
				case ValueOpcode::ShiftLeftLogical32:
				case ValueOpcode::ShiftRightLogical32:
				case ValueOpcode::BitFieldUExtract:
				case ValueOpcode::LogicalNot:
				case ValueOpcode::LogicalAnd:
				case ValueOpcode::LogicalOr:
				case ValueOpcode::IEqual32:
				case ValueOpcode::INotEqual32:
				case ValueOpcode::ULessThan32:
				case ValueOpcode::UGreaterThan32: break;
				default: continue;
			}
			for (size_t i = 0; i < inst->NumArgs(); ++i)
				pending.push_back(inst->Arg(i));
		}
		if (std::ranges::none_of(axes, [](bool used) { return used; })) return false;
		std::array<uint32_t, 3> extent {1, 1, 1};
		uint64_t                groups = 1;
		for (size_t axis = 0; axis < axes.size(); ++axis) {
			if (!axes[axis]) continue;
			extent[axis] = m_runtime.workgroup_count[axis];
			if (extent[axis] == 0 || extent[axis] > MaxValues || groups > MaxValues / extent[axis])
				return false;
			groups *= extent[axis];
		}
		std::unordered_set<uint32_t> keys;

		for (uint32_t z = 0; z < extent[2]; ++z) {
			for (uint32_t y = 0; y < extent[1]; ++y) {
				for (uint32_t x = 0; x < extent[0]; ++x) {
					BufferOffsets group(m_program, m_runtime, m_ranges);
					group.m_invariants = m_invariants;
					group.m_workgroup  = std::array {x, y, z};
					std::vector<uint32_t> values;
					if (!group.Evaluate(value, values)) return false;
					keys.insert(values.begin(), values.end());
					if (keys.size() > MaxValues) return false;
				}
			}
		}
		out.assign(keys.begin(), keys.end());
		std::ranges::sort(out);

		return true;
	}

	bool Evaluate(Value value, std::vector<uint32_t>& out) {
		value = value.Resolve();
		if (value.GetType() != Type::U32) {
			return false;
		}
		if (value.IsImmediate()) {
			out = {value.U32()};
			return true;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || m_visiting.size() >= 128 || m_visiting.contains(inst)) {
			return false;
		}
		if (const auto assigned = m_assigned.find(inst); assigned != m_assigned.end()) {
			out = assigned->second;
			return true;
		}
		if (inst == m_row_alias && m_active_mask == m_row_mask) {
			out = m_assigned.at(m_bound_row);
			return true;
		}
		if (const auto it = m_cache.find(inst); it != m_cache.end()) {
			if (!it->second) return false;
			out = *it->second;
			return true;
		}
		m_visiting.insert(inst);
		uint32_t   word  = 0;
		bool       valid = false;
		const auto range = std::ranges::find_if(
		    m_ranges, [&](const auto& candidate) { return candidate.value.Resolve() == value; });
		if (range != m_ranges.end()) {
			std::vector<uint32_t> begins, ends;
			valid = Evaluate(range->begin, begins) && Evaluate(range->end, ends);
			for (const auto& [bound, limit]: range->bound_limits) {
				std::vector<uint32_t> values;
				if (!valid || !Evaluate(bound, values) || values.empty() ||
				    *std::ranges::max_element(values) > limit) {
					valid = false;
					break;
				}
			}
			if (valid && !begins.empty() && !ends.empty()) {
				const auto begin = *std::ranges::min_element(begins);
				const auto end   = *std::ranges::max_element(ends);
				valid            = begin <= INT32_MAX && end <= INT32_MAX &&
				                   (end <= begin || end - begin <= MaxValues);
				if (valid) {
					out.resize(end > begin ? end - begin : 0u);
					std::iota(out.begin(), out.end(), begin);
				}
			} else if (valid) {
				out.clear();
			}
		} else if (IsUniform(value)) {
			const auto found = m_invariants->values.find(inst);
			if (found != m_invariants->values.end()) {
				valid = found->second.has_value();
				if (valid) word = *found->second;
			} else {
				valid = EvaluateUniformValues(m_program, std::span(&value, 1), m_runtime,
				                              std::span(&word, 1), &m_invariants->uniform_values);
				m_invariants->values.emplace(inst, valid ? std::optional {word} : std::nullopt);
			}
			if (valid) {
				out = {word};
			}
		} else {
			valid = EvaluateInst(*inst, out);
			if (!valid && !m_read_failed) {
				const auto mask = PossibleBits(value, 0);
				if (std::popcount(mask) <= 10) {
					out.clear();
					uint32_t bits = mask;
					do {
						out.push_back(bits);
						bits = (bits - 1u) & mask;
					} while (bits != mask);
					valid = true;
				}
			}
		}
		m_visiting.erase(inst);
		if (valid) {
			SortUniqueOffsets(out);
			valid = out.size() <= MaxValues;
		}
		m_cache[inst] = valid ? std::optional {out} : std::nullopt;
		return valid;
	}

private:
	static constexpr size_t MaxValues = 65536;
	bool EvaluateCountedRows(Value value, std::vector<uint32_t>& out, bool preferred = false) {
		if (m_counted_attempted) return false;
		// A zero count prevents the guarded resource access. Enumerate only rows
		// whose count is in bounds, keeping each row and its count correlated.
		if (m_ranges.size() != 1 || m_ranges[0].begin.Resolve() != Value(0u)) return false;
		const auto& range = m_ranges[0];
		const auto* count = range.end.Resolve().TryInstruction();
		if (count == nullptr || count->GetOpcode() != ValueOpcode::ReadConstBuffer) return false;
		const auto* offset = count->Arg(1).Resolve().TryInstruction();
		if (offset == nullptr || offset->GetOpcode() != ValueOpcode::ShiftLeftLogical32 ||
		    offset->Arg(1).Resolve() != Value(2u))
			return false;
		const auto row = offset->Arg(0).Resolve();
		if (row.TryInstruction() == nullptr || IsUniform(row)) return false;
		const auto row_op = row.TryInstruction()->GetOpcode();
		if (preferred && row_op != ValueOpcode::ReadFirstLane && row_op != ValueOpcode::ReadLane)
			return false;
		m_counted_attempted = true;
		Value row_alias, row_mask;
		if (row.TryInstruction()->GetOpcode() == ValueOpcode::ReadFirstLane) {
			row_alias = row.TryInstruction()->Arg(0).Resolve();
			row_mask  = row.TryInstruction()->Arg(1).Resolve();
			if (row_alias.TryInstruction() == nullptr) return false;
		}
		const auto memory_index = count->Flags<MemoryFlags>().index;
		if (memory_index >= m_program.memory_info.size()) return false;
		const auto& memory = m_program.memory_info[memory_index];
		if (memory.kind != ResourceKind::ScalarBuffer || memory.offset != 0) return false;
		const auto* handle = count->Arg(0).Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetBufferResource)
			return false;
		const std::array     roots {handle->Arg(0), handle->Arg(1), handle->Arg(2), handle->Arg(3)};
		ShaderBufferResource descriptor;
		if (!std::ranges::all_of(roots, [&](Value root) { return IsUniform(root); }) ||
		    !EvaluateUniformValues(m_program, roots, m_runtime, descriptor.fields,
		                           &m_invariants->uniform_values))
			return false;
		const auto rows = ScalarBufferSize(descriptor) / sizeof(uint32_t);
		if (rows > MaxValues) return false;
		// The output must require a loop index, so an out-of-bounds zero count
		// cannot contribute a resource through an unrelated select/phi branch.
		std::unordered_set<const Inst*> active;
		const auto requires_index = [&](auto&& self, Value input, Value mask) -> bool {
			input = input.Resolve();
			if (input == range.value.Resolve()) return true;
			const auto* inst = input.TryInstruction();
			if (inst == nullptr || !active.insert(inst).second) return false;
			bool required = false;
			switch (inst->GetOpcode()) {
				case ValueOpcode::ReadFirstLane:
					required = self(self, inst->Arg(0), inst->Arg(1).Resolve());
					break;
				case ValueOpcode::SelectU32:
					if (!mask.IsEmpty() && inst->Arg(0).Resolve() == mask)
						required = self(self, inst->Arg(1), mask);
					break;
				case ValueOpcode::ReadConstBuffer:
				case ValueOpcode::LoadAddressU32: required = self(self, inst->Arg(1), mask); break;
				case ValueOpcode::IAdd32:
				case ValueOpcode::ISub32:
				case ValueOpcode::IMul32:
				case ValueOpcode::BitwiseAnd32:
				case ValueOpcode::BitwiseOr32:
				case ValueOpcode::ShiftLeftLogical32:
				case ValueOpcode::ShiftRightLogical32:
					required = self(self, inst->Arg(0), mask) || self(self, inst->Arg(1), mask);
					break;
				default: break;
			}
			active.erase(inst);
			return required;
		};
		if (!requires_index(requires_index, value, {})) return false;
		// count[row] uses a wrapping 32-bit byte offset. High two row bits
		// may be discarded only if every other use discards them as well.
		std::vector<std::pair<Value, Value>> pending {{value, {}}, {range.end, {}}};
		for (const auto& [bound, limit]: range.bound_limits)
			pending.emplace_back(bound, Value {});
		std::unordered_map<const Inst*, std::vector<Value>> visited;
		while (!pending.empty()) {
			const auto [input_value, mask] = pending.back();
			const auto input               = input_value.Resolve();
			pending.pop_back();
			if (input == range.value.Resolve()) continue;
			const auto* inst = input.TryInstruction();
			if (inst == nullptr) continue;
			auto& contexts = visited[inst];
			if (std::ranges::find(contexts, mask) != contexts.end()) continue;
			contexts.push_back(mask);
			if (inst->GetOpcode() == ValueOpcode::ReadFirstLane) {
				pending.emplace_back(inst->Arg(0), inst->Arg(1).Resolve());
				continue;
			}
			if (inst->GetOpcode() == ValueOpcode::SelectU32 && !mask.IsEmpty() &&
			    inst->Arg(0).Resolve() == mask) {
				pending.emplace_back(inst->Arg(1), mask);
				continue;
			}
			for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
				const auto source = inst->Arg(arg).Resolve();
				if (source == row || (!row_alias.IsEmpty() && source == row_alias)) {
					if (source == row_alias && mask != row_mask) return false;
					if (arg != 0 || inst->GetOpcode() != ValueOpcode::ShiftLeftLogical32)
						return false;
					const auto shift = inst->Arg(1).Resolve();
					if (!shift.IsImmediate() || (shift.U32() & 31u) < 2u) return false;
				} else {
					pending.emplace_back(inst->Arg(arg), mask);
				}
			}
		}
		std::unordered_map<uint32_t, std::vector<uint32_t>> counted_rows;
		for (uint32_t index = 0; index < rows; ++index) {
			uint32_t length = 0;
			if (!ReadScalarBufferWord(descriptor, index * 4u, 0, m_runtime, length) ||
			    length > MaxValues)
				return false;
			if (length != 0) counted_rows[length].push_back(index);
		}
		std::unordered_set<uint32_t> keys;
		// Rows with the same count can be evaluated together without mixing the
		// bounds of different lists. Bound each batch's address cross product.
		for (const auto& [length, indices]: counted_rows) {
			const auto batch_size = std::min<size_t>(256, MaxValues / length);
			for (size_t begin = 0; begin < indices.size(); begin += batch_size) {
				const auto    end = std::min(begin + batch_size, indices.size());
				BufferOffsets tile(m_program, m_runtime, m_ranges);
				tile.m_invariants = m_invariants;
				tile.m_assigned.emplace(
				    row.TryInstruction(),
				    std::vector<uint32_t>(indices.begin() + begin, indices.begin() + end));
				tile.m_assigned.emplace(count, std::vector<uint32_t> {length});
				tile.m_bound_row = row.TryInstruction();
				tile.m_row_alias = row_alias.TryInstruction();
				tile.m_row_mask  = row_mask;
				std::vector<uint32_t> values;
				if (!tile.Evaluate(value, values)) return false;
				keys.insert(values.begin(), values.end());
				// Some compute passes populate only their dispatched rows. A broad
				// table domain can include many stale entries; prefer the ordinary
				// dispatch-aware analysis when that domain ceases to be small.
				if (keys.size() > 128u) return false;
			}
		}
		out.assign(keys.begin(), keys.end());
		std::ranges::sort(out);
		return true;
	}

	struct InvariantCache {
		UniformValueCache                                        uniform_values;
		std::unordered_map<const Inst*, bool>                    uniform;
		std::unordered_map<const Inst*, std::optional<uint32_t>> values;
		std::unordered_map<const Inst*, ShaderBufferResource>    buffers;
	};
	bool IsUniform(Value value) {
		const auto* inst = value.Resolve().TryInstruction();
		if (inst == nullptr) return ValidateRuntimeValue(m_program, value);
		const auto found = m_invariants->uniform.find(inst);
		if (found != m_invariants->uniform.end()) return found->second;
		const bool uniform = ValidateRuntimeValue(m_program, value);
		m_invariants->uniform.emplace(inst, uniform);
		return uniform;
	}

	bool Implies(Value predicate, bool truth, Value wanted, bool wanted_truth,
	             uint32_t depth = 0) const {
		predicate = predicate.Resolve();
		wanted    = wanted.Resolve();
		if (predicate == wanted) {
			return truth == wanted_truth;
		}
		const auto* inst = predicate.TryInstruction();
		if (inst == nullptr || depth >= 64) {
			return false;
		}
		if (inst->GetOpcode() == ValueOpcode::LogicalNot) {
			return Implies(inst->Arg(0), !truth, wanted, wanted_truth, depth + 1);
		}
		if ((truth && inst->GetOpcode() == ValueOpcode::LogicalAnd) ||
		    (!truth && inst->GetOpcode() == ValueOpcode::LogicalOr)) {
			return Implies(inst->Arg(0), truth, wanted, wanted_truth, depth + 1) ||
			       Implies(inst->Arg(1), truth, wanted, wanted_truth, depth + 1);
		}
		return false;
	}

	bool Nonzero(Value value, Value predicate, uint32_t depth = 0) const {
		const auto* inst = predicate.Resolve().TryInstruction();
		if (inst == nullptr || depth >= 64) {
			return false;
		}
		if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
			return Nonzero(value, inst->Arg(0), depth + 1) ||
			       Nonzero(value, inst->Arg(1), depth + 1);
		}
		const auto op = inst->GetOpcode();
		return ((op == ValueOpcode::ULessThan32 || op == ValueOpcode::INotEqual32) &&
		        inst->Arg(0).Resolve() == Value(0u) && inst->Arg(1).Resolve() == value.Resolve()) ||
		       ((op == ValueOpcode::UGreaterThan32 || op == ValueOpcode::INotEqual32) &&
		        inst->Arg(1).Resolve() == Value(0u) && inst->Arg(0).Resolve() == value.Resolve());
	}

	std::optional<bool> Predicate(Value value, uint32_t depth = 0) {
		value = value.Resolve();
		if (value.IsImmediate() && value.GetType() == Type::U1) {
			return value.U1();
		}
		if (depth >= 64) {
			return {};
		}
		if (!m_active_mask.IsEmpty()) {
			if (Implies(m_active_mask, true, value, true)) return true;
			if (Implies(m_active_mask, true, value, false)) return false;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return {};
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::LogicalNot) {
			const auto arg = Predicate(inst->Arg(0), depth + 1);
			return arg ? std::optional<bool> {!*arg} : std::nullopt;
		}
		if (op == ValueOpcode::LogicalAnd || op == ValueOpcode::LogicalOr) {
			const auto a = Predicate(inst->Arg(0), depth + 1);
			const auto b = Predicate(inst->Arg(1), depth + 1);
			if (op == ValueOpcode::LogicalAnd) {
				if ((a && !*a) || (b && !*b)) return false;
				if (a && b) return *a && *b;
			} else {
				if ((a && *a) || (b && *b)) return true;
				if (a && b) return *a || *b;
			}
			return {};
		}
		if (op != ValueOpcode::IEqual32 && op != ValueOpcode::INotEqual32 &&
		    op != ValueOpcode::ULessThan32 && op != ValueOpcode::UGreaterThan32)
			return {};
		if ((op == ValueOpcode::IEqual32 || op == ValueOpcode::INotEqual32) &&
		    ((inst->Arg(0).Resolve() == Value(0u) && Nonzero(inst->Arg(1), m_active_mask)) ||
		     (inst->Arg(1).Resolve() == Value(0u) && Nonzero(inst->Arg(0), m_active_mask)))) {
			return op == ValueOpcode::INotEqual32;
		}
		std::vector<uint32_t> lhs, rhs;
		if (!Evaluate(inst->Arg(0), lhs) || !Evaluate(inst->Arg(1), rhs) ||
		    lhs.size() * rhs.size() > MaxValues)
			return {};
		std::optional<bool> result;
		for (const auto a: lhs) {
			for (const auto b: rhs) {
				const bool test = op == ValueOpcode::IEqual32      ? a == b
				                  : op == ValueOpcode::INotEqual32 ? a != b
				                  : op == ValueOpcode::ULessThan32 ? a < b
				                                                   : a > b;
				if (result && *result != test) return {};
				result = test;
			}
		}
		return result;
	}

	uint32_t PossibleBits(Value value, uint32_t depth, Value active_mask = {}) const {
		value = value.Resolve();
		if (value.IsImmediate() && value.GetType() == Type::U32) {
			return value.U32();
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || depth > 32) {
			return UINT32_MAX;
		}
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::BitFieldUExtract) {
			const auto offset = inst->Arg(1).Resolve();
			const auto width  = inst->Arg(2).Resolve();
			if (offset.IsImmediate() && width.IsImmediate() && offset.U32() <= 32u &&
			    width.U32() <= 32u - offset.U32()) {
				if (width.U32() == 0u) return 0u;
				const auto mask = width.U32() == 32u ? UINT32_MAX : (1u << width.U32()) - 1u;
				return (PossibleBits(inst->Arg(0), depth + 1, active_mask) >> offset.U32()) & mask;
			}
		}
		if (op == ValueOpcode::ReadFirstLane) {
			return PossibleBits(inst->Arg(0), depth + 1, inst->Arg(1).Resolve());
		}
		if (op == ValueOpcode::SelectU32) {
			if (!active_mask.IsEmpty() && inst->Arg(0).Resolve() == active_mask) {
				return PossibleBits(inst->Arg(1), depth + 1, active_mask);
			}
			return PossibleBits(inst->Arg(1), depth + 1, active_mask) |
			       PossibleBits(inst->Arg(2), depth + 1, active_mask);
		}
		if (op == ValueOpcode::IAdd32 || op == ValueOpcode::IMul32) {
			const auto left  = std::countr_zero(PossibleBits(inst->Arg(0), depth + 1, active_mask));
			const auto right = std::countr_zero(PossibleBits(inst->Arg(1), depth + 1, active_mask));
			const auto zeros =
			    op == ValueOpcode::IAdd32 ? std::min(left, right) : std::min(left + right, 32);
			return zeros == 32 ? 0u : UINT32_MAX << zeros;
		}
		if (op == ValueOpcode::BitwiseAnd32) {
			return PossibleBits(inst->Arg(0), depth + 1, active_mask) &
			       PossibleBits(inst->Arg(1), depth + 1, active_mask);
		}
		if (op == ValueOpcode::BitwiseOr32) {
			return PossibleBits(inst->Arg(0), depth + 1, active_mask) |
			       PossibleBits(inst->Arg(1), depth + 1, active_mask);
		}
		if (op == ValueOpcode::ShiftLeftLogical32 || op == ValueOpcode::ShiftRightLogical32) {
			const auto shift = inst->Arg(1).Resolve();
			if (shift.IsImmediate() && shift.GetType() == Type::U32) {
				const auto bits = PossibleBits(inst->Arg(0), depth + 1, active_mask);
				return op == ValueOpcode::ShiftLeftLogical32 ? bits << (shift.U32() & 31u)
				                                             : bits >> (shift.U32() & 31u);
			}
		}
		return UINT32_MAX;
	}

	bool EvaluateInst(const Inst& inst, std::vector<uint32_t>& out) {
		const auto op = inst.GetOpcode();
		if (op == ValueOpcode::BitFieldUExtract) {
			std::vector<uint32_t> values, offsets, widths;
			if (!Evaluate(inst.Arg(0), values) || !Evaluate(inst.Arg(1), offsets) ||
			    !Evaluate(inst.Arg(2), widths) || offsets.size() * widths.size() > MaxValues ||
			    values.size() * offsets.size() * widths.size() > MaxValues * 16u)
				return false;
			out.clear();
			for (const auto offset: offsets)
				for (const auto width: widths) {
					if (offset > 32u || width > 32u - offset) return false;
					const auto mask = width == 32u  ? UINT32_MAX
					                  : width == 0u ? 0u
					                                : (1u << width) - 1u;
					for (const auto value: values)
						out.push_back(width == 0u ? 0u : (value >> offset) & mask);
				}
			return true;
		}
		if (op == ValueOpcode::GetBuiltin && m_workgroup &&
		    inst.Arg(0) == Value(static_cast<uint32_t>(StageInputKind::WorkgroupId))) {
			const auto axis = inst.Arg(1).Resolve();
			if (!axis.IsImmediate() || axis.U32() >= m_workgroup->size()) return false;
			out = {(*m_workgroup)[axis.U32()]};
			return true;
		}
		if (op == ValueOpcode::ReadFirstLane) {
			if (m_lane_depth >= 16) return false;
			BufferOffsets lane(m_program, m_runtime, m_ranges);
			lane.m_invariants  = m_invariants;
			lane.m_assigned    = m_assigned;
			lane.m_bound_row   = m_bound_row;
			lane.m_row_alias   = m_row_alias;
			lane.m_row_mask    = m_row_mask;
			lane.m_workgroup   = m_workgroup;
			lane.m_active_mask = inst.Arg(1).Resolve();
			lane.m_lane_depth  = m_lane_depth + 1;
			const bool result  = lane.Evaluate(inst.Arg(0), out);
			m_read_failed |= lane.m_read_failed;
			return result;
		}
		if (op == ValueOpcode::SelectU32) {
			const auto condition = Predicate(inst.Arg(0));
			if (condition) {
				return Evaluate(inst.Arg(*condition ? 1 : 2), out);
			}
			std::vector<uint32_t> other;
			if (!Evaluate(inst.Arg(1), out) || !Evaluate(inst.Arg(2), other)) return false;
			out.insert(out.end(), other.begin(), other.end());
			return true;
		}
		if (op == ValueOpcode::FindUMsb32) {
			const auto possible = PossibleBits(inst.Arg(0), 0);
			out.clear();
			for (uint32_t bit = 0; bit < 32; ++bit) {
				if ((possible & (1u << bit)) != 0) out.push_back(bit);
			}
			if (!Nonzero(inst.Arg(0), m_active_mask)) out.push_back(UINT32_MAX);
			return true;
		}
		if (op == ValueOpcode::ReadConstBuffer) {
			const auto  index  = inst.Flags<MemoryFlags>().index;
			const auto* handle = inst.Arg(0).ResolveInstruction();
			if (index >= m_program.memory_info.size() ||
			    m_program.memory_info[index].kind != ResourceKind::ScalarBuffer ||
			    handle == nullptr || handle->GetOpcode() != ValueOpcode::GetBufferResource) {
				return false;
			}
			const std::array roots {handle->Arg(0), handle->Arg(1), handle->Arg(2), handle->Arg(3)};
			ShaderBufferResource descriptor;
			if (const auto found = m_invariants->buffers.find(handle);
			    found != m_invariants->buffers.end()) {
				descriptor = found->second;
			} else {
				if (!std::ranges::all_of(roots, [&](Value root) { return IsUniform(root); }) ||
				    !EvaluateUniformValues(m_program, roots, m_runtime, descriptor.fields,
				                           &m_invariants->uniform_values))
					return false;
				m_invariants->buffers.emplace(handle, descriptor);
			}
			std::vector<uint32_t> offsets;
			if (!Evaluate(inst.Arg(1), offsets)) {
				const auto         size          = ScalarBufferSize(descriptor) & ~uint64_t {3};
				constexpr uint64_t MaxIndexBytes = 4u * 1024u * 1024u;
				if (m_read_failed || size > MaxIndexBytes) {
					return false;
				}
				// An unknown index can select any word in the bounded index buffer. Read
				// its contents in one transaction; GPU-produced indices require a readback.
				std::vector<uint32_t> words(size / sizeof(uint32_t));
				const auto            base = descriptor.Base48() & ~uint64_t {3};
				if (size != 0 && m_runtime.read_specialization_range != nullptr) {
					if (!m_runtime.read_specialization_range(m_runtime.userdata, base, words.data(),
					                                         size)) {
						m_read_failed = true;
						return false;
					}
				} else {
					for (uint32_t word = 0; word < words.size(); ++word) {
						if (!ReadSpecializationWord(m_runtime, base + word * 4u, words[word])) {
							m_read_failed = true;
							return false;
						}
					}
				}
				out.clear();
				const auto step = uint64_t {1}
				                  << std::max(2, std::countr_zero(PossibleBits(inst.Arg(1), 0)));
				for (uint64_t offset = 0; offset < size; offset += step) {
					const auto word =
					    (offset + m_program.memory_info[index].offset) / sizeof(uint32_t);
					if (word < words.size()) {
						out.push_back(words[word]);
					}
				}
				out.push_back(0u); // Out-of-bounds scalar loads return zero.
				return true;
			}
			out.clear();
			for (const auto offset: offsets) {
				uint32_t word = 0;
				if (!ReadScalarBufferWord(descriptor, offset, m_program.memory_info[index].offset,
				                          m_runtime, word)) {
					m_read_failed = true;
					return false;
				}
				out.push_back(word);
			}
			return true;
		}
		if (op == ValueOpcode::Phi) {
			out.clear();
			for (size_t arg = 0; arg < inst.NumArgs(); ++arg) {
				std::vector<uint32_t> branch;
				if (!Evaluate(inst.Arg(arg), branch) || out.size() + branch.size() > MaxValues) {
					return false;
				}
				out.insert(out.end(), branch.begin(), branch.end());
			}
			return true;
		}
		if (op == ValueOpcode::LoadAddressU32) {
			const auto index = inst.Flags<MemoryFlags>().index;
			if (index >= m_program.memory_info.size() ||
			    m_program.memory_info[index].kind != ResourceKind::ScalarAddress) {
				return false;
			}
			const auto* base = inst.Arg(0).ResolveInstruction();
			if (base == nullptr || base->GetOpcode() != ValueOpcode::GetAddressResource) {
				return false;
			}
			const std::array        roots {base->Arg(0), base->Arg(1)};
			std::array<uint32_t, 2> address_words {};
			std::vector<uint32_t>   offsets;
			if (!ValidateRuntimeValue(m_program, roots[0]) ||
			    !ValidateRuntimeValue(m_program, roots[1]) ||
			    !EvaluateUniformValues(m_program, roots, m_runtime, address_words,
			                           &m_invariants->uniform_values) ||
			    !Evaluate(inst.Arg(1), offsets)) {
				return false;
			}
			const auto address = ((uint64_t {address_words[1]} << 32u) | address_words[0]) &
			                     (AddressMask & ~uint64_t {3});
			const auto immediate =
			    int64_t {static_cast<int32_t>(m_program.memory_info[index].offset)} & ~int64_t {3};
			out.clear();
			for (const auto offset: offsets) {
				const auto location = static_cast<int64_t>(address) + immediate + (offset & ~3u);
				uint32_t   word     = 0;
				if (location < 0 || static_cast<uint64_t>(location) > AddressMask - 3u ||
				    !ReadSpecializationWord(m_runtime, location, word)) {
					m_read_failed = true;
					return false;
				}
				out.push_back(word);
			}
			return true;
		}
		switch (op) {
			case ValueOpcode::IAdd32:
			case ValueOpcode::ISub32:
			case ValueOpcode::IMul32:
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::BitwiseOr32:
			case ValueOpcode::ShiftLeftLogical32:
			case ValueOpcode::ShiftRightLogical32: break;
			default: return false;
		}
		std::vector<uint32_t> lhs, rhs;
		if (!Evaluate(inst.Arg(0), lhs) || !Evaluate(inst.Arg(1), rhs) ||
		    lhs.size() * rhs.size() > MaxValues * 16u) {
			return false;
		}
		out.clear();
		for (const auto a: lhs) {
			for (const auto b: rhs) {
				switch (op) {
					case ValueOpcode::IAdd32: out.push_back(a + b); break;
					case ValueOpcode::ISub32: out.push_back(a - b); break;
					case ValueOpcode::IMul32: out.push_back(a * b); break;
					case ValueOpcode::BitwiseAnd32: out.push_back(a & b); break;
					case ValueOpcode::BitwiseOr32: out.push_back(a | b); break;
					case ValueOpcode::ShiftLeftLogical32: out.push_back(a << (b & 31u)); break;
					case ValueOpcode::ShiftRightLogical32: out.push_back(a >> (b & 31u)); break;
					default: return false;
				}
			}
		}
		return true;
	}

	const ResourcePlan&                                                   m_program;
	SrtRuntime                                                            m_runtime;
	std::unordered_map<const Inst*, std::vector<uint32_t>>                m_assigned;
	const Inst*                                                           m_bound_row = nullptr;
	const Inst*                                                           m_row_alias = nullptr;
	Value                                                                 m_row_mask;
	std::unordered_map<const Inst*, std::optional<std::vector<uint32_t>>> m_cache;
	std::unordered_set<const Inst*>                                       m_visiting;
	bool                                                                  m_read_failed = false;
	bool                                          m_counted_attempted                   = false;
	std::span<const DescriptorSource::IndexRange> m_ranges;
	Value                                         m_active_mask;
	uint32_t                                      m_lane_depth = 0;
	std::optional<std::array<uint32_t, 3>>        m_workgroup;
	InvariantCache                                m_invariant_storage;
	InvariantCache*                               m_invariants = &m_invariant_storage;
};

bool ReadScalarBufferWord(const ShaderBufferResource& descriptor, uint32_t dynamic_offset,
                          uint32_t immediate_offset, const SrtRuntime& runtime, uint32_t& word) {
	const auto byte_offset = static_cast<uint64_t>(dynamic_offset) + immediate_offset;
	const auto aligned     = byte_offset & ~uint64_t {3};
	const auto size        = ScalarBufferSize(descriptor);
	if (aligned > size || size - aligned < sizeof(uint32_t)) {
		word = 0;
		return true;
	}
	const auto base = descriptor.Base48() & ~uint64_t {3};
	if (aligned > AddressMask - base) {
		return false;
	}
	const auto address = base + aligned;
	if (!ReadSpecializationWord(runtime, address, word)) {
		return false;
	}
	return true;
}

bool MaterializeIndirectImage(const DescriptorSource::IndirectImage& indirect,
                              const DescriptorValue&                 material_value,
                              const DescriptorValue& heap_value, bool r128,
                              const SrtRuntime& runtime, IndirectDescriptorTable& result) {
	ShaderBufferResource material;
	ShaderBufferResource heap;
	if (!DecodeBufferDescriptor(material_value, material) ||
	    !DecodeBufferDescriptor(heap_value, heap)) {
		return false;
	}
	if (material.Stride() != indirect.selector_stride) {
		return false;
	}

	// S_BUFFER_LOAD ignores vector-buffer swizzle/add-thread fields. The shader computes the
	// record stride explicitly; enumerate every wrapped 32-bit offset that can pass bounds.
	const auto period      = uint64_t {1} << 32u;
	const auto step        = std::gcd<uint64_t>(indirect.selector_stride, period);
	const auto residue     = static_cast<uint64_t>(indirect.selector_offset) % step;
	const auto size        = ScalarBufferSize(material);
	const auto limit       = std::min<uint64_t>(UINT32_MAX, size + 3u);
	const auto probe_count = residue <= limit ? (limit - residue) / step + 1u : 0u;
	if (probe_count > MaxIndirectImageProbes) {
		return false;
	}

	std::vector<uint32_t>        keys {0u};
	std::unordered_set<uint32_t> seen {0u};
	keys.reserve(static_cast<size_t>(probe_count) + 1u);
	seen.reserve(static_cast<size_t>(probe_count) + 1u);
	for (uint64_t offset = residue; offset <= limit && probe_count != 0u; offset += step) {
		uint32_t key = 0;
		if (!ReadScalarBufferWord(material, static_cast<uint32_t>(offset), 0u, runtime, key)) {
			return false;
		}
		if (seen.insert(key).second) {
			keys.push_back(key);
		}
		if (limit - offset < step) {
			break;
		}
	}

	IndirectDescriptorTable next;
	next.keys = std::move(keys);
	next.candidates.reserve(next.keys.size());
	next.descriptors.reserve(
	    std::min(next.keys.size(), static_cast<size_t>(ShaderInfo::MaxImages)));
	for (const auto key: next.keys) {
		DescriptorValue candidate;
		candidate.dword_count  = 8u;
		const auto heap_offset = key << 5u;
		for (uint32_t dword = 0; dword < candidate.dword_count; dword++) {
			if (!ReadScalarBufferWord(heap, heap_offset, dword * sizeof(uint32_t), runtime,
			                          candidate.dwords[dword])) {
				return false;
			}
		}
		if (NullImageDescriptor(candidate) || !ValidImageDescriptor(candidate, r128)) {
			candidate.dwords.fill(0);
		}
		const auto found = std::ranges::find(next.descriptors, candidate);
		if (found == next.descriptors.end()) {
			if (next.descriptors.size() >= ShaderInfo::MaxImages) {
				return false;
			}
			next.descriptors.push_back(candidate);
			next.candidates.push_back(static_cast<uint32_t>(next.descriptors.size() - 1u));
		} else {
			next.candidates.push_back(static_cast<uint32_t>(found - next.descriptors.begin()));
		}
	}
	result = std::move(next);
	return true;
}

} // namespace

static bool MaterializeSnapshot(const ResourcePlan& program, const SrtRuntime& runtime,
                                MaterializedSnapshot& snapshot) {
	if (!program.resource_tracking_complete) {
		return false;
	}

	if (program.requires_specialization_memory && runtime.read_specialization_memory == nullptr) {
		return false;
	}
	std::vector<DescriptorValue> values;
	std::vector<uint32_t>        flattened_srt;
	std::vector<uint8_t>         active_sources;
	if (!EvaluateRuntimeSources(program, program.materialization_sources, runtime, values,
	                            flattened_srt, program.clean_flat_slots, active_sources)) {
		return false;
	}

	auto&                   next  = snapshot.resources;
	const auto&             fill  = program.uniform_fill;
	const auto              words = fill.fill.words;
	std::array<uint32_t, 4> stored {};
	if (words != 0 &&
	    EvaluateUniformValues(program, std::span(fill.values).first(words), runtime,
	                          std::span(stored).first(words)) &&
	    std::all_of(stored.begin(), stored.begin() + words,
	                [&](uint32_t value) { return value == stored[0]; })) {
		next.uniform_fill       = fill.fill;
		next.uniform_fill.value = stored[0];
	}
	auto cursor = values.begin();
	next.buffers.assign(cursor, cursor + program.info.buffers.size());
	cursor += program.info.buffers.size();
	next.flattened_srt = std::move(flattened_srt);
	for (uint32_t index = 0; index < program.info.buffers.size(); ++index) {
		const auto& buffer = program.info.buffers[index];
		const auto* source = Source(program, buffer.source);
		if (source == nullptr || !source->indirect_buffer.has_value()) {
			continue;
		}
		const auto base_words = next.buffers[index];
		next.buffers[index].dwords.fill(0);
		if (!active_sources[buffer.source]) {
			continue;
		}
		IndirectDescriptorTable table;
		table.resource = index;
		if (!BufferOffsets(program, runtime, source->indirect_buffer->index_ranges)
		         .EvaluateDispatch(source->indirect_buffer->byte_offset, table.keys)) {
			return SpecializationFail(
			    fmt::format("shader 0x{:016x} buffer {} has no finite, CPU-known descriptor table",
			                program.shader_hash, index));
		}
		// Candidate zero is a null buffer, also used for an unmatched key.
		table.descriptors.push_back(next.buffers[index]);
		const auto base = ((uint64_t {base_words.dwords[1]} << 32u) | base_words.dwords[0]) &
		                  (AddressMask & ~uint64_t {3});
		const auto immediate =
		    int64_t {static_cast<int32_t>(source->indirect_buffer->immediate_offset)} &
		    ~int64_t {3};
		for (const auto key: table.keys) {
			DescriptorValue descriptor;
			descriptor.dword_count = 4;
			const auto address     = static_cast<int64_t>(base) + immediate + (key & ~3u);
			if (address < 0 || static_cast<uint64_t>(address) > AddressMask - 15u) {
				return SpecializationFail("indirect buffer descriptor address overflow");
			}
			for (uint32_t word = 0; word < 4; ++word) {
				if (!ReadSpecializationWord(runtime, address + word * 4u,
				                            descriptor.dwords[word])) {
					return SpecializationFail(fmt::format(
					    "indirect buffer descriptor at 0x{:x} is not CPU-known", address));
				}
			}
			ShaderBufferResource decoded;
			if (!DecodeBufferDescriptor(descriptor, decoded)) {
				return false;
			}
			if (decoded.Type() != 0) {
				descriptor.dwords.fill(0);
			}
			const auto found     = std::ranges::find(table.descriptors, descriptor);
			const auto candidate = static_cast<uint32_t>(found - table.descriptors.begin());
			if (found == table.descriptors.end()) {
				table.descriptors.push_back(descriptor);
			}
			table.candidates.push_back(candidate);
		}
		if (table.descriptors.size() > 1u) {
			snapshot.indirect_buffers.push_back(std::move(table));
		}
	}
	next.images.resize(program.info.images.size());
	for (uint32_t image_index = 0; image_index < program.info.images.size(); image_index++) {
		const auto& image  = program.info.images[image_index];
		const auto* source = Source(program, image.source);
		if (source != nullptr && source->indirect_image.has_value()) {
			if (!active_sources[image.source]) {
				next.images[image_index].dword_count = 8u;
				continue;
			}
			const std::array requests {source->indirect_image->material_source,
			                           source->indirect_image->heap_source};
			SrtRuntime       clean_runtime = runtime;
			clean_runtime.read_memory      = runtime.read_specialization_memory;
			std::vector<DescriptorValue> tables;
			if (!EvaluateDescriptorSources(program, requests, clean_runtime, tables)) {
				return false;
			}
			const auto&   material = tables[0];
			const auto&   heap     = tables[1];
			IndirectDescriptorTable table;
			if (!source->indirect_image->direct_offset.IsEmpty()) {
				ShaderBufferResource descriptor;
				if (!DecodeBufferDescriptor(material, descriptor) ||
				    !BufferOffsets(program, runtime, source->indirect_image->index_ranges)
				         .EvaluateDispatch(source->indirect_image->direct_offset, table.keys)) {

					return SpecializationFail(
					    fmt::format("shader 0x{:016x}: inline image descriptor offsets are not "
					                "bounded by readable tables",
					                program.shader_hash));
				}
				DescriptorValue null_image;
				null_image.dword_count = 8;
				table.descriptors.push_back(null_image);
				for (const auto key: table.keys) {
					DescriptorValue candidate;
					candidate.dword_count = 8;
					const auto immediate  = source->indirect_image->immediate_offset;
					const auto raw_address =
					    static_cast<int64_t>(descriptor.Base48() & ~uint64_t {3}) + (key & ~3u) +
					    (int64_t {static_cast<int32_t>(immediate)} & ~int64_t {3});
					if (source->indirect_image->direct_address &&
					    (raw_address < 0 || static_cast<uint64_t>(raw_address) > AddressMask - 31u))
						return SpecializationFail("inline image descriptor address overflow");
					const auto candidate_address = static_cast<uint64_t>(raw_address);
					// Finite-set analysis may include offsets that no invocation selects. An
					// unmapped speculative entry represents an unbound descriptor; do not read
					// it on the CPU. A single known key and any mapped read failure stay strict.
					if (source->indirect_image->direct_address && table.keys.size() > 1u &&
					    runtime.is_memory_mapped != nullptr &&
					    !runtime.is_memory_mapped(runtime.userdata, candidate_address, 32u)) {
						table.candidates.push_back(0u);
						continue;
					}
					for (uint32_t word = 0; word < 8; ++word) {
						const auto base = descriptor.Base48() & ~uint64_t {3};
						const bool read =
						    source->indirect_image->direct_address
						        ? ReadSpecializationWord(runtime, candidate_address + word * 4u,
						                                 candidate.dwords[word])
						        : ReadScalarBufferWord(descriptor, key, immediate + word * 4u,
						                               runtime, candidate.dwords[word]);
						if (!read) {
							return SpecializationFail(fmt::format(
							    "shader 0x{:016x} pc=0x{:x}: image descriptor is not readable, "
							    "base=0x{:x} key=0x{:x} word={} keys={} "
							    "table={:08x},{:08x},{:08x},{:08x}",
							    program.shader_hash, image.first_use_pc, base, key, word,
							    table.keys.size(), material.dwords[0], material.dwords[1],
							    material.dwords[2], material.dwords[3]));
						}
					}
					const auto image_address =
					    ((uint64_t {candidate.dwords[1]} << 32u | candidate.dwords[0]) &
					     0xffffffffffull)
					    << 8u;
					const bool unbound =
					    table.keys.size() > 1u && runtime.is_memory_mapped != nullptr &&
					    !runtime.is_memory_mapped(runtime.userdata, image_address, 1u);
					if (unbound || NullImageDescriptor(candidate) ||
					    !ValidImageDescriptor(candidate, image.r128)) {
						candidate.dwords.fill(0);
					}
					const auto found    = std::ranges::find(table.descriptors, candidate);
					const auto selected = static_cast<uint32_t>(found - table.descriptors.begin());
					if (found == table.descriptors.end()) {
						table.descriptors.push_back(candidate);
					}
					table.candidates.push_back(selected);
				}
			} else if (!MaterializeIndirectImage(*source->indirect_image, material, heap,
			                                     image.r128, runtime, table)) {
				return false;
			}
			// A guarded loop with zero iterations has no reachable image access.
			next.images[image_index] =
			    table.descriptors[table.candidates.empty() ? 0u : table.candidates[0]];
			if (table.descriptors.size() > 1u) {
				table.resource = image_index;
				snapshot.indirect_images.push_back(std::move(table));
			}
		} else {
			auto descriptor = *cursor++;
			if (!ValidImageDescriptor(descriptor, image.r128)) {
				descriptor.dwords.fill(0);
			}
			next.images[image_index] = descriptor;
		}
	}
	next.samplers.assign(cursor, cursor + program.info.samplers.size());
	next.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	return true;
}

struct SamplerPlan {
	std::array<uint32_t, ShaderInfo::MaxSamplers> point_sampler {};
	uint32_t                                      sampler_count = 0;
};

struct ImageRemap {
	explicit ImageRemap(const ResourceSpecialization& specialization) {
		for (const auto& image: specialization.images) {
			indices.push_back(image.fmask ? UINT32_MAX : count++);
		}
	}

	template <typename T>
	void Apply(std::vector<T>& images) const {
		EXIT_IF(images.size() != indices.size());
		for (uint32_t index = 0; index < indices.size(); index++) {
			if (indices[index] != UINT32_MAX && indices[index] != index) {
				images[indices[index]] = std::move(images[index]);
			}
		}
		images.resize(count);
	}

	std::vector<uint32_t> indices;
	uint32_t              count = 0;
};

template <typename Images>
bool BuildSamplerPlan(const ShaderInfo& base, const Images& images, SamplerPlan& plan);

static bool BuildResourceSpecialization(const ResourcePlan& program, MaterializedSnapshot snapshot,
                                        ResourceSnapshot&       specialized_snapshot,
                                        ResourceSpecialization& specialization) {
	auto                   next_snapshot = std::move(snapshot.resources);
	ResourceSpecialization next_specialization;
	next_specialization.buffers.resize(program.info.buffers.size());
	for (const auto& table: snapshot.indirect_buffers) {
		if (next_specialization.buffers.size() + table.descriptors.size() - 1u >
		    ShaderInfo::MaxBuffers) {
			return SpecializationFail(
			    "indirect buffer candidates exceed the dense buffer resource limit");
		}
		for (uint32_t candidate = 1; candidate < table.descriptors.size(); ++candidate) {
			ResourceSpecialization::Buffer buffer;
			buffer.indirect_root = table.resource;
			next_specialization.buffers.push_back(buffer);
			next_snapshot.buffers.push_back(table.descriptors[candidate]);
		}
		auto& root                      = next_specialization.buffers[table.resource];
		root.indirect_root              = table.resource;
		root.indirect_mapping_offset    = static_cast<uint32_t>(next_snapshot.flattened_srt.size());
		root.indirect_search_iterations = std::bit_width(table.keys.size());
		next_snapshot.flattened_srt.push_back(static_cast<uint32_t>(table.keys.size()));
		for (uint32_t entry = 0; entry < table.keys.size(); ++entry) {
			next_snapshot.flattened_srt.push_back(table.keys[entry]);
			next_snapshot.flattened_srt.push_back(table.candidates[entry]);
		}
	}
	size_t image_count   = program.info.images.size();
	size_t mapping_words = 0;
	for (const auto& table: snapshot.indirect_images) {
		if (table.resource >= program.info.images.size() || table.descriptors.size() < 2u ||
		    image_count + table.descriptors.size() - 1u > ShaderInfo::MaxImages) {
			return SpecializationFail(
			    "indirect image candidates exceed the dense image resource limit");
		}
		image_count += table.descriptors.size() - 1u;
		mapping_words += 1u + table.keys.size() * 2u;
	}
	next_snapshot.images.reserve(image_count);
	next_snapshot.flattened_srt.reserve(next_snapshot.flattened_srt.size() + mapping_words);
	next_specialization.images.reserve(image_count);
	for (const auto& image: program.info.images) {
		next_specialization.images.push_back({
		    .numeric_class              = image.numeric_class,
		    .dimension                  = image.dimension,
		    .mip_count                  = image.mip_count,
		    .conversion_format          = image.conversion_format,
		    .shader_swizzle             = image.shader_swizzle,
		    .indirect_root              = image.indirect_root,
		    .indirect_mapping_offset    = image.indirect_mapping_offset,
		    .indirect_search_iterations = image.indirect_search_iterations,
		    .cube                       = image.cube,
		});
	}
	for (const auto& table: snapshot.indirect_images) {
		const auto root_image = next_specialization.images[table.resource];
		for (uint32_t candidate = 1; candidate < table.descriptors.size(); candidate++) {
			auto image          = root_image;
			image.indirect_root = table.resource;
			next_specialization.images.push_back(image);
			next_snapshot.images.push_back(table.descriptors[candidate]);
		}
		auto& root                      = next_specialization.images[table.resource];
		root.indirect_root              = table.resource;
		root.indirect_mapping_offset    = static_cast<uint32_t>(next_snapshot.flattened_srt.size());
		root.indirect_search_iterations = std::bit_width(table.keys.size());
		next_snapshot.flattened_srt.resize(next_snapshot.flattened_srt.size() + 1u +
		                                   table.keys.size() * 2u);
		std::vector<uint32_t> order(table.keys.size());
		std::iota(order.begin(), order.end(), 0u);
		std::ranges::sort(order, {}, [&](uint32_t index) { return table.keys[index]; });
		next_snapshot.flattened_srt[root.indirect_mapping_offset] =
		    static_cast<uint32_t>(table.keys.size());
		for (uint32_t entry = 0; entry < order.size(); entry++) {
			const auto source                   = order[entry];
			const auto offset                   = root.indirect_mapping_offset + 1u + entry * 2u;
			next_snapshot.flattened_srt[offset] = table.keys[source];
			next_snapshot.flattened_srt[offset + 1] = table.candidates[source];
		}
		next_snapshot.images[table.resource] = table.descriptors[0];
	}
	for (uint32_t i = 0; i < next_specialization.buffers.size(); i++) {
		auto&      descriptor_value = next_snapshot.buffers[i];
		auto&      buffer           = next_specialization.buffers[i];
		const auto base_index       = i < program.info.buffers.size() ? i : buffer.indirect_root;
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
		buffer.packed_stride      = packed_stride;
		buffer.descriptor_format  = program.info.buffers[base_index].formatted
		                                ? descriptor.Format()
		                                : Prospero::BufferFormat::kInvalid;
		buffer.descriptor_swizzle = program.info.buffers[base_index].formatted
		                                ? descriptor.DstSelXYZW()
		                                : DstSel(4, 5, 6, 7);
	}
	for (uint32_t i = 0; i < next_specialization.images.size(); i++) {
		const auto& descriptor = next_snapshot.images[i];
		auto&       image      = next_specialization.images[i];
		const auto  base_index = i < program.info.images.size() ? i : image.indirect_root;
		if (base_index >= program.info.images.size()) {
			return SpecializationFail(fmt::format("image resource {} has an invalid root", i));
		}
		const auto& base = program.info.images[base_index];
		if (base.resource_class == ImageResourceClass::None ||
		    (base.atomic && base.resource_class != ImageResourceClass::Storage)) {
			return SpecializationFail(fmt::format("image resource {} has an invalid class", i));
		}
		image.mip_count = StorageMipCount(base, descriptor);
		if (image.mip_count == 0u) {
			return SpecializationFail(
			    fmt::format("storage image descriptor {} has an invalid mip range", i));
		}
		if (NullImageDescriptor(descriptor)) {
			image.numeric_class = base.atomic ? Prospero::TextureNumericClass::Uint
			                                  : Prospero::TextureNumericClass::Float;
			image.dimension     = Decoder::ImageDimension::Dim2D;
			image.cube          = false;
			continue;
		}
		const auto descriptor_dimension = DescriptorDimension(descriptor, base.dimension);
		if (descriptor_dimension == Decoder::ImageDimension::Unknown) {
			return SpecializationFail(fmt::format(
			    "image descriptor {} has unsupported type {}: {:08x},{:08x},{:08x},{:08x},"
			    "{:08x},{:08x},{:08x},{:08x}",
			    i, (descriptor.dwords[3] >> 28u) & 0xfu, descriptor.dwords[0], descriptor.dwords[1],
			    descriptor.dwords[2], descriptor.dwords[3], descriptor.dwords[4],
			    descriptor.dwords[5], descriptor.dwords[6], descriptor.dwords[7]));
		}
		image.dimension = descriptor_dimension;
		image.cube      = DescriptorIsCube(descriptor);
		const auto format =
		    static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
		if (base.atomic && format != Prospero::BufferFormat::k32UInt) {
			return SpecializationFail(
			    fmt::format("atomic image descriptor {} uses unsupported format {}", i,
			                static_cast<uint32_t>(format)));
		}
		const bool storage      = base.resource_class == ImageResourceClass::Storage;
		image.fmask             = Prospero::IsFmaskTextureFormat(format);
		if (image.fmask) {
			if (storage || base.depth_compare ||
			    image.indirect_root != ImageResource::NoIndirectImage ||
			    std::ranges::any_of(program.info.sampled_pairs,
			                        [&](const auto& pair) { return pair.image == i; })) {
				return SpecializationFail("FMASK requires a direct image load");
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
				return SpecializationFail(
				    fmt::format("storage image descriptor {} uses unsupported format {}", i,
				                static_cast<uint32_t>(format)));
			}
			if (raw_sint_storage) {
				image.numeric_class = Prospero::TextureNumericClass::Uint;
			}
		} else if (image.numeric_class == Prospero::TextureNumericClass::Unsupported ||
		           (base.depth_compare &&
		            image.numeric_class != Prospero::TextureNumericClass::Float)) {
			return SpecializationFail(
			    fmt::format("sampled image descriptor {} uses unsupported format {}", i,
			                static_cast<uint32_t>(format)));
		}
	}
	for (uint32_t root_index = 0; root_index < next_specialization.images.size(); root_index++) {
		auto& root = next_specialization.images[root_index];
		if (root.indirect_root != root_index) {
			continue;
		}
		const auto key_count = root.indirect_mapping_offset < next_snapshot.flattened_srt.size()
		                           ? next_snapshot.flattened_srt[root.indirect_mapping_offset]
		                           : 0u;
		if (root.indirect_search_iterations == 0u || key_count == 0u ||
		    static_cast<size_t>(root.indirect_mapping_offset) + 1u +
		            static_cast<size_t>(key_count) * 2u >
		        next_snapshot.flattened_srt.size()) {
			return SpecializationFail("indirect image specialization has an invalid key mapping");
		}
		uint32_t exemplar       = ImageResource::NoIndirectImage;
		uint32_t resource_count = 0;
		for (uint32_t resource = 0; resource < next_specialization.images.size(); resource++) {
			if (next_specialization.images[resource].indirect_root != root_index) {
				continue;
			}
			resource_count++;
			if (exemplar == ImageResource::NoIndirectImage &&
			    !NullImageDescriptor(next_snapshot.images[resource])) {
				exemplar = resource;
			}
		}
		if (resource_count < 2u || exemplar == ImageResource::NoIndirectImage) {
			return SpecializationFail("indirect image specialization has no typed candidate");
		}
		const auto& image_class = next_specialization.images[exemplar];
		for (uint32_t candidate = 0; candidate < next_specialization.images.size(); candidate++) {
			auto& image = next_specialization.images[candidate];
			if (image.indirect_root != root_index) {
				continue;
			}
			if (NullImageDescriptor(next_snapshot.images[candidate])) {
				image.numeric_class     = image_class.numeric_class;
				image.dimension         = image_class.dimension;
				image.mip_count         = image_class.mip_count;
				image.conversion_format = image_class.conversion_format;
				image.shader_swizzle    = image_class.shader_swizzle;
				image.cube              = image_class.cube;
			}
		}
	}
	SamplerPlan sampler_plan;
	if (!BuildSamplerPlan(program.info, next_specialization.images, sampler_plan)) {
		return SpecializationFail("specialized sampler layout exceeds its resource limit");
	}
	for (uint32_t index = 0; index < program.info.samplers.size(); index++) {
		const auto target = sampler_plan.point_sampler[index];
		if (target != UINT32_MAX && target >= program.info.samplers.size()) {
			next_snapshot.samplers.push_back(next_snapshot.samplers[index]);
		}
	}
	ImageRemap(next_specialization).Apply(next_snapshot.images);
	specialization       = std::move(next_specialization);
	specialized_snapshot = std::move(next_snapshot);
	return true;
}

template <typename Images>
bool BuildSamplerPlan(const ShaderInfo& base, const Images& images, SamplerPlan& plan) {
	if (base.samplers.size() > plan.point_sampler.size()) {
		return false;
	}
	std::array<uint8_t, ShaderInfo::MaxSamplers> usage {};
	plan.point_sampler.fill(UINT32_MAX);
	plan.sampler_count = static_cast<uint32_t>(base.samplers.size());
	for (const auto& pair: base.sampled_pairs) {
		if (pair.image >= images.size() || pair.sampler >= base.samplers.size()) {
			return false;
		}
		usage[pair.sampler] |= RequiresPointSampler(images[pair.image]) ? 2u : 1u;
		if (images[pair.image].indirect_root == pair.image) {
			for (const auto& candidate: images) {
				if (candidate.indirect_root == pair.image) {
					usage[pair.sampler] |= RequiresPointSampler(candidate) ? 2u : 1u;
				}
			}
		}
	}
	for (uint32_t index = 0; index < base.samplers.size(); index++) {
		if ((usage[index] & 2u) == 0u) {
			continue;
		}
		if ((usage[index] & 1u) == 0u) {
			plan.point_sampler[index] = index;
		} else {
			if (plan.sampler_count >= ShaderInfo::MaxSamplers) {
				return false;
			}
			plan.point_sampler[index] = plan.sampler_count++;
		}
	}
	return true;
}

static std::vector<ResourceBlock> ResourceControlFlow(const Program& program) {
	if (program.blocks.size() != program.block_info.size()) {
		return {};
	}
	std::unordered_map<uint32_t, uint32_t> indices;
	for (uint32_t i = 0; i < program.block_info.size(); i++) {
		if (!indices.emplace(program.block_info[i].id, i).second) {
			return {};
		}
	}
	std::vector<ResourceBlock> blocks(program.blocks.size());
	std::vector<uint8_t>       predicate_may_be_written(blocks.size());
	for (uint32_t i = 0; i < blocks.size(); i++) {
		auto&                 block      = blocks[i];
		const auto&           info       = program.block_info[i];
		const auto&           terminator = info.terminator;
		std::vector<uint32_t> successors;
		switch (terminator.kind) {
			case CFG::TerminatorKind::Branch: successors.push_back(terminator.true_block); break;
			case CFG::TerminatorKind::ConditionalBranch:
				successors = {terminator.true_block, terminator.false_block};
				if (ValidateRuntimeValue(program, info.condition, RuntimeValueType::Integer)) {
					block.condition = info.condition;
				}
				break;
			case CFG::TerminatorKind::IndirectBranch:
				successors = terminator.indirect_targets;
				break;
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
				const auto slot = inst.Arg(1).Resolve();
				if (slot.IsImmediate() && slot.GetType() == Type::U32 &&
				    slot.U32() < program.srt_reads.size()) {
					block.flat_slots.push_back(slot.U32());
				}
			}
			const auto buffer = BufferAccessOf(op);
			const auto image  = ImageOpcodeInfoOf(op);
			// Only predicates reachable after a shader write can observe its result.
			// Earlier uniform branches still determine which descriptors are used.
			if (buffer == BufferAccess::Write || buffer == BufferAccess::Atomic ||
			    image.access == ImageAccess::Write || image.access == ImageAccess::Atomic ||
			    AddressOpcodeInfoOf(op).access == AddressAccess::Write) {
				predicate_may_be_written[i] = 1u;
			}
			if (buffer == BufferAccess::None && image.access == ImageAccess::None) {
				continue;
			}
			const auto& memory = program.memory_info.at(inst.Flags<MemoryFlags>().index);
			if (memory.planning_only) {
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
	// Include back edges: a predicate before a write in the first iteration may
	// depend on that write on a subsequent visit. Unknown predicates retain both edges.
	std::vector<uint32_t> pending;
	for (uint32_t i = 0; i < blocks.size(); ++i) {
		if (predicate_may_be_written[i]) pending.push_back(i);
	}
	while (!pending.empty()) {
		const auto index = pending.back();
		pending.pop_back();
		blocks[index].condition = {};
		for (const auto successor: blocks[index].successors) {
			if (!predicate_may_be_written[successor]) {
				predicate_may_be_written[successor] = 1u;
				pending.push_back(successor);
			}
		}
	}
	if (std::ranges::none_of(
	        blocks, [](const ResourceBlock& block) { return !block.condition.IsEmpty(); })) {
		return {};
	}
	return blocks;
}

// Nonnegative affine coefficients for constant, local and workgroup coordinates. Reject modular
// arithmetic that could wrap; runtime coverage also bounds the largest invocation index.
static std::optional<std::array<uint64_t, 3>> FillIndex(Value value, uint32_t axis,
                                                      uint32_t depth = 0) {
	value = value.Resolve();
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
	auto left  = FillIndex(inst->Arg(0), axis, depth + 1);
	auto right = FillIndex(inst->Arg(1), axis, depth + 1);
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

static UniformFillPlan AnalyzeUniformFill(const Program& program) {
	if (program.stage != ShaderType::Compute || program.blocks.empty() ||
	    program.blocks.size() != program.block_info.size() || program.info.uses_dma ||
	    !program.info.samplers.empty()) {
		return {};
	}
	std::unordered_set<uint32_t> visited;
	uint32_t                     index = 0;
	const Inst*                  store = nullptr;
	for (;;) {
		if (!visited.insert(index).second) return {};
		for (const auto& inst: *program.blocks[index]) {
			if (AddressOpcodeInfoOf(inst.GetOpcode()).access != AddressAccess::None) return {};
			if (!inst.MayHaveSideEffects()) continue;
			if (store != nullptr || (BufferAccessOf(inst.GetOpcode()) != BufferAccess::Write &&
			                         inst.GetOpcode() != ValueOpcode::ImageWrite))
				return {};
			store = &inst;
		}
		const auto& term = program.block_info[index].terminator;
		if (term.kind == CFG::TerminatorKind::Return) break;
		if (term.kind != CFG::TerminatorKind::Branch) return {};
		const auto next = std::ranges::find(program.block_info, term.true_block, &BlockInfo::id);
		if (next == program.block_info.end()) return {};
		index = static_cast<uint32_t>(next - program.block_info.begin());
	}
	if (store == nullptr || visited.size() != program.blocks.size()) return {};
	for (const auto& buffer: program.info.buffers) {
		if (buffer.read && (!buffer.scalar || buffer.written)) return {};
	}
	const auto& memory = program.memory_info.at(store->Flags<MemoryFlags>().index);
	UniformFillPlan result;
	result.fill.resource = memory.resource;
	Value data;
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
			const auto index = FillIndex(address->Arg(axis), axis);
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
		    store->Arg(3).Resolve() != Value(0u) || store->Arg(5).Resolve() != Value(true))
			return {};
		if (!memory.formatted || memory.typed || !memory.idxen || memory.offen || memory.offset != 0 ||
		    memory.data_bits != 32 ||
		    memory.data_dwords != static_cast<uint32_t>(store_op - stores.begin() + 1))
			return {};
		const auto address = FillIndex(store->Arg(1), 0);
		if (!address || (*address)[0] != 0 || (*address)[1] != 1 || (*address)[2] == 0) return {};
		result.fill.kind = UniformFillKind::Buffer;
		result.fill.group_stride[0] = static_cast<uint32_t>((*address)[2]);
		result.fill.words = memory.data_dwords;
		data = store->Arg(4);
	}
	data = data.Resolve();
	const auto*          vector = data.TryInstruction();
	constexpr std::array composites {ValueOpcode::CompositeConstructU32x2,
	                                 ValueOpcode::CompositeConstructU32x3,
	                                 ValueOpcode::CompositeConstructU32x4};
	if (result.fill.words > 1 &&
	    (vector == nullptr || vector->GetOpcode() != composites[result.fill.words - 2]))
		return {};
	for (uint32_t i = 0; i < result.fill.words; ++i) {
		const auto word = result.fill.words == 1 ? data : vector->Arg(i);
		if (word.GetType() != Type::U32 ||
		    !ValidateRuntimeValue(program, word, RuntimeValueType::Integer))
			return {};
		result.values[i] = word;
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
	plan.srt_plan_complete          = program.srt_plan_complete;
	plan.resource_tracking_complete = program.resource_tracking_complete;

	// A table pointer can be defined only on paths reaching its scalar load. Resolve
	// those PHIs for the load's PC in this CPU plan, preserving the original GPU SSA.
	// Include the use PC in the cache key because different uses can select different edges.
	std::map<std::pair<const Inst*, uint32_t>, Inst*> cloned;
	std::map<std::pair<const Inst*, uint32_t>, bool>  contextual;
	std::map<std::pair<const Inst*, uint32_t>, Value> resolved_phis;
	const auto ResolvePhiAt = [&](const Inst* phi, uint32_t pc) {
		const auto key = std::pair {phi, pc};
		if (const auto found = resolved_phis.find(key); found != resolved_phis.end())
			return found->second;
		const auto value = ResolveResourcePhi(program, Value(const_cast<Inst*>(phi)), pc);
		resolved_phis.emplace(key, value);
		return value;
	};
	const auto NeedsContext = [&](const Inst* root, uint32_t pc) {
		if (pc == UINT32_MAX) return false;
		const auto key = std::pair {root, pc};
		if (const auto found = contextual.find(key); found != contextual.end())
			return found->second;
		std::vector<const Inst*>        pending {root};
		std::unordered_set<const Inst*> visited;
		bool                            needed = false;
		while (!pending.empty() && !needed) {
			const auto* inst = pending.back();
			pending.pop_back();
			if (!visited.insert(inst).second) continue;
			if (const auto found = contextual.find({inst, pc}); found != contextual.end()) {
				needed = found->second;
				continue;
			}
			const auto op = inst->GetOpcode();
			// Loads establish their own context; flattened reads refer to a separate SRT entry.
			if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer ||
			    op == ValueOpcode::ReadConst)
				continue;
			if (op == ValueOpcode::Phi &&
			    ResolvePhiAt(inst, pc) != Value(const_cast<Inst*>(inst))) {
				needed = true;
				break;
			}
			for (size_t i = 0; i < inst->NumArgs(); ++i) {
				if (const auto* arg = inst->Arg(i).Resolve().TryInstruction())
					pending.push_back(arg);
			}
		}
		if (!needed) {
			for (const auto* inst: visited)
				contextual.emplace(std::pair {inst, pc}, false);
		}
		contextual.emplace(key, needed);
		return needed;
	};
	std::function<Value(Value, uint32_t)> CloneAt = [&](Value value, uint32_t pc) -> Value {
		value              = value.Resolve();
		const auto* source = value.TryInstruction();
		if (source == nullptr) {
			return value;
		}
		const auto op = source->GetOpcode();
		if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
			pc = source->Flags<MemoryFlags>().pc;
		} else if (op == ValueOpcode::ReadConst || op == ValueOpcode::GetUserData ||
		           op == ValueOpcode::GetShaderBase || op == ValueOpcode::GetSrtResource) {
			pc = UINT32_MAX;
		}
		if (source->GetOpcode() == ValueOpcode::Phi) {
			if (pc != UINT32_MAX) {
				const auto guarded = ResolvePhiAt(source, pc);
				if (guarded != value) return CloneAt(guarded, pc);
			}
			const auto invariant = ResolveInvariantPhi(program, value);
			if (!invariant.IsEmpty() && invariant != value) {
				return CloneAt(invariant, pc);
			}
		}
		// Preserve shared loop/index identities when no guarded PHI needs rewriting.
		const auto key = std::pair {source, NeedsContext(source, pc) ? pc : UINT32_MAX};
		if (const auto found = cloned.find(key); found != cloned.end()) {
			return Value(found->second);
		}
		auto& target =
		    plan.value_storage.emplace_back(source->GetOpcode(), source->Flags<uint64_t>());
		cloned.emplace(key, &target);
		if (source->GetOpcode() == ValueOpcode::Phi) {
			for (size_t index = 0; index < source->NumArgs(); index++) {
				target.AddPhiOperand(nullptr, CloneAt(source->Arg(index), pc));
			}
		} else {
			for (size_t index = 0; index < source->NumArgs(); index++) {
				target.SetArg(index, CloneAt(source->Arg(index), pc));
			}
		}
		return Value(&target);
	};
	const auto Clone = [&](Value value) { return CloneAt(value, UINT32_MAX); };

	plan.descriptor_sources.reserve(program.descriptor_sources.size());
	for (const auto& source: program.descriptor_sources) {
		auto& target          = plan.descriptor_sources.emplace_back();
		target.dword_count    = source.dword_count;
		target.indirect_image = source.indirect_image;
		if (source.indirect_image.has_value() && !source.indirect_image->direct_offset.IsEmpty()) {
			target.indirect_image->direct_offset = Clone(source.indirect_image->direct_offset);
			for (auto& range: target.indirect_image->index_ranges) {
				range.value = Clone(range.value);
				range.begin = Clone(range.begin);
				range.end   = Clone(range.end);
				for (auto& [bound, limit]: range.bound_limits)
					bound = Clone(bound);
			}
		}
		if (source.indirect_buffer.has_value()) {
			target.indirect_buffer              = source.indirect_buffer;
			target.indirect_buffer->byte_offset = Clone(source.indirect_buffer->byte_offset);
			for (auto& range: target.indirect_buffer->index_ranges) {
				range.value = Clone(range.value);
				range.begin = Clone(range.begin);
				range.end   = Clone(range.end);
				for (auto& [bound, limit]: range.bound_limits)
					bound = Clone(bound);
			}
			plan.requires_specialization_memory = true;
		}
		for (uint32_t dword = 0; dword < source.dword_count; dword++) {
			target.dwords[dword] = Clone(source.dwords[dword]);
		}
	}
	plan.srt_reads.reserve(program.srt_reads.size());
	for (const auto& read: program.srt_reads) {
		plan.srt_reads.push_back({Clone(read.value), read.flat_offset});
	}
	plan.control_flow = ResourceControlFlow(program);
	for (auto& block: plan.control_flow) {
		block.condition = Clone(block.condition);
	}
	plan.uniform_fill = AnalyzeUniformFill(program);
	for (uint32_t i = 0; i < plan.uniform_fill.fill.words; ++i) {
		plan.uniform_fill.values[i] = Clone(plan.uniform_fill.values[i]);
	}
	plan.materialization_sources.reserve(plan.info.buffers.size() + plan.info.images.size() +
	                                     plan.info.samplers.size());
	for (const auto& buffer: plan.info.buffers) {
		plan.materialization_sources.push_back(buffer.source);
	}
	for (const auto& image: plan.info.images) {
		const auto* source = Source(plan, image.source);
		if (source != nullptr && source->indirect_image.has_value()) {
			plan.requires_specialization_memory = true;
		} else {
			plan.materialization_sources.push_back(image.source);
		}
	}
	for (const auto& sampler: plan.info.samplers) {
		plan.materialization_sources.push_back(sampler.source);
	}
	plan.clean_flat_slots.resize(plan.srt_reads.size());
	for (const auto& buffer: plan.info.buffers) {
		const auto* source = Source(plan, buffer.source);
		if (source != nullptr && source->indirect_buffer.has_value()) {
			MarkCleanFlatSlots(plan, source, plan.clean_flat_slots);
		}
	}
	for (const auto& image: plan.info.images) {
		const auto* source = Source(plan, image.source);
		if (source == nullptr || !source->indirect_image.has_value()) {
			continue;
		}
		MarkCleanFlatSlots(plan, Source(plan, source->indirect_image->material_source),
		                   plan.clean_flat_slots);
		MarkCleanFlatSlots(plan, Source(plan, source->indirect_image->heap_source),
		                   plan.clean_flat_slots);
	}
	return plan;
}

bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization) {
	MaterializedSnapshot materialized;
	if (!MaterializeSnapshot(program, runtime, materialized)) {
		return false;
	}
	return BuildResourceSpecialization(program, std::move(materialized), snapshot, specialization);
}

void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization) {
	EXIT_IF(!program.resource_tracking_complete || program.shader_info_complete ||
	        program.binding_layout_complete);
	EXIT_IF(program.info.buffers.size() > specialization.buffers.size() ||
	        program.info.images.size() > specialization.images.size());

	auto buffers = program.info.buffers;
	for (size_t index = 0; index < specialization.buffers.size(); index++) {
		const auto& source = specialization.buffers[index];
		if (index >= buffers.size()) {
			EXIT_IF(source.indirect_root >= program.info.buffers.size());
			buffers.push_back(program.info.buffers[source.indirect_root]);
		}
		buffers[index].packed_stride           = specialization.buffers[index].packed_stride;
		buffers[index].descriptor_format       = specialization.buffers[index].descriptor_format;
		buffers[index].descriptor_swizzle      = specialization.buffers[index].descriptor_swizzle;
		buffers[index].indirect_root           = source.indirect_root;
		buffers[index].indirect_mapping_offset = source.indirect_mapping_offset;
		buffers[index].indirect_search_iterations = source.indirect_search_iterations;
		buffers[index].indirect_resources.clear();
	}
	for (uint32_t index = 0; index < buffers.size(); ++index) {
		const auto root = buffers[index].indirect_root;
		if (root != BufferResource::NoIndirectBuffer) {
			EXIT_IF(root >= buffers.size());
			buffers[root].indirect_resources.push_back(index);
		}
	}
	auto images = program.info.images;
	images.reserve(specialization.images.size());
	for (uint32_t index = 0; index < specialization.images.size(); index++) {
		const auto& source = specialization.images[index];
		if (index >= images.size()) {
			EXIT_IF(source.indirect_root >= program.info.images.size());
			images.push_back(program.info.images[source.indirect_root]);
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
	EXIT_IF(!BuildSamplerPlan(program.info, images, sampler_plan));
	auto samplers      = program.info.samplers;
	auto sampled_pairs = program.info.sampled_pairs;
	samplers.reserve(sampler_plan.sampler_count);
	for (uint32_t index = 0; index < program.info.samplers.size(); index++) {
		const auto target = sampler_plan.point_sampler[index];
		if (target == UINT32_MAX) {
			continue;
		}
		if (target == index) {
			samplers[index].force_point_filtering = true;
		} else {
			EXIT_IF(target != samplers.size());
			auto sampler                  = samplers[index];
			sampler.force_point_filtering = true;
			samplers.push_back(std::move(sampler));
		}
	}
	for (auto& pair: sampled_pairs) {
		if (RequiresPointSampler(images[pair.image])) {
			EXIT_IF(sampler_plan.point_sampler[pair.sampler] == UINT32_MAX);
			pair.sampler = sampler_plan.point_sampler[pair.sampler];
		}
		samplers[pair.sampler].depth_compare |= images[pair.image].depth_compare;
	}

	auto memory_info = program.memory_info;
	const ImageRemap image_remap(specialization);
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto& inst = *it;
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
			if (image_opcode.needs_sampler && RequiresPointSampler(image) &&
			    memory.sampler < program.info.samplers.size()) {
				EXIT_IF(sampler_plan.point_sampler[memory.sampler] == UINT32_MAX);
				memory.sampler = sampler_plan.point_sampler[memory.sampler];
			}
			EXIT_IF(image.indirect_root == memory.resource &&
			        inst.GetOpcode() != ValueOpcode::ImageSampleRaw &&
			        inst.GetOpcode() != ValueOpcode::ImageRead);
		}
	}
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() == ValueOpcode::GetImageResource) {
				inst.SetFlags(image_remap.indices.at(inst.Flags<uint32_t>()));
			}
		}
	}
	for (auto& memory: memory_info) {
		if (memory.kind == ResourceKind::Image && !memory.planning_only) {
			memory.resource = image_remap.indices.at(memory.resource);
		}
	}
	for (auto& buffer: buffers) {
		if (buffer.image_alias != BufferResource::NoImageAlias) {
			buffer.image_alias = image_remap.indices.at(buffer.image_alias);
		}
	}
	for (auto& pair: sampled_pairs) {
		pair.image = image_remap.indices.at(pair.image);
	}
	for (auto& image: images) {
		if (image.indirect_root != ImageResource::NoIndirectImage) {
			image.indirect_root = image_remap.indices.at(image.indirect_root);
		}
		for (auto& resource: image.indirect_resources) {
			resource = image_remap.indices.at(resource);
		}
	}
	image_remap.Apply(images);
	program.info.buffers       = std::move(buffers);
	program.info.images        = std::move(images);
	program.info.samplers      = std::move(samplers);
	program.info.sampled_pairs = std::move(sampled_pairs);
	program.memory_info        = std::move(memory_info);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
