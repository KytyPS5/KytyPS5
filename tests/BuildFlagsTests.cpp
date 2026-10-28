#include <cstdio>

#if !defined(__AVX2__) || !defined(__BMI2__) || !defined(__FMA__)
#error "x86-64 builds target x86-64-v3"
#endif

int main() {
  // (1 + 2^-23)^2 = 1 + 2^-22 + 2^-46; only a fused multiply-add keeps the 2^-46.
  volatile float a = 1.0f + 0x1p-23f;
  const float result = a * a - (1.0f + 0x1p-22f);
  if (result != 0.0f) {
    std::fprintf(stderr, "BuildFlagsTests: failed: a * a - b was contracted (%a)\n", result);
    return 1;
  }
  std::puts("BuildFlagsTests: all cases passed");
  return 0;
}
