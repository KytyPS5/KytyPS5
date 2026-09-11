// SRT evaluator memo storage tests.
//
// Every Evaluator owns a private memo map whose storage comes from a per-thread pool. These tests
// run each evaluation with pooled storage (the production path) and with unpooled storage (the
// new/delete resource, i.e. the previous allocation behaviour; the container type is the same) and
// require identical return status, outputs and reader-call order. They also pin down the memo
// semantics that pooling must not disturb: separate main/clean and parent/ReadFirstLane contexts,
// dependencies that survive an enclosing failure, re-entrant and concurrent evaluation, allocation
// failure, and thread exit.

#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <memory_resource>
#include <new>
#include <numeric>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace srt_evaluator_memo_tests {

using namespace Libs::Graphics::ShaderRecompiler::IR;
using Libs::Graphics::ShaderType;

int g_checks = 0;

void Check(bool value, const std::string& text) {
	++g_checks;
	if (!value) {
		std::fprintf(stderr, "SrtEvaluatorMemoTests: failed: %s\n", text.c_str());
		std::fflush(stderr);
		std::abort();
	}
}

// ------------------------------------------------------------------------------- IR building

struct Fixture {
	Program program;
	Block*  block = nullptr;

	explicit Fixture(ShaderType stage = ShaderType::Compute) {
		program.stage                      = stage;
		program.user_data_count            = 64;
		program.srt_plan_complete          = true;
		program.resource_tracking_complete = true;
		auto storage                       = std::make_unique<Block>();
		block                              = storage.get();
		program.block_storage.push_back(std::move(storage));
		program.blocks.push_back(block);
		program.block_info.push_back({.id = 0});
	}

	Inst& EmitInst(ValueOpcode opcode, std::initializer_list<Value> args = {}) {
		return block->AppendNewInst(opcode, args);
	}
	Value Emit(ValueOpcode opcode, std::initializer_list<Value> args = {}) {
		return Value(&EmitInst(opcode, args));
	}
	Value UserData(uint32_t index) {
		return Emit(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(index))});
	}
	Value Handle(Value low, Value high) {
		return Emit(ValueOpcode::GetAddressResource, {low, high});
	}
	// A raw scalar read (the evaluator's guest-memory read path) at handle + byte offset.
	Value RawRead(Value handle, uint32_t offset) {
		MemoryInfo memory;
		memory.kind          = ResourceKind::ScalarAddress;
		memory.planning_only = true;
		const auto index     = static_cast<uint32_t>(program.memory_info.size());
		program.memory_info.push_back(memory);
		auto& inst =
		    EmitInst(ValueOpcode::LoadAddressU32, {handle, Value(offset), Value(0u), Value(true)});
		inst.SetFlags(MemoryFlags {.index = index, .pc = 0x40u + index * 4u});
		return Value(&inst);
	}
	uint32_t Source(std::initializer_list<Value> dwords) {
		DescriptorSource source;
		uint32_t         count = 0;
		for (const auto& dword: dwords) {
			source.dwords[count++] = dword;
		}
		source.dword_count = count;
		program.descriptor_sources.push_back(source);
		return static_cast<uint32_t>(program.descriptor_sources.size() - 1u);
	}
	uint32_t Flat(Value value) {
		const auto slot = static_cast<uint32_t>(program.srt_reads.size());
		program.srt_reads.push_back({value, slot});
		return slot;
	}
	Value ReadConst(uint32_t slot) {
		const auto srt = Emit(ValueOpcode::GetSrtResource);
		return Emit(ValueOpcode::ReadConst, {srt, Value(slot)});
	}
	Value Add(Value a, Value b) { return Emit(ValueOpcode::IAdd32, {a, b}); }
	Value Add(Value a, uint32_t b) { return Add(a, Value(b)); }
	Value And(Value a, uint32_t b) { return Emit(ValueOpcode::BitwiseAnd32, {a, Value(b)}); }
	Value IsZero(Value a) { return Emit(ValueOpcode::IEqual32, {a, Value(0u)}); }
	Value Select(Value c, uint32_t a, uint32_t b) {
		return Emit(ValueOpcode::SelectU32, {c, Value(a), Value(b)});
	}
	Value FirstLane(Value v, Value mask) { return Emit(ValueOpcode::ReadFirstLane, {v, mask}); }
};

std::vector<uint32_t> AllSources(const ResourcePlan& plan) {
	std::vector<uint32_t> all(plan.descriptor_sources.size());
	std::iota(all.begin(), all.end(), 0u);
	return all;
}

// ---------------------------------------------------------------------------------- readers

using Trace = std::vector<std::pair<char, uint64_t>>;

// Two guest memories: `raw` answers read_memory (main evaluator), `clean` answers
// read_specialization_memory (clean evaluator). Every call is recorded in order.
struct Readers {
	static constexpr uint64_t kBase = 0x100000;

	std::vector<uint32_t>                    raw   = std::vector<uint32_t>(4096);
	std::vector<uint32_t>                    clean = std::vector<uint32_t>(4096);
	Trace                                    trace;
	uint64_t                                 fail_raw_address   = UINT64_MAX;
	uint64_t                                 fail_clean_address = UINT64_MAX;
	std::function<bool(uint64_t, uint32_t&)> raw_hook;
};

bool ReadFrom(Readers& r, const std::vector<uint32_t>& words, char tag, uint64_t address,
              uint32_t* value) {
	r.trace.emplace_back(tag, address);
	if (value == nullptr || address < Readers::kBase || (address & 3u) != 0u ||
	    (address - Readers::kBase) / 4u >= words.size()) {
		return false;
	}
	*value = words[(address - Readers::kBase) / 4u];
	return true;
}

bool ReadRaw(void* userdata, uint64_t address, uint32_t* value) {
	auto& r = *static_cast<Readers*>(userdata);
	if (address == r.fail_raw_address) {
		r.trace.emplace_back('f', address);
		return false;
	}
	if (r.raw_hook) {
		uint32_t hooked = 0;
		if (r.raw_hook(address, hooked)) {
			r.trace.emplace_back('h', address);
			*value = hooked;
			return true;
		}
	}
	return ReadFrom(r, r.raw, 'r', address, value);
}

bool ReadClean(void* userdata, uint64_t address, uint32_t* value) {
	auto& r = *static_cast<Readers*>(userdata);
	if (address == r.fail_clean_address) {
		r.trace.emplace_back('g', address);
		return false;
	}
	return ReadFrom(r, r.clean, 'c', address, value);
}

SrtRuntime Runtime(Readers& r, std::span<const uint32_t> user_data) {
	return {.user_data                  = user_data,
	        .read_memory                = ReadRaw,
	        .userdata                   = &r,
	        .read_specialization_memory = ReadClean};
}

struct Input {
	Readers               readers;
	std::vector<uint32_t> user_data;
};

// seed selects the SRT base (kBase + 0x100 * (seed % 4)) and the memory contents.
Input MakeInput(uint32_t seed) {
	Input input;
	input.user_data = {static_cast<uint32_t>(Readers::kBase + 0x100u * (seed % 4u)),
	                   static_cast<uint32_t>(Readers::kBase >> 32u)};
	for (size_t i = 0; i < input.readers.raw.size(); ++i) {
		input.readers.raw[i]   = 0x01000000u * (seed + 1u) + static_cast<uint32_t>(i) * 16u;
		input.readers.clean[i] = ~input.readers.raw[i];
	}
	return input;
}

// ---------------------------------------------------------------------------------- outcomes

struct SourcesOutcome {
	bool                         threw = false;
	bool                         ok    = false;
	std::vector<DescriptorValue> results;
	std::vector<uint32_t>        flat;
	std::vector<uint8_t>         active;
	Trace                        trace;

	bool operator==(const SourcesOutcome&) const = default;
};

bool Same(const SourcesOutcome& a, const SourcesOutcome& b) {
	return a == b;
}

bool Untouched(const SourcesOutcome& out) {
	return out.results == std::vector<DescriptorValue> {DescriptorValue {{0xdead0001u}, 1u}} &&
	       out.flat == std::vector<uint32_t> {0xdead0002u} &&
	       out.active == std::vector<uint8_t> {0xabu};
}

SourcesOutcome EvalSources(const ResourcePlan& plan, const std::vector<uint32_t>& sources,
                           Readers& readers, const std::vector<uint32_t>& user_data,
                           const std::vector<uint8_t>& clean = {}) {
	SourcesOutcome out;
	readers.trace.clear();
	out.results = {DescriptorValue {{0xdead0001u}, 1u}};
	out.flat    = {0xdead0002u};
	out.active  = {0xabu};
	try {
		out.ok = EvaluateRuntimeSources(plan, sources, Runtime(readers, user_data), out.results,
		                                out.flat, clean, out.active);
	} catch (const std::bad_alloc&) {
		out.threw = true;
	}
	out.trace = readers.trace;
	return out;
}

struct MaterializeOutcome {
	bool                   threw = false;
	bool                   ok    = false;
	ResourceSnapshot       snapshot;
	ResourceSpecialization specialization;
	Trace                  trace;
};

bool SameSnapshot(const ResourceSnapshot& a, const ResourceSnapshot& b) {
	return a.buffers == b.buffers && a.images == b.images && a.samplers == b.samplers &&
	       a.flattened_srt == b.flattened_srt && a.user_data == b.user_data &&
	       a.uniform_fill == b.uniform_fill;
}

bool Same(const MaterializeOutcome& a, const MaterializeOutcome& b) {
	return a.threw == b.threw && a.ok == b.ok && SameSnapshot(a.snapshot, b.snapshot) &&
	       a.specialization == b.specialization && a.trace == b.trace;
}

MaterializeOutcome Materialize(const ResourcePlan& plan, Readers& readers,
                               const std::vector<uint32_t>& user_data) {
	MaterializeOutcome out;
	readers.trace.clear();
	try {
		out.ok = MaterializeResources(plan, Runtime(readers, user_data), out.snapshot,
		                              out.specialization);
	} catch (const std::bad_alloc&) {
		out.threw = true;
	}
	out.trace = readers.trace;
	return out;
}

// Runs `run` with unpooled storage, then twice with the thread's pool (cold, then warm), and
// requires identical outcomes.
template <typename Outcome, typename Run>
Outcome Differential(const std::string& name, Run&& run) {
	SrtTestHooks::SetMemoResource(std::pmr::new_delete_resource());
	const Outcome unpooled = run();
	SrtTestHooks::SetMemoResource(nullptr);
	const Outcome pooled       = run();
	const Outcome pooled_again = run();
	Check(Same(unpooled, pooled), name + ": pooled storage changed the result");
	Check(Same(pooled, pooled_again), name + ": reusing the pool changed the result");
	return pooled;
}

// Forwards to new/delete, counts blocks, and throws std::bad_alloc at allocation `fail_at`.
class TestResource final: public std::pmr::memory_resource {
public:
	explicit TestResource(uint64_t fail_at = UINT64_MAX): m_fail_at(fail_at) {}

	uint64_t allocations = 0;
	int64_t  live_bytes  = 0;

private:
	void* do_allocate(size_t bytes, size_t alignment) override {
		if (allocations == m_fail_at) {
			throw std::bad_alloc();
		}
		void* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
		allocations++;
		live_bytes += static_cast<int64_t>(bytes);
		return result;
	}
	void do_deallocate(void* p, size_t bytes, size_t alignment) override {
		live_bytes -= static_cast<int64_t>(bytes);
		std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
	}
	bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
		return this == &other;
	}

	uint64_t m_fail_at;
};

// --------------------------------------------------------------------------------- plans

// Graphics-like plan: buffers and a sampler built from raw SRT reads with shared
// subexpressions, plus flat SRT slots.
ResourcePlan MakeBufferPlan(ShaderType stage, uint32_t extra_buffers = 0) {
	Fixture              f(stage);
	const auto           handle = f.Handle(f.UserData(0), f.UserData(1));
	std::array<Value, 8> r;
	for (uint32_t i = 0; i < r.size(); ++i) {
		r[i] = f.RawRead(handle, 4u * i);
	}
	const auto hi      = f.And(r[1], 0xffffu);
	const auto b0      = f.Source({r[0], hi, r[2], Value(0u)});
	const auto shifted = f.Add(r[0], 0x100u);
	const auto b1      = f.Source({shifted, hi, r[2], Value(0u)});
	f.program.info.buffers.push_back({.source = b0});
	f.program.info.buffers.push_back({.source = b1});
	for (uint32_t i = 0; i < extra_buffers; ++i) {
		const auto base = f.Add(r[3u + i % 5u], 0x40u * (i + 1u));
		f.program.info.buffers.push_back({.source = f.Source({base, hi, r[2], Value(0u)})});
	}
	f.program.info.samplers.push_back({.source = f.Source({r[4], r[5], r[6], r[7]})});
	f.Flat(r[0]);
	f.Flat(shifted);
	f.Flat(r[6]);
	return ExtractResourcePlan(f.program);
}

// Mask false, s = SelectU32(mask, 7, 9), q = s + 1: the parent sees q = 10, a ReadFirstLane
// child under mask must see q = 8.
struct MaskPlan {
	ResourcePlan plan;
	uint32_t     parent_first = 0; // {q, child}
	uint32_t     child_first  = 0; // {child, q}
};

MaskPlan MakeMaskPlan() {
	Fixture    f;
	const auto mask  = f.IsZero(f.UserData(0));
	const auto q     = f.Add(f.Select(mask, 7u, 9u), 1u);
	const auto child = f.FirstLane(q, mask);
	MaskPlan   result;
	result.parent_first = f.Source({q, child});
	result.child_first  = f.Source({child, q});
	result.plan         = ExtractResourcePlan(f.program);
	return result;
}

// ReadFirstLane chain: v0 = ud0; v_k = FirstLane(v_{k-1} + sel_k, m_k) with
// sel_k = Select(m_k, k, 1000 + k). Every sel_k is evaluated inside the child whose mask is m_k,
// so v_N = ud0 + N(N+1)/2, and the parent-level v_{N-1} + sel_N = ud0 + (N-1)N/2 + 1000 + N.
struct DeepPlan {
	ResourcePlan plan;
	uint32_t     source = 0;
};

DeepPlan MakeDeepPlan(uint32_t depth) {
	Fixture f;
	Value   v = f.UserData(0);
	Value   previous;
	Value   last_sel;
	for (uint32_t k = 1; k <= depth; ++k) {
		const auto mask = f.IsZero(f.UserData(1u + k % 2u));
		const auto sel  = f.Select(mask, k, 1000u + k);
		previous        = v;
		last_sel        = sel;
		v               = f.FirstLane(f.Add(v, sel), mask);
	}
	DeepPlan result;
	result.source = f.Source({v, f.Add(previous, last_sel)});
	result.plan   = ExtractResourcePlan(f.program);
	return result;
}

// -------------------------------------------------------------------------------------- tests

void TestSameEvaluationInputsAndBindings() {
	const auto plan = MakeBufferPlan(ShaderType::Pixel);
	auto       a    = MakeInput(1);
	auto       b    = MakeInput(2);
	const auto first =
	    Differential<MaterializeOutcome>("same inputs",
	                                     [&] { return Materialize(plan, a.readers, a.user_data); });
	const uint32_t r0 = a.readers.raw[0x40];
	Check(first.ok && first.snapshot.buffers.size() == 2 && first.snapshot.samplers.size() == 1 &&
	          first.snapshot.flattened_srt ==
	              std::vector<uint32_t> {r0, r0 + 0x100u, a.readers.raw[0x46]} &&
	          first.trace.size() == 7,
	      "materialized values, or one read per reachable SRT word");
	const auto all = AllSources(plan);
	Differential<SourcesOutcome>("runtime sources",
	                             [&] { return EvalSources(plan, all, a.readers, a.user_data); });

	// Different inputs through the same instructions: A, B, A.
	const auto second =
	    Differential<MaterializeOutcome>("input B",
	                                     [&] { return Materialize(plan, b.readers, b.user_data); });
	Check(!SameSnapshot(first.snapshot, second.snapshot), "inputs A and B must differ");
	Check(Same(Materialize(plan, a.readers, a.user_data), first),
	      "A -> B -> A changed the result for A");

	// Same specialization, new binding: only the first buffer's base address changes.
	auto c = a;
	c.readers.raw[0x40] += 0x1000u;
	const auto rebound =
	    Differential<MaterializeOutcome>("rebound",
	                                     [&] { return Materialize(plan, c.readers, c.user_data); });
	Check(rebound.specialization == first.specialization &&
	          rebound.snapshot.buffers[0].dwords[0] == first.snapshot.buffers[0].dwords[0] + 0x1000u,
	      "same specialization must still see the new binding");

	// Graphics-like and compute-like plans interleaved on one pool, compared with unpooled runs.
	const auto graphics = MakeBufferPlan(ShaderType::Pixel, 3);
	const auto compute  = MakeMaskPlan();
	Readers    compute_readers;
	SrtTestHooks::SetMemoResource(std::pmr::new_delete_resource());
	const auto g_ref = Materialize(graphics, a.readers, a.user_data);
	const auto c_ref = EvalSources(compute.plan, {compute.parent_first}, compute_readers, {1u});
	SrtTestHooks::SetMemoResource(nullptr);
	for (int i = 0; i < 20000; ++i) {
		if ((i & 1) == 0) {
			if (!Same(Materialize(graphics, a.readers, a.user_data), g_ref)) {
				Check(false, "interleaved graphics evaluation " + std::to_string(i));
			}
		} else if (!Same(EvalSources(compute.plan, {compute.parent_first}, compute_readers, {1u}),
		                 c_ref)) {
			Check(false, "interleaved compute evaluation " + std::to_string(i));
		}
	}
}

void TestRawAndCleanEvaluatorsStaySeparate() {
	Fixture    f;
	const auto handle = f.Handle(f.UserData(0), f.UserData(1));
	const auto x      = f.RawRead(handle, 0x20);
	const auto slot   = f.Flat(x);
	const auto src    = f.Source({x, f.ReadConst(slot)});
	const auto plan   = ExtractResourcePlan(f.program);
	auto       in     = MakeInput(0);
	in.readers.raw[8]   = 0xaaaa0001u;
	in.readers.clean[8] = 0xbbbb0002u;
	const std::vector<uint8_t> clean {1u};
	const auto out = Differential<SourcesOutcome>(
	    "raw/clean", [&] { return EvalSources(plan, {src}, in.readers, in.user_data, clean); });
	Check(out.ok && out.results[0].dwords[0] == 0xaaaa0001u &&
	          out.results[0].dwords[1] == 0xbbbb0002u &&
	          out.flat == std::vector<uint32_t> {0xbbbb0002u},
	      "main and clean evaluators must keep separate memo contents");
	Check(out.trace == Trace {{'r', Readers::kBase + 0x20}, {'c', Readers::kBase + 0x20}},
	      "reader order");
}

void TestReadFirstLaneContexts() {
	const auto m = MakeMaskPlan();
	Readers    readers;
	const auto parent = Differential<SourcesOutcome>(
	    "parent first", [&] { return EvalSources(m.plan, {m.parent_first}, readers, {1u}); });
	const auto child = Differential<SourcesOutcome>(
	    "child first", [&] { return EvalSources(m.plan, {m.child_first}, readers, {1u}); });
	Check(parent.ok && parent.results[0].dwords[0] == 10u && parent.results[0].dwords[1] == 8u,
	      "parent must see q = 10 and the ReadFirstLane child q = 8");
	Check(child.ok && child.results[0].dwords[0] == 8u && child.results[0].dwords[1] == 10u,
	      "child-first order must give 8 then 10");

	Fixture    f; // two nested levels with interacting masks
	const auto m1  = f.IsZero(f.UserData(0));
	const auto m2  = f.IsZero(f.UserData(1));
	const auto s1  = f.Select(m1, 100u, 200u);
	const auto s2  = f.Select(m2, 10u, 20u);
	const auto t   = f.Add(s1, s2);
	const auto c2  = f.FirstLane(t, m2);
	const auto u   = f.Add(c2, s1);
	const auto src = f.Source({t, c2, u, f.FirstLane(u, m1)});
	const auto two = ExtractResourcePlan(f.program);
	const auto out = Differential<SourcesOutcome>(
	    "two nested levels", [&] { return EvalSources(two, {src}, readers, {1u, 1u}); });
	Check(out.ok && out.results[0].dwords[0] == 220u && out.results[0].dwords[1] == 210u &&
	          out.results[0].dwords[2] == 410u && out.results[0].dwords[3] == 310u,
	      "two nested ReadFirstLane levels");
	for (const uint32_t depth: {3u, 8u, 24u}) {
		const auto deep  = MakeDeepPlan(depth);
		const auto chain = Differential<SourcesOutcome>(
		    "depth " + std::to_string(depth),
		    [&] { return EvalSources(deep.plan, {deep.source}, readers, {5u, 1u, 1u}); });
		Check(chain.ok && chain.results[0].dwords[0] == 5u + depth * (depth + 1u) / 2u &&
		          chain.results[0].dwords[1] == 5u + (depth - 1u) * depth / 2u + 1000u + depth,
		      "nested ReadFirstLane depth " + std::to_string(depth));
	}
}

void TestFailureThenSuccess() {
	Fixture    f;
	const auto handle = f.Handle(f.UserData(0), f.UserData(1));
	const auto a      = f.RawRead(handle, 0x40);
	const auto b      = f.RawRead(handle, 0x44);
	const auto cond   = f.IsZero(f.Add(a, b));
	f.Flat(a);
	const auto s0 = f.Source({a});
	const auto s1 = f.Source({b});
	f.program.control_flow = {
	    ResourceBlock {.condition = cond, .successors = {1u, 2u}, .sources = {}},
	    ResourceBlock {.condition = {}, .successors = {}, .sources = {s0}},
	    ResourceBlock {.condition = {}, .successors = {}, .sources = {s1}},
	};
	// The clean read of b fails, so the condition fails after a was evaluated successfully; the
	// clean flat slot must reuse a's surviving memo entry (one clean read of a).
	auto in = MakeInput(0);
	in.readers.fail_clean_address = Readers::kBase + 0x44;
	const std::vector<uint8_t> clean {1u};
	const auto out = Differential<SourcesOutcome>(
	    "condition failure",
	    [&] { return EvalSources(f.program, {s0, s1}, in.readers, in.user_data, clean); });
	const auto clean_a_reads =
	    std::count(out.trace.begin(), out.trace.end(),
	               std::pair<char, uint64_t> {'c', Readers::kBase + 0x40});
	Check(out.ok && clean_a_reads == 1 && out.flat == std::vector<uint32_t> {in.readers.clean[16]},
	      "a successful dependency must survive its enclosing instruction's failure");

	in.readers.fail_clean_address = UINT64_MAX;
	in.readers.fail_raw_address   = Readers::kBase + 0x44;
	const auto failed = Differential<SourcesOutcome>(
	    "failing call", [&] { return EvalSources(f.program, {s1}, in.readers, in.user_data); });
	Check(!failed.ok && !failed.threw && Untouched(failed),
	      "a failed evaluation must not change its outputs");
	in.readers.fail_raw_address = UINT64_MAX;
	const auto recovered = Differential<SourcesOutcome>(
	    "recovered call", [&] { return EvalSources(f.program, {s1}, in.readers, in.user_data); });
	Check(recovered.ok && recovered.results[0].dwords[0] == in.readers.raw[17],
	      "evaluation after a failure must see fresh values");

	Fixture    g; // out-of-range user data
	const auto bad = g.Source({g.UserData(40)});
	Readers    readers;
	Check(Untouched(Differential<SourcesOutcome>(
	          "invalid user data", [&] { return EvalSources(g.program, {bad}, readers, {1u, 2u}); })),
	      "out-of-range user data must fail");
	Fixture h; // cycle
	auto&   inst = h.EmitInst(ValueOpcode::IAdd32, {Value(1u), Value(2u)});
	inst.SetArg(0, Value(&inst));
	const auto cyclic = h.Source({h.Add(Value(&inst), 3u)});
	Check(Untouched(Differential<SourcesOutcome>(
	          "cycle", [&] { return EvalSources(h.program, {cyclic}, readers, {}); })),
	      "a cycle must fail");
	inst.SetArg(0, Value(1u));
}

void TestCallbackReentry() {
	const auto inner = MakeMaskPlan();
	Fixture    f;
	const auto handle = f.Handle(f.UserData(0), f.UserData(1));
	const auto x      = f.RawRead(handle, 0x80);
	const auto src    = f.Source({f.Add(x, 1u), x});
	const auto outer  = ExtractResourcePlan(f.program);
	auto       in     = MakeInput(0);
	in.readers.raw_hook = [&](uint64_t address, uint32_t& value) {
		if (address != Readers::kBase + 0x80) {
			return false;
		}
		Readers                      inner_readers;
		const std::vector<uint32_t>  ud {1u};
		std::vector<DescriptorValue> results;
		std::vector<uint32_t>        flat;
		std::vector<uint8_t>         active;
		const uint32_t               source = inner.parent_first;
		if (!EvaluateRuntimeSources(inner.plan, std::span {&source, 1}, Runtime(inner_readers, ud),
		                            results, flat, {}, active)) {
			return false;
		}
		value = results[0].dwords[1];
		return true;
	};
	const auto out = Differential<SourcesOutcome>(
	    "re-entry", [&] { return EvalSources(outer, {src}, in.readers, in.user_data); });
	Check(out.ok && out.results[0].dwords[0] == 9u && out.results[0].dwords[1] == 8u,
	      "an evaluation started from a reader callback must not disturb the outer memo");
}

void TestTwoThreadsAndThreadExit() {
	const auto plan = MakeBufferPlan(ShaderType::Pixel, 2);
	auto       a    = MakeInput(5);
	auto       b    = MakeInput(6);
	SrtTestHooks::SetMemoResource(std::pmr::new_delete_resource());
	const auto ref_a = Materialize(plan, a.readers, a.user_data);
	const auto ref_b = Materialize(plan, b.readers, b.user_data);
	SrtTestHooks::SetMemoResource(nullptr);
	Check(ref_a.ok && ref_b.ok && !SameSnapshot(ref_a.snapshot, ref_b.snapshot), "thread references");

	std::atomic<int> failures {0};
	const auto       worker = [&](Input input, const MaterializeOutcome* ref) {
        for (int i = 0; i < 3000; ++i) {
            if (!Same(Materialize(plan, input.readers, input.user_data), *ref)) {
                failures.fetch_add(1);
            }
        }
	};
	std::thread t1(worker, a, &ref_a);
	std::thread t2(worker, b, &ref_b);
	t1.join();
	t2.join();
	Check(failures.load() == 0, "two threads evaluating one plan with different readers");

	// Each thread builds its pool, evaluates (including nested contexts) and tears the pool down at
	// exit; later threads must start clean.
	const auto deep = MakeDeepPlan(24);
	for (int round = 0; round < 8; ++round) {
		MaterializeOutcome outcome;
		SourcesOutcome     nested;
		std::thread([&] {
			auto    local = a;
			Readers readers;
			outcome = Materialize(plan, local.readers, local.user_data);
			nested  = EvalSources(deep.plan, {deep.source}, readers, {5u, 1u, 1u});
		}).join();
		Check(Same(outcome, ref_a) && nested.ok && nested.results[0].dwords[0] == 305u,
		      "evaluation on a fresh thread after earlier threads exited");
	}
}

void TestAllocationFailure() {
	const auto plan = MakeBufferPlan(ShaderType::Pixel, 2);
	const auto all  = AllSources(plan);
	auto       in   = MakeInput(12);
	const auto reference =
	    Differential<SourcesOutcome>("allocation reference", [&] {
		    return EvalSources(plan, all, in.readers, in.user_data);
	    });
	Check(reference.ok, "allocation reference");

	TestResource counting;
	SrtTestHooks::SetMemoResource(&counting);
	Check(Same(EvalSources(plan, all, in.readers, in.user_data), reference) &&
	          counting.allocations > 0 &&
	          counting.live_bytes == 0,
	      "memo storage must come from the evaluator's resource and be returned when it ends");
	const auto total = counting.allocations;

	int threw_count = 0;
	for (uint64_t fail_at = 0; fail_at <= total; ++fail_at) {
		TestResource failing(fail_at);
		SrtTestHooks::SetMemoResource(&failing);
		const auto out = EvalSources(plan, all, in.readers, in.user_data);
		SrtTestHooks::SetMemoResource(nullptr);
		const auto label = "fail at allocation " + std::to_string(fail_at);
		Check(out.threw ? Untouched(out) : Same(out, reference),
		      "allocation failure must propagate as std::bad_alloc with outputs untouched, " + label);
		Check(failing.live_bytes == 0, "memo storage leaked after an allocation failure, " + label);
		Check(Same(EvalSources(plan, all, in.readers, in.user_data), reference),
		      "pooled evaluation after an allocation failure, " + label);
		threw_count += out.threw ? 1 : 0;
	}
	Check(threw_count == static_cast<int>(total), "every allocation point must be able to fail");
}

void TestResultSurvivesAndWideValues() {
	const auto plan = MakeBufferPlan(ShaderType::Pixel, 2);
	auto       a    = MakeInput(9);
	auto       b    = MakeInput(10);
	const auto first = Materialize(plan, a.readers, a.user_data);
	const auto copy  = first;
	for (int i = 0; i < 1000; ++i) {
		Check(Materialize(plan, b.readers, b.user_data).ok, "later evaluation");
	}
	Check(first.ok && Same(first, copy),
	      "a snapshot must not change when later evaluations reuse the pool");

	Fixture    f;
	const auto v    = f.Emit(ValueOpcode::CompositeConstructU64, {f.UserData(0), f.UserData(1)});
	const auto w    = f.Emit(ValueOpcode::IAdd64, {v, v});
	const auto src  = f.Source({f.Emit(ValueOpcode::CompositeExtractU64, {w, Value(0u)}),
	                            f.Emit(ValueOpcode::CompositeExtractU64, {w, Value(1u)})});
	const auto imm  = f.Source({Value(1u), Value(2u), Value(3u)});
	const auto wide = ExtractResourcePlan(f.program);
	Readers    readers;
	const auto out = Differential<SourcesOutcome>(
	    "64-bit memo value",
	    [&] { return EvalSources(wide, {src, imm}, readers, {0x80000001u, 0x12345678u}); });
	Check(out.ok && out.results[0].dwords[0] == 0x00000002u &&
	          out.results[0].dwords[1] == 0x2468acf1u &&
	          out.results[1].dwords[2] == 3u,
	      "a memoized 64-bit value must keep its high bits");
}

int RunAll() {
	TestSameEvaluationInputsAndBindings();
	TestRawAndCleanEvaluatorsStaySeparate();
	TestReadFirstLaneContexts();
	TestFailureThenSuccess();
	TestCallbackReentry();
	TestTwoThreadsAndThreadExit();
	TestAllocationFailure();
	TestResultSurvivesAndWideValues();
	std::printf("SrtEvaluatorMemoTests: all cases passed (%d checks)\n", g_checks);
	return 0;
}

} // namespace srt_evaluator_memo_tests

namespace Common {

int DbgExitHandler(const char*, int, std::string_view) {
	std::abort();
}

int DbgExitIfHandler(const char*, const char*, int) {
	return 1;
}

void DbgExit(int) {
	std::abort();
}

} // namespace Common

// Keep this focused standalone target self-contained by amalgamating its small typed-IR
// implementation set, as ResourceMaterializationTests does.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"

int main() {
	return srt_evaluator_memo_tests::RunAll();
}
