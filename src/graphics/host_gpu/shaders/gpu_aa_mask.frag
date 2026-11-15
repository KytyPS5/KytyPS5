#version 450 core

// A draw without a pixel shader under a PA_SC_AA_MASK that leaves quad pixels uncovered.
layout(push_constant) uniform Push {
	// PushData::AaMaskDword: bit (y & 1) * 2 + (x & 1) keeps that pixel of a quad.
	layout(offset = 124) uint quad_mask;
};

void main() {
	const ivec2 pixel = ivec2(gl_FragCoord.xy) & 1;
	if ((quad_mask >> (pixel.y * 2 + pixel.x) & 1u) == 0u) {
		discard;
	}
}
