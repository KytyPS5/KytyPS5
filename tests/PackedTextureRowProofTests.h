#pragma once

#include "graphics/shader/recompiler/ir/passes/PackedTextureRowProof.h"

#include <array>
#include <stdexcept>

namespace PackedTextureRowProofTests {

using namespace Libs::Graphics::ShaderRecompiler::IR;
namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;

inline void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    Program program;
    Block *entry, *header, *body, *exit, *packed_header, *packed_body;
    Value row;

    Block* AddBlock() {
        auto storage = std::make_unique<Block>();
        auto* result = storage.get();
        program.block_storage.push_back(std::move(storage));
        program.blocks.push_back(result);
        program.block_info.push_back({.id = uint32_t(program.block_info.size())});
        return result;
    }
    Value Emit(Block* block, ValueOpcode opcode, std::initializer_list<Value> args = {}, uint64_t flags = 0u) {
        return Value(&block->AppendNewInst(opcode, args, flags));
    }
    void Jump(Block* from, Block* to, uint32_t id) {
        from->AddBranch(to);
        auto& info = program.block_info[std::ranges::find(program.blocks, from) - program.blocks.begin()];
        info.terminator.kind = CFG::TerminatorKind::Branch;
        info.terminator.true_block = id;
    }
    explicit Fixture(bool early_exit = false, bool partial_write = false, bool invalid_shift = false, bool growing_mask = false) {
        program.stage = Libs::Graphics::ShaderType::Pixel;
        program.wave_size = 64u;
        program.info.images.emplace_back().read = true;
        MemoryInfo memory;
        memory.kind = ResourceKind::Image; memory.resource = 0u;
        program.memory_info.push_back(memory);
        entry = AddBlock(); header = AddBlock(); body = AddBlock(); exit = AddBlock();
        packed_header = AddBlock(); packed_body = AddBlock();
        const auto lane = Emit(entry, ValueOpcode::LaneId);
        const auto active = Emit(entry, ValueOpcode::INotEqual32, {lane, Value(0u)});
        const auto record = Emit(entry, ValueOpcode::BitwiseAnd32, {lane, Value(1u)});
        const auto arbitrary = Emit(entry, ValueOpcode::UndefU32);
        Jump(entry, header, 1u); Jump(header, body, 2u);
        auto& remaining = header->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U1));
        remaining.AddPhiOperand(entry, active);
        const auto first = Emit(body, ValueOpcode::ReadFirstLane, {record, Value(&remaining)});
        const auto equal = Emit(body, ValueOpcode::IEqual32, {first, record});
        const auto selected = Emit(body, ValueOpcode::LogicalAnd, {Value(&remaining), equal});
        const auto written = partial_write ? Emit(body, ValueOpcode::LogicalAnd,
            {selected, Emit(body, ValueOpcode::INotEqual32, {lane, Value(2u)})}) : selected;
        const auto handle = Emit(body, ValueOpcode::GetImageResource,
            {Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u)});
        const auto address = Emit(body, ValueOpcode::MakeImageAddress,
            {Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
             Value(0u), Value(0u), Value(0u), Value(0u), Value(0u)});
        const auto image = Emit(body, ValueOpcode::ImageRead, {handle, address, written});
        std::array<Value, 3> rgb;
        for (uint32_t channel = 0; channel < 3u; ++channel) {
            auto& accumulated = header->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
            accumulated.AddPhiOperand(entry, arbitrary);
            const auto component = Emit(body, ValueOpcode::CompositeExtractU32x4, {image, Value(channel)});
            rgb[channel] = Emit(body, ValueOpcode::SelectU32, {written, component, Value(&accumulated)});
            accumulated.AddPhiOperand(body, rgb[channel]);
        }
        const auto after = Emit(body, growing_mask ? ValueOpcode::LogicalOr : ValueOpcode::LogicalAnd,
            {Value(&remaining), Emit(body, ValueOpcode::LogicalNot, {selected})});
        remaining.AddPhiOperand(body, after);
        body->AddBranch(header); body->AddBranch(exit);
        auto& term = program.block_info[2];
        term.terminator.kind = CFG::TerminatorKind::ConditionalBranch;
        term.terminator.true_block = 1u; term.terminator.false_block = 3u;
        term.condition = Emit(body, ValueOpcode::ConditionRef, {after},
            uint64_t(early_exit ? CFG::BranchCondition::SccNonZero : CFG::BranchCondition::ExecNonZero));
        auto packed = Emit(exit, ValueOpcode::BitwiseOr32, {Value(0xff000000u), rgb[0]});
        packed = Emit(exit, ValueOpcode::BitwiseOr32,
            {packed, Emit(exit, ValueOpcode::ShiftLeftLogical32, {rgb[1], Value(8u)})});
        packed = Emit(exit, ValueOpcode::BitwiseOr32,
            {packed, Emit(exit, ValueOpcode::ShiftLeftLogical32, {rgb[2], Value(16u)})});
        packed = Emit(exit, ValueOpcode::SelectU32, {active, packed, arbitrary});
        Jump(exit, packed_header, 4u); Jump(packed_header, packed_body, 5u);
        auto& words = packed_header->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
        words.AddPhiOperand(exit, packed);
        const auto shift = Emit(packed_body, ValueOpcode::ShiftRightLogical32,
            {Value(&words), Value(invalid_shift ? 4u : 8u)});
        const auto keep_or_shift = Emit(packed_body, ValueOpcode::SelectU32, {equal, shift, Value(&words)});
        words.AddPhiOperand(packed_body, keep_or_shift);
        Jump(packed_body, packed_header, 4u);
        const auto byte = Emit(packed_body, ValueOpcode::BitwiseAnd32, {Value(&words), Value(255u)});
        const auto initialized = Emit(packed_body, ValueOpcode::SelectU32, {active, byte, Value(UINT32_MAX)});
        const auto full = Emit(packed_body, ValueOpcode::LogicalOr,
            {active, Emit(packed_body, ValueOpcode::LogicalNot, {active})});
        const auto moved = Emit(packed_body, ValueOpcode::DppMoveU32, {initialized, full});
        const auto minimum = Emit(packed_body, ValueOpcode::UMin32, {moved, initialized});
        const auto updated = Emit(packed_body, ValueOpcode::DppUpdateU32, {minimum, initialized, full});
        const auto permuted = Emit(packed_body, ValueOpcode::Permlane16U32,
            {updated, Value(UINT32_MAX), Value(UINT32_MAX), full});
        const auto reduced = Emit(packed_body, ValueOpcode::UMin32, {updated, permuted});
        row = Emit(packed_body, ValueOpcode::UMin32,
            {Emit(packed_body, ValueOpcode::ReadLane, {reduced, Value(31u)}),
             Emit(packed_body, ValueOpcode::ReadLane, {reduced, Value(63u)})});
    }
};

inline void Run() {
    Fixture valid;
    const auto proof = ProveTextureWaveRowDomain(valid.program, valid.row);
    Require(proof.has_value(), "initialized waterfall packed RGB row proof missing");
    Require(proof->images.size() == 1u && proof->images[0].image == 0u && proof->images[0].channels == 7u,
        "packed row proof included alpha or lost RGB channel origins");
    for (const auto constant: {0u, 255u, UINT32_MAX})
        Require(std::ranges::find(proof->constants, constant) != proof->constants.end(), "packed row proof lost a neutral or shift-fill constant");
    Fixture early_exit(true);
    Require(!ProveTextureWaveRowDomain(early_exit.program, early_exit.row), "partial initialization at scalar early exit was admitted");
    Fixture partial_write(false, true);
    Require(!ProveTextureWaveRowDomain(partial_write.program, partial_write.row), "removed lane without matching accumulator write was admitted");
    Fixture invalid_shift(false, false, true);
    Require(!ProveTextureWaveRowDomain(invalid_shift.program, invalid_shift.row), "non-byte packed shift was admitted");
    Fixture growing_mask(false, false, false, true);
    Require(!ProveTextureWaveRowDomain(growing_mask.program, growing_mask.row), "growing initialization mask was admitted");
    Fixture missing_exit;
    missing_exit.program.block_info[2].terminator.false_block = UINT32_MAX;
    Require(!ProveTextureWaveRowDomain(missing_exit.program, missing_exit.row), "unknown initialization exit block was admitted");
    Fixture absent_exit_storage;
    absent_exit_storage.program.block_info.push_back({.id = 99u});
    absent_exit_storage.program.block_info[2].terminator.false_block = 99u;
    Require(!ProveTextureWaveRowDomain(absent_exit_storage.program, absent_exit_storage.row), "missing exit block storage was admitted");
    Fixture empty_first_lane;
    empty_first_lane.row = empty_first_lane.Emit(empty_first_lane.packed_body, ValueOpcode::ReadFirstLane,
        {empty_first_lane.row, Value(false)});
    Require(!ProveTextureWaveRowDomain(empty_first_lane.program, empty_first_lane.row), "empty first-lane selection was admitted");
    valid.program.wave_size = 32u;
    Require(!ProveTextureWaveRowDomain(valid.program, valid.row), "out-of-wave ReadLane63 was admitted");
}

} // namespace PackedTextureRowProofTests
