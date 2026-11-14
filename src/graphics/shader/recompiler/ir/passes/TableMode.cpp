#include "graphics/shader/recompiler/ir/passes/TableMode.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

std::optional<TablePlan::Operand> TableOperand(Value value, size_t slots);

// A phi whose incoming values, through the phis among them, are all one table operand (a loop's headers carrying a
// value the loop never changes, phi(x, phi(x, ...)) cycles constant propagation does not fold): that operand.
std::optional<TablePlan::Operand> TablePhiOperand(const Inst& phi, size_t slots) {
	std::optional<TablePlan::Operand> common;
	std::vector<const Inst*>          pending {&phi}, seen;
	while (!pending.empty()) {
		const auto* current = pending.back();
		pending.pop_back();
		if (std::ranges::find(seen, current) != seen.end()) continue;
		seen.push_back(current);
		if (seen.size() > 256) return std::nullopt;
		for (size_t i = 0; i < current->NumArgs(); ++i) {
			const auto incoming = current->Arg(i).Resolve();
			if (const auto* source = incoming.TryInstruction(); source != nullptr && source->GetOpcode() == ValueOpcode::Phi) {
				pending.push_back(source);
				continue;
			}
			const auto operand = TableOperand(incoming, slots);
			if (!operand || (common && !(*common == *operand))) return std::nullopt;
			common = operand;
		}
	}
	return common;
}

// A value table mode evaluates at the program's entry: an immediate, user data or an SRT slot below `slots` (or a phi
// of one of them alone: TablePhiOperand).
std::optional<TablePlan::Operand> TableOperand(Value value, size_t slots) {
	using Kind = TablePlan::Operand::Kind;
	value      = value.Resolve();
	if (value.IsImmediate()) {
		if (value.GetType() != Type::U32) return std::nullopt;
		return TablePlan::Operand {Kind::Immediate, value.U32()};
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) return std::nullopt;
	if (inst->GetOpcode() == ValueOpcode::Phi) return TablePhiOperand(*inst, slots);
	if (inst->GetOpcode() == ValueOpcode::GetUserData && inst->Arg(0).GetType() == Type::ScalarReg)
		return TablePlan::Operand {Kind::UserData, static_cast<uint32_t>(RegIndex(inst->Arg(0).ScalarRegister()))};
	if (inst->GetOpcode() != ValueOpcode::ReadConst) return std::nullopt;
	const auto slot = inst->Arg(1).Resolve();
	if (!slot.IsImmediate() || slot.GetType() != Type::U32 || slot.U32() >= slots) return std::nullopt;
	return TablePlan::Operand {Kind::Slot, slot.U32()};
}

} // namespace

bool EnterTableMode(Program& program, std::string& reason) {
	const auto refuse = [&](const char* why) {
		reason = why;
		return false;
	};
	if (!program.srt_plan_complete || !program.resource_tracking_complete) return refuse("no complete SRT plan");
	if (program.info.uses_gds) return refuse("GDS");
	TablePlan plan;
	plan.global_memory = program.info.uses_dma;
	// The flattened SRT's slots in order (a slot's address only depends on lower slots and user data).
	for (size_t slot = 0; slot < program.srt_reads.size(); ++slot) {
		if (program.srt_reads[slot].flat_offset != slot) return refuse("an SRT slot out of order");
		const auto* load = program.srt_reads[slot].value.Resolve().TryInstruction();
		// A read through a V# (s_buffer_load_dword): its base address words, its range (word 2) and its offset, an
		// immediate the slot's address takes whole.
		if (load != nullptr && load->GetOpcode() == ValueOpcode::ReadConstBuffer && load->NumArgs() == 2 &&
		    load->Flags<MemoryFlags>().index < program.memory_info.size()) {
			const auto& memory = program.memory_info[load->Flags<MemoryFlags>().index];
			const auto* handle = load->Arg(0).Resolve().TryInstruction();
			const auto  offset = load->Arg(1).Resolve();
			if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetBufferResource || handle->NumArgs() != 4 ||
			    static_cast<int32_t>(memory.offset) < 0 || !offset.IsImmediate() || offset.GetType() != Type::U32)
				return refuse("an SRT slot that is no scalar buffer load");
			const auto low = TableOperand(handle->Arg(0), slot), high = TableOperand(handle->Arg(1), slot),
			           records = TableOperand(handle->Arg(2), slot);
			const auto bytes   = (uint64_t {static_cast<uint32_t>(memory.offset)} + offset.U32()) & ~uint64_t {3};
			if (!low || !high || !records || bytes > INT32_MAX)
				return refuse("an SRT slot address table mode cannot evaluate");
			plan.slots.push_back({*low, *high, TablePlan::Operand {}, static_cast<int32_t>(bytes), *records, true});
			continue;
		}
		if (load == nullptr || load->GetOpcode() != ValueOpcode::LoadAddressU32 || load->NumArgs() != 4 ||
		    load->Flags<MemoryFlags>().index >= program.memory_info.size())
			return refuse("an SRT slot that is no address load");
		const auto& memory = program.memory_info[load->Flags<MemoryFlags>().index];
		const auto  active = load->Arg(3).Resolve();
		const auto* handle = load->Arg(0).Resolve().TryInstruction();
		if (memory.kind != ResourceKind::ScalarAddress || memory.address_is_full || !active.IsImmediate() ||
		    !active.U1() || handle == nullptr || handle->GetOpcode() != ValueOpcode::GetAddressResource ||
		    handle->NumArgs() != 2)
			return refuse("an SRT slot that is no scalar address load");
		const auto low = TableOperand(handle->Arg(0), slot), high = TableOperand(handle->Arg(1), slot);
		const auto offset = TableOperand(load->Arg(1), slot);
		if (!low || !high || !offset) return refuse("an SRT slot address table mode cannot evaluate");
		plan.slots.push_back({*low, *high, *offset,
		                      static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(memory.offset)) & ~3u)});
	}
	const auto words_of = [&](uint32_t source_index, uint32_t count, auto& words) {
		if (source_index >= program.descriptor_sources.size()) return false;
		const auto& source = program.descriptor_sources[source_index];
		if (source.dword_count != count || source.indirect_descriptor) return false;
		for (uint32_t i = 0; i < count; ++i) {
			const auto word = TableOperand(source.dwords[i], program.srt_reads.size());
			if (!word) return false;
			words[i] = *word;
		}
		return true;
	};
	// (Written and atomic buffers too: through their device address, as the renderer resolved them.)
	for (const auto& buffer: program.info.buffers) {
		if (buffer.indirect_root != BufferResource::NoIndirectBuffer ||
		    buffer.indirect_write_table || (buffer.packed_stride & ((1u << 14u) | (1u << 20u))) != 0)
			return refuse("an aliased, indirect, swizzled or ADD_TID buffer");
		if (!words_of(buffer.source, 4, plan.buffers.emplace_back()))
			return refuse("a buffer descriptor table mode cannot evaluate");
	}
	for (const auto& image: program.info.images) {
		if (image.mip_mode != ImageMipMode::None || image.indirect_root != ImageResource::NoIndirectImage ||
		    image.indirect_search_iterations != 0 || image.table_capacity != 0)
			return refuse("an indirect image or one of dynamic mip levels");
		if (image.atomic || ((image.written || image.resource_class == ImageResourceClass::Storage) &&
		                     program.stage != ShaderType::Pixel && program.stage != ShaderType::Compute))
			return refuse("an image the shader writes");
		auto& descriptor = plan.images.emplace_back();
		descriptor.count = image.source < program.descriptor_sources.size()
		                       ? program.descriptor_sources[image.source].dword_count
		                       : 0;
		if (descriptor.count != 4 && descriptor.count != 8) return refuse("an image descriptor of another size");
		if (!words_of(image.source, descriptor.count, descriptor.words))
			return refuse("an image descriptor table mode cannot evaluate");
	}
	for (const auto& sampler: program.info.samplers) {
		if (sampler.indirect_root != SamplerResource::NoIndirectSampler) return refuse("an indirect sampler");
		auto& descriptor = plan.samplers.emplace_back();
		descriptor.count = 4;
		if (!words_of(sampler.source, 4, descriptor.words))
			return refuse("a sampler descriptor table mode cannot evaluate");
	}
	// The slots the shader reads (from its block): the V#s' words, branch conditions and any read with a use other
	// than the address of planning-only loads (which only told the CPU-side plan where the slots are).
	using Kind = TablePlan::Operand::Kind;
	plan.gpu.assign(plan.slots.size(), 0);
	plan.cpu.assign(plan.slots.size(), 0);
	const auto mark = [](std::vector<uint8_t>& set, const TablePlan::Operand& operand) {
		if (operand.kind == Kind::Slot) set[operand.value] = 1;
	};
	const auto planning = [&](const Inst& inst) {
		const auto index = inst.Flags<MemoryFlags>().index;
		return (inst.GetOpcode() == ValueOpcode::LoadAddressU32 || inst.GetOpcode() == ValueOpcode::ReadConstBuffer) &&
		       index < program.memory_info.size() && program.memory_info[index].planning_only;
	};
	const auto planning_handle = [&](const Inst& handle) {
		return (handle.GetOpcode() == ValueOpcode::GetAddressResource ||
		        handle.GetOpcode() == ValueOpcode::GetBufferResource) &&
		       std::ranges::all_of(handle.Uses(), [&](const Use& use) { return planning(*use.user); });
	};
	for (const auto& words: plan.buffers)
		for (const auto& word: words) mark(plan.cpu, word);
	for (const auto* descriptors: {&plan.images, &plan.samplers})
		for (const auto& descriptor: *descriptors)
			for (uint32_t i = 0; i < descriptor.count; ++i) mark(plan.cpu, descriptor.words[i]);
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::ReadConst) continue;
			const auto slot_value = inst.Arg(1).Resolve();
			if (!slot_value.IsImmediate() || slot_value.U32() >= plan.gpu.size())
				return refuse("a flattened SRT read table mode cannot serve");
			for (const auto& use: inst.Uses())
				if (!planning_handle(*use.user)) plan.gpu[slot_value.U32()] = 1;
		}
	}
	// The renderer evaluates what the shader reads and the image and sampler words, with the slots their addresses
	// are read through.
	for (size_t slot = 0; slot < plan.slots.size(); ++slot) plan.cpu[slot] |= plan.gpu[slot];
	for (size_t slot = plan.slots.size(); slot-- > 0;) {
		if (plan.cpu[slot] == 0) continue;
		const auto& read = plan.slots[slot];
		for (const auto* operand: {&read.low, &read.high, &read.offset, &read.records}) mark(plan.cpu, *operand);
	}
	// The buffers whose format the code can decode at run time: formatted, only loaded from, by plain 32-bit loads.
	for (size_t i = 0; i < program.info.buffers.size() && i < 32; ++i) {
		const auto& buffer = program.info.buffers[i];
		if (buffer.formatted && !buffer.written && !buffer.atomic) plan.runtime_formats |= 1u << i;
	}
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (BufferComponentCount(inst.GetOpcode()) == 0 || inst.GetOpcode() == ValueOpcode::ReadConstBuffer) continue;
			const auto index = inst.Flags<MemoryFlags>().index;
			if (index >= program.memory_info.size()) continue;
			const auto& memory = program.memory_info[index];
			if (memory.kind != ResourceKind::Buffer || memory.resource >= 32) continue;
			if (BufferAccessOf(inst.GetOpcode()) != BufferAccess::Read || memory.typed || memory.data_bits != 32u ||
			    memory.IsCoherentBufferPairLoad(inst.GetOpcode()))
				plan.runtime_formats &= ~(1u << memory.resource);
		}
	}
	program.table_plan = std::move(plan);
	program.table_mode = true;
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
