#include "graphics/shader/recompiler/ShaderCalls.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "graphics/shader/recompiler/ir/passes/SsaRewrite.h"

#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler {

namespace {

// Encodings for the synthesized table-call dispatcher (GFX10 SOPP / SOPC).
constexpr uint32_t SoppBranch      = 0x02u;
constexpr uint32_t SoppCbranchScc1 = 0x05u;
constexpr uint32_t SopcCmpLgU32    = 0x07u;
constexpr uint32_t InlineLiteral   = 0xffu; // next dword is an immediate
constexpr uint64_t MaxTableBytes   = uint64_t {64} << 20u;
constexpr uint64_t TableChunkBytes = uint64_t {1} << 20u;
constexpr uint32_t MaxTableCallees = 64u;

uint32_t EncodeSopc(uint32_t op, uint32_t ssrc0, uint32_t ssrc1) {
	return 0xbf000000u | (op << 16u) | (ssrc1 << 8u) | ssrc0;
}

// Cross-layout jumps only need to decode as a branch: the CFG and the translator take the
// target from branch_target, so the displacement must not be range-checked (a linked body can
// sit farther away than S_BRANCH can encode).
uint32_t EncodeUncheckedBranch(uint32_t op) {
	return 0xbf800000u | (op << 16u);
}

uint32_t EncodeSoppBranch(uint32_t op, uint32_t pc, uint32_t target) {
	const auto words = (static_cast<int64_t>(target) - static_cast<int64_t>(pc) - 4) / 4;
	EXIT_IF(words < std::numeric_limits<int16_t>::min() || words > std::numeric_limits<int16_t>::max());
	return 0xbf800000u | (op << 16u) | (static_cast<uint32_t>(words) & 0xffffu);
}

void AppendSynthesized(ShaderSource::Linked& linked, std::initializer_list<uint32_t> words, uint32_t pc,
                       Decoder::Opcode expected, uint32_t branch_target = UINT32_MAX) {
	Decoder::Instruction inst;
	const std::vector<uint32_t> code(words);
	Decoder::DecodeInstruction(code, 0u, inst);
	EXIT_IF(inst.opcode != expected || inst.word_count != code.size());
	inst.pc = pc;
	if (branch_target != UINT32_MAX) inst.branch_target = branch_target;
	linked.code.insert(linked.code.end(), code.begin(), code.end());
	linked.decoded.instructions.push_back(std::move(inst));
}

// Folds an expression of U32 immediates (the translator masks shift counts with BitwiseAnd, and
// constant propagation has not run yet when call targets are prepared).
std::optional<uint32_t> ConstantU32(IR::Value value, uint32_t depth = 0) {
	using namespace IR;
	value = value.Resolve();
	if (value.IsImmediate()) {
		if (value.GetType() != Type::U32) return std::nullopt;
		return value.U32();
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr || depth > 4u || inst->NumArgs() != 2u) return std::nullopt;
	const auto a = ConstantU32(inst->Arg(0), depth + 1u);
	const auto b = ConstantU32(inst->Arg(1), depth + 1u);
	if (!a || !b) return std::nullopt;
	switch (inst->GetOpcode()) {
		case ValueOpcode::BitwiseAnd32: return *a & *b;
		case ValueOpcode::IAdd32: return *a + *b;
		case ValueOpcode::IMul32: return *a * *b;
		case ValueOpcode::ShiftLeftLogical32: return *b < 32u ? *a << *b : 0u;
		default: return std::nullopt;
	}
}

// Recognizes a call target pair read as ReadConstBuffer(V#, index * stride) + immediate from one
// runtime-evaluable buffer descriptor: a function table selected by a GPU value.
std::optional<ShaderSource::Call::Table> MatchCallTable(const IR::Program& query,
                                                        const std::array<const IR::Inst*, 2>& reads) {
	using namespace IR;
	if (reads[0] == nullptr || reads[1] == nullptr) return std::nullopt;
	const auto* handle = reads[0]->Arg(0).ResolveInstruction();
	if (handle == nullptr || handle->NumArgs() != 4u ||
	    reads[1]->Arg(0).ResolveInstruction() != handle ||
	    reads[0]->Arg(1).Resolve() != reads[1]->Arg(1).Resolve())
		return std::nullopt;
	ShaderSource::Call::Table table;
	for (uint32_t i = 0; i < 4u; ++i) {
		const auto word = handle->Arg(i).Resolve();
		if (!ValidateRuntimeValue(query, word, RuntimeValueType::Integer)) return std::nullopt;
		table.descriptor[i] = word;
	}
	auto     offset   = reads[0]->Arg(1).Resolve();
	uint32_t constant = 0;
	if (const auto* add = offset.TryInstruction();
	    add != nullptr && add->GetOpcode() == ValueOpcode::IAdd32) {
		if (const auto b = ConstantU32(add->Arg(1))) {
			constant = *b;
			offset   = add->Arg(0).Resolve();
		} else if (const auto a = ConstantU32(add->Arg(0))) {
			constant = *a;
			offset   = add->Arg(1).Resolve();
		}
	}
	const auto* scale = offset.TryInstruction();
	if (scale == nullptr || scale->NumArgs() != 2u) return std::nullopt;
	const auto amount = ConstantU32(scale->Arg(1));
	if (!amount) return std::nullopt;
	if (scale->GetOpcode() == ValueOpcode::ShiftLeftLogical32 && *amount >= 2u && *amount < 16u) {
		table.stride = 1u << *amount;
	} else if (scale->GetOpcode() == ValueOpcode::IMul32 && *amount >= 8u && *amount <= 0x10000u &&
	           *amount % 4u == 0u && std::has_single_bit(*amount)) {
		// A 32-bit index product wraps modulo 2^32. The scan walks a fixed stride grid, which
		// covers every wrapped offset only when the stride divides 2^32 (a power of two);
		// for any other stride an index can select an offset between grid entries.
		table.stride = *amount;
	} else {
		return std::nullopt;
	}
	std::array<uint32_t, 2> position {};
	for (uint32_t i = 0; i < 2u; ++i) {
		const auto index = reads[i]->Flags<MemoryFlags>().index;
		if (index >= query.memory_info.size()) return std::nullopt;
		position[i] = (query.memory_info[index].offset & ~3u) + (constant & ~3u);
	}
	if (position[1] != position[0] + 4u || position[1] + 4u > table.stride) return std::nullopt;
	table.low_offset = position[0];
	return table;
}

} // namespace

void PrepareCallTargets(ShaderSource& source, const CompileOptions& options) {
	using namespace IR;
	EXIT_NOT_IMPLEMENTED(options.stage != ShaderType::Compute);
	source.wave_size = options.wave_size;
	for (uint32_t i = 0; i < source.decoded.instructions.size(); ++i) {
		if (source.decoded.instructions[i].opcode != Decoder::Opcode::S_SWAPPC_B64) continue;
		source.calls.push_back({.instruction = i});
	}
	const auto graph = CFG::BuildGraph(source.decoded);
	Program query;
	query.stage = options.stage;
	query.user_data_base = options.user_data_base;
	query.user_data_count = static_cast<uint32_t>(options.user_data.size());
	query.wave_size = options.wave_size;
	auto& block = *query.block_storage.emplace_back(std::make_unique<Block>());
	query.blocks.push_back(&block);
	struct Producer { bool ready = false; std::map<uint32_t, Value> values; };
	std::map<uint32_t, Producer> producers;
	std::function<Value(uint32_t, uint32_t)> Register;
	std::function<Value(uint32_t, uint32_t)> Produce;
	const auto Definitions = [&](uint32_t before, uint32_t code) {
		std::vector<uint32_t> crossed_calls;
		auto definitions = CFG::FindScalarDefinitions(source.decoded, graph, before, code,
		                                              options.wave_size, crossed_calls);
		for (const auto crossed: crossed_calls) {
			std::ranges::find(source.calls, crossed, &ShaderSource::Call::instruction)
			    ->preserved_registers.set(code);
		}
		return definitions;
	};
	std::function<Value(uint32_t, Value)> Resolve = [&](uint32_t before, Value value) -> Value {
		value = value.Resolve();
		auto* inst = value.TryInstruction();
		if (inst == nullptr) return value;
		uint32_t code = UINT32_MAX;
		switch (inst->GetOpcode()) {
			case ValueOpcode::GetScalarRegister: code = static_cast<uint32_t>(inst->Arg(0).ScalarRegister()); break;
			case ValueOpcode::GetVccLo: code = 106u; break;
			case ValueOpcode::GetVccHi: code = 107u; break;
			default: break;
		}
		if (code != UINT32_MAX) {
			const auto result = Register(before, code);
			inst->ReplaceUsesWith(result);
			return result;
		}
		for (size_t i = 0; i < inst->NumArgs(); ++i) inst->SetArg(i, Resolve(before, inst->Arg(i)));
		return value;
	};
	Register = [&](uint32_t before, uint32_t code) -> Value {
		const auto definitions = Definitions(before, code);
		// An operand selected by GPU data cannot be planned. Returning an undefined value makes
		// the call take the dispatch-skip fallback and avoids recursing into a loop-carried
		// producer; only the target registers themselves keep several candidates.
		if (definitions.size() > 1u) return Value(&block.AppendNewInst(ValueOpcode::UndefU32));
		return Produce(definitions.front(), code);
	};
	Produce = [&](uint32_t index, uint32_t code) -> Value {
		if (index == UINT32_MAX) {
			// A launch register outside the user-data span (s4 as the workgroup id, for example)
			// cannot be planned. Return an unplannable value so the call takes the dispatch-skip
			// path instead of terminating emulation.
			if (code < query.user_data_base ||
			    code - query.user_data_base >= query.user_data_count)
				return Value(&block.AppendNewInst(ValueOpcode::UndefU32));
			return IREmitter(&block).GetUserData(static_cast<ScalarReg>(code));
		}
		auto [entry, inserted] = producers.try_emplace(index);
		auto& producer = entry->second;
		if (inserted) {
			Block translated;
			Frontend::Translator translator(query, &translated, 1u, Frontend::NoLaneSpills,
			    options.input_info.compute->float_mode);
			translator.TranslateInstruction(source.decoded.instructions[index]);
			for (const auto& inst: translated) {
				uint32_t destination = UINT32_MAX;
				Value value;
				switch (inst.GetOpcode()) {
					case ValueOpcode::SetScalarRegister:
						destination = static_cast<uint32_t>(inst.Arg(0).ScalarRegister()); value = inst.Arg(1); break;
					case ValueOpcode::SetVccLo: destination = 106u; value = inst.Arg(0); break;
					case ValueOpcode::SetVccHi: destination = 107u; value = inst.Arg(0); break;
					default: break;
				}
				if (destination != UINT32_MAX) producer.values.insert_or_assign(destination, value);
			}
			for (auto& [destination, value]: producer.values) value = Resolve(index, value);
			for (auto& inst: translated) inst.SetParent(&block);
			block.Instructions().splice(block.end(), translated.Instructions());
			producer.ready = true;
		}
		EXIT_IF(!producer.ready || !producer.values.contains(code));
		return producer.values.at(code).Resolve();
	};
	for (auto& site: source.calls) {
		const auto& call = source.decoded.instructions[site.instruction];
		EXIT_NOT_IMPLEMENTED(call.src0.kind != Decoder::OperandKind::Sgpr ||
		                     call.dst.kind != Decoder::OperandKind::Sgpr ||
		                     call.src0.reg != call.dst.reg || call.dst.reg >= 105u);
		// For each target word: every reaching definition becomes a candidate value that the CPU
		// evaluates at dispatch. A single definition that the CPU cannot evaluate (it depends on
		// GPU data) may still be a read from a function table; remember it for MatchCallTable.
		std::array<const Inst*, 2> table_reads {};
		for (uint32_t i = 0; i < 2u; ++i) {
			const auto definitions = Definitions(site.instruction, call.src0.reg + i);
			if (definitions.size() > 1u) {
				LOGF("ShaderRecompiler CS call at pc 0x%08x: s%u has %u reaching definitions\n",
				     call.pc, call.src0.reg + i, static_cast<uint32_t>(definitions.size()));
			}
			for (const auto definition: definitions) {
				const auto value = Produce(definition, call.src0.reg + i);
				if (!ValidateRuntimeValue(query, value, RuntimeValueType::Integer)) {
					// Only a single buffer read can be a function table; any other target that
					// depends on GPU data takes the dispatch-skip fallback.
					const auto* read = value.TryInstruction();
					if (definitions.size() == 1u && read != nullptr &&
					    read->GetOpcode() == ValueOpcode::ReadConstBuffer) {
						table_reads[i] = read;
						continue;
					}
					LOGF("ShaderRecompiler CS call at pc 0x%08x: s%u depends on GPU data and is not a "
					     "function-table read\n", call.pc, call.src0.reg + i);
					source.unsupported_call_pc = call.pc;
					source.calls.clear();
					return;
				}
				auto& root = query.value_storage.emplace_back(ValueOpcode::ReferenceU32);
				root.SetArg(0, value);
				site.target[i].push_back(Value(&root));
			}
		}
		if (table_reads[0] != nullptr || table_reads[1] != nullptr) {
			auto table = MatchCallTable(query, table_reads);
			if (!table || !options.function_table_calls) {
				LOGF("ShaderRecompiler CS call at pc 0x%08x: GPU-selected target is %s\n", call.pc,
				     table ? "a function table (linking disabled; see --function-table-calls)"
				           : "not a recognized function table");
				source.unsupported_call_pc = call.pc;
				source.calls.clear();
				return;
			}
			// Root the descriptor words like targets so the cleanup passes below keep them alive;
			// the per-entry targets are read from memory at refresh time instead.
			for (auto& word: table->descriptor) {
				auto& root = query.value_storage.emplace_back(ValueOpcode::ReferenceU32);
				root.SetArg(0, word);
				word = Value(&root);
			}
			site.target = {};
			site.table  = std::move(table);
			LOGF("ShaderRecompiler CS call at pc 0x%08x: linked through a function table "
			     "(stride %u, low word at +%u)\n", call.pc, site.table->stride, site.table->low_offset);
		}
	}
	RewriteToSsa(query.blocks);
	ConstantPropagationPass(query.blocks, query.wave_size);
	RemoveIdentities(query);
	EliminateDeadCode(query.blocks);
	// Strip the ReferenceU32 roots: keep the simplified values they point at.
	for (auto& site: source.calls) {
		for (auto& candidates: site.target)
			for (auto& value: candidates) value = value.Instruction()->Arg(0).Resolve();
		if (site.table)
			for (auto& word: site.table->descriptor) word = word.Instruction()->Arg(0).Resolve();
	}
	query.value_storage.clear();
	for (auto& inst: block) {
		inst.SetParent(nullptr);
		if (inst.GetOpcode() == ValueOpcode::LoadAddressU32 || inst.GetOpcode() == ValueOpcode::ReadConstBuffer)
			inst.SetFlags(MemoryFlags {.index = inst.Flags<MemoryFlags>().index});
	}
	query.value_storage.splice(query.value_storage.end(), block.Instructions());
	source.call_targets = std::move(static_cast<ResourcePlan&>(query));
	std::vector<Value> targets;
	for (const auto& site: source.calls) {
		for (const auto& candidates: site.target) targets.insert(targets.end(), candidates.begin(), candidates.end());
		if (site.table) targets.insert(targets.end(), site.table->descriptor.begin(), site.table->descriptor.end());
	}
	CompileSrtWalker(source.call_targets, targets);
}

const Decoder::Program& RefreshShaderSource(ShaderSource& source, const IR::SrtRuntime& runtime) {
	EXIT_IF(source.unsupported_call_pc != UINT32_MAX);
	// A refresh re-reads the tables and re-evaluates the targets; an earlier refresh's failure
	// says nothing about this one. The pipeline retries these sources on every dispatch.
	source.unlinkable_call_pc = UINT32_MAX;
	auto& reads = source.reads;
	reads.clear();
	source.table_reads.clear();
	std::vector<uint32_t> flat;
	const bool walked = IR::WalkSrt(source.call_targets, runtime, flat, &reads);
	const auto read_memory = [&](uint64_t address, std::span<uint32_t> words) {
		if (runtime.read_specialization_memory == nullptr ||
		    !runtime.read_specialization_memory(runtime.userdata, address, words)) return false;
		reads.emplace_back(address, words.size_bytes());
		return true;
	};
	const auto read_table = [&](uint64_t address, std::span<uint32_t> words) {
		if (!read_memory(address, words)) return false;
		source.table_reads.emplace_back(address, words.size_bytes());
		return true;
	};
	for (auto& site: source.calls) {
		const auto pc = source.decoded.instructions[site.instruction].pc;
		if (site.table) {
			// A missing specialization reader is a setup error, not unreadable guest data.
			EXIT_IF(runtime.read_specialization_memory == nullptr);
			// Read every entry of the function table; each distinct target becomes a callee.
			std::array<uint32_t, 4> descriptor {};
			for (uint32_t i = 0; i < 4u; ++i) {
				if (!walked || !IR::WalkedValue(source.call_targets, site.table->descriptor[i], descriptor[i]))
					EXIT("shader call at pc 0x%08x has an unavailable function table", pc);
			}
			// V# layout: base[47:0] in dwords 0-1, stride in dword 1 [29:16], num_records in dword 2.
			// Scalar buffer loads count records in bytes when the stride is zero.
			const auto base   = ((uint64_t {descriptor[1] & 0xffffu} << 32u) | descriptor[0]) & ~uint64_t {3};
			const auto stride = (descriptor[1] >> 16u) & 0x3fffu;
			const auto size   = stride == 0u ? uint64_t {descriptor[2]} : uint64_t {stride} * descriptor[2];
			const auto& table = *site.table;
			site.table_targets.clear();
			// Tables can be large (megabytes, with most entries empty), so read them in chunks: one
			// recorded read per chunk instead of one per entry. A descriptor that claims more than
			// that is guest data we cannot link, not a reason to terminate emulation.
			if (size > MaxTableBytes) {
				LOGF("shader call at pc 0x%08x: function table is 0x%llx bytes; skipping the dispatch\n",
				     pc, static_cast<unsigned long long>(size));
				source.unlinkable_call_pc = pc;
				return source.decoded;
			}
			std::vector<uint32_t> chunk;
			for (uint64_t chunk_start = 0; chunk_start < size; chunk_start += TableChunkBytes) {
				const auto chunk_bytes = std::min<uint64_t>(TableChunkBytes, size - chunk_start) & ~uint64_t {3};
				if (chunk_bytes == 0u) break;
				chunk.resize(chunk_bytes / 4u);
				if (!read_table(base + chunk_start, chunk)) {
					LOGF("shader call at pc 0x%08x cannot read its function table at +0x%llx; "
					     "skipping the dispatch\n", pc, static_cast<unsigned long long>(chunk_start));
					source.unlinkable_call_pc = pc;
					return source.decoded;
				}
				// Each entry belongs to the chunk where its target pair starts, so every pair is
				// visited exactly once: the first entry here is the first whose pair starts at or
				// after chunk_start (the entry itself may start in the previous chunk). A pair that
				// straddles the chunk end is re-read on its own. Both cases need a stride that does
				// not divide the chunk size.
				const auto first_entry =
				    chunk_start <= table.low_offset
				        ? 0u
				        : (chunk_start - table.low_offset + table.stride - 1u) / table.stride * table.stride;
				for (uint64_t entry = first_entry; entry + table.low_offset + 8u <= size &&
				                                   entry + table.low_offset < chunk_start + chunk_bytes;
				     entry += table.stride) {
					const auto at = entry + table.low_offset - chunk_start;
					std::array<uint32_t, 2> words {};
					if (at + 8u <= chunk_bytes) {
						words = {chunk[at / 4u], chunk[at / 4u + 1u]};
					} else if (!read_table(base + entry + table.low_offset, words)) {
						LOGF("shader call at pc 0x%08x cannot read its function table; "
						     "skipping the dispatch\n", pc);
						source.unlinkable_call_pc = pc;
						return source.decoded;
					}
					// Zero entries are unused. Targets are kept raw (with their low bits):
					// the dispatcher compares the exact register values the shader loads.
					const auto target = (uint64_t {words[1]} << 32u) | words[0];
					if ((target & ~uint64_t {3}) == 0u ||
					    std::ranges::find(site.table_targets, target) != site.table_targets.end())
						continue;
					site.table_targets.push_back(target);
					if (site.table_targets.size() > MaxTableCallees) {
						LOGF("shader call at pc 0x%08x: function table has over %u callees; "
						     "skipping the dispatch\n", pc, MaxTableCallees);
						source.unlinkable_call_pc = pc;
						return source.decoded;
					}
				}
			}
			// A table with no live entry means the buffer read at pipeline-build time was empty
			// (typically GPU-written or not uploaded yet). Running the shader would skip the call
			// and break its control flow, so fall back to the old dispatch-skip behavior. The
			// pipeline keeps the source and retries it, so a later upload can still link.
			if (site.table_targets.empty()) {
				LOGF("shader call at pc 0x%08x: function table is empty; skipping the dispatch\n", pc);
				source.unlinkable_call_pc = pc;
				return source.decoded;
			}
			site.address = 0;
			continue;
		}
		// Plain call: evaluate every candidate of each target word; they must all agree.
		std::array<uint32_t, 2> words {};
		for (uint32_t i = 0; i < 2u; ++i) {
			EXIT_IF(site.target[i].empty());
			for (size_t c = 0; c < site.target[i].size(); ++c) {
				uint32_t word = 0;
				if (!walked || !IR::WalkedValue(source.call_targets, site.target[i][c], word))
					EXIT("shader call at pc 0x%08x has an unavailable scalar target", pc);
				// Distinct candidates are fine as long as they name the same callee. SWAPPC
				// ignores the target's low two bits, so candidates that differ only there are
				// the same address; the high word is compared in full. If they still disagree
				// the target depends on GPU data we cannot plan, so skip this dispatch instead
				// of terminating emulation (the guest does not need it to boot).
				const auto mask = i == 0u ? ~uint32_t {3} : ~uint32_t {0};
				if (c != 0 && (word & mask) != (words[i] & mask)) {
					LOGF("shader call at pc 0x%08x selects between callees at runtime "
					     "(s%u candidates 0x%08x and 0x%08x); skipping the dispatch\n", pc,
					     source.decoded.instructions[site.instruction].src0.reg + i, words[i], word);
					source.unlinkable_call_pc = pc;
					return source.decoded;
				}
				words[i] = word;
			}
		}
		const auto low = words[0], high = words[1];
		// SWAPPC reads the old aliased pair and ignores the target's low two bits.
		const auto address = ((uint64_t {high} << 32u) | low) & ~uint64_t {3};
		EXIT_IF(address == 0u);
		site.address = address;
	}
	// Callees in link order: one per plain call, one per distinct table target.
	std::vector<uint64_t> callees;
	for (const auto& site: source.calls) {
		if (!site.table) {
			callees.push_back(site.address);
			continue;
		}
		for (const auto target: site.table_targets) callees.push_back(target & ~uint64_t {3});
	}
	const auto query_reads = reads.size();
	if (source.linked) {
		auto& linked = *source.linked;
		// Plain callees may move if their code is unchanged; table targets are compared by value
		// because the dispatcher compares them.
		bool unchanged = linked.callees.size() == callees.size() && linked.tables.size() == source.calls.size();
		for (size_t i = 0; unchanged && i < source.calls.size(); ++i)
			unchanged = linked.tables[i] == source.calls[i].table_targets;
		// Then every linked body must still match guest memory. A callee seen earlier in the list
		// is compared against that earlier body instead of being re-read.
		for (size_t i = 0; unchanged && i < callees.size(); ++i) {
			const auto& function = linked.functions[i];
			const auto expected = std::span(linked.code).subspan(function.offset, function.size);
			const auto preceding = std::span(callees).first(i);
			const auto previous = std::ranges::find(preceding, callees[i]);
			if (previous != preceding.end()) {
				const auto& observed = linked.functions[previous - preceding.begin()];
				if (std::ranges::equal(expected, std::span(linked.code).subspan(observed.offset, observed.size))) continue;
				unchanged = false;
				break;
			}
			linked.observed_function.resize(function.size);
			if (!read_memory(callees[i], linked.observed_function) ||
			    !std::ranges::equal(expected, linked.observed_function)) {
				unchanged = false;
				break;
			}
		}
		if (unchanged) {
			linked.callees = std::move(callees);
			return linked.decoded;
		}
		reads.resize(query_reads);
	}
	ShaderSource::Linked linked {.code = source.code, .decoded = source.decoded};
	const auto& last = source.decoded.instructions.back();
	EXIT_NOT_IMPLEMENTED(last.opcode != Decoder::Opcode::S_ENDPGM);
	// Keep the caller's data/footer at native offsets. Expand leaf CFGs per call site
	// before SSA, so return edges neither require a dispatcher nor merge resource roots.
	uint32_t function_pc = last.pc + last.word_count * sizeof(uint32_t);
	linked.decoded.native_bytes = function_pc;
	std::map<uint64_t, std::vector<Decoder::Instruction>> functions;
	// Decodes the leaf at `address` once; returns its instructions (pcs relative to the leaf).
	const auto load = [&](const ShaderSource::Call& site,
	                      uint64_t address) -> const std::vector<Decoder::Instruction>* {
		const auto& call = source.decoded.instructions[site.instruction];
		// A missing specialization reader is a setup error; an unreadable leaf is guest data.
		EXIT_IF(runtime.read_specialization_memory == nullptr);
		auto [entry, inserted] = functions.try_emplace(address);
		auto& function = entry->second;
		if (!inserted) return &function;
		for (uint32_t index = 0;;) {
			std::array<uint32_t, Decoder::MaxInstructionRawWords> words {};
			EXIT_IF(index > UINT32_MAX / sizeof(uint32_t) - words.size() ||
			        address > UINT64_MAX - (uint64_t {index} + words.size()) * sizeof(uint32_t));
			uint32_t count = 1;
			Decoder::Instruction inst;
			for (;;) {
				if (!read_memory(address + uint64_t {index} * sizeof(uint32_t), std::span(words).first(count))) {
					// Guest code that is not mapped yet is a retryable skip, not a fatal error.
					LOGF("shader call at pc 0x%08x cannot read leaf code; skipping the dispatch\n", call.pc);
					functions.erase(entry);
					return nullptr;
				}
				inst = {};
				Decoder::DecodeInstruction(words, 0u, inst);
				if (inst.word_count <= count) break;
				count = inst.word_count;
			}
			inst.pc = index * sizeof(uint32_t);
			if (Decoder::IsDirectBranch(inst.opcode)) inst.branch_target += inst.pc;
			index += inst.word_count;
			EXIT_NOT_IMPLEMENTED(inst.opcode == Decoder::Opcode::S_SWAPPC_B64 ||
			                     inst.opcode == Decoder::Opcode::S_GETPC_B64 ||
			                     inst.opcode == Decoder::Opcode::S_ENDPGM);
			function.push_back(inst);
			if (inst.opcode == Decoder::Opcode::S_SETPC_B64) break;
		}
		return &function;
	};
	const auto byte_size = [](const std::vector<Decoder::Instruction>& function) {
		return function.back().pc + function.back().word_count * static_cast<uint32_t>(sizeof(uint32_t));
	};
	// Appends the leaf at `address` at function_pc; its return branches to return_pc.
	const auto append_body = [&](const ShaderSource::Call& site, uint64_t address, uint32_t return_pc) {
		const auto& call     = source.decoded.instructions[site.instruction];
		const auto* loaded   = load(site, address);
		EXIT_IF(loaded == nullptr);
		const auto&    function      = *loaded;
		const uint32_t function_size = byte_size(function);
		EXIT_IF(function_pc > UINT32_MAX - function_size);
		const auto function_offset = static_cast<uint32_t>(linked.code.size());
		const auto body_pc         = function_pc;
		// Copy the leaf, rebasing its pcs and branches to function_pc. The leaf must not write the
		// return-address pair or any register the caller still needs after the call.
		for (auto inst: function) {
			EXIT_IF(CFG::MayWriteScalarRegister(inst, call.dst.reg, source.wave_size) ||
			        CFG::MayWriteScalarRegister(inst, call.dst.reg + 1u, source.wave_size));
			for (uint32_t code = 0; code < site.preserved_registers.size(); ++code) {
				if (site.preserved_registers[code] &&
				    CFG::MayWriteScalarRegister(inst, code, source.wave_size))
					EXIT("shader call at pc 0x%08x clobbers scalar target dependency s%u", call.pc, code);
			}
			linked.code.insert(linked.code.end(), inst.raw, inst.raw + inst.word_count);
			if (inst.opcode == Decoder::Opcode::S_SETPC_B64) {
				EXIT_NOT_IMPLEMENTED(inst.src0.kind != Decoder::OperandKind::Sgpr || inst.src0.reg != call.dst.reg);
				inst.opcode = Decoder::Opcode::S_BRANCH;
				inst.branch_target = return_pc;
			} else if (Decoder::IsDirectBranch(inst.opcode)) {
				EXIT_IF(inst.branch_target >= function_size);
				inst.branch_target += function_pc;
			}
			inst.pc += function_pc;
			linked.decoded.instructions.push_back(std::move(inst));
		}
		linked.functions.push_back({function_offset, function_size / 4u});
		linked.callees.push_back(address);
		function_pc += function_size;
		return body_pc;
	};
	for (const auto& site: source.calls) {
		const auto& call = source.decoded.instructions[site.instruction];
		const auto return_pc = call.pc + call.word_count * static_cast<uint32_t>(sizeof(uint32_t));
		// A table with one live entry always selects that callee, so link it like a plain call.
		if (!site.table || site.table_targets.size() == 1u) {
			const auto address = site.table ? site.table_targets.front() & ~uint64_t {3} : site.address;
			if (load(site, address) == nullptr) {
				source.unlinkable_call_pc = call.pc;
				return source.decoded;
			}
			linked.decoded.instructions[site.instruction].branch_target = append_body(site, address, return_pc);
			continue;
		}
		// Dispatcher, appended after the caller, followed by one body per target:
		//   per target k:  s_cmp_lg_u32 s[lo], k.low  ; s_cbranch_scc1 next
		//                  s_cmp_lg_u32 s[hi], k.high ; s_cbranch_scc1 next
		//                  s_swappc_b64 (linked to body k, which returns to the s_mov below)
		//                  s_mov_b32 s[lo], k.low ; s_mov_b32 s[hi], k.high
		//                  s_branch end
		//   end:           s_branch return_pc
		// The structurizer turns "s_branch end" into a guarded goto, so later compares see the
		// pair through a phi. Restoring the matched target there keeps the return PC (native
		// shader address) from flowing into them; callers never read the return PC back.
		// SWAPPC ignores the target's low two bits, and the table holds the exact pairs compared
		// here; a target outside the table (impossible while it is unchanged) skips the call.
		EXIT_NOT_IMPLEMENTED(call.word_count != 1u);
		// 48 bytes per target:
		//   +0  s_cmp_lg_u32 lo, k.low   (2 words)
		//   +8  s_cbranch_scc1 next
		//   +12 s_cmp_lg_u32 hi, k.high  (2 words)
		//   +20 s_cbranch_scc1 next
		//   +24 s_swappc_b64 (linked to body k)
		//   +28 s_mov_b32 lo, k.low      (2 words)
		//   +36 s_mov_b32 hi, k.high     (2 words)
		//   +44 s_branch end
		// The compares write SCC, which SWAPPC would preserve. Keeping it needs a scratch
		// register, and every candidate (VCC, M0, spare SGPRs) is guest-visible: stashing SCC in
		// one of them only moves the clobber. The chain therefore leaves SCC at 0 for the leaf.
		constexpr uint32_t EntryBytes = 48u;
		const auto& targets  = site.table_targets;
		const auto  chain_pc = function_pc;
		const auto  end_pc   = chain_pc + static_cast<uint32_t>(targets.size()) * EntryBytes;
		// Bodies are laid out after the dispatcher, so their pcs are known before any is appended
		// and the chain's SWAPPCs can name them directly.
		std::vector<uint32_t> body_pcs;
		uint32_t next_body = end_pc + 4u;
		for (const auto target: targets) {
			const auto address = target & ~uint64_t {3};
			if (load(site, address) == nullptr) {
				source.unlinkable_call_pc = call.pc;
				return source.decoded;
			}
			body_pcs.push_back(next_body);
			next_body += byte_size(*load(site, address));
		}
		const auto low_sgpr = call.src0.reg, high_sgpr = call.src0.reg + 1u;
		const auto compare  = [](uint32_t sgpr) {
			return EncodeSopc(SopcCmpLgU32, sgpr, InlineLiteral);
		};
		const auto move     = [](uint32_t sgpr) { return 0xbe8003ffu | (sgpr << 16u); }; // s_mov_b32 sgpr, literal
		for (size_t k = 0; k < targets.size(); ++k) {
			const auto entry = chain_pc + static_cast<uint32_t>(k) * EntryBytes;
			const auto next  = entry + EntryBytes; // the last entry's next is end_pc
			AppendSynthesized(linked, {compare(low_sgpr), static_cast<uint32_t>(targets[k])}, entry,
			                  Decoder::Opcode::S_CMP_LG_U32);
			AppendSynthesized(linked, {EncodeSoppBranch(SoppCbranchScc1, entry + 8u, next)}, entry + 8u,
			                  Decoder::Opcode::S_CBRANCH_SCC1, next);
			AppendSynthesized(linked, {compare(high_sgpr), static_cast<uint32_t>(targets[k] >> 32u)},
			                  entry + 12u, Decoder::Opcode::S_CMP_LG_U32);
			AppendSynthesized(linked, {EncodeSoppBranch(SoppCbranchScc1, entry + 20u, next)}, entry + 20u,
			                  Decoder::Opcode::S_CBRANCH_SCC1, next);
			AppendSynthesized(linked, {call.raw[0]}, entry + 24u, Decoder::Opcode::S_SWAPPC_B64, body_pcs[k]);
			AppendSynthesized(linked, {move(low_sgpr), static_cast<uint32_t>(targets[k])}, entry + 28u,
			                  Decoder::Opcode::S_MOV_B32);
			AppendSynthesized(linked, {move(high_sgpr), static_cast<uint32_t>(targets[k] >> 32u)}, entry + 36u,
			                  Decoder::Opcode::S_MOV_B32);
			AppendSynthesized(linked, {EncodeUncheckedBranch(SoppBranch)}, entry + 44u,
			                  Decoder::Opcode::S_BRANCH, end_pc);
		}
		// No entry matched (or a matched callee returned and branched here): continue after the call.
		AppendSynthesized(linked, {EncodeUncheckedBranch(SoppBranch)}, end_pc,
		                  Decoder::Opcode::S_BRANCH, return_pc);
		function_pc = end_pc + 4u;
		for (size_t k = 0; k < targets.size(); ++k) {
			const auto return_to = chain_pc + static_cast<uint32_t>(k) * EntryBytes + 28u; // the s_mov
			EXIT_IF(append_body(site, targets[k] & ~uint64_t {3}, return_to) != body_pcs[k]);
		}
		// The native call jumps to the dispatcher instead.
		const auto jump = EncodeUncheckedBranch(SoppBranch);
		const std::vector<uint32_t> jump_code {jump};
		Decoder::Instruction branch;
		Decoder::DecodeInstruction(jump_code, 0u, branch);
		EXIT_IF(branch.opcode != Decoder::Opcode::S_BRANCH);
		branch.pc            = call.pc;
		branch.branch_target = chain_pc;
		linked.decoded.instructions[site.instruction] = branch;
		EXIT_IF(call.pc / 4u >= linked.code.size());
		linked.code[call.pc / 4u] = jump;
	}
	// Record each distinct body as a source read, so a later write to the leaf code (or the
	// table, read above) invalidates this link.
	reads.resize(query_reads);
	for (size_t i = 0; i < linked.callees.size(); ++i) {
		const auto preceding = std::span(linked.callees).first(i);
		if (std::ranges::find(preceding, linked.callees[i]) == preceding.end())
			reads.emplace_back(linked.callees[i], linked.functions[i].size * sizeof(uint32_t));
	}
	linked.tables.clear();
	for (const auto& site: source.calls) linked.tables.push_back(site.table_targets);
	linked.decoded.code = linked.code;
	if (!linked.functions.empty())
		linked.observed_function.resize(std::ranges::max_element(linked.functions, {},
		    &ShaderSource::Linked::Function::size)->size);

	source.linked = std::move(linked);
	++source.revision;
	return source.linked->decoded;
}

} // namespace Libs::Graphics::ShaderRecompiler
