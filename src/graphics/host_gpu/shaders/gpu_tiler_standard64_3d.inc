#include "gpu_tiler_standard4_3d.inc"
uvec3 standard64_3d_extent() {
	if (ELEMENT_BYTES == 1) return uvec3(64, 32, 32);
	if (ELEMENT_BYTES == 2) return uvec3(32, 32, 32);
	if (ELEMENT_BYTES == 4) return uvec3(32, 32, 16);
	if (ELEMENT_BYTES == 8) return uvec3(32, 16, 16);
	return uvec3(16, 16, 16);
}
// Early returns rather than a ternary chain: a specialization-constant select between uvec4
// values is miscompiled by some drivers (observed on RADV, Mesa 26.2), see GpuTilerCpuParity.
uvec4 standard64_3d_shifts() {
	if (ELEMENT_BYTES == 1) return uvec4(4, 4, 4, 5);
	if (ELEMENT_BYTES == 2) return uvec4(3, 4, 4, 4);
	if (ELEMENT_BYTES == 4) return uvec4(3, 3, 4, 4);
	if (ELEMENT_BYTES == 8) return uvec4(3, 3, 3, 4);
	return uvec4(2, 3, 3, 3);
}
uint standard64_3d_offset(uvec3 p) {
	uvec4 s = standard64_3d_shifts();
	return standard3d_offset(p) ^ (((p.x >> s.x) & 1u) << 12) ^
	       (((p.z >> s.y) & 1u) << 13) ^ (((p.y >> s.z) & 1u) << 14) ^
	       (((p.x >> s.w) & 1u) << 15);
}
