#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Decoder {
struct Program;
} // namespace Libs::Graphics::ShaderRecompiler::Decoder

namespace Libs::Graphics::ShaderRecompiler::IR {

// Drives each recognized compute BVH traversal loop over header format 1, 3 or 6 instances with a
// ray query over the host mirror of its TLAS: a flat walk over 64-bit node pointers, a BLAS walk
// nested under an instance leaf of a TLAS walk, or a walk over a stack the wave shares. Every
// iteration takes one candidate triangle into the loop's own triangle test, any-hit and commit
// code, or into copies of a shared-stack walk's instance and commit tests, which keep the hit
// state. Without a valid mirror the loop traverses as before. Returns the loops it drives. The
// walks of the native ray tracing sites (BvhIntersectNative) are left as they are.
[[nodiscard]] uint32_t TraceTraversalsWithRayQueries(Program& program);

// The traversal loops of the native ray tracing forms (sce::Psr templates), found before resource
// tracking: their TLAS source (RayTracing::TlasSource) and the guest pcs of their loops (the code
// that stays in the guest BVH, Decoder::Program::ray_tracing_reach).
struct NativeTraversals {
	uint32_t                                   sites         = 0; // Rewritten to RayQuery* sites.
	bool                                       source_valid  = false;
	uint32_t                                   user_sgpr     = 0;
	uint32_t                                   header_offset = 0;
	uint32_t                                   header_vsharp = 0xffffffffu;
	uint32_t                                   root          = 0;
	std::array<uint32_t, 4>                    layout {};
	std::vector<std::pair<uint32_t, uint32_t>> reach;
};

struct NativeTraversalOptions {
	uint32_t wave_size = 64;
	// Each walk runs over the host's ray queries (the RayQuery* operations of the native sites, one
	// site per walk, at most MaxRayQuerySites) when the host has the structures of its TLAS, else as
	// before.
	bool rewrite = false;
	// The program's machine code (the walks whose guest code is a known template).
	const Decoder::Program* decoded = nullptr;
};

// Recognizes them in an SSA program. The BVH intersections of the walks it rewrites take
// BvhIntersectNative.
[[nodiscard]] NativeTraversals TraceNativeTraversals(Program&                      program,
                                                     const NativeTraversalOptions& options);

} // namespace Libs::Graphics::ShaderRecompiler::IR
