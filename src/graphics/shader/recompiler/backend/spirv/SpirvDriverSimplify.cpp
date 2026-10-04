#include "graphics/shader/recompiler/backend/spirv/SpirvDriverSimplify.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <spirv-tools/libspirv.h>
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

namespace {

constexpr size_t MaxSpeculatedBlocks       = 8;
constexpr size_t MaxSpeculatedInstructions = 64;
constexpr int    MaxObservationDepth       = 64;

std::atomic<uint32_t> g_min_words {0};

struct Inst {
	spv::Op               op     = spv::OpNop;
	uint32_t              type   = 0;
	uint32_t              result = 0;
	bool                  glsl   = false;
	std::vector<uint32_t> words;
	std::vector<uint16_t> ids; // word offsets of the id operands, the result excluded
};

struct Block {
	uint32_t              label = 0;
	std::vector<uint32_t> insts; // arena indices; the label itself is not stored
	bool                  removed = false;
};

struct Function {
	std::vector<uint32_t> head;
	std::vector<Block>    blocks;
	uint32_t              end = 0;
};

struct Module {
	uint32_t                               header[5] {};
	std::vector<Inst>                      arena;
	std::vector<uint32_t>                  preamble;
	std::vector<Function>                  functions;
	std::vector<uint32_t>                  def; // id -> arena index + 1
	std::unordered_map<uint32_t, uint32_t> zero_of_type;
	std::vector<uint32_t>                  new_constants;

	Inst&       At(uint32_t index) { return arena[index]; }
	const Inst* Def(uint32_t id) const {
		return id < def.size() && def[id] != 0 ? &arena[def[id] - 1] : nullptr;
	}
	uint32_t AllocateId() {
		const auto id = header[3]++;
		def.push_back(0);
		return id;
	}
	uint32_t Add(Inst inst) {
		arena.push_back(std::move(inst));
		const auto index = static_cast<uint32_t>(arena.size() - 1);
		if (arena[index].result != 0) {
			def[arena[index].result] = index + 1;
		}
		return index;
	}
};

bool IsIdOperand(spv_operand_type_t type) {
	return type == SPV_OPERAND_TYPE_ID || type == SPV_OPERAND_TYPE_TYPE_ID ||
	       type == SPV_OPERAND_TYPE_SCOPE_ID || type == SPV_OPERAND_TYPE_MEMORY_SEMANTICS_ID;
}

spv_result_t OnHeader(void* user, spv_endianness_t, uint32_t magic, uint32_t version,
                      uint32_t generator, uint32_t bound, uint32_t schema) {
	auto& m     = *static_cast<Module*>(user);
	m.header[0] = magic;
	m.header[1] = version;
	m.header[2] = generator;
	m.header[3] = bound;
	m.header[4] = schema;
	m.def.assign(bound, 0);
	return SPV_SUCCESS;
}

spv_result_t OnInstruction(void* user, const spv_parsed_instruction_t* parsed) {
	auto& m = *static_cast<Module*>(user);
	Inst  inst;
	inst.op     = static_cast<spv::Op>(parsed->opcode);
	inst.type   = parsed->type_id;
	inst.result = parsed->result_id;
	inst.glsl   = parsed->ext_inst_type == SPV_EXT_INST_TYPE_GLSL_STD_450;
	inst.words.assign(parsed->words, parsed->words + parsed->num_words);
	for (uint16_t i = 0; i < parsed->num_operands; i++) {
		const auto& operand = parsed->operands[i];
		if (IsIdOperand(operand.type) && operand.num_words == 1) {
			inst.ids.push_back(operand.offset);
		}
	}
	const auto op    = inst.op;
	const auto index = m.Add(std::move(inst));
	if (op == spv::OpFunction) {
		m.functions.emplace_back();
		m.functions.back().head.push_back(index);
	} else if (m.functions.empty()) {
		m.preamble.push_back(index);
	} else if (op == spv::OpLabel) {
		m.functions.back().blocks.push_back({.label = m.At(index).result});
	} else if (op == spv::OpFunctionEnd) {
		m.functions.back().end = index;
	} else if (m.functions.back().blocks.empty()) {
		m.functions.back().head.push_back(index);
	} else {
		m.functions.back().blocks.back().insts.push_back(index);
	}
	return SPV_SUCCESS;
}

bool IsTotalLaneLocal(spv::Op op) {
	switch (op) {
		case spv::OpSNegate:
		case spv::OpFNegate:
		case spv::OpIAdd:
		case spv::OpFAdd:
		case spv::OpISub:
		case spv::OpFSub:
		case spv::OpIMul:
		case spv::OpFMul:
		case spv::OpFDiv:
		case spv::OpFRem:
		case spv::OpFMod:
		case spv::OpVectorTimesScalar:
		case spv::OpDot:
		case spv::OpIAddCarry:
		case spv::OpISubBorrow:
		case spv::OpUMulExtended:
		case spv::OpSMulExtended:
		case spv::OpShiftRightLogical:
		case spv::OpShiftRightArithmetic:
		case spv::OpShiftLeftLogical:
		case spv::OpBitwiseOr:
		case spv::OpBitwiseXor:
		case spv::OpBitwiseAnd:
		case spv::OpNot:
		case spv::OpBitFieldInsert:
		case spv::OpBitFieldSExtract:
		case spv::OpBitFieldUExtract:
		case spv::OpBitReverse:
		case spv::OpBitCount:
		case spv::OpConvertFToU:
		case spv::OpConvertFToS:
		case spv::OpConvertSToF:
		case spv::OpConvertUToF:
		case spv::OpUConvert:
		case spv::OpSConvert:
		case spv::OpFConvert:
		case spv::OpQuantizeToF16:
		case spv::OpBitcast:
		case spv::OpCompositeConstruct:
		case spv::OpCompositeExtract:
		case spv::OpCompositeInsert:
		case spv::OpVectorShuffle:
		case spv::OpCopyObject:
		case spv::OpAny:
		case spv::OpAll:
		case spv::OpIsNan:
		case spv::OpIsInf:
		case spv::OpLogicalEqual:
		case spv::OpLogicalNotEqual:
		case spv::OpLogicalOr:
		case spv::OpLogicalAnd:
		case spv::OpLogicalNot:
		case spv::OpSelect:
		case spv::OpIEqual:
		case spv::OpINotEqual:
		case spv::OpUGreaterThan:
		case spv::OpSGreaterThan:
		case spv::OpUGreaterThanEqual:
		case spv::OpSGreaterThanEqual:
		case spv::OpULessThan:
		case spv::OpSLessThan:
		case spv::OpULessThanEqual:
		case spv::OpSLessThanEqual:
		case spv::OpFOrdEqual:
		case spv::OpFUnordEqual:
		case spv::OpFOrdNotEqual:
		case spv::OpFUnordNotEqual:
		case spv::OpFOrdLessThan:
		case spv::OpFUnordLessThan:
		case spv::OpFOrdGreaterThan:
		case spv::OpFUnordGreaterThan:
		case spv::OpFOrdLessThanEqual:
		case spv::OpFUnordLessThanEqual:
		case spv::OpFOrdGreaterThanEqual:
		case spv::OpFUnordGreaterThanEqual: return true;
		default: return false;
	}
}

bool IsPureGlsl(const Inst& inst) {
	if (inst.op != spv::OpExtInst || !inst.glsl) {
		return false;
	}
	switch (inst.words[4]) {
		case GLSLstd450Modf:
		case GLSLstd450Frexp:
		case GLSLstd450InterpolateAtCentroid:
		case GLSLstd450InterpolateAtSample:
		case GLSLstd450InterpolateAtOffset: return false;
		default: return true;
	}
}

bool IsTotalLaneLocal(const Inst& inst) {
	return IsTotalLaneLocal(inst.op) || IsPureGlsl(inst);
}

bool IsIntegerDivision(spv::Op op) {
	return op == spv::OpUDiv || op == spv::OpSDiv || op == spv::OpUMod || op == spv::OpSRem ||
	       op == spv::OpSMod;
}

bool IsAccessChain(spv::Op op) {
	return op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain;
}

bool IsConstant(const Module& m, uint32_t id) {
	const auto* inst = m.Def(id);
	return inst != nullptr && inst->op == spv::OpConstant;
}

bool IsSafeDivision(const Module& m, const Inst& inst) {
	const auto* divisor = m.Def(inst.words[4]);
	if (divisor == nullptr || divisor->op != spv::OpConstant) {
		return false;
	}
	bool zero = true;
	bool ones = true;
	for (size_t i = 3; i < divisor->words.size(); i++) {
		zero = zero && divisor->words[i] == 0;
		ones = ones && divisor->words[i] == ~0u;
	}
	return !zero && (inst.op == spv::OpUDiv || inst.op == spv::OpUMod || !ones);
}

uint32_t StorageClassOfVariable(const Module& m, uint32_t id) {
	const auto* var = m.Def(id);
	return var != nullptr && var->op == spv::OpVariable ? var->words[3] : ~0u;
}

uint32_t RootVariable(const Module& m, uint32_t pointer) {
	for (int depth = 0; depth < 16; depth++) {
		const auto* inst = m.Def(pointer);
		if (inst == nullptr) {
			return 0;
		}
		if (inst->op == spv::OpVariable) {
			return pointer;
		}
		if (!IsAccessChain(inst->op)) {
			return 0;
		}
		pointer = inst->words[3];
	}
	return 0;
}

// Covered by robustBufferAccess, which the device is created with.
bool IsRobustClass(uint32_t storage) {
	return storage == spv::StorageClassStorageBuffer || storage == spv::StorageClassUniform;
}

// Not covered by robustness: an index into these must stay in bounds.
bool IsPrivateClass(uint32_t storage) {
	return storage == spv::StorageClassWorkgroup || storage == spv::StorageClassFunction ||
	       storage == spv::StorageClassPrivate || storage == spv::StorageClassPushConstant;
}

bool HasConstantDescriptorIndex(const Module& m, uint32_t pointer, uint32_t root) {
	for (int depth = 0; depth < 16; depth++) {
		const auto* inst = m.Def(pointer);
		if (inst == nullptr || inst->op == spv::OpVariable) {
			return true;
		}
		if (!IsAccessChain(inst->op)) {
			return false;
		}
		if (inst->words[3] == root) {
			return inst->words.size() > 4 && IsConstant(m, inst->words[4]);
		}
		pointer = inst->words[3];
	}
	return false;
}

uint32_t ZeroOfType(Module& m, uint32_t type) {
	if (auto it = m.zero_of_type.find(type); it != m.zero_of_type.end()) {
		return it->second;
	}
	const auto* type_inst = m.Def(type);
	if (type_inst == nullptr || type_inst->op != spv::OpTypeInt) {
		return 0;
	}
	Inst zero;
	zero.op     = spv::OpConstant;
	zero.type   = type;
	zero.result = m.AllocateId();
	zero.words  = {0u, type, zero.result};
	zero.words.resize(type_inst->words[2] == 64 ? 5 : 4, 0u);
	zero.words[0] =
	    (static_cast<uint32_t>(zero.words.size()) << spv::WordCountShift) | spv::OpConstant;
	zero.ids = {1};
	m.new_constants.push_back(m.Add(std::move(zero)));
	m.zero_of_type[type] = m.arena[m.new_constants.back()].result;
	return m.zero_of_type[type];
}

Inst MakeSelect(uint32_t type, uint32_t result, uint32_t condition, uint32_t if_true,
                uint32_t if_false) {
	Inst inst;
	inst.op     = spv::OpSelect;
	inst.type   = type;
	inst.result = result;
	inst.words  = {
	    (6u << spv::WordCountShift) | spv::OpSelect, type, result, condition, if_true, if_false};
	inst.ids = {1, 3, 4, 5};
	return inst;
}

struct Region {
	uint32_t condition = 0;
	bool     inverted  = false; // the conditional side is the false target
};

uint32_t ClampIndex(Module& m, const Region& region, uint32_t index_id,
                    std::vector<uint32_t>& out) {
	const auto* index = m.Def(index_id);
	if (index == nullptr || index->type == 0) {
		return 0;
	}
	const auto type = index->type;
	const auto zero = ZeroOfType(m, type);
	if (zero == 0) {
		return 0;
	}
	const auto result = m.AllocateId();
	out.push_back(m.Add(region.inverted
	                        ? MakeSelect(type, result, region.condition, zero, index_id)
	                        : MakeSelect(type, result, region.condition, index_id, zero)));
	return result;
}

enum class Speculation { No, Yes, ClampChain, ClampLoad };

Speculation Classify(const Module& m, const Inst& inst) {
	if (IsTotalLaneLocal(inst)) {
		return Speculation::Yes;
	}
	if (IsIntegerDivision(inst.op)) {
		return IsSafeDivision(m, inst) ? Speculation::Yes : Speculation::No;
	}
	if (IsAccessChain(inst.op)) {
		const auto root    = RootVariable(m, inst.result);
		const auto storage = StorageClassOfVariable(m, root);
		if (IsRobustClass(storage)) {
			return HasConstantDescriptorIndex(m, inst.result, root) ? Speculation::Yes
			                                                        : Speculation::No;
		}
		if (!IsPrivateClass(storage) || inst.words.size() <= 4 || inst.words[3] != root) {
			return Speculation::No;
		}
		for (size_t i = 4; i + 1 < inst.words.size(); i++) {
			if (!IsConstant(m, inst.words[i])) {
				return Speculation::No;
			}
		}
		return IsConstant(m, inst.words.back()) ? Speculation::Yes : Speculation::ClampChain;
	}
	if (inst.op == spv::OpArrayLength) {
		const auto root = RootVariable(m, inst.words[3]);
		return IsRobustClass(StorageClassOfVariable(m, root)) &&
		               HasConstantDescriptorIndex(m, inst.words[3], root)
		           ? Speculation::Yes
		           : Speculation::No;
	}
	if (inst.op == spv::OpLoad) {
		if (inst.words.size() > 4 &&
		    (inst.words[4] & ~static_cast<uint32_t>(spv::MemoryAccessAlignedMask |
		                                            spv::MemoryAccessNontemporalMask)) != 0) {
			return Speculation::No;
		}
		const auto  pointer = inst.words[3];
		const auto* source  = m.Def(pointer);
		const auto  root    = RootVariable(m, pointer);
		const auto  storage = StorageClassOfVariable(m, root);
		if (source == nullptr || (!IsRobustClass(storage) && !IsPrivateClass(storage))) {
			return Speculation::No;
		}
		if (source->op == spv::OpVariable) {
			return Speculation::Yes;
		}
		if (IsRobustClass(storage)) {
			return HasConstantDescriptorIndex(m, pointer, root) ? Speculation::Yes
			                                                    : Speculation::No;
		}
		const auto chain = Classify(m, *source);
		if (chain == Speculation::No) {
			return Speculation::No;
		}
		return chain == Speculation::ClampChain ? Speculation::ClampLoad : Speculation::Yes;
	}
	return Speculation::No;
}

bool IsScalarType(const Module& m, uint32_t type) {
	const auto* inst = m.Def(type);
	return inst != nullptr && (inst->op == spv::OpTypeInt || inst->op == spv::OpTypeFloat ||
	                           inst->op == spv::OpTypeBool);
}

std::vector<uint32_t> Successors(const Module& m, const Inst& terminator,
                                 const std::unordered_map<uint32_t, size_t>& labels) {
	std::vector<uint32_t> out;
	for (const auto offset: terminator.ids) {
		if (labels.contains(terminator.words[offset])) {
			out.push_back(terminator.words[offset]);
		}
	}
	return out;
}

uint32_t SpeculateRegions(Module& m, Function& function) {
	std::unordered_map<uint32_t, size_t> labels;
	for (size_t i = 0; i < function.blocks.size(); i++) {
		if (!function.blocks[i].removed) {
			labels[function.blocks[i].label] = i;
		}
	}
	std::unordered_map<uint32_t, uint32_t> preds;
	std::unordered_map<uint32_t, bool>     structural;
	for (const auto& block: function.blocks) {
		if (block.removed || block.insts.empty()) {
			continue;
		}
		const auto& terminator = m.At(block.insts.back());
		for (const auto target: Successors(m, terminator, labels)) {
			preds[target]++;
		}
		if (block.insts.size() >= 2) {
			const auto& merge = m.At(block.insts[block.insts.size() - 2]);
			if (merge.op == spv::OpSelectionMerge) {
				structural[merge.words[1]] = true;
			} else if (merge.op == spv::OpLoopMerge) {
				structural[merge.words[1]] = true;
				structural[merge.words[2]] = true;
			}
		}
	}

	uint32_t speculated = 0;
	for (size_t header_index = 0; header_index < function.blocks.size(); header_index++) {
		auto& header = function.blocks[header_index];
		if (header.removed || header.insts.size() < 2) {
			continue;
		}
		const auto& branch = m.At(header.insts.back());
		const auto& merge  = m.At(header.insts[header.insts.size() - 2]);
		if (branch.op != spv::OpBranchConditional || merge.op != spv::OpSelectionMerge) {
			continue;
		}
		const auto merge_label = merge.words[1];
		const auto if_true     = branch.words[2];
		const auto if_false    = branch.words[3];
		if (if_true == if_false || (if_true != merge_label && if_false != merge_label)) {
			continue;
		}
		const Region region {.condition = branch.words[1], .inverted = if_true == merge_label};
		const auto   first = region.inverted ? if_false : if_true;

		std::vector<size_t> chain;
		size_t              instructions = 0;
		bool                ok           = true;
		for (auto label = first; label != merge_label;) {
			auto it = labels.find(label);
			if (it == labels.end() || preds[label] != 1 || structural[label] ||
			    chain.size() == MaxSpeculatedBlocks) {
				ok = false;
				break;
			}
			const auto& block = function.blocks[it->second];
			const auto& last  = m.At(block.insts.back());
			if (last.op != spv::OpBranch) {
				ok = false;
				break;
			}
			chain.push_back(it->second);
			instructions += block.insts.size() - 1;
			label = last.words[1];
		}
		if (!ok || chain.empty() || preds[merge_label] != 2 ||
		    instructions > MaxSpeculatedInstructions) {
			continue;
		}
		std::vector<std::pair<uint32_t, Speculation>> plan;
		std::unordered_map<uint32_t, bool>            inside;
		for (const auto block_index: chain) {
			const auto& insts = function.blocks[block_index].insts;
			for (size_t i = 0; ok && i + 1 < insts.size(); i++) {
				const auto& inst = m.At(insts[i]);
				auto        kind = Classify(m, inst);
				if (kind == Speculation::ClampLoad && inside.contains(inst.words[3])) {
					kind = Speculation::Yes;
				}
				if (kind == Speculation::ClampChain || kind == Speculation::ClampLoad) {
					const auto chain_id =
					    kind == Speculation::ClampChain ? inst.result : inst.words[3];
					const auto* index = m.Def(m.Def(chain_id)->words.back());
					const auto* type  = index != nullptr ? m.Def(index->type) : nullptr;
					ok                = type != nullptr && type->op == spv::OpTypeInt;
				}
				ok                  = ok && kind != Speculation::No;
				inside[inst.result] = true;
				plan.emplace_back(insts[i], kind);
			}
		}
		const auto  exit_label  = function.blocks[chain.back()].label;
		const auto& merge_block = function.blocks[labels[merge_label]];
		size_t      phi_count   = 0;
		for (; ok && phi_count < merge_block.insts.size(); phi_count++) {
			const auto& phi = m.At(merge_block.insts[phi_count]);
			if (phi.op != spv::OpPhi) {
				break;
			}
			ok = phi.words.size() == 7 && IsScalarType(m, phi.type) &&
			     ((phi.words[4] == exit_label && phi.words[6] == header.label) ||
			      (phi.words[4] == header.label && phi.words[6] == exit_label));
		}
		if (!ok) {
			continue;
		}

		std::vector<uint32_t> body(header.insts.begin(), header.insts.end() - 2);
		for (const auto& [index, kind]: plan) {
			if (kind == Speculation::ClampChain) {
				m.At(index).words.back() = ClampIndex(m, region, m.At(index).words.back(), body);
			} else if (kind == Speculation::ClampLoad) {
				auto chain_copy         = *m.Def(m.At(index).words[3]);
				chain_copy.words.back() = ClampIndex(m, region, chain_copy.words.back(), body);
				chain_copy.result       = m.AllocateId();
				chain_copy.words[2]     = chain_copy.result;
				m.At(index).words[3]    = chain_copy.result;
				body.push_back(m.Add(std::move(chain_copy)));
			}
			body.push_back(index);
		}
		for (const auto block_index: chain) {
			function.blocks[block_index].removed = true;
		}
		Inst jump;
		jump.op    = spv::OpBranch;
		jump.words = {(2u << spv::WordCountShift) | spv::OpBranch, merge_label};
		jump.ids   = {1};
		body.push_back(m.Add(std::move(jump)));
		header.insts = std::move(body);

		auto& merge_insts = function.blocks[labels[merge_label]].insts;
		for (size_t i = 0; i < phi_count; i++) {
			auto&      phi         = m.At(merge_insts[i]);
			const auto from_exit   = phi.words[4] == exit_label ? phi.words[3] : phi.words[5];
			const auto from_header = phi.words[4] == exit_label ? phi.words[5] : phi.words[3];
			phi = region.inverted
			          ? MakeSelect(phi.type, phi.result, region.condition, from_header, from_exit)
			          : MakeSelect(phi.type, phi.result, region.condition, from_exit, from_header);
		}
		preds[merge_label]      = 1;
		structural[merge_label] = false;
		speculated++;
	}
	return speculated;
}

struct Forwarder {
	Module&                            m;
	std::vector<std::vector<uint32_t>> users; // id -> arena indices
	std::unordered_map<uint64_t, bool> memo;

	bool ObservedOnlyUnder(uint32_t value, uint32_t condition, int depth) {
		if (depth > MaxObservationDepth || value >= users.size() || users[value].empty()) {
			return false;
		}
		const auto key = (static_cast<uint64_t>(value) << 32u) | condition;
		if (auto it = memo.find(key); it != memo.end()) {
			return it->second;
		}
		memo[key] = false;
		bool only = true;
		for (const auto user_index: users[value]) {
			const auto& user = m.At(user_index);
			if (user.op == spv::OpSelect && user.words[3] == condition && user.words[4] == value &&
			    user.words[5] != value) {
				continue;
			}
			if (!IsTotalLaneLocal(user) || user.result == 0 ||
			    !ObservedOnlyUnder(user.result, condition, depth + 1)) {
				only = false;
				break;
			}
		}
		memo[key] = only;
		return only;
	}
};

void ForwardPredicatedValues(Module& m, Function& function) {
	Forwarder forwarder {.m = m};
	forwarder.users.resize(m.def.size());
	for (auto& block: function.blocks) {
		if (block.removed) {
			continue;
		}
		for (const auto index: block.insts) {
			for (const auto offset: m.At(index).ids) {
				const auto id = m.At(index).words[offset];
				if (id < forwarder.users.size()) {
					forwarder.users[id].push_back(index);
				}
			}
		}
	}
	for (auto& block: function.blocks) {
		if (block.removed) {
			continue;
		}
		for (const auto index: block.insts) {
			auto& inst = m.At(index);
			if (!IsTotalLaneLocal(inst) || inst.result == 0) {
				continue;
			}
			for (const auto offset: inst.ids) {
				if (offset == 1) {
					continue; // the result type
				}
				for (;;) {
					const auto* source = m.Def(inst.words[offset]);
					if (source == nullptr || source->op != spv::OpSelect) {
						break;
					}
					const auto condition = source->words[3];
					uint32_t   forwarded = 0;
					if (inst.op == spv::OpSelect && inst.words[3] == condition && offset != 3) {
						forwarded = offset == 4 ? source->words[4] : source->words[5];
					} else if (forwarder.ObservedOnlyUnder(inst.result, condition, 0)) {
						forwarded = source->words[4];
					} else {
						break;
					}
					forwarder.users[forwarded].push_back(index);
					inst.words[offset] = forwarded;
				}
			}
		}
	}
}

bool IsRemovableWhenUnused(const Inst& inst) {
	if (inst.result == 0) {
		return false;
	}
	if (IsTotalLaneLocal(inst) || IsIntegerDivision(inst.op) || IsAccessChain(inst.op) ||
	    inst.op == spv::OpArrayLength) {
		return true;
	}
	return inst.op == spv::OpLoad &&
	       (inst.words.size() <= 4 || (inst.words[4] & spv::MemoryAccessVolatileMask) == 0);
}

void RemoveUnused(Module& m) {
	std::vector<uint32_t> uses(m.def.size(), 0);
	std::vector<uint32_t> live;
	for (auto& function: m.functions) {
		for (auto& block: function.blocks) {
			if (block.removed) {
				continue;
			}
			for (const auto index: block.insts) {
				live.push_back(index);
				for (const auto offset: m.At(index).ids) {
					uses[m.At(index).words[offset]]++;
				}
			}
		}
	}
	std::vector<bool>     dead(m.arena.size(), false);
	std::vector<uint32_t> work;
	for (const auto index: live) {
		if (IsRemovableWhenUnused(m.At(index)) && uses[m.At(index).result] == 0) {
			work.push_back(index);
		}
	}
	while (!work.empty()) {
		const auto index = work.back();
		work.pop_back();
		if (dead[index]) {
			continue;
		}
		dead[index] = true;
		for (const auto offset: m.At(index).ids) {
			const auto id = m.At(index).words[offset];
			if (--uses[id] == 0 && m.def[id] != 0 && IsRemovableWhenUnused(m.At(m.def[id] - 1))) {
				work.push_back(m.def[id] - 1);
			}
		}
	}
	std::vector<bool> removed_id(m.def.size(), false);
	for (auto& function: m.functions) {
		for (auto& block: function.blocks) {
			std::erase_if(block.insts, [&](uint32_t index) {
				if (!dead[index]) {
					return false;
				}
				removed_id[m.At(index).result] = true;
				return true;
			});
		}
	}
	std::erase_if(m.preamble, [&](uint32_t index) {
		const auto& inst    = m.At(index);
		const bool  targets = inst.op == spv::OpDecorate || inst.op == spv::OpName ||
		                      inst.op == spv::OpDecorateId || inst.op == spv::OpDecorateString;
		return targets && removed_id[inst.words[1]];
	});
}

struct Dominators {
	std::vector<uint32_t>              order; // reachable blocks, reverse postorder
	std::vector<int32_t>               idom;  // by block index; -1 when unreachable
	std::vector<std::vector<uint32_t>> children;
};

Dominators ComputeDominators(const Module& m, const Function& function) {
	const auto                           count = function.blocks.size();
	std::unordered_map<uint32_t, size_t> labels;
	for (size_t i = 0; i < count; i++) {
		if (!function.blocks[i].removed) {
			labels[function.blocks[i].label] = i;
		}
	}
	std::vector<std::vector<uint32_t>> successors(count);
	std::vector<std::vector<uint32_t>> predecessors(count);
	for (size_t i = 0; i < count; i++) {
		const auto& block = function.blocks[i];
		if (block.removed || block.insts.empty()) {
			continue;
		}
		const auto& terminator = m.arena[block.insts.back()];
		for (const auto offset: terminator.ids) {
			if (const auto it = labels.find(terminator.words[offset]); it != labels.end()) {
				successors[i].push_back(static_cast<uint32_t>(it->second));
				predecessors[it->second].push_back(static_cast<uint32_t>(i));
			}
		}
	}
	Dominators result;
	result.idom.assign(count, -1);
	result.children.resize(count);
	if (count == 0 || function.blocks[0].removed) {
		return result;
	}
	std::vector<uint32_t>                    postorder;
	std::vector<bool>                        visited(count, false);
	std::vector<std::pair<uint32_t, size_t>> stack {{0u, 0u}};
	visited[0] = true;
	while (!stack.empty()) {
		const auto block = stack.back().first;
		const auto next  = stack.back().second;
		if (next < successors[block].size()) {
			stack.back().second++;
			const auto successor = successors[block][next];
			if (!visited[successor]) {
				visited[successor] = true;
				stack.emplace_back(successor, 0u);
			}
		} else {
			postorder.push_back(block);
			stack.pop_back();
		}
	}
	result.order.assign(postorder.rbegin(), postorder.rend());
	std::vector<int32_t> rank(count, -1);
	for (size_t i = 0; i < result.order.size(); i++) {
		rank[result.order[i]] = static_cast<int32_t>(i);
	}
	auto& idom = result.idom;
	idom[0]    = 0;
	for (bool changed = true; changed;) {
		changed = false;
		for (size_t i = 1; i < result.order.size(); i++) {
			const auto block    = result.order[i];
			int32_t    new_idom = -1;
			for (const auto predecessor: predecessors[block]) {
				if (rank[predecessor] < 0 || idom[predecessor] < 0) {
					continue;
				}
				if (new_idom < 0) {
					new_idom = static_cast<int32_t>(predecessor);
					continue;
				}
				auto a = static_cast<int32_t>(predecessor);
				auto b = new_idom;
				while (a != b) {
					while (rank[a] > rank[b]) {
						a = idom[a];
					}
					while (rank[b] > rank[a]) {
						b = idom[b];
					}
				}
				new_idom = a;
			}
			if (new_idom >= 0 && idom[block] != new_idom) {
				idom[block] = new_idom;
				changed     = true;
			}
		}
	}
	for (size_t i = 1; i < result.order.size(); i++) {
		const auto block = result.order[i];
		if (idom[block] >= 0) {
			result.children[idom[block]].push_back(block);
		}
	}
	return result;
}

struct WordsHash {
	size_t operator()(const std::vector<uint32_t>& key) const {
		uint64_t hash = 1469598103934665603ull;
		for (const auto word: key) {
			hash = (hash ^ word) * 1099511628211ull;
		}
		return static_cast<size_t>(hash ^ (hash >> 29u));
	}
};

// Replaces a value by an equal one: a bit-preserving identity or a dominating duplicate.
class ValueSimplifier {
public:
	explicit ValueSimplifier(Module& module): m(module) {
		decorated.assign(m.def.size(), false);
		for (const auto index: m.preamble) {
			const auto& inst = m.At(index);
			if ((inst.op == spv::OpDecorate || inst.op == spv::OpDecorateId ||
			     inst.op == spv::OpMemberDecorate) &&
			    inst.words[1] < decorated.size()) {
				decorated[inst.words[1]] = true;
			}
			if (inst.op == spv::OpConstant && inst.words.size() == 4) {
				constants.try_emplace((static_cast<uint64_t>(inst.type) << 32u) | inst.words[3],
				                      inst.result);
			}
			if (inst.op == spv::OpExtInstImport && inst.words.size() > 2) {
				const auto* name = reinterpret_cast<const char*>(inst.words.data() + 2);
				if (std::strncmp(name, "GLSL.std.450", 12) == 0) {
					glsl_set = inst.result;
				}
			}
		}
	}

	void Run(Function& function) {
		const auto dominators = ComputeDominators(m, function);
		if (dominators.order.empty()) {
			return;
		}
		struct Frame {
			uint32_t block;
			size_t   child;
			size_t   mark;
		};
		std::vector<Frame> stack;
		const auto         enter = [&](uint32_t block) {
			stack.push_back({block, 0, undo.size()});
			Simplify(function.blocks[block]);
		};
		enter(dominators.order.front());
		while (!stack.empty()) {
			auto& frame = stack.back();
			if (frame.child < dominators.children[frame.block].size()) {
				const auto child = dominators.children[frame.block][frame.child++];
				enter(child);
				continue;
			}
			for (auto i = undo.size(); i > frame.mark; i--) {
				table.erase(undo[i - 1]);
			}
			undo.resize(frame.mark);
			stack.pop_back();
		}
	}

	// Values defined later than a use (a phi's back edge) are only replaced here.
	void RewriteAll() {
		for (auto& function: m.functions) {
			for (auto& block: function.blocks) {
				if (block.removed) {
					continue;
				}
				for (const auto index: block.insts) {
					auto& inst = m.At(index);
					for (const auto offset: inst.ids) {
						inst.words[offset] = Resolve(inst.words[offset]);
					}
				}
			}
		}
	}

private:
	uint32_t Resolve(uint32_t id) {
		uint32_t current = id;
		for (int guard = 0; guard < 64 && current < repl.size() && repl[current] != 0; guard++) {
			current = repl[current];
		}
		return current;
	}

	void Replace(uint32_t result, uint32_t value) {
		if (repl.size() <= result) {
			repl.resize(m.def.size(), 0);
		}
		repl[result] = value;
	}

	const Inst* TypeDef(uint32_t type) const { return m.Def(type); }

	bool IsInt32(uint32_t type) const {
		const auto* def = TypeDef(type);
		return def != nullptr && def->op == spv::OpTypeInt && def->words[2] == 32;
	}

	bool IsArithmetic(uint32_t type) const {
		const auto* def = TypeDef(type);
		return def != nullptr && (def->op == spv::OpTypeInt || def->op == spv::OpTypeFloat ||
		                          def->op == spv::OpTypeVector);
	}

	uint32_t TypeOf(uint32_t id) const {
		const auto* def = m.Def(id);
		return def != nullptr ? def->type : 0;
	}

	bool ConstantValue(uint32_t id, uint32_t& value) const {
		const auto* def = m.Def(id);
		if (def == nullptr || def->op != spv::OpConstant || def->words.size() != 4) {
			return false;
		}
		value = def->words[3];
		return true;
	}

	bool IsBoolConstant(uint32_t id, bool& value) const {
		const auto* def = m.Def(id);
		if (def == nullptr || (def->op != spv::OpConstantTrue && def->op != spv::OpConstantFalse)) {
			return false;
		}
		value = def->op == spv::OpConstantTrue;
		return true;
	}

	// Bits of a 32-bit integer that may be set.
	uint32_t MaybeOnes(uint32_t id, int depth = 0) {
		uint32_t value = 0;
		if (ConstantValue(id, value)) {
			return value;
		}
		const auto* def = m.Def(id);
		if (def == nullptr || depth > 12 || def->result == 0 || !IsInt32(def->type)) {
			return ~0u;
		}
		if (const auto found = maybe_ones.find(id); found != maybe_ones.end()) {
			return found->second;
		}
		uint32_t result = ~0u;
		uint32_t amount = 0;
		switch (def->op) {
			case spv::OpBitFieldUExtract:
				if (ConstantValue(def->words[5], amount)) {
					result = amount >= 32u ? ~0u : (1u << amount) - 1u;
				}
				break;
			case spv::OpBitwiseAnd:
				result = MaybeOnes(def->words[3], depth + 1) & MaybeOnes(def->words[4], depth + 1);
				break;
			case spv::OpBitwiseOr:
			case spv::OpBitwiseXor:
				result = MaybeOnes(def->words[3], depth + 1) | MaybeOnes(def->words[4], depth + 1);
				break;
			case spv::OpShiftRightLogical:
				if (ConstantValue(def->words[4], amount) && amount < 32u) {
					result = MaybeOnes(def->words[3], depth + 1) >> amount;
				}
				break;
			case spv::OpShiftLeftLogical:
				if (ConstantValue(def->words[4], amount) && amount < 32u) {
					result = MaybeOnes(def->words[3], depth + 1) << amount;
				}
				break;
			case spv::OpSelect:
				result = MaybeOnes(def->words[4], depth + 1) | MaybeOnes(def->words[5], depth + 1);
				break;
			default: break;
		}
		maybe_ones.emplace(id, result);
		return result;
	}

	uint32_t FindOrAddConstant(uint32_t type, uint32_t value) {
		const auto key = (static_cast<uint64_t>(type) << 32u) | value;
		if (const auto found = constants.find(key); found != constants.end()) {
			return found->second;
		}
		Inst constant;
		constant.op     = spv::OpConstant;
		constant.type   = type;
		constant.result = m.AllocateId();
		constant.words  = {(4u << spv::WordCountShift) | spv::OpConstant, type, constant.result,
		                   value};
		constant.ids    = {1};
		const auto id   = constant.result;
		m.new_constants.push_back(m.Add(std::move(constant)));
		constants.emplace(key, id);
		return id;
	}

	// A float constant a driver keeps bit for bit: no NaN payload, no denormal to flush.
	static bool PlainFloatBits(uint32_t bits) {
		const auto exponent = bits & 0x7f800000u;
		return exponent != 0x7f800000u ? (exponent != 0 || (bits & 0x007fffffu) == 0)
		                               : (bits & 0x007fffffu) == 0;
	}

	std::vector<uint32_t> Key(const Inst& inst) const {
		std::vector<uint32_t> key;
		key.reserve(inst.words.size() - 1);
		key.push_back(inst.words[0]);
		key.push_back(inst.words[1]);
		key.insert(key.end(), inst.words.begin() + 3, inst.words.end());
		return key;
	}

	bool Deduplicable(const Inst& inst) const {
		if (inst.result == 0 || inst.type == 0 || IsDecorated(inst.result)) {
			return false;
		}
		return (IsTotalLaneLocal(inst) && inst.op != spv::OpCopyObject) || IsAccessChain(inst.op);
	}

	bool IsDecorated(uint32_t id) const { return id < decorated.size() && decorated[id]; }

	// The value the instruction is equal to, or 0.
	uint32_t Fold(uint32_t index, std::vector<uint32_t>& out) {
		auto& inst = m.At(index);
		if (inst.result == 0 || IsDecorated(inst.result)) {
			return 0;
		}
		const auto op    = inst.op;
		uint32_t   value = 0;
		bool       flag  = false;
		const auto lhs   = inst.words.size() > 3 ? inst.words[3] : 0u;
		const auto rhs   = inst.words.size() > 4 ? inst.words[4] : 0u;
		switch (op) {
			case spv::OpCopyObject: return lhs;
			case spv::OpBitcast: {
				if (!IsArithmetic(inst.type) || !IsArithmetic(TypeOf(lhs))) {
					return 0;
				}
				if (TypeOf(lhs) == inst.type) {
					return lhs;
				}
				const auto* source = m.Def(lhs);
				if (source != nullptr && source->op == spv::OpBitcast &&
				    IsArithmetic(TypeOf(source->words[3]))) {
					const auto inner = source->words[3];
					if (TypeOf(inner) == inst.type) {
						return inner;
					}
					inst.words[3] = inner;
					return 0;
				}
				// NVIDIA quiets an sNaN float constant, so NaN/denormal bits must stay integers.
				const auto* type        = TypeDef(inst.type);
				const auto* source_type = TypeDef(TypeOf(lhs));
				if (ConstantValue(lhs, value) && type != nullptr && source_type != nullptr &&
				    (type->op == spv::OpTypeInt || type->op == spv::OpTypeFloat) &&
				    type->words[2] == 32 &&
				    ((type->op != spv::OpTypeFloat && source_type->op != spv::OpTypeFloat) ||
				     PlainFloatBits(value))) {
					return FindOrAddConstant(inst.type, value);
				}
				return 0;
			}
			case spv::OpBitwiseAnd:
				if (!IsInt32(inst.type)) {
					return 0;
				}
				if (lhs == rhs) {
					return lhs;
				}
				if (ConstantValue(rhs, value) && (MaybeOnes(lhs) & ~value) == 0) {
					return lhs;
				}
				if (ConstantValue(lhs, value) && (MaybeOnes(rhs) & ~value) == 0) {
					return rhs;
				}
				return 0;
			case spv::OpBitwiseOr:
			case spv::OpBitwiseXor:
			case spv::OpIAdd:
				if (!IsInt32(inst.type)) {
					return 0;
				}
				if (op == spv::OpBitwiseOr && lhs == rhs) {
					return lhs;
				}
				if (ConstantValue(rhs, value) && value == 0) {
					return lhs;
				}
				if (ConstantValue(lhs, value) && value == 0) {
					return rhs;
				}
				return 0;
			case spv::OpShiftLeftLogical:
			case spv::OpShiftRightLogical:
			case spv::OpShiftRightArithmetic:
				return IsInt32(inst.type) && ConstantValue(rhs, value) && value == 0 ? lhs : 0u;
			case spv::OpSelect: {
				const auto if_true  = inst.words[4];
				const auto if_false = inst.words[5];
				if (if_true == if_false) {
					return if_true;
				}
				if (IsBoolConstant(lhs, flag)) {
					return flag ? if_true : if_false;
				}
				return 0;
			}
			case spv::OpLogicalNot: {
				const auto* source = m.Def(lhs);
				return source != nullptr && source->op == spv::OpLogicalNot ? source->words[3] : 0u;
			}
			case spv::OpLogicalAnd:
			case spv::OpLogicalOr: {
				if (lhs == rhs) {
					return lhs;
				}
				const bool neutral = op == spv::OpLogicalAnd;
				if (IsBoolConstant(rhs, flag) && flag == neutral) {
					return lhs;
				}
				if (IsBoolConstant(lhs, flag) && flag == neutral) {
					return rhs;
				}
				return 0;
			}
			case spv::OpCompositeExtract: {
				if (inst.words.size() != 5) {
					return 0;
				}
				const auto* source = m.Def(lhs);
				if (source == nullptr) {
					return 0;
				}
				const auto component = inst.words[4];
				if (source->op == spv::OpCompositeConstruct) {
					const auto* type = TypeDef(source->type);
					if (type != nullptr && type->op == spv::OpTypeVector &&
					    source->words.size() == 3u + type->words[3] && component < type->words[3] &&
					    TypeOf(source->words[3 + component]) == inst.type) {
						return source->words[3 + component];
					}
					return 0;
				}
				if (source->op == spv::OpExtInst && source->words[3] == glsl_set && glsl_set != 0 &&
				    source->words[4] == GLSLstd450UnpackHalf2x16 && component < 2) {
					return NarrowUnpack(index, out);
				}
				return 0;
			}
			default: return 0;
		}
	}

	// UnpackHalf2x16 reads only the half it is asked for.
	uint32_t NarrowUnpack(uint32_t index, std::vector<uint32_t>& out) {
		const auto& extract   = m.At(index);
		const auto* unpack    = m.Def(extract.words[3]);
		uint32_t    bits      = unpack->words[5];
		uint32_t    component = extract.words[4];
		uint32_t    value     = 0;
		uint32_t    count     = 0;
		bool        narrowed  = false;
		for (int step = 0; step < 4; step++) {
			const auto* def = m.Def(bits);
			if (def == nullptr || !IsInt32(def->type)) {
				break;
			}
			if (def->op == spv::OpBitwiseAnd) {
				if (ConstantValue(def->words[4], value) && (value & 0xffffu) == 0xffffu &&
				    component == 0) {
					bits = def->words[3];
				} else if (ConstantValue(def->words[3], value) && (value & 0xffffu) == 0xffffu &&
				           component == 0) {
					bits = def->words[4];
				} else {
					break;
				}
			} else if (def->op == spv::OpBitFieldUExtract && ConstantValue(def->words[4], value) &&
			           ConstantValue(def->words[5], count)) {
				if (value == 0 && count >= 16 && component == 0) {
					bits = def->words[3];
				} else if (value == 16 && count == 16 && component == 0) {
					bits      = def->words[3];
					component = 1;
				} else {
					break;
				}
			} else if (def->op == spv::OpShiftRightLogical && ConstantValue(def->words[4], value) &&
			           value == 16 && component == 0) {
				bits      = def->words[3];
				component = 1;
			} else {
				break;
			}
			narrowed = true;
		}
		if (!narrowed || !IsInt32(TypeOf(bits))) {
			return 0;
		}
		Inst replacement     = *unpack;
		replacement.words[5] = bits;
		uint32_t unpacked    = 0;
		if (const auto found = table.find(Key(replacement)); found != table.end()) {
			unpacked = found->second;
		}
		if (unpacked == 0) {
			replacement.result   = m.AllocateId();
			replacement.words[2] = replacement.result;
			unpacked             = replacement.result;
			const auto key       = Key(replacement);
			out.push_back(m.Add(std::move(replacement)));
			table.emplace(key, unpacked);
			undo.push_back(key);
		}
		auto& rewritten    = m.At(index);
		rewritten.words[3] = unpacked;
		rewritten.words[4] = component;
		return 0;
	}

	void Simplify(Block& block) {
		std::vector<uint32_t> out;
		out.reserve(block.insts.size() + 8);
		for (const auto index: block.insts) {
			{
				auto& inst = m.At(index);
				for (const auto offset: inst.ids) {
					inst.words[offset] = Resolve(inst.words[offset]);
				}
			}
			if (const auto value = Fold(index, out); value != 0) {
				Replace(m.At(index).result, value);
				out.push_back(index);
				continue;
			}
			const auto& inst = m.At(index);
			if (Deduplicable(inst)) {
				auto key = Key(inst);
				if (const auto found = table.find(key); found != table.end()) {
					Replace(inst.result, found->second);
				} else {
					table.emplace(key, inst.result);
					undo.push_back(std::move(key));
				}
			}
			out.push_back(index);
		}
		block.insts = std::move(out);
	}

	Module&                                                        m;
	std::vector<uint32_t>                                          repl;
	std::vector<bool>                                              decorated;
	std::unordered_map<uint64_t, uint32_t>                         constants;
	std::unordered_map<uint32_t, uint32_t>                         maybe_ones;
	std::unordered_map<std::vector<uint32_t>, uint32_t, WordsHash> table;
	std::vector<std::vector<uint32_t>>                             undo;
	uint32_t                                                       glsl_set = 0;
};

// Moves each pure lane-local instruction to just before its first user in its block.
void SinkToUses(Module& m, Function& function) {
	std::unordered_map<uint32_t, uint32_t>   position; // result id -> index in the block
	std::vector<uint32_t>                    order;
	std::vector<uint8_t>                     state; // 0 = waiting, 1 = on the stack, 2 = placed
	std::vector<std::pair<uint32_t, size_t>> stack;
	for (auto& block: function.blocks) {
		if (block.removed || block.insts.size() < 3) {
			continue;
		}
		const auto& insts = block.insts;
		const auto  count = insts.size();
		size_t      tail  = count - 1; // the terminator, with its merge instruction before it
		if (tail > 0 && (m.At(insts[tail - 1]).op == spv::OpSelectionMerge ||
		                 m.At(insts[tail - 1]).op == spv::OpLoopMerge)) {
			tail--;
		}
		position.clear();
		std::vector<bool> movable(count, false);
		std::vector<bool> used_here(count, false);
		for (size_t i = 0; i < tail; i++) {
			const auto& inst = m.At(insts[i]);
			if (inst.result != 0 && inst.op != spv::OpPhi && IsTotalLaneLocal(inst)) {
				movable[i] = true;
				position.emplace(inst.result, static_cast<uint32_t>(i));
			}
		}
		for (size_t i = 0; i < count; i++) {
			const auto& inst = m.At(insts[i]);
			if (inst.op == spv::OpPhi) {
				continue;
			}
			for (const auto offset: inst.ids) {
				if (const auto found = position.find(inst.words[offset]);
				    found != position.end() && found->second != i) {
					used_here[found->second] = true;
				}
			}
		}
		order.clear();
		state.assign(count, 0);
		const auto place = [&](size_t root) {
			stack.clear();
			stack.emplace_back(static_cast<uint32_t>(root), 0);
			state[root] = 1;
			while (!stack.empty()) {
				auto&       top    = stack.back();
				const auto& inst   = m.At(insts[top.first]);
				bool        pushed = false;
				if (inst.op != spv::OpPhi) {
					while (top.second < inst.ids.size()) {
						const auto id = inst.words[inst.ids[top.second++]];
						if (const auto found = position.find(id);
						    found != position.end() && state[found->second] == 0) {
							state[found->second] = 1;
							stack.emplace_back(found->second, 0);
							pushed = true;
							break;
						}
					}
				}
				if (!pushed) {
					state[stack.back().first] = 2;
					order.push_back(insts[stack.back().first]);
					stack.pop_back();
				}
			}
		};
		for (size_t i = 0; i < tail; i++) {
			if (state[i] == 0 && !(movable[i] && used_here[i])) {
				place(i);
			}
		}
		for (size_t i = 0; i < tail; i++) {
			if (state[i] == 0) {
				place(i); // a value only a phi of this block reads
			}
		}
		for (size_t i = tail; i < count; i++) {
			if (state[i] == 0) {
				place(i);
			}
		}
		if (order != insts) {
			block.insts = order;
		}
	}
}

void Append(std::vector<uint32_t>& out, const Inst& inst) {
	out.insert(out.end(), inst.words.begin(), inst.words.end());
}

std::vector<uint32_t> Serialize(Module& m) {
	std::vector<uint32_t> out(std::begin(m.header), std::end(m.header));
	for (const auto index: m.preamble) {
		Append(out, m.At(index));
	}
	for (const auto index: m.new_constants) {
		Append(out, m.At(index));
	}
	for (const auto& function: m.functions) {
		for (const auto index: function.head) {
			Append(out, m.At(index));
		}
		for (const auto& block: function.blocks) {
			if (block.removed) {
				continue;
			}
			out.push_back((2u << spv::WordCountShift) | spv::OpLabel);
			out.push_back(block.label);
			for (const auto index: block.insts) {
				Append(out, m.At(index));
			}
		}
		Append(out, m.At(function.end));
	}
	return out;
}

} // namespace

uint32_t DriverSimplifyMinWords() {
	return g_min_words.load(std::memory_order_relaxed);
}

void SetDriverSimplifyMinWords(uint32_t words) {
	g_min_words.store(words, std::memory_order_relaxed);
}

bool SimplifyForDriver(std::vector<uint32_t>& words) {
	Module      module;
	spv_context context = spvContextCreate(SPV_ENV_UNIVERSAL_1_6);
	const auto  parsed  = spvBinaryParse(context, &module, words.data(), words.size(), OnHeader,
	                                     OnInstruction, nullptr);
	spvContextDestroy(context);
	if (parsed != SPV_SUCCESS || module.functions.empty()) {
		return false;
	}
	for (const auto index: module.preamble) {
		const auto& inst = module.At(index);
		if (inst.op == spv::OpConstant && inst.words.size() == 4 && inst.words[3] == 0) {
			module.zero_of_type.try_emplace(inst.type, inst.result);
		}
	}
	for (auto& function: module.functions) {
		while (SpeculateRegions(module, function) != 0) {
		}
		ForwardPredicatedValues(module, function);
	}
	ValueSimplifier simplifier(module);
	for (auto& function: module.functions) {
		simplifier.Run(function);
	}
	simplifier.RewriteAll();
	RemoveUnused(module);
	for (auto& function: module.functions) {
		SinkToUses(module, function);
	}
	words = Serialize(module);
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
