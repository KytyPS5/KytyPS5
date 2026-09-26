#ifndef GPU_TILER_ELEMENT_INC
#define GPU_TILER_ELEMENT_INC
// Picks a per-element-size constant (1, 2, 4, 8 or 16 bytes per element). Kept scalar on
// purpose: a vector ternary on the specialization constant is folded by glslang into
// OpSpecConstantOp Select with a scalar condition and vector operands, which some drivers
// (lavapipe among them) evaluate for the first component only.
uint element_select(uint b1, uint b2, uint b4, uint b8, uint b16) {
	return ELEMENT_BYTES == 1 ? b1 : ELEMENT_BYTES == 2 ? b2 : ELEMENT_BYTES == 4 ? b4 :
	       ELEMENT_BYTES == 8 ? b8 : b16;
}
#endif
