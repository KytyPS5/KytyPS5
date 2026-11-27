// stb_image's implementation, which the image store needs from whoever links it. The gallery
// compiles it here, once, as the emulator does in libJpegDec.cpp. This one keeps stb's defaults,
// while the emulator's turns SIMD off and allocates zeroed memory, so the gallery doesn't decode
// exactly as the emulator will.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
