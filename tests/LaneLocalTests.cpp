// The wave64 lane-locality proof (IsWaveLaneLocal). Without arguments: small RDNA2 programs with the expected verdict
// (the patterns a half running alone gets wrong). With files (KYTY_SHADER_DUMP=<dir> writes CS_<hash>.bin): prints
// "yes" or the refusal for each (-d first: and the decoded program).
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/frontend/translate/Translate.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace Libs::Graphics::ShaderRecompiler;

// "yes" or the reason.
std::string Classify(std::span<const uint32_t> code) {
	Decoder::Program decoded;
	Decoder::DecodeProgram(code, decoded);
	std::string reason;
	return Frontend::IsWaveLaneLocal(decoded, &reason) ? "yes" : reason;
}

struct Case {
	const char*           name;
	const char*           refusal; // (empty: lane-local)
	std::vector<uint32_t> code;
};

// EXEC narrowed first by v_cmpx_ne_u32 exec, 0, v0 unless said otherwise; L: the branch target.
const Case Cases[] = {
    // s_cbranch_execz L; s_add_u32 s5, s5, 1; L: s_mov_b64 exec, -1; v_mov_b32 v1, s5
    {"escape_s5", "scalar state escapes", {0x7daa0080, 0xbf880001, 0x80058105, 0xbefe04c1, 0x7e020205, 0xbf810000}},
    // the same, reading s6 after the merge
    {"noescape", "", {0x7daa0080, 0xbf880001, 0x80058105, 0xbefe04c1, 0x7e020206, 0xbf810000}},
    // v_cmp_ne_u32 vcc, 0, v0; s_cbranch_vccz L; v_mov_b32 v1, 1; L:
    {"vcc_lane", "a branch on VCC", {0x7d8a0080, 0xbf860001, 0x7e020281, 0xbf810000}},
    // v_mov_b32 v2, s4; v_cmp_ne_u32 vcc, 0, v2; s_cbranch_vccz L; v_mov_b32 v1, 1; L:
    {"vcc_uniform", "", {0x7e040204, 0x7d8a0480, 0xbf860001, 0x7e020281, 0xbf810000}},
    // s_mov_b64 s[8:9], exec; ...; s_cbranch_execz L; s_mov_b64 exec, s[8:9]; v_mov_b32 v1, 1; L: s_mov_b64 exec, s[8:9]
    {"exec_widen", "EXEC gaining", {0xbe88047e, 0x7daa0080, 0xbf880002, 0xbefe0408, 0x7e020281, 0xbefe0408, 0xbf810000}},
    // v_mov_b32 v2, s4; ...; v_readfirstlane_b32 s5, v2; s_mov_b64 exec, -1; v_mov_b32 v1, s5
    {"rfl_empty", "V_READFIRSTLANE where", {0x7e040204, 0x7daa0080, 0x7e0a0502, 0xbefe04c1, 0x7e020205, 0xbf810000}},
    // the same behind s_cbranch_execz L (s5 read before the merge only)
    {"rfl_skip", "", {0x7e040204, 0x7daa0080, 0xbf880002, 0x7e0a0502, 0x7e020205, 0xbefe04c1, 0xbf810000}},
    // v_cmp_ne_u32 vcc, 0, v0; s_and_b64 s[6:7], exec, vcc; s_cselect_b64 s[8:9], exec, 0
    {"scc_mask", "SCC made from", {0x7d8a0080, 0x87866a7e, 0x8588807e, 0xbf810000}},
    // the same with s_mov_b32 s5, s4 (SCC kept) before the select
    {"scc_kept", "SCC made from", {0x7d8a0080, 0x87866a7e, 0xbe850304, 0x8588807e, 0xbf810000}},
    // s_mov_b64 s[10:11], 0; L: v_cmp_ne_u32 vcc, 0, v0; s_or_b64 s[10:11], s[10:11], vcc;
    // s_andn2_b64 exec, exec, vcc; s_cbranch_execnz L; s_or_b64 exec, exec, s[10:11]; v_mov_b32 v1, 1
    {"loop_acc", "", {0xbe8a0480, 0x7d8a0080, 0x888a6a0a, 0x8afe6a7e, 0xbf89fffc, 0x88fe0a7e, 0x7e020281, 0xbf810000}},
    // s_mov_b64 s[4:5], 0; L: s_add_u32 s5, s5, 1; v_cmp_ne_u32 vcc, 0, v0; s_andn2_b64 exec, exec, vcc;
    // s_cbranch_execnz L; s_mov_b64 exec, -1; v_mov_b32 v1, s5
    {"loop_count",
     "scalar state escapes",
     {0xbe840480, 0x80058105, 0x7d8a0080, 0x8afe6a7e, 0xbf89fffc, 0xbefe04c1, 0x7e020205, 0xbf810000}},
    // s_cbranch_execz L; s_barrier; L: s_mov_b64 exec, -1
    {"barrier", "a barrier", {0x7daa0080, 0xbf880001, 0xbf8a0000, 0xbefe04c1, 0xbf810000}},
    // v_cmp_ne_u32 vcc, 0, v1; s_and_saveexec_b64 s[8:9], vcc; s_cbranch_execz L; v_cmp_ne_u32 vcc, 0, v0;
    // L: s_mov_b64 exec, s[8:9]; s_and_b64 exec, exec, vcc; v_mov_b32 v1, 1
    {"vcc_after",
     "scalar state escapes",
     {0x7d8a0280, 0xbe88246a, 0xbf880001, 0x7d8a0080, 0xbefe0408, 0x87fe6a7e, 0x7e020281, 0xbf810000}},
    // (Without v_cmpx.) s_mov_b32 exec_hi, 0
    {"exec_half", "EXEC set from the wave's layout", {0xbeff0380, 0xbf810000}},
    // s_mov_b64 s[4:5], 0xffff; s_and_b64 exec, exec, s[4:5]
    {"literal_mask", "EXEC set from the wave's layout", {0xbe8404ff, 0x0000ffff, 0x87fe047e, 0xbf810000}},
    // v_cmp_ne_u32 vcc, 0, v0; s_mov_b32 s4, vcc_lo; s_mov_b32 s5, vcc_hi; s_and_b64 exec, exec, s[4:5]
    {"mask_half", "EXEC set from the wave's layout", {0x7d8a0080, 0xbe84036a, 0xbe85036b, 0x87fe047e, 0xbf810000}},
    // s_mov_b64 s[4:5], 0xffff; s_and_saveexec_b64 s[8:9], s[4:5]
    {"saveexec_loaded", "EXEC set from the wave's layout", {0xbe8404ff, 0x0000ffff, 0xbe882404, 0xbf810000}},
    // v_cmp_ne_u32 vcc, 0, v0; s_and_b64 vcc, s[0:1], vcc (a pointer's bits: unused)
    {"layout_dead", "", {0x7d8a0080, 0x87ea6a00, 0xbf810000}},
    // the same; v_cndmask_b32 v1, 0, v2 (selected by it)
    {"layout_select", "a lane select from the wave's layout", {0x7d8a0080, 0x87ea6a00, 0x02020480, 0xbf810000}},
    // (Without v_cmpx.) v_cmp_ne_u32 vcc, 0, v0; s_mov_b64 s[10:11], vcc; buffer_load_dword v1, v2, s[8:11], 0 idxen
    // (a mask as the V#'s third word)
    {"vsharp_mask", "a lane mask used as an address", {0x7d8a0080, 0xbe8a046a, 0xe0302000, 0x80020102, 0xbf810000}},
    // s_mov_b64 s[0:1], exec; image_load v1, v[2:3], s[4:11], s[0:3] dmask:0x1 dim:2d (no sampler read)
    {"sampler_unused", "", {0xbe80047e, 0xf0000108, 0x00010102, 0xbf810000}},
    // v_mov_b32 v1, v0; v_mov_b32 v2, s5; v_add_f16 v1, s4, v2 (v1's high half stays); v_readfirstlane_b32 s6, v1
    {"half16", "V_READFIRSTLANE of a lane value", {0x7e020300, 0x7e040205, 0x64020404, 0x7e0c0501, 0xbf810000}},
    // the same with v_add_f32
    {"half32", "", {0x7e020300, 0x7e040205, 0x06020404, 0x7e0c0501, 0xbf810000}},
};

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		int failed = 0;
		for (const auto& test: Cases) {
			const auto verdict = Classify(test.code);
			const bool pass    = *test.refusal == '\0' ? verdict == "yes" : verdict.find(test.refusal) != std::string::npos;
			if (!pass) {
				std::printf("FAIL %s: %s\n", test.name, verdict.c_str());
				++failed;
			}
		}
		std::printf("%d of %d lane-local cases passed\n", static_cast<int>(std::size(Cases)) - failed,
		            static_cast<int>(std::size(Cases)));
		return failed == 0 ? 0 : 1;
	}
	// (-d first: the decoded programs too.)
	const bool dump  = std::string_view(argv[1]) == "-d";
	const int  first = dump ? 2 : 1;
	int        yes   = 0;
	for (int a = first; a < argc; ++a) {
		std::ifstream         file(argv[a], std::ios::binary);
		std::vector<char>     bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		std::vector<uint32_t> code(bytes.size() / 4);
		std::memcpy(code.data(), bytes.data(), code.size() * 4);
		try {
			const auto verdict = Classify(code);
			yes += verdict == "yes" ? 1 : 0;
			std::printf("%s: %s\n", argv[a], verdict.c_str());
			if (dump) {
				Decoder::Program decoded;
				Decoder::DecodeProgram(code, decoded);
				std::printf("%s", Decoder::ProgramToString(decoded).c_str());
			}
		} catch (const std::exception& error) {
			std::printf("%s: error %s\n", argv[a], error.what());
		}
	}
	std::printf("%d of %d lane-local\n", yes, argc - first);
	return 0;
}
