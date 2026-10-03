#include "gpu_tiler_standard4_3d.inc"
uvec3 standard64_3d_extent() {
	if (ELEMENT_BYTES == 1) return uvec3(64, 32, 32);
	if (ELEMENT_BYTES == 2) return uvec3(32, 32, 32);
	if (ELEMENT_BYTES == 4) return uvec3(32, 32, 16);
	if (ELEMENT_BYTES == 8) return uvec3(32, 16, 16);
	return uvec3(16, 16, 16);
}
uint standard64_3d_offset(uvec3 p) {
	// Explicit branches avoid vector OpSpecConstantOp Select.
	uvec4 s;
	if (ELEMENT_BYTES == 1) s = uvec4(4, 4, 4, 5);
	else if (ELEMENT_BYTES == 2) s = uvec4(3, 4, 4, 4);
	else if (ELEMENT_BYTES == 4) s = uvec4(3, 3, 4, 4);
	else if (ELEMENT_BYTES == 8) s = uvec4(3, 3, 3, 4);
	else s = uvec4(2, 3, 3, 3);
	return standard3d_offset(p) ^ (((p.x >> s.x) & 1u) << 12) ^
	       (((p.z >> s.y) & 1u) << 13) ^ (((p.y >> s.z) & 1u) << 14) ^
	       (((p.x >> s.w) & 1u) << 15);
}
