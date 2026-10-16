#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <vector>
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
		uint64_t visited = 0;
		bool queued = false;
	};

	void Register(Block::iterator instruction) {
		locations.emplace(&*instruction, Location {instruction});
		Queue(&*instruction);
	}

	void Queue(Inst* inst) {
		const auto found = locations.find(inst);
		if (found != locations.end() && !found->second.queued) {
			found->second.queued = true;
			pending.push_back(inst);
		}
	}

	void QueueUsers(const std::vector<Use>& users) {
		++generation;
		affected.clear();
		for (const auto& use: users) affected.push_back({use.user, 1u});
		// Pattern folds inspect nested operands even when intermediate operations
		// stay dynamic. Visit consumers once per change, including cyclic Phi edges.
		// The walk stops at a fixed depth: a fold looks only a few operations down, and a
		// consumer further away is queued anyway when the operation feeding it changes.
		// Without the bound every change of a kernel with a large Phi web walks all of it.
		for (size_t i = 0; i < affected.size(); ++i) {
			auto* inst = affected[i].inst;
			const auto found = locations.find(inst);
			if (found == locations.end() || found->second.visited == generation) continue;
			found->second.visited = generation;
			Queue(inst);
			if (affected[i].depth >= MaxUserDepth) continue;
			for (const auto& use: inst->Uses()) affected.push_back({use.user, affected[i].depth + 1u});
		}
	}

	Block::iterator Prepend(Block& block, Block::iterator before, ValueOpcode opcode,
	                         std::initializer_list<Value> args = {}) {
		const auto result = block.PrependNewInst(before, opcode, args);
		Register(result);
		return result;
	}

	std::unordered_map<Inst*, Location> locations;
	std::vector<Inst*> pending;
	std::unordered_set<Inst*> lowered_ancillary;

private:
	static constexpr uint32_t MaxUserDepth = 4;

	struct Affected {
		Inst*    inst;
		uint32_t depth;
	};

	std::vector<Affected> affected;
	uint64_t generation = 0;
};

template <typename Function>
bool FoldU32(Inst& inst, Function function) {
	const auto lhs = Arg(inst, 0);
	const auto rhs = Arg(inst, 1);
	if (!IsImmediate(lhs, Type::U32) || !IsImmediate(rhs, Type::U32)) {
		return false;
	}
	Replace(inst, Value(static_cast<uint32_t>(function(lhs.U32(), rhs.U32()))));
	return true;
}

template <typename Function>
bool FoldU64(Inst& inst, Function function) {
	const auto lhs = Arg(inst, 0);
	const auto rhs = Arg(inst, 1);
	if (!IsImmediate(lhs, Type::U64) || !IsImmediate(rhs, Type::U64)) {
		return false;
	}
	Replace(inst, Value(static_cast<uint64_t>(function(lhs.U64(), rhs.U64()))));
	return true;
}

template <typename Function>
bool FoldU64Shift(Inst& inst, Function function) {
	const auto value = Arg(inst, 0);
	const auto shift = Arg(inst, 1);
	if (!IsImmediate(value, Type::U64) || !IsImmediate(shift, Type::U32)) {
		return false;
	}
	Replace(inst, Value(static_cast<uint64_t>(function(value.U64(), shift.U32()))));
	return true;
}

template <typename Function>
bool FoldU32Compare(Inst& inst, Function function) {
	const auto lhs = Arg(inst, 0);
	const auto rhs = Arg(inst, 1);
	if (!IsImmediate(lhs, Type::U32) || !IsImmediate(rhs, Type::U32)) {
		return false;
	}
	Replace(inst, Value(function(lhs.U32(), rhs.U32())));
	return true;
}

template <typename Function>
bool FoldU64Compare(Inst& inst, Function function) {
	const auto lhs = Arg(inst, 0);
	const auto rhs = Arg(inst, 1);
	if (!IsImmediate(lhs, Type::U64) || !IsImmediate(rhs, Type::U64)) {
		return false;
	}
	Replace(inst, Value(function(lhs.U64(), rhs.U64())));
	return true;
}

template <typename Function>
bool FoldLogical(Inst& inst, Function function) {
	const auto lhs = Arg(inst, 0);
	const auto rhs = Arg(inst, 1);
	if (!IsImmediate(lhs, Type::U1) || !IsImmediate(rhs, Type::U1)) {
		return false;
	}
	Replace(inst, Value(function(lhs.U1(), rhs.U1())));
	return true;
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
	if (component < 2u && producer != nullptr &&
	    producer->GetOpcode() == ValueOpcode::IAddCarry32) {
		const auto lhs = producer->Arg(0).Resolve();
		const auto rhs = producer->Arg(1).Resolve();
		if (IsImmediate(lhs, Type::U32) && IsImmediate(rhs, Type::U32)) {
			const auto sum = static_cast<uint64_t>(lhs.U32()) + rhs.U32();
			Replace(inst, Value(component == 0u ? static_cast<uint32_t>(sum)
			                                    : static_cast<uint32_t>(sum >> 32u)));
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
		case ValueOpcode::SelectF32: FoldSelect(inst); return;
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
			if (!FoldU32(inst, [](uint32_t a, uint32_t b) { return a + b; })) {
				ReplaceBinaryIdentity(inst, Type::U32, 0u);
			}
			return;
		case ValueOpcode::IAdd64:
			if (!FoldU64(inst, [](uint64_t a, uint64_t b) { return a + b; })) {
				ReplaceBinaryIdentity(inst, Type::U64, 0u);
			}
			return;
		case ValueOpcode::ISub32: {
			if (FoldU32(inst, [](uint32_t a, uint32_t b) { return a - b; })) {
				return;
			}
			const auto rhs = Arg(inst, 1);
			if (IsImmediate(rhs, Type::U32) && rhs.U32() == 0u) {
				Replace(inst, Arg(inst, 0));
			}
			return;
		}
		case ValueOpcode::ISub64: {
			if (FoldU64(inst, [](uint64_t a, uint64_t b) { return a - b; })) {
				return;
			}
			const auto rhs = Arg(inst, 1);
			if (IsImmediate(rhs, Type::U64) && rhs.U64() == 0u) {
				Replace(inst, Arg(inst, 0));
			}
			return;
		}
		case ValueOpcode::IMul32:
			if (!FoldU32(inst, [](uint32_t a, uint32_t b) { return a * b; })) {
				ReplaceBinaryIdentity(inst, Type::U32, 1u);
			}
			return;
		case ValueOpcode::IMul64:
			if (!FoldU64(inst, [](uint64_t a, uint64_t b) { return a * b; })) {
				ReplaceBinaryIdentity(inst, Type::U64, 1u);
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
				FoldU32(inst, [](uint32_t a, uint32_t b) { return a / b; });
			}
			return;
		}
		case ValueOpcode::SMulHi:
			FoldU32(inst, [](uint32_t a, uint32_t b) {
				const auto product = static_cast<int64_t>(std::bit_cast<int32_t>(a)) *
				                     static_cast<int64_t>(std::bit_cast<int32_t>(b));
				return static_cast<uint32_t>(static_cast<uint64_t>(product) >> 32u);
			});
			return;
		case ValueOpcode::UMulHi:
			FoldU32(inst, [](uint32_t a, uint32_t b) {
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
			if (!FoldU32(inst, [](uint32_t a, uint32_t b) { return a << (b & 31u); })) {
				const auto shift = Arg(inst, 1);
				if (IsImmediate(shift, Type::U32) && (shift.U32() & 31u) == 0u) {
					Replace(inst, Arg(inst, 0));
				}
			}
			return;
		case ValueOpcode::ShiftRightLogical32:
			if (!FoldU32(inst, [](uint32_t a, uint32_t b) { return a >> (b & 31u); })) {
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
			FoldU32(inst, [](uint32_t a, uint32_t b) {
				return static_cast<uint32_t>(std::bit_cast<int32_t>(a) >> (b & 31u));
			});
			return;
		case ValueOpcode::ShiftLeftLogical64:
			if (!FoldU64Shift(inst, [](uint64_t a, uint32_t b) { return a << (b & 63u); })) {
				const auto shift = Arg(inst, 1);
				if (IsImmediate(shift, Type::U32) && (shift.U32() & 63u) == 0u) {
					Replace(inst, Arg(inst, 0));
				}
			}
			return;
		case ValueOpcode::ShiftRightLogical64:
			if (!FoldU64Shift(inst, [](uint64_t a, uint32_t b) { return a >> (b & 63u); })) {
				const auto shift = Arg(inst, 1);
				if (IsImmediate(shift, Type::U32) && (shift.U32() & 63u) == 0u) {
					Replace(inst, Arg(inst, 0));
				}
			}
			return;
		case ValueOpcode::ShiftRightArithmetic64:
			FoldU64Shift(inst, [](uint64_t a, uint32_t b) {
				return static_cast<uint64_t>(std::bit_cast<int64_t>(a) >> (b & 63u));
			});
			return;
		case ValueOpcode::BitwiseAnd32:
			if (FoldU32(inst, [](uint32_t a, uint32_t b) { return a & b; }) ||
			    ReplaceBinaryIdentity(inst, Type::U32, UINT32_MAX)) return;
			for (uint32_t arm = 0; arm < 2u; ++arm) {
				const auto mask = Arg(inst, arm);
				if (!IsImmediate(mask, Type::U32)) continue;
				if (mask.U32() == 0u) {
					Replace(inst, Value(0u));
					return;
				}
				const auto* shifted = Arg(inst, arm ^ 1u).TryInstruction();
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
			if (!FoldU64(inst, [](uint64_t a, uint64_t b) { return a & b; })) {
				ReplaceBinaryIdentity(inst, Type::U64, UINT64_MAX);
			}
			return;
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
			if (!FoldU32(inst, [opcode = inst.GetOpcode()](uint32_t a, uint32_t b) {
				    return opcode == ValueOpcode::BitwiseOr32 ? a | b : a ^ b;
			    })) {
				ReplaceBinaryIdentity(inst, Type::U32, 0u);
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
		case ValueOpcode::BitCount32: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U32)) {
				Replace(inst, Value(static_cast<uint32_t>(std::popcount(value.U32()))));
			}
			return;
		}
		case ValueOpcode::BitCount64: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U64)) {
				Replace(inst, Value(static_cast<uint32_t>(std::popcount(value.U64()))));
			}
			return;
		}
		case ValueOpcode::SMin32:
		case ValueOpcode::SMax32:
			FoldU32(inst, [opcode = inst.GetOpcode()](uint32_t a, uint32_t b) {
				const auto lhs = std::bit_cast<int32_t>(a);
				const auto rhs = std::bit_cast<int32_t>(b);
				return opcode == ValueOpcode::SMin32 ? (lhs < rhs ? a : b) : (lhs > rhs ? a : b);
			});
			return;
		case ValueOpcode::UMin32:
			FoldU32(inst, [](uint32_t a, uint32_t b) { return std::min(a, b); });
			return;
		case ValueOpcode::UMax32:
			FoldU32(inst, [](uint32_t a, uint32_t b) { return std::max(a, b); });
			return;
		case ValueOpcode::IEqual32:
			if (!FoldU32Compare(inst, [](uint32_t a, uint32_t b) { return a == b; }))
				FoldZeroComparison(block, instruction, worklist);
			return;
		case ValueOpcode::INotEqual32:
			if (!FoldU32Compare(inst, [](uint32_t a, uint32_t b) { return a != b; }))
				FoldZeroComparison(block, instruction, worklist);
			return;
		case ValueOpcode::ULessThan32:
			FoldU32Compare(inst, [](uint32_t a, uint32_t b) { return a < b; });
			return;
		case ValueOpcode::ULessThanEqual32:
			FoldU32Compare(inst, [](uint32_t a, uint32_t b) { return a <= b; });
			return;
		case ValueOpcode::UGreaterThan32:
			FoldU32Compare(inst, [](uint32_t a, uint32_t b) { return a > b; });
			return;
		case ValueOpcode::UGreaterThanEqual32:
			FoldU32Compare(inst, [](uint32_t a, uint32_t b) { return a >= b; });
			return;
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::SGreaterThanEqual32:
			FoldU32Compare(inst, [opcode = inst.GetOpcode()](uint32_t a, uint32_t b) {
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
			FoldU64Compare(inst, [](uint64_t a, uint64_t b) { return a == b; });
			return;
		case ValueOpcode::INotEqual64:
			FoldU64Compare(inst, [](uint64_t a, uint64_t b) { return a != b; });
			return;
		case ValueOpcode::ULessThan64:
			FoldU64Compare(inst, [](uint64_t a, uint64_t b) { return a < b; });
			return;
		case ValueOpcode::UGreaterThan64:
			FoldU64Compare(inst, [](uint64_t a, uint64_t b) { return a > b; });
			return;
		case ValueOpcode::ULessThanEqual64:
			FoldU64Compare(inst, [](uint64_t a, uint64_t b) { return a <= b; });
			return;
		case ValueOpcode::UGreaterThanEqual64:
			FoldU64Compare(inst, [](uint64_t a, uint64_t b) { return a >= b; });
			return;
		case ValueOpcode::SLessThanEqual64:
			FoldU64Compare(inst, [](uint64_t a, uint64_t b) {
				return std::bit_cast<int64_t>(a) <= std::bit_cast<int64_t>(b);
			});
			return;
		case ValueOpcode::SLessThan64:
			FoldU64Compare(inst, [](uint64_t a, uint64_t b) {
				return std::bit_cast<int64_t>(a) < std::bit_cast<int64_t>(b);
			});
			return;
		case ValueOpcode::LogicalAnd:
			if (!FoldLogical(inst, [](bool a, bool b) { return a && b; })) {
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
			if (!FoldLogical(inst, [](bool a, bool b) { return a || b; })) {
				const auto lhs = Arg(inst, 0);
				const auto rhs = Arg(inst, 1);
				if (IsImmediate(lhs, Type::U1)) {
					Replace(inst, lhs.U1() ? lhs : rhs);
				} else if (IsImmediate(rhs, Type::U1)) {
					Replace(inst, rhs.U1() ? rhs : lhs);
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
			if (!FoldLogical(inst, [](bool a, bool b) { return a != b; })) {
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
		return source != nullptr && source->GetOpcode() == ValueOpcode::Ballot
		           ? Arg(*source, 0) : Value {};
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

// A predicated register write is a SelectU32 over the new and the old value. A float result goes through
// BitCastU32F32 into it and out through BitCastF32U32 into the next float operation, so the chain of float
// instructions pays two bitcasts per step. When every user of the select takes it back as F32 and one side is
// itself a float bitcast, the select is done on F32 values: SelectF32 keeps the bits, and the bitcast pair goes.
uint32_t PromoteSelectBitcasts(const BlockList& blocks) {
	uint32_t promoted = 0;
	for (auto* block: blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto& inst = *it;
			if (inst.GetOpcode() != ValueOpcode::SelectU32 || !inst.HasUses()) continue;
			bool all_float_users = true;
			for (const auto& use: inst.Uses()) {
				if (use.user->GetOpcode() != ValueOpcode::BitCastF32U32) {
					all_float_users = false;
					break;
				}
			}
			if (!all_float_users) continue;
			const auto float_source = [](Value side) -> Value {
				auto* producer = side.Resolve().TryInstruction();
				return producer != nullptr && producer->GetOpcode() == ValueOpcode::BitCastU32F32
				           ? producer->Arg(0) : Value();
			};
			const auto first_float  = float_source(inst.Arg(1));
			const auto second_float = float_source(inst.Arg(2));
			if (first_float.IsEmpty() && second_float.IsEmpty()) continue;
			const auto as_float = [&](Value side, Value known) -> Value {
				if (!known.IsEmpty()) return known;
				return Value(&*block->PrependNewInst(it, ValueOpcode::BitCastF32U32, {side}));
			};
			const auto true_value  = as_float(inst.Arg(1), first_float);
			const auto false_value = as_float(inst.Arg(2), second_float);
			const auto replacement = Value(&*block->PrependNewInst(
			    it, ValueOpcode::SelectF32, {inst.Arg(0), true_value, false_value}));
			const std::vector<Use> users(inst.Uses().begin(), inst.Uses().end());
			for (const auto& use: users) use.user->ReplaceUsesWith(replacement);
			promoted++;
		}
	}
	return promoted;
}

// Register values are U32 in the IR, so a float result that goes through a predicated write (SelectU32) or a join
// (Phi) is bitcast to U32 on the way in and back to F32 on the way out, twice per step of a float chain. A web of
// selects and phis whose users all take the value back as F32 (or are in the web themselves) can hold F32 instead:
// SelectF32 and an F32-typed Phi keep every bit, and the bitcast pairs between the steps go.
//
// The web is the largest set of candidates that stays closed under "every user is a F32 bitcast or in the set".
// Candidates that would cost more bitcasts than they save (many operands that are not float values) are dropped
// and the set is closed again.
uint32_t PromoteFloatWebs(const BlockList& blocks) {
	std::unordered_map<Inst*, Block::iterator> position;
	std::vector<Inst*>                         candidates;
	for (auto* block: blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto& inst = *it;
			const bool select = inst.GetOpcode() == ValueOpcode::SelectU32;
			const bool phi    = inst.GetOpcode() == ValueOpcode::Phi && inst.GetType() == Type::U32;
			if ((select || phi) && inst.HasUses()) {
				position.emplace(&inst, it);
				candidates.push_back(&inst);
			}
		}
	}
	if (candidates.empty()) return 0;
	std::unordered_set<Inst*> web(candidates.begin(), candidates.end());

	// Value operands of a candidate: a select's two values (not its condition), all of a phi's.
	const auto first_operand = [](const Inst& inst) -> size_t {
		return inst.GetOpcode() == ValueOpcode::SelectU32 ? 1u : 0u;
	};
	const auto float_source = [](Value operand) -> Value {
		const auto resolved = operand.Resolve();
		if (resolved.IsImmediate() && resolved.GetType() == Type::U32) return Value::F32(std::bit_cast<float>(resolved.U32()));
		auto* producer = resolved.TryInstruction();
		return producer != nullptr && producer->GetOpcode() == ValueOpcode::BitCastU32F32
		           ? producer->Arg(0) : Value();
	};

	for (bool changed = true; changed;) {
		changed = false;
		for (auto* node: candidates) {
			if (!web.contains(node)) continue;
			bool keep = true;
			std::unordered_set<Inst*> bitcast_users;
			bool needs_back = false;
			for (const auto& use: node->Uses()) {
				if (use.user->GetOpcode() == ValueOpcode::BitCastF32U32) {
					bitcast_users.insert(use.user);
				} else if (web.contains(use.user)) {
					if (use.user->GetOpcode() == ValueOpcode::SelectU32 && use.operand == 0u) { keep = false; break; }
				} else if (use.user->GetOpcode() == ValueOpcode::Phi) {
					keep = false; // the bitcast back would have to sit in a predecessor block
					break;
				} else {
					needs_back = true; // one shared U32 bitcast serves every integer user
				}
			}
			if (keep) {
				int benefit = static_cast<int>(bitcast_users.size()) - (needs_back ? 1 : 0);
				for (size_t index = first_operand(*node); index < node->NumArgs(); index++) {
					const auto operand = node->Arg(index).Resolve();
					auto* producer     = operand.TryInstruction();
					if (producer != nullptr && web.contains(producer)) continue;
					if (float_source(operand).IsEmpty()) {
						if (node->GetOpcode() == ValueOpcode::Phi) { benefit = -1; break; }
						benefit -= 1;
					} else {
						benefit += 1;
					}
				}
				keep = benefit >= 0;
			}
			if (!keep) {
				web.erase(node);
				changed = true;
			}
		}
	}
	if (web.empty()) return 0;

	// Phase A: a replacement SelectF32 per select; a phi becomes F32 in place.
	std::unordered_map<Inst*, Value> promoted;
	for (auto* node: candidates) {
		if (!web.contains(node)) continue;
		if (node->GetOpcode() == ValueOpcode::SelectU32) {
			auto* block = node->Parent();
			const auto made = block->PrependNewInst(position.at(node), ValueOpcode::SelectF32,
			    {node->Arg(0), Value::F32(0.0f), Value::F32(0.0f)});
			position[&*made] = made; // new bitcasts for its operands go in front of it
			promoted.emplace(node, Value(&*made));
		} else {
			node->SetFlags(Type::F32);
			promoted.emplace(node, Value(node));
		}
	}
	// Phase B: float operands.
	const auto as_float = [&](Value operand, Block* block, std::optional<Block::iterator> before) -> Value {
		// A phi operand reaching here is a float value or a web member (checked above), never a new bitcast.
		const auto resolved = operand.Resolve();
		if (auto* producer = resolved.TryInstruction(); producer != nullptr) {
			if (const auto found = promoted.find(producer); found != promoted.end()) return found->second;
		}
		if (const auto known = float_source(resolved); !known.IsEmpty()) return known;
		return Value(&*block->PrependNewInst(*before, ValueOpcode::BitCastF32U32, {resolved}));
	};
	for (auto* node: candidates) {
		if (!web.contains(node)) continue;
		if (node->GetOpcode() == ValueOpcode::SelectU32) {
			auto* replacement = promoted.at(node).TryInstruction();
			auto* block       = node->Parent();
			const auto at     = position.at(replacement);
			const auto first  = as_float(node->Arg(1), block, at);
			const auto second = as_float(node->Arg(2), block, at);
			replacement->SetArg(1, first);
			replacement->SetArg(2, second);
		} else {
			for (size_t index = 0; index < node->NumArgs(); index++) {
				node->SetArg(index, as_float(node->Arg(index), nullptr, std::nullopt));
			}
		}
	}
	// Phase C: the F32 bitcasts of the web values are the values themselves; the remaining users get one U32 bitcast.
	uint32_t count = 0;
	std::unordered_set<Inst*> fresh; // the F32 selects made above already take F32 operands
	for (const auto& [original, value]: promoted) fresh.insert(value.TryInstruction());
	for (auto* node: candidates) {
		if (!web.contains(node)) continue;
		const std::vector<Use> users(node->Uses().begin(), node->Uses().end());
		Value back;
		for (const auto& use: users) {
			if (use.user->GetOpcode() == ValueOpcode::BitCastF32U32) {
				use.user->ReplaceUsesWith(promoted.at(node));
			} else if (!web.contains(use.user) && !fresh.contains(use.user)) {
				if (back.IsEmpty()) {
					auto* block = node->Parent();
					auto  where = position.at(node);
					if (node->GetOpcode() == ValueOpcode::Phi) {
						where = block->begin();
						while (where != block->end() && where->GetOpcode() == ValueOpcode::Phi) ++where;
					}
					back = Value(&*block->PrependNewInst(where, ValueOpcode::BitCastU32F32, {promoted.at(node)}));
				}
				use.user->SetArg(use.operand, back);
			}
		}
		count++;
	}
	return count;
}

uint32_t EliminateDuplicateBitcasts(const BlockList& blocks) {
	// A block with a single predecessor sees everything that predecessor computed, so the available bitcasts
	// carry down such chains instead of restarting in every block.
	struct Available {
		std::unordered_map<Inst*, Inst*> to_float, to_uint;
	};
	uint32_t removed = 0;
	std::unordered_map<Block*, Available> finished;
	for (auto* block: blocks) {
		Available available;
		const auto preds = block->ImmPredecessors();
		if (preds.size() == 1u && preds[0] != block) {
			if (const auto parent = finished.find(preds[0]); parent != finished.end()) available = parent->second;
		}
		for (auto& inst: *block) {
			const auto opcode = inst.GetOpcode();
			if ((opcode != ValueOpcode::BitCastF32U32 && opcode != ValueOpcode::BitCastU32F32) ||
			    !inst.HasUses()) continue;
			auto* source = inst.Arg(0).Resolve().TryInstruction();
			if (source == nullptr) continue;
			auto& table = opcode == ValueOpcode::BitCastF32U32 ? available.to_float : available.to_uint;
			const auto [found, inserted] = table.emplace(source, &inst);
			if (!inserted) {
				// Same source and destination type, earlier in a dominating position: no memory
				// or floating-point operation is changed, and the replacement dominates.
				inst.ReplaceUsesWith(Value(found->second));
				removed++;
			}
		}
		finished.emplace(block, std::move(available));
	}
	return removed;
}
void ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size) {
	FoldWorklist worklist;
	LaneMaskProjection mask_projection(worklist);
	for (auto* block: blocks)
		for (auto inst = block->begin(); inst != block->end(); ++inst) worklist.Register(inst);
	std::vector<Use> users;
	std::vector<Value> operands;
	const auto drain = [&] {
		for (size_t i = 0; i < worklist.pending.size(); ++i) {
			auto* inst = worklist.pending[i];
			auto& location = worklist.locations.at(inst);
			location.queued = false;
			const auto opcode = inst->GetOpcode();
			if (opcode == ValueOpcode::Identity || opcode == ValueOpcode::Void) continue;
			const auto where = location.instruction;
			users.assign(inst->Uses().begin(), inst->Uses().end());
			operands.clear();
			for (size_t arg = 0; arg < inst->NumArgs(); ++arg) operands.push_back(inst->Arg(arg));
			mask_projection.Fold(*inst, wave_size);
			FoldInstruction(*inst->Parent(), where, worklist);
			bool changed = opcode != inst->GetOpcode() || operands.size() != inst->NumArgs();
			for (size_t arg = 0; !changed && arg < operands.size(); ++arg)
				changed = operands[arg] != inst->Arg(arg);
			if (changed) {
				worklist.QueueUsers(users);
				worklist.Queue(inst);
			}
		}
		worklist.pending.clear();
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
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
