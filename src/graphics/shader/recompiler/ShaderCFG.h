#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERCFG_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERCFG_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/ShaderDecoder.h"

#include <vector>

namespace Libs::Graphics::ShaderRecompiler::CFG {

enum class BranchCondition {
	Always,
	SccZero,
	SccNonZero,
	VccZero,
	VccNonZero,
	ExecZero,
	ExecNonZero,
	Unknown
};

enum class TerminatorKind { Branch, ConditionalBranch, IndirectBranch, Return, Unsupported };

enum class FailureKind {
	None,
	InvalidInput,
	UnsupportedInstruction,
	InvalidBranchTarget,
	MissingFallthrough,
	InvalidLabel,
	IrreducibleControlFlow,
	StructuredControlFlow
};

struct Terminator {
	TerminatorKind        kind                   = TerminatorKind::Return;
	BranchCondition       condition              = BranchCondition::Always;
	uint32_t              true_block             = UINT32_MAX;
	uint32_t              false_block            = UINT32_MAX;
	uint32_t              merge_block            = UINT32_MAX;
	uint32_t              continue_block         = UINT32_MAX;
	uint32_t              indirect_pc_sgpr       = UINT32_MAX;
	uint32_t              indirect_selector_code = UINT32_MAX;
	std::vector<uint32_t> indirect_target_pcs;
	std::vector<uint32_t> indirect_targets;
	std::vector<uint32_t> indirect_selector_values;
	std::vector<uint32_t> indirect_selector_targets;
	bool                  loop_header = false;
};

struct BasicBlock {
	uint32_t              id         = 0;
	uint32_t              start_pc   = 0;
	uint32_t              end_pc     = 0;
	uint32_t              inst_begin = 0;
	uint32_t              inst_end   = 0;
	std::vector<uint32_t> predecessors;
	std::vector<uint32_t> successors;
	std::vector<uint32_t> dominators;
	std::vector<uint32_t> post_dominators;
	Terminator            terminator;
};

struct BackEdge {
	uint32_t from    = UINT32_MAX;
	uint32_t to      = UINT32_MAX;
	bool     natural = false;
};

struct NaturalLoop {
	uint32_t              header         = UINT32_MAX;
	uint32_t              latch          = UINT32_MAX;
	uint32_t              merge          = UINT32_MAX;
	uint32_t              continue_block = UINT32_MAX;
	std::vector<uint32_t> body_blocks;
	std::vector<uint32_t> exit_blocks;
};

struct StronglyConnectedComponent {
	std::vector<uint32_t> blocks;
	std::vector<uint32_t> entry_blocks;
	bool                  irreducible = false;
};

struct Graph {
	std::vector<BasicBlock>                 blocks;
	std::vector<BackEdge>                   back_edges;
	std::vector<NaturalLoop>                natural_loops;
	std::vector<StronglyConnectedComponent> components;
	std::vector<uint32_t>                   code_table_load_pcs;
	uint32_t                                entry_block   = UINT32_MAX;
	bool                                    irreducible   = false;
	bool                                    unsupported   = false;
	FailureKind                             failure_kind  = FailureKind::None;
	uint32_t                                failure_block = UINT32_MAX;
	std::string                             unsupported_reason;

	const BasicBlock* FindBlock(uint32_t id) const;
	BasicBlock*       FindBlock(uint32_t id);
	const BasicBlock* FindBlockByPc(uint32_t pc) const;
	BasicBlock*       FindBlockByPc(uint32_t pc);
	bool              Dominates(uint32_t dominator, uint32_t block) const;
	bool              PostDominates(uint32_t post_dominator, uint32_t block) const;
	uint32_t          FindNearestCommonPostDominator(uint32_t block_a, uint32_t block_b) const;
};

bool        BuildGraph(const Decoder::Program& program, Graph* graph, std::string* error);
bool        Structurize(Graph* graph, std::string* error);
std::string BranchConditionToString(BranchCondition condition);
std::string FailureKindToString(FailureKind kind);
std::string FormatBlockDiagnostic(const Graph& graph, uint32_t block_id, const char* stage,
                                  const std::string& reason);
std::string GraphToString(const Graph& graph);

} // namespace Libs::Graphics::ShaderRecompiler::CFG

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERCFG_H_ */
