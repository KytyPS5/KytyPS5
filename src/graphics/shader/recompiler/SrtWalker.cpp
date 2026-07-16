#include "graphics/shader/recompiler/SrtWalker.h"

#include "graphics/host_gpu/hostMemory.h"
#include "graphics/shader/recompiler/ScalarProvenance.h"

#include <algorithm>
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

bool AddSignedAddress(uint64_t base, int64_t offset, uint64_t* result) {
	if (result == nullptr || base > AddressMask) {
		return false;
	}
	if (offset < 0) {
		const auto magnitude = uint64_t {0} - static_cast<uint64_t>(offset);
		if (magnitude > base) {
			return false;
		}
		*result = base - magnitude;
		return true;
	}
	const auto magnitude = static_cast<uint64_t>(offset);
	if (magnitude > AddressMask - base) {
		return false;
	}
	*result = base + magnitude;
	return true;
}

bool ApplyOperation(ScalarValueOp op, const uint32_t args[3], uint32_t* result) {
	if (result == nullptr) {
		return false;
	}
	const auto shift = args[1] & 31u;
	switch (op) {
		case ScalarValueOp::Add: *result = args[0] + args[1]; break;
		case ScalarValueOp::AddCarry: *result = args[0] + args[1] + (args[2] & 1u); break;
		case ScalarValueOp::Carry:
			*result = static_cast<uint32_t>(
			    (static_cast<uint64_t>(args[0]) + args[1] + (args[2] & 1u)) >> 32u);
			break;
		case ScalarValueOp::Sub: *result = args[0] - args[1]; break;
		case ScalarValueOp::SubBorrow: *result = args[0] - args[1] - (args[2] & 1u); break;
		case ScalarValueOp::Borrow:
			*result = static_cast<uint64_t>(args[1]) + (args[2] & 1u) > args[0] ? 1u : 0u;
			break;
		case ScalarValueOp::Mul: *result = args[0] * args[1]; break;
		case ScalarValueOp::And: *result = args[0] & args[1]; break;
		case ScalarValueOp::AndNot: *result = args[0] & ~args[1]; break;
		case ScalarValueOp::Or: *result = args[0] | args[1]; break;
		case ScalarValueOp::OrNot: *result = args[0] | ~args[1]; break;
		case ScalarValueOp::Xor: *result = args[0] ^ args[1]; break;
		case ScalarValueOp::Not: *result = ~args[0]; break;
		case ScalarValueOp::ShiftLeft: *result = args[0] << shift; break;
		case ScalarValueOp::ShiftRight: *result = args[0] >> shift; break;
		case ScalarValueOp::ShiftRightArithmetic:
			*result = static_cast<uint32_t>(static_cast<int32_t>(args[0]) >> shift);
			break;
		case ScalarValueOp::BitFieldMaskU32: {
			const auto count  = args[0] & 31u;
			const auto offset = args[1] & 31u;
			*result           = ((uint32_t {1} << count) - 1u) << offset;
			break;
		}
		case ScalarValueOp::BitFieldMaskU64Low:
		case ScalarValueOp::BitFieldMaskU64High: {
			const auto count = args[0] & 63u;
			const auto value = ((uint64_t {1} << count) - 1u) << (args[1] & 63u);
			*result = op == ScalarValueOp::BitFieldMaskU64Low ? static_cast<uint32_t>(value)
			                                                  : static_cast<uint32_t>(value >> 32u);
			break;
		}
		case ScalarValueOp::Add3: *result = args[0] + args[1] + args[2]; break;
		case ScalarValueOp::ShiftLeftAdd: *result = (args[0] << shift) + args[2]; break;
		case ScalarValueOp::ShiftLeftAddCarry:
			*result =
			    static_cast<uint32_t>(((static_cast<uint64_t>(args[0]) << shift) + args[2]) >> 32u);
			break;
		case ScalarValueOp::AddShiftLeft: *result = (args[0] + args[1]) << (args[2] & 31u); break;
		case ScalarValueOp::XorAdd: *result = (args[0] ^ args[1]) + args[2]; break;
		case ScalarValueOp::ShiftLeftOr: *result = (args[0] << shift) | args[2]; break;
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
				uint32_t args[3] = {};
				for (uint32_t i = 0; i < count; i++) {
					if (!Fold(value.args[i], args[i])) {
						return Dynamic(id);
					}
				}
				if (!ApplyOperation(value.op, args, &out)) {
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
	explicit PlanBuilder(Program* program): m_program(program), m_folder(program->provenance) {
		m_state.resize(program->provenance.values.size());
	}

	bool Run(std::string* error) {
		m_program->srt = {};
		for (const auto& block: m_program->blocks) {
			for (const auto& inst: block.instructions) {
				if (!CollectDescriptor(inst.memory.resource_source, inst.pc, error) ||
				    !CollectDescriptor(inst.memory.sampler_source, inst.pc, error)) {
					return false;
				}
			}
		}
		for (const auto& block: m_program->blocks) {
			for (const auto& inst: block.instructions) {
				if (inst.op == Opcode::SLoadDword && inst.scalar_value < m_state.size() &&
				    m_state[inst.scalar_value] == 0) {
					const auto& value   = m_program->provenance.values[inst.scalar_value];
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
			*error = Diagnostic(*m_program, pc, message);
		}
		return false;
	}

	bool CollectDescriptor(uint32_t source, uint32_t use_pc, std::string* error) {
		const auto* descriptor = GetDescriptorSource(*m_program, source);
		if (descriptor == nullptr) {
			if (source <= ScalarProvenance::Unknown) {
				return source == ScalarProvenance::Undefined || AddDynamicSource(source);
			}
			return Fail(use_pc, error, fmt::format("invalid descriptor source {}", source));
		}
		if (!DescriptorSourceResolved(*m_program, source) ||
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
		const auto& value = m_program->provenance.values[id];
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
		std::vector<uint8_t> visited(m_program->provenance.values.size());
		for (uint32_t i = 0; i < descriptor.dword_count; i++) {
			if (ValueNeedsControlFlow(descriptor.dwords[i], &visited)) {
				return true;
			}
		}
		return false;
	}

	bool ValueNeedsControlFlow(uint32_t id, std::vector<uint8_t>* visited) {
		if (id >= m_program->provenance.values.size() || (*visited)[id] != 0) {
			return false;
		}
		(*visited)[id]    = 1;
		const auto& value = m_program->provenance.values[id];
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
		if (std::find(m_program->srt.dynamic_sources.begin(), m_program->srt.dynamic_sources.end(),
		              source) == m_program->srt.dynamic_sources.end()) {
			m_program->srt.dynamic_sources.push_back(source);
		}
		return true;
	}

	bool CollectValue(uint32_t id, uint32_t use_pc, std::string* error) {
		if (id >= m_program->provenance.values.size()) {
			return Fail(use_pc, error, fmt::format("invalid scalar value {}", id));
		}
		if (m_state[id] == 2) {
			return true;
		}
		if (m_state[id] == 1) {
			return Fail(use_pc, error, fmt::format("cyclic scalar value {}", id));
		}
		m_state[id]       = 1;
		const auto& value = m_program->provenance.values[id];
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
				m_program->srt.reads.push_back(
				    {id, static_cast<uint32_t>(m_program->srt.reads.size()), use_pc});
			} else {
				m_program->srt.dynamic_reads.push_back(id);
			}
		}
		m_state[id] = 2;
		return true;
	}

	Program*             m_program;
	ConstantFolder       m_folder;
	std::vector<uint8_t> m_state;
};

class Evaluator {
public:
	Evaluator(const Program& program, const SrtRuntime& runtime, uint32_t use_pc)
	    : m_program(program), m_runtime(runtime), m_use_pc(use_pc),
	      m_values(program.provenance.values.size()), m_state(program.provenance.values.size()) {}

	void SetUsePc(uint32_t use_pc) { m_use_pc = use_pc; }

	bool Evaluate(uint32_t id, uint32_t& result, std::string* error) {
		if (id >= m_program.provenance.values.size()) {
			return Fail(error, fmt::format("invalid scalar value {}", id));
		}
		if (m_state[id] == 2) {
			result = m_values[id];
			return true;
		}
		if (m_state[id] == 1) {
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
				if (!Read(value, &out, error)) {
					return false;
				}
				break;
			case ScalarValueOp::Undefined:
			case ScalarValueOp::Unknown:
				return Fail(error, fmt::format("scalar value {} is unresolved", id));
			default:
				if (!EvaluateOperation(value, &out, error)) {
					return false;
				}
				break;
		}
		m_state[id]  = 2;
		m_values[id] = out;
		result       = out;
		return true;
	}

private:
	bool Fail(std::string* error, const std::string& message) const {
		if (error != nullptr) {
			*error = Diagnostic(m_program, m_use_pc, message);
		}
		return false;
	}

	bool EvaluateOperation(const ScalarValue& value, uint32_t* result, std::string* error) {
		const auto count = ScalarValueArgCount(value.op);
		if (count == 0 || count > 3) {
			return Fail(error, fmt::format("unsupported scalar operation {}",
			                               static_cast<uint32_t>(value.op)));
		}
		uint32_t args[3] = {};
		for (uint32_t i = 0; i < count; i++) {
			if (!Evaluate(value.args[i], args[i], error)) {
				return false;
			}
		}
		return ApplyOperation(value.op, args, result) ||
		       Fail(error, fmt::format("unsupported scalar operation {}",
		                               static_cast<uint32_t>(value.op)));
	}

	bool Read(const ScalarValue& value, uint32_t* result, std::string* error) {
		const bool buffer = value.op == ScalarValueOp::ReadConstBuffer;
		uint32_t   lo     = 0;
		uint32_t   hi     = 0;
		uint32_t   offset = 0;
		if (!Evaluate(value.args[0], lo, error) || !Evaluate(value.args[1], hi, error) ||
		    !Evaluate(value.args[buffer ? 4u : 2u], offset, error)) {
			return false;
		}
		const auto base      = (static_cast<uint64_t>(hi) << 32u | lo) & AddressMask;
		const auto immediate = static_cast<int64_t>(static_cast<int32_t>(value.imm));
		uint64_t   address   = 0;
		if (buffer) {
			uint32_t num_records = 0;
			uint32_t ignored     = 0;
			if (!Evaluate(value.args[2], num_records, error) ||
			    !Evaluate(value.args[3], ignored, error)) {
				return false;
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
			if (!AddSignedAddress(base_aligned, relative, &address)) {
				return Fail(error,
				            fmt::format("ReadConst pc=0x{:08x} address base=0x{:016x} offset={} "
				                        "is outside the 48-bit address space",
				                        value.pc, base_aligned, relative));
			}
		}
		const auto read_memory =
		    m_runtime.read_memory != nullptr ? m_runtime.read_memory : HostMemoryReadDword;
		if (!read_memory(m_runtime.userdata, address, result)) {
			return Fail(
			    error, fmt::format("ReadConst pc=0x{:08x} failed at 0x{:016x}", value.pc, address));
		}
		return true;
	}

	const Program&        m_program;
	const SrtRuntime&     m_runtime;
	uint32_t              m_use_pc;
	std::vector<uint32_t> m_values;
	std::vector<uint8_t>  m_state;
};

} // namespace

bool FoldScalarConstant(const ScalarProvenance& provenance, uint32_t value, uint32_t* result) {
	if (result == nullptr) {
		return false;
	}
	ConstantFolder folder(provenance);
	return folder.Fold(value, *result);
}

bool BuildSrtPlan(Program* program, std::string* error) {
	if (program == nullptr) {
		if (error != nullptr) {
			*error = "invalid SRT program";
		}
		return false;
	}
	if (program->resource_tracking_complete) {
		if (error != nullptr) {
			*error = "cannot rebuild SRT plan after resource tracking";
		}
		return false;
	}
	if (program->srt_patching_complete) {
		if (error != nullptr) {
			*error = "cannot rebuild SRT plan after SRT patching";
		}
		return false;
	}
	program->srt_plan_complete = false;
	if (!PlanBuilder(program).Run(error)) {
		return false;
	}
	program->srt_plan_complete = true;
	return true;
}

bool EvaluateDescriptorSource(const Program& program, uint32_t source, uint32_t use_pc,
                              const SrtRuntime& runtime, DescriptorValue* result,
                              std::string* error) {
	if (result == nullptr) {
		if (error != nullptr) {
			*error = Diagnostic(program, use_pc, "invalid descriptor result");
		}
		return false;
	}
	const DescriptorSourceRequest request {source, use_pc};
	std::vector<DescriptorValue>  results;
	if (!EvaluateDescriptorSources(program, std::span {&request, 1}, runtime, &results, error)) {
		return false;
	}
	*result = results[0];
	return true;
}

static bool EvaluateRuntimeSourcesImpl(const Program&                           program,
                                       std::span<const DescriptorSourceRequest> requests,
                                       const SrtRuntime&                        runtime,
                                       std::vector<DescriptorValue>*            results,
                                       std::vector<uint32_t>* flat, bool evaluate_flat,
                                       std::string* error) {
	if (results == nullptr || flat == nullptr || !program.srt_plan_complete) {
		if (error != nullptr) {
			*error = Diagnostic(program, 0,
			                    results == nullptr || flat == nullptr
			                        ? "invalid runtime evaluation output"
			                        : "SRT plan is not ready");
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
	*results = std::move(evaluated);
	if (evaluate_flat) {
		*flat = std::move(flattened);
	}
	return true;
}

bool EvaluateDescriptorSources(const Program&                           program,
                               std::span<const DescriptorSourceRequest> requests,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>* results,
                               std::string* error) {
	if (results == nullptr) {
		if (error != nullptr) {
			*error = Diagnostic(program, 0, "invalid descriptor results");
		}
		return false;
	}
	std::vector<uint32_t> ignored;
	return EvaluateRuntimeSourcesImpl(program, requests, runtime, results, &ignored, false, error);
}

bool EvaluateRuntimeSources(const Program&                           program,
                            std::span<const DescriptorSourceRequest> requests,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>* results,
                            std::vector<uint32_t>* flat, std::string* error) {
	return EvaluateRuntimeSourcesImpl(program, requests, runtime, results, flat, true, error);
}

bool WalkSrt(const Program& program, const SrtRuntime& runtime, std::vector<uint32_t>* flat,
             std::string* error) {
	if (flat == nullptr) {
		if (error != nullptr) {
			*error = Diagnostic(program, 0, "invalid SRT walker output");
		}
		return false;
	}
	std::vector<DescriptorValue> ignored;
	return EvaluateRuntimeSources(program, {}, runtime, &ignored, flat, error);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
