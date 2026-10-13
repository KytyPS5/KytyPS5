#include "graphics/shader/recompiler/ShaderRecompiler.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/shader/recompiler/Tessellation.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvDriverSimplify.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/frontend/translate/Translate.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/DeadLaneMasks.h"
#include "graphics/shader/recompiler/ir/passes/IndexRangeFold.h"
#include "graphics/shader/recompiler/ir/passes/ReadLaneElimination.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"
#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"
#include "graphics/shader/recompiler/ir/passes/SsaRewrite.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler {

namespace {

const char* GetDumpLabel(const CompileOptions& options) {
	return options.dump_label != nullptr ? options.dump_label : "ShaderRecompiler";
}

// What the CFG step decided, so a cache hit reproduces the same control flow - and the same log -
// as the build it stands in for.
enum class CfgOutcome { BuildRejected, Irreducible, StructurizeFailed, Structured };

const char* CfgOutcomeName(CfgOutcome outcome) {
	switch (outcome) {
		case CfgOutcome::BuildRejected: return "build-rejected";
		case CfgOutcome::Irreducible: return "irreducible";
		case CfgOutcome::StructurizeFailed: return "structurize-failed";
		case CfgOutcome::Structured: return "structured";
	}
	return "unknown";
}

// The heap one graph holds. An estimate, and only ever used to bound the cache, but it counts the
// dominator and post-dominator sets explicitly because those are the term that can run away: they
// are O(blocks) per block, so they are quadratic in a graph that nothing else about a shader makes
// large.
size_t GraphFootprint(const CFG::Graph& graph) {
	const auto words = [](const std::vector<uint32_t>& value) {
		return value.capacity() * sizeof(uint32_t);
	};
	size_t bytes = sizeof(CFG::Graph) + graph.unsupported_reason.capacity();
	bytes += graph.blocks.capacity() * sizeof(CFG::BasicBlock);
	bytes += graph.back_edges.capacity() * sizeof(CFG::BackEdge);
	bytes += graph.natural_loops.capacity() * sizeof(CFG::NaturalLoop);
	bytes += graph.components.capacity() * sizeof(CFG::StronglyConnectedComponent);
	bytes += graph.expressions.capacity() * sizeof(CFG::ConditionExpression);
	bytes += words(graph.code_table_load_pcs);
	for (const auto& block: graph.blocks) {
		bytes += words(block.predecessors) + words(block.successors) + words(block.dominators);
		bytes += block.assignments.capacity() * sizeof(CFG::BasicBlock::Assignment);
		bytes += words(block.terminator.indirect_target_pcs) +
		         words(block.terminator.indirect_targets) +
		         words(block.terminator.indirect_selector_values) +
		         words(block.terminator.indirect_selector_targets);
	}
	for (const auto& loop: graph.natural_loops) {
		bytes += words(loop.body_blocks);
	}
	for (const auto& component: graph.components) {
		bytes += words(component.blocks) + words(component.entry_blocks);
	}
	return bytes;
}

uint64_t HashCodeWords(std::span<const uint32_t> front, std::span<const uint32_t> back) {
	// FNV-1a over both bodies, with the front length folded in so that moving a word from one span
	// to the other cannot land on the same bucket. Only a bucket selector: every hit is confirmed
	// by comparing the words themselves, so a collision costs a comparison, never a wrong graph.
	uint64_t   hash = 0xcbf29ce484222325ull;
	const auto mix  = [&hash](uint64_t value) {
		for (uint32_t byte = 0; byte < sizeof(value); byte++) {
			hash ^= (value >> (byte * 8u)) & 0xffu;
			hash *= 0x100000001b3ull;
		}
	};
	mix(front.size());
	for (const auto word: front) {
		mix(word);
	}
	for (const auto word: back) {
		mix(word);
	}
	return hash;
}

// CFG::Graph is a value type all the way down, so every vector serializes as a 32-bit count
// followed by its elements, in little-endian host order.

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
	out.push_back(static_cast<uint8_t>(value));
	out.push_back(static_cast<uint8_t>(value >> 8u));
	out.push_back(static_cast<uint8_t>(value >> 16u));
	out.push_back(static_cast<uint8_t>(value >> 24u));
}

void PutU64(std::vector<uint8_t>& out, uint64_t value) {
	PutU32(out, static_cast<uint32_t>(value));
	PutU32(out, static_cast<uint32_t>(value >> 32u));
}

void PutWords(std::vector<uint8_t>& out, const std::vector<uint32_t>& words) {
	PutU32(out, static_cast<uint32_t>(words.size()));
	for (const auto word: words) {
		PutU32(out, word);
	}
}

struct Reader {
	const uint8_t* data = nullptr;
	size_t         size = 0;
	size_t         at   = 0;
	bool           ok   = true;

	uint32_t U32() {
		if (!ok || at + 4u > size) {
			ok = false;
			return 0;
		}
		const uint32_t value = static_cast<uint32_t>(data[at]) |
		                       (static_cast<uint32_t>(data[at + 1]) << 8u) |
		                       (static_cast<uint32_t>(data[at + 2]) << 16u) |
		                       (static_cast<uint32_t>(data[at + 3]) << 24u);
		at += 4u;
		return value;
	}
	uint64_t U64() {
		const auto low  = U32();
		const auto high = U32();
		return static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32u);
	}
	bool Count(uint32_t& out, size_t element_bytes) {
		out = U32();
		if (!ok || static_cast<size_t>(out) * element_bytes > size - at) {
			ok = false;
			return false;
		}
		return true;
	}
	std::vector<uint32_t> Words() {
		std::vector<uint32_t> words;
		uint32_t              count = 0;
		if (!Count(count, 4u)) {
			return words;
		}
		words.resize(count);
		for (uint32_t i = 0; i < count; i++) {
			words[i] = U32();
		}
		return words;
	}
};

void PutTerminator(std::vector<uint8_t>& out, const CFG::Terminator& value) {
	PutU32(out, static_cast<uint32_t>(value.kind));
	PutU32(out, static_cast<uint32_t>(value.condition));
	PutU32(out, value.true_block);
	PutU32(out, value.false_block);
	PutU32(out, value.merge_block);
	PutU32(out, value.continue_block);
	PutU32(out, value.indirect_pc_sgpr);
	PutU32(out, value.indirect_selector_code);
	PutWords(out, value.indirect_target_pcs);
	PutWords(out, value.indirect_targets);
	PutWords(out, value.indirect_selector_values);
	PutWords(out, value.indirect_selector_targets);
	PutU32(out, value.expression);
	PutU32(out, value.loop_header ? 1u : 0u);
}

void GetTerminator(Reader& in, CFG::Terminator& value) {
	value.kind                      = static_cast<CFG::TerminatorKind>(in.U32());
	value.condition                 = static_cast<CFG::BranchCondition>(in.U32());
	value.true_block                = in.U32();
	value.false_block               = in.U32();
	value.merge_block               = in.U32();
	value.continue_block            = in.U32();
	value.indirect_pc_sgpr          = in.U32();
	value.indirect_selector_code    = in.U32();
	value.indirect_target_pcs       = in.Words();
	value.indirect_targets          = in.Words();
	value.indirect_selector_values  = in.Words();
	value.indirect_selector_targets = in.Words();
	value.expression                = in.U32();
	value.loop_header               = in.U32() != 0u;
}

void SerializeGraph(std::vector<uint8_t>& out, const CFG::Graph& graph) {
	PutU32(out, static_cast<uint32_t>(graph.blocks.size()));
	for (const auto& block: graph.blocks) {
		PutU32(out, block.id);
		PutU32(out, block.start_pc);
		PutU32(out, block.end_pc);
		PutU32(out, block.inst_begin);
		PutU32(out, block.inst_end);
		PutU32(out, static_cast<uint32_t>(block.assignments.size()));
		for (const auto& assignment: block.assignments) {
			PutU32(out, assignment.variable);
			PutU32(out, assignment.expression);
		}
		PutWords(out, block.predecessors);
		PutWords(out, block.successors);
		PutWords(out, block.dominators);
		PutTerminator(out, block.terminator);
	}
	PutU32(out, static_cast<uint32_t>(graph.expressions.size()));
	for (const auto& expression: graph.expressions) {
		PutU32(out, static_cast<uint32_t>(expression.op));
		PutU32(out, expression.lhs);
		PutU32(out, expression.rhs);
	}
	PutU32(out, static_cast<uint32_t>(graph.back_edges.size()));
	for (const auto& edge: graph.back_edges) {
		PutU32(out, edge.from);
		PutU32(out, edge.to);
		PutU32(out, edge.natural ? 1u : 0u);
	}
	PutU32(out, static_cast<uint32_t>(graph.natural_loops.size()));
	for (const auto& loop: graph.natural_loops) {
		PutU32(out, loop.header);
		PutU32(out, loop.latch);
		PutWords(out, loop.body_blocks);
	}
	PutU32(out, static_cast<uint32_t>(graph.components.size()));
	for (const auto& component: graph.components) {
		PutWords(out, component.blocks);
		PutWords(out, component.entry_blocks);
		PutU32(out, component.irreducible ? 1u : 0u);
	}
	PutWords(out, graph.code_table_load_pcs);
	PutU32(out, graph.entry_block);
	PutU32(out, graph.irreducible ? 1u : 0u);
	PutU32(out, graph.unsupported ? 1u : 0u);
	PutU32(out, static_cast<uint32_t>(graph.failure_kind));
	PutU32(out, graph.failure_block);
	PutU32(out, graph.failure_pc);
	PutU32(out, static_cast<uint32_t>(graph.unsupported_reason.size()));
	out.insert(out.end(), graph.unsupported_reason.begin(), graph.unsupported_reason.end());
}

bool DeserializeGraph(Reader& in, CFG::Graph& graph) {
	uint32_t count = 0;
	if (!in.Count(count, 32u)) {
		return false;
	}
	graph.blocks.resize(count);
	for (auto& block: graph.blocks) {
		block.id             = in.U32();
		block.start_pc       = in.U32();
		block.end_pc         = in.U32();
		block.inst_begin     = in.U32();
		block.inst_end       = in.U32();
		uint32_t assignments = 0;
		if (!in.Count(assignments, 8u)) {
			return false;
		}
		block.assignments.resize(assignments);
		for (auto& assignment: block.assignments) {
			assignment.variable   = in.U32();
			assignment.expression = in.U32();
		}
		block.predecessors = in.Words();
		block.successors   = in.Words();
		block.dominators   = in.Words();
		GetTerminator(in, block.terminator);
		if (!in.ok) {
			return false;
		}
	}
	if (!in.Count(count, 12u)) {
		return false;
	}
	graph.expressions.resize(count);
	for (auto& expression: graph.expressions) {
		expression.op  = static_cast<CFG::ConditionExpression::Op>(in.U32());
		expression.lhs = in.U32();
		expression.rhs = in.U32();
	}
	if (!in.Count(count, 12u)) {
		return false;
	}
	graph.back_edges.resize(count);
	for (auto& edge: graph.back_edges) {
		edge.from    = in.U32();
		edge.to      = in.U32();
		edge.natural = in.U32() != 0u;
	}
	if (!in.Count(count, 12u)) {
		return false;
	}
	graph.natural_loops.resize(count);
	for (auto& loop: graph.natural_loops) {
		loop.header      = in.U32();
		loop.latch       = in.U32();
		loop.body_blocks = in.Words();
		if (!in.ok) {
			return false;
		}
	}
	if (!in.Count(count, 12u)) {
		return false;
	}
	graph.components.resize(count);
	for (auto& component: graph.components) {
		component.blocks       = in.Words();
		component.entry_blocks = in.Words();
		component.irreducible  = in.U32() != 0u;
		if (!in.ok) {
			return false;
		}
	}
	graph.code_table_load_pcs = in.Words();
	graph.entry_block         = in.U32();
	graph.irreducible         = in.U32() != 0u;
	graph.unsupported         = in.U32() != 0u;
	graph.failure_kind        = static_cast<CFG::FailureKind>(in.U32());
	graph.failure_block       = in.U32();
	graph.failure_pc          = in.U32();
	uint32_t reason_size      = 0;
	if (!in.Count(reason_size, 1u)) {
		return false;
	}
	graph.unsupported_reason.assign(reinterpret_cast<const char*>(in.data + in.at), reason_size);
	in.at += reason_size;
	return in.ok;
}

uint64_t HashBytesFnv(const uint8_t* data, size_t size) {
	uint64_t hash = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < size; i++) {
		hash ^= data[i];
		hash *= 0x100000001b3ull;
	}
	return hash;
}

// The structurized control-flow graph of a guest body, keyed on the body itself.
//
// Structurization reads nothing but the graph, and BuildGraph reads nothing but the decoded
// program, which is a pure function of the code words. Nothing else reaches either one: not the
// resource specialization, not the pipeline's static state, not the stage, the wave size or the
// user data, and not the proven-unfoldable set, which is attached to the IR program after
// translation and read only by resource tracking. So the second translation of a body - and a
// permutation miss forces one on every draw that takes it - structurizes an identical graph from
// identical inputs. In `story8` that was 146 of 825 structurizations and 69.3 s of its 104.9 s.
//
// The key is the code words themselves, compared in full on every hit. That is not caution about
// the hash so much as about what the graph means: a block names its instructions as a half-open
// range into the decoded program, so a graph is only meaningful beside a decode of exactly the
// words it was built from.
//
// CACHE KEY INVARIANT - read this before giving BuildGraph or Structurize a new argument.
// The key is the code words and nothing else, because as of this writing nothing else reaches
// either pass: no configuration, no environment, no static mutable state, and none of the stage,
// wave size, user data, resource specialization or proven-unfoldable set. **If either pass ever
// gains an input, it must be added to this key in the same change.** Nothing here will fail if it
// is not: a stale graph structures one shader's control flow and runs another through it, which
// renders the wrong thing silently rather than raising anything. The test beside this
// (TestStructurizedCfgIsCachedByBody) asserts that the stage does not split the key - it cannot
// notice a newly added argument, and no test can.
class CfgCache {
public:
	// Bounded in bytes rather than entries, because a graph's size is not a function of anything
	// that bounds the number of shaders: the dominator sets are quadratic in the block count, so
	// one pathological body can outweigh hundreds of ordinary ones. Measured over story7/8/9, all
	// 680 graphs of the largest run fit in 19 MiB even at the worst-case dominator density, so this
	// budget never evicts on this title; a title that does not fit degrades to rebuilding what it
	// evicted, which is exactly today's behaviour, rather than to an unbounded map.
	static constexpr size_t Budget = size_t {64} << 20u;
	// And no single entry may take more than an eighth of it. A graph that large is worth one
	// rebuild; flushing every useful entry to hold it is not.
	static constexpr size_t EntryLimit = Budget / 8u;

	bool Get(std::span<const uint32_t> code, std::span<const uint32_t> back_code,
	         CFG::Graph& native, CFG::Graph& structured, CfgOutcome& outcome) {
		const auto                        hash = HashCodeWords(code, back_code);
		const std::lock_guard<std::mutex> lock(m_mutex);
		const auto                        range = m_index.equal_range(hash);
		for (auto it = range.first; it != range.second; ++it) {
			const auto& entry = *it->second;
			if (!std::ranges::equal(entry.code, code) ||
			    !std::ranges::equal(entry.back_code, back_code)) {
				continue;
			}
			native     = entry.native;
			structured = entry.structured;
			outcome    = entry.outcome;
			if (entry.from_file) {
				m_hits_from_file++;
				m_avoided_file_us += entry.cost_us;
			} else {
				m_avoided_memory_us += entry.cost_us;
			}
			// Most recently used to the front, so eviction takes the body the run has stopped
			// drawing rather than the one it draws every frame.
			m_entries.splice(m_entries.begin(), m_entries, it->second);
			m_hits++;
			return true;
		}
		m_misses++;
		return false;
	}

	void Put(std::span<const uint32_t> code, std::span<const uint32_t> back_code,
	         const CFG::Graph& native, const CFG::Graph& structured, CfgOutcome outcome,
	         uint64_t cost_us) {
		const auto bytes = GraphFootprint(native) + GraphFootprint(structured) +
		                   (code.size() + back_code.size()) * sizeof(uint32_t) + sizeof(Entry);
		if (bytes > EntryLimit) {
			m_refused++;
			return;
		}
		const auto                        hash = HashCodeWords(code, back_code);
		const std::lock_guard<std::mutex> lock(m_mutex);
		while (m_bytes + bytes > Budget && !m_entries.empty()) {
			Evict();
		}
		m_entries.push_front(Entry {.code       = {code.begin(), code.end()},
		                            .back_code  = {back_code.begin(), back_code.end()},
		                            .native     = native,
		                            .structured = structured,
		                            .outcome    = outcome,
		                            .hash       = hash,
		                            .bytes      = bytes,
		                            .cost_us    = cost_us,
		                            .from_file  = false});
		m_index.emplace(hash, m_entries.begin());
		m_bytes += bytes;
	}

	static constexpr uint64_t FileMagic  = 0x3343464759544b53ull; // "SKTYFC3"
	static constexpr size_t   FileBudget = Budget;

	void OpenFile(const std::string& path, uint64_t stamp) {
		const std::lock_guard<std::mutex> lock(m_mutex);
		m_path  = path;
		m_stamp = stamp;
		if (path.empty() || stamp == 0u) {
			m_path.clear();
			return;
		}
		LoadLocked();
	}

	void Flush() {
		const std::lock_guard<std::mutex> lock(m_mutex);
		if (m_path.empty()) {
			return;
		}
		StoreLocked();
	}

	// Drops everything held in memory and forgets the file. Tests only.
	void ResetForTest() {
		const std::lock_guard<std::mutex> lock(m_mutex);
		m_entries.clear();
		m_index.clear();
		m_bytes             = 0;
		m_loaded            = 0;
		m_stored            = 0;
		m_rejected          = 0;
		m_hits_from_file    = 0;
		m_avoided_file_us   = 0;
		m_avoided_memory_us = 0;
		m_path.clear();
		m_stamp = 0;
	}

	CfgCacheStats Stats() {
		const std::lock_guard<std::mutex> lock(m_mutex);
		return {.hits              = m_hits,
		        .misses            = m_misses,
		        .evicted           = m_evicted,
		        .refused           = m_refused,
		        .entries           = static_cast<uint64_t>(m_entries.size()),
		        .bytes             = static_cast<uint64_t>(m_bytes),
		        .loaded            = m_loaded,
		        .stored            = m_stored,
		        .rejected          = m_rejected,
		        .hits_from_file    = m_hits_from_file,
		        .avoided_file_us   = m_avoided_file_us,
		        .avoided_memory_us = m_avoided_memory_us};
	}

	void Report(const char* label) {
		uint64_t hits              = 0;
		uint64_t misses            = 0;
		uint64_t evicted           = 0;
		uint64_t refused           = 0;
		uint64_t hits_from_file    = 0;
		uint64_t avoided_file_ms   = 0;
		uint64_t avoided_memory_ms = 0;
		size_t   bytes             = 0;
		size_t   entries           = 0;
		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			const auto                        total = m_hits + m_misses;
			// First sighting and every doubling, the way every other counter here reports.
			if (total == 0u || (total & (total - 1u)) != 0u || total == m_reported) {
				return;
			}
			m_reported        = total;
			hits              = m_hits;
			misses            = m_misses;
			evicted           = m_evicted;
			refused           = m_refused;
			bytes             = m_bytes;
			entries           = m_entries.size();
			hits_from_file    = m_hits_from_file;
			avoided_file_ms   = m_avoided_file_us / 1000u;
			avoided_memory_ms = m_avoided_memory_us / 1000u;
		}
		LOGF("%s CFG cache: hits=%" PRIu64 " (file %" PRIu64 ") misses=%" PRIu64 " entries=%" PRIu64
		     " bytes=%" PRIu64 " evicted=%" PRIu64 " refused=%" PRIu64 " avoided_ms=%" PRIu64
		     " (file %" PRIu64 ")\n",
		     label, hits, hits_from_file, misses, static_cast<uint64_t>(entries),
		     static_cast<uint64_t>(bytes), evicted, refused, avoided_file_ms + avoided_memory_ms,
		     avoided_file_ms);
	}

private:
	struct Entry {
		std::vector<uint32_t> code;
		std::vector<uint32_t> back_code;
		CFG::Graph            native;
		CFG::Graph            structured;
		CfgOutcome            outcome   = CfgOutcome::Structured;
		uint64_t              hash      = 0;
		size_t                bytes     = 0;
		uint64_t              cost_us   = 0;
		bool                  from_file = false;
	};

	// Called with the lock held.
	bool Holds(const Entry& entry) const {
		const auto range = m_index.equal_range(entry.hash);
		for (auto it = range.first; it != range.second; ++it) {
			if (std::ranges::equal(it->second->code, entry.code) &&
			    std::ranges::equal(it->second->back_code, entry.back_code)) {
				return true;
			}
		}
		return false;
	}

	// Called with the lock held.
	void LoadLocked() {
		std::ifstream file(m_path, std::ios::binary);
		if (!file) {
			return;
		}
		std::vector<uint8_t> blob((std::istreambuf_iterator<char>(file)),
		                          std::istreambuf_iterator<char>());
		file.close();
		Reader in {.data = blob.data(), .size = blob.size()};
		if (in.U64() != FileMagic || !in.ok) {
			return;
		}
		if (in.U64() != m_stamp || !in.ok) {
			return;
		}
		while (in.ok && in.at < in.size) {
			const auto payload_size = in.U32();
			const auto checksum     = in.U64();
			if (!in.ok || payload_size == 0u || payload_size > in.size - in.at) {
				m_rejected++;
				break;
			}
			const auto* payload = in.data + in.at;
			if (HashBytesFnv(payload, payload_size) != checksum) {
				m_rejected++;
				break;
			}
			Reader body {.data = payload, .size = payload_size};
			Entry  entry;
			entry.code      = body.Words();
			entry.back_code = body.Words();
			entry.outcome   = static_cast<CfgOutcome>(body.U32());
			entry.cost_us   = body.U64();
			if (!body.ok || !DeserializeGraph(body, entry.native) ||
			    !DeserializeGraph(body, entry.structured) || entry.code.empty()) {
				m_rejected++;
				break;
			}
			in.at += payload_size;
			entry.hash  = HashCodeWords(entry.code, entry.back_code);
			entry.bytes = GraphFootprint(entry.native) + GraphFootprint(entry.structured) +
			              (entry.code.size() + entry.back_code.size()) * sizeof(uint32_t) +
			              sizeof(Entry);
			if (entry.bytes > EntryLimit || m_bytes + entry.bytes > Budget || Holds(entry)) {
				continue;
			}
			entry.from_file = true;
			m_bytes += entry.bytes;
			m_entries.push_back(std::move(entry));
			m_index.emplace(m_entries.back().hash, std::prev(m_entries.end()));
			m_loaded++;
		}
	}

	// Called with the lock held.
	void StoreLocked() {
		std::vector<uint8_t> blob;
		PutU64(blob, FileMagic);
		PutU64(blob, m_stamp);
		uint64_t stored = 0;
		for (const auto& entry: m_entries) {
			std::vector<uint8_t> payload;
			PutWords(payload, entry.code);
			PutWords(payload, entry.back_code);
			PutU32(payload, static_cast<uint32_t>(entry.outcome));
			PutU64(payload, entry.cost_us);
			SerializeGraph(payload, entry.native);
			SerializeGraph(payload, entry.structured);
			if (blob.size() + payload.size() + 12u > FileBudget) {
				break;
			}
			PutU32(blob, static_cast<uint32_t>(payload.size()));
			PutU64(blob, HashBytesFnv(payload.data(), payload.size()));
			blob.insert(blob.end(), payload.begin(), payload.end());
			stored++;
		}
		// Written beside the target and renamed over it, so the live file is only ever replaced
		// whole.
		std::error_code error;
		const auto      target = std::filesystem::path(m_path);
		if (target.has_parent_path()) {
			std::filesystem::create_directories(target.parent_path(), error);
		}
		const auto temporary = std::filesystem::path(m_path + ".tmp");
		{
			std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
			if (!file) {
				return;
			}
			file.write(reinterpret_cast<const char*>(blob.data()),
			           static_cast<std::streamsize>(blob.size()));
			file.flush();
			if (!file) {
				file.close();
				std::filesystem::remove(temporary, error);
				return;
			}
		}
		std::filesystem::rename(temporary, target, error);
		if (error) {
			std::filesystem::remove(temporary, error);
			return;
		}
		m_stored = stored;
	}

	// Called with the lock held.
	void Evict() {
		const auto& victim = m_entries.back();
		const auto  range  = m_index.equal_range(victim.hash);
		for (auto it = range.first; it != range.second; ++it) {
			if (it->second == std::prev(m_entries.end())) {
				m_index.erase(it);
				break;
			}
		}
		m_bytes -= victim.bytes;
		m_entries.pop_back();
		m_evicted++;
	}

	std::mutex                                                    m_mutex;
	std::list<Entry>                                              m_entries;
	std::unordered_multimap<uint64_t, std::list<Entry>::iterator> m_index;
	size_t                                                        m_bytes    = 0;
	uint64_t                                                      m_hits     = 0;
	uint64_t                                                      m_misses   = 0;
	uint64_t                                                      m_evicted  = 0;
	uint64_t                                                      m_refused  = 0;
	uint64_t                                                      m_reported = 0;
	std::string                                                   m_path;
	uint64_t                                                      m_stamp             = 0;
	uint64_t                                                      m_loaded            = 0;
	uint64_t                                                      m_stored            = 0;
	uint64_t                                                      m_rejected          = 0;
	uint64_t                                                      m_hits_from_file    = 0;
	uint64_t                                                      m_avoided_file_us   = 0;
	uint64_t                                                      m_avoided_memory_us = 0;
};

CfgCache& StructurizedCfgCache() {
	static CfgCache cache;
	return cache;
}

std::string MakeIrDump(std::string_view cfg, const IR::Program& ir) {
	std::string dump = "CFG:\n";
	dump += cfg;
	dump += "\nIR:\n";
	dump += fmt::format("mode={} scratch_dwords={}\n",
	                    ir.dispatcher_fallback ? "dispatcher" : "structured", ir.scratch_dwords);
	dump += IR::ProgramToString(ir);
	return dump;
}

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Compute: return "CS";
		case ShaderType::Vertex: return "VS";
		case ShaderType::Local: return "LS";
		case ShaderType::TessellationControl: return "HS";
		case ShaderType::TessellationEvaluation: return "TES";
		case ShaderType::Mesh: return "MS";
		case ShaderType::Pixel: return "PS";
		default: return "unknown";
	}
}

void LogDispatcherFallback(const CompileOptions& options, const CFG::Graph& cfg,
                           const char* phase) {
	const auto* block        = cfg.FindBlock(cfg.failure_block);
	const auto  start        = block != nullptr ? block->start_pc : UINT32_MAX;
	const auto  end          = block != nullptr ? block->end_pc : UINT32_MAX;
	const auto  predecessors = block != nullptr ? block->predecessors.size() : 0u;
	const auto  successors   = block != nullptr ? block->successors.size() : 0u;
	LOGF("%s CFG dispatcher fallback: stage=%s hash=0x%016" PRIx64
	     " phase=%s failure=%s block=%" PRIu32 " pc=0x%08" PRIx32 "..0x%08" PRIx32 " preds=%" PRIu64
	     " succs=%" PRIu64 " blocks=%" PRIu64 " loops=%" PRIu64 " back_edges=%" PRIu64
	     " reason=%s\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash, phase,
	     CFG::FailureKindToString(cfg.failure_kind).c_str(), cfg.failure_block, start, end,
	     static_cast<uint64_t>(predecessors), static_cast<uint64_t>(successors),
	     static_cast<uint64_t>(cfg.blocks.size()), static_cast<uint64_t>(cfg.natural_loops.size()),
	     static_cast<uint64_t>(cfg.back_edges.size()), cfg.unsupported_reason.c_str());
}

enum class EmbeddedFetchValueType { Unknown, Constant, AttribTable, Attrib, BufferTable, Buffer };

struct EmbeddedFetchSgprInfo {
	EmbeddedFetchValueType type      = EmbeddedFetchValueType::Unknown;
	int                    attrib_id = 0;
	uint32_t               value     = 0;
};

using EmbeddedFetchVectorLanes = std::map<uint64_t, EmbeddedFetchSgprInfo>;

uint64_t EmbeddedFetchVectorLaneKey(uint32_t reg, uint32_t lane) {
	return (static_cast<uint64_t>(reg) << 32u) | lane;
}

uint32_t EmbeddedFetchLane(uint32_t lane, uint32_t wave_size) {
	return wave_size == 32 || wave_size == 64 ? lane % wave_size : lane;
}

void ClearEmbeddedFetchVectorLanes(EmbeddedFetchVectorLanes* lanes, uint32_t reg) {
	const auto first = lanes->lower_bound(EmbeddedFetchVectorLaneKey(reg, 0));
	const auto last  = lanes->lower_bound(EmbeddedFetchVectorLaneKey(reg + 1u, 0));
	lanes->erase(first, last);
}

bool IsDecodedSgpr(const Decoder::Operand& op) {
	return op.kind == Decoder::OperandKind::Sgpr || op.kind == Decoder::OperandKind::VccLo ||
	       op.kind == Decoder::OperandKind::VccHi;
}

uint32_t DecodedSgprReg(const Decoder::Operand& op) {
	switch (op.kind) {
		case Decoder::OperandKind::VccLo: return 106u;
		case Decoder::OperandKind::VccHi: return 107u;
		default: return op.reg;
	}
}

bool IsDecodedVgpr(const Decoder::Operand& op) {
	return op.kind == Decoder::OperandKind::Vgpr;
}

uint32_t DecodedDstSize(const Decoder::Instruction& inst) {
	return std::max(inst.data_dwords, 1u);
}

uint32_t EmbeddedFetchDstSize(const Decoder::Instruction& inst) {
	return inst.opcode == Decoder::Opcode::V_MAD_U64_U32 ? 2u : DecodedDstSize(inst);
}

void ClearEmbeddedFetchSgprs(std::array<EmbeddedFetchSgprInfo, 108>& sgprs,
                             const Decoder::Operand& dst, uint32_t size) {
	if (!IsDecodedSgpr(dst)) {
		return;
	}
	const auto register_id = DecodedSgprReg(dst);
	for (uint32_t i = 0; i < size && register_id + i < sgprs.size(); i++) {
		sgprs[register_id + i] = {};
	}
}

bool TryDecodedOperandConstant(const std::array<EmbeddedFetchSgprInfo, 108>& sgprs,
                               const Decoder::Operand& op, uint32_t& value) {
	switch (op.kind) {
		case Decoder::OperandKind::LiteralConstant:
		case Decoder::OperandKind::IntegerInlineConstant:
		case Decoder::OperandKind::FloatInlineConstant: value = op.value; return true;
		case Decoder::OperandKind::Null: value = 0; return true;
		default: break;
	}
	if (IsDecodedSgpr(op) && DecodedSgprReg(op) < sgprs.size() &&
	    sgprs[DecodedSgprReg(op)].type == EmbeddedFetchValueType::Constant) {
		value = sgprs[DecodedSgprReg(op)].value;
		return true;
	}
	return false;
}

bool TryDecodedSmemOffset(const std::array<EmbeddedFetchSgprInfo, 108>& sgprs,
                          const Decoder::Instruction& inst, uint32_t& raw_offset) {
	uint32_t base = 0;
	if (!TryDecodedOperandConstant(sgprs, inst.src1, base)) {
		return false;
	}
	const auto value = static_cast<uint64_t>(base) + inst.offset;
	if (value > 0xffffffffull) {
		return false;
	}
	raw_offset = static_cast<uint32_t>(value);
	return true;
}

bool IsEmbeddedFetchSLoad(const Decoder::Instruction& inst) {
	switch (inst.opcode) {
		case Decoder::Opcode::S_LOAD_DWORD:
		case Decoder::Opcode::S_LOAD_DWORDX2:
		case Decoder::Opcode::S_LOAD_DWORDX4:
		case Decoder::Opcode::S_LOAD_DWORDX8:
		case Decoder::Opcode::S_LOAD_DWORDX16: return true;
		default: return false;
	}
}

bool IsEmbeddedFetchBufferLoad(const Decoder::Instruction& inst) {
	switch (inst.opcode) {
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_X:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XY:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XYZ:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XYZW: return true;
		default: return false;
	}
}

bool IsEmbeddedFetchAttribPropagationAlu(const Decoder::Instruction& inst) {
	switch (inst.opcode) {
		case Decoder::Opcode::S_BFE_U32:
		case Decoder::Opcode::S_AND_B32:
		case Decoder::Opcode::S_ADD_I32:
		case Decoder::Opcode::S_ADD_U32:
		case Decoder::Opcode::S_LSHL_B32: return true;
		default: return false;
	}
}

int BufferTableAttribFromOffset(uint32_t raw_offset, int dword) {
	return static_cast<int>((raw_offset + static_cast<uint32_t>(dword) * 4u) / 16u);
}

Frontend::EmbeddedFetchPlan
DetectEmbeddedVertexFetch(const Decoder::Program& decoded, const ShaderVertexInputInfo* input_info,
                          uint32_t user_data_base, uint32_t user_data_count, uint32_t wave_size) {
	const uint32_t vertex_index_reg   = input_info->logical_stage == ShaderType::Local ? 2u : 5u;
	const uint32_t instance_index_reg = input_info->logical_stage == ShaderType::Local ? 5u : 8u;
	Frontend::EmbeddedFetchPlan data;
	data.loads.reserve(input_info->resources_num);
	int32_t vertex_offset_candidate   = -1;
	int32_t instance_offset_candidate = -1;
	bool    vertex_offset_conflict    = false;
	bool    instance_offset_conflict  = false;

	const int shift_regs = 8;
	const int attrib_reg = input_info->fetch_attrib_reg + shift_regs;
	const int buffer_reg = input_info->fetch_buffer_reg + shift_regs;

	std::array<EmbeddedFetchSgprInfo, 108> sgprs {};
	std::array<bool, 256>                  vgpr_is_index {};
	EmbeddedFetchVectorLanes               vector_lanes;
	const bool                             track_vector_lanes = std::none_of(
	    decoded.instructions.begin(), decoded.instructions.end(), [](const auto& inst) {
		    return Decoder::IsDirectBranch(inst.opcode) ||
		           inst.opcode == Decoder::Opcode::S_SETPC_B64;
	    });

	if (attrib_reg >= 0 && attrib_reg < static_cast<int>(sgprs.size())) {
		sgprs[attrib_reg].type = EmbeddedFetchValueType::AttribTable;
	}
	if (attrib_reg + 1 >= 0 && attrib_reg + 1 < static_cast<int>(sgprs.size())) {
		sgprs[attrib_reg + 1].type = EmbeddedFetchValueType::AttribTable;
	}
	if (buffer_reg >= 0 && buffer_reg < static_cast<int>(sgprs.size())) {
		sgprs[buffer_reg].type = EmbeddedFetchValueType::BufferTable;
	}
	if (buffer_reg + 1 >= 0 && buffer_reg + 1 < static_cast<int>(sgprs.size())) {
		sgprs[buffer_reg + 1].type = EmbeddedFetchValueType::BufferTable;
	}

	for (const auto& inst: decoded.instructions) {
		// Fetch shaders accumulate the draw's vertex offset in v0. The PS5 NGG ABI
		// seeds S_NGG_VERTEX_INDEX in v5 and S_NGG_INSTANCE_INDEX in v8, then applies the
		// corresponding direct-draw offsets before fetching.
		const bool vertex_index_accumulator =
		    IsDecodedVgpr(inst.dst) &&
		    (inst.dst.reg == 0 || (user_data_base == 8 && inst.dst.reg == vertex_index_reg));
		const bool instance_index_accumulator =
		    IsDecodedVgpr(inst.dst) &&
		    (inst.dst.reg == (user_data_base == 8 ? instance_index_reg : 3u));
		uint32_t   sad_zero = 0;
		const bool index_offset_add =
		    (vertex_index_accumulator || instance_index_accumulator) && IsDecodedSgpr(inst.src0) &&
		    ((inst.opcode == Decoder::Opcode::V_ADD_I32 && IsDecodedVgpr(inst.src1) &&
		      inst.src1.reg == inst.dst.reg) ||
		     (user_data_base == 8 &&
		      (inst.dst.reg == vertex_index_reg || inst.dst.reg == instance_index_reg) &&
		      inst.opcode == Decoder::Opcode::V_SAD_U32 && IsDecodedVgpr(inst.src2) &&
		      inst.src2.reg == inst.dst.reg &&
		      TryDecodedOperandConstant(sgprs, inst.src1, sad_zero) && sad_zero == 0));
		if (data.loads.empty() && index_offset_add) {
			const auto reg = DecodedSgprReg(inst.src0);
			if (reg >= user_data_base && reg - user_data_base < user_data_count) {
				auto& candidate =
				    vertex_index_accumulator ? vertex_offset_candidate : instance_offset_candidate;
				auto& conflict =
				    vertex_index_accumulator ? vertex_offset_conflict : instance_offset_conflict;
				if (candidate >= 0 && candidate != static_cast<int32_t>(reg)) {
					conflict = true;
				} else {
					candidate = static_cast<int32_t>(reg);
				}
			}
		}
		switch (inst.opcode) {
			case Decoder::Opcode::V_WRITELANE_B32: {
				uint32_t lane = 0;
				if (IsDecodedVgpr(inst.dst) && inst.dst.reg < vgpr_is_index.size()) {
					vgpr_is_index[inst.dst.reg] = false;
				}
				if (track_vector_lanes && IsDecodedVgpr(inst.dst) && IsDecodedSgpr(inst.src0) &&
				    DecodedSgprReg(inst.src0) < sgprs.size() &&
				    TryDecodedOperandConstant(sgprs, inst.src1, lane)) {
					vector_lanes[EmbeddedFetchVectorLaneKey(inst.dst.reg,
					                                        EmbeddedFetchLane(lane, wave_size))] =
					    sgprs[DecodedSgprReg(inst.src0)];
				} else if (IsDecodedVgpr(inst.dst)) {
					ClearEmbeddedFetchVectorLanes(&vector_lanes, inst.dst.reg);
				}
				break;
			}
			case Decoder::Opcode::V_READLANE_B32: {
				uint32_t lane = 0;
				if (track_vector_lanes && IsDecodedSgpr(inst.dst) &&
				    DecodedSgprReg(inst.dst) < sgprs.size() && IsDecodedVgpr(inst.src0) &&
				    TryDecodedOperandConstant(sgprs, inst.src1, lane)) {
					const auto found = vector_lanes.find(EmbeddedFetchVectorLaneKey(
					    inst.src0.reg, EmbeddedFetchLane(lane, wave_size)));
					sgprs[DecodedSgprReg(inst.dst)] =
					    found != vector_lanes.end() ? found->second : EmbeddedFetchSgprInfo {};
				} else if (IsDecodedSgpr(inst.dst)) {
					ClearEmbeddedFetchSgprs(sgprs, inst.dst, 1);
				}
				break;
			}
			case Decoder::Opcode::S_MOV_B32:
				if (IsDecodedSgpr(inst.dst) && IsDecodedSgpr(inst.src0) &&
				    DecodedSgprReg(inst.src0) < sgprs.size()) {
					sgprs[DecodedSgprReg(inst.dst)] = sgprs[DecodedSgprReg(inst.src0)];
				} else if (IsDecodedSgpr(inst.dst)) {
					uint32_t value = 0;
					if (TryDecodedOperandConstant(sgprs, inst.src0, value)) {
						auto& dst = sgprs[DecodedSgprReg(inst.dst)];
						dst.type  = EmbeddedFetchValueType::Constant;
						dst.value = value;
					} else {
						ClearEmbeddedFetchSgprs(sgprs, inst.dst, 1);
					}
				}
				break;
			case Decoder::Opcode::S_MOVK_I32:
				if (IsDecodedSgpr(inst.dst)) {
					auto& dst = sgprs[DecodedSgprReg(inst.dst)];
					dst.type  = EmbeddedFetchValueType::Constant;
					dst.value = inst.src0.value;
				}
				break;
			default:
				if (IsEmbeddedFetchSLoad(inst)) {
					if (IsDecodedSgpr(inst.src0) && DecodedSgprReg(inst.src0) < sgprs.size() &&
					    sgprs[DecodedSgprReg(inst.src0)].type ==
					        EmbeddedFetchValueType::AttribTable) {
						uint32_t raw_offset = 0;
						if (TryDecodedSmemOffset(sgprs, inst, raw_offset)) {
							const auto register_id = DecodedSgprReg(inst.dst);
							const int  index       = static_cast<int>(raw_offset / 4u);
							for (uint32_t i = 0;
							     i < DecodedDstSize(inst) && register_id + i < sgprs.size(); i++) {
								auto& dst     = sgprs[register_id + i];
								dst.type      = EmbeddedFetchValueType::Attrib;
								dst.attrib_id = index + static_cast<int>(i);
							}
						} else {
							ClearEmbeddedFetchSgprs(sgprs, inst.dst, DecodedDstSize(inst));
						}
					} else if (IsDecodedSgpr(inst.src0) &&
					           DecodedSgprReg(inst.src0) < sgprs.size() &&
					           sgprs[DecodedSgprReg(inst.src0)].type ==
					               EmbeddedFetchValueType::BufferTable) {
						const auto register_id = DecodedSgprReg(inst.dst);
						uint32_t   raw_offset  = 0;
						if (TryDecodedSmemOffset(sgprs, inst, raw_offset)) {
							for (uint32_t i = 0;
							     i < DecodedDstSize(inst) && register_id + i < sgprs.size(); i++) {
								auto& dst = sgprs[register_id + i];
								dst.type  = EmbeddedFetchValueType::Buffer;
								dst.attrib_id =
								    BufferTableAttribFromOffset(raw_offset, static_cast<int>(i));
							}
						} else if (IsDecodedSgpr(inst.src1) &&
						           DecodedSgprReg(inst.src1) < sgprs.size() &&
						           sgprs[DecodedSgprReg(inst.src1)].type ==
						               EmbeddedFetchValueType::Attrib &&
						           (inst.offset & 0x3u) == 0) {
							for (uint32_t i = 0;
							     i < DecodedDstSize(inst) && register_id + i < sgprs.size(); i++) {
								auto& dst     = sgprs[register_id + i];
								dst.type      = EmbeddedFetchValueType::Buffer;
								dst.attrib_id = sgprs[DecodedSgprReg(inst.src1)].attrib_id;
							}
						} else {
							ClearEmbeddedFetchSgprs(sgprs, inst.dst, DecodedDstSize(inst));
						}
					} else {
						ClearEmbeddedFetchSgprs(sgprs, inst.dst, DecodedDstSize(inst));
					}
				} else if (inst.opcode == Decoder::Opcode::V_CNDMASK_B32) {
					if (IsDecodedVgpr(inst.dst) && inst.dst.reg < vgpr_is_index.size()) {
						ClearEmbeddedFetchVectorLanes(&vector_lanes, inst.dst.reg);
					}
					if (IsDecodedVgpr(inst.dst) && inst.dst.reg < vgpr_is_index.size() &&
					    IsDecodedVgpr(inst.src0) && inst.src0.reg == instance_index_reg &&
					    IsDecodedVgpr(inst.src1) && inst.src1.reg == vertex_index_reg) {
						vgpr_is_index[inst.dst.reg] = true;
					}
				} else if (IsEmbeddedFetchAttribPropagationAlu(inst)) {
					if (IsDecodedSgpr(inst.dst) && IsDecodedSgpr(inst.src0) &&
					    DecodedSgprReg(inst.src0) < sgprs.size() &&
					    sgprs[DecodedSgprReg(inst.src0)].type == EmbeddedFetchValueType::Attrib) {
						sgprs[DecodedSgprReg(inst.dst)] = sgprs[DecodedSgprReg(inst.src0)];
					} else if (IsDecodedSgpr(inst.dst)) {
						uint32_t src0 = 0;
						uint32_t src1 = 0;
						if (TryDecodedOperandConstant(sgprs, inst.src0, src0) &&
						    TryDecodedOperandConstant(sgprs, inst.src1, src1)) {
							auto& dst = sgprs[DecodedSgprReg(inst.dst)];
							dst.type  = EmbeddedFetchValueType::Constant;
							switch (inst.opcode) {
								case Decoder::Opcode::S_AND_B32: dst.value = src0 & src1; break;
								case Decoder::Opcode::S_LSHL_B32:
									dst.value = src0 << (src1 & 31u);
									break;
								case Decoder::Opcode::S_BFE_U32:
									dst.value = src0 >> (src1 & 31u);
									break;
								default: dst.value = src0 + src1; break;
							}
						} else {
							ClearEmbeddedFetchSgprs(sgprs, inst.dst, 1);
						}
					}
				} else if (IsEmbeddedFetchBufferLoad(inst)) {
					if (IsDecodedVgpr(inst.src0) && inst.src0.reg < vgpr_is_index.size() &&
					    vgpr_is_index[inst.src0.reg] && IsDecodedSgpr(inst.src1) &&
					    DecodedSgprReg(inst.src1) < sgprs.size() &&
					    sgprs[DecodedSgprReg(inst.src1)].type == EmbeddedFetchValueType::Buffer) {
						const auto& buffer = sgprs[DecodedSgprReg(inst.src1)];
						if (data.loads.empty()) {
							if (!vertex_offset_conflict) {
								data.vertex_offset_sgpr = vertex_offset_candidate;
							}
							if (!instance_offset_conflict) {
								data.instance_offset_sgpr = instance_offset_candidate;
							}
						}
						auto& load      = data.loads.emplace_back();
						load.pc         = inst.pc;
						load.attrib_id  = buffer.attrib_id;
						load.components = DecodedDstSize(inst);
					}
				}
				break;
		}
		if (inst.opcode == Decoder::Opcode::V_MOVRELD_B32) {
			vector_lanes.clear();
		} else if (inst.opcode != Decoder::Opcode::V_WRITELANE_B32 && IsDecodedVgpr(inst.dst)) {
			for (uint32_t i = 0;
			     i < EmbeddedFetchDstSize(inst) && inst.dst.reg + i < vgpr_is_index.size(); i++) {
				ClearEmbeddedFetchVectorLanes(&vector_lanes, inst.dst.reg + i);
			}
		}
	}

	return data;
}

Decoder::Program DecodeFusedProgram(std::span<const uint32_t> front, std::span<const uint32_t> back,
                                    std::vector<uint32_t>& joined_code) {
	EXIT_IF(back.empty());
	auto       result      = Decoder::DecodeFrontProgram(front);
	const auto front_words = static_cast<uint32_t>(result.code.size());
	joined_code.assign(result.code.begin(), result.code.end());
	joined_code.insert(joined_code.end(), back.begin(), back.end());
	// The merged-stage ABI passes the back shader in s[6:7]. Give that handoff an
	// ordinary CFG edge, retaining both bodies in one register and LDS lifetime.
	joined_code[front_words - 1u] = 0xbf820000u; // s_branch to the following instruction
	result.instructions.back()    = {};
	Decoder::DecodeInstruction(joined_code, front_words - 1u, result.instructions.back());
	Decoder::Program back_program;
	Decoder::DecodeProgram(back, back_program);
	result.has_swap_pc |= back_program.has_swap_pc;
	const auto back_pc = front_words * sizeof(uint32_t);
	for (auto& inst: back_program.instructions) {
		// A back-stage PC-relative data reference requires its guest code address.
		EXIT_NOT_IMPLEMENTED(inst.opcode == Decoder::Opcode::S_GETPC_B64);
		inst.pc += back_pc;
		inst.branch_target += back_pc;
		result.instructions.push_back(std::move(inst));
	}
	result.code = joined_code;
	return result;
}

// Links only a single compute S_SWAPPC_B64 whose target the host can re-execute. Anything else
// leaves `source.call` empty, and the translator keeps the any-hit stub for that call.
void PrepareCallTarget(ShaderSource& source, const CompileOptions& options) {
	using namespace IR;
	uint32_t calls = 0;
	for (uint32_t i = 0; i < source.decoded.instructions.size(); ++i) {
		if (source.decoded.instructions[i].opcode != Decoder::Opcode::S_SWAPPC_B64) continue;
		source.call = ShaderSource::Call {.instruction = i};
		calls++;
	}
	if (calls != 1u || options.stage != ShaderType::Compute ||
	    options.input_info.compute == nullptr) {
		source.call.reset();
		return;
	}
	const auto& call = source.decoded.instructions[source.call->instruction];
	if (call.src0.kind != Decoder::OperandKind::Sgpr ||
	    call.dst.kind != Decoder::OperandKind::Sgpr || call.src0.reg != call.dst.reg ||
	    call.dst.reg >= 105u) {
		source.call.reset();
		return;
	}
	bool       failed = false;
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
		const auto index = CFG::FindScalarDefinition(source.decoded, graph, before, code);
		if (index == UINT32_MAX) {
			if (code < query.user_data_base ||
			    code - query.user_data_base >= query.user_data_count) {
				failed = true;
				return Value(0u);
			}
			return IREmitter(&block).GetUserData(static_cast<ScalarReg>(code));
		}
		auto [entry, inserted] = producers.try_emplace(index);
		auto& producer = entry->second;
		if (inserted) {
			Block translated;
			Frontend::Translator translator(query, &translated, 1u,
			    (options.input_info.compute->float_mode & 0x10u) == 0u,
			    !options.input_info.compute->async_compute);
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
		if (!producer.ready || !producer.values.contains(code)) {
			failed = true;
			return Value(0u);
		}
		return producer.values.at(code).Resolve();
	};
	for (uint32_t i = 0; i < 2u; ++i) {
		const auto value = Register(source.call->instruction, call.src0.reg + i);
		if (failed || !ValidateRuntimeValue(query, value, RuntimeValueType::Integer)) {
			source.call.reset();
			return;
		}
		auto& root = query.value_storage.emplace_back(ValueOpcode::ReferenceU32);
		root.SetArg(0, value);
		source.call->target[i] = Value(&root);
	}
	RewriteToSsa(query.blocks);
	ConstantPropagationPass(query.blocks, query.wave_size);
	RemoveIdentities(query.blocks);
	EliminateDeadCode(query.blocks);
	for (auto& value: source.call->target) value = value.Instruction()->Arg(0).Resolve();
	query.value_storage.clear();
	// The retained reads keep their MemoryFlags, which is what the SRT walker reads them by.
	for (auto& inst: block) {
		inst.SetParent(nullptr);
	}
	query.value_storage.splice(query.value_storage.end(), block.Instructions());
	source.call_targets = std::move(static_cast<ResourcePlan&>(query));
}

} // namespace

ShaderSource PrepareShaderSource(std::span<const uint32_t> code, const CompileOptions& options) {
	EXIT_IF(code.empty());
	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " code_words=%" PRIu64 " decode\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(code.size()));
	const Decoder::DecodeContextScope decode_context(options.shader_hash, code.data());
	ShaderSource                      source;
	if (!options.back_code.empty()) {
		source.decoded = DecodeFusedProgram(code, options.back_code, source.code);
	} else if (options.stage == ShaderType::Local) {
		source.decoded = Decoder::DecodeFrontProgram(code);
		auto& handoff = source.decoded.instructions.back();
		handoff.opcode = Decoder::Opcode::S_ENDPGM;
		handoff.src_count = 0;
	} else {
		Decoder::DecodeProgram(code, source.decoded);
	}
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " decode instructions=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(source.decoded.instructions.size()));
	if (source.decoded.has_swap_pc) {
		PrepareCallTarget(source, options);
		if (source.call) {
			source.code.assign(code.begin(), code.end());
			source.decoded.code = source.code;
		}
	}
	return source;
}

const Decoder::Program& RefreshShaderSource(ShaderSource& source, const IR::SrtRuntime& runtime) {
	auto& reads = source.reads;
	reads.clear();
	// An unlinked call keeps the translator's any-hit stub.
	if (!source.call) return source.decoded;
	IR::SrtReadCapture capture(runtime, reads);
	const auto         clean = IR::CleanRuntime(capture.ObservedRuntime());
	IR::SrtWalker      walker(source.call_targets, clean);
	uint32_t           low = 0, high = 0;
	const auto&        call = source.decoded.instructions[source.call->instruction];
	if (!walker.Evaluate(source.call->target[0], low) ||
	    !walker.Evaluate(source.call->target[1], high)) {
		LOGF("shader call at pc 0x%08x has an unavailable scalar target; using the stub\n",
		     call.pc);
		return source.decoded;
	}
	// SWAPPC reads the old aliased pair and ignores the target's low two bits.
	const auto address = ((uint64_t {high} << 32u) | low) & ~uint64_t {3};
	if (address == 0u) return source.decoded;
	const auto query_reads = reads.size();
	if (source.linked) {
		auto& linked = *source.linked;
		const auto function = std::span(linked.code).subspan(source.code.size());
		if (clean.read_memory(clean.userdata, address, linked.observed_function) &&
		    std::ranges::equal(function, linked.observed_function)) {
			return linked.decoded;
		}
		reads.resize(query_reads);
	}
	ShaderSource::Linked linked {.code = source.code, .decoded = source.decoded};
	const auto&          last = source.decoded.instructions.back();
	// A call this linker cannot follow keeps the stub: only a scalar leaf that returns through
	// the call's own register pair is inlined.
	const auto unlinked = [&](const char* reason) -> const Decoder::Program& {
		LOGF("shader call at pc 0x%08x not linked (%s); using the stub\n", call.pc, reason);
		reads.resize(query_reads);
		return source.decoded;
	};
	if (last.opcode != Decoder::Opcode::S_ENDPGM) return unlinked("program does not end");
	// Keep the caller's data/footer bytes at their native offsets. The leaf's virtual CFG
	// PCs follow decoded instructions; its physical code is independently read and owned.
	const auto function_pc = last.pc + last.word_count * sizeof(uint32_t);
	for (uint32_t index = 0;;) {
		// The observed queue helper is a scalar leaf. Scalar encodings need at most one literal.
		if (index >= 64u || address > UINT64_MAX - (uint64_t {index} + 2u) * sizeof(uint32_t))
			return unlinked("leaf too long");
		std::array<uint32_t, 2> words {};
		if (!clean.read_memory(clean.userdata, address + uint64_t {index} * sizeof(uint32_t),
		                       std::span(words).first(1)))
			return unlinked("leaf code unreadable");
		const auto family = Decoder::GetInstructionFamily(words[0]);
		if (family != Decoder::Family::SOP1 && family != Decoder::Family::SOP2 &&
		    family != Decoder::Family::SOPC && family != Decoder::Family::SOPK &&
		    family != Decoder::Family::SOPP)
			return unlinked("leaf is not scalar");
		Decoder::Instruction inst;
		Decoder::DecodeInstruction(words, 0u, inst);
		if (inst.word_count == 2u) {
			if (!clean.read_memory(clean.userdata,
			                       address + uint64_t {index + 1u} * sizeof(uint32_t),
			                       std::span(words).last(1)))
				return unlinked("leaf literal unreadable");
			inst = {};
			Decoder::DecodeInstruction(words, 0u, inst);
		}
		inst.pc = index * sizeof(uint32_t);
		if (Decoder::IsDirectBranch(inst.opcode)) inst.branch_target += inst.pc;
		linked.code.insert(linked.code.end(), inst.raw, inst.raw + inst.word_count);
		index += inst.word_count;
		if (inst.opcode == Decoder::Opcode::S_SWAPPC_B64 ||
		    inst.opcode == Decoder::Opcode::S_GETPC_B64 || inst.opcode == Decoder::Opcode::S_ENDPGM)
			return unlinked("leaf calls, reads its PC or ends the program");
		if (CFG::MayWriteScalarRegister(inst, call.dst.reg) ||
		    CFG::MayWriteScalarRegister(inst, call.dst.reg + 1u))
			return unlinked("leaf overwrites its return address");
		const bool returns = inst.opcode == Decoder::Opcode::S_SETPC_B64;
		if (returns) {
			if (inst.src0.kind != Decoder::OperandKind::Sgpr || inst.src0.reg != call.dst.reg)
				return unlinked("leaf returns elsewhere");
			inst.opcode        = Decoder::Opcode::S_BRANCH;
			inst.branch_target = call.pc + call.word_count * sizeof(uint32_t);
		} else if (Decoder::IsDirectBranch(inst.opcode)) {
			if (inst.branch_target > UINT32_MAX - function_pc) return unlinked("branch overflows");
			inst.branch_target += function_pc;
		}
		inst.pc += function_pc;
		linked.decoded.instructions.push_back(std::move(inst));
		if (returns) break;
	}
	const auto function_end = linked.decoded.instructions.back().pc +
	                          linked.decoded.instructions.back().word_count * sizeof(uint32_t);
	for (size_t i = source.decoded.instructions.size(); i + 1u < linked.decoded.instructions.size();
	     ++i) {
		const auto& inst = linked.decoded.instructions[i];
		if (Decoder::IsDirectBranch(inst.opcode) &&
		    (inst.branch_target < function_pc || inst.branch_target >= function_end))
			return unlinked("leaf branches out of itself");
	}
	reads.resize(query_reads);
	reads.emplace_back(address, (linked.code.size() - source.code.size()) * sizeof(uint32_t));
	linked.decoded.instructions[source.call->instruction].branch_target = function_pc;
	linked.decoded.code = linked.code;
	linked.observed_function.resize(linked.code.size() - source.code.size());
	source.linked = std::move(linked);
	++source.revision;
	return source.linked->decoded;
}

TranslateResult TranslateProgram(std::span<const uint32_t> code, const CompileOptions& options) {
	auto source = PrepareShaderSource(code, options);
	return TranslateProgram(source.decoded, options);
}

TranslateResult TranslateProgram(const Decoder::Program& decoded, const CompileOptions& options) {
	EXIT_IF(decoded.instructions.empty());
	// An unlinked call (branch_target still UINT32_MAX) translates as the any-hit stub; only a
	// linked leaf becomes control flow inside this program.
	bool linked_call = false;
	if (decoded.has_swap_pc) {
		for (const auto& inst: decoded.instructions) {
			linked_call |=
			    inst.opcode == Decoder::Opcode::S_SWAPPC_B64 && inst.branch_target != UINT32_MAX;
		}
	}
	if (options.stage != ShaderType::Compute && options.stage != ShaderType::Vertex &&
	    options.stage != ShaderType::Pixel && options.stage != ShaderType::Mesh &&
	    options.stage != ShaderType::Local && options.stage != ShaderType::TessellationControl &&
	    options.stage != ShaderType::TessellationEvaluation) {
		EXIT("shader recompiler received unsupported stage %u\n",
		     static_cast<unsigned>(options.stage));
	}

	const auto compile_begin = std::chrono::steady_clock::now();
	const auto phase_ms = [&compile_begin]() {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		    std::chrono::steady_clock::now() - compile_begin).count());
	};

	std::string decoded_dump;
	if (options.dump_ir) {
		decoded_dump = Decoder::ProgramToString(decoded);
		if (options.early_dump) {
			LOGF("%s decoded RDNA2 (early):\n%s", GetDumpLabel(options), decoded_dump.c_str());
		}
	}

	CFG::Graph native_cfg;
	CFG::Graph structured_cfg;
	auto       cfg_outcome = CfgOutcome::Structured;
	const auto cfg_begin   = std::chrono::steady_clock::now();
	// Keyed by the words the program was decoded from, which include a linked SWAPPC leaf.
	const bool cfg_cached = StructurizedCfgCache().Get(decoded.code, options.back_code, native_cfg,
	                                                   structured_cfg, cfg_outcome);
	if (cfg_cached) {
		const auto& cached = cfg_outcome == CfgOutcome::Structured ? structured_cfg : native_cfg;
		LOGF("%s CFG cache hit: stage=%s hash=0x%016" PRIx64 " outcome=%s blocks=%" PRIu64
		     " loops=%" PRIu64 " elapsed_ms=%" PRIu64 "\n",
		     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
		     CfgOutcomeName(cfg_outcome), static_cast<uint64_t>(cached.blocks.size()),
		     static_cast<uint64_t>(cached.natural_loops.size()), phase_ms());
	} else {
		LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " CFG BuildGraph\n",
		     GetDumpLabel(options), StageName(options.stage), options.shader_hash);
		native_cfg = CFG::BuildGraph(decoded);
		LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " CFG BuildGraph blocks=%" PRIu64
		     " loops=%" PRIu64 " back_edges=%" PRIu64 " elapsed_ms=%" PRIu64 "\n",
		     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
		     static_cast<uint64_t>(native_cfg.blocks.size()),
		     static_cast<uint64_t>(native_cfg.natural_loops.size()),
		     static_cast<uint64_t>(native_cfg.back_edges.size()), phase_ms());
		// A shader the CFG builder cannot model - an undecodable instruction, a branch into
		// nothing - is dropped, not fatal. Only a build failure sets unsupported here: the
		// irreducible path clears it inside BuildGraph, and the structurizer has not run yet.
		if (native_cfg.unsupported) {
			cfg_outcome = CfgOutcome::BuildRejected;
		} else if (native_cfg.irreducible) {
			cfg_outcome = CfgOutcome::Irreducible;
			LogDispatcherFallback(options, native_cfg, "build");
		} else {
			LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " CFG Structurize\n",
			     GetDumpLabel(options), StageName(options.stage), options.shader_hash);
			structured_cfg = CFG::Structurize(native_cfg);
			if (structured_cfg.unsupported) {
				cfg_outcome                   = CfgOutcome::StructurizeFailed;
				native_cfg.unsupported        = true;
				native_cfg.failure_kind       = structured_cfg.failure_kind;
				native_cfg.failure_block      = structured_cfg.failure_block;
				native_cfg.unsupported_reason = structured_cfg.unsupported_reason;
				structured_cfg                = {};
				LogDispatcherFallback(options, native_cfg, "structurize");
			} else {
				LOGF("%s structured CFG success: blocks=%" PRIu64 "\n", GetDumpLabel(options),
				     static_cast<uint64_t>(structured_cfg.blocks.size()));
			}
			const auto& selected =
			    cfg_outcome == CfgOutcome::Structured ? structured_cfg : native_cfg;
			LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " CFG Structurize blocks=%" PRIu64
			     " loops=%" PRIu64 " elapsed_ms=%" PRIu64 "\n",
			     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
			     static_cast<uint64_t>(selected.blocks.size()),
			     static_cast<uint64_t>(selected.natural_loops.size()), phase_ms());
		}
		const auto cfg_cost_us =
		    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		                              std::chrono::steady_clock::now() - cfg_begin)
		                              .count());
		StructurizedCfgCache().Put(decoded.code, options.back_code, native_cfg, structured_cfg,
		                           cfg_outcome, cfg_cost_us);
	}
	StructurizedCfgCache().Report(GetDumpLabel(options));
	const auto* selected_cfg =
	    cfg_outcome == CfgOutcome::Structured ? &structured_cfg : &native_cfg;
	if (cfg_outcome == CfgOutcome::BuildRejected) {
		LOGF("%s CFG build rejected: stage=%s hash=0x%016" PRIx64 " pc=0x%08" PRIx32 " %s\n",
		     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
		     native_cfg.failure_pc, native_cfg.unsupported_reason.c_str());
		TranslateResult result;
		result.status.ok     = false;
		result.status.pc     = native_cfg.failure_pc;
		result.status.reason = native_cfg.unsupported_reason;
		if (options.dump_ir) {
			result.decoded_dump = std::move(decoded_dump);
			result.cfg_dump     = CFG::GraphToString(native_cfg);
		}
		return result;
	}
	// A hit has not logged why this body falls back to the dispatcher, and the run still needs to
	// be told - it is the difference between a shader that is structured and one that is not.
	if (cfg_cached && cfg_outcome != CfgOutcome::Structured) {
		LogDispatcherFallback(options, native_cfg,
		                      cfg_outcome == CfgOutcome::Irreducible ? "build" : "structurize");
	}

	const auto&                 cfg = *selected_cfg;
	Frontend::EmbeddedFetchPlan embedded_fetch;
	if ((options.stage == ShaderType::Vertex || options.stage == ShaderType::Local) &&
	    options.input_info.vertex != nullptr && options.input_info.vertex->fetch_embedded) {
		embedded_fetch = DetectEmbeddedVertexFetch(
		    decoded, options.input_info.vertex, options.user_data_base,
		    static_cast<uint32_t>(options.user_data.size()), options.wave_size);
		if (!embedded_fetch.loads.empty()) {
			LOGF("%s embedded vertex fetch plan: detected=%" PRIu64 "\n", GetDumpLabel(options),
			     static_cast<uint64_t>(embedded_fetch.loads.size()));
		}
	}
	Frontend::TranslateOptions translate_options {
	    .stage                 = options.stage,
	    .wave_size             = options.wave_size,
	    .host_subgroup_size    = options.host_subgroup_size,
	    .robust_buffer_access2 = options.robust_buffer_access2,
	    .shader_hash           = options.shader_hash,
	    .user_data_base        = options.user_data_base,
	    .user_data_count       = static_cast<uint32_t>(options.user_data.size()),
	    .input_info            = options.input_info,
	    .embedded_fetch        = embedded_fetch.loads.empty() ? nullptr : &embedded_fetch,
	};
	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " IR TranslateProgram\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash);
	auto ir = Frontend::TranslateProgram(decoded, cfg, translate_options);
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " IR TranslateProgram blocks=%" PRIu64
	     " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(ir.blocks.size()), phase_ms());
	std::vector<IR::Inst*> exec_reads;
	for (auto* block: ir.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() == IR::ValueOpcode::GetExec) {
				exec_reads.push_back(&inst);
			}
		}
	}
	IR::RewriteToSsa(ir.blocks);
	IR::ConstantPropagationPass(ir.blocks, ir.wave_size);
	// Before identities are removed: the former GetExec reads are still identities in place.
	IR::EliminateDeadLaneWriteMasks(ir, exec_reads);
	if (IR::FoldUnreachableIndexCompares(ir) != 0) {
		// Twice: the first pass reaches loop phis before their back edges are rewritten.
		IR::ConstantPropagationPass(ir.blocks, ir.wave_size);
		IR::ConstantPropagationPass(ir.blocks, ir.wave_size);
	}
	IR::ResolveControlFlowIdentities(ir);
	IR::RemoveIdentities(ir.blocks);
	IR::EliminateDeadCode(ir.blocks);
	const auto read_lane_stats = IR::EliminateReadLane(ir, ir.wave_size);
	if (read_lane_stats.rewritten_reads != 0) {
		LOGF("%s read-lane elimination: reads=%" PRIu32 "\n", GetDumpLabel(options),
		     read_lane_stats.rewritten_reads);
		IR::ConstantPropagationPass(ir.blocks, ir.wave_size);
		IR::ResolveControlFlowIdentities(ir);
		IR::RemoveIdentities(ir.blocks);
		IR::EliminateDeadCode(ir.blocks);
	}
	if (linked_call) {
		// Leaf returns are resolved CFG edges; native PC values must not escape into GPU data.
		for (const auto* block: ir.blocks)
			for (const auto& inst: *block)
				EXIT_NOT_IMPLEMENTED(inst.GetOpcode() == IR::ValueOpcode::GetShaderBase);
	}
	LowerTessellationMemory(ir, options);
	std::string cfg_dump;
	if (options.dump_ir) {
		cfg_dump = CFG::GraphToString(cfg);
		if (options.early_dump) {
			LOGF("%s native IR before resource tracking:\n%s", GetDumpLabel(options),
			     MakeIrDump(cfg_dump, ir).c_str());
		}
	}
	// What an earlier draw of this shader proved the walk cannot fold. Set before tracking, which
	// is the only pass that reads it: it is what lets a recognizer drop the guards that exist
	// solely to avoid moving a descriptor that still resolves.
	ir.unfoldable_pcs.assign(options.unfoldable_pcs.begin(), options.unfoldable_pcs.end());
	ir.refuse_indirect_buffer_table = options.refuse_indirect_buffer_table;
	auto tracking                   = IR::TrackResources(ir, decoded, native_cfg);
	ir.refuse_indirect_buffer_table = nullptr;
	TranslateResult result;
	if (!tracking.ok) {
		LOGF("%s resource tracking rejected: stage=%s hash=0x%016" PRIx64 " pc=0x%08" PRIx32
		     " %s\n",
		     GetDumpLabel(options), StageName(options.stage), options.shader_hash, tracking.pc,
		     tracking.reason.c_str());
		result.status.ok     = false;
		result.status.pc     = tracking.pc;
		result.status.reason = std::move(tracking.reason);
		result.program       = std::move(ir);
		return result;
	}
	result.program = std::move(ir);
	if (options.dump_ir) {
		result.decoded_dump = std::move(decoded_dump);
		result.cfg_dump     = std::move(cfg_dump);
	}
	return result;
}

CfgCacheStats CfgCacheStatistics() {
	return StructurizedCfgCache().Stats();
}

void OpenCfgCacheFile(const std::string& path, uint64_t stamp) {
	StructurizedCfgCache().OpenFile(path, stamp);
}

void FlushCfgCacheFile() {
	StructurizedCfgCache().Flush();
}

void ResetCfgCacheForTest() {
	StructurizedCfgCache().ResetForTest();
}

CompileResult CompileProgram(TranslateResult translated, const CompileOptions& options,
                             const IR::ResourceSpecialization& specialization,
                             uint32_t                          push_data_start_dword) {
	const auto emit_begin = std::chrono::steady_clock::now();
	auto&      ir         = translated.program;
	if (!IR::ApplyResourceSpecialization(ir, specialization)) {
		// Derived from a different tracking of this shader; refuse the permutation the way the
		// backend refuses one it cannot express.
		CompileResult refused;
		refused.status.ok     = false;
		refused.status.pc     = 0;
		refused.status.reason = "resource specialization does not describe this translation";
		refused.program       = std::move(ir);
		return refused;
	}
	// The resource plan owns host descriptor evaluation now. Keep only dependencies consumed
	// by GPU memory operations; bound descriptor dwords must not retain shader instructions.
	for (auto& inst: ir.value_storage) {
		inst.Invalidate();
	}
	for (auto* block: ir.blocks) {
		for (auto& inst: *block) {
			const auto op    = inst.GetOpcode();
			uint32_t   first = 0;
			if (op == IR::ValueOpcode::GetBufferResource) {
				// A descriptor the shader decodes for itself keeps every dword it decodes.
				if (std::ranges::any_of(inst.Uses(), [&](const IR::Use& use) {
					    const auto& memory =
					        ir.memory_info[use.user->Flags<IR::MemoryFlags>().index];
					    return memory.kind == IR::ResourceKind::IndirectBuffer ||
					           memory.dynamic_buffer;
				    })) {
					continue;
				}
				// A buffer table's root carries the runtime key in arg 0, as an image table's does.
				const auto resource = inst.Flags<uint32_t>();
				first = resource < ir.info.buffers.size() &&
				                ir.info.buffers[resource].indirect_root == resource &&
				                ir.info.buffers[resource].indirect_search_iterations != 0u
				            ? 1u
				            : 0u;
			} else if (op == IR::ValueOpcode::GetImageResource) {
				const auto resource = inst.Flags<uint32_t>();
				first               = resource < ir.info.images.size() &&
				                              ir.info.images[resource].indirect_root == resource
				                          ? 1u
				                          : 0u;
			} else if (op == IR::ValueOpcode::GetSamplerResource) {
				const auto resource = inst.Flags<uint32_t>();
				first               = resource < ir.info.samplers.size() &&
				                              !ir.info.samplers[resource].indirect_resources.empty()
				                          ? 1u
				                          : 0u;
			} else {
				continue;
			}
			for (size_t index = first; index < inst.NumArgs(); index++) {
				inst.SetArg(index, IR::Value(0u));
			}
		}
	}
	ir.value_storage.clear();
	IR::RemoveIdentities(ir.blocks);
	IR::EliminateDeadCode(ir.blocks);
	if (IR::EliminateUndemandedWriteMasks(ir) != 0) {
		IR::RemoveIdentities(ir.blocks);
		IR::EliminateDeadCode(ir.blocks);
	}

	IR::CollectShaderInfo(ir, options.input_info);
	std::string ir_dump;
	if (options.dump_ir) {
		ir_dump = MakeIrDump(translated.cfg_dump, ir);
		if (options.early_dump) {
			LOGF("%s native IR (early):\n%s", GetDumpLabel(options), ir_dump.c_str());
		}
	}

	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " SPIR-V EmitProgram\n",
	     GetDumpLabel(options), StageName(ir.stage), ir.shader_hash);
	std::string refusal;
	auto        spirv = Spirv::EmitProgram(ir, options.input_info, push_data_start_dword, &refusal);
	if (!refusal.empty()) {
		LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " SPIR-V EmitProgram refused: %s\n",
		     GetDumpLabel(options), StageName(ir.stage), ir.shader_hash, refusal.c_str());
		CompileResult refused;
		refused.status.ok     = false;
		refused.status.pc     = 0;
		refused.status.reason = std::move(refusal);
		refused.program       = std::move(ir);
		return refused;
	}
	if (spirv.size() >= Spirv::DriverSimplifyMinWords()) {
		Spirv::SimplifyForDriver(spirv);
	}
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " SPIR-V EmitProgram words=%" PRIu64
	     " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(ir.stage), ir.shader_hash,
	     static_cast<uint64_t>(spirv.size()),
	     static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                               std::chrono::steady_clock::now() - emit_begin)
	                               .count()));
	CompileResult result;
	result.spirv   = std::move(spirv);
	result.program = std::move(ir);
	if (options.dump_ir) {
		result.decoded_dump = std::move(translated.decoded_dump);
		result.ir_dump      = std::move(ir_dump);
	}
	return result;
}

} // namespace Libs::Graphics::ShaderRecompiler
