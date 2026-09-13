#include "gpu_tiler_standard.inc"
uint standard64_offset(uvec2 p) {
	uint x = p.x, y = p.y;
	uint offset = standard_offset(p);
	switch (ELEMENT_BYTES) {
	case 1: return offset ^ ((x << 7) & 0x2000u) ^ ((x << 8) & 0x8000u) ^
	                       ((y << 6) & 0x1000u) ^ ((y << 7) & 0x4000u);
	case 2: return offset ^ ((x << 7) & 0x2000u) ^ ((x << 8) & 0x8000u) ^
	                       ((y << 7) & 0x1000u) ^ ((y << 8) & 0x4000u);
	case 4: return offset ^ ((x << 8) & 0x2000u) ^ ((x << 9) & 0x8000u) ^
	                       ((y << 7) & 0x1000u) ^ ((y << 8) & 0x4000u);
	case 8: return offset ^ ((x << 8) & 0x2000u) ^ ((x << 9) & 0x8000u) ^
	                       ((y << 8) & 0x1000u) ^ ((y << 9) & 0x4000u);
	default: return offset ^ ((x << 9) & 0x2000u) ^ ((x << 10) & 0x8000u) ^
	                        ((y << 8) & 0x1000u) ^ ((y << 9) & 0x4000u);
	}
}
