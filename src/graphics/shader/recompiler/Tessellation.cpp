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
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler {
namespace {

struct TessellationAddress {
	enum class Kind { Unknown, Affine, PackedControlPoint };
	Kind     kind        = Kind::Unknown;
	uint32_t coefficient = 0;
	uint32_t constant    = 0;
};

uint32_t ReflectTessellationStride(const Decoder::Program& program, bool local,
                                   uint32_t control_points, uint32_t input_stride,
                                   uint32_t* patch_begin = nullptr, uint32_t* patch_end = nullptr) {
	using namespace Decoder;
	using Address = TessellationAddress;
	using Kind    = Address::Kind;
	std::array<Address, IR::NumVectorRegs> registers {};
	registers[local ? 3u : 1u] =
	    local ? Address {Kind::Affine, 1u, 0u} : Address {Kind::PackedControlPoint};
	const auto constant = [](uint32_t value) { return Address {Kind::Affine, 0u, value}; };
	// Scalar registers (and VCC) holding known constants, e.g. a stride loaded with s_movk_i32
	// and then used as a VALU operand. Keys are SGPR numbers, VCC_LO/VCC_HI use 0x10000/0x10001.
	std::map<uint32_t, uint32_t> scalars;
	const auto scalar_key = [](const Operand& operand, uint32_t& key) {
		switch (operand.kind) {
			case OperandKind::Sgpr: key = operand.reg; return true;
			case OperandKind::VccLo: key = 0x10000u; return true;
			case OperandKind::VccHi: key = 0x10001u; return true;
			default: return false;
		}
	};
	const auto read     = [&](const Operand& operand) -> Address {
		if (operand.absolute || operand.negate || operand.sdwa_sext || operand.op_sel ||
		    operand.op_sel_hi || operand.negate_hi || operand.dpp)
			return {};
		Address value;
		if (operand.kind == OperandKind::IntegerInlineConstant ||
		    operand.kind == OperandKind::LiteralConstant) {
			value = constant(operand.value);
		} else if (operand.kind == OperandKind::Vgpr) {
			value = registers.at(operand.reg);
		} else if (uint32_t key = 0; scalar_key(operand, key)) {
			if (const auto found = scalars.find(key); found != scalars.end()) {
				value = constant(found->second);
			}
		}
		// The native packed HS ID's low byte is the patch ordinal. The logical
		// interface addresses one patch, as in the frontend's v1 initialization.
		if (value.kind == Kind::PackedControlPoint && operand.sdwa_sel == 0u) return constant(0u);
		return operand.sdwa_sel == 6u ? value : Address {};
	};
	const auto affine = [](uint64_t coefficient, uint64_t offset) {
		return coefficient <= UINT32_MAX && offset <= UINT32_MAX
		           ? Address {Kind::Affine, static_cast<uint32_t>(coefficient),
		                      static_cast<uint32_t>(offset)}
		           : Address {};
	};
	const auto add = [&](Address lhs, Address rhs) {
		return lhs.kind == Kind::Affine && rhs.kind == Kind::Affine
		           ? affine(uint64_t {lhs.coefficient} + rhs.coefficient,
		                    uint64_t {lhs.constant} + rhs.constant)
		           : Address {};
	};
	const auto multiply = [&](Address lhs, Address rhs) {
		if ((lhs.kind == Kind::Affine && lhs.coefficient == 0u && lhs.constant == 0u) ||
		    (rhs.kind == Kind::Affine && rhs.coefficient == 0u && rhs.constant == 0u))
			return constant(0u);
		if (lhs.kind != Kind::Affine || rhs.kind != Kind::Affine) return Address {};
		if (rhs.coefficient != 0u) std::swap(lhs, rhs);
		return rhs.coefficient == 0u ? affine(uint64_t {lhs.coefficient} * rhs.constant,
		                                      uint64_t {lhs.constant} * rhs.constant)
		                             : Address {};
	};
	const auto low24 = [&](Address value) {
		if (value.kind == Kind::Affine && value.coefficient == 0u)
			return constant(value.constant & 0xffffffu);
		return value.kind == Kind::Affine &&
		               uint64_t {value.coefficient} * (control_points - 1u) + value.constant <=
		                   0xffffffu
		           ? value
		           : Address {};
	};
	// a | b equals a + b when the constant side only has bits below every address bit.
	const auto disjoint_or = [&](Address lhs, Address rhs) {
		if (lhs.kind != Kind::Affine || rhs.kind != Kind::Affine) return Address {};
		if (lhs.coefficient == 0u && rhs.coefficient == 0u)
			return constant(lhs.constant | rhs.constant);
		if (lhs.coefficient != 0u) std::swap(lhs, rhs);
		if (lhs.coefficient != 0u) return Address {};
		const uint32_t aligned = rhs.coefficient | rhs.constant;
		const uint32_t low     = aligned & (~aligned + 1u); // lowest set bit
		return lhs.constant < low ? add(lhs, rhs) : Address {};
	};
	// The largest address any control point can produce, used to prove masks are no-ops.
	const auto max_value = [&](Address value) {
		return uint64_t {value.coefficient} * (control_points - 1u) + value.constant;
	};
	const auto and_mask = [&](Address lhs, Address rhs) {
		// Masking the packed HS id to its low byte selects the patch ordinal, which the logical
		// interface fixes at 0 (as for V_BFE_U32 v1, 0, 8 and SDWA byte 0 in read()).
		if (lhs.kind == Kind::PackedControlPoint) std::swap(lhs, rhs);
		if (rhs.kind == Kind::PackedControlPoint)
			return lhs.kind == Kind::Affine && lhs.coefficient == 0u && lhs.constant <= 0xffu
			           ? constant(0u)
			           : Address {};
		if (lhs.kind != Kind::Affine || rhs.kind != Kind::Affine) return Address {};
		if (lhs.coefficient == 0u && rhs.coefficient == 0u)
			return constant(lhs.constant & rhs.constant);
		if (lhs.coefficient != 0u) std::swap(lhs, rhs);
		if (lhs.coefficient != 0u) return Address {};
		const uint32_t mask = lhs.constant;
		return (mask & (mask + 1u)) == 0u && max_value(rhs) <= mask ? rhs : Address {};
	};
	const auto shift_right = [&](Address value, Address amount) {
		if (value.kind != Kind::Affine || amount.kind != Kind::Affine || amount.coefficient != 0u ||
		    amount.constant >= 32u)
			return Address {};
		const uint32_t bits = amount.constant;
		if (value.coefficient == 0u) return constant(value.constant >> bits);
		const uint32_t mask = (1u << bits) - 1u;
		return (value.coefficient & mask) == 0u && (value.constant & mask) == 0u
		           ? affine(value.coefficient >> bits, value.constant >> bits)
		           : Address {};
	};
	const auto dump_program = [&] {
		// One line per instruction: a single log call truncates long programs.
		LOGF("%s program:\n", local ? "LS" : "HS");
		for (const auto& listed: program.instructions) {
			LOGF("%s\n", Decoder::InstructionToString(listed).c_str());
		}
	};
	using Registers = std::array<Address, IR::NumVectorRegs>;
	// Forward branches: a register keeps its value at a join only if every incoming path
	// agrees on it; otherwise it becomes unknown. Code after an unconditional branch is only
	// reachable through a recorded branch target.
	const auto merge = [](Registers& into, const Registers& from) {
		for (size_t i = 0; i < into.size(); i++) {
			if (into[i].kind != from[i].kind || into[i].coefficient != from[i].coefficient ||
			    into[i].constant != from[i].constant) {
				into[i] = {};
			}
		}
	};
	const auto merge_scalars = [](std::map<uint32_t, uint32_t>& into,
	                              const std::map<uint32_t, uint32_t>& from) {
		std::erase_if(into, [&](const auto& entry) {
			const auto found = from.find(entry.first);
			return found == from.end() || found->second != entry.second;
		});
	};
	std::unordered_map<uint32_t, Registers> joins;
	std::unordered_map<uint32_t, std::map<uint32_t, uint32_t>> scalar_joins;
	// Fixed-address HS stores; those past the per-control-point area are per-patch outputs.
	std::vector<std::pair<uint32_t, uint32_t>> constant_stores;
	bool                                    reachable = true;
	uint32_t stride = 0;
	for (const auto& inst: program.instructions) {
		if (const auto join = joins.find(inst.pc); join != joins.end()) {
			const auto scalar_join = scalar_joins.find(inst.pc);
			if (reachable) {
				merge(registers, join->second);
				merge_scalars(scalars, scalar_join->second);
			} else {
				registers = join->second;
				scalars   = scalar_join->second;
			}
			reachable = true;
			joins.erase(join);
			scalar_joins.erase(scalar_join);
		}
		if (IsDirectBranch(inst.opcode)) {
			if (inst.branch_target <= inst.pc) {
				// Loops would need a fixed point over the loop body.
				dump_program();
				EXIT("%s tessellation analysis does not support the backward branch at pc 0x%08x\n",
				     local ? "LS" : "HS", inst.pc);
			}
			if (reachable) {
				if (const auto found = joins.find(inst.branch_target); found != joins.end()) {
					merge(found->second, registers);
					merge_scalars(scalar_joins.at(inst.branch_target), scalars);
				} else {
					joins.emplace(inst.branch_target, registers);
					scalar_joins.emplace(inst.branch_target, scalars);
				}
			}
			if (inst.opcode == Opcode::S_BRANCH) {
				reachable = false;
			}
			continue;
		}
		if (!reachable) {
			continue;
		}
		const bool local_store =
		    inst.opcode == Opcode::DS_WRITE_B32 || inst.opcode == Opcode::DS_WRITE2_B32 ||
		    inst.opcode == Opcode::DS_WRITE_B64 || inst.opcode == Opcode::DS_WRITE2_B64 ||
		    inst.opcode == Opcode::DS_WRITE_B96 || inst.opcode == Opcode::DS_WRITE_B128;
		const bool buffer_store = inst.opcode == Opcode::BUFFER_STORE_DWORD ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX2 ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX3 ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX4;
		const bool control_store =
		    !local && buffer_store && inst.src2.kind == OperandKind::Sgpr && inst.src2.reg == 2u;
		const bool control_read =
		    !local && (inst.opcode == Opcode::DS_READ_B32 || inst.opcode == Opcode::DS_READ2_B32 ||
		               inst.opcode == Opcode::DS_READ_B64 || inst.opcode == Opcode::DS_READ2_B64 ||
		               inst.opcode == Opcode::DS_READ_B96 || inst.opcode == Opcode::DS_READ_B128);
		if ((local && local_store) || control_store || control_read) {
			const auto address = read(inst.src0);
			if (address.kind != Kind::Affine) {
				dump_program();
				EXIT("%s tessellation address is not affine at pc 0x%08x\n", local ? "LS" : "HS",
				     inst.pc);
			}
			if (control_store && address.coefficient == 0u) {
				const uint64_t begin = uint64_t {address.constant} + inst.offset;
				const uint64_t end   = begin + 4u * std::max(inst.data_dwords, 1u);
				if (end <= UINT32_MAX) {
					constant_stores.emplace_back(static_cast<uint32_t>(begin),
					                             static_cast<uint32_t>(end));
				}
			}
			if (address.coefficient != 0u) {
				const auto expected = control_read ? input_stride : stride;
				EXIT_NOT_IMPLEMENTED((address.coefficient & 3u) != 0u ||
				                     (expected != 0u && expected != address.coefficient));
				if (!control_read) stride = address.coefficient;
			}
		}
		// Any scalar write forgets the destination (64-bit writes and loads cover several
		// registers); only constant moves record a value.
		for (const auto* written: {&inst.dst, &inst.dst2}) {
			if (uint32_t key = 0; scalar_key(*written, key)) {
				const uint32_t count = std::max(std::max(inst.data_dwords, 1u), 2u);
				for (uint32_t index = 0; index < count; index++) scalars.erase(key + index);
			}
		}
		if (uint32_t key = 0; (inst.opcode == Opcode::S_MOV_B32 || inst.opcode == Opcode::S_MOVK_I32) &&
		                      scalar_key(inst.dst, key)) {
			if (const auto value = read(inst.src0);
			    value.kind == Kind::Affine && value.coefficient == 0u) {
				scalars[key] = value.constant;
			}
		}
		// Store vdata is a source, despite occupying the decoder's dst field.
		if (local_store || buffer_store || inst.dst.kind != OperandKind::Vgpr) continue;
		const auto lhs   = read(inst.src0);
		const auto rhs   = read(inst.src1);
		const auto third = read(inst.src2);
		Address    value;
		switch (inst.opcode) {
			case Opcode::V_BFE_U32:
				if (lhs.kind == Kind::PackedControlPoint && rhs.kind == Kind::Affine &&
				    rhs.coefficient == 0u && third.kind == Kind::Affine &&
				    third.coefficient == 0u) {
					if (rhs.constant == 8u && third.constant == 5u) value = {Kind::Affine, 1u, 0u};
					if (rhs.constant == 0u && third.constant == 8u) value = constant(0u);
				}
				break;
			case Opcode::V_MUL_U32_U24: value = multiply(low24(lhs), low24(rhs)); break;
			case Opcode::V_MAD_U32_U24: value = add(multiply(low24(lhs), low24(rhs)), third); break;
			case Opcode::V_LSHL_ADD_U32:
				if (rhs.kind == Kind::Affine && rhs.coefficient == 0u && rhs.constant < 32u)
					value = add(multiply(lhs, constant(1u << rhs.constant)), third);
				break;
			case Opcode::V_LSHLREV_B32:
				if (lhs.kind == Kind::Affine && lhs.coefficient == 0u && lhs.constant < 32u)
					value = multiply(rhs, constant(1u << lhs.constant));
				break;
			case Opcode::V_MOV_B32: value = lhs; break;
			case Opcode::V_ADD_NC_U32: value = add(lhs, rhs); break;
			case Opcode::V_ADD3_U32: value = add(add(lhs, rhs), third); break;
			case Opcode::V_MUL_LO_U32: value = multiply(lhs, rhs); break;
			case Opcode::V_SUBREV_NC_U32:
				if (lhs.kind == Kind::Affine && rhs.kind == Kind::Affine &&
				    rhs.coefficient >= lhs.coefficient && rhs.constant >= lhs.constant)
					value = affine(rhs.coefficient - lhs.coefficient, rhs.constant - lhs.constant);
				break;
			case Opcode::V_OR_B32: value = disjoint_or(lhs, rhs); break;
			case Opcode::V_LSHL_OR_B32:
				if (rhs.kind == Kind::Affine && rhs.coefficient == 0u && rhs.constant < 32u)
					value = disjoint_or(multiply(lhs, constant(1u << rhs.constant)), third);
				break;
			case Opcode::V_AND_B32: value = and_mask(lhs, rhs); break;
			case Opcode::V_LSHRREV_B32: value = shift_right(rhs, lhs); break;
			case Opcode::V_SUB_NC_U32:
				if (lhs.kind == Kind::Affine && rhs.kind == Kind::Affine &&
				    lhs.coefficient >= rhs.coefficient && lhs.constant >= rhs.constant)
					value = affine(lhs.coefficient - rhs.coefficient, lhs.constant - rhs.constant);
				break;
			default: break;
		}
		if (inst.dst.sdwa_sel != 6u || inst.dst.op_sel || inst.dst.omod || inst.dst.clamp ||
		    inst.dst.dpp)
			value = {};
		for (uint32_t index = 0;
		     index < std::max(inst.data_dwords, 1u) && inst.dst.reg + index < registers.size(); index++) {
			registers[inst.dst.reg + index] = {};
		}
		registers.at(inst.dst.reg) = value;
	}
	if (stride == 0u) {
		dump_program();
		EXIT("%s tessellation analysis found no per-control-point stride\n", local ? "LS" : "HS");
	}
	if (patch_end != nullptr) {
		// Per-patch outputs follow the control points of every patch in the threadgroup, so
		// their base is the lowest fixed store address past one patch's control points.
		*patch_end = 0;
		uint32_t   lowest        = UINT32_MAX;
		const uint64_t control_area = uint64_t {stride} * control_points;
		for (const auto& [begin, end]: constant_stores) {
			if (begin >= control_area) {
				*patch_end = std::max(*patch_end, end);
				lowest     = std::min(lowest, begin);
			}
		}
		if (patch_begin != nullptr) *patch_begin = *patch_end == 0u ? 0u : lowest;
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
	if (source->GetOpcode() != ValueOpcode::IAdd32 && source->GetOpcode() != ValueOpcode::ISub32)
		return value;
	const auto lhs = ActiveAddress(block, before, source->Arg(0), predicate, resolved);
	const auto rhs = ActiveAddress(block, before, source->Arg(1), predicate, resolved);
	if (lhs != source->Arg(0).Resolve() || rhs != source->Arg(1).Resolve()) {
		auto copy = block.PrependNewInst(before, source->GetOpcode(), {lhs, rhs},
		                                 source->Flags<uint64_t>());
		value     = Value(&*copy);
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
	                                           info.ls_stride, &info.patch_begin, &info.patch_end);
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
					kind = write ? TessellationAttribute::ControlOutput
					             : TessellationAttribute::EvaluationInput;
					EXIT_NOT_IMPLEMENTED(write
					                         ? options.stage != ShaderType::TessellationControl
					                         : options.stage != ShaderType::TessellationEvaluation);
					if (address.IsImmediate() &&
					    address.U32() >= tess.hs_stride * tess.output_control_points) {
						// HS per-patch outputs, read back by the DS as per-patch inputs.
						kind = write ? TessellationAttribute::PatchOutput
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
