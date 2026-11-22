# KytyPS5 mesh-shader compute fallback - hand-off

Goal: make KytyPS5 render 3D games on GPUs without VK_EXT_mesh_shader (user has AMD RX 5600M / RDNA1).
Fork: github.com/MOONL1GTH/KytyPS5MeshesFix (main = upstream 7d4e0d9 + "stage 1" commit that only skips mesh draws).
Patch: `mesh-compute-fallback.patch` (apply on top of stage 1: `git apply mesh-compute-fallback.patch`).
STATUS: builds and runs (Hello Neighbor 2 reaches its initial cutscene on an RX 5600M) but is unplayable: 0-4 fps and
most textures missing in the cutscene. Sandbox still cannot compile (no submodules/network), so every change is untested
until a GitHub Actions build. Iterate: user pastes red build errors / game console log -> fix -> push.

## Design
Mesh programs (guest NGG/primitive shaders, IR stage `ShaderType::Mesh`) are emitted as a COMPUTE shader when
`Profile::mesh_compute` (= !graphics.mesh_shader_enabled). Each workgroup writes a "slot" to a persistent 192 MiB
device buffer: header vec4 (vertex count, primitive count), vertices (vec4 position + vec4 per param location
0..highest), primitive records (uvec4 i0,i1,i2, flags: bit31 culled | layer). Layout code: `IR::MeshFallbackLayout`
in src/graphics/shader/recompiler/ir/ShaderIR.h (shared by emitter and host). Then a generic vertex shader
`src/graphics/host_gpu/shaders/gpu_mesh_fallback.vert` (4 variants PARAM_COUNT 8/16/24/32, built by CMake) draws
groups*max_primitives*3 vertices x instances, reading the buffer via buffer_reference; unused/culled triangles are degenerate.
Push constants: PushData::MeshDrawDwordCount 5 -> 8; dwords 5,6 = buffer address, 7 = group count X. The VS reads dwords 0..7:
max_prims, slot_stride, prim_offset, vertex_stride, param_mask, addr lo/hi, groups. gpu_mesh_indirect.task payload enlarged to 9.

## Files touched
ShaderIR.h, SpirvEmitter.h/.cpp, spirvEmitterModule.cpp, spirvEmitterMesh.cpp (EmitMeshComputeEntryPoint),
host_gpu/shaders/gpu_mesh_fallback.vert (+ CMakeLists.txt), gpu_mesh_indirect.task,
pipeline/pipelineCache.cpp/.h (limits, GetMeshComputePipeline, profile), pipeline/shaders.cpp (graphics pipeline uses the VS),
pipeline/shaderResourceBarrier.cpp/.h (NativeShaderStage(Mesh)->Compute in fallback), render.h + renderDraw.cpp (dispatch + draw).

## Known gaps / ideas
- Clip/cull distances and user clip planes are not exported (no clipping against them).
- Indirect mesh draws use the CPU path (no task shader in fallback). Primitive restart ignored.
- One render-pass break per fallback draw (EndRendering before the compute dispatch): slow.
- Output buffer fixed at 192 MiB; larger draws are dropped with a warning. Reuse relies on a barrier before each dispatch.
- Possible failure points: SPIR-V validation of the compute-model mesh module (look for "validation" errors in the log),
  requiredSubgroupSize vs emitter lane_count, descriptor/push-constant mismatch between the compute pipeline and the graphics pipeline.
- User must delete the `_PipelineCache` folder next to the exe before testing a new build.

## Session 2 changes (perf pass, untested)
- Output buffer is now a ring (RenderExecutor::m_mesh_fallback_offset): each fallback draw gets its own 256-byte aligned region,
  and the VS|CS -> CS full barrier before the dispatch only happens when the ring wraps. Before, every draw drained the vertex
  stage of the previous draw before its compute pass could start.
- Compute entry point stores each vec4 / primitive record with ONE 16-byte store (was 4 dword stores, each with 64-bit address math).
- Diagnostics: unsupported-limit drops are logged once per shader hash with the numbers (threads, shared memory vs device limit,
  max_vertices/primitives, lds); too-large drops are counted and logged (#1-3, then every 1000th).
- Facts worth knowing: mesh.max_primitives = primitives_per_group * GE_NGG_SUBGRP_CNTL.PRIM_AMP_FACTOR, so it is usually == primitives_per_group
  (the VS draw is tight in the common case). RDNA1 reports maxComputeSharedMemorySize = 32 KiB, which PS5 NGG LDS requests can exceed
  (those draws are dropped = missing geometry); check the new warning in the log.

## Next ideas, in order of expected payoff
1. Read the log: count of "MeshFallback draw" lines vs frame time; run once with KYTY_SKIP_MESH=1 to see the fps without mesh draws.
2. Remove the per-draw render pass break: hoist compute dispatches of consecutive mesh draws, or run them before the pass when
   their inputs are not written earlier in the same pass.
3. Emit a compacted index buffer + indirect draw from the compute pass so culled triangles cost nothing in the vertex pass.
4. Export clip/cull distances; handle primitive restart; add an indirect-draw path.
