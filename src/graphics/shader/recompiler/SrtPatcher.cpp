#include "graphics/shader/recompiler/SrtPatcher.h"

#include <fmt/format.h>
#include <map>
#include <set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

bool PatchSrtReads(Program* program, std::string* error) {
	if (program == nullptr || !program->srt_plan_complete || program->srt_patching_complete ||
	    program->resource_tracking_complete) {
		if (error != nullptr) {
			*error = program == nullptr               ? "invalid SRT patch program"
			         : !program->srt_plan_complete    ? "SRT plan is not ready"
			         : program->srt_patching_complete ? "SRT reads already patched"
			                                          : "resources already tracked";
		}
		return false;
	}

	std::map<uint32_t, uint32_t> offsets;
	std::map<uint32_t, uint32_t> values;
	for (const auto& read: program->srt.reads) {
		if (read.value >= program->provenance.values.size() ||
		    read.flat_offset >= program->srt.reads.size() ||
		    (program->provenance.values[read.value].op != ScalarValueOp::ReadConst &&
		     program->provenance.values[read.value].op != ScalarValueOp::ReadConstBuffer)) {
			if (error != nullptr) {
				*error = fmt::format("invalid SRT read value={} flat_offset={}", read.value,
				                     read.flat_offset);
			}
			return false;
		}
		const auto [it, inserted] = offsets.emplace(read.value, read.flat_offset);
		if (!inserted && it->second != read.flat_offset) {
			if (error != nullptr) {
				*error = fmt::format("SRT read value {} has conflicting flat offsets", read.value);
			}
			return false;
		}
		const auto [value, unique] = values.emplace(read.flat_offset, read.value);
		if (!unique && value->second != read.value) {
			if (error != nullptr) {
				*error = fmt::format("SRT flat offset {} has conflicting values", read.flat_offset);
			}
			return false;
		}
	}
	if (offsets.size() != program->srt.reads.size() || values.size() != program->srt.reads.size()) {
		if (error != nullptr) {
			*error = "SRT reads do not form a dense value-to-offset bijection";
		}
		return false;
	}

	std::set<uint32_t> dynamic_reads;
	for (const auto value: program->srt.dynamic_reads) {
		if (value >= program->provenance.values.size() || offsets.contains(value) ||
		    (program->provenance.values[value].op != ScalarValueOp::ReadConst &&
		     program->provenance.values[value].op != ScalarValueOp::ReadConstBuffer) ||
		    !dynamic_reads.insert(value).second) {
			if (error != nullptr) {
				*error = fmt::format("invalid dynamic SRT read value={}", value);
			}
			return false;
		}
	}
	std::set<uint32_t> dynamic_sources;
	for (const auto source: program->srt.dynamic_sources) {
		if (source == ScalarProvenance::Undefined ||
		    (source > ScalarProvenance::Unknown &&
		     source - 2u >= program->provenance.descriptors.size()) ||
		    !dynamic_sources.insert(source).second) {
			if (error != nullptr) {
				*error = fmt::format("invalid dynamic descriptor source={}", source);
			}
			return false;
		}
	}

	struct Patch {
		Instruction* inst        = nullptr;
		uint32_t     flat_offset = 0;
	};
	std::vector<Patch>       patches;
	std::map<uint32_t, bool> matched;
	for (const auto& [value, offset]: offsets) {
		matched.emplace(value, false);
	}
	for (auto& block: program->blocks) {
		for (auto& inst: block.instructions) {
			if (inst.op != Opcode::SLoadDword && inst.op != Opcode::SBufferLoadDword) {
				continue;
			}
			if (const auto found = offsets.find(inst.scalar_value); found != offsets.end()) {
				const auto value_op = program->provenance.values[found->first].op;
				if ((inst.op == Opcode::SLoadDword) != (value_op == ScalarValueOp::ReadConst)) {
					if (error != nullptr) {
						*error = fmt::format("SRT read value {} has the wrong scalar-load producer",
						                     found->first);
					}
					return false;
				}
				patches.push_back({&inst, found->second});
				matched[found->first] = true;
			}
		}
	}
	for (const auto& [value, found]: matched) {
		if (!found) {
			if (error != nullptr) {
				*error = fmt::format("SRT read value {} has no scalar-load producer", value);
			}
			return false;
		}
	}

	for (const auto& patch: patches) {
		patch.inst->op        = Opcode::LoadSrtDword;
		patch.inst->src_count = 1;
		for (auto& src: patch.inst->src) {
			src = {};
		}
		patch.inst->src[0].kind = OperandKind::ImmediateU32;
		patch.inst->src[0].imm  = patch.flat_offset;
		patch.inst->memory      = {};
	}
	program->srt_patching_complete = true;
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
