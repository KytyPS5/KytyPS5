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

struct TessellationPatchRange {
	uint32_t begin = UINT32_MAX;
	uint32_t end   = 0;
};

// Also reports the range of the HS's per-patch outputs: constant stores past the control
// point records.
uint32_t ReflectTessellationStride(const Decoder::Program& program, bool local,
                                   uint32_t control_points, uint32_t input_stride,
                                   TessellationPatchRange* patch = nullptr) {
	using namespace Decoder;
	using Address = TessellationAddress;
	using Kind    = Address::Kind;
	std::array<Address, IR::NumVectorRegs> registers {};
	registers[local ? 3u : 1u] =
	    local ? Address {Kind::Affine, 1u, 0u} : Address {Kind::PackedControlPoint};
	const auto constant = [](uint32_t value) { return Address {Kind::Affine, 0u, value}; };
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
	const auto same = [](const Address& lhs, const Address& rhs) {
		return lhs.kind == rhs.kind && lhs.coefficient == rhs.coefficient &&
		       lhs.constant == rhs.constant;
	};
	// Forward branches to an internal join: at the join, a register keeps its address only if
	// every incoming path agrees on it.
	std::vector<std::pair<uint32_t, std::array<Address, IR::NumVectorRegs>>> joins;
	std::vector<TessellationPatchRange> constant_stores;
	uint32_t stride = 0;
	// Cleared after an unconditional branch: until a join, no path reaches the instructions.
	bool reachable = true;
	for (const auto& inst: program.instructions) {
		for (auto join = joins.begin(); join != joins.end();) {
			if (join->first != inst.pc) {
				++join;
				continue;
			}
			if (!reachable) {
				registers = join->second;
				reachable = true;
			} else {
				for (size_t reg = 0; reg < registers.size(); reg++) {
					if (!same(registers[reg], join->second[reg])) registers[reg] = {};
				}
			}
			join = joins.erase(join);
		}
		if (!reachable) continue;
		// Stage exits preserve the active path's definitions.
		if (IsDirectBranch(inst.opcode) && inst.branch_target != program.instructions.back().pc) {
			EXIT_NOT_IMPLEMENTED(inst.branch_target <= inst.pc);
			joins.emplace_back(inst.branch_target, registers);
		}
		if (inst.opcode == Opcode::S_BRANCH || inst.opcode == Opcode::S_ENDPGM) {
			reachable = false;
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
				EXIT("%s tessellation address is not affine at pc 0x%08x\n", local ? "LS" : "HS",
				     inst.pc);
			}
			if (control_store && address.coefficient == 0u) {
				const auto begin = address.constant + inst.offset;
				constant_stores.push_back({begin, begin + 4u * std::max(inst.data_dwords, 1u)});
			}
			if (address.coefficient != 0u) {
				const auto expected = control_read ? input_stride : stride;
				EXIT_NOT_IMPLEMENTED((address.coefficient & 3u) != 0u ||
				                     (expected != 0u && expected != address.coefficient));
				if (!control_read) stride = address.coefficient;
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
			case Opcode::V_AND_B32: {
				// The packed HS ID's low byte is the patch ordinal: zero for the logical patch.
				const auto mask_of = [](Address operand) {
					return operand.kind == Kind::Affine && operand.coefficient == 0u &&
					       (operand.constant & ~0xffu) == 0u;
				};
				if ((lhs.kind == Kind::PackedControlPoint && mask_of(rhs)) ||
				    (rhs.kind == Kind::PackedControlPoint && mask_of(lhs)))
					value = constant(0u);
				break;
			}
			case Opcode::V_ADD_NC_U32: value = add(lhs, rhs); break;
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
	EXIT_NOT_IMPLEMENTED(stride == 0u);
	if (patch != nullptr) {
		for (const auto& store: constant_stores) {
			if (store.begin >= stride * control_points) {
				patch->begin = std::min(patch->begin, store.begin);
				patch->end   = std::max(patch->end, store.end);
			}
		}
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
	const auto opcode = source->GetOpcode();
	if (opcode != ValueOpcode::IAdd32 && opcode != ValueOpcode::ISub32 &&
	    opcode != ValueOpcode::IMul32 && opcode != ValueOpcode::ShiftLeftLogical32 &&
	    opcode != ValueOpcode::BitFieldUExtract)
		return value;
	// Address arithmetic; 24-bit multiplies also extract their operands' low bits.
	std::array<Value, 3> args {};
	bool                 changed = false;
	for (size_t index = 0; index < source->NumArgs(); index++) {
		args[index] = ActiveAddress(block, before, source->Arg(index), predicate, resolved);
		changed |= args[index] != source->Arg(index).Resolve();
	}
	if (changed) {
		auto copy =
		    source->NumArgs() == 3u
		        ? block.PrependNewInst(before, opcode, {args[0], args[1], args[2]},
		                               source->Flags<uint64_t>())
		        : block.PrependNewInst(before, opcode, {args[0], args[1]}, source->Flags<uint64_t>());
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
	TessellationPatchRange patch;
	info.hs_stride = ReflectTessellationStride(control_program, false, info.output_control_points,
	                                           info.ls_stride, &patch);
	if (patch.begin < patch.end) {
		info.patch_base = patch.begin;
		info.patch_size = patch.end - patch.begin;
	}
	LOGF("Tessellation interface: input_cp=%u output_cp=%u ls_stride=%u hs_stride=%u "
	     "patch=0x%x+0x%x\n",
	     info.input_control_points, info.output_control_points, info.ls_stride, info.hs_stride,
	     info.patch_base, info.patch_size);
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
					// The instruction offset is part of the byte address, as in the reflection.
					if (address.IsImmediate() && address.U32() + memory.offset >=
					                                 tess.hs_stride * tess.output_control_points) {
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
	RemoveIdentities(program.blocks);
	EliminateDeadCode(program.blocks);
	LOGF("%s tessellation lowering: reads=%u writes=%u factors=%u\n",
	     options.stage == ShaderType::Local                 ? "LS"
	     : options.stage == ShaderType::TessellationControl ? "HS"
	                                                       : "TES",
	     reads, writes, factors);
}

} // namespace Libs::Graphics::ShaderRecompiler
