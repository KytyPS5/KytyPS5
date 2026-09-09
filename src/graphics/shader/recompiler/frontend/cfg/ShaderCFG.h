#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERCFG_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERCFG_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <string>
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
	GotoVariable,
	DispatchDone,
	Unknown
};

inline constexpr uint32_t kDispatchHaltState = 0xFFFFFFFEu;

enum class TerminatorKind {
	Branch,
	ConditionalBranch,
	IndirectBranch,
	DispatchSwitch,
	Return,
	Unsupported
};

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
	uint32_t              goto_variable = UINT32_MAX;
	int32_t               goto_value    = -1;
	uint32_t              dispatch_next = UINT32_MAX;
	bool                  loop_header   = false;
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

enum class StructurizerKind {
	Auto,
	LegacySplit,
	DispatcherFull,
};

struct StructurizeOptions {
	StructurizerKind kind                      = StructurizerKind::Auto;
	uint32_t         max_block_growth_percent  = 200;
	uint32_t         max_rewrites              = 0;
	bool             validate_after_each_phase = false;
	bool             dump_graph_on_failure     = true;
	bool             collect_stats             = true;
};

struct StructurizeStats {
	StructurizerKind kind                     = StructurizerKind::LegacySplit;
	uint32_t         original_blocks          = 0;
	uint32_t         final_blocks             = 0;
	uint32_t         synthetic_blocks         = 0;
	uint32_t         cloned_semantic_blocks   = 0;
	uint32_t         rewrites                 = 0;
	uint32_t         loop_repairs             = 0;
	uint32_t         selection_repairs        = 0;
	uint32_t         route_blocks             = 0;
	uint32_t         dispatcher_cases         = 0;
	uint32_t         unresolved_illegal_edges = 0;
	uint64_t         elapsed_us               = 0;
	bool             success                  = false;
	std::string      failure_reason;
};

Graph              BuildGraph(const Decoder::Program& program);
bool               Structurize(Graph& graph);
StructurizeOptions ReadStructurizeOptions();
bool               StructurizeWithStrategy(Graph& graph, const StructurizeOptions& options,
                                          StructurizeStats* stats);
std::string        StructurizerKindToString(StructurizerKind kind);
std::string        BranchConditionToString(BranchCondition condition);
std::string        FailureKindToString(FailureKind kind);
std::string        GraphToString(const Graph& graph);

} // namespace Libs::Graphics::ShaderRecompiler::CFG

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERCFG_H_ */
