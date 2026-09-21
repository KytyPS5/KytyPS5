#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_VACONTEXT_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_VACONTEXT_H_

#include <cstddef>
#include <bit>

#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

#if defined(__x86_64__)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define VA_ARGS                                                                                    \
	uint64_t rdi, uint64_t rsi, uint64_t rdx, uint64_t rcx, uint64_t r8, uint64_t r9,              \
	    uint64_t overflow_arg_area, __m128 xmm0, __m128 xmm1, __m128 xmm2, __m128 xmm3,            \
	    __m128 xmm4, __m128 xmm5, __m128 xmm6, __m128 xmm7, ...

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define VA_CONTEXT(ctx)                                                                            \
	alignas(16) VaContext ctx;                                                                     \
	(ctx).reg_save_area.gp[0]       = rdi;                                                         \
	(ctx).reg_save_area.gp[1]       = rsi;                                                         \
	(ctx).reg_save_area.gp[2]       = rdx;                                                         \
	(ctx).reg_save_area.gp[3]       = rcx;                                                         \
	(ctx).reg_save_area.gp[4]       = r8;                                                          \
	(ctx).reg_save_area.gp[5]       = r9;                                                          \
	(ctx).reg_save_area.fp[0]       = xmm0;                                                        \
	(ctx).reg_save_area.fp[1]       = xmm1;                                                        \
	(ctx).reg_save_area.fp[2]       = xmm2;                                                        \
	(ctx).reg_save_area.fp[3]       = xmm3;                                                        \
	(ctx).reg_save_area.fp[4]       = xmm4;                                                        \
	(ctx).reg_save_area.fp[5]       = xmm5;                                                        \
	(ctx).reg_save_area.fp[6]       = xmm6;                                                        \
	(ctx).reg_save_area.fp[7]       = xmm7;                                                        \
	(ctx).va_list.reg_save_area     = &(ctx).reg_save_area;                                        \
	(ctx).va_list.gp_offset         = offsetof(VaRegSave, gp);                                     \
	(ctx).va_list.fp_offset         = offsetof(VaRegSave, fp);                                     \
	(ctx).va_list.overflow_arg_area = &overflow_arg_area;

namespace Libs {

#pragma pack(1)

struct VaList {
	uint32_t gp_offset;
	uint32_t fp_offset;
	void*    overflow_arg_area;
	void*    reg_save_area;
};

struct VaRegSave {
	uint64_t gp[6];
	__m128   fp[8];
};

struct VaContext {
	VaRegSave reg_save_area;
	VaList    va_list;
};

struct VaCharX16 {
	char x[16];
};
#elif defined(__aarch64__) || defined(__arm64__)
// ARM64 calling convention: x0-x7 for integer args, v0-v7 for FP args (128-bit NEON registers)
// Use portable 16-byte type instead of x86 __m128 intrinsic
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define VA_ARGS                                                                                    \
	uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6,      \
	    uint64_t x7, uint64_t overflow_arg_area, uint64_t v0_lo, uint64_t v0_hi,                     \
	    uint64_t v1_lo, uint64_t v1_hi, uint64_t v2_lo, uint64_t v2_hi, uint64_t v3_lo,             \
	    uint64_t v3_hi, uint64_t v4_lo, uint64_t v4_hi, uint64_t v5_lo, uint64_t v5_hi,             \
	    uint64_t v6_lo, uint64_t v6_hi, uint64_t v7_lo, uint64_t v7_hi, ...

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define VA_CONTEXT(ctx)                                                                            \
	alignas(16) VaContext ctx;                                                                     \
	(ctx).reg_save_area.gp[0]       = x0;                                                          \
	(ctx).reg_save_area.gp[1]       = x1;                                                          \
	(ctx).reg_save_area.gp[2]       = x2;                                                          \
	(ctx).reg_save_area.gp[3]       = x3;                                                          \
	(ctx).reg_save_area.gp[4]       = x4;                                                          \
	(ctx).reg_save_area.gp[5]       = x5;                                                          \
	(ctx).reg_save_area.gp[6]       = x6;                                                          \
	(ctx).reg_save_area.gp[7]       = x7;                                                          \
	(ctx).reg_save_area.fp[0].lo    = v0_lo; (ctx).reg_save_area.fp[0].hi = v0_hi;                 \
	(ctx).reg_save_area.fp[1].lo    = v1_lo; (ctx).reg_save_area.fp[1].hi = v1_hi;                 \
	(ctx).reg_save_area.fp[2].lo    = v2_lo; (ctx).reg_save_area.fp[2].hi = v2_hi;                 \
	(ctx).reg_save_area.fp[3].lo    = v3_lo; (ctx).reg_save_area.fp[3].hi = v3_hi;                 \
	(ctx).reg_save_area.fp[4].lo    = v4_lo; (ctx).reg_save_area.fp[4].hi = v4_hi;                 \
	(ctx).reg_save_area.fp[5].lo    = v5_lo; (ctx).reg_save_area.fp[5].hi = v5_hi;                 \
	(ctx).reg_save_area.fp[6].lo    = v6_lo; (ctx).reg_save_area.fp[6].hi = v6_hi;                 \
	(ctx).reg_save_area.fp[7].lo    = v7_lo; (ctx).reg_save_area.fp[7].hi = v7_hi;                 \
	(ctx).va_list.reg_save_area     = &(ctx).reg_save_area;                                        \
	(ctx).va_list.gp_offset         = offsetof(VaRegSave, gp);                                     \
	(ctx).va_list.fp_offset         = offsetof(VaRegSave, fp);                                     \
	(ctx).va_list.overflow_arg_area = &overflow_arg_area;

namespace Libs {

#pragma pack(1)

struct VaList {
	uint32_t gp_offset;
	uint32_t fp_offset;
	void*    overflow_arg_area;
	void*    reg_save_area;
};

struct VaFpReg {
	uint64_t lo;
	uint64_t hi;
};

struct VaRegSave {
	uint64_t gp[8];
	VaFpReg  fp[8];
};

struct VaContext {
	VaRegSave reg_save_area;
	VaList    va_list;
};

struct VaCharX16 {
	char x[16];
};
#else
namespace Libs {

#pragma pack(1)

struct VaList {
	uint32_t gp_offset;
	uint32_t fp_offset;
	void*    overflow_arg_area;
	void*    reg_save_area;
};

struct VaRegSave {
	uint64_t gp[8];
	uint64_t fp[8];
};

struct VaContext {
	VaRegSave reg_save_area;
	VaList    va_list;
};

struct VaCharX16 {
	char x[16];
};
#endif

struct VaShortX8 {
	short x[8]; // NOLINT(google-runtime-int)
};

struct VaIntX4 {
	int x[4];
};

struct VaFloatX4 {
	float x[4];
};

#pragma pack()

template <class T, uint64_t Align, uint64_t Size>
T VaArg_overflow_arg_area(VaList* l) {
	auto  ptr  = ((reinterpret_cast<uint64_t>(l->overflow_arg_area) + (Align - 1)) & ~(Align - 1));
	auto* addr = reinterpret_cast<T*>(ptr);
	l->overflow_arg_area = reinterpret_cast<void*>(ptr + Size);
	return *addr;
}

template <class T, uint32_t Size>
T VaArg_reg_save_area_gp(VaList* l) {
	auto* addr = reinterpret_cast<T*>(static_cast<uint8_t*>(l->reg_save_area) + l->gp_offset);
	l->gp_offset += Size;
	return *addr;
}

template <class T, uint32_t Size>
T VaArg_reg_save_area_fp(VaList* l) {
	auto* addr = reinterpret_cast<T*>(static_cast<uint8_t*>(l->reg_save_area) + l->fp_offset);
	l->fp_offset += Size;
	return *addr;
}

#if defined(__x86_64__)
template <>
inline VaFloatX4 VaArg_reg_save_area_fp<VaFloatX4, 32>(VaList* l) {
	auto* addr =
	    reinterpret_cast<__m128*>(static_cast<uint8_t*>(l->reg_save_area) + l->fp_offset);
	l->fp_offset += 32;
	VaFloatX4 ret = {{addr[0].m128_f32[0], addr[0].m128_f32[1],
	                  addr[1].m128_f32[0], addr[1].m128_f32[1]}};
	return ret;
}

inline double VaArg_double(VaList* l) {
	// 8 FP registers * 16 bytes = 128 bytes total; max valid offset is 112 (7*16)
	if (l->fp_offset <= 112) {
		auto* addr = reinterpret_cast<__m128*>(static_cast<uint8_t*>(l->reg_save_area) + l->fp_offset);
		l->fp_offset += 16;
		return addr[0].m128_f64[0];
	}
	return VaArg_overflow_arg_area<double, 1, 8>(l);
}
#elif defined(__aarch64__) || defined(__arm64__)
template <>
inline VaFloatX4 VaArg_reg_save_area_fp<VaFloatX4, 32>(VaList* l) {
	auto* addr =
	    reinterpret_cast<VaFpReg*>(static_cast<uint8_t*>(l->reg_save_area) + l->fp_offset);
	l->fp_offset += 32;
	VaFloatX4 ret = {{static_cast<float>(addr[0].lo), static_cast<float>(addr[0].hi),
	                  static_cast<float>(addr[1].lo), static_cast<float>(addr[1].hi)}};
	return ret;
}

inline double VaArg_double(VaList* l) {
	// 8 FP registers * 16 bytes = 128 bytes total; max valid offset is 112 (7*16)
	if (l->fp_offset <= 112) {
		auto* addr = reinterpret_cast<VaFpReg*>(static_cast<uint8_t*>(l->reg_save_area) + l->fp_offset);
		l->fp_offset += 16;
		uint64_t bits = addr[0].lo;
		return std::bit_cast<double>(bits);
	}
	return VaArg_overflow_arg_area<double, 1, 8>(l);
}
#else
// Fallback for other architectures
template <>
inline VaFloatX4 VaArg_reg_save_area_fp<VaFloatX4, 32>(VaList* l) {
	auto* addr =
	    reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(l->reg_save_area) + l->fp_offset);
	l->fp_offset += 32;
	VaFloatX4 ret = {{static_cast<float>(addr[0]), static_cast<float>(addr[1]),
	                  static_cast<float>(addr[2]), static_cast<float>(addr[3])}};
	return ret;
}

inline double VaArg_double(VaList* l) {
	if (l->fp_offset <= 112) {
		auto* addr = reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(l->reg_save_area) + l->fp_offset);
		l->fp_offset += 16;
		return std::bit_cast<double>(addr[0]);
	}
	return VaArg_overflow_arg_area<double, 1, 8>(l);
}
#endif

inline int VaArg_int(VaList* l) {
	if (l->gp_offset <= 40) {
		return VaArg_reg_save_area_gp<int, 8>(l);
	}
	return VaArg_overflow_arg_area<int, 1, 8>(l);
}

inline long double VaArg_long_double(VaList* l) {
	return VaArg_overflow_arg_area<long double, 16, 16>(l);
}

/*inline wint_t VaArg_wint_t(VaList* l)
{
    if (l->gp_offset <= 40)
    {
        return VaArg_reg_save_area_gp<wint_t, 8>(l);
    }
    return VaArg_overflow_arg_area<wint_t, 1, 8>(l);
}*/

inline VaCharX16 VaArg_char_x16(VaList* l) {
	if (l->gp_offset <= 32) {
		return VaArg_reg_save_area_gp<VaCharX16, 16>(l);
	}
	return VaArg_overflow_arg_area<VaCharX16, 1, 16>(l);
}

inline long VaArg_long(VaList* l) // NOLINT(google-runtime-int)
{
	if (l->gp_offset <= 40) {
		return VaArg_reg_save_area_gp<long, 8>(l); // NOLINT(google-runtime-int)
	}
	return VaArg_overflow_arg_area<long, 1, 8>(l); // NOLINT(google-runtime-int)
}

inline intmax_t VaArg_intmax_t(VaList* l) {
	if (l->gp_offset <= 40) {
		return VaArg_reg_save_area_gp<intmax_t, 8>(l);
	}
	return VaArg_overflow_arg_area<intmax_t, 1, 8>(l);
}

inline long long VaArg_long_long(VaList* l) // NOLINT(google-runtime-int)
{
	if (l->gp_offset <= 40) {
		return VaArg_reg_save_area_gp<long long, 8>(l); // NOLINT(google-runtime-int)
	}
	return VaArg_overflow_arg_area<long long, 1, 8>(l); // NOLINT(google-runtime-int)
}

inline ptrdiff_t VaArg_ptrdiff_t(VaList* l) {
	if (l->gp_offset <= 40) {
		return VaArg_reg_save_area_gp<ptrdiff_t, 8>(l);
	}
	return VaArg_overflow_arg_area<ptrdiff_t, 1, 8>(l);
}

inline size_t VaArg_size_t(VaList* l) {
	if (l->gp_offset <= 40) {
		return VaArg_reg_save_area_gp<size_t, 8>(l);
	}
	return VaArg_overflow_arg_area<size_t, 1, 8>(l);
}

inline VaShortX8 VaArg_ShortX8(VaList* l) {
	if (l->gp_offset <= 32) {
		return VaArg_reg_save_area_gp<VaShortX8, 16>(l);
	}
	return VaArg_overflow_arg_area<VaShortX8, 1, 16>(l);
}

inline VaIntX4 VaArg_IntX4(VaList* l) {
	if (l->gp_offset <= 32) {
		return VaArg_reg_save_area_gp<VaIntX4, 16>(l);
	}
	return VaArg_overflow_arg_area<VaIntX4, 1, 16>(l);
}

inline VaFloatX4 VaArg_FloatX4(VaList* l) {
	// 8 FP registers * 16 bytes = 128 bytes; FloatX4 takes 2 registers (32 bytes)
	// Max valid offset for first of pair is 96 (6*16), so 96+32=128
	if (l->fp_offset <= 96) {
		return VaArg_reg_save_area_fp<VaFloatX4, 32>(l);
	}
	return VaArg_overflow_arg_area<VaFloatX4, 1, 16>(l);
}

template <class T>
T* VaArg_ptr(VaList* l) {
	if (l->gp_offset <= 40) {
		return VaArg_reg_save_area_gp<T*, 8>(l);
	}
	return VaArg_overflow_arg_area<T*, 1, 8>(l);
}

} // namespace Libs

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_VACONTEXT_H_ */