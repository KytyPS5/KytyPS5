#pragma once

#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

struct EmbeddedFetchLoad {
	uint32_t              pc         = 0;
	int                   attrib_id  = -1;
	uint32_t              components = 0;
	std::vector<uint32_t> prolog_loads;
};

struct EmbeddedFetchPlan {
	std::vector<EmbeddedFetchLoad> loads;
	int32_t                        vertex_offset_sgpr   = -1;
	int32_t                        instance_offset_sgpr = -1;
};

struct TranslateOptions {
	ShaderType                    stage               = ShaderType::Unknown;
	uint32_t                      wave_size           = 64;
	uint64_t                      shader_hash         = 0;
	uint32_t                      user_data_base      = 0;
	uint32_t                      user_data_count     = 64;
	uint32_t                      scratch_dwords      = 0;
	bool                          dispatcher_fallback = false;
	CFG::FailureKind              cfg_failure_kind    = CFG::FailureKind::None;
	std::string_view              fallback_reason;
	const ShaderVertexInputInfo*  vertex         = nullptr;
	const ShaderPixelInputInfo*   pixel          = nullptr;
	const ShaderComputeInputInfo* compute        = nullptr;
	const EmbeddedFetchPlan*      embedded_fetch = nullptr;
};

IR::Program TranslateProgram(const Decoder::Program& decoded, const CFG::Graph& cfg,
                             const TranslateOptions& options);

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
