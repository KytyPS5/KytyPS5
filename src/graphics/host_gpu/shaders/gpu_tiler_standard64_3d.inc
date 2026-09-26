#include "gpu_tiler_element.inc"
#include "gpu_tiler_standard4_3d.inc"
uvec3 standard64_3d_extent() {
	if (ELEMENT_BYTES == 1) return uvec3(64, 32, 32);
	if (ELEMENT_BYTES == 2) return uvec3(32, 32, 32);
	if (ELEMENT_BYTES == 4) return uvec3(32, 32, 16);
	if (ELEMENT_BYTES == 8) return uvec3(32, 16, 16);
	return uvec3(16, 16, 16);
}
uint standard64_3d_offset(uvec3 p) {
	// Source bit of x/z/y/x for offset bits 12..15, per element size 1, 2, 4, 8, 16.
	uint sx = element_select(4, 3, 3, 3, 2);
	uint sz = element_select(4, 4, 3, 3, 3);
	uint sy = element_select(4, 4, 4, 3, 3);
	uint sw = element_select(5, 4, 4, 4, 3);
	return standard3d_offset(p) ^ (((p.x >> sx) & 1u) << 12) ^
	       (((p.z >> sz) & 1u) << 13) ^ (((p.y >> sy) & 1u) << 14) ^
	       (((p.x >> sw) & 1u) << 15);
}
