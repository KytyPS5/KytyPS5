#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_PSRSHADERS_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_PSRSHADERS_H_

#include <cstdint>
#include <span>

namespace Libs::Graphics::Psr {

// sce::Psr is Sony's ray tracing BVH library, linked statically into the games that use it (GTA
// V, Astro Bot, Marvel's Wolverine). It builds and refits BVHs on the GPU with its own compute
// shaders, dispatched like any other compute work (there is nothing to intercept on the CPU), and
// registers each shader by name with the driver (owner "sce::Psr", see
// sceAgcDriverRegisterResource). The emulator recognizes them by that name, not by the hash of
// their code, which differs from game to game, and reads their user data with the layout of the
// library's generation. When the library has not registered psr_ names (GTA V 01.005 registers
// them without the prefix, ASTRO BOT registers none), they are recognized by their code
// (CodeFingerprint()), for the builds of the library whose shaders the emulator knows.
//
// Only the shaders the emulator replaces or watches are listed.
enum class Shader : uint8_t {
	None,

	// Top level build (kBase format), in dispatch order. Some of them (scans, sorts) serve other
	// builds too. Generation Base252 has no indirect arguments, valid instance indices or vec2u
	// scans, and its last pass (CollapseToQbvhSimpleTop) is also the last pass of its BLAS builds.
	MortonComputeBoundingBoxForInstances,
	MortonInitializeIndirectArgsTop,
	SinglePassScanInitializeDescriptors,
	SinglePassScanUint32,
	MortonComputeValidInstanceIndices,
	MortonSetupIndirectArgsTop,
	// Only in builds of one or two instances: their root node, instead of the sort and the passes
	// that build and rotate the hierarchy.
	MortonInitOneInstanceRootNode,
	MortonComputeCodeForInstances,
	BitonicSortPassKeyValueIndirect,
	MortonBuildQbvhHierarchyForInstances,
	MortonBuildInstanceNode,
	MortonRefitQbvhHierarchyForInstances,
	MortonRotateTreeFirstRunTop,
	MortonRotateTreeTop,
	CollapseToQbvhSimpleComputeCompactedNodesIndirect,
	SinglePassScanSetupIndirectArgsVec2u,
	SinglePassScanInitializeDescriptorsVec2uIndirect,
	SinglePassScanVec2uIndirect,
	CollapseToQbvhSimpleTop,

	// Refit of a bottom level BVH built for triangle fan refits, in dispatch order (the second one
	// once per geometry).
	QbvhFanRefitCopyDispatchArgs,
	QbvhFanRefitAsync,
	QbvhFanRefitRecoverAabbs,
	QbvhRefitBottomLevel,

	// Bottom level build of triangle geometry (kBase format): the bounds of a geometry's vertices
	// and the Morton codes of its triangles (once per geometry; the codes in any index format and
	// precision, and not in builds that split triangles) and the last pass, which writes the BLAS
	// (once per BLAS, with or without refit data).
	MortonComputeBoundingBoxForVertices,
	MortonComputeCodeForMesh,
	CollapseToQbvhSimpleBottom,

	// The first pass of every build (the memory it may use: the BVH or the scratch memory, and
	// their sizes).
	MortonCaptureMemory,
	// Decompression of a compressed bottom level BVH (kBase format): the header (the first pass)
	// and the triangle nodes (the last ones, any index format).
	DecompressHeaderBase,
	DecompressComputeFanNode,

	// Any other shader of the library (generation known): a pass of a build or refit the emulator
	// does not replace or watch.
	Other,
	// A shader of the library's kTrinity builds (the BVH format of the PS5 Pro), which neither the
	// replacements nor the native traversal produce.
	Trinity,

	Count
};

// The generations of the library whose user data layouts the emulator knows.
enum class Generation : uint8_t {
	Unknown,
	// 460 shaders: GTA V 01.010/01.011 (build J03180688), Astro Bot (J03093718) and Nightreign.
	Base460,
	// 252 shaders, named without the psr_ prefix: GTA V 01.005 (build J02232310).
	Base252,
};

// The generation of the library the game registered (Unknown until it registered its shaders,
// and for generations whose layouts are not known).
[[nodiscard]] Generation LibraryGeneration();

// The Psr shader whose code starts at `code_address`, or None (not a Psr shader, a shader the
// emulator does not list, or a library of an unknown generation).
[[nodiscard]] Shader IdentifyShader(uint64_t code_address);

// A fingerprint of a shader's code (`binary`, from its first instruction) that does not depend on
// where the code is, on the registers the compiler allocated or on the order it scheduled the
// instructions in: the sorted multiset of its instructions up to S_ENDPGM, each reduced to its
// opcode, the kinds of its operands, its constants (not those added to the program counter) and
// its memory offsets and formats. 0 when the code does not end with S_ENDPGM.
[[nodiscard]] uint64_t CodeFingerprint(std::span<const uint32_t> binary);

// The Psr shader whose code is `binary` (the whole binary, code to footer), recognized by its
// fingerprint among the shaders of the builds of the library the emulator knows: the generation
// and the shader (Other for the library's shaders it does not list), or Unknown and None.
struct CodeMatch {
	Generation generation = Generation::Unknown;
	Shader     shader     = Shader::None;
};
[[nodiscard]] CodeMatch IdentifyCode(std::span<const uint32_t> binary);

// Whether `shader` belongs to the top level (kBase) build.
[[nodiscard]] bool IsTopLevelBuildShader(Shader shader);

// Whether the emulator may still replace the library's work with its own (the TLAS builds and
// refits of CommandProcessor::HleTlasDispatch() and HleBlasDispatch()) and follow its BLAS
// builds. A game that builds kTrinity BVHs, or whose builds run passes in an order the
// replacements do not expect, turns that off for the rest of the session (StopReplacement()): its
// builds are emulated as they are.
[[nodiscard]] bool ReplacementEnabled();
void               StopReplacement(const char* reason, const char* shader_name);

} // namespace Libs::Graphics::Psr

#endif // EMULATOR_SRC_GRAPHICS_GUEST_GPU_PSRSHADERS_H_
