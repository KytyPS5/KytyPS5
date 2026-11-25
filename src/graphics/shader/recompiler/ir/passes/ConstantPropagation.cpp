#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

Value Arg(const Inst& inst, size_t index) {
	return inst.Arg(index).Resolve();
}

bool IsImmediate(Value value, Type type) {
	return value.IsImmediate() && value.GetType() == type;
}

void Replace(Inst& inst, Value value) {
	inst.ReplaceUsesWith(value.Resolve());
}

bool HasOneUse(const Inst& inst) {
	// Replacement aliases preserve external Values but emit no additional operation.
	return std::ranges::count_if(inst.Uses(), [](const Use& use) {
		return use.user->GetOpcode() != ValueOpcode::Identity;
	}) == 1;
}

class FoldWorklist {
public:
	struct Location {
		Block::iterator instruction;
		bool queued = false;
	};

	void Register(Block::iterator instruction) {
		Queue(locations.emplace(&*instruction, Location {instruction}).first->second);
	}

	void Queue(Location& location) {
		if (!location.queued) {
			location.queued = true;
			pending.push_back(&location);
		}
	}

	void QueueUsers(const std::vector<Use>& users) {
		for (const auto& use: users) affected.push_back(use.user);
	}

	void FlushUsers() {
		// Pattern folds inspect nested operands even when intermediate operations
		// stay dynamic. Visit their union once per drain, including cyclic Phi edges;
		// the drain left nothing queued, so a queued instruction was visited here.
		for (size_t i = 0; i < affected.size(); ++i) {
			auto* inst = affected[i];
			const auto found = locations.find(inst);
			if (found == locations.end() || found->second.queued) continue;
			Queue(found->second);
			for (const auto& use: inst->Uses()) affected.push_back(use.user);
		}
		affected.clear();
	}

	Block::iterator Prepend(Block& block, Block::iterator before, ValueOpcode opcode,
	                         std::initializer_list<Value> args = {}) {
		const auto result = block.PrependNewInst(before, opcode, args);
		Register(result);
		return result;
	}

	std::unordered_map<Inst*, Location> locations;
	std::vector<Location*>              pending; // Map nodes keep their addresses across rehashes.
	std::unordered_set<Inst*> lowered_ancillary;

private:
	std::vector<Inst*> affected;
};

// Reads an immediate operand of a fold function's parameter type.
template <typename T>
bool ReadImmediate(Value value, T& result) {
	if constexpr (std::is_same_v<T, bool>) {
		if (!IsImmediate(value, Type::U1)) return false;
		result = value.U1();
	} else if constexpr (std::is_same_v<T, uint32_t>) {
		if (!IsImmediate(value, Type::U32)) return false;
		result = value.U32();
	} else {
		static_assert(std::is_same_v<T, uint64_t>);
		if (!IsImmediate(value, Type::U64)) return false;
		result = value.U64();
	}
	return true;
}

// Folds a binary operation on two immediates of the function's parameter types.
template <typename Function, typename Result, typename Lhs, typename Rhs>
bool FoldBinary(Inst& inst, Function function, Result (Function::*)(Lhs, Rhs) const) {
	Lhs lhs {};
	Rhs rhs {};
	if (!ReadImmediate(Arg(inst, 0), lhs) || !ReadImmediate(Arg(inst, 1), rhs)) {
		return false;
	}
	Replace(inst, Value(function(lhs, rhs)));
	return true;
}

template <typename Function>
bool FoldBinary(Inst& inst, Function function) {
	return FoldBinary(inst, function, &Function::operator());
}

bool ReplaceBinaryIdentity(Inst& inst, Type type, uint64_t identity) {
	const auto lhs = Arg(inst, 0);
	const auto rhs = Arg(inst, 1);
	if (IsImmediate(lhs, type) &&
	    (type == Type::U32 ? lhs.U32() == identity : lhs.U64() == identity)) {
		Replace(inst, rhs);
		return true;
	}
	if (IsImmediate(rhs, type) &&
	    (type == Type::U32 ? rhs.U32() == identity : rhs.U64() == identity)) {
		Replace(inst, lhs);
		return true;
	}
	return false;
}

// Whether one operand is the other's negation.
bool IsComplementPair(const Inst& inst, ValueOpcode negation) {
	for (uint32_t arg = 0; arg < 2u; ++arg) {
		const auto* negate = Arg(inst, arg).TryInstruction();
		if (negate != nullptr && negate->GetOpcode() == negation &&
		    Arg(*negate, 0) == Arg(inst, arg ^ 1u)) return true;
	}
	return false;
}

bool FoldSelect(Inst& inst) {
	const auto condition   = Arg(inst, 0);
	const auto true_value  = Arg(inst, 1);
	const auto false_value = Arg(inst, 2);
	if (IsImmediate(condition, Type::U1)) {
		Replace(inst, condition.U1() ? true_value : false_value);
		return true;
	}
	if (true_value == false_value) {
		Replace(inst, true_value);
		return true;
	}
	return false;
}

bool FoldPhi(Inst& inst) {
	Value same;
	for (size_t index = 0; index < inst.NumArgs(); index++) {
		const auto value = Arg(inst, index);
		if (value.TryInstruction() == &inst) {
			continue;
		}
		if (same.IsEmpty()) {
			same = value;
		} else if (same != value) {
			return false;
		}
	}
	if (same.IsEmpty()) {
		return false;
	}
	Replace(inst, same);
	return true;
}

bool FoldBitCast(Inst& inst, ValueOpcode reverse) {
	const auto value = Arg(inst, 0);
	if (IsImmediate(value, Type::F32) && inst.GetOpcode() == ValueOpcode::BitCastU32F32) {
		Replace(inst, Value(std::bit_cast<uint32_t>(value.F32Value())));
		return true;
	}
	if (IsImmediate(value, Type::U32) && inst.GetOpcode() == ValueOpcode::BitCastF32U32) {
		Replace(inst, Value::F32(std::bit_cast<float>(value.U32())));
		return true;
	}
	if (IsImmediate(value, Type::F16) && inst.GetOpcode() == ValueOpcode::BitCastU16F16) {
		Replace(inst, Value(value.F16Bits()));
		return true;
	}
	if (IsImmediate(value, Type::U16) && inst.GetOpcode() == ValueOpcode::BitCastF16U16) {
		Replace(inst, Value::F16(value.U16()));
		return true;
	}
	if (auto* producer = value.TryInstruction();
	    producer != nullptr && producer->GetOpcode() == reverse) {
		Replace(inst, producer->Arg(0));
		return true;
	}
	return false;
}

bool FoldCompositeExtract(Inst& inst, ValueOpcode construct, size_t components) {
	const auto composite = Arg(inst, 0);
	const auto index     = Arg(inst, 1);
	if (!IsImmediate(index, Type::U32) || index.U32() >= components) {
		return false;
	}
	const auto  component = index.U32();
	const auto* producer  = composite.TryInstruction();
	if (IsImmediate(composite, Type::U64)) {
		Replace(inst, Value(static_cast<uint32_t>(composite.U64() >> (component * 32u))));
		return true;
	}
	if (producer != nullptr && producer->GetOpcode() == construct) {
		Replace(inst, producer->Arg(component));
		return true;
	}
	return false;
}

uint32_t KnownNonzeroBits(Value value, uint32_t depth = 0) {
	value = value.Resolve();
	if (value.IsEmpty() || depth > 8u) {
		return 0xffffffffu;
	}
	if (value.IsImmediate()) {
		return value.GetType() == Type::U32 ? value.U32() : 0xffffffffu;
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) {
		return 0xffffffffu;
	}
	switch (inst->GetOpcode()) {
		case ValueOpcode::BitwiseAnd32:
			return KnownNonzeroBits(inst->Arg(0), depth + 1u) &
			       KnownNonzeroBits(inst->Arg(1), depth + 1u);
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
			return KnownNonzeroBits(inst->Arg(0), depth + 1u) |
			       KnownNonzeroBits(inst->Arg(1), depth + 1u);
		case ValueOpcode::ShiftLeftLogical32: {
			const auto shift = Arg(*inst, 1);
			if (IsImmediate(shift, Type::U32) && shift.U32() < 32u) {
				return KnownNonzeroBits(inst->Arg(0), depth + 1u) << shift.U32();
			}
			return 0xffffffffu;
		}
		case ValueOpcode::ShiftRightLogical32: {
			const auto shift = Arg(*inst, 1);
			if (IsImmediate(shift, Type::U32) && shift.U32() < 32u) {
				return KnownNonzeroBits(inst->Arg(0), depth + 1u) >> shift.U32();
			}
			return 0xffffffffu;
		}
		case ValueOpcode::BitFieldUExtract: {
			const auto width = Arg(*inst, 2);
			if (IsImmediate(width, Type::U32) && width.U32() < 32u) {
				return width.U32() == 0u ? 0u : ((1u << width.U32()) - 1u);
			}
			return 0xffffffffu;
		}
		case ValueOpcode::SelectU32:
			return KnownNonzeroBits(inst->Arg(1), depth + 1u) |
			       KnownNonzeroBits(inst->Arg(2), depth + 1u);
		case ValueOpcode::ConvertU32U8:
			return 0xffu;
		case ValueOpcode::ConvertU32U16:
			return 0xffffu;
		default: return 0xffffffffu;
	}
}

bool FoldKnownBitsComparison(Inst& inst) {
	const bool equal = inst.GetOpcode() == ValueOpcode::IEqual32;
	for (uint32_t arm = 0; arm < 2u; ++arm) {
		const auto imm = Arg(inst, arm);
		if (!IsImmediate(imm, Type::U32)) continue;
		const auto other = Arg(inst, arm ^ 1u);
		if ((imm.U32() & ~KnownNonzeroBits(other)) != 0u) {
			Replace(inst, Value(!equal));
			return true;
		}
	}
	return false;
}

void FoldZeroComparison(Block& block, Block::iterator instruction, FoldWorklist& worklist) {
	auto& inst = *instruction;
	Value value;
	for (uint32_t arg = 0; arg < 2u; ++arg)
		if (Arg(inst, arg) == Value(0u)) value = Arg(inst, arg ^ 1u);
	const auto* source = value.TryInstruction();
	if (source == nullptr) return;
	const bool equal = inst.GetOpcode() == ValueOpcode::IEqual32;
	const auto boolean_select = [](const Inst* select) {
		return select != nullptr && select->GetOpcode() == ValueOpcode::SelectU32 &&
		       IsImmediate(Arg(*select, 1), Type::U32) &&
		       IsImmediate(Arg(*select, 2), Type::U32);
	};
	if (boolean_select(source)) {
		const bool when_true = (Arg(*source, 1).U32() == 0u) == equal;
		const bool when_false = (Arg(*source, 2).U32() == 0u) == equal;
		if (when_true == when_false) Replace(inst, Value(when_true));
		else if (when_true) Replace(inst, Arg(*source, 0));
		else Replace(inst, Value(&*worklist.Prepend(block, instruction, ValueOpcode::LogicalNot,
		                                          {Arg(*source, 0)})));
	} else if (source->GetOpcode() == ValueOpcode::BitwiseOr32 &&
	           (boolean_select(Arg(*source, 0).TryInstruction()) ||
	            boolean_select(Arg(*source, 1).TryInstruction()))) {
		// SCC encoded as a DWORD needs no select or bitwise operation when only
		// its zero predicate is consumed. Keep arbitrary DWORDs as zero comparisons.
		const auto lhs = worklist.Prepend(block, instruction, inst.GetOpcode(),
		                                 {Arg(*source, 0), Value(0u)});
		const auto rhs = worklist.Prepend(block, instruction, inst.GetOpcode(),
		                                 {Arg(*source, 1), Value(0u)});
		Replace(inst, Value(&*worklist.Prepend(block, instruction,
		    equal ? ValueOpcode::LogicalAnd : ValueOpcode::LogicalOr, {Value(&*lhs), Value(&*rhs)})));
	}
}

bool MatchEqualityImmediate(Value cond, Value& out_x, uint32_t& out_c);
bool TrySimplifyEqualityMuxChain(Block& block, Block::iterator root_it,
                                 FoldWorklist* worklist = nullptr);

void FoldInstruction(Block& block, Block::iterator instruction,
	                      FoldWorklist& worklist) {
	auto& inst = *instruction;
	switch (inst.GetOpcode()) {
		case ValueOpcode::StoreBufferU32: {
			const auto data = ResolveActiveU32(Arg(inst, 4), Arg(inst, 5));
			if (!data.IsEmpty() && data != inst.Arg(4)) inst.SetArg(4, data);
			return;
		}
		case ValueOpcode::Phi: FoldPhi(inst); return;
		case ValueOpcode::SelectU1:
			if (!FoldSelect(inst) && IsImmediate(Arg(inst, 2), Type::U1) &&
			    !Arg(inst, 2).U1()) {
				auto result = worklist.Prepend(block, instruction, ValueOpcode::LogicalAnd,
				                                   {Arg(inst, 0), Arg(inst, 1)});
				Replace(inst, Value(&*result));
			}
			return;
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
			if (!FoldSelect(inst) && inst.GetOpcode() == ValueOpcode::SelectU32) {
				TrySimplifyEqualityMuxChain(block, instruction, &worklist);
			}
			return;
		case ValueOpcode::FPCmpClass32: {
			const auto mask = Arg(inst, 1);
			if (!IsImmediate(mask, Type::U32)) {
				return;
			}
			switch (mask.U32() & 0x3ffu) {
				case 0u: Replace(inst, Value(false)); break;
				case 0x3ffu: Replace(inst, Value(true)); break;
				case 3u: {
					const auto result = worklist.Prepend(
					    block, instruction, ValueOpcode::FPIsNan32, {Arg(inst, 0)});
					Replace(inst, Value(&*result));
					break;
				}
				default: break;
			}
			return;
		}
		case ValueOpcode::BitFieldInsert: {
			const auto base   = Arg(inst, 0);
			const auto insert = Arg(inst, 1);
			const auto offset = Arg(inst, 2);
			const auto count  = Arg(inst, 3);
			if (IsImmediate(base, Type::U32) && IsImmediate(insert, Type::U32) &&
			    IsImmediate(offset, Type::U32) && IsImmediate(count, Type::U32) &&
			    offset.U32() <= 32u && count.U32() <= 32u - offset.U32()) {
				if (count.U32() == 0u) {
					Replace(inst, base);
					return;
				}
				const auto mask = count.U32() == 32u
				                      ? UINT32_MAX
				                      : ((uint32_t {1} << count.U32()) - 1u) << offset.U32();
				Replace(inst,
				        Value((base.U32() & ~mask) | ((insert.U32() << offset.U32()) & mask)));
			}
			return;
		}
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract: {
			const auto value  = Arg(inst, 0);
			const auto offset = Arg(inst, 1);
			const auto count  = Arg(inst, 2);
			auto* source = value.TryInstruction();
			if (source != nullptr && source->GetOpcode() == ValueOpcode::ShiftLeftLogical32 &&
			    IsImmediate(offset, Type::U32) && IsImmediate(count, Type::U32)) {
				const auto shift = Arg(*source, 1);
				if (IsImmediate(shift, Type::U32) && shift.U32() < 32u &&
				    offset.U32() <= shift.U32() && count.U32() <= shift.U32() - offset.U32()) {
					Replace(inst, Value(0u));
					return;
				}
			}
			if (source != nullptr && source->GetOpcode() == ValueOpcode::GetBuiltin &&
			    source->Arg(0) == Value(static_cast<uint32_t>(StageInputKind::PackedAncillary)) &&
			    IsImmediate(offset, Type::U32) && IsImmediate(count, Type::U32) && count.U32() != 0u) {
				constexpr struct {
					uint32_t       start;
					uint32_t       end;
					StageInputKind kind;
				} fields[] = {{8u, 12u, StageInputKind::SampleId}, {16u, 27u, StageInputKind::Layer}};
				for (const auto& field: fields) {
					if (offset.U32() >= field.start && offset.U32() < field.end &&
					    count.U32() <= field.end - offset.U32()) {
						// Preserve extraction and sign extension while exposing only the used field.
						const auto input = worklist.Prepend(
						    block, instruction, ValueOpcode::GetBuiltin,
						    {Value(static_cast<uint32_t>(field.kind)), Value(0u)});
						inst.SetArg(0, Value(&*input));
						inst.SetArg(1, Value(offset.U32() - field.start));
						worklist.lowered_ancillary.insert(source);
						return;
					}
				}
			}
			if (!IsImmediate(value, Type::U32) || !IsImmediate(offset, Type::U32) ||
			    !IsImmediate(count, Type::U32) || offset.U32() > 32u ||
			    count.U32() > 32u - offset.U32()) {
				return;
			}
			if (count.U32() == 0u) {
				Replace(inst, Value(0u));
			} else if (inst.GetOpcode() == ValueOpcode::BitFieldUExtract) {
				const auto mask =
				    count.U32() == 32u ? UINT32_MAX : (uint32_t {1} << count.U32()) - 1u;
				Replace(inst, Value((value.U32() >> offset.U32()) & mask));
			} else {
				const auto left = 32u - offset.U32() - count.U32();
				const auto bits = value.U32() << left;
				Replace(inst, Value(static_cast<uint32_t>(std::bit_cast<int32_t>(bits) >>
				                                          (left + offset.U32()))));
			}
			return;
		}
		case ValueOpcode::BitCastU16F16: FoldBitCast(inst, ValueOpcode::BitCastF16U16); return;
		case ValueOpcode::BitCastF16U16: FoldBitCast(inst, ValueOpcode::BitCastU16F16); return;
		case ValueOpcode::BitCastU32F32: FoldBitCast(inst, ValueOpcode::BitCastF32U32); return;
		case ValueOpcode::BitCastF32U32: FoldBitCast(inst, ValueOpcode::BitCastU32F32); return;
		case ValueOpcode::ConvertU16U32: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U32)) {
				Replace(inst, Value(static_cast<uint16_t>(value.U32())));
			}
			return;
		}
		case ValueOpcode::ConvertU32U16: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U16)) {
				Replace(inst, Value(static_cast<uint32_t>(value.U16())));
			} else if (auto* producer = value.TryInstruction();
			           producer != nullptr && producer->GetOpcode() == ValueOpcode::ConvertU16U32) {
				Replace(inst, producer->Arg(0));
			}
			return;
		}
		case ValueOpcode::ConvertU8U32: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U32)) {
				Replace(inst, Value(static_cast<uint8_t>(value.U32())));
			}
			return;
		}
		case ValueOpcode::ConvertU32U8: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U8)) {
				Replace(inst, Value(static_cast<uint32_t>(value.U8())));
			} else if (auto* producer = value.TryInstruction();
			           producer != nullptr && producer->GetOpcode() == ValueOpcode::ConvertU8U32) {
				Replace(inst, producer->Arg(0));
			}
			return;
		}
		case ValueOpcode::CompositeExtractU64:
			FoldCompositeExtract(inst, ValueOpcode::CompositeConstructU64, 2);
			return;
		case ValueOpcode::CompositeExtractU32x2:
			FoldCompositeExtract(inst, ValueOpcode::CompositeConstructU32x2, 2);
			return;
		case ValueOpcode::CompositeExtractU32x3:
			FoldCompositeExtract(inst, ValueOpcode::CompositeConstructU32x3, 3);
			return;
		case ValueOpcode::CompositeExtractU32x4:
			FoldCompositeExtract(inst, ValueOpcode::CompositeConstructU32x4, 4);
			return;
		case ValueOpcode::CompositeConstructU64: {
			const auto low  = Arg(inst, 0);
			const auto high = Arg(inst, 1);
			if (IsImmediate(low, Type::U32) && IsImmediate(high, Type::U32)) {
				Replace(inst, Value(static_cast<uint64_t>(low.U32()) |
				                    (static_cast<uint64_t>(high.U32()) << 32u)));
			} else {
				const auto* low_extract  = low.TryInstruction();
				const auto* high_extract = high.TryInstruction();
				if (low_extract != nullptr && high_extract != nullptr &&
				    low_extract->GetOpcode() == ValueOpcode::CompositeExtractU64 &&
				    high_extract->GetOpcode() == ValueOpcode::CompositeExtractU64 &&
				    Arg(*low_extract, 1) == Value(0u) && Arg(*high_extract, 1) == Value(1u) &&
				    Arg(*low_extract, 0) == Arg(*high_extract, 0)) {
					// Guest register pairs need not unpack and repack between wide operations.
					Replace(inst, Arg(*low_extract, 0));
				}
			}
			return;
		}
		case ValueOpcode::IAdd32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a + b; })) {
				ReplaceBinaryIdentity(inst, Type::U32, 0u);
			}
			return;
		case ValueOpcode::IAdd64:
			if (!FoldBinary(inst, [](uint64_t a, uint64_t b) { return a + b; })) {
				ReplaceBinaryIdentity(inst, Type::U64, 0u);
			}
			return;
		case ValueOpcode::ISub32: {
			if (FoldBinary(inst, [](uint32_t a, uint32_t b) { return a - b; })) {
				return;
			}
			const auto rhs = Arg(inst, 1);
			if (IsImmediate(rhs, Type::U32) && rhs.U32() == 0u) {
				Replace(inst, Arg(inst, 0));
			}
			return;
		}
		case ValueOpcode::ISub64: {
			if (FoldBinary(inst, [](uint64_t a, uint64_t b) { return a - b; })) {
				return;
			}
			const auto rhs = Arg(inst, 1);
			if (IsImmediate(rhs, Type::U64) && rhs.U64() == 0u) {
				Replace(inst, Arg(inst, 0));
			}
			return;
		}
		case ValueOpcode::IMul32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a * b; })) {
				ReplaceBinaryIdentity(inst, Type::U32, 1u);
			}
			return;
		case ValueOpcode::WqmU64: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U64)) {
				const auto expand = [](uint32_t word) {
					auto quads = word | (word >> 1u);
					quads |= quads >> 2u;
					return (quads & 0x11111111u) * 0x0fu;
				};
				const auto low  = expand(static_cast<uint32_t>(value.U64()));
				const auto high = expand(static_cast<uint32_t>(value.U64() >> 32u));
				Replace(inst,
				        Value(static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32u)));
			}
			return;
		}
		case ValueOpcode::UDiv32: {
			const auto rhs = Arg(inst, 1);
			if (IsImmediate(rhs, Type::U32) && rhs.U32() == 1u) {
				Replace(inst, Arg(inst, 0));
			} else if (IsImmediate(rhs, Type::U32) && rhs.U32() != 0u) {
				FoldBinary(inst, [](uint32_t a, uint32_t b) { return a / b; });
			}
			return;
		}
		case ValueOpcode::SMulHi:
			FoldBinary(inst, [](uint32_t a, uint32_t b) {
				const auto product = static_cast<int64_t>(std::bit_cast<int32_t>(a)) *
				                     static_cast<int64_t>(std::bit_cast<int32_t>(b));
				return static_cast<uint32_t>(static_cast<uint64_t>(product) >> 32u);
			});
			return;
		case ValueOpcode::UMulHi:
			FoldBinary(inst, [](uint32_t a, uint32_t b) {
				return static_cast<uint32_t>((static_cast<uint64_t>(a) * b) >> 32u);
			});
			return;
		case ValueOpcode::IAbs32: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U32)) {
				Replace(inst,
				        Value((value.U32() & 0x80000000u) != 0u ? 0u - value.U32() : value.U32()));
			}
			return;
		}
		case ValueOpcode::ShiftLeftLogical32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a << (b & 31u); })) {
				const auto shift = Arg(inst, 1);
				if (IsImmediate(shift, Type::U32) && (shift.U32() & 31u) == 0u) {
					Replace(inst, Arg(inst, 0));
				}
			}
			return;
		case ValueOpcode::ShiftRightLogical32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a >> (b & 31u); })) {
				if (IsImmediate(Arg(inst, 0), Type::U32) && Arg(inst, 0).U32() == 0u) {
					Replace(inst, Value(0u));
					return;
				}
				const auto shift = Arg(inst, 1);
				if (IsImmediate(shift, Type::U32) && (shift.U32() & 31u) == 0u) {
					Replace(inst, Arg(inst, 0));
				}
			}
			return;
		case ValueOpcode::ShiftRightArithmetic32:
			FoldBinary(inst, [](uint32_t a, uint32_t b) {
				return static_cast<uint32_t>(std::bit_cast<int32_t>(a) >> (b & 31u));
			});
			return;
		case ValueOpcode::ShiftLeftLogical64:
			if (!FoldBinary(inst, [](uint64_t a, uint32_t b) { return a << (b & 63u); })) {
				const auto shift = Arg(inst, 1);
				if (IsImmediate(shift, Type::U32) && (shift.U32() & 63u) == 0u) {
					Replace(inst, Arg(inst, 0));
				}
			}
			return;
		case ValueOpcode::ShiftRightLogical64:
			if (!FoldBinary(inst, [](uint64_t a, uint32_t b) { return a >> (b & 63u); })) {
				const auto shift = Arg(inst, 1);
				if (IsImmediate(shift, Type::U32) && (shift.U32() & 63u) == 0u) {
					Replace(inst, Arg(inst, 0));
				}
			}
			return;
		case ValueOpcode::ShiftRightArithmetic64:
			FoldBinary(inst, [](uint64_t a, uint32_t b) {
				return static_cast<uint64_t>(std::bit_cast<int64_t>(a) >> (b & 63u));
			});
			return;
		case ValueOpcode::BitwiseAnd32:
			if (FoldBinary(inst, [](uint32_t a, uint32_t b) { return a & b; }) ||
			    ReplaceBinaryIdentity(inst, Type::U32, UINT32_MAX)) return;
			for (uint32_t arm = 0; arm < 2u; ++arm) {
				const auto mask = Arg(inst, arm);
				if (!IsImmediate(mask, Type::U32)) continue;
				const auto other = Arg(inst, arm ^ 1u);
				const auto known = KnownNonzeroBits(other);
				if ((mask.U32() & known) == 0u) {
					Replace(inst, Value(0u));
					return;
				}
				if ((known & ~mask.U32()) == 0u) {
					Replace(inst, other);
					return;
				}
				const auto* shifted = other.TryInstruction();
				if (shifted == nullptr || shifted->GetOpcode() != ValueOpcode::ShiftLeftLogical32) continue;
				const auto shift = Arg(*shifted, 1);
				if (!IsImmediate(shift, Type::U32)) continue;
				const auto shifted_mask = mask.U32() >> (shift.U32() & 31u);
				if (shifted_mask == 0u) {
					Replace(inst, Value(0u));
					return;
				}
				if (!HasOneUse(*shifted) || shift.U32() >= 32u) continue;
				const auto masked = worklist.Prepend(block, instruction, ValueOpcode::BitwiseAnd32,
				    {Arg(*shifted, 0), Value(shifted_mask)});
				Replace(inst, Value(&*worklist.Prepend(block, instruction, ValueOpcode::ShiftLeftLogical32,
				    {Value(&*masked), shift})));
				return;
			}
			return;
		case ValueOpcode::BitwiseAnd64:
			if (!FoldBinary(inst, [](uint64_t a, uint64_t b) { return a & b; })) {
				ReplaceBinaryIdentity(inst, Type::U64, UINT64_MAX);
			}
			return;
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
			if (!FoldBinary(inst, [opcode = inst.GetOpcode()](uint32_t a, uint32_t b) {
				    return opcode == ValueOpcode::BitwiseOr32 ? a | b : a ^ b;
			    }) && !ReplaceBinaryIdentity(inst, Type::U32, 0u) &&
			    IsComplementPair(inst, ValueOpcode::BitwiseNot32)) {
				Replace(inst, Value(UINT32_MAX));
			}
			return;
		case ValueOpcode::BitwiseNot32: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U32)) {
				Replace(inst, Value(~value.U32()));
			} else if (auto* producer = value.TryInstruction();
			           producer != nullptr && producer->GetOpcode() == ValueOpcode::BitwiseNot32) {
				Replace(inst, producer->Arg(0));
			}
			return;
		}
		case ValueOpcode::BitReverse32: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U32)) {
				uint32_t source = value.U32(), reversed = 0;
				for (uint32_t bit = 0; bit < 32u; ++bit, source >>= 1u) {
					reversed = (reversed << 1u) | (source & 1u);
				}
				Replace(inst, Value(reversed));
			}
			return;
		}
		case ValueOpcode::BitCount32:
		case ValueOpcode::FindILsb32: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U32)) {
				const auto bits = value.U32();
				Replace(inst, Value(static_cast<uint32_t>(
				    inst.GetOpcode() == ValueOpcode::BitCount32 ? std::popcount(bits)
				    : bits == 0u                                ? -1
				                                                : std::countr_zero(bits))));
			}
			return;
		}
		case ValueOpcode::SMin32:
		case ValueOpcode::SMax32:
			FoldBinary(inst, [opcode = inst.GetOpcode()](uint32_t a, uint32_t b) {
				const auto lhs = std::bit_cast<int32_t>(a);
				const auto rhs = std::bit_cast<int32_t>(b);
				const bool take_a = opcode == ValueOpcode::SMin32 ? lhs < rhs : lhs > rhs;
				return take_a ? a : b;
			});
			return;
		case ValueOpcode::UMin32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return std::min(a, b); })) {
				ReplaceBinaryIdentity(inst, Type::U32, ~0u);
			}
			return;
		case ValueOpcode::UMax32:
			FoldBinary(inst, [](uint32_t a, uint32_t b) { return std::max(a, b); });
			return;
		case ValueOpcode::IEqual32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a == b; }) &&
			    !FoldKnownBitsComparison(inst))
				FoldZeroComparison(block, instruction, worklist);
			return;
		case ValueOpcode::INotEqual32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a != b; }) &&
			    !FoldKnownBitsComparison(inst))
				FoldZeroComparison(block, instruction, worklist);
			return;
		case ValueOpcode::ULessThan32:
			FoldBinary(inst, [](uint32_t a, uint32_t b) { return a < b; });
			return;
		case ValueOpcode::ULessThanEqual32:
			FoldBinary(inst, [](uint32_t a, uint32_t b) { return a <= b; });
			return;
		case ValueOpcode::UGreaterThan32:
			FoldBinary(inst, [](uint32_t a, uint32_t b) { return a > b; });
			return;
		case ValueOpcode::UGreaterThanEqual32:
			FoldBinary(inst, [](uint32_t a, uint32_t b) { return a >= b; });
			return;
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::SGreaterThanEqual32:
			FoldBinary(inst, [opcode = inst.GetOpcode()](uint32_t a, uint32_t b) {
				const auto lhs = std::bit_cast<int32_t>(a);
				const auto rhs = std::bit_cast<int32_t>(b);
				switch (opcode) {
					case ValueOpcode::SLessThan32: return lhs < rhs;
					case ValueOpcode::SLessThanEqual32: return lhs <= rhs;
					case ValueOpcode::SGreaterThan32: return lhs > rhs;
					default: return lhs >= rhs;
				}
			});
			return;
		case ValueOpcode::IEqual64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) { return a == b; });
			return;
		case ValueOpcode::INotEqual64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) { return a != b; });
			return;
		case ValueOpcode::ULessThan64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) { return a < b; });
			return;
		case ValueOpcode::UGreaterThan64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) { return a > b; });
			return;
		case ValueOpcode::ULessThanEqual64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) { return a <= b; });
			return;
		case ValueOpcode::UGreaterThanEqual64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) { return a >= b; });
			return;
		case ValueOpcode::SLessThanEqual64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) {
				return std::bit_cast<int64_t>(a) <= std::bit_cast<int64_t>(b);
			});
			return;
		case ValueOpcode::SLessThan64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) {
				return std::bit_cast<int64_t>(a) < std::bit_cast<int64_t>(b);
			});
			return;
		case ValueOpcode::LogicalAnd:
			if (!FoldBinary(inst, [](bool a, bool b) { return a && b; })) {
				const auto lhs = Arg(inst, 0);
				const auto rhs = Arg(inst, 1);
				const auto simplify = [&](Value assumption, Value expression) {
					const auto* test = expression.TryInstruction();
					if (test == nullptr) return false;
					const auto replace = [&](Value value) {
						inst.SetArg(assumption == lhs ? 1u : 0u, value);
						return true;
					};
					const auto negate = [&](Value value) {
						return Value(&*worklist.Prepend(block, instruction, ValueOpcode::LogicalNot, {value}));
					};
					const bool inverse = test->GetOpcode() == ValueOpcode::LogicalNot;
					if (inverse) {
						test = Arg(*test, 0).TryInstruction();
						if (test == nullptr) return false;
						if (test->GetOpcode() == ValueOpcode::LogicalAnd && HasOneUse(*test))
							for (uint32_t arg = 0; arg < 2u; ++arg)
								if (Arg(*test, arg) == assumption)
									return replace(negate(Arg(*test, arg ^ 1u)));
					}
					if (test->GetOpcode() == ValueOpcode::IEqual32 ||
					    test->GetOpcode() == ValueOpcode::INotEqual32) {
						for (uint32_t arg = 0; arg < 2u; ++arg) {
							if (Arg(*test, arg) != Value(0u)) continue;
							const auto original = Arg(*test, arg ^ 1u);
							const auto active = ResolveActiveU32(original, assumption);
							if (!active.IsEmpty() && active != original) {
								const auto comparison = Value(&*worklist.Prepend(block, instruction,
								    test->GetOpcode(), {active, Value(0u)}));
								return replace(inverse ? negate(comparison) : comparison);
							}
						}
					}
					if (inverse || test->GetOpcode() != ValueOpcode::LogicalOr) return false;
					for (uint32_t arg = 0; arg < 2u; ++arg) {
						const auto* negation = Arg(*test, arg).TryInstruction();
						if (negation != nullptr && negation->GetOpcode() == ValueOpcode::LogicalNot &&
						    Arg(*negation, 0) == assumption) return replace(Arg(*test, arg ^ 1u));
					}
					return false;
				};
				if (simplify(lhs, rhs) || simplify(rhs, lhs)) return;
				if (IsImmediate(lhs, Type::U1)) {
					Replace(inst, lhs.U1() ? rhs : lhs);
				} else if (IsImmediate(rhs, Type::U1)) {
					Replace(inst, rhs.U1() ? lhs : rhs);
				}
			}
			return;
		case ValueOpcode::LogicalOr:
			if (!FoldBinary(inst, [](bool a, bool b) { return a || b; })) {
				const auto lhs = Arg(inst, 0);
				const auto rhs = Arg(inst, 1);
				if (IsImmediate(lhs, Type::U1)) {
					Replace(inst, lhs.U1() ? lhs : rhs);
				} else if (IsImmediate(rhs, Type::U1)) {
					Replace(inst, rhs.U1() ? rhs : lhs);
				} else if (IsComplementPair(inst, ValueOpcode::LogicalNot)) {
					Replace(inst, Value(true));
				} else {
					const auto* left = lhs.TryInstruction();
					const auto* right = rhs.TryInstruction();
					if (left == nullptr || right == nullptr ||
					    left->GetOpcode() != ValueOpcode::LogicalAnd ||
					    right->GetOpcode() != ValueOpcode::LogicalAnd ||
					    (!HasOneUse(*left) && !HasOneUse(*right))) return;
					// Factor a shared execution predicate without retaining more operations.
					for (uint32_t a = 0; a < 2u; ++a) {
						for (uint32_t b = 0; b < 2u; ++b) {
							const auto common = Arg(*left, a);
							if (common != Arg(*right, b)) continue;
							const auto either = Value(&*worklist.Prepend(block, instruction,
							    ValueOpcode::LogicalOr, {Arg(*left, a ^ 1u), Arg(*right, b ^ 1u)}));
							Replace(inst, Value(&*worklist.Prepend(block, instruction,
							    ValueOpcode::LogicalAnd, {common, either})));
							return;
						}
					}
				}
			}
			return;
		case ValueOpcode::LogicalXor:
			if (!FoldBinary(inst, [](bool a, bool b) { return a != b; })) {
				const auto lhs = Arg(inst, 0);
				const auto rhs = Arg(inst, 1);
				if (IsImmediate(lhs, Type::U1) && !lhs.U1()) {
					Replace(inst, rhs);
				} else if (IsImmediate(rhs, Type::U1) && !rhs.U1()) {
					Replace(inst, lhs);
				}
			}
			return;
		case ValueOpcode::ConditionRef: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U1)) Replace(inst, value);
			return;
		}
		case ValueOpcode::LogicalNot: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U1)) {
				Replace(inst, Value(!value.U1()));
			} else if (auto* producer = value.TryInstruction();
			           producer != nullptr && producer->GetOpcode() == ValueOpcode::LogicalNot) {
				Replace(inst, producer->Arg(0));
			}
			return;
		}
		default: return;
	}
}

// Full and empty masks have a constant predicate in either wave size. In wave32,
// ballot projection also survives scalar EXEC operations and loop-carried mask Phis.
class LaneMaskProjection {
public:
	explicit LaneMaskProjection(FoldWorklist& worklist): m_worklist(worklist) {}

	void Fold(Inst& inst, uint32_t wave_size) {
		if (inst.GetOpcode() != ValueOpcode::INotEqual32 || !Immediate(Arg(inst, 1), 0u)) return;
		const auto* bit = Arg(inst, 0).TryInstruction();
		if (bit == nullptr || bit->GetOpcode() != ValueOpcode::BitwiseAnd32 ||
		    !Immediate(Arg(*bit, 1), 1u)) return;
		const auto* shift = Arg(*bit, 0).TryInstruction();
		if (shift == nullptr || shift->GetOpcode() != ValueOpcode::ShiftRightLogical32) return;
		const auto* index = Arg(*shift, 1).TryInstruction();
		if (index == nullptr || index->GetOpcode() != ValueOpcode::BitwiseAnd32 ||
		    !Immediate(Arg(*index, 1), 31u)) return;
		const auto* lane = Arg(*index, 0).TryInstruction();
		if (lane == nullptr || lane->GetOpcode() != ValueOpcode::LaneId) return;
		const auto mask = Arg(*shift, 0);
		if (Immediate(mask, 0u) || Immediate(mask, UINT32_MAX)) {
			Replace(inst, Value(mask.U32() != 0u));
			return;
		}
		if (wave_size != 32u) return;
		m_visited.clear();
		m_grounded = false;
		if (CanProject(mask) && m_grounded) Replace(inst, Project(mask));
	}

private:
	static bool Immediate(Value value, uint32_t expected) {
		return IsImmediate(value, Type::U32) && value.U32() == expected;
	}

	static Value BallotPredicate(const Inst& inst) {
		if (inst.GetOpcode() != ValueOpcode::CompositeExtractU32x4 ||
		    !Immediate(Arg(inst, 1), 0u)) return {};
		const auto* source = Arg(inst, 0).TryInstruction();
		if (source == nullptr) return {};
		// Helper lanes keep the per-lane EXEC, so the live mask projects to true.
		if (source->GetOpcode() == ValueOpcode::LiveMask) return Value(true);
		return source->GetOpcode() == ValueOpcode::Ballot ? Arg(*source, 0) : Value {};
	}

	bool CanProject(Value value) {
		if (value.IsImmediate()) {
			const bool supported = Immediate(value, 0u) || Immediate(value, UINT32_MAX);
			m_grounded |= supported;
			return supported;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return false;
		if (m_values.contains(inst) || !BallotPredicate(*inst).IsEmpty()) {
			m_grounded = true;
			return true;
		}
		if (!m_visited.insert(inst).second) return true;
		const auto op = inst->GetOpcode();
		if (op != ValueOpcode::BitwiseAnd32 && op != ValueOpcode::BitwiseOr32 &&
		    op != ValueOpcode::BitwiseNot32 && op != ValueOpcode::SelectU32 && op != ValueOpcode::Phi)
			return false;
		for (size_t arg = op == ValueOpcode::SelectU32 ? 1u : 0u; arg < inst->NumArgs(); ++arg) {
			if (!CanProject(Arg(*inst, arg))) return false;
		}
		return inst->NumArgs() != 0;
	}

	Value Project(Value value) {
		if (value.IsImmediate()) return Value(value.U32() != 0u);
		auto* source = value.TryInstruction();
		if (const auto found = m_values.find(source); found != m_values.end()) return found->second.Resolve();
		if (const auto predicate = BallotPredicate(*source); !predicate.IsEmpty()) return predicate;
		auto* block = source->Parent();
		const auto where = m_worklist.locations.at(source).instruction;
		const auto op = source->GetOpcode() == ValueOpcode::BitwiseAnd32 ? ValueOpcode::LogicalAnd
		              : source->GetOpcode() == ValueOpcode::BitwiseOr32 ? ValueOpcode::LogicalOr
		              : source->GetOpcode() == ValueOpcode::BitwiseNot32 ? ValueOpcode::LogicalNot
		              : source->GetOpcode() == ValueOpcode::SelectU32 ? ValueOpcode::SelectU1
		                                                               : ValueOpcode::Phi;
		auto result = op == ValueOpcode::Phi ? m_worklist.Prepend(*block, where, op)
		            : op == ValueOpcode::LogicalNot
		                ? m_worklist.Prepend(*block, where, op, {Value(false)})
		            : op == ValueOpcode::SelectU1
		                ? m_worklist.Prepend(*block, where, op, {Arg(*source, 0), Value(false), Value(false)})
		                : m_worklist.Prepend(*block, where, op, {Value(false), Value(false)});
		m_values.emplace(source, Value(&*result));
		if (op == ValueOpcode::Phi) result->SetFlags(Type::U1);
		for (size_t arg = op == ValueOpcode::SelectU1 ? 1u : 0u; arg < source->NumArgs(); ++arg) {
			const auto projected = Project(Arg(*source, arg));
			if (op == ValueOpcode::Phi) result->AddPhiOperand(source->PhiBlock(arg), projected);
			else result->SetArg(arg, projected);
		}
		FoldInstruction(*block, result, m_worklist);
		return Value(&*result).Resolve();
	}

	FoldWorklist& m_worklist;
	std::unordered_set<const Inst*> m_visited;
	std::unordered_map<const Inst*, Value> m_values;
	bool m_grounded = false;
};

} // namespace

void ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size) {
	FoldWorklist worklist;
	LaneMaskProjection mask_projection(worklist);
	for (auto* block: blocks)
		for (auto inst = block->begin(); inst != block->end(); ++inst) worklist.Register(inst);
	std::vector<Use> users;
	std::vector<Value> operands;
	const auto drain = [&] {
		do {
			for (size_t i = 0; i < worklist.pending.size(); ++i) {
				auto& location    = *worklist.pending[i];
				location.queued = false;
				auto*      inst   = &*location.instruction;
				const auto opcode = inst->GetOpcode();
				if (opcode == ValueOpcode::Identity || opcode == ValueOpcode::Void) continue;
				users.assign(inst->Uses().begin(), inst->Uses().end());
				operands.clear();
				for (size_t arg = 0; arg < inst->NumArgs(); ++arg) operands.push_back(inst->Arg(arg));
				mask_projection.Fold(*inst, wave_size);
				FoldInstruction(*inst->Parent(), location.instruction, worklist);
				bool changed = opcode != inst->GetOpcode() || operands.size() != inst->NumArgs();
				for (size_t arg = 0; !changed && arg < operands.size(); ++arg)
					changed = operands[arg] != inst->Arg(arg);
				if (changed) {
					worklist.QueueUsers(users);
					worklist.Queue(location);
				}
			}
			worklist.pending.clear();
			worklist.FlushUsers();
		} while (!worklist.pending.empty());
	};
	drain();
	// Normalize retained PHI/select values only after every supported field read has
	// been lowered; direct raw consumers remain unsupported.
	bool normalized;
	do {
		normalized = false;
		for (auto* source: worklist.lowered_ancillary) {
			if (source->GetOpcode() == ValueOpcode::Identity) continue;
			const bool retained_only = std::ranges::all_of(source->Uses(), [](const Use& use) {
				const auto& user = *use.user;
				if (!user.HasUses() && !user.MayHaveSideEffects()) {
					return true;
				}
				return user.GetOpcode() == ValueOpcode::Phi ||
				       (user.GetOpcode() == ValueOpcode::SelectU32 && use.operand == 2u);
			});
			if (retained_only) {
				worklist.QueueUsers(source->Uses());
				Replace(*source, Value(0u));
				normalized = true;
			}
		}
		drain();
	} while (normalized);
	// Repeated register reads share a bitcast only within the same SSA block.
	std::unordered_map<const Inst*, Inst*> bitcasts;
	for (auto* block: blocks) {
		bitcasts.clear();
		for (auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::BitCastF32U32 &&
			    inst.GetOpcode() != ValueOpcode::BitCastU32F32) continue;
			const auto* source = Arg(inst, 0).TryInstruction();
			if (source == nullptr) continue;
			const auto [entry, inserted] = bitcasts.try_emplace(source, &inst);
			if (!inserted) Replace(inst, Value(entry->second));
		}
	}
}

namespace {

bool IsPurePerLaneAlu(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::Phi:
		case ValueOpcode::Identity:
		case ValueOpcode::BitCastU16F16:
		case ValueOpcode::BitCastF16U16:
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::BitCastU64F64:
		case ValueOpcode::BitCastF64U64:
		case ValueOpcode::ConvertU16U32:
		case ValueOpcode::ConvertU32U16:
		case ValueOpcode::ConvertU8U32:
		case ValueOpcode::ConvertU32U8:
		case ValueOpcode::ConvertF32F16:
		case ValueOpcode::ConvertF16F32:
		case ValueOpcode::ConvertS32F32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32S32:
		case ValueOpcode::ConvertF64S32:
		case ValueOpcode::ConvertF32F64:
		case ValueOpcode::ConvertF64F32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::ConvertF64U32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeConstructU32x3:
		case ValueOpcode::CompositeConstructF32x2:
		case ValueOpcode::CompositeConstructU32x4:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::CompositeExtractU32x3:
		case ValueOpcode::CompositeExtractU32x4:
		case ValueOpcode::PackHalf2x16:
		case ValueOpcode::PackSnorm2x16:
		case ValueOpcode::PackUnorm2x16:
		case ValueOpcode::PackFloat2x16Rtz:
		case ValueOpcode::FPAbs32:
		case ValueOpcode::FPNeg32:
		case ValueOpcode::FPSaturate32:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectF32:
		case ValueOpcode::SelectF64:
		case ValueOpcode::SelectU32:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::SMulHi:
		case ValueOpcode::UMulHi:
		case ValueOpcode::IAbs32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::BitReverse32:
		case ValueOpcode::BitCount32:
		case ValueOpcode::FindUMsb32:
		case ValueOpcode::FindILsb32:
		case ValueOpcode::SMin32:
		case ValueOpcode::UMin32:
		case ValueOpcode::SMax32:
		case ValueOpcode::UMax32:
		case ValueOpcode::SMinTri32:
		case ValueOpcode::UMinTri32:
		case ValueOpcode::SMaxTri32:
		case ValueOpcode::UMaxTri32:
		case ValueOpcode::SMedTri32:
		case ValueOpcode::UMedTri32:
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThan64:
		case ValueOpcode::SLessThanEqual64:
		case ValueOpcode::ULessThanEqual64:
		case ValueOpcode::UGreaterThanEqual64:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThan64:
		case ValueOpcode::IEqual32:
		case ValueOpcode::IEqual64:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::UGreaterThan64:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::INotEqual64:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::UGreaterThanEqual32:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdEqual32:
		case ValueOpcode::FPOrdEqual64:
		case ValueOpcode::FPOrdLessThanEqual64:
		case ValueOpcode::FPOrdGreaterThanEqual64:
		case ValueOpcode::FPOrdNotEqual64:
		case ValueOpcode::FPOrdLessThan64:
		case ValueOpcode::FPOrdGreaterThan64:
		case ValueOpcode::FPUnordEqual64:
		case ValueOpcode::FPUnordNotEqual64:
		case ValueOpcode::FPUnordLessThan64:
		case ValueOpcode::FPUnordGreaterThan64:
		case ValueOpcode::FPUnordLessThanEqual64:
		case ValueOpcode::FPUnordGreaterThanEqual64:
		case ValueOpcode::FPUnordEqual32:
		case ValueOpcode::FPOrdNotEqual32:
		case ValueOpcode::FPUnordNotEqual32:
		case ValueOpcode::FPOrdLessThan32:
		case ValueOpcode::FPUnordLessThan32:
		case ValueOpcode::FPOrdGreaterThan32:
		case ValueOpcode::FPUnordGreaterThan32:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPUnordLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPUnordGreaterThanEqual32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPIsNan64:
		case ValueOpcode::FPCmpClass32:
		case ValueOpcode::FPCmpClass16:
		case ValueOpcode::FPAdd32:
		case ValueOpcode::FPSub32:
		case ValueOpcode::FPFma32:
		case ValueOpcode::FPMad32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPAdd64:
		case ValueOpcode::FPMul64:
		case ValueOpcode::FPFma64:
		case ValueOpcode::FPRecip64:
		case ValueOpcode::FPMin32:
		case ValueOpcode::FPMax32:
		case ValueOpcode::FPMin64:
		case ValueOpcode::FPMax64:
		case ValueOpcode::FPMinTri32:
		case ValueOpcode::FPMaxTri32:
		case ValueOpcode::FPMedTri32:
		case ValueOpcode::FPRecip32:
		case ValueOpcode::FPRecipIFlag32:
		case ValueOpcode::FPRecipSqrt32:
		case ValueOpcode::FPSqrt:
		case ValueOpcode::FPSin:
		case ValueOpcode::FPCos:
		case ValueOpcode::FPExp2:
		case ValueOpcode::FPLog2:
		case ValueOpcode::FPLdexp:
		case ValueOpcode::FPRoundEven32:
		case ValueOpcode::FPFloor32:
		case ValueOpcode::FPCeil32:
		case ValueOpcode::FPTrunc32:
		case ValueOpcode::FPFloor64:
		case ValueOpcode::FPCeil64:
		case ValueOpcode::FPTrunc64:
		case ValueOpcode::FPFract32:
		case ValueOpcode::FPFract64:
			return true;
		default:
			return false;
	}
}

bool IsExecPredicatedOp(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::SetAttribute:
		case ValueOpcode::WriteSharedU8:
		case ValueOpcode::WriteSharedU16:
		case ValueOpcode::WriteSharedU32:
		case ValueOpcode::WriteSharedU32x2:
		case ValueOpcode::WriteSharedU32x3:
		case ValueOpcode::WriteSharedU32x4:
		case ValueOpcode::LoadSharedU8:
		case ValueOpcode::LoadSharedU16:
		case ValueOpcode::LoadSharedU32:
		case ValueOpcode::LoadSharedU32x2:
		case ValueOpcode::LoadSharedU32x3:
		case ValueOpcode::LoadSharedU32x4:
		case ValueOpcode::LoadBufferU8:
		case ValueOpcode::LoadBufferU16:
		case ValueOpcode::LoadBufferU32:
		case ValueOpcode::LoadBufferU32x2:
		case ValueOpcode::LoadBufferU32x3:
		case ValueOpcode::LoadBufferU32x4:
		case ValueOpcode::StoreBufferU8:
		case ValueOpcode::StoreBufferU16:
		case ValueOpcode::StoreBufferU32:
		case ValueOpcode::StoreBufferU32x2:
		case ValueOpcode::StoreBufferU32x3:
		case ValueOpcode::StoreBufferU32x4:
		case ValueOpcode::LoadAddressU8:
		case ValueOpcode::LoadAddressU16:
		case ValueOpcode::LoadAddressU32:
		case ValueOpcode::LoadDeviceAddressU32:
		case ValueOpcode::StoreAddressU8:
		case ValueOpcode::StoreAddressU16:
		case ValueOpcode::StoreAddressU32:
		case ValueOpcode::ImageRead:
		case ValueOpcode::ImageWrite:
			return true;
		default:
			return false;
	}
}

bool IsZeroBitsImmediate(Value value) {
	value = value.Resolve();
	if (!value.IsImmediate()) {
		return false;
	}
	if (value.GetType() == Type::U32) {
		return value.U32() == 0u;
	}
	if (value.GetType() == Type::F32) {
		return std::bit_cast<uint32_t>(value.F32Value()) == 0u;
	}
	return false;
}

bool IsZeroWhenExecFalseLoad(const Inst& inst, Value& out_exec) {
	switch (inst.GetOpcode()) {
		case ValueOpcode::LoadSharedU8:
		case ValueOpcode::LoadSharedU16:
		case ValueOpcode::LoadSharedU32:
		case ValueOpcode::LoadSharedU32x2:
		case ValueOpcode::LoadSharedU32x3:
		case ValueOpcode::LoadSharedU32x4:
		case ValueOpcode::LoadBufferU8:
		case ValueOpcode::LoadBufferU16:
		case ValueOpcode::LoadBufferU32:
		case ValueOpcode::LoadBufferU32x2:
		case ValueOpcode::LoadBufferU32x3:
		case ValueOpcode::LoadBufferU32x4:
		case ValueOpcode::LoadAddressU8:
		case ValueOpcode::LoadAddressU16:
		case ValueOpcode::LoadAddressU32:
		case ValueOpcode::LoadDeviceAddressU32:
			if (inst.NumArgs() == 0) return false;
			out_exec = Arg(inst, inst.NumArgs() - 1u);
			return true;
		default:
			return false;
	}
}

struct ActiveLaneSimplifier {
	std::unordered_map<const Inst*, uint32_t> order_map;
	std::unordered_map<uint64_t, Value>       false_imm_memo;
	std::unordered_map<uint64_t, bool>        owt_memo;

	static uint64_t MemoKey(uint32_t inst_order, Value cond) {
		cond               = cond.Resolve();
		const auto* c_inst = cond.TryInstruction();
		const auto  c_tag  = c_inst != nullptr
		                         ? reinterpret_cast<uintptr_t>(c_inst)
		                         : static_cast<uintptr_t>(cond.IsImmediate() && cond.GetType() == Type::U1 && cond.U1() ? 1u : 2u);
		return (static_cast<uint64_t>(inst_order) << 32u) ^
		       static_cast<uint64_t>((c_tag >> 3u) ^ (c_tag >> 35u));
	}

	uint32_t OrderOf(const Inst* inst) const {
		const auto found = order_map.find(inst);
		return found != order_map.end() ? found->second : UINT32_MAX;
	}

	Value ResolveWhenFalseImmediate(Value value, Value cond) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			return value;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return {};
		}
		const auto v_ord = OrderOf(inst);
		if (v_ord == UINT32_MAX) {
			return {};
		}
		const auto key = MemoKey(v_ord, cond);
		if (const auto found = false_imm_memo.find(key); found != false_imm_memo.end()) {
			return found->second;
		}
		false_imm_memo.emplace(key, Value {});
		const auto zero_for = [](Type type) -> Value {
			switch (type) {
				case Type::U8:  return Value(uint8_t {0});
				case Type::U16: return Value(uint16_t {0});
				case Type::U32: return Value(0u);
				default:        return {};
			}
		};
		Value result;
		if (inst->GetOpcode() == ValueOpcode::SelectU32 &&
		    PredicateImplies(Arg(*inst, 0), cond)) {
			const auto arg2      = Arg(*inst, 2);
			const auto* arg2_ins = arg2.TryInstruction();
			if (arg2.IsImmediate()) {
				result = arg2;
			} else if (arg2_ins != nullptr && OrderOf(arg2_ins) < v_ord) {
				result = ResolveWhenFalseImmediate(arg2, cond);
			}
		} else if (inst->GetOpcode() == ValueOpcode::Phi && inst->NumArgs() != 0) {
			Value invariant;
			bool  ok = true;
			for (size_t i = 0; i < inst->NumArgs(); ++i) {
				const auto  arm     = Arg(*inst, i);
				const auto* arm_ins = arm.TryInstruction();
				if (!arm.IsImmediate() && (arm_ins == nullptr || OrderOf(arm_ins) >= v_ord)) {
					ok = false;
					break;
				}
				const auto arm_imm = ResolveWhenFalseImmediate(arm, cond);
				if (arm_imm.IsEmpty()) {
					ok = false;
					break;
				}
				if (invariant.IsEmpty()) {
					invariant = arm_imm;
				} else if (invariant != arm_imm) {
					ok = false;
					break;
				}
			}
			if (ok) {
				result = invariant;
			}
		} else if (Value load_exec;
		           IsZeroWhenExecFalseLoad(*inst, load_exec) &&
		           (inst->GetType() == Type::U32 || inst->GetType() == Type::U16 ||
		            inst->GetType() == Type::U8) &&
		           PredicateImplies(load_exec, cond)) {
			result = zero_for(inst->GetType());
		} else if ((inst->GetOpcode() == ValueOpcode::CompositeExtractU32x2 ||
		            inst->GetOpcode() == ValueOpcode::CompositeExtractU32x3 ||
		            inst->GetOpcode() == ValueOpcode::CompositeExtractU32x4) &&
		           inst->NumArgs() >= 1u) {
			if (const auto* comp_inst = Arg(*inst, 0).TryInstruction(); comp_inst != nullptr) {
				if (Value load_exec;
				    IsZeroWhenExecFalseLoad(*comp_inst, load_exec) &&
				    PredicateImplies(load_exec, cond)) {
					result = Value(0u);
				} else if (comp_inst->GetOpcode() == ValueOpcode::ImageRead &&
				           comp_inst->NumArgs() >= 1u &&
				           PredicateImplies(Arg(*comp_inst, comp_inst->NumArgs() - 1u), cond)) {
					result = Value(0u);
				}
			}
		} else if ((inst->GetOpcode() == ValueOpcode::ConvertU32U8 ||
		            inst->GetOpcode() == ValueOpcode::ConvertU32U16 ||
		            inst->GetOpcode() == ValueOpcode::ConvertU8U32 ||
		            inst->GetOpcode() == ValueOpcode::ConvertU16U32 ||
		            inst->GetOpcode() == ValueOpcode::ShiftLeftLogical32 ||
		            inst->GetOpcode() == ValueOpcode::ShiftRightLogical32 ||
		            inst->GetOpcode() == ValueOpcode::ShiftRightArithmetic32 ||
		            inst->GetOpcode() == ValueOpcode::BitFieldUExtract ||
		            inst->GetOpcode() == ValueOpcode::BitFieldSExtract ||
		            inst->GetOpcode() == ValueOpcode::BitReverse32 ||
		            inst->GetOpcode() == ValueOpcode::BitCount32 ||
		            inst->GetOpcode() == ValueOpcode::IAbs32) &&
		           inst->NumArgs() >= 1u) {
			const auto a0     = Arg(*inst, 0);
			const auto* a0_in = a0.TryInstruction();
			if (a0.IsImmediate() || (a0_in != nullptr && OrderOf(a0_in) < v_ord)) {
				if (IsZeroBitsImmediate(ResolveWhenFalseImmediate(a0, cond))) {
					result = zero_for(inst->GetType());
				}
			}
		} else if ((inst->GetOpcode() == ValueOpcode::BitwiseAnd32 ||
		            inst->GetOpcode() == ValueOpcode::IMul32 ||
		            inst->GetOpcode() == ValueOpcode::UMulHi ||
		            inst->GetOpcode() == ValueOpcode::SMulHi ||
		            inst->GetOpcode() == ValueOpcode::UMin32) &&
		           inst->NumArgs() >= 2u) {
			for (size_t i = 0; i < 2u; ++i) {
				const auto ai     = Arg(*inst, i);
				const auto* ai_in = ai.TryInstruction();
				if (ai.IsImmediate() || (ai_in != nullptr && OrderOf(ai_in) < v_ord)) {
					if (IsZeroBitsImmediate(ResolveWhenFalseImmediate(ai, cond))) {
						result = Value(0u);
						break;
					}
				}
			}
		} else if ((inst->GetOpcode() == ValueOpcode::IAdd32 ||
		            inst->GetOpcode() == ValueOpcode::ISub32 ||
		            inst->GetOpcode() == ValueOpcode::BitwiseOr32 ||
		            inst->GetOpcode() == ValueOpcode::BitwiseXor32) &&
		           inst->NumArgs() >= 2u) {
			const auto a0     = Arg(*inst, 0);
			const auto a1     = Arg(*inst, 1);
			const auto* a0_in = a0.TryInstruction();
			const auto* a1_in = a1.TryInstruction();
			if ((a0.IsImmediate() || (a0_in != nullptr && OrderOf(a0_in) < v_ord)) &&
			    (a1.IsImmediate() || (a1_in != nullptr && OrderOf(a1_in) < v_ord))) {
				if (IsZeroBitsImmediate(ResolveWhenFalseImmediate(a0, cond)) &&
				    IsZeroBitsImmediate(ResolveWhenFalseImmediate(a1, cond))) {
					result = Value(0u);
				}
			}
		}
		false_imm_memo[key] = result;
		return result;
	}

	bool IsZeroWhenFalse(Value value, Value cond, uint32_t depth = 0) {
		if (depth > 5u) {
			return false;
		}
		value = value.Resolve();
		if (IsZeroBitsImmediate(value)) {
			return true;
		}
		if (IsZeroBitsImmediate(ResolveWhenFalseImmediate(value, cond))) {
			return true;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return false;
		}
		switch (inst->GetOpcode()) {
			case ValueOpcode::BitCastF32U32:
			case ValueOpcode::BitCastU32F32:
				return IsZeroWhenFalse(inst->Arg(0), cond, depth + 1u);
			case ValueOpcode::FPMul32:
			case ValueOpcode::IMul32:
			case ValueOpcode::BitwiseAnd32:
				return IsZeroWhenFalse(inst->Arg(0), cond, depth + 1u) ||
				       IsZeroWhenFalse(inst->Arg(1), cond, depth + 1u);
			default:
				return false;
		}
	}

	bool PredicateImplies(Value sub, Value sup, uint32_t depth = 0) {
		sub = sub.Resolve();
		sup = sup.Resolve();
		if (sub == sup) {
			return true;
		}
		if (depth > 4u || sub.IsEmpty() || sup.IsEmpty()) {
			return false;
		}
		if (const auto* sub_inst = sub.TryInstruction()) {
			if (sub_inst->GetOpcode() == ValueOpcode::LogicalAnd) {
				if (PredicateImplies(sub_inst->Arg(0), sup, depth + 1u) ||
				    PredicateImplies(sub_inst->Arg(1), sup, depth + 1u)) {
					return true;
				}
			} else if (sub_inst->GetOpcode() == ValueOpcode::LogicalNot) {
				if (const auto* eq = Arg(*sub_inst, 0).TryInstruction();
				    eq != nullptr && eq->GetOpcode() == ValueOpcode::FPOrdEqual32) {
					for (uint32_t arm = 0; arm < 2u; ++arm) {
						if (IsZeroBitsImmediate(eq->Arg(arm)) &&
						    IsZeroWhenFalse(eq->Arg(arm ^ 1u), sup)) {
							return true;
						}
					}
				}
			}
		}
		if (const auto* sup_inst = sup.TryInstruction();
		    sup_inst != nullptr && sup_inst->GetOpcode() == ValueOpcode::LogicalOr) {
			if (PredicateImplies(sub, sup_inst->Arg(0), depth + 1u) ||
			    PredicateImplies(sub, sup_inst->Arg(1), depth + 1u)) {
				return true;
			}
		}
		return false;
	}

	bool OnlyObservedWhenTrue(const Inst& node, Value cond) {
		cond = cond.Resolve();
		if (cond.IsEmpty()) {
			return false;
		}
		const auto n_ord = OrderOf(&node);
		if (n_ord == UINT32_MAX) {
			return false;
		}
		const auto key = MemoKey(n_ord, cond);
		if (const auto found = owt_memo.find(key); found != owt_memo.end()) {
			return found->second;
		}
		owt_memo.emplace(key, false);
		for (const auto& use: node.Uses()) {
			const auto* user = use.user;
			if (user == nullptr || user->Parent() == nullptr) {
				return false;
			}
			const auto u_op    = user->GetOpcode();
			const auto arg_idx = use.operand;
			if (u_op == ValueOpcode::Identity) {
				if (!user->HasUses()) {
					continue;
				}
				if (OrderOf(user) <= n_ord || !OnlyObservedWhenTrue(*user, cond)) {
					return false;
				}
				continue;
			}
			if (IsExecPredicatedOp(u_op)) {
				if (user->NumArgs() >= 2u && arg_idx < user->NumArgs() - 1u &&
				    PredicateImplies(user->Arg(user->NumArgs() - 1u), cond)) {
					continue;
				}
				return false;
			}
			if (OrderOf(user) <= n_ord) {
				return false;
			}
			if (u_op == ValueOpcode::SelectU32 || u_op == ValueOpcode::SelectF32 ||
			    u_op == ValueOpcode::SelectU1 || u_op == ValueOpcode::SelectF64) {
				const auto u_cond = Arg(*user, 0);
				if (arg_idx == 1u && PredicateImplies(u_cond, cond)) {
					continue;
				}
				if ((arg_idx == 1u || arg_idx == 2u) && OnlyObservedWhenTrue(*user, cond)) {
					continue;
				}
				return false;
			}
			if (IsPurePerLaneAlu(u_op) && OnlyObservedWhenTrue(*user, cond)) {
				continue;
			}
			return false;
		}
		owt_memo[key] = true;
		return true;
	}
};

bool MatchEqualityImmediate(Value cond, Value& out_x, uint32_t& out_c) {
	const auto* inst = cond.Resolve().TryInstruction();
	if (inst == nullptr || inst->GetOpcode() != ValueOpcode::IEqual32) {
		return false;
	}
	const auto lhs = Arg(*inst, 0);
	const auto rhs = Arg(*inst, 1);
	Value      x;
	uint32_t   c = 0;
	if (IsImmediate(rhs, Type::U32) && !lhs.IsImmediate()) {
		x = lhs;
		c = rhs.U32();
	} else if (IsImmediate(lhs, Type::U32) && !rhs.IsImmediate()) {
		x = rhs;
		c = lhs.U32();
	} else {
		return false;
	}
	if (const auto* shl = x.TryInstruction();
	    shl != nullptr && shl->GetOpcode() == ValueOpcode::ShiftLeftLogical32) {
		const auto shift = Arg(*shl, 1);
		if (IsImmediate(shift, Type::U32) && shift.U32() > 0u && shift.U32() < 32u) {
			const auto s = shift.U32();
			if ((c & ((1u << s) - 1u)) == 0u &&
			    ((KnownNonzeroBits(Arg(*shl, 0)) >> (32u - s)) == 0u)) {
				x = Arg(*shl, 0);
				c >>= s;
			}
		}
	}
	out_x = x;
	out_c = c;
	return true;
}

bool TrySimplifyEqualityMuxChain(Block& block, Block::iterator root_it, FoldWorklist* worklist) {
	auto& root = *root_it;
	if (root.GetOpcode() != ValueOpcode::SelectU32) {
		return false;
	}
	Value    x;
	uint32_t first_c = 0;
	if (!MatchEqualityImmediate(Arg(root, 0), x, first_c)) {
		return false;
	}
	// Only rewrite chains starting from a root SelectU32 that is not itself the false
	// arm of another SelectU32 testing the same selector x.
	for (const auto& use: root.Uses()) {
		if (use.user == nullptr || use.user->GetOpcode() != ValueOpcode::SelectU32 ||
		    use.operand != 2u) {
			continue;
		}
		Value    parent_x;
		uint32_t parent_c = 0;
		if (MatchEqualityImmediate(Arg(*use.user, 0), parent_x, parent_c) && parent_x == x) {
			return false;
		}
	}
	struct MuxArm {
		Inst*    select_inst = nullptr;
		Value    cond;
		uint32_t c = 0;
		Value    v;
	};
	std::vector<MuxArm> arms;
	arms.push_back({&root, Arg(root, 0), first_c, Arg(root, 1)});
	Value curr = Arg(root, 2);
	while (auto* next = curr.TryInstruction()) {
		if (next->GetOpcode() != ValueOpcode::SelectU32 || next->Parent() != &block ||
		    !HasOneUse(*next)) {
			break;
		}
		Value    arm_x;
		uint32_t arm_c = 0;
		if (!MatchEqualityImmediate(Arg(*next, 0), arm_x, arm_c) || arm_x != x) {
			break;
		}
		if (std::ranges::any_of(arms, [&](const MuxArm& a) { return a.c == arm_c; })) {
			break;
		}
		arms.push_back({next, Arg(*next, 0), arm_c, Arg(*next, 1)});
		curr = Arg(*next, 2);
	}
	if (arms.size() < 2u) {
		return false;
	}
	Value base_default = curr;
	const auto known_mask = KnownNonzeroBits(x);
	const auto bit_count  = static_cast<uint32_t>(std::popcount(known_mask));
	const bool has_zero_arm =
	    std::ranges::any_of(arms, [](const MuxArm& a) { return a.c == 0u; });
	const bool all_within_mask = std::ranges::all_of(
	    arms, [&](const MuxArm& a) { return (a.c & ~known_mask) == 0u; });
	const auto prepend_inst = [&](ValueOpcode op, std::initializer_list<Value> args) {
		return worklist != nullptr ? worklist->Prepend(block, root_it, op, args)
		                           : block.PrependNewInst(root_it, op, args);
	};
	bool rebased = false;
	if (bit_count >= 1u && bit_count <= 5u && !has_zero_arm && all_within_mask &&
	    arms.size() + 1u == (1u << bit_count)) {
		// {c_0, ..., c_{n-1}} U {0} exhausts all possible values of x.
		std::vector<Value> candidates;
		candidates.reserve(arms.size() + 1u);
		for (const auto& arm: arms) {
			candidates.push_back(arm.v);
		}
		candidates.push_back(base_default);
		Value    best_val   = base_default;
		uint32_t best_count = 0;
		for (const auto& cand: candidates) {
			const auto cnt = static_cast<uint32_t>(std::ranges::count(candidates, cand));
			if (cnt > best_count || (cnt == best_count && cand == base_default)) {
				best_count = cnt;
				best_val   = cand;
			}
		}
		if (best_count >= 2u && best_val != base_default) {
			const auto zero_cond =
			    Value(&*prepend_inst(ValueOpcode::IEqual32, {x, Value(0u)}));
			arms.push_back({nullptr, zero_cond, 0u, base_default});
			base_default = best_val;
			rebased      = true;
		}
	}
	std::vector<MuxArm> kept;
	kept.reserve(arms.size());
	for (const auto& arm: arms) {
		if (arm.v != base_default) {
			kept.push_back(arm);
		}
	}
	if (!rebased && kept.size() == arms.size()) {
		return false;
	}
	if (kept.empty()) {
		Replace(root, base_default);
		return true;
	}
	Value built = base_default;
	for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
		built = Value(&*prepend_inst(ValueOpcode::SelectU32, {it->cond, it->v, built}));
	}
	Replace(root, built);
	return true;
}

} // namespace

void SimplifyActiveLaneSelects(const BlockList& blocks) {
	for (auto* block: blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			if (it->GetOpcode() == ValueOpcode::SelectU32) {
				TrySimplifyEqualityMuxChain(*block, it);
			}
		}
	}
	ActiveLaneSimplifier simplifier;
	uint32_t             total_insts = 0;
	for (const auto* block: blocks) {
		for (const auto& inst: *block) {
			simplifier.order_map.emplace(&inst, total_insts++);
		}
	}
	if (total_insts == 0) {
		return;
	}
	for (uint32_t pass = 0; pass < 3u; ++pass) {
		bool changed = false;
		simplifier.false_imm_memo.clear();
		for (auto* block: blocks) {
			std::vector<Inst*> seen;
			for (auto& inst: *block) {
				const auto op = inst.GetOpcode();
				if (op == ValueOpcode::Identity || op == ValueOpcode::Void) {
					continue;
				}
				if (op == ValueOpcode::SelectU32) {
					const auto self_ord = simplifier.OrderOf(&inst);
					auto       c        = Arg(inst, 0);
					auto       t        = Arg(inst, 1);
					auto       f        = Arg(inst, 2);
					if (t == f) {
						Replace(inst, t);
						changed = true;
						continue;
					}
					while (const auto* f_inst = f.TryInstruction()) {
						if (f_inst->GetOpcode() == ValueOpcode::SelectU32 &&
						    simplifier.OrderOf(f_inst) < self_ord &&
						    simplifier.PredicateImplies(Arg(*f_inst, 0), c)) {
							f = Arg(*f_inst, 2);
						} else if (f_inst->GetOpcode() == ValueOpcode::SelectU32 &&
						           simplifier.OrderOf(f_inst) < self_ord &&
						           simplifier.PredicateImplies(c, Arg(*f_inst, 0)) &&
						           Arg(*f_inst, 1) == t) {
							inst.SetArg(0, Arg(*f_inst, 0));
							c       = Arg(*f_inst, 0);
							f       = Arg(*f_inst, 2);
							changed = true;
						} else {
							break;
						}
					}
					if (const auto imm_f = simplifier.ResolveWhenFalseImmediate(f, c);
					    !imm_f.IsEmpty()) {
						f = imm_f;
					}
					if (f != Arg(inst, 2)) {
						inst.SetArg(2, f);
						changed = true;
					}
					while (const auto* t_inst = t.TryInstruction()) {
						if (t_inst->GetOpcode() == ValueOpcode::SelectU32 &&
						    simplifier.OrderOf(t_inst) < self_ord &&
						    simplifier.PredicateImplies(c, Arg(*t_inst, 0))) {
							t = Arg(*t_inst, 1);
						} else {
							break;
						}
					}
					if (t != Arg(inst, 1)) {
						inst.SetArg(1, t);
						changed = true;
					}
					if (!t.IsImmediate()) {
						if (const auto imm_t = simplifier.ResolveWhenFalseImmediate(t, c);
						    !imm_t.IsEmpty() && imm_t == f) {
							Replace(inst, t);
							changed = true;
							continue;
						}
					}
					if (t == f) {
						Replace(inst, t);
						changed = true;
						continue;
					}
				} else if (op == ValueOpcode::BitCastF32U32) {
					if (FoldBitCast(inst, ValueOpcode::BitCastU32F32)) {
						changed = true;
						continue;
					}
				} else if (op == ValueOpcode::BitCastU32F32) {
					if (FoldBitCast(inst, ValueOpcode::BitCastF32U32)) {
						changed = true;
						continue;
					}
				} else if (op == ValueOpcode::Phi) {
					if (FoldPhi(inst)) {
						changed = true;
						continue;
					}
				}
				if (inst.GetOpcode() == ValueOpcode::Phi ||
				    inst.GetOpcode() == ValueOpcode::Identity ||
				    inst.GetOpcode() == ValueOpcode::FPFract32 ||
				    inst.GetOpcode() == ValueOpcode::FPSin ||
				    inst.GetOpcode() == ValueOpcode::FPCos ||
				    !IsPurePerLaneAlu(inst.GetOpcode())) {
					continue;
				}
				Inst* match = nullptr;
				for (auto* candidate: seen) {
					if (candidate->GetOpcode() != inst.GetOpcode() ||
					    candidate->Flags<uint64_t>() != inst.Flags<uint64_t>() ||
					    candidate->NumArgs() != inst.NumArgs()) {
						continue;
					}
					bool same_args = true;
					for (size_t i = 0; i < inst.NumArgs(); ++i) {
						if (Arg(*candidate, i) != Arg(inst, i)) {
							same_args = false;
							break;
						}
					}
					if (same_args) {
						match = candidate;
						break;
					}
				}
				if (match != nullptr) {
					Replace(inst, Value(match));
					changed = true;
				} else {
					seen.push_back(&inst);
				}
			}
		}
		simplifier.owt_memo.clear();
		for (auto block_it = blocks.rbegin(); block_it != blocks.rend(); ++block_it) {
			auto& instructions = (*block_it)->Instructions();
			for (auto inst_it = instructions.rbegin(); inst_it != instructions.rend(); ++inst_it) {
				auto& inst = *inst_it;
				if (inst.GetOpcode() != ValueOpcode::SelectU32 || !inst.HasUses()) {
					continue;
				}
				const auto c = Arg(inst, 0);
				const auto t = Arg(inst, 1);
				if (t.IsImmediate() || t.TryInstruction() == &inst) {
					continue;
				}
				if (simplifier.OnlyObservedWhenTrue(inst, c)) {
					Replace(inst, t);
					changed = true;
					continue;
				}
				if (const auto* and_inst = c.TryInstruction();
				    and_inst != nullptr && and_inst->GetOpcode() == ValueOpcode::LogicalAnd) {
					const auto c0 = Arg(*and_inst, 0);
					const auto c1 = Arg(*and_inst, 1);
					if (simplifier.OnlyObservedWhenTrue(inst, c0)) {
						inst.SetArg(0, c1);
						changed = true;
					} else if (simplifier.OnlyObservedWhenTrue(inst, c1)) {
						inst.SetArg(0, c0);
						changed = true;
					}
				}
			}
		}
		for (auto* block: blocks) {
			for (auto it = block->begin(); it != block->end(); ++it) {
				if (it->GetOpcode() == ValueOpcode::SelectU32 &&
				    TrySimplifyEqualityMuxChain(*block, it)) {
					changed = true;
				}
			}
		}
		if (changed) {
			simplifier.order_map.clear();
			total_insts = 0;
			for (const auto* block: blocks) {
				for (const auto& inst: *block) {
					simplifier.order_map.emplace(&inst, total_insts++);
				}
			}
		} else {
			break;
		}
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
