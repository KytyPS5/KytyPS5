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
#include <optional>
#include <set>
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

// The byte stride of one control point in the ring, or nothing when the addressing is not one
// the affine analysis below can describe.
std::optional<uint32_t> ReflectTessellationStride(const Decoder::Program& program, bool local,
                                                  uint32_t control_points,
                                                  uint32_t input_stride) {
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
	std::array<const Decoder::Instruction*, IR::NumVectorRegs> writer {};
	uint32_t stride = 0;
	// Control flow inside the program: a branch joins two paths (or closes a loop) at its target.
	// At a join a register keeps its value only if neither path redefines it, so every register
	// written between a branch and its target is unknown from the target on. The ring addresses
	// are set up before any of that in practice, and one that is not simply fails the affine
	// check below instead of being guessed.
	std::unordered_map<uint32_t, std::vector<uint32_t>> join_invalidates;
	for (const auto& branch: program.instructions) {
		if (!IsDirectBranch(branch.opcode) ||
		    branch.branch_target == program.instructions.back().pc) {
			continue;
		}
		const auto lo   = std::min(branch.pc, branch.branch_target);
		const auto hi   = std::max(branch.pc, branch.branch_target);
		auto&      regs = join_invalidates[branch.branch_target];
		for (const auto& body: program.instructions) {
			if (body.pc < lo || body.pc > hi || body.dst.kind != OperandKind::Vgpr) continue;
			for (uint32_t index = 0; index < std::max(body.data_dwords, 1u); index++) {
				regs.push_back(body.dst.reg + index);
			}
		}
	}
	for (const auto& inst: program.instructions) {
		if (const auto join = join_invalidates.find(inst.pc); join != join_invalidates.end()) {
			for (const auto reg: join->second) {
				if (reg < registers.size()) registers[reg] = {};
			}
		}
		// Every LDS store width the frontend lowers: LS programs write whole attribute vectors
		// (b64/b96/b128 and the two-offset forms), not just dwords, and a stride analysis that
		// only knew the 32-bit forms found no stride and discarded every tessellated draw.
		const bool local_store =
		    inst.opcode == Opcode::DS_WRITE_B32 || inst.opcode == Opcode::DS_WRITE2_B32 ||
		    inst.opcode == Opcode::DS_WRITE2ST64_B32 || inst.opcode == Opcode::DS_WRITE_B64 ||
		    inst.opcode == Opcode::DS_WRITE2_B64 || inst.opcode == Opcode::DS_WRITE2ST64_B64 ||
		    inst.opcode == Opcode::DS_WRITE_B96 || inst.opcode == Opcode::DS_WRITE_B128;
		const bool buffer_store = inst.opcode == Opcode::BUFFER_STORE_DWORD ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX2 ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX3 ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX4;
		const bool control_store =
		    !local && buffer_store && inst.src2.kind == OperandKind::Sgpr && inst.src2.reg == 2u;
		const bool control_read =
		    !local && (inst.opcode == Opcode::DS_READ_B32 || inst.opcode == Opcode::DS_READ2_B32 ||
		               inst.opcode == Opcode::DS_READ2ST64_B32 || inst.opcode == Opcode::DS_READ_B64 ||
		               inst.opcode == Opcode::DS_READ2_B64 ||
		               inst.opcode == Opcode::DS_READ2ST64_B64 || inst.opcode == Opcode::DS_READ_B96 ||
		               inst.opcode == Opcode::DS_READ_B128);
		bool continue_after_read = false;
		if ((local && local_store) || control_store || control_read) {
			const auto address = read(inst.src0);
			// An HS may gather control-point inputs at an address that comes out of memory (bone
			// or index tables). The stride is fixed by the LS that wrote them, and the lowering
			// emits the address at run time, so a data-dependent read constrains nothing here.
			if (control_read && address.kind != Kind::Affine) continue_after_read = true;
			if (!continue_after_read && address.kind != Kind::Affine) {
				const auto* from = inst.src0.kind == OperandKind::Vgpr && inst.src0.reg < writer.size()
				                       ? writer[inst.src0.reg]
				                       : nullptr;
				std::string producer = "none";
				if (from != nullptr) {
					Program one;
					one.instructions.push_back(*from);
					producer = ProgramToString(one);
					while (!producer.empty() && (producer.back() == '\n' || producer.back() == '\r')) {
						producer.pop_back();
					}
				}
				LOGF("%s tessellation address is not affine at pc 0x%08x (v%u written by: %s)\n",
				     local ? "LS" : "HS", inst.pc, inst.src0.reg, producer.c_str());
				return std::nullopt;
			}
			if (address.coefficient != 0u) {
				const auto expected = control_read ? input_stride : stride;
				if ((address.coefficient & 3u) != 0u ||
				    (expected != 0u && expected != address.coefficient)) {
					return std::nullopt;
				}
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
			case Opcode::V_AND_B32:
				// The native packed HS id keeps the patch ordinal in its low byte; masking it out
				// leaves the logical interface's single patch, i.e. zero.
				// The same id masked down to its control point field (bits 8-12) is the control
				// point index scaled by 256, which is how a hull shader addresses its output ring.
				for (const auto& [packed, mask]: {std::pair {lhs, rhs}, std::pair {rhs, lhs}}) {
					if (packed.kind == Kind::PackedControlPoint && mask.kind == Kind::Affine &&
					    mask.coefficient == 0u) {
						if (mask.constant == 0xffu) value = constant(0u);
						if (mask.constant == 0x1f00u) value = {Kind::Affine, 256u, 0u};
					}
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
			case Opcode::V_ADD_NC_U32: value = add(lhs, rhs); break;
			case Opcode::V_ADD3_U32: value = add(add(lhs, rhs), third); break;
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
		for (uint32_t index = 0;
		     index < std::max(inst.data_dwords, 1u) && inst.dst.reg + index < writer.size(); index++) {
			writer[inst.dst.reg + index] = &inst;
		}
	}
	if (stride == 0u) {
		return std::nullopt;
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

bool AnalyzeTessellationPrograms(std::span<const uint32_t> local, std::span<const uint32_t> control,
                                 ShaderTessellationInputInfo& info) {
	const auto       local_program = Decoder::DecodeFrontProgram(local);
	Decoder::Program control_program;
	Decoder::DecodeProgram(control, control_program);
	const auto ls_stride =
	    ReflectTessellationStride(local_program, true, info.input_control_points, 0u);
	if (!ls_stride) {
		LOGF("Tessellation analysis failed: LS stride (input_cp=%u output_cp=%u)\n",
		     info.input_control_points, info.output_control_points);
		return false;
	}
	const auto hs_stride = ReflectTessellationStride(control_program, false,
	                                                 info.output_control_points, *ls_stride);
	if (!hs_stride) {
		LOGF("Tessellation analysis failed: HS stride (input_cp=%u output_cp=%u ls_stride=%u)\n",
		     info.input_control_points, info.output_control_points, *ls_stride);
		return false;
	}
	info.ls_stride = *ls_stride;
	info.hs_stride = *hs_stride;
	LOGF("Tessellation interface: input_cp=%u output_cp=%u ls_stride=%u hs_stride=%u\n",
	     info.input_control_points, info.output_control_points, info.ls_stride, info.hs_stride);
	return true;
}

bool LowerTessellationMemory(IR::Program& program, const CompileOptions& options) {
	using namespace IR;
	if (options.stage != ShaderType::Local && options.stage != ShaderType::TessellationControl &&
	    options.stage != ShaderType::TessellationEvaluation) {
		return true;
	}
	const auto unsupported = [&](const char* what) {
		LOGF("%s tessellation lowering does not model %s\n",
		     options.stage == ShaderType::Local                 ? "LS"
		     : options.stage == ShaderType::TessellationControl ? "HS"
		                                                       : "TES",
		     what);
		return false;
	};
	const auto& tess = options.input_info.vertex->tess;
	if ((tess.domain != 1u && tess.domain != 2u) ||
	    (tess.output_topology != 2u && tess.output_topology != 3u)) {
		// Isolines and point/line output have no mode in the emitter; skip the draw.
		return unsupported("a tessellation domain or output topology without an emitter mode");
	}
	if (tess.ls_stride == 0u || tess.hs_stride == 0u) {
		// The LS/HS interface could not be reflected, so every ring address below would be
		// meaningless; rendering garbage is worse than skipping the draw.
		return unsupported("a tessellation interface that was not reflected");
	}
	// A tessellation-control barrier must be reached by every invocation of the patch, so it cannot
	// sit in front of the guest's patch-constant phase, which runs behind a per-lane branch.
	Block* last_write_block = nullptr;
	Block* read_block       = nullptr;
	bool   seen_self_read = false, post_read_barrier = false;
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
				if (memory.data_bits != 32u ||
				    (options.stage == ShaderType::Local
				         ? !write
				         : options.stage != ShaderType::TessellationControl ||
				               shared != SharedAccess::Read)) {
					return unsupported("this LDS access");
				}
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
				if (memory.data_bits != 32u || memory.formatted || memory.idxen || !memory.offen ||
				    buffer == BufferAccess::Atomic) {
					return unsupported("this ring buffer access");
				}
				write      = buffer == BufferAccess::Write;
				components = BufferComponentCount(inst.GetOpcode());
				address    = inst.Arg(2).Resolve();
				predicate  = inst.Arg(inst.NumArgs() - 1u);
				if (base->Arg(0).U32() == 1u) {
					if (!write || options.stage != ShaderType::TessellationControl) {
						return unsupported("a tessellation factor access outside the HS");
					}
					kind = TessellationAttribute::Factor;
					factors += components;
				} else {
					// A hull shader reads its own control-point outputs back (the patch-constant
					// phase's OutputPatch parameter). That targets the region ControlOutput writes,
					// not the evaluation stage's input. The hardware orders the two phases itself;
					// Vulkan's tessellation-control model needs an explicit barrier before one
					// invocation may observe another's writes (placed below).
					const bool self_read = !write && options.stage == ShaderType::TessellationControl;
					kind = write || self_read ? TessellationAttribute::ControlOutput
					                          : TessellationAttribute::EvaluationInput;
					if (write || self_read ? options.stage != ShaderType::TessellationControl
					                       : options.stage != ShaderType::TessellationEvaluation) {
						return unsupported("a control-point access in the wrong stage");
					}
					// The instruction's own constant offset is part of the address: the
					// evaluation stage reads patch constants as voffset 0 plus an offset past the
					// control points.
					if (address.IsImmediate() &&
					    address.U32() + memory.offset >= tess.hs_stride * tess.output_control_points) {
						// The patch-constant phase writes this region (PatchOutput) and the
						// evaluation stage reads it back as a patch input. A hull shader reading
						// its own patch constants has no such interface.
						if (!write && options.stage != ShaderType::TessellationEvaluation) {
							return unsupported("a patch-constant read");
						}
						kind = TessellationAttribute::PatchOutput;
					}
				}
			} else {
				continue;
			}
			if (kind == TessellationAttribute::ControlOutput) {
				// Vulkan orders a control-point read after another invocation's write only across a
				// control barrier. It goes at the end of the last block that writes control points before
				// the first read (the join point every lane passes), not in front of the read itself.
				// Writes that follow the reads need the opposite ordering and get one in front of the
				// first of them.
				if (write && !seen_self_read) {
					last_write_block = block;
				} else if (!write) {
					if (!seen_self_read) {
						seen_self_read = true;
						if (last_write_block != nullptr && last_write_block != block) {
							last_write_block->PrependNewInst(last_write_block->end(), ValueOpcode::Barrier, {});
						} else {
							block->PrependNewInst(it, ValueOpcode::Barrier, {});
						}
					}
					read_block = block;
				} else if (!post_read_barrier && block != read_block) {
					block->PrependNewInst(it, ValueOpcode::Barrier, {});
					post_read_barrier = true;
				}
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
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler
