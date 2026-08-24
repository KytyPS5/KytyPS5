#include "graphics/shader/shaderMergedGeometry.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "MergedGeometryTests: failed: %s\n", text);
    std::abort();
  }
}

// SOP1: [31:23] opcode class, [22:16] SDST, [15:8] OP, [7:0] SSRC0.
// s_setpc_b64 s[6:7] is OP 0x20 reading s6, which is how every merged ES half ends.
constexpr uint32_t SetPcB64S6 = 0xBE800000u | (0x20u << 8u) | 6u;
// SOPP: s_endpgm is OP 1.
constexpr uint32_t EndPgm = 0xBF810000u | (1u << 16u);
// s_nop, used here only as filler that decodes cleanly.
constexpr uint32_t Nop = 0xBF800000u;

void JoinsAtTheEsExit() {
  const std::vector<uint32_t> es {Nop, Nop, SetPcB64S6, Nop, Nop}; // trailing words are padding
  const std::vector<uint32_t> gs {Nop, EndPgm};

  Libs::Graphics::MergedGeometryProgram merged;
  std::string error;
  // Sequenced deliberately: building the message inside the Check call would read `error` before
  // the call that fills it, since argument evaluation order is unspecified.
  const bool assembled = Libs::Graphics::ShaderAssembleMergedGeometry(es, gs, merged, &error);
  const std::string message = "assembly succeeds: " + error;
  Check(assembled, message.c_str());

  // The ES contributes everything before its setpc, and nothing after it.
  Check(merged.gs_word_offset == 2, "the GS half starts where the ES setpc was");
  Check(merged.code.size() == 2 + gs.size(), "padding after the setpc is dropped");
  Check(merged.code[0] == Nop && merged.code[1] == Nop, "the ES body is preserved");
  Check(merged.code[2] == gs[0] && merged.code[3] == gs[1], "the GS body follows immediately");

  // The setpc itself must not survive: it would jump to launch state we no longer honour.
  for (const auto word : merged.code) {
    Check(word != SetPcB64S6, "the setpc is dropped rather than carried into the merged program");
  }
}

void RejectsAnEsWithoutTheMergedExit() {
  const std::vector<uint32_t> es {Nop, EndPgm};
  const std::vector<uint32_t> gs {Nop, EndPgm};

  Libs::Graphics::MergedGeometryProgram merged;
  std::string error;
  Check(!Libs::Graphics::ShaderAssembleMergedGeometry(es, gs, merged, &error),
        "an ES that does not end in s_setpc_b64 is rejected");
  Check(!error.empty(), "the rejection explains itself");
}

void RejectsEmptyHalves() {
  Libs::Graphics::MergedGeometryProgram merged;
  std::string error;
  const std::vector<uint32_t> code {Nop, SetPcB64S6};
  Check(!Libs::Graphics::ShaderAssembleMergedGeometry({}, code, merged, &error),
        "a missing ES is rejected");
  Check(!Libs::Graphics::ShaderAssembleMergedGeometry(code, {}, merged, &error),
        "a missing GS is rejected");
}

// The register values Stray and Silent Hill actually emit: both group sizes 0x40, and
// GE_MAX_OUTPUT_PER_SUBGROUP 0xc0. Triangles, so three vertices per primitive.
constexpr uint32_t GroupSize = 0x40;
constexpr uint32_t MaxOut = 0xc0;
constexpr uint32_t TriVerts = 3;

Libs::Graphics::MeshDispatch DispatchFor(uint32_t index_count) {
  Libs::Graphics::MeshDispatch dispatch;
  std::string error;
  const uint32_t primitives = Libs::Graphics::MeshPrimitiveCount(
      Libs::Graphics::MeshInputTopology::TriangleList, index_count);
  const bool ok = Libs::Graphics::ShaderComputeMeshDispatch(
      primitives, TriVerts, TriVerts, GroupSize, GroupSize, MaxOut, dispatch, &error);
  const std::string message = "dispatch computed: " + error;
  Check(ok, message.c_str());
  return dispatch;
}

void AWorkgroupNeverOverrunsAWave() {
  const auto dispatch = DispatchFor(3 * 1000);

  // The vertex bound is what binds: 64 primitives would need 192 input vertices, but only 64
  // lanes exist to hold them, so 21 primitives at three vertices each is the most that fits.
  Check(dispatch.primitives_per_workgroup == 21, "21 primitives fit one workgroup");
  Check(dispatch.vertices_per_workgroup == 63, "which is 63 vertices");
  Check(dispatch.vertices_per_workgroup <= Libs::Graphics::MeshWaveLanes,
        "vertices never exceed one lane each");
  Check(dispatch.primitives_per_workgroup <= Libs::Graphics::MeshWaveLanes,
        "primitives never exceed one lane each");
}

void EveryPrimitiveIsCoveredExactlyOnce() {
  // A partial final workgroup is the normal case, so the split has to account for it exactly.
  for (uint32_t primitives = 1; primitives <= 200; primitives++) {
    const auto dispatch = DispatchFor(primitives * TriVerts);
    const uint32_t covered =
        (dispatch.workgroup_count - 1) * dispatch.primitives_per_workgroup +
        dispatch.last_primitives;
    Check(covered == primitives, "the workgroups cover every primitive exactly once");
    Check(dispatch.last_primitives >= 1 &&
              dispatch.last_primitives <= dispatch.primitives_per_workgroup,
          "the final workgroup is non-empty and no larger than a full one");
    Check(dispatch.last_vertices == dispatch.last_primitives * TriVerts,
          "the final vertex count matches its primitive count");
  }
}

void AnExactMultipleHasAFullFinalWorkgroup() {
  const auto dispatch = DispatchFor(21 * TriVerts * 4);
  Check(dispatch.workgroup_count == 4, "four full workgroups");
  Check(dispatch.last_primitives == dispatch.primitives_per_workgroup,
        "the final workgroup is full, not empty");
}

void AnEmptyDrawDispatchesNothing() {
  const auto dispatch = DispatchFor(0);
  Check(dispatch.workgroup_count == 0, "an empty draw needs no workgroups");
}

void ImpossibleConfigurationsAreRejected() {
  Libs::Graphics::MeshDispatch dispatch;
  std::string error;
  // A primitive wider than the group can hold has no valid split.
  Check(!Libs::Graphics::ShaderComputeMeshDispatch(100, 100, 100, GroupSize, GroupSize, MaxOut,
                                                   dispatch, &error),
        "a primitive too wide for a workgroup is rejected");
  Check(!Libs::Graphics::ShaderComputeMeshDispatch(100, 0, TriVerts, GroupSize, GroupSize, MaxOut,
                                                   dispatch, &error),
        "a primitive with no vertices is rejected");
  Check(!Libs::Graphics::ShaderComputeMeshDispatch(100, TriVerts, 0, GroupSize, GroupSize, MaxOut,
                                                   dispatch, &error),
        "a primitive that emits no vertices is rejected");
  Check(!Libs::Graphics::ShaderComputeMeshDispatch(100, TriVerts, TriVerts, 0, GroupSize, MaxOut,
                                                   dispatch, &error),
        "a zero group size is rejected");
}

void LdsIsSizedFromTheGranuleCount() {
  // Two real geometry shaders, each with its declared granule count and the highest LDS offset
  // it addresses, read from the same run so the pairing is real. A 128-dword granule covers both
  // with little to spare, which is what a compiler sizing LDS precisely looks like.
  struct Sample { uint32_t granules; uint32_t highest_offset; };
  constexpr Sample Samples[] = {{17, 7456}, {31, 15456}};

  for (const auto &sample : Samples) {
    const uint32_t bytes = Libs::Graphics::MeshLdsDwords(sample.granules) * 4;
    Check(bytes > sample.highest_offset, "the allocation covers what the shader addresses");
    // Half the granule would not reach the offsets these shaders use, which is what fixes the
    // granule at 128 dwords rather than something smaller.
    Check(bytes / 2 <= sample.highest_offset, "and a smaller granule would not");
  }

  Check(Libs::Graphics::MeshLdsDwords(17) * 4 == 8704, "17 granules is 8704 bytes");
  Check(Libs::Graphics::MeshLdsDwords(31) * 4 == 15872, "31 granules is 15872 bytes");
}

void BothRealGeometryShadersFitTheHost() {
  constexpr uint32_t MaxBytes = 28672; // maxMeshSharedMemorySize on the RTX 2000 Ada
  std::string error;
  for (const uint32_t granules : {17u, 31u}) {
    Check(Libs::Graphics::ShaderValidateMeshLds(Libs::Graphics::MeshLdsDwords(granules), MaxBytes,
                                                &error),
          "a real geometry shader's LDS fits a mesh workgroup");
  }
  // A doubled granule would push the larger shader past the host limit even though it only needs
  // about 15 KB - the reason the granule size has to be right rather than merely generous.
  Check(31u * 256u * 4u > MaxBytes, "an over-sized granule would reject a shader that fits");
}

void LdsBeyondTheHostLimitIsRejected() {
  constexpr uint32_t MaxBytes = 28672; // maxMeshSharedMemorySize on the RTX 2000 Ada
  std::string error;

  Check(Libs::Graphics::ShaderValidateMeshLds(Libs::Graphics::MeshLdsDwords(17), MaxBytes, &error),
        "Stray's 8704 bytes fit");
  Check(Libs::Graphics::ShaderValidateMeshLds(MaxBytes / 4, MaxBytes, &error),
        "exactly the limit fits");
  Check(!Libs::Graphics::ShaderValidateMeshLds(MaxBytes / 4 + 1, MaxBytes, &error),
        "one dword over the limit is rejected");
  Check(!error.empty(), "the rejection says how much was wanted and how much is allowed");
  Check(!Libs::Graphics::ShaderValidateMeshLds(1, 0, &error),
        "a host reporting no mesh shared memory is rejected");
}

// A store keeps its data register in the destination field of the encoding, and an LDS store
// leaves that field naming nothing at all. Both shapes appear in every merged geometry program,
// and reading the field as a written register is what hid the primitive connectivity these
// programs receive at launch.
void AStoreDoesNotWriteItsDestinationField() {
  // ds_write_b32 v3, v2 - addr in v3, data in v2, and the unused vdst field left at zero, which
  // names v0. Then a read of v0, which the hardware really does supply.
  constexpr uint32_t DsWriteWord0 = 0xD8000000u | (0x0du << 18u);
  constexpr uint32_t DsWriteWord1 = (2u << 8u) | 3u; // data0 = v2, addr = v3
  // v_mov_b32 v4, v0
  constexpr uint32_t VMovV4FromV0 = 0x7E000000u | (4u << 17u) | (1u << 9u) | 256u;

  const std::vector<uint32_t> code {DsWriteWord0, DsWriteWord1, VMovV4FromV0, EndPgm};

  Libs::Graphics::MergedGeometryLaunchState launch;
  std::string error;
  const bool ok = Libs::Graphics::ShaderAnalyzeMergedLaunchState(code, launch, &error);
  const std::string message = "the scan succeeds: " + error;
  Check(ok, message.c_str());

  Check(launch.vector[0], "v0 is launch state: the store's unused vdst field did not write it");
  Check(launch.vector[2], "the store's data register is a read");
  Check(launch.vector[3], "so is its address register");
  Check(!launch.vector[4], "a register the program writes is not launch state");
}

void ABufferStoreReadsItsDataRegister() {
  // buffer_store_dword v7, v1, s[4:7], 0 - vdata is v7, and it is data the store reads.
  // soffset 128 is the inline constant zero, so no SGPR read comes from it.
  constexpr uint32_t BufferStoreWord0 = 0xE0000000u | (0x1cu << 18u);
  constexpr uint32_t BufferStoreWord1 =
      (128u << 24u) | (1u << 16u) | (7u << 8u) | 1u; // srsrc s[4:7], vdata v7, vaddr v1
  const std::vector<uint32_t> code {BufferStoreWord0, BufferStoreWord1, EndPgm};

  Libs::Graphics::MergedGeometryLaunchState launch;
  std::string error;
  Check(Libs::Graphics::ShaderAnalyzeMergedLaunchState(code, launch, &error),
        "the scan succeeds");
  Check(launch.vector[7], "the stored data register is read, not written");
  Check(launch.vector[1], "the address register is read");
  // A buffer descriptor is four consecutive registers. Counting only the named one would leave
  // s5-s7 looking like registers the program never touches.
  Check(launch.scalar[4] && launch.scalar[5] && launch.scalar[6] && launch.scalar[7],
        "the whole four-register descriptor is read, not just its base");
  Check(!launch.scalar[8], "and not one register more");
}

// A strip shares vertices between consecutive primitives, so the same indices describe a different
// number of triangles than a list does. The split is driven by that count, not by the index count.
void AStripCountsItsPrimitivesDifferently() {
  using Libs::Graphics::MeshInputTopology;
  using Libs::Graphics::MeshPrimitiveCount;

  Check(MeshPrimitiveCount(MeshInputTopology::TriangleList, 18) == 6, "18 indices are 6 triangles");
  // The shape both Stray and Silent Hill draw: a four-vertex quad as two strip triangles.
  Check(MeshPrimitiveCount(MeshInputTopology::TriangleStrip, 4) == 2,
        "4 strip indices are 2 triangles");
  Check(MeshPrimitiveCount(MeshInputTopology::TriangleStrip, 3) == 1, "3 are the minimum strip");
  Check(MeshPrimitiveCount(MeshInputTopology::TriangleStrip, 2) == 0, "2 cannot form a triangle");
  Check(MeshPrimitiveCount(MeshInputTopology::TriangleList, 2) == 0, "nor can 2 as a list");

  // A strip still spends three ES lanes per primitive: the shared vertices are fetched twice
  // rather than deduplicated, which is what keeps the slot mapping a closed form.
  Libs::Graphics::MeshDispatch dispatch;
  std::string error;
  Check(Libs::Graphics::ShaderComputeMeshDispatch(
            MeshPrimitiveCount(MeshInputTopology::TriangleStrip, 4), TriVerts, TriVerts, GroupSize,
            GroupSize, MaxOut, dispatch, &error),
        "a four-index strip splits");
  Check(dispatch.workgroup_count == 1, "into one workgroup");
  Check(dispatch.last_primitives == 2 && dispatch.last_vertices == 6,
        "of two primitives over six slots");
}

// Silent Hill issues a GS with VGT_GS_MAX_VERT_OUT of 24 - eight triangles out of one in. The
// output arrays and the subgroup bound have to be sized by what it emits, not by what it reads;
// those are the same number only for the 1:1 triangle GS that hid the distinction.
void AnAmplifyingGsIsSizedByWhatItEmits() {
  constexpr uint32_t Amplified = 24;
  Libs::Graphics::MeshDispatch dispatch;
  std::string error;
  Check(Libs::Graphics::ShaderComputeMeshDispatch(100, TriVerts, Amplified, GroupSize, GroupSize,
                                                   MaxOut, dispatch, &error),
        "an amplifying GS splits");

  // GE_MAX_OUTPUT_PER_SUBGROUP counts emitted vertices: 192 of them at 24 each is 8 primitives.
  Check(dispatch.primitives_per_workgroup == MaxOut / Amplified,
        "the subgroup output bound divides by what a primitive emits");
  Check(dispatch.vertices_per_workgroup == dispatch.primitives_per_workgroup * TriVerts,
        "its input vertices still come three to a primitive");
  Check(dispatch.output_vertices_per_workgroup == dispatch.primitives_per_workgroup * Amplified,
        "and its output vertices come 24 to a primitive");
  // A GS emits a strip, so 24 vertices are 22 triangles.
  Check(dispatch.output_primitives_per_workgroup == dispatch.primitives_per_workgroup * 22,
        "24 emitted vertices are 22 triangles");

  // The 1:1 case is where the two counts coincide, which is worth pinning so a future change
  // cannot quietly reintroduce the conflation.
  Libs::Graphics::MeshDispatch plain;
  Check(Libs::Graphics::ShaderComputeMeshDispatch(100, TriVerts, TriVerts, GroupSize, GroupSize,
                                                   MaxOut, plain, &error),
        "a 1:1 GS splits");
  Check(plain.output_vertices_per_workgroup == plain.vertices_per_workgroup,
        "a 1:1 GS emits exactly the vertices it reads");
  Check(plain.output_primitives_per_workgroup == plain.primitives_per_workgroup,
        "and one triangle per primitive");
}

} // namespace

int main() {
  LdsIsSizedFromTheGranuleCount();
  BothRealGeometryShadersFitTheHost();
  LdsBeyondTheHostLimitIsRejected();
  JoinsAtTheEsExit();
  RejectsAnEsWithoutTheMergedExit();
  RejectsEmptyHalves();
  AWorkgroupNeverOverrunsAWave();
  EveryPrimitiveIsCoveredExactlyOnce();
  AnExactMultipleHasAFullFinalWorkgroup();
  AnEmptyDrawDispatchesNothing();
  ImpossibleConfigurationsAreRejected();
  AStoreDoesNotWriteItsDestinationField();
  ABufferStoreReadsItsDataRegister();
  AStripCountsItsPrimitivesDifferently();
  AnAmplifyingGsIsSizedByWhatItEmits();

  std::printf("MergedGeometryTests: ok\n");
  return 0;
}
