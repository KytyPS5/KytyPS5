#include "graphics/shader/recompiler/Tessellation.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"

#include <algorithm>
#include <array>
#include <bit>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler {
namespace {

struct TessellationAddress {
	// Affine: coefficient * ordinal + constant. Scaled: coefficient * n + constant for an
	// unknown integer n.
	enum class Kind { Unknown, Affine, PackedControlPoint, Scaled };
	Kind     kind        = Kind::Unknown;
	uint32_t coefficient = 0;
	uint32_t constant    = 0;

	bool operator==(const TessellationAddress&) const = default;
};

bool IsLocalWrite(Decoder::Opcode opcode) {
	using Decoder::Opcode;
	return opcode == Opcode::DS_WRITE_B32 || opcode == Opcode::DS_WRITE_B64 ||
	       opcode == Opcode::DS_WRITE_B96 || opcode == Opcode::DS_WRITE_B128 ||
	       opcode == Opcode::DS_WRITE2_B32 || opcode == Opcode::DS_WRITE2_B64;
}

bool IsLocalRead(Decoder::Opcode opcode) {
	using Decoder::Opcode;
	return opcode == Opcode::DS_READ_B32 || opcode == Opcode::DS_READ_B64 ||
	       opcode == Opcode::DS_READ_B96 || opcode == Opcode::DS_READ_B128 ||
	       opcode == Opcode::DS_READ2_B32 || opcode == Opcode::DS_READ2_B64;
}

// Returns the stage's ring stride; for the HS, also the byte range of its patch output stores.
uint32_t ReflectTessellationStride(const Decoder::Program& program, bool local,
                                   uint32_t control_points, uint32_t input_stride,
                                   ShaderTessellationInputInfo* patch = nullptr) {
	using namespace Decoder;
	using Address          = TessellationAddress;
	using Kind             = Address::Kind;
	using Registers        = std::array<Address, IR::NumVectorRegs>;
	const auto constant    = [](uint32_t value) { return Address {Kind::Affine, 0u, value}; };
	const auto is_constant = [](Address value) {
		return value.kind == Kind::Affine && value.coefficient == 0u;
	};
	const auto read = [&](const Registers& registers, const Operand& operand) -> Address {
		if (operand.absolute || operand.negate || operand.sdwa_sext || operand.op_sel ||
		    operand.op_sel_hi || operand.negate_hi || operand.dpp)
			return {};
		Address value;
		if (operand.kind == OperandKind::IntegerInlineConstant ||
		    operand.kind == OperandKind::LiteralConstant) {
			value = constant(operand.value);
		} else if (operand.kind == OperandKind::Vgpr) {
			value = registers.at(operand.reg);
		}
		// The native packed HS ID's low byte is the patch ordinal. The logical
		// interface addresses one patch, as in the frontend's v1 initialization.
		if (value.kind == Kind::PackedControlPoint && operand.sdwa_sel == 0u) return constant(0u);
		return operand.sdwa_sel == 6u ? value : Address {};
	};
	const auto affine = [](Kind kind, uint64_t coefficient, uint64_t offset) {
		return coefficient <= UINT32_MAX && offset <= UINT32_MAX
		           ? Address {kind, static_cast<uint32_t>(coefficient),
		                      static_cast<uint32_t>(offset)}
		           : Address {};
	};
	const auto add = [&](Address lhs, Address rhs) {
		if (lhs.kind == Kind::Affine && rhs.kind == Kind::Affine)
			return affine(Kind::Affine, uint64_t {lhs.coefficient} + rhs.coefficient,
			              uint64_t {lhs.constant} + rhs.constant);
		// Ordinal multiples of a Scaled coefficient stay multiples of it.
		const auto scaled = [](Address value) {
			return value.kind == Kind::Affine || value.kind == Kind::Scaled;
		};
		if (!scaled(lhs) || !scaled(rhs)) return Address {};
		const auto coefficient = std::gcd(lhs.coefficient, rhs.coefficient);
		return affine(Kind::Scaled, coefficient, uint64_t {lhs.constant} + rhs.constant);
	};
	const auto multiply = [&](Address lhs, Address rhs, bool u24) {
		const auto zero = [](Address value) {
			return value.kind == Kind::Affine && value.coefficient == 0u && value.constant == 0u;
		};
		if (zero(lhs) || zero(rhs)) return constant(0u);
		if (!is_constant(rhs)) std::swap(lhs, rhs);
		if (!is_constant(rhs)) return Address {};
		if (lhs.kind == Kind::Affine)
			return affine(Kind::Affine, uint64_t {lhs.coefficient} * rhs.constant,
			              uint64_t {lhs.constant} * rhs.constant);
		if (lhs.kind == Kind::PackedControlPoint) return Address {};
		// A product by a power of two keeps that factor when it wraps; a 24-bit operand times at
		// most 256 cannot wrap.
		const bool power = std::has_single_bit(rhs.constant);
		if (lhs.kind == Kind::Scaled && power && std::has_single_bit(lhs.coefficient))
			return affine(Kind::Scaled, uint64_t {lhs.coefficient} * rhs.constant,
			              uint64_t {lhs.constant} * rhs.constant);
		return power || (u24 && rhs.constant <= 0x100u) ? Address {Kind::Scaled, rhs.constant, 0u}
		                                                : Address {};
	};
	const auto low24 = [&](Address value) {
		if (is_constant(value)) return constant(value.constant & 0xffffffu);
		return value.kind == Kind::Affine &&
		               uint64_t {value.coefficient} * (control_points - 1u) + value.constant <=
		                   0xffffffu
		           ? value
		           : Address {};
	};
	uint32_t stride = 0;
	// Constant-address HS stores: {address, first byte, end byte}.
	std::vector<std::array<uint32_t, 3>> fixed_stores;
	// Applies one instruction; ring addresses are only checked once the states are final.
	const auto step = [&](const Instruction& inst, Registers& registers, bool check) {
		const bool local_store  = IsLocalWrite(inst.opcode);
		const bool buffer_store = inst.opcode == Opcode::BUFFER_STORE_DWORD ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX2 ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX3 ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX4;
		const bool control_store =
		    !local && buffer_store && inst.src2.kind == OperandKind::Sgpr && inst.src2.reg == 2u;
		const bool control_read = !local && IsLocalRead(inst.opcode);
		if (check && ((local && local_store) || control_store || control_read)) {
			const auto address = read(registers, inst.src0);
			// A read may index another control point; only its record offset must be fixed.
			const bool indexed_read = control_read && address.kind == Kind::Scaled &&
			                          address.coefficient % input_stride == 0u;
			if (address.kind != Kind::Affine && !indexed_read) {
				EXIT("%s tessellation address is not affine at pc 0x%08x\n", local ? "LS" : "HS",
				     inst.pc);
			}
			if (control_store && is_constant(address)) {
				const auto first = uint64_t {address.constant} + inst.offset;
				EXIT_NOT_IMPLEMENTED(first + 4u * inst.data_dwords > UINT32_MAX);
				fixed_stores.push_back({address.constant, static_cast<uint32_t>(first),
				                        static_cast<uint32_t>(first + 4u * inst.data_dwords)});
			}
			if (address.kind == Kind::Affine && address.coefficient != 0u) {
				const auto expected = control_read ? input_stride : stride;
				EXIT_NOT_IMPLEMENTED((address.coefficient & 3u) != 0u ||
				                     (expected != 0u && expected != address.coefficient));
				if (!control_read) stride = address.coefficient;
				// The lowering writes a per-control-point store to the invocation's own output.
				if (control_store &&
				    uint64_t {address.constant} + inst.offset + 4u * inst.data_dwords > stride) {
					EXIT("HS store at pc 0x%08x leaves its %u-byte control point record\n", inst.pc,
					     stride);
				}
			}
		}
		// Store vdata is a source, despite occupying the decoder's dst field.
		if (local_store || buffer_store || inst.dst.kind != OperandKind::Vgpr) return;
		const auto lhs   = read(registers, inst.src0);
		const auto rhs   = read(registers, inst.src1);
		const auto third = read(registers, inst.src2);
		Address    value;
		switch (inst.opcode) {
			case Opcode::V_MOV_B32: value = lhs; break;
			case Opcode::V_BFE_U32:
				if (lhs.kind == Kind::PackedControlPoint && is_constant(rhs) &&
				    is_constant(third)) {
					if (rhs.constant == 8u && third.constant == 5u) value = {Kind::Affine, 1u, 0u};
					if (rhs.constant == 0u && third.constant == 8u) value = constant(0u);
				}
				break;
			case Opcode::V_MUL_U32_U24: value = multiply(low24(lhs), low24(rhs), true); break;
			case Opcode::V_MAD_U32_U24:
				value = add(multiply(low24(lhs), low24(rhs), true), third);
				break;
			case Opcode::V_ADD_NC_U32: value = add(lhs, rhs); break;
			case Opcode::V_ADD3_U32: value = add(add(lhs, rhs), third); break;
			case Opcode::V_LSHL_ADD_U32:
				if (is_constant(rhs) && rhs.constant < 32u)
					value = add(multiply(lhs, constant(1u << rhs.constant), false), third);
				break;
			case Opcode::V_LSHLREV_B32:
				if (is_constant(lhs) && lhs.constant < 32u)
					value = multiply(rhs, constant(1u << lhs.constant), false);
				break;
			case Opcode::V_SUB_NC_U32:
				if (lhs.kind == Kind::Affine && rhs.kind == Kind::Affine &&
				    lhs.coefficient >= rhs.coefficient && lhs.constant >= rhs.constant)
					value = affine(Kind::Affine, lhs.coefficient - rhs.coefficient,
					               lhs.constant - rhs.constant);
				break;
			default: break;
		}
		if (inst.dst.sdwa_sel != 6u || inst.dst.op_sel || inst.dst.omod || inst.dst.clamp ||
		    inst.dst.dpp)
			value = {};
		for (uint32_t index = 0;
		     index < std::max(inst.data_dwords, 1u) && inst.dst.reg + index < registers.size();
		     index++) {
			registers[inst.dst.reg + index] = {};
		}
		registers.at(inst.dst.reg) = value;
	};

	// Forward dataflow over the stage's blocks; a register whose incoming values differ is unknown.
	const auto& insts    = program.instructions;
	const auto  index_of = [&](uint32_t pc) {
		const auto it = std::ranges::lower_bound(insts, pc, {}, &Instruction::pc);
		EXIT_IF(it == insts.end() || it->pc != pc);
		return static_cast<size_t>(it - insts.begin());
	};
	std::vector<bool> leader(insts.size(), false);
	leader.at(0) = true;
	for (size_t index = 0; index < insts.size(); index++) {
		if (!IsDirectBranch(insts[index].opcode)) continue;
		leader[index_of(insts[index].branch_target)] = true;
		if (index + 1u < insts.size()) leader[index + 1u] = true;
	}
	// Entry states of the reached blocks, by leader index.
	std::unordered_map<size_t, Registers> entry;
	entry[0].at(local ? 3u : 1u) =
	    local ? Address {Kind::Affine, 1u, 0u} : Address {Kind::PackedControlPoint};
	std::vector<size_t> work {0};
	const auto          flow = [&](size_t target, const Registers& registers) {
		const auto [it, inserted] = entry.try_emplace(target, registers);
		bool changed              = inserted;
		for (size_t reg = 0; !inserted && reg < registers.size(); reg++) {
			if (it->second[reg] != registers[reg] && it->second[reg].kind != Kind::Unknown) {
				it->second[reg] = {};
				changed         = true;
			}
		}
		if (changed) work.push_back(target);
	};
	const auto run = [&](size_t begin, bool check) {
		auto registers = entry.at(begin);
		for (size_t index = begin; index < insts.size(); index++) {
			if (index != begin && leader[index]) {
				if (!check) flow(index, registers);
				return;
			}
			const auto& inst = insts[index];
			step(inst, registers, check);
			if (inst.opcode == Opcode::S_ENDPGM) return;
			if (IsDirectBranch(inst.opcode)) {
				if (!check) {
					flow(index_of(inst.branch_target), registers);
					if (IsConditionalBranch(inst.opcode) && index + 1u < insts.size())
						flow(index + 1u, registers);
				}
				return;
			}
		}
	};
	while (!work.empty()) {
		const auto begin = work.back();
		work.pop_back();
		run(begin, false);
	}
	for (size_t index = 0; index < insts.size(); index++) {
		if (entry.contains(index)) run(index, true);
	}
	EXIT_NOT_IMPLEMENTED(stride == 0u);
	if (patch != nullptr) {
		// As in the lowering, a store address past every control point selects the patch outputs.
		patch->patch_begin = UINT32_MAX;
		patch->patch_end   = 0;
		for (const auto& [address, first, end]: fixed_stores) {
			if (address < stride * control_points) continue;
			patch->patch_begin = std::min(patch->patch_begin, first);
			patch->patch_end   = std::max(patch->patch_end, end);
		}
		if (patch->patch_end == 0u) patch->patch_begin = 0;
	}
	return stride;
}

const IR::Inst* TessellationBufferBase(const IR::Inst& inst) {
	if (IR::BufferAccessOf(inst.GetOpcode()) == IR::BufferAccess::None || inst.NumArgs() <= 3u)
		return nullptr;
	const auto* base = inst.Arg(3).Resolve().TryInstruction();
	return base != nullptr && base->GetOpcode() == IR::ValueOpcode::TessellationBase ? base
	                                                                                 : nullptr;
}

IR::Value ActiveAddress(IR::Block& block, IR::Block::iterator before, IR::Value value,
                        IR::Value predicate, std::unordered_map<IR::Inst*, IR::Value>& resolved) {
	using namespace IR;
	value        = value.Resolve();
	auto* source = value.TryInstruction();
	if (source == nullptr) return value;
	if (const auto found = resolved.find(source); found != resolved.end()) return found->second;
	if (source->GetOpcode() == ValueOpcode::SelectU32) {
		return source->Arg(0).Resolve() == predicate.Resolve()
		           ? ActiveAddress(block, before, source->Arg(1), predicate, resolved)
		           : value;
	}
	// Lanes that pass the predicate see the selected operand through any integer operation.
	switch (source->GetOpcode()) {
		case ValueOpcode::IAdd32:
		case ValueOpcode::ISub32:
		case ValueOpcode::IMul32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitFieldUExtract: break;
		default: return value;
	}
	std::array<Value, 3> args;
	bool                 changed = false;
	for (size_t index = 0; index < source->NumArgs(); index++) {
		args.at(index) = ActiveAddress(block, before, source->Arg(index), predicate, resolved);
		changed |= args[index] != source->Arg(index).Resolve();
	}
	if (changed) {
		auto copy =
		    source->NumArgs() == 3u
		        ? block.PrependNewInst(before, source->GetOpcode(), {args[0], args[1], args[2]},
		                               source->Flags<uint64_t>())
		        : block.PrependNewInst(before, source->GetOpcode(), {args[0], args[1]},
		                               source->Flags<uint64_t>());
		value = Value(&*copy);
	}
	resolved.emplace(source, value);
	return value;
}

} // namespace

void AnalyzeTessellationPrograms(std::span<const uint32_t> local, std::span<const uint32_t> control,
                                 ShaderTessellationInputInfo& info) {
	const auto       local_program = Decoder::DecodeFrontProgram(local);
	Decoder::Program control_program;
	Decoder::DecodeProgram(control, control_program);
	info.ls_stride = ReflectTessellationStride(local_program, true, info.input_control_points, 0u);
	info.hs_stride = ReflectTessellationStride(control_program, false, info.output_control_points,
	                                           info.ls_stride, &info);
	LOGF("Tessellation interface: input_cp=%u output_cp=%u ls_stride=%u hs_stride=%u "
	     "patch=[%u, %u)\n",
	     info.input_control_points, info.output_control_points, info.ls_stride, info.hs_stride,
	     info.patch_begin, info.patch_end);
}

void LowerTessellationMemory(IR::Program& program, const CompileOptions& options) {
	using namespace IR;
	if (options.stage != ShaderType::Local && options.stage != ShaderType::TessellationControl &&
	    options.stage != ShaderType::TessellationEvaluation) {
		return;
	}
	const auto& tess = options.input_info.vertex->tess;
	// Ring addresses can reuse data VGPRs. Their inactive values are irrelevant to
	// a store guarded by the same EXEC predicate, but must remain intact elsewhere.
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			if (TessellationBufferBase(*it) == nullptr) continue;
			std::unordered_map<Inst*, Value> resolved;
			it->SetArg(
			    2, ActiveAddress(*block, it, it->Arg(2), it->Arg(it->NumArgs() - 1u), resolved));
		}
	}
	ConstantPropagationPass(program.blocks, program.wave_size);
	uint32_t reads = 0, writes = 0, factors = 0;
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto&      inst   = *it;
			const auto shared = SharedAccessOf(inst.GetOpcode());
			const auto buffer = BufferAccessOf(inst.GetOpcode());
			if (shared == SharedAccess::None && buffer == BufferAccess::None) {
				continue;
			}
			const auto&           memory = program.memory_info.at(inst.Flags<MemoryFlags>().index);
			bool                  write  = false;
			uint32_t              components = 0;
			TessellationAttribute kind;
			Value                 address, predicate;
			if (shared != SharedAccess::None && memory.kind == ResourceKind::Lds) {
				write = shared == SharedAccess::Write;
				EXIT_NOT_IMPLEMENTED(memory.data_bits != 32u ||
				                     (options.stage == ShaderType::Local
				                          ? !write
				                          : options.stage != ShaderType::TessellationControl ||
				                                shared != SharedAccess::Read));
				kind       = write ? TessellationAttribute::LocalOutput
				                   : TessellationAttribute::ControlInput;
				components = SharedComponentCount(inst.GetOpcode());
				address    = inst.Arg(0);
				predicate  = inst.Arg(inst.NumArgs() - 1u);
			} else if (buffer != BufferAccess::None && memory.kind == ResourceKind::Buffer) {
				const auto* base = TessellationBufferBase(inst);
				if (base == nullptr) {
					continue;
				}
				EXIT_NOT_IMPLEMENTED(memory.data_bits != 32u || memory.formatted || memory.idxen ||
				                     !memory.offen || buffer == BufferAccess::Atomic);
				write      = buffer == BufferAccess::Write;
				components = BufferComponentCount(inst.GetOpcode());
				address    = inst.Arg(2).Resolve();
				predicate  = inst.Arg(inst.NumArgs() - 1u);
				if (base->Arg(0).U32() == 1u) {
					EXIT_NOT_IMPLEMENTED(!write ||
					                     options.stage != ShaderType::TessellationControl);
					kind = TessellationAttribute::Factor;
					factors += components;
				} else {
					// The HS reads back its own outputs, written by any control point before a
					// barrier.
					const bool control = options.stage == ShaderType::TessellationControl;
					kind               = control ? TessellationAttribute::ControlOutput
					                             : TessellationAttribute::EvaluationInput;
					EXIT_NOT_IMPLEMENTED(
					    write ? !control
					          : !control && options.stage != ShaderType::TessellationEvaluation);
					if (address.IsImmediate() &&
					    address.U32() >= tess.hs_stride * tess.output_control_points) {
						kind = control ? TessellationAttribute::PatchOutput
						               : TessellationAttribute::PatchInput;
					}
				}
			} else {
				continue;
			}
			const auto emit = [&](ValueOpcode opcode, std::initializer_list<Value> args) {
				return Value(&*block->PrependNewInst(it, opcode, args));
			};
			std::array<Value, 4> values;
			for (uint32_t component = 0; component < components; component++) {
				const auto offset = memory.offset + 4u * component;
				const auto byte_address =
				    offset == 0u ? address : emit(ValueOpcode::IAdd32, {address, Value(offset)});
				if (write) {
					Value data = shared != SharedAccess::None ? inst.Arg(component + 1u)
					             : components == 1u
					                 ? inst.Arg(4)
					                 : emit(components == 2u   ? ValueOpcode::CompositeExtractU32x2
					                        : components == 3u ? ValueOpcode::CompositeExtractU32x3
					                                           : ValueOpcode::CompositeExtractU32x4,
					                        {inst.Arg(4), Value(component)});
					emit(ValueOpcode::SetTessellationAttribute,
					     {Value(static_cast<uint32_t>(kind)), byte_address, data, predicate});
					writes++;
				} else {
					values[component] =
					    emit(ValueOpcode::GetTessellationAttribute,
					         {Value(static_cast<uint32_t>(kind)), byte_address, predicate});
					reads++;
				}
			}
			if (!write) {
				Value replacement = values[0];
				if (components == 2u)
					replacement =
					    emit(ValueOpcode::CompositeConstructU32x2, {values[0], values[1]});
				if (components == 3u)
					replacement = emit(ValueOpcode::CompositeConstructU32x3,
					                   {values[0], values[1], values[2]});
				if (components == 4u)
					replacement = emit(ValueOpcode::CompositeConstructU32x4,
					                   {values[0], values[1], values[2], values[3]});
				inst.ReplaceUsesWith(replacement);
			}
			inst.Invalidate();
		}
	}
	ConstantPropagationPass(program.blocks, program.wave_size);
	RemoveIdentities(program);
	EliminateDeadCode(program.blocks);
	LOGF("%s tessellation lowering: reads=%u writes=%u factors=%u\n",
	     options.stage == ShaderType::Local                 ? "LS"
	     : options.stage == ShaderType::TessellationControl ? "HS"
	                                                       : "TES",
	     reads, writes, factors);
}

} // namespace Libs::Graphics::ShaderRecompiler
