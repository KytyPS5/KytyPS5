#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <optional>
#include <string_view>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

namespace {

[[noreturn]] void Fail(const IR::Program& program, const char* reason) {
	EXIT("SPIR-V validation failed: hash=0x%016" PRIx64 " stage=%u reason=%s\n",
	     program.shader_hash, static_cast<unsigned>(program.stage), reason);
	std::abort();
}

void ValidateNativeProgram(const IR::Program& program, bool lds_storage) {
	// MaxBuffers caps what a shader TRACKS; an expanded table materializes up to MaxDenseBuffers.
	std::bitset<IR::ShaderInfo::MaxDenseBuffers> live_buffers;
	bool uses_lds = false;
	bool uses_gds = false;
	const auto uses_mapping = [](const auto& resource) {
		return resource.indirect_root != UINT32_MAX;
	};
	// A bindless image samples the heap through the flattened SRT's arena records.
	bool uses_flattened_srt =
	    std::ranges::any_of(program.info.buffers, uses_mapping) ||
	    std::ranges::any_of(program.info.images, uses_mapping) ||
	    std::ranges::any_of(program.info.images,
	                        [](const IR::ImageResource& image) { return image.bindless; });
	const auto planning_only_handle = [&](const IR::Inst& handle) {
		return !handle.Uses().empty() &&
		       std::ranges::all_of(handle.Uses(), [&](const IR::Use& use) {
			       const auto op = use.user->GetOpcode();
			       if (op != IR::ValueOpcode::LoadAddressU32 &&
			           op != IR::ValueOpcode::ReadConstBuffer &&
			           op != IR::ValueOpcode::LoadBufferU32) {
				       return false;
			       }
			       const auto index = use.user->Flags<IR::MemoryFlags>().index;
			       return index < program.memory_info.size() &&
			              program.memory_info[index].planning_only;
		       });
	};
	const auto indirect_buffer_handle = [&](const IR::Inst& handle) {
		return program.info.uses_dma && handle.NumArgs() == 4u && !handle.Uses().empty() &&
		       std::ranges::all_of(handle.Uses(), [&](const IR::Use& use) {
			       if (IR::BufferAccessOf(use.user->GetOpcode()) != IR::BufferAccess::Read) {
				       return false;
			       }
			       const auto index = use.user->Flags<IR::MemoryFlags>().index;
			       return index < program.memory_info.size() &&
			              program.memory_info[index].kind == IR::ResourceKind::IndirectBuffer;
		       });
	};
	const auto local_flat_handle = [&](const IR::Inst& handle) {
		return !handle.Uses().empty() &&
		       std::ranges::all_of(handle.Uses(), [&](const IR::Use& use) {
			       if (IR::AddressOpcodeInfoOf(use.user->GetOpcode()).access == IR::AddressAccess::None)
				       return false;
			       const auto index = use.user->Flags<IR::MemoryFlags>().index;
			       return index < program.memory_info.size() &&
			              program.memory_info[index].kind == IR::ResourceKind::FlatLocal;
		       });
	};
	// Any access whose V# the shader decodes from memory, stores and atomics included.
	const auto dynamic_buffer_handle = [&](const IR::Inst& handle) {
		return !handle.Uses().empty() &&
		       std::ranges::any_of(handle.Uses(), [&](const IR::Use& use) {
			       const auto op = use.user->GetOpcode();
			       if (IR::BufferAccessOf(op) == IR::BufferAccess::None) {
				       return false;
			       }
			       const auto index = use.user->Flags<IR::MemoryFlags>().index;
			       return index < program.memory_info.size() &&
			              program.memory_info[index].dynamic_buffer;
		       });
	};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto op = inst.GetOpcode();
			uses_flattened_srt |= op == IR::ValueOpcode::ReadConst;
			if (IR::BufferAccessOf(op) != IR::BufferAccess::None ||
			    IR::SharedAccessOf(op) != IR::SharedAccess::None ||
			    IR::AddressOpcodeInfoOf(op).access != IR::AddressAccess::None) {
				const auto index = inst.Flags<IR::MemoryFlags>().index;
				if (index >= program.memory_info.size()) {
					Fail(program, "typed shader contains invalid memory metadata");
				}
				const auto& memory = program.memory_info[index];
				// A dynamic V# keeps no host buffer alive, and its resource index is still the raw
				// SRSRC operand.
				if (!memory.planning_only) {
					uses_lds |= memory.kind == IR::ResourceKind::FlatLocal;
					if (memory.dynamic_buffer) {
						// Served through the page table, not a descriptor.
					} else if (IR::SharedAccessOf(op) != IR::SharedAccess::None) {
						if (memory.kind != IR::ResourceKind::Lds && memory.kind != IR::ResourceKind::Gds) {
							Fail(program, "typed shader contains invalid shared-memory metadata");
						}
						uses_gds |= memory.kind == IR::ResourceKind::Gds;
						uses_lds |= memory.kind == IR::ResourceKind::Lds;
					} else if (memory.kind == IR::ResourceKind::Buffer ||
					           memory.kind == IR::ResourceKind::ScalarBuffer) {
						if (memory.resource >= program.info.buffers.size()) {
							Fail(program, "typed shader contains an invalid buffer resource");
						}
						live_buffers.set(memory.resource);
						// Nothing names a candidate, so without this the binding and the descriptor
						// write disagree.
						for (const auto child: program.info.buffers[memory.resource].indirect_resources) {
							if (child >= program.info.buffers.size()) {
								Fail(program, "typed shader contains an invalid indirect buffer resource");
							}
							live_buffers.set(child);
						}
					}
				}
			}
			const auto dense = inst.Flags<uint32_t>();
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::GetBufferResource:
					if (planning_only_handle(inst) || indirect_buffer_handle(inst) ||
					    dynamic_buffer_handle(inst)) {
						break;
					}
					if (dense >= program.info.buffers.size()) {
						Fail(program, "typed buffer handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::GetAddressResource:
					if (planning_only_handle(inst)) {
						break;
					}
					if (inst.NumArgs() != 2 || (!program.info.uses_dma && !local_flat_handle(inst))) {
						Fail(program, "typed address handle has invalid DMA metadata");
					}
					break;
				case IR::ValueOpcode::GetScratchResource:
					if (inst.NumArgs() != 0 || program.scratch_dwords == 0) {
						Fail(program, "typed scratch handle has invalid shader metadata");
					}
					break;
				case IR::ValueOpcode::GetImageResource:
					if (dense >= program.info.images.size()) {
						Fail(program, "typed image handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::GetSamplerResource:
					if (dense >= program.info.samplers.size()) {
						Fail(program, "typed sampler handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::ReadConst: {
					const auto slot = inst.Arg(1).Resolve();
					if (!slot.IsImmediate() || slot.GetType() != IR::Type::U32 ||
					    slot.U32() >= program.srt_reads.size()) {
						Fail(program, "flattened SRT read has an invalid dense slot");
					}
					break;
				}
				default: break;
			}
		}
	}
	using Kind = IR::DescriptorBindingKind;
	std::array<uint32_t, static_cast<size_t>(Kind::Count)> expected {};
	auto& buffer_count = expected[static_cast<size_t>(Kind::Buffers)];
	for (uint32_t index = 0; index < program.info.buffers.size(); ++index) {
		const auto slot = live_buffers.test(index) ? buffer_count++ : UINT32_MAX;
		if (program.info.buffers[index].descriptor_index != slot) {
			Fail(program, "native buffer descriptor index does not match shader topology");
		}
	}
	std::array<bool, IR::ImageBindingCount> indexed {};
	for (const auto& image: program.info.images) {
		// A bindless image is sampled from the global heap arrays, never from a native binding.
		if (image.bindless) {
			if (image.descriptor_index != UINT32_MAX) {
				Fail(program, "native bindless image has a descriptor index");
			}
			continue;
		}
		const auto kind = IR::DescriptorBindingForImage(image);
		if (!kind.has_value()) {
			Fail(program, "native shader plan has an invalid image class");
		}
		if (image.mip_count == 0u ||
		    (image.mip_mode != IR::ImageMipMode::Dynamic && image.mip_count != 1u)) {
			Fail(program, "native shader plan has an invalid image mip descriptor count");
		}
		auto& count = expected[static_cast<size_t>(*kind)];
		if (image.descriptor_index != count) {
			Fail(program, "native image descriptor index does not match shader topology");
		}
		count += image.mip_count;
		indexed[IR::ImageBindingIndex(*kind)] |=
		    image.indirect_root != IR::ImageResource::NoIndirectImage;
	}
	for (uint32_t group = 0; group < IR::ImageBindingCount; group++) {
		auto& count = expected[IR::FirstImageBinding + group];
		if (indexed[group]) {
			if (count > IR::IndexedImageBindingElements) {
				Fail(program,
				     "an indexed image binding holds more images than it can be padded to");
			}
			count = IR::IndexedImageBindingElements;
		}
	}
	expected[static_cast<size_t>(Kind::Samplers)] = static_cast<uint32_t>(program.info.samplers.size());
	expected[static_cast<size_t>(Kind::Gds)] = uses_gds;
	expected[static_cast<size_t>(Kind::SharedMemory)] = uses_lds && lds_storage;
	expected[static_cast<size_t>(Kind::BdaPagetable)] = program.info.uses_dma;
	expected[static_cast<size_t>(Kind::FaultBuffer)] = program.info.uses_dma;
	expected[static_cast<size_t>(Kind::FlattenedSrt)] = uses_flattened_srt;
	expected[static_cast<size_t>(Kind::ShaderData)] =
	    program.bindings.ShaderDataDwords() != 0 && !program.bindings.UsesPushData();
	uint64_t descriptor_mask = 0;
	for (uint32_t index = 0; index < expected.size(); ++index) {
		if (expected[index] != 0) descriptor_mask |= uint64_t{1} << index;
	}
	if (program.bindings.descriptor_counts != expected ||
	    program.bindings.descriptor_mask != descriptor_mask) {
		Fail(program, "native descriptor counts do not match shader topology");
	}
	const auto shader_data_dwords = program.bindings.ShaderDataDwords();
	const auto& registers = program.info.user_data_registers;
	if ((program.bindings.UsesPushData() &&
	     !IR::PushData::CanFit(program.bindings.push_data_start_dword, shader_data_dwords)) ||
	    program.bindings.memory_offset_dword != registers.size() ||
	    !std::is_sorted(registers.begin(), registers.end()) ||
	    std::adjacent_find(registers.begin(), registers.end()) != registers.end()) {
		Fail(program, "native shader-data layout is inconsistent");
	}
}

class LdsAddressRange {
public:
	struct Range {
		uint64_t max  = 0;
		uint64_t bits = 0;
	};

	explicit LdsAddressRange(IR::Value predicate): m_predicate(predicate.Resolve()) {}

	std::optional<Range> Of(IR::Value value) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			if (value.GetType() != IR::Type::U32) {
				return std::nullopt;
			}
			return Range {value.U32(), value.U32()};
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || m_path.size() >= 64u ||
		    std::ranges::find(m_path, inst) != m_path.end()) {
			return std::nullopt;
		}
		m_path.push_back(inst);
		auto result = Compute(*inst);
		m_path.pop_back();
		if (result.has_value()) {
			result->max = std::min(result->max, result->bits);
		}
		return result;
	}

private:
	static constexpr uint64_t Word = 0xffffffffull;

	static std::optional<Range> UpTo(uint64_t max) {
		if (max > Word) {
			return std::nullopt;
		}
		return Range {max, max == 0 ? 0 : (uint64_t {1} << std::bit_width(max)) - 1u};
	}

	std::optional<uint32_t> Immediate(const IR::Inst& inst, size_t index) const {
		const auto operand = inst.Arg(index).Resolve();
		if (operand.IsImmediate() && operand.GetType() == IR::Type::U32) {
			return operand.U32();
		}
		return std::nullopt;
	}

	std::optional<Range> Compute(const IR::Inst& inst) {
		const auto arg = [&](size_t index) { return Of(inst.Arg(index)); };
		switch (inst.GetOpcode()) {
			case IR::ValueOpcode::LaneId: return UpTo(63u);
			case IR::ValueOpcode::BitCount32: return UpTo(32u);
			case IR::ValueOpcode::BitCount64: return UpTo(64u);
			case IR::ValueOpcode::IAdd32: {
				const auto a = arg(0);
				const auto b = a ? arg(1) : std::nullopt;
				return a && b ? UpTo(a->max + b->max) : std::nullopt;
			}
			case IR::ValueOpcode::IMul32: {
				const auto a = arg(0);
				const auto b = a ? arg(1) : std::nullopt;
				if (!a || !b || (a->max != 0 && b->max > Word / a->max)) {
					return std::nullopt;
				}
				return UpTo(a->max * b->max);
			}
			case IR::ValueOpcode::BitwiseAnd32: {
				const auto a = arg(0);
				const auto b = arg(1);
				if (a && b) {
					return Range {std::min(a->max, b->max), a->bits & b->bits};
				}
				return a ? a : b;
			}
			case IR::ValueOpcode::UMin32: {
				const auto a = arg(0);
				const auto b = arg(1);
				if (a && b) {
					return UpTo(std::min(a->max, b->max));
				}
				return a ? a : b;
			}
			case IR::ValueOpcode::BitwiseOr32:
			case IR::ValueOpcode::BitwiseXor32: {
				const auto a = arg(0);
				const auto b = a ? arg(1) : std::nullopt;
				if (!a || !b) {
					return std::nullopt;
				}
				const auto bits = a->bits | b->bits;
				return Range {std::min(bits, a->max + b->max), bits};
			}
			case IR::ValueOpcode::ShiftLeftLogical32: {
				const auto a     = arg(0);
				const auto shift = Immediate(inst, 1);
				if (!a || !shift || *shift >= 32u || (a->bits << *shift) > Word) {
					return std::nullopt;
				}
				return Range {a->max << *shift, a->bits << *shift};
			}
			case IR::ValueOpcode::ShiftRightLogical32: {
				const auto a     = arg(0);
				const auto shift = Immediate(inst, 1);
				if (a && shift && *shift < 32u) {
					return Range {a->max >> *shift, a->bits >> *shift};
				}
				return a;
			}
			case IR::ValueOpcode::BitFieldUExtract: {
				const auto offset = Immediate(inst, 1);
				const auto count  = Immediate(inst, 2);
				if (!count || *count > 32u) {
					return std::nullopt;
				}
				const uint64_t field = (uint64_t {1} << *count) - 1u;
				const auto     a     = arg(0);
				if (a && offset && *offset < 32u) {
					const auto bits = (a->bits >> *offset) & field;
					return Range {std::min(bits, a->max >> *offset), bits};
				}
				return Range {field, field};
			}
			case IR::ValueOpcode::SelectU32: {
				if (inst.Arg(0).Resolve() == m_predicate) {
					return arg(1);
				}
				const auto a = arg(1);
				const auto b = a ? arg(2) : std::nullopt;
				if (!a || !b) {
					return std::nullopt;
				}
				return Range {std::max(a->max, b->max), a->bits | b->bits};
			}
			case IR::ValueOpcode::Phi: {
				if (inst.NumArgs() == 0) {
					return std::nullopt;
				}
				Range merged {};
				for (size_t index = 0; index < inst.NumArgs(); index++) {
					const auto incoming = arg(index);
					if (!incoming) {
						return std::nullopt;
					}
					merged.max = std::max(merged.max, incoming->max);
					merged.bits |= incoming->bits;
				}
				return merged;
			}
			default: return std::nullopt;
		}
	}

	IR::Value                    m_predicate;
	std::vector<const IR::Inst*> m_path;
};

uint32_t FunctionLdsDwords(const IR::Program& program, uint32_t& unbounded_pc) {
	uint64_t end_bytes = 0;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto op     = inst.GetOpcode();
			const auto shared = IR::SharedAccessOf(op);
			const bool flat   = IR::AddressOpcodeInfoOf(op).access != IR::AddressAccess::None;
			if (shared == IR::SharedAccess::None && !flat) {
				continue;
			}
			const auto flags = inst.Flags<IR::MemoryFlags>();
			if (flags.index >= program.memory_info.size()) {
				continue;
			}
			const auto& memory = program.memory_info[flags.index];
			if (flat) {
				// A flat access in the LDS aperture addresses LDS with a runtime offset.
				if (memory.kind == IR::ResourceKind::FlatLocal) {
					unbounded_pc = flags.pc;
					return 0;
				}
				continue;
			}
			if (memory.kind != IR::ResourceKind::Lds) {
				continue;
			}
			if (shared != IR::SharedAccess::Read && shared != IR::SharedAccess::Write &&
			    shared != IR::SharedAccess::Atomic) {
				unbounded_pc = flags.pc;
				return 0;
			}
			// Every LDS operation's last operand is the predicate it runs under.
			LdsAddressRange range(inst.Arg(inst.NumArgs() - 1));
			const auto      address = range.Of(inst.Arg(0));
			if (!address.has_value()) {
				unbounded_pc = flags.pc;
				return 0;
			}
			end_bytes = std::max(end_bytes, address->max + memory.offset +
			                                    4ull * IR::SharedComponentCount(op));
		}
	}
	return static_cast<uint32_t>(std::clamp<uint64_t>((end_bytes + 3u) / 4u, 1u, 8192u));
}

} // namespace

Emitter::SpirvRequirements Emitter::AnalyzeProgramRequirements(const IR::Program& program) {
	SpirvRequirements requirements {};
	const auto refuse = [&](const IR::Inst& inst, const char* reason) {
		if (requirements.refusal == nullptr) {
			requirements.refusal      = reason;
			requirements.refused_inst = &inst;
		}
	};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (IR::BufferAccessOf(inst.GetOpcode()) != IR::BufferAccess::None) {
				const auto memory_index = inst.Flags<IR::MemoryFlags>().index;
				if (memory_index >= program.memory_info.size()) {
					Fail(program, "buffer operation has invalid memory metadata");
				}
				const auto& memory = program.memory_info[memory_index];
				// Must match the emitter's predicate, or a module can use the alias undeclared.
				if (!memory.planning_only &&
				    IR::BufferAccessOf(inst.GetOpcode()) != IR::BufferAccess::Atomic &&
				    Emitter::CoherentBufferAccess(memory)) {
					requirements.coherent_buffers = true;
				}
				// A planning-only read is never tracked or emitted; its resource is still a
				// register. A dynamic V# has no host resource for the checks below.
				if (!memory.planning_only && memory.kind == IR::ResourceKind::Buffer &&
				    !memory.dynamic_buffer) {
					requirements.coherent_buffers |= memory.coherent;
					// A glc DWORDX2 goes through the u64 alias as one access, see LoadWideBuffer.
					if (memory.glc && IR::BufferComponentCount(inst.GetOpcode()) == 2u) {
						requirements.wide_glc_buffers = true;
					}
					if (memory.resource >= program.info.buffers.size()) {
						Fail(program, "buffer operation has invalid resource metadata");
					}
					if ((program.info.buffers[memory.resource].packed_stride & (1u << 20u)) != 0u &&
					    program.stage != ShaderType::Compute) {
						refuse(inst, "buffer ADD_TID is only valid for compute shaders");
					}
				}
			}
			const auto shared_access = IR::SharedAccessOf(inst.GetOpcode());
			if (shared_access == IR::SharedAccess::Atomic) {
				const auto index = inst.Flags<IR::MemoryFlags>().index;
				if (index >= program.memory_info.size()) {
					Fail(program, "shared operation has invalid memory metadata");
				}
				const auto kind = program.memory_info[index].kind;
				// The aliased 64-bit view of LDS is a Workgroup block, and only a compute shader
				// keeps LDS in the Workgroup storage class.
				if ((IR::SharedComponentCount(inst.GetOpcode()) == 2u ||
				     inst.GetType() == IR::Type::U64) &&
				    (kind != IR::ResourceKind::Lds || program.stage != ShaderType::Compute)) {
					refuse(inst, "64-bit shared atomics require compute LDS");
				}
			}
			if (inst.GetOpcode() == IR::ValueOpcode::DispatchThreadInRange) {
				requirements.dispatch_thread_limit = true;
			}
		}
	}
	if (program.info.function_lds && !FunctionLdsDefaultForced()) {
		requirements.function_lds_dwords =
		    FunctionLdsDwords(program, requirements.function_lds_unbounded_pc);
	}
	return requirements;
}

bool Emitter::FunctionLdsDefaultForced() {
	static const bool forced = [] {
		const char* text = std::getenv("KYTY_FUNCTION_LDS_DEFAULT");
		return text != nullptr && std::strcmp(text, "0") != 0;
	}();
	return forced;
}

std::vector<uint32_t> EmitProgram(IR::Program& program, ShaderStageInputInfo input_info,
                                  uint32_t push_data_start_dword, std::string* refusal) {
	using namespace Emitter;

	if (program.stage != ShaderType::Compute && program.stage != ShaderType::Vertex &&
	    program.stage != ShaderType::Pixel && program.stage != ShaderType::Mesh &&
	    program.stage != ShaderType::Local && program.stage != ShaderType::TessellationControl &&
	    program.stage != ShaderType::TessellationEvaluation) {
		Fail(program, "binary SPIR-V emitter received an unsupported shader stage");
	}
	if (!program.srt_plan_complete || !program.resource_tracking_complete ||
	    !program.shader_info_complete) {
		Fail(program, "SPIR-V emitter requires a fully planned native shader program");
	}
	// An expanded indirect table binds its candidates beside the buffers the shader tracks.
	if (program.info.buffers.size() > IR::ShaderInfo::MaxDenseBuffers) {
		Fail(program, "native shader exceeds the buffer resource limit");
	}
	IR::ValidateProgram(program, true);
	program.bindings = {.push_data_start_dword = push_data_start_dword};
	EmitterState state(program, input_info);
	const auto refused = [&]() -> std::vector<uint32_t> {
		const auto* inst = state.refused_inst;
		auto        text = fmt::format("SPIR-V emission refused: opcode={} reason={}",
		                               inst != nullptr ? IR::ValueOpcodeName(inst->GetOpcode())
		                                               : std::string_view {"none"},
		                               state.refusal);
		if (refusal == nullptr) {
			EXIT("%s hash=0x%016" PRIx64 " stage=%u\n", text.c_str(), program.shader_hash,
			     static_cast<unsigned>(program.stage));
		}
		*refusal = std::move(text);
		return {};
	};
	if (state.requirements.refusal != nullptr) {
		state.Refuse(state.requirements.refusal, state.requirements.refused_inst);
		return refused();
	}
	const auto* workgroup = ShaderWorkgroupInput(program.stage, input_info);
	state.lane_count =
	    workgroup != nullptr && program.wave_size == 64u && workgroup->host_subgroup_size == 32u
	        ? 2u
	        : 1u;
	DefineModule(state);
	if (state.Refused()) {
		return refused();
	}
	ValidateNativeProgram(program, program.stage == ShaderType::Compute &&
	                                   input_info.compute != nullptr && input_info.compute->lds_storage);
	EmitProgram(state);
	if (state.Refused()) {
		return refused();
	}
	state.builder.AddEntryPoint(ExecutionModelForStage(state.program.stage), state.main_func,
	                            "main", state.interface_variables);

	return state.builder.Build();
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
