#include "graphics/host_gpu/renderer/perVertexTransform.h"

#include <cstdio>
#include <cstdlib>

int main() {
    // Exercise line anchors, regex iteration over adjacent declarations, and
    // searches away from the start, including an unterminated final line.
    for (const std::string newline : {"\n", "\r\n"}) {
        const std::string vs = "OpDecorate %param0 Location 0" + newline +
            "OpDecorate %param3 Location 3" + newline +
            "%param0 = OpVariable %ptr Output" + newline +
            "%param3 = OpVariable %ptr Output" + newline +
            "%gl_ClipDistance = OpVariable %clip_ptr Output";
        const std::string ps = "OpDecorate %in0 PerVertexKHR" + newline +
            "OpDecorate %in3 PerVertexKHR" + newline +
            "OpDecorate %in0 Location 0" + newline +
            "OpDecorate %in3 Location 3";
        Libs::Graphics::PerVertexLayout layout;
        std::map<uint32_t, std::string> params;
        const std::map<uint32_t, std::string> expected_params = {{0, "%param0"}, {3, "%param3"}};
        const std::map<uint32_t, uint32_t> expected_slots = {{0, 1}, {3, 2}};
        if (!Libs::Graphics::DerivePerVertexLayout(vs, ps, layout, params) ||
            params != expected_params || layout.location_to_slot != expected_slots ||
            layout.num_params != 2 || !layout.has_clip || layout.clip_slot != 3 ||
            layout.record_stride_vec4 != 4) {
            std::fprintf(stderr, "FAIL: multiline PerVertex layout derivation\n");
            return EXIT_FAILURE;
        }
        const std::string missing_location = ps + newline + "OpDecorate %missing PerVertexKHR";
        if (Libs::Graphics::DerivePerVertexLayout(vs, missing_location, layout, params)) {
            std::fprintf(stderr, "FAIL: missing location on final line was accepted\n");
            return EXIT_FAILURE;
        }
    }
    std::puts("PASS: multiline PerVertex parsing (LF/CRLF, adjacent and final lines)");
    return EXIT_SUCCESS;
}
