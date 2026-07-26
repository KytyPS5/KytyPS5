#include "graphics/shader/recompiler/SrtWalker.h"

#include "common/virtualMemory.h"
#include "graphics/shader/recompiler/ScalarProvenance.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fmt/format.h>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask = 0x0000ffffffffffffull;

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "vertex";
		case ShaderType::Pixel: return "pixel";
		case ShaderType::Fetch: return "fetch";
		case ShaderType::Compute: return "compute";
		default: return "unknown";
	}
}

std::string Diagnostic(const Program& program, uint32_t pc, const std::string& message) {
	return fmt::format("shader SRT: hash=0x{:016x} stage={} pc=0x{:08x} {}", program.shader_hash,
	                   StageName(program.stage), pc, message);
}

bool AddSignedAddress(uint64_t base, int64_t offset, uint64_t& result) {
	if (base > AddressMask) {
		return false;
	}
	if (offset < 0) {
		const auto magnitude = uint64_t {0} - static_cast<uint64_t>(offset);
		if (magnitude > base) {
			return false;
		}
		result = base - magnitude;
		return true;
	}
	const auto magnitude = static_cast<uint64_t>(offset);
	if (magnitude > AddressMask - base) {
		return false;
	}
	result = base + magnitude;
	return true;
}

bool ApplyOperation(ScalarValueOp op, const std::array<uint32_t, 3>& args, uint32_t& result) {
	const auto shift = args[1] & 31u;
	switch (op) {
		case ScalarValueOp::Add: result = args[0] + args[1]; break;
		case ScalarValueOp::AddCarry: result = args[0] + args[1] + (args[2] & 1u); break;
		case ScalarValueOp::Carry:
			result = static_cast<uint32_t>(
			    (static_cast<uint64_t>(args[0]) + args[1] + (args[2] & 1u)) >> 32u);
			break;
		case ScalarValueOp::Sub: result = args[0] - args[1]; break;
		case ScalarValueOp::SubBorrow: result = args[0] - args[1] - (args[2] & 1u); break;
		case ScalarValueOp::Borrow:
			result = static_cast<uint64_t>(args[1]) + (args[2] & 1u) > args[0] ? 1u : 0u;
			break;
		case ScalarValueOp::Mul: result = args[0] * args[1]; break;
		case ScalarValueOp::And: result = args[0] & args[1]; break;
		case ScalarValueOp::AndNot: result = args[0] & ~args[1]; break;
		case ScalarValueOp::Or: result = args[0] | args[1]; break;
		case ScalarValueOp::OrNot: result = args[0] | ~args[1]; break;
		case ScalarValueOp::Xor: result = args[0] ^ args[1]; break;
		case ScalarValueOp::Not: result = ~args[0]; break;
		case ScalarValueOp::ShiftLeft: result = args[0] << shift; break;
		case ScalarValueOp::ShiftRight: result = args[0] >> shift; break;
		case ScalarValueOp::ShiftRightArithmetic:
			result = static_cast<uint32_t>(static_cast<int32_t>(args[0]) >> shift);
			break;
		case ScalarValueOp::BitFieldMaskU32: {
			const auto count  = args[0] & 31u;
			const auto offset = args[1] & 31u;
			result            = ((uint32_t {1} << count) - 1u) << offset;
			break;
		}
		case ScalarValueOp::BitFieldMaskU64Low:
		case ScalarValueOp::BitFieldMaskU64High: {
			const auto count = args[0] & 63u;
			const auto value = ((uint64_t {1} << count) - 1u) << (args[1] & 63u);
			result = op == ScalarValueOp::BitFieldMaskU64Low ? static_cast<uint32_t>(value)
			                                                 : static_cast<uint32_t>(value >> 32u);
			break;
		}
		case ScalarValueOp::Add3: result = args[0] + args[1] + args[2]; break;
		case ScalarValueOp::ShiftLeftAdd: result = (args[0] << shift) + args[2]; break;
		case ScalarValueOp::ShiftLeftAddCarry:
			result =
			    static_cast<uint32_t>(((static_cast<uint64_t>(args[0]) << shift) + args[2]) >> 32u);
			break;
		case ScalarValueOp::AddShiftLeft: result = (args[0] + args[1]) << (args[2] & 31u); break;
		case ScalarValueOp::XorAdd: result = (args[0] ^ args[1]) + args[2]; break;
		case ScalarValueOp::ShiftLeftOr: result = (args[0] << shift) | args[2]; break;
		case ScalarValueOp::MinI32:
			result = static_cast<uint32_t>(
			    std::min(static_cast<int32_t>(args[0]), static_cast<int32_t>(args[1])));
			break;
		case ScalarValueOp::MaxI32:
			result = static_cast<uint32_t>(
			    std::max(static_cast<int32_t>(args[0]), static_cast<int32_t>(args[1])));
			break;
		case ScalarValueOp::MinU32: result = std::min(args[0], args[1]); break;
		case ScalarValueOp::MaxU32: result = std::max(args[0], args[1]); break;
		case ScalarValueOp::AbsI32:
			result = (args[0] & 0x80000000u) != 0 ? (~args[0] + 1u) : args[0];
			break;
		case ScalarValueOp::BitFieldExtractU32: {
			const auto offset = args[1] & 0x1fu;
			const auto count  = (args[1] >> 16u) & 0x7fu;
			result = count == 0 ? 0u
			         : count >= 32u
			             ? (args[0] >> offset)
			             : ((args[0] >> offset) & ((uint32_t {1} << count) - 1u));
			break;
		}
		case ScalarValueOp::SelectU32: result = args[0] != 0 ? args[1] : args[2]; break;
		case ScalarValueOp::ShiftLeftU64Low:
		case ScalarValueOp::ShiftLeftU64High: {
			const auto     shift_amount = args[2] & 63u;
			const uint64_t v = (static_cast<uint64_t>(args[1]) << 32u) | args[0];
			const uint64_t shifted      = v << shift_amount;
			result                      = static_cast<uint32_t>(op == ScalarValueOp::ShiftLeftU64Low
			                                                        ? shifted
			                                                        : shifted >> 32u);
			break;
		}
		case ScalarValueOp::ShiftRightU64Low:
		case ScalarValueOp::ShiftRightU64High: {
			const auto     shift_amount = args[2] & 63u;
			const uint64_t v = (static_cast<uint64_t>(args[1]) << 32u) | args[0];
			const uint64_t shifted      = v >> shift_amount;
			result                      = static_cast<uint32_t>(op == ScalarValueOp::ShiftRightU64Low
			                                                        ? shifted
			                                                        : shifted >> 32u);
			break;
		}
		default: return false;
	}
	return true;
}

class ConstantFolder {
public:
	explicit ConstantFolder(const ScalarProvenance& provenance)
	    : m_provenance(provenance), m_values(provenance.values.size()),
	      m_state(provenance.values.size()) {}

	bool Fold(uint32_t id, uint32_t& result) {
		if (id >= m_provenance.values.size() || m_state[id] == 1 || m_state[id] == 3) {
			return false;
		}
		if (m_state[id] == 2) {
			result = m_values[id];
			return true;
		}
		m_state[id]       = 1;
		const auto& value = m_provenance.values[id];
		uint32_t    out   = 0;
		switch (value.op) {
			case ScalarValueOp::Constant: out = value.imm; break;
			case ScalarValueOp::Phi:
				if (value.phi_args.empty() || !Fold(value.phi_args[0], out)) {
					return Dynamic(id);
				}
				for (size_t i = 1; i < value.phi_args.size(); i++) {
					uint32_t other = 0;
					if (!Fold(value.phi_args[i], other) || other != out) {
						return Dynamic(id);
					}
				}
				break;
			default: {
				const auto count = ScalarValueArgCount(value.op);
				if (count == 0 || count > 3 || value.op == ScalarValueOp::ReadConst) {
					return Dynamic(id);
				}
				std::array<uint32_t, 3> args {};
				for (uint32_t i = 0; i < count; i++) {
					if (!Fold(value.args[i], args[i])) {
						return Dynamic(id);
					}
				}
				if (!ApplyOperation(value.op, args, out)) {
					return Dynamic(id);
				}
				break;
			}
		}
		m_state[id]  = 2;
		m_values[id] = out;
		result       = out;
		return true;
	}

private:
	bool Dynamic(uint32_t id) {
		m_state[id] = 3;
		return false;
	}

	const ScalarProvenance& m_provenance;
	std::vector<uint32_t>   m_values;
	std::vector<uint8_t>    m_state;
};

class PlanBuilder {
public:
	explicit PlanBuilder(Program& program): m_program(program), m_folder(program.provenance) {
		m_state.resize(program.provenance.values.size());
	}

	bool Run(std::string* error) {
		m_program.srt = {};
		for (const auto& block: m_program.blocks) {
			for (const auto& inst: block.instructions) {
				if (!CollectDescriptor(inst.memory.resource_source, inst.pc, error) ||
				    !CollectDescriptor(inst.memory.sampler_source, inst.pc, error)) {
					return false;
				}
			}
		}
		for (const auto& block: m_program.blocks) {
			for (const auto& inst: block.instructions) {
				// Only the host can service a flat-SRT read, so a load is promotable only when its
				// pointer resolves on the host too. A run-time base leaves the read unevaluable;
				// the shader performs it itself instead.
				if (inst.op == Opcode::SLoadDword && inst.scalar_value < m_state.size() &&
				    m_state[inst.scalar_value] == 0 &&
				    DescriptorSourceResolved(m_program, inst.memory.resource_source)) {
					const auto& value   = m_program.provenance.values[inst.scalar_value];
					uint32_t    ignored = 0;
					if (value.op == ScalarValueOp::ReadConst &&
					    m_folder.Fold(value.args[2], ignored) &&
					    !CollectValue(inst.scalar_value, inst.pc, error)) {
						return false;
					}
				}
			}
		}
		return true;
	}

private:
	bool Fail(uint32_t pc, std::string* error, const std::string& message) const {
		if (error != nullptr) {
			*error = Diagnostic(m_program, pc, message);
		}
		return false;
	}

	bool CollectDescriptor(uint32_t source, uint32_t use_pc, std::string* error) {
		const auto* descriptor = GetDescriptorSource(m_program, source);
		if (descriptor == nullptr) {
			if (source <= ScalarProvenance::Unknown) {
				return source == ScalarProvenance::Undefined || AddDynamicSource(source);
			}
			return Fail(use_pc, error, fmt::format("invalid descriptor source {}", source));
		}
		if (!DescriptorSourceResolved(m_program, source) ||
		    DescriptorNeedsControlFlow(*descriptor)) {
			MarkDescriptor(*descriptor);
			return AddDynamicSource(source);
		}
		for (uint32_t i = 0; i < descriptor->dword_count; i++) {
			if (!CollectValue(descriptor->dwords[i], use_pc, error)) {
				return false;
			}
		}
		return true;
	}

	void MarkDescriptor(const DescriptorValue& descriptor) {
		for (uint32_t i = 0; i < descriptor.dword_count; i++) {
			MarkValue(descriptor.dwords[i]);
		}
	}

	void MarkValue(uint32_t id) {
		if (id >= m_state.size() || m_state[id] != 0) {
			return;
		}
		m_state[id]       = 2;
		const auto& value = m_program.provenance.values[id];
		if (value.op == ScalarValueOp::Phi) {
			for (const auto arg: value.phi_args) {
				MarkValue(arg);
			}
			return;
		}
		for (uint32_t i = 0; i < ScalarValueArgCount(value.op); i++) {
			MarkValue(value.args[i]);
		}
	}

	bool DescriptorNeedsControlFlow(const DescriptorValue& descriptor) {
		std::vector<uint8_t> visited(m_program.provenance.values.size());
		for (uint32_t i = 0; i < descriptor.dword_count; i++) {
			if (ValueNeedsControlFlow(descriptor.dwords[i], visited)) {
				return true;
			}
		}
		return false;
	}

	bool ValueNeedsControlFlow(uint32_t id, std::vector<uint8_t>& visited) {
		if (id >= m_program.provenance.values.size() || visited[id] != 0) {
			return false;
		}
		visited[id]       = 1;
		const auto& value = m_program.provenance.values[id];
		if (value.op == ScalarValueOp::Phi) {
			uint32_t ignored = 0;
			return !m_folder.Fold(id, ignored);
		}
		for (uint32_t i = 0; i < ScalarValueArgCount(value.op); i++) {
			if (ValueNeedsControlFlow(value.args[i], visited)) {
				return true;
			}
		}
		return false;
	}

	bool AddDynamicSource(uint32_t source) {
		if (std::find(m_program.srt.dynamic_sources.begin(), m_program.srt.dynamic_sources.end(),
		              source) == m_program.srt.dynamic_sources.end()) {
			m_program.srt.dynamic_sources.push_back(source);
		}
		return true;
	}

	bool CollectValue(uint32_t id, uint32_t use_pc, std::string* error) {
		if (id >= m_program.provenance.values.size()) {
			return Fail(use_pc, error, fmt::format("invalid scalar value {}", id));
		}
		if (m_state[id] == 2) {
			return true;
		}
		if (m_state[id] == 1) {
			return Fail(use_pc, error, fmt::format("cyclic scalar value {}", id));
		}
		m_state[id]       = 1;
		const auto& value = m_program.provenance.values[id];
		if (value.op == ScalarValueOp::Phi) {
			for (const auto arg: value.phi_args) {
				if (!CollectValue(arg, use_pc, error)) {
					return false;
				}
			}
		} else {
			for (uint32_t i = 0; i < ScalarValueArgCount(value.op); i++) {
				if (!CollectValue(value.args[i], use_pc, error)) {
					return false;
				}
			}
		}
		if (value.op == ScalarValueOp::ReadConst || value.op == ScalarValueOp::ReadConstBuffer) {
			const auto offset_arg = value.op == ScalarValueOp::ReadConst ? 2u : 4u;
			uint32_t   ignored    = 0;
			if (m_folder.Fold(value.args[offset_arg], ignored)) {
				m_program.srt.reads.push_back(
				    {id, static_cast<uint32_t>(m_program.srt.reads.size()), use_pc});
			} else {
				m_program.srt.dynamic_reads.push_back(id);
			}
		}
		m_state[id] = 2;
		return true;
	}

	Program&             m_program;
	ConstantFolder       m_folder;
	std::vector<uint8_t> m_state;
};

// How much a partially evaluated subterm tells us about the value it stands for.
enum class Resolution : uint8_t {
	// Nothing: the subterm depends on a run-time quantity in a way that cannot be summarised.
	None,
	// The evaluated value is the value the shader will see.
	Exact,
	// The evaluated value is a base the shader will add a non-negative run-time delta to.
	Base,
};

class Evaluator {
public:
	Evaluator(const Program& program, const SrtRuntime& runtime, uint32_t use_pc)
	    : m_program(program), m_runtime(runtime), m_use_pc(use_pc),
	      m_values(program.provenance.values.size()), m_state(program.provenance.values.size()),
	      m_resolution(program.provenance.values.size(), Resolution::Exact) {}

	void SetUsePc(uint32_t use_pc) { m_use_pc = use_pc; }

	// Partial mode keeps going when a subterm turns out to be run-time dependent, reporting how
	// much of the value survived instead of failing outright. Used to recover the base of an
	// address the shader computes for itself.
	void SetPartial(bool partial) { m_partial = partial; }

	Resolution ResolutionOf(uint32_t id) const {
		return id < m_resolution.size() ? m_resolution[id] : Resolution::None;
	}

	bool SpansMultipleBases() const { return m_spans_multiple_bases; }

	bool Evaluate(uint32_t id, uint32_t& result, std::string* error) {
		Resolution resolution = Resolution::Exact;
		return Evaluate(id, result, resolution, error);
	}

	bool Evaluate(uint32_t id, uint32_t& result, Resolution& resolution, std::string* error) {
		resolution = Resolution::Exact;
		if (id >= m_program.provenance.values.size()) {
			return Fail(error, fmt::format("invalid scalar value {}", id));
		}
		if (m_state[id] == 2) {
			result     = m_values[id];
			resolution = m_resolution[id];
			return true;
		}
		if (m_state[id] == 1) {
			// A value that only exists once the loop it lives in has run. In partial mode the
			// enclosing phi supplies the other arms, so report it as run-time rather than failing.
			if (m_partial) {
				result     = 0;
				resolution = Resolution::None;
				return true;
			}
			return Fail(error, fmt::format("cyclic scalar value {}", id));
		}
		m_state[id]       = 1;
		const auto& value = m_program.provenance.values[id];
		uint32_t    out   = 0;
		switch (value.op) {
			case ScalarValueOp::UserData:
				if (value.imm < m_program.user_data_base ||
				    value.imm - m_program.user_data_base >= m_runtime.user_data.size()) {
					return Fail(error, fmt::format("user SGPR {} is unavailable", value.imm));
				}
				out = m_runtime.user_data[value.imm - m_program.user_data_base];
				break;
			case ScalarValueOp::Constant: out = value.imm; break;
			case ScalarValueOp::PcRelativeLow:
				out = static_cast<uint32_t>(m_runtime.shader_base + value.imm);
				break;
			case ScalarValueOp::PcRelativeHigh:
				out = static_cast<uint32_t>((m_runtime.shader_base + value.imm) >> 32u);
				break;
			case ScalarValueOp::Phi: {
				const auto first = std::find_if(value.phi_args.begin(), value.phi_args.end(),
				                                [id](uint32_t arg) { return arg != id; });
				if (first == value.phi_args.end()) {
					return Fail(error, fmt::format("empty scalar phi {}", id));
				}
				if (m_partial) {
					if (!EvaluatePartialPhi(id, value, out, resolution, error)) {
						return false;
					}
					break;
				}
				if (!Evaluate(*first, out, error)) {
					return false;
				}
				for (const auto arg: value.phi_args) {
					if (arg == id || arg == *first) {
						continue;
					}
					uint32_t other = 0;
					if (!Evaluate(arg, other, error)) {
						return false;
					}
					if (other != out) {
						return Fail(error,
						            fmt::format("scalar phi {} has runtime-dependent values", id));
					}
				}
				break;
			}
			case ScalarValueOp::ReadConst:
			case ScalarValueOp::ReadConstBuffer:
				if (!Read(value, out, resolution, error)) {
					return false;
				}
				break;
			case ScalarValueOp::Undefined:
			case ScalarValueOp::Unknown:
				if (!m_partial) {
					return Fail(error, fmt::format("scalar value {} is unresolved", id));
				}
				out        = 0;
				resolution = Resolution::None;
				break;
			default:
				if (!EvaluateOperation(value, out, resolution, error)) {
					return false;
				}
				break;
		}
		m_state[id]      = 2;
		m_values[id]     = out;
		m_resolution[id] = resolution;
		result           = out;
		return true;
	}

private:
	bool Fail(std::string* error, const std::string& message) const {
		if (error != nullptr) {
			*error = Diagnostic(m_program, m_use_pc, message);
		}
		return false;
	}

	bool EvaluateOperation(const ScalarValue& value, uint32_t& result, Resolution& resolution,
	                       std::string* error) {
		resolution       = Resolution::Exact;
		const auto count = ScalarValueArgCount(value.op);
		if (count == 0 || count > 3) {
			return Fail(error, fmt::format("unsupported scalar operation {}",
			                               static_cast<uint32_t>(value.op)));
		}
		std::array<uint32_t, 3>   args {};
		std::array<Resolution, 3> arg_resolution {};
		bool                      exact = true;
		for (uint32_t i = 0; i < count; i++) {
			if (!Evaluate(value.args[i], args[i], arg_resolution[i], error)) {
				return false;
			}
			exact = exact && arg_resolution[i] == Resolution::Exact;
		}
		if (exact) {
			return ApplyOperation(value.op, args, result) ||
			       Fail(error, fmt::format("unsupported scalar operation {}",
			                               static_cast<uint32_t>(value.op)));
		}
		return ApplyPartialOperation(value.op, args, arg_resolution, count, result, resolution);
	}

	// Summarises an operation whose operands are not all exact. Only additions survive: a base plus
	// a run-time displacement is still a usable base, because the shader adds the same displacement
	// itself. Anything that scales, masks or reorders bits would move the result away from the base
	// by an unbounded amount, so it degrades to Resolution::None.
	static bool ApplyPartialOperation(ScalarValueOp op, const std::array<uint32_t, 3>& args,
	                                  const std::array<Resolution, 3>& arg_resolution,
	                                  uint32_t count, uint32_t& result, Resolution& resolution) {
		result     = 0;
		resolution = Resolution::None;
		switch (op) {
			case ScalarValueOp::Add:
			case ScalarValueOp::AddCarry:
			case ScalarValueOp::Add3: {
				uint32_t sum = 0;
				bool     any = false;
				// AddCarry's third operand is a carry-in; an unknown carry only ever adds one, so
				// dropping it keeps the result a valid lower bound.
				const auto addends = op == ScalarValueOp::AddCarry ? std::min(count, 2u) : count;
				for (uint32_t i = 0; i < addends; i++) {
					if (arg_resolution[i] != Resolution::None) {
						sum += args[i];
						any = true;
					}
				}
				if (!any) {
					return true;
				}
				result     = sum;
				resolution = Resolution::Base;
				return true;
			}
			// A carry-out of a run-time addend is zero or one. Treating it as zero keeps the high
			// half of the address a lower bound, which is what Resolution::Base promises.
			case ScalarValueOp::Carry:
			case ScalarValueOp::Borrow: resolution = Resolution::Base; return true;
			default: return true;
		}
	}

	bool EvaluatePartialPhi(uint32_t id, const ScalarValue& value, uint32_t& result,
	                        Resolution& resolution, std::string* error) {
		result              = 0;
		resolution          = Resolution::None;
		bool     have       = false;
		bool     all_exact  = true;
		bool     disagree   = false;
		uint32_t selected   = 0;
		for (const auto arg: value.phi_args) {
			if (arg == id) {
				continue;
			}
			uint32_t   arm            = 0;
			Resolution arm_resolution = Resolution::None;
			if (!Evaluate(arg, arm, arm_resolution, error)) {
				return false;
			}
			if (arm_resolution == Resolution::None) {
				all_exact = false;
				continue;
			}
			all_exact = all_exact && arm_resolution == Resolution::Exact;
			if (!have) {
				selected = arm;
				have     = true;
				continue;
			}
			if (arm != selected) {
				disagree = true;
				// The arms sit at different places in guest memory. Keep the lowest so the window
				// the host binds starts at or below every one of them.
				selected = std::min(selected, arm);
			}
		}
		if (!have) {
			return true;
		}
		result     = selected;
		resolution = all_exact && !disagree ? Resolution::Exact : Resolution::Base;
		if (disagree) {
			m_spans_multiple_bases = true;
		}
		return true;
	}

	bool Read(const ScalarValue& value, uint32_t& result, Resolution& resolution,
	          std::string* error) {
		resolution        = Resolution::Exact;
		const bool buffer = value.op == ScalarValueOp::ReadConstBuffer;
		uint32_t   lo     = 0;
		uint32_t   hi     = 0;
		uint32_t   offset = 0;
		std::array<Resolution, 3> parts {};
		if (!Evaluate(value.args[0], lo, parts[0], error) ||
		    !Evaluate(value.args[1], hi, parts[1], error) ||
		    !Evaluate(value.args[buffer ? 4u : 2u], offset, parts[2], error)) {
			return false;
		}
		// A memory read is only meaningful at an address we know exactly; a base is not good
		// enough, because the contents at base and at base+delta are unrelated.
		if (std::any_of(parts.begin(), parts.end(),
		                [](Resolution part) { return part != Resolution::Exact; })) {
			result     = 0;
			resolution = Resolution::None;
			return true;
		}
		const auto base      = (static_cast<uint64_t>(hi) << 32u | lo) & AddressMask;
		const auto immediate = static_cast<int64_t>(static_cast<int32_t>(value.imm));
		uint64_t   address   = 0;
		if (buffer) {
			uint32_t   num_records        = 0;
			uint32_t   ignored            = 0;
			Resolution records_resolution = Resolution::Exact;
			Resolution ignored_resolution = Resolution::Exact;
			if (!Evaluate(value.args[2], num_records, records_resolution, error) ||
			    !Evaluate(value.args[3], ignored, ignored_resolution, error)) {
				return false;
			}
			if (records_resolution != Resolution::Exact ||
			    ignored_resolution != Resolution::Exact) {
				result     = 0;
				resolution = Resolution::None;
				return true;
			}
			if (immediate < 0) {
				return Fail(error,
				            fmt::format("ReadConstBuffer pc=0x{:08x} has negative immediate {}",
				                        value.pc, immediate));
			}
			const auto byte_offset    = static_cast<uint64_t>(immediate) + offset;
			const auto aligned_offset = byte_offset & ~uint64_t {3};
			const auto stride         = (hi >> 16u) & 0x3fffu;
			const auto size           = stride == 0 ? static_cast<uint64_t>(num_records)
			                                        : static_cast<uint64_t>(stride) * num_records;
			if (aligned_offset > size || size - aligned_offset < sizeof(uint32_t)) {
				return Fail(error,
				            fmt::format("ReadConstBuffer pc=0x{:08x} offset={} exceeds size={}",
				                        value.pc, aligned_offset, size));
			}
			address = ((base & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
		} else {
			const auto base_aligned      = base & ~uint64_t {3};
			const auto immediate_aligned = immediate & ~int64_t {3};
			const auto relative = immediate_aligned + static_cast<int64_t>(offset & ~uint32_t {3});
			if (!AddSignedAddress(base_aligned, relative, address)) {
				return Fail(error,
				            fmt::format("ReadConst pc=0x{:08x} address base=0x{:016x} offset={} "
				                        "is outside the 48-bit address space",
				                        value.pc, base_aligned, relative));
			}
		}
		if (m_runtime.read_memory != nullptr &&
		    !m_runtime.read_memory(m_runtime.userdata, address, &result)) {
			return Fail(
			    error, fmt::format("ReadConst pc=0x{:08x} failed at 0x{:016x}", value.pc, address));
		}
		// No reader supplied: read the guest address ourselves, but never by dereferencing it. The
		// address is derived from shader analysis of guest data, so an unresolved base leaves a
		// small constant here (a null descriptor pointer plus its field offset) and a raw load
		// takes the whole emulator down. Reporting the failure instead lets TryRecompile() return
		// false, which the dispatch path already handles by skipping the dispatch.
		if (m_runtime.read_memory == nullptr &&
		    !Common::VirtualMemory::TryRead(address, &result, sizeof(result))) {
			return Fail(error, fmt::format("ReadConst pc=0x{:08x} cannot read guest memory at "
			                               "0x{:016x}",
			                               value.pc, address));
		}
		return true;
	}

	const Program&          m_program;
	const SrtRuntime&       m_runtime;
	uint32_t                m_use_pc;
	std::vector<uint32_t>   m_values;
	std::vector<uint8_t>    m_state;
	std::vector<Resolution> m_resolution;
	bool                    m_partial              = false;
	bool                    m_spans_multiple_bases = false;
};

} // namespace

bool FoldScalarConstant(const ScalarProvenance& provenance, uint32_t value, uint32_t& result) {
	ConstantFolder folder(provenance);
	return folder.Fold(value, result);
}

bool BuildSrtPlan(Program& program, std::string* error) {
	if (program.resource_tracking_complete) {
		if (error != nullptr) {
			*error = "cannot rebuild SRT plan after resource tracking";
		}
		return false;
	}
	if (program.srt_patching_complete) {
		if (error != nullptr) {
			*error = "cannot rebuild SRT plan after SRT patching";
		}
		return false;
	}
	program.srt_plan_complete = false;
	if (!PlanBuilder(program).Run(error)) {
		return false;
	}
	program.srt_plan_complete = true;
	return true;
}

bool EvaluateDescriptorSource(const Program& program, uint32_t source, uint32_t use_pc,
	                          const SrtRuntime& runtime, DescriptorValue& result,
	                          std::string* error) {
	const DescriptorSourceRequest request {source, use_pc};
	std::vector<DescriptorValue>  results;
	if (!EvaluateDescriptorSources(program, std::span {&request, 1}, runtime, results, error)) {
		return false;
	}
	result = results[0];
	return true;
}

bool EvaluateAddressBase(const Program& program, uint32_t source, uint32_t use_pc,
                         const SrtRuntime& runtime, AddressBase& result, std::string* error) {
	result = {};
	if (!program.srt_plan_complete) {
		if (error != nullptr) {
			*error = Diagnostic(program, use_pc, "SRT plan is not ready");
		}
		return false;
	}
	const auto* descriptor = GetDescriptorSource(program, source);
	if (descriptor == nullptr || descriptor->dword_count != 2) {
		if (error != nullptr) {
			*error = Diagnostic(program, use_pc,
			                    fmt::format("address source {} is missing or has wrong width",
			                                source));
		}
		return false;
	}

	Evaluator evaluator(program, runtime, use_pc);
	evaluator.SetPartial(true);
	std::array<uint32_t, 2>   parts {};
	std::array<Resolution, 2> resolution {};
	for (uint32_t i = 0; i < 2; i++) {
		if (!evaluator.Evaluate(descriptor->dwords[i], parts[i], resolution[i], error)) {
			return false;
		}
	}
	if (resolution[0] == Resolution::None || resolution[1] == Resolution::None) {
		if (error != nullptr) {
			*error = Diagnostic(
			    program, use_pc,
			    fmt::format("address source {} has no resolvable base: low={} high={}", source,
			                static_cast<uint32_t>(resolution[0]),
			                static_cast<uint32_t>(resolution[1])));
		}
		return false;
	}

	result.base = ((static_cast<uint64_t>(parts[1]) << 32u) | parts[0]) & AddressMask;
	result.exact =
	    resolution[0] == Resolution::Exact && resolution[1] == Resolution::Exact;
	result.spans_multiple_bases = evaluator.SpansMultipleBases();
	return true;
}

static bool EvaluateRuntimeSourcesImpl(const Program&                           program,
	                                   std::span<const DescriptorSourceRequest> requests,
	                                   const SrtRuntime&                        runtime,
	                                   std::vector<DescriptorValue>&            results,
	                                   std::vector<uint32_t>& flat, bool evaluate_flat,
	                                   std::string* error) {
	if (!program.srt_plan_complete) {
		if (error != nullptr) {
			*error = Diagnostic(program, 0, "SRT plan is not ready");
		}
		return false;
	}
	std::vector<DescriptorValue> evaluated;
	evaluated.reserve(requests.size());
	std::vector<uint32_t> flattened(evaluate_flat ? program.srt.reads.size() : 0u);
	Evaluator             evaluator(program, runtime, 0);
	for (const auto& request: requests) {
		const auto* descriptor = GetDescriptorSource(program, request.source);
		const auto  dynamic =
		    std::find(program.srt.dynamic_sources.begin(), program.srt.dynamic_sources.end(),
		              request.source) != program.srt.dynamic_sources.end();
		if (descriptor != nullptr && request.allow_unresolved &&
		    (dynamic || !DescriptorSourceResolved(program, request.source))) {
			auto zeroed = *descriptor;
			zeroed.dwords.fill(0);
			evaluated.push_back(zeroed);
			continue;
		}
		if (descriptor == nullptr || dynamic ||
		    !DescriptorSourceResolved(program, request.source)) {
			if (error != nullptr) {
				*error = Diagnostic(program, request.use_pc,
				                    fmt::format("descriptor source {} is {}", request.source,
				                                dynamic ? "GPU-dynamic" : "unresolved"));
			}
			return false;
		}
		evaluator.SetUsePc(request.use_pc);
		auto value = *descriptor;
		for (uint32_t i = 0; i < descriptor->dword_count; i++) {
			if (!evaluator.Evaluate(descriptor->dwords[i], value.dwords[i], error)) {
				return false;
			}
		}
		evaluated.push_back(value);
	}
	if (evaluate_flat) {
		for (const auto& read: program.srt.reads) {
			evaluator.SetUsePc(read.use_pc);
			if (read.flat_offset >= flattened.size()) {
				if (error != nullptr) {
					*error = Diagnostic(program, read.use_pc, "invalid flat SRT offset");
				}
				return false;
			}
			if (!evaluator.Evaluate(read.value, flattened[read.flat_offset], error)) {
				return false;
			}
		}
	}
	results = std::move(evaluated);
	if (evaluate_flat) {
		flat = std::move(flattened);
	}
	return true;
}

bool EvaluateDescriptorSources(const Program&                           program,
	                           std::span<const DescriptorSourceRequest> requests,
	                           const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
	                           std::string* error) {
	std::vector<uint32_t> ignored;
	return EvaluateRuntimeSourcesImpl(program, requests, runtime, results, ignored, false, error);
}

bool EvaluateRuntimeSources(const Program&                           program,
	                        std::span<const DescriptorSourceRequest> requests,
	                        const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
	                        std::vector<uint32_t>& flat, std::string* error) {
	return EvaluateRuntimeSourcesImpl(program, requests, runtime, results, flat, true, error);
}

bool WalkSrt(const Program& program, const SrtRuntime& runtime, std::vector<uint32_t>& flat,
	         std::string* error) {
	std::vector<DescriptorValue> ignored;
	return EvaluateRuntimeSources(program, {}, runtime, ignored, flat, error);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
