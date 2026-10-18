#include "gpu_tiler_standard4_3d.inc"
uvec3 standard64_3d_extent() {
	if (ELEMENT_BYTES == 1) return uvec3(64, 32, 32);
	if (ELEMENT_BYTES == 2) return uvec3(32, 32, 32);
	if (ELEMENT_BYTES == 4) return uvec3(32, 32, 16);
	if (ELEMENT_BYTES == 8) return uvec3(32, 16, 16);
	return uvec3(16, 16, 16);
}
uint standard64_3d_offset(uvec3 p) {
	// Keep specialization selects scalar so vector constant folding preserves the shifts.
	uvec4 s = uvec4(ELEMENT_BYTES == 1 ? 4u : ELEMENT_BYTES == 16 ? 2u : 3u,
	                ELEMENT_BYTES <= 2 ? 4u : 3u,
	                ELEMENT_BYTES <= 4 ? 4u : 3u,
	                ELEMENT_BYTES == 1 ? 5u : ELEMENT_BYTES == 16 ? 3u : 4u);
	return standard3d_offset(p) ^ (((p.x >> s.x) & 1u) << 12) ^
	       (((p.z >> s.y) & 1u) << 13) ^ (((p.y >> s.z) & 1u) << 14) ^
	       (((p.x >> s.w) & 1u) << 15);
}
