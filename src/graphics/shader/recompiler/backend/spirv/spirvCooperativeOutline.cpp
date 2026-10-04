#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <algorithm>
#include <map>
#include <set>
#include <spirv-tools/libspirv.h>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {
struct Parsed {
	std::vector<uint32_t> words;
	std::vector<uint16_t> references;
	uint32_t result = 0, type = 0;
	uint16_t Op() const { return words[0] & 0xffffu; }
};
spv_result_t ParseInstruction(void* user, const spv_parsed_instruction_t* instruction) {
	auto& output = *static_cast<std::vector<Parsed>*>(user);
	Parsed parsed;
	parsed.words.assign(instruction->words, instruction->words + instruction->num_words);
	parsed.result = instruction->result_id;
	parsed.type = instruction->type_id;
	for (uint16_t i = 0; i < instruction->num_operands; ++i) {
		const auto& operand = instruction->operands[i];
		if (operand.type == SPV_OPERAND_TYPE_ID || operand.type == SPV_OPERAND_TYPE_TYPE_ID ||
		    operand.type == SPV_OPERAND_TYPE_SCOPE_ID || operand.type == SPV_OPERAND_TYPE_MEMORY_SEMANTICS_ID)
			parsed.references.push_back(operand.offset);
	}
	output.push_back(std::move(parsed));
	return SPV_SUCCESS;
}
void Append(std::vector<uint32_t>& output, spv::Op op, const std::vector<uint32_t>& operands) {
	output.push_back((static_cast<uint32_t>(operands.size() + 1u) << 16u) | op);
	output.insert(output.end(), operands.begin(), operands.end());
}
} // namespace

// Cooperative scheduling dispatches a uniform PC to a complete guest segment.
// Keep that dispatcher compact and compile each segment independently. Main
// retains ownership of every Function variable and its initializer. Helpers
// receive the original memory objects through typed Function-pointer parameters;
// no storage class or lifetime is changed. SSA imports include the active mask,
// and workgroup rendezvous retain their original uniform call context.
std::vector<uint32_t> OutlineCooperativeSegments(std::vector<uint32_t> binary, uint32_t entry) {
	const auto retain = [&]() { return std::move(binary); };
	std::vector<Parsed> instructions;
	const auto context = spvContextCreate(SPV_ENV_VULKAN_1_3);
	spv_diagnostic diagnostic = nullptr;
	const auto result = spvBinaryParse(context, &instructions, binary.data(), binary.size(),
	                                  nullptr, ParseInstruction, &diagnostic);
	if (diagnostic) spvDiagnosticDestroy(diagnostic);
	spvContextDestroy(context);
	EXIT_IF(result != SPV_SUCCESS);

	size_t first_function = instructions.size(), main = instructions.size(), end = main;
	uint32_t current = 0, void_type = 0;
	std::unordered_set<uint32_t> memory_objects;
	std::unordered_map<uint32_t, uint32_t> pointer_storage;
	std::unordered_map<uint32_t, size_t> labels;
	std::unordered_map<uint32_t, uint32_t> main_types;
	for (size_t i = 0; i < instructions.size(); ++i) {
		const auto& inst = instructions[i];
		if (inst.Op() == spv::OpTypePointer) pointer_storage.emplace(inst.result, inst.words[2]);
		if (inst.Op() == spv::OpFunction) {
			first_function = std::min(first_function, i);
			current = inst.result;
			if (current == entry) { main = i; void_type = inst.type; }
		}
		if (current == entry) {
			if (inst.result) main_types.emplace(inst.result, inst.type);
			if (inst.Op() == spv::OpLabel) labels.emplace(inst.result, i);
			if (inst.Op() == spv::OpVariable && inst.words[3] == spv::StorageClassFunction)
				memory_objects.insert(inst.result);
		}
		if (inst.Op() == spv::OpFunctionEnd) {
			if (current == entry) end = i + 1u;
			current = 0;
		}
	}
	if (main == instructions.size()) return retain();
	size_t dispatch = main;
	while (dispatch < end && instructions[dispatch].Op() != spv::OpSwitch) ++dispatch;
	if (dispatch == end || instructions[dispatch - 1u].Op() != spv::OpSelectionMerge) return retain();
	const auto merge = instructions[dispatch - 1u].words[1];
	if (!labels.contains(merge)) return retain();
	const auto merge_position = labels.at(merge);
	const auto& switch_words = instructions[dispatch].words;
	if (switch_words.size() < 7u || (switch_words.size() - 3u) % 2u != 0u) return retain();
	struct Case {
		size_t begin = 0, end = 0;
		uint32_t label = 0, wrapper = 0, function = 0, function_type = 0;
		std::vector<uint32_t> imports;
		std::unordered_map<uint32_t, uint32_t> parameters;
		bool outlined = true;
	};
	std::vector<Case> cases;
	for (size_t at = 4u; at < switch_words.size(); at += 2u) {
		const auto label = switch_words[at];
		if (!labels.contains(label)) return retain();
		cases.push_back({.begin = labels.at(label), .label = label});
	}
	std::ranges::sort(cases, {}, &Case::begin);
	std::vector<size_t> owners(instructions.size(), cases.size());
	std::unordered_map<uint32_t, size_t> definitions;
	for (size_t i = 0; i < cases.size(); ++i) {
		auto& arm = cases[i];
		arm.end = i + 1u < cases.size() ? cases[i + 1u].begin : merge_position;
		if (arm.begin <= dispatch || arm.end <= arm.begin || arm.end > merge_position) return retain();
		for (size_t j = arm.begin; j < arm.end; ++j) {
			owners[j] = i;
			if (instructions[j].result) definitions.emplace(instructions[j].result, i);
		}
	}
	// This ABI returns no SSA values. Keep any arm whose values or internal
	// labels escape to the entry merge/another arm; otherwise an outer Phi
	// would refer to definitions and predecessors moved into another function.
	for (size_t i = main; i < end; ++i) {
		for (const auto offset: instructions[i].references) {
			const auto id = instructions[i].words[offset];
			if (!definitions.contains(id)) continue;
			const auto owner = definitions.at(id);
			if (owners[i] != owner && !(i == dispatch && id == cases[owner].label))
				cases[owner].outlined = false;
		}
	}
	for (auto& arm: cases) {
		std::unordered_set<uint32_t> locals;
		for (size_t j = arm.begin; j < arm.end; ++j)
			if (instructions[j].result) locals.insert(instructions[j].result);
		std::set<uint32_t> imports;
		for (size_t j = arm.begin; j < arm.end; ++j) {
			const auto& inst = instructions[j];
			for (const auto offset: inst.references)
				if (inst.words[offset] == merge && inst.Op() != spv::OpBranch) arm.outlined = false;
			// Keep atomic RMW sequences in their proven uniform dispatcher. The
			// outlined ABI is not yet proved for those sequences on native GPUAV.
			const std::string_view opcode = spvOpcodeString(inst.Op());
			if (opcode.starts_with("Atomic") && inst.Op() != spv::OpAtomicLoad &&
			    inst.Op() != spv::OpAtomicStore) arm.outlined = false;
			for (const auto offset: inst.references) {
				const auto id = inst.words[offset];
				if (id == merge || locals.contains(id) || !main_types.contains(id)) continue;
				if (main_types.at(id) == 0) return retain();
				imports.insert(id);
			}
		}
		// Without VariablePointers, a Function/Private pointer argument must be
		// a memory object declaration, not an imported derived pointer. Preserve
		// such segments in the original entry rather than adding capabilities.
		for (const auto id: imports) {
			const auto type = main_types.at(id);
			if (pointer_storage.contains(type) && !memory_objects.contains(id)) arm.outlined = false;
		}
		// SPIR-V universal limit: never invent a larger function ABI. Keep the
		// original valid dispatcher when its SSA interface cannot be outlined.
		if (imports.size() > 255u) arm.outlined = false;
		arm.imports.assign(imports.begin(), imports.end());
	}
	if (std::ranges::none_of(cases, &Case::outlined)) return retain();

	uint32_t next = binary[3];
	const auto allocate = [&] { return next++; };
	std::map<std::vector<uint32_t>, uint32_t> function_types;
	for (const auto& inst: instructions) {
		if (inst.Op() == spv::OpTypeFunction)
			function_types.emplace(std::vector<uint32_t>(inst.words.begin() + 2u, inst.words.end()), inst.result);
	}
	std::vector<uint32_t> declarations, helpers, wrappers;
	std::unordered_map<uint32_t, uint32_t> wrapper_labels;
	for (auto& arm: cases) {
		if (!arm.outlined) {
			wrapper_labels.emplace(arm.label, arm.label);
			for (size_t j = arm.begin; j < arm.end; ++j) {
				const auto& inst = instructions[j];
				wrappers.insert(wrappers.end(), inst.words.begin(), inst.words.end());
			}
			continue;
		}
		arm.wrapper = allocate(); arm.function = allocate();
		wrapper_labels.emplace(arm.label, arm.wrapper);
		std::vector<uint32_t> signature {void_type};
		for (const auto id: arm.imports) signature.push_back(main_types.at(id));
		if (!function_types.contains(signature)) {
			const auto type = allocate();
			function_types.emplace(signature, type);
			auto operands = signature; operands.insert(operands.begin(), type);
			Append(declarations, spv::OpTypeFunction, operands);
		}
		arm.function_type = function_types.at(signature);
		Append(helpers, spv::OpFunction, {void_type, arm.function, spv::FunctionControlDontInlineMask, arm.function_type});
		for (const auto id: arm.imports) {
			const auto parameter = allocate();
			arm.parameters.emplace(id, parameter);
			Append(helpers, spv::OpFunctionParameter, {main_types.at(id), parameter});
		}
		for (size_t j = arm.begin; j < arm.end; ++j) {
			auto inst = instructions[j];
			if (inst.Op() == spv::OpBranch && inst.words[1] == merge) {
				Append(helpers, spv::OpReturn, {});
				continue;
			}
			for (const auto offset: inst.references) {
				auto& id = inst.words[offset];
				if (arm.parameters.contains(id)) id = arm.parameters.at(id);
			}
			helpers.insert(helpers.end(), inst.words.begin(), inst.words.end());
		}
		Append(helpers, spv::OpFunctionEnd, {});
		Append(wrappers, spv::OpLabel, {arm.wrapper});
		std::vector<uint32_t> call {void_type, allocate(), arm.function};
		call.insert(call.end(), arm.imports.begin(), arm.imports.end());
		Append(wrappers, spv::OpFunctionCall, call);
		Append(wrappers, spv::OpBranch, {merge});
	}
	std::vector<uint32_t> output(binary.begin(), binary.begin() + 5u);
	output[3] = next;
	for (size_t i = 0; i < instructions.size(); ++i) {
		if (i == first_function) {
			output.insert(output.end(), declarations.begin(), declarations.end());
		}
		if (i == main) output.insert(output.end(), helpers.begin(), helpers.end());
		if (i == cases.front().begin) {
			output.insert(output.end(), wrappers.begin(), wrappers.end());
			i = merge_position;
		}
		auto inst = instructions[i];
		if (i == dispatch)
			for (size_t at = 4u; at < inst.words.size(); at += 2u)
				inst.words[at] = wrapper_labels.at(inst.words[at]);
		output.insert(output.end(), inst.words.begin(), inst.words.end());
	}
	return output;
}
} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
