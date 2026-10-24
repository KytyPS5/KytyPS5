// The focused target already amalgamates IR sources. Keep access to Tracker local
// to this test translation unit; production exposes no new analysis API.
#define KYTY_LOOP_PROOF_TESTING 1
#include "graphics/shader/recompiler/ir/passes/ResourceTracking.cpp"

#include <stdexcept>
#include <string_view>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {
struct LoopProofTestAccess {
  static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
  }

  static void CheckDceInvalidation(bool guarded, bool take_true) {
    Program program;
    program.user_data_count = 1;
    const auto block = [&]() {
      program.block_storage.push_back(std::make_unique<Block>());
      auto* result = program.block_storage.back().get();
      program.blocks.push_back(result);
      result->id = static_cast<uint32_t>(program.blocks.size() - 1);
      return result;
    };
    auto* entry = block();
    auto* header = block();
    auto* split = block();
    auto* detour = block();
    auto* body = block();
    auto* exit = block();
    entry->AddBranch(header);
    header->AddBranch(split);
    header->AddBranch(exit);
    split->AddBranch(body);
    split->AddBranch(detour);
    detour->AddBranch(body);
    body->AddBranch(header);
    entry->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = header};
    header->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                          .true_block = split, .false_block = exit, .loop_header = true};
    split->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                         .true_block = take_true ? body : detour,
                         .false_block = take_true ? detour : body};
    detour->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = body};
    body->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = header};
    auto& input = entry->AppendNewInst(ValueOpcode::GetUserData, {Value(ScalarReg(0))});
    auto& phi = header->AppendNewInst(ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
    auto& increment = body->AppendNewInst(ValueOpcode::IAdd32, {Value(&phi), Value(1u)});
    phi.AddPhiOperand(entry, Value(0u));
    phi.AddPhiOperand(body, Value(&increment));
    auto& compare = header->AppendNewInst(ValueOpcode::ULessThan32, {Value(&phi), Value(4u)});
    auto& condition = header->AppendNewInst(ValueOpcode::IEqual32, {Value(&input), Value(0u)});
    // Retain every instruction without a Reference in split masking the missing notification.
    auto& bound_reference = header->AppendNewInst(ValueOpcode::Reference, {Value(&compare)});
    auto& condition_reference = header->AppendNewInst(ValueOpcode::Reference, {Value(&condition)});
    header->condition = guarded ? Value(&compare) : Value(true);
    split->condition = Value(&condition);
    ValidateProgram(program, true);
    Decoder::Program decoded;
    CFG::Graph cfg;
    Tracker tracker(program, decoded, cfg, {});
    const auto query = [&] { return tracker.IncrementGuarded(phi, compare, body); };
    const auto key = std::tuple{&phi, &compare, static_cast<const Block*>(body)};
    Check(query() == guarded && query() == guarded, "DCE fixture did not cache expected verdict");
    const auto old_epoch = g_ir_mutation_epoch;
    Check(tracker.m_increment_guards.at(key).epoch == old_epoch, "DCE fixture cache epoch mismatch");
    const auto instructions = [&] {
      std::vector<const Inst*> result;
      for (const auto* current : program.blocks)
        for (const auto& inst : *current) result.push_back(&inst);
      return result;
    };
    const auto edges = [&] {
      std::vector<std::pair<const Block*, const Block*>> result;
      for (const auto* current : program.blocks) {
        for (const auto* next : current->ImmSuccessors()) result.emplace_back(current, next);
        for (const auto* previous : current->ImmPredecessors()) result.emplace_back(previous, current);
      }
      return result;
    };
    const auto old_instructions = instructions();
    const auto old_edges = edges();
    EliminateDeadCode(program.blocks);
    Check(split->condition == Value(take_true), "DCE fixture did not fold the intended branch");
    Check(instructions() == old_instructions, "DCE fixture deleted an instruction");
    Check(bound_reference.Arg(0) == Value(&compare) && condition_reference.Arg(0) == Value(&condition),
          "DCE fixture rewrote a retaining Reference");
    Check(edges() == old_edges, "DCE changed CFG edges");
    ValidateProgram(program, true);
    Check(g_ir_mutation_epoch != old_epoch, "DCE branch fold did not advance IR mutation epoch");
    Check(tracker.m_increment_guards.at(key).epoch == old_epoch, "DCE unexpectedly refreshed cache");
    const bool fresh = tracker.ComputeIncrementGuard(phi, compare, body);
    Check(query() == fresh && fresh == guarded, "post-DCE cached and fresh verdicts differ");
    Check(tracker.m_increment_guards.at(key).epoch == g_ir_mutation_epoch,
          "post-DCE query retained an obsolete cache entry");
    const auto refreshed_epoch = g_ir_mutation_epoch;
    Check(query() == fresh, "post-DCE cache reuse changed the verdict");
    EliminateDeadCode(program.blocks);
    Check(g_ir_mutation_epoch == refreshed_epoch, "no-op DCE invalidated cached proofs");
  }

  static void Run() {
    for (const bool guarded : {true, false})
      for (const bool take_true : {true, false}) CheckDceInvalidation(guarded, take_true);
    Program program;
    program.user_data_count = 6;
    const auto block = [&]() {
      program.block_storage.push_back(std::make_unique<Block>());
      auto* result = program.block_storage.back().get();
      program.blocks.push_back(result);
      result->id = static_cast<uint32_t>(program.blocks.size() - 1);
      return result;
    };
    auto* entry = block();
    auto* header = block();
    auto* body = block();
    auto* exit = block();
    entry->AddBranch(header);
    header->AddBranch(body);
    header->AddBranch(exit);
    body->AddBranch(header);
    entry->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = header};
    body->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = header};
    header->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                          .true_block = body, .false_block = exit};
    auto& phi = header->AppendNewInst(ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
    auto& increment = body->AppendNewInst(ValueOpcode::IAdd32, {Value(&phi), Value(1u)});
    phi.AddPhiOperand(entry, Value(0u));
    phi.AddPhiOperand(body, Value(&increment));
    auto& compare = header->AppendNewInst(ValueOpcode::ULessThan32, {Value(&phi), Value(4u)});
    auto& guard = header->AppendNewInst(ValueOpcode::Identity, {Value(&compare)});
    header->condition = Value(&guard);
    ValidateProgram(program, true);
    Decoder::Program decoded;
    CFG::Graph cfg;
    Tracker tracker(program, decoded, cfg, {});
    const auto stops = [&](const Block* current) { return current == header; };
    const auto query = [&] { return tracker.IncrementGuarded(phi, compare, body); };
    const auto fresh = [&] {
      LoopBoundProof proof(program, phi, compare);
      return tracker.GuardedOnEntry(body, stops, [&](const auto& edge) {
        return proof.Excludes(edge.condition, edge.positive);
      });
    };
    Check(query() && query(), "positive guard must be reusable");
    guard.SetArg(0, Value(true));
    Check(!query() && !query(), "stale positive guard after operand mutation");
    guard.SetArg(0, Value(&compare));
    Check(query(), "stale negative guard after operand mutation");
    guard.ReplaceUsesWith(Value(false), true);
    Check(query() == fresh(), "preserving replacement invalidation");

    // Adversarial state: public CFG fields require explicit notification inside
    // a live Tracker. Deliberately omit it to prove the verifier is sensitive.
    header->condition = Value(&compare);
    NoteIrMutation();
    Check(query(), "notified CFG guard must prove");
    header->condition = Value(true);
    Check(!fresh(), "unguarded increment must fail before stale-positive check");
    bool detected = false;
    try { (void)query(); }
    catch (const std::runtime_error& error) {
      Check(std::string_view(error.what()).find(
          "typed IR assertion: found->second.guarded != ComputeIncrementGuard") != std::string_view::npos,
          "unexpected exception in stale-positive verifier");
      detected = true;
    }
    Check(detected, "cache-hit verifier did not detect a stale positive");
    NoteIrMutation();
    Check(!query(), "notified CFG mutation must invalidate");

    header->condition = Value(false);
    Check(fresh(), "unreachable increment must prove before stale-negative check");
    detected = false;
    try { (void)query(); }
    catch (const std::runtime_error& error) {
      Check(std::string_view(error.what()).find(
          "typed IR assertion: found->second.guarded != ComputeIncrementGuard") != std::string_view::npos,
          "unexpected exception in stale-negative verifier");
      detected = true;
    }
    Check(detected, "cache-hit verifier did not detect a stale negative");

    header->condition = Value(false);
    NoteIrMutation();
    Check(query(), "unreachable increment must prove");
    exit->AddBranch(body);
    exit->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = body};
    Check(!query(), "new predecessor must invalidate");

    // These operations need not change this verdict, but must retire the record
    // before an address or a different Phi/flag dependency can be queried again.
    const auto check_refresh = [&](auto mutate) {
      const auto old_epoch = g_ir_mutation_epoch;
      mutate();
      Check(g_ir_mutation_epoch != old_epoch, "mutation did not notify a live analysis");
      Check(query() == fresh(), "mutation changed cached/fresh agreement");
      const auto key = std::tuple{&phi, &compare, static_cast<const Block*>(body)};
      Check(tracker.m_increment_guards.at(key).epoch == g_ir_mutation_epoch,
            "query retained an obsolete record");
    };
    check_refresh([&] { phi.SetArg(0, Value(1u)); });
    check_refresh([&] { compare.SetFlags<uint64_t>(1u); });
    check_refresh([&] {
      exit->AddBranch(header);
      exit->condition = Value(&compare);
      exit->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                          .true_block = body, .false_block = header};
      phi.AddPhiOperand(exit, Value(2u));
    });
    auto& unused = body->AppendNewInst(ValueOpcode::Void);
    Check(query() == fresh(), "unconnected zero-argument insertion changed a proof");
    check_refresh([&] {
      unused.SetParent(nullptr);
      program.value_storage.splice(program.value_storage.end(), body->Instructions(),
          std::prev(body->end()));
    });
    check_refresh([&] { unused.Invalidate(); program.value_storage.clear(); });

    // Independent Boolean truth tables: six free atoms and a failed current
    // bound, 64 assignments each. This is a finite soundness check, not a proof
    // of arithmetic, lane reductions or arbitrary loop semantics.
    struct Expression { Value value; uint64_t truth; };
    std::vector<Expression> expressions {{Value(&compare), 0u}, {Value(false), 0u},
                                         {Value(true), UINT64_MAX}};
    for (uint32_t bit = 0; bit < 6; ++bit) {
      auto& input = entry->AppendNewInst(ValueOpcode::GetUserData,
          {Value(static_cast<ScalarReg>(bit))});
      auto& atom = header->AppendNewInst(ValueOpcode::IEqual32,
          {Value(&input), Value(0u)});
      uint64_t truth = 0;
      for (uint32_t assignment = 0; assignment < 64; ++assignment)
        if (assignment & (1u << bit)) truth |= uint64_t{1} << assignment;
      expressions.push_back({Value(&atom), truth});
    }
    uint32_t seed = 0x52a715u;
    uint32_t exclusions = 0, unknowns = 0;
    const auto random = [&]() { seed = seed * 1664525u + 1013904223u; return seed; };
    for (uint32_t test = 0; test < 400; ++test) {
      const auto a = expressions[random() % expressions.size()];
      const auto b = expressions[random() % expressions.size()];
      const auto c = expressions[random() % expressions.size()];
      const auto op = random() % 5u;
      const auto opcode = std::array {ValueOpcode::LogicalNot, ValueOpcode::LogicalAnd,
          ValueOpcode::LogicalOr, ValueOpcode::SelectU1, ValueOpcode::Identity}[op];
      auto& inst = op == 0 || op == 4 ? header->AppendNewInst(opcode, {a.value}) :
          op == 3 ? header->AppendNewInst(opcode, {a.value, b.value, c.value}) :
                    header->AppendNewInst(opcode, {a.value, b.value});
      const uint64_t truth = op == 0 ? ~a.truth : op == 1 ? a.truth & b.truth :
          op == 2 ? a.truth | b.truth : op == 3 ? (a.truth & b.truth) | (~a.truth & c.truth) : a.truth;
      expressions.push_back({Value(&inst), truth});
      LoopBoundProof proof(program, phi, compare);
      const bool positive = proof.Excludes(Value(&inst), true);
      Check(!positive || truth == 0,
            "independent truth table refutes positive exclusion");
      LoopBoundProof negative(program, phi, compare);
      const bool inverse = negative.Excludes(Value(&inst), false);
      Check(!inverse || truth == UINT64_MAX,
            "independent truth table refutes negative exclusion");
      exclusions += positive + inverse;
      unknowns += !positive + !inverse;
    }
    Check(exclusions != 0 && unknowns != 0, "finite oracle did not exercise both proof outcomes");
    ValidateProgram(program, true);
  }
};
} // namespace
} // namespace Libs::Graphics::ShaderRecompiler::IR

void TestLoopProofCache() {
  Libs::Graphics::ShaderRecompiler::IR::LoopProofTestAccess::Run();
}
