#include "gpu_tiler_standard4_3d.inc"
uvec3 standard64_3d_extent() {
	if (ELEMENT_BYTES == 1) return uvec3(64, 32, 32);
	if (ELEMENT_BYTES == 2) return uvec3(32, 32, 32);
	if (ELEMENT_BYTES == 4) return uvec3(32, 32, 16);
	if (ELEMENT_BYTES == 8) return uvec3(32, 16, 16);
	return uvec3(16, 16, 16);
}
// Each element size spells out its shifts. Choosing between vector constants on a specialization
// constant compiles to a vector OpSpecConstantOp select, which crashes AMD's Windows compiler.
uint standard64_3d_bits(uvec3 p, uint x0, uint z0, uint y0, uint x1) {
	return (((p.x >> x0) & 1u) << 12) ^ (((p.z >> z0) & 1u) << 13) ^
	       (((p.y >> y0) & 1u) << 14) ^ (((p.x >> x1) & 1u) << 15);
}
uint standard64_3d_offset(uvec3 p) {
	uint offset = standard3d_offset(p);
	if (ELEMENT_BYTES == 1) return offset ^ standard64_3d_bits(p, 4u, 4u, 4u, 5u);
	if (ELEMENT_BYTES == 2) return offset ^ standard64_3d_bits(p, 3u, 4u, 4u, 4u);
	if (ELEMENT_BYTES == 4) return offset ^ standard64_3d_bits(p, 3u, 3u, 4u, 4u);
	if (ELEMENT_BYTES == 8) return offset ^ standard64_3d_bits(p, 3u, 3u, 3u, 4u);
	return offset ^ standard64_3d_bits(p, 2u, 3u, 3u, 3u);
}
