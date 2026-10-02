#include "loader/x64InstructionEmulator.h"

#include "common/common.h"
#include "common/logging/log.h"

#include <Zydis/Zydis.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <emmintrin.h>
#include <xmmintrin.h>
#include <cpuid.h>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#elif defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <sched.h>
#include <ucontext.h>
#endif

namespace Loader::X64InstructionEmulator {

static uint64_t ExtractBitField(uint64_t value, uint32_t length, uint32_t index) {
	length &= 0x3fu;
	index &= 0x3fu;

	if (length == 0) {
		length = 64;
	}

	if (index >= 64) {
		return 0;
	}

	auto available = 64u - index;
	if (length > available) {
		length = available;
	}

	const uint64_t mask = (length == 64 ? UINT64_MAX : ((uint64_t {1} << length) - 1u));
	return (value >> index) & mask;
}

static uint64_t InsertBitField(uint64_t dst, uint64_t src, uint32_t length, uint32_t index) {
	length &= 0x3fu;
	index &= 0x3fu;

	if (length == 0) {
		length = 64;
	}

	if (index >= 64) {
		return dst;
	}

	auto available = 64u - index;
	if (length > available) {
		length = available;
	}

	const uint64_t mask        = (length == 64 ? UINT64_MAX : ((uint64_t {1} << length) - 1u));
	const uint64_t shifted     = (index == 0 ? mask : (mask << index));
	const uint64_t src_shifted = (src & mask) << index;

	return (dst & ~shifted) | src_shifted;
}

struct XmmWords {
	uint32_t w[4];
};

static void Sha1Msg1(XmmWords& dest, const XmmWords& src2) {
	const uint32_t w0 = dest.w[3];
	const uint32_t w1 = dest.w[2];
	const uint32_t w2 = dest.w[1];
	const uint32_t w3 = dest.w[0];
	const uint32_t w4 = src2.w[3];
	const uint32_t w5 = src2.w[2];
	dest.w[3]         = w2 ^ w0;
	dest.w[2]         = w3 ^ w1;
	dest.w[1]         = w4 ^ w2;
	dest.w[0]         = w5 ^ w3;
}

static void Sha1Msg2(XmmWords& dest, const XmmWords& src2) {
	const uint32_t w13 = src2.w[2];
	const uint32_t w14 = src2.w[1];
	const uint32_t w15 = src2.w[0];
	const uint32_t w16 = std::rotl(dest.w[3] ^ w13, 1);
	const uint32_t w17 = std::rotl(dest.w[2] ^ w14, 1);
	const uint32_t w18 = std::rotl(dest.w[1] ^ w15, 1);
	const uint32_t w19 = std::rotl(dest.w[0] ^ w16, 1);
	dest.w[3]          = w16;
	dest.w[2]          = w17;
	dest.w[1]          = w18;
	dest.w[0]          = w19;
}

static void Sha1Nexte(XmmWords& dest, const XmmWords& src2) {
	const uint32_t tmp = std::rotl(dest.w[3], 30);
	dest.w[3]          = src2.w[3] + tmp;
	dest.w[2]          = src2.w[2];
	dest.w[1]          = src2.w[1];
	dest.w[0]          = src2.w[0];
}

static uint32_t Sha1RoundFunc(uint8_t group, uint32_t b, uint32_t c, uint32_t d) {
	switch (group & 3u) {
		case 0: return (b & c) ^ ((~b) & d);
		case 1: return b ^ c ^ d;
		case 2: return (b & c) ^ (b & d) ^ (c & d);
		default: return b ^ c ^ d;
	}
}

static uint32_t Sha1RoundConstant(uint8_t group) {
	switch (group & 3u) {
		case 0: return 0x5a827999u;
		case 1: return 0x6ed9eba1u;
		case 2: return 0x8f1bbcdcu;
		default: return 0xca62c1d6u;
	}
}

static void Sha1Rnds4(XmmWords& dest, const XmmWords& src2, uint8_t imm8) {
	const uint8_t  group = imm8 & 3u;
	const uint32_t k     = Sha1RoundConstant(group);
	const uint32_t w[4]  = {src2.w[3], src2.w[2], src2.w[1], src2.w[0]};

	uint32_t a = dest.w[3];
	uint32_t b = dest.w[2];
	uint32_t c = dest.w[1];
	uint32_t d = dest.w[0];
	uint32_t e = 0;

	for (unsigned int round = 0; round < 4u; round++) {
		uint32_t term = Sha1RoundFunc(group, b, c, d) + std::rotl(a, 5) + w[round] + k;
		if (round > 0u) {
			term += e;
		}
		const uint32_t a1 = term;
		e                 = d;
		d                 = c;
		c                 = std::rotl(b, 30);
		b                 = a;
		a                 = a1;
	}

	dest.w[3] = a;
	dest.w[2] = b;
	dest.w[1] = c;
	dest.w[0] = d;
}

static uint32_t Sha256Sigma0(uint32_t x) {
	return std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3u);
}

static uint32_t Sha256Sigma1(uint32_t x) {
	return std::rotr(x, 17) ^ std::rotr(x, 19) ^ (x >> 10u);
}

static uint32_t Sha256Sum0(uint32_t x) {
	return std::rotr(x, 2) ^ std::rotr(x, 13) ^ std::rotr(x, 22);
}

static uint32_t Sha256Sum1(uint32_t x) {
	return std::rotr(x, 6) ^ std::rotr(x, 11) ^ std::rotr(x, 25);
}

static uint32_t Sha256Ch(uint32_t e, uint32_t f, uint32_t g) {
	return (e & f) ^ ((~e) & g);
}

static uint32_t Sha256Maj(uint32_t a, uint32_t b, uint32_t c) {
	return (a & b) ^ (a & c) ^ (b & c);
}

static void Sha256Msg1(XmmWords& dest, const XmmWords& src2) {
	const uint32_t w4 = src2.w[0];
	const uint32_t w3 = dest.w[3];
	const uint32_t w2 = dest.w[2];
	const uint32_t w1 = dest.w[1];
	const uint32_t w0 = dest.w[0];
	dest.w[3]         = w3 + Sha256Sigma0(w4);
	dest.w[2]         = w2 + Sha256Sigma0(w3);
	dest.w[1]         = w1 + Sha256Sigma0(w2);
	dest.w[0]         = w0 + Sha256Sigma0(w1);
}

static void Sha256Msg2(XmmWords& dest, const XmmWords& src2) {
	const uint32_t w14 = src2.w[2];
	const uint32_t w15 = src2.w[3];
	const uint32_t w16 = dest.w[0] + Sha256Sigma1(w14);
	const uint32_t w17 = dest.w[1] + Sha256Sigma1(w15);
	const uint32_t w18 = dest.w[2] + Sha256Sigma1(w16);
	const uint32_t w19 = dest.w[3] + Sha256Sigma1(w17);
	dest.w[3]          = w19;
	dest.w[2]          = w18;
	dest.w[1]          = w17;
	dest.w[0]          = w16;
}

static void Sha256Rnds2(XmmWords& dest, const XmmWords& src2, const XmmWords& xmm0) {
	uint32_t a = src2.w[3];
	uint32_t b = src2.w[2];
	uint32_t c = dest.w[3];
	uint32_t d = dest.w[2];
	uint32_t e = src2.w[1];
	uint32_t f = src2.w[0];
	uint32_t g = dest.w[1];
	uint32_t h = dest.w[0];

	for (unsigned int round = 0; round < 2u; round++) {
		const uint32_t wk = xmm0.w[round];
		const uint32_t t1 = Sha256Ch(e, f, g) + Sha256Sum1(e) + wk + h;
		const uint32_t t2 = Sha256Maj(a, b, c) + Sha256Sum0(a);
		const uint32_t a1 = t1 + t2;
		const uint32_t e1 = t1 + d;
		const uint32_t b1 = a;
		const uint32_t c1 = b;
		const uint32_t d1 = c;
		const uint32_t f1 = e;
		const uint32_t g1 = f;
		const uint32_t h1 = g;
		a                 = a1;
		b                 = b1;
		c                 = c1;
		d                 = d1;
		e                 = e1;
		f                 = f1;
		g                 = g1;
		h                 = h1;
	}

	dest.w[3] = a;
	dest.w[2] = b;
	dest.w[1] = e;
	dest.w[0] = f;
}

struct ShaNiInsn {
	uint8_t escape;
	uint8_t opcode;
	uint8_t imm8;
	uint8_t rex;
	size_t  modrm_offset;
	size_t  length;
};

static bool DecodeShaNiInsn(const uint8_t* rip, ShaNiInsn& insn) {
	size_t  offset = 0;
	uint8_t rex    = 0;
	if ((rip[0] & 0xf0u) == 0x40u) {
		rex    = rip[0];
		offset = 1;
	}

	if (rip[offset] != 0x0f) {
		return false;
	}

	if (rip[offset + 1] == 0x38) {
		const uint8_t op = rip[offset + 2];
		if (op != 0xc8 && op != 0xc9 && op != 0xca && op != 0xcb && op != 0xcc && op != 0xcd) {
			return false;
		}
		insn.escape       = 0x38;
		insn.opcode       = op;
		insn.imm8         = 0;
		insn.rex          = rex;
		insn.modrm_offset = offset + 3;
	} else if (rip[offset + 1] == 0x3a && rip[offset + 2] == 0xcc) {
		insn.escape       = 0x3a;
		insn.opcode       = 0xcc;
		insn.rex          = rex;
		insn.modrm_offset = offset + 3;
	} else {
		return false;
	}

	const uint8_t modrm = rip[insn.modrm_offset];
	const uint8_t mod   = modrm >> 6u;
	const uint8_t rm    = modrm & 0x07u;
	size_t        end   = insn.modrm_offset + 1;

	if (mod != 3u) {
		uint8_t sib_base = 0xffu;
		if (rm == 4u) {
			sib_base = rip[end] & 0x07u;
			end++;
		}

		if (mod == 0u && (rm == 5u || (rm == 4u && sib_base == 5u))) {
			end += 4;
		} else if (mod == 1u) {
			end++;
		} else if (mod == 2u) {
			end += 4;
		}
	}

	if (insn.escape == 0x3a) {
		insn.imm8 = rip[end];
		end++;
	}

	insn.length = end;
	return true;
}

static bool ShaNiModrmIsRegister(uint8_t modrm) {
	return (modrm & 0xc0u) == 0xc0u;
}

static uint8_t ShaNiRegIndex(uint8_t modrm, uint8_t rex, bool reg_field) {
	if (reg_field) {
		return ((modrm >> 3u) & 0x07u) | ((rex & 0x04u) << 1u);
	}
	return (modrm & 0x07u) | ((rex & 0x01u) << 3u);
}

static bool ResolveShaNiMemoryAddress(const uint8_t* rip, const ShaNiInsn&    insn,
                                      const uint64_t (&gpr)[16], const void*& address) {
	const uint8_t modrm = rip[insn.modrm_offset];
	const uint8_t mod   = modrm >> 6u;
	const uint8_t rm    = modrm & 0x07u;
	if (mod == 3u) {
		return false;
	}

	size_t   offset = insn.modrm_offset + 1;
	uint64_t result = 0;

	if (rm == 4u) {
		const uint8_t sib       = rip[offset++];
		const uint8_t scale     = sib >> 6u;
		const uint8_t index_low = (sib >> 3u) & 0x07u;
		const uint8_t base_low  = sib & 0x07u;
		const bool    has_index = index_low != 4u || (insn.rex & 0x02u) != 0;
		const bool    has_base  = mod != 0u || base_low != 5u;

		if (has_base) {
			const uint8_t base = base_low | ((insn.rex & 0x01u) << 3u);
			result += gpr[base];
		}
		if (has_index) {
			const uint8_t index = index_low | ((insn.rex & 0x02u) << 2u);
			result += gpr[index] << scale;
		}

		if (!has_base) {
			int32_t displacement = 0;
			std::memcpy(&displacement, rip + offset, sizeof(displacement));
			result += static_cast<uint64_t>(static_cast<int64_t>(displacement));
			offset += sizeof(displacement);
		}
	} else if (mod == 0u && rm == 5u) {
		int32_t displacement = 0;
		std::memcpy(&displacement, rip + offset, sizeof(displacement));
		result = reinterpret_cast<uint64_t>(rip + insn.length) +
		         static_cast<uint64_t>(static_cast<int64_t>(displacement));
		offset += sizeof(displacement);
	} else {
		const uint8_t base = rm | ((insn.rex & 0x01u) << 3u);
		result             = gpr[base];
	}

	if (mod == 1u) {
		const auto displacement = static_cast<int8_t>(rip[offset]);
		result += static_cast<uint64_t>(static_cast<int64_t>(displacement));
	} else if (mod == 2u) {
		int32_t displacement = 0;
		std::memcpy(&displacement, rip + offset, sizeof(displacement));
		result += static_cast<uint64_t>(static_cast<int64_t>(displacement));
	}

	address = reinterpret_cast<const void*>(result);
	return true;
}

static bool ExecuteShaNiInsn(const ShaNiInsn& insn, const XmmWords& src2, const XmmWords& xmm0,
                             XmmWords& dest) {
	if (insn.escape == 0x3a && insn.opcode == 0xcc) {
		Sha1Rnds4(dest, src2, insn.imm8);
		return true;
	}

	switch (insn.opcode) {
		case 0xc8: Sha1Nexte(dest, src2); return true;
		case 0xc9: Sha1Msg1(dest, src2); return true;
		case 0xca: Sha1Msg2(dest, src2); return true;
		case 0xcb: Sha256Rnds2(dest, src2, xmm0); return true;
		case 0xcc: Sha256Msg1(dest, src2); return true;
		case 0xcd: Sha256Msg2(dest, src2); return true;
		default: return false;
	}
}

// Keep instruction semantics shared; only access to the saved host context differs.
struct Context {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	PCONTEXT native;

	[[nodiscard]] uint64_t Rip() const { return native->Rip; }
	void                   Advance(size_t length) { native->Rip += length; }
	void                   SetRip(uint64_t rip) { native->Rip = rip; }
	[[nodiscard]] void*    Xmm(uint8_t index) const { return &native->Xmm0 + index; }

	[[nodiscard]] auto& Gpr(uint8_t index) const {
		DWORD64* registers[] = {&native->Rax, &native->Rcx, &native->Rdx, &native->Rbx,
		                        &native->Rsp, &native->Rbp, &native->Rsi, &native->Rdi,
		                        &native->R8,  &native->R9,  &native->R10, &native->R11,
		                        &native->R12, &native->R13, &native->R14, &native->R15};
		return *registers[index];
	}

	void ClearUpperYmm(uint8_t index) const {
		if ((native->ContextFlags & CONTEXT_XSTATE) != CONTEXT_XSTATE) {
			return;
		}
		DWORD64 features = 0;
		if (!GetXStateFeaturesMask(native, &features) || (features & XSTATE_MASK_AVX) == 0) {
			return; // An absent AVX component restores zeroes.
		}
		DWORD size = 0;
		auto* ymm = static_cast<M128A*>(LocateXStateFeature(native, XSTATE_AVX, &size));
		if (ymm != nullptr && size >= (index + 1u) * sizeof(M128A)) {
			ymm[index] = {};
		}
	}
#elif defined(__APPLE__)
	ucontext_t* native;

	[[nodiscard]] uint64_t Rip() const {
		return static_cast<uint64_t>(native->uc_mcontext->__ss.__rip);
	}
	void Advance(size_t length) {
		native->uc_mcontext->__ss.__rip += static_cast<uint64_t>(length);
	}
	[[nodiscard]] auto& Gpr(uint8_t index) const {
		auto*     state       = &native->uc_mcontext->__ss;
		uint64_t* registers[] = {&state->__rax, &state->__rcx, &state->__rdx, &state->__rbx,
		                         &state->__rsp, &state->__rbp, &state->__rsi, &state->__rdi,
		                         &state->__r8,  &state->__r9,  &state->__r10, &state->__r11,
		                         &state->__r12, &state->__r13, &state->__r14, &state->__r15};
		return *registers[index];
	}
	void SetRip(uint64_t rip) { native->uc_mcontext->__ss.__rip = rip; }
	// Darwin names the XMM file __fpu_xmm0..__fpu_xmm15 instead of exposing an array.
	[[nodiscard]] void* Xmm(uint8_t index) const {
		auto* fs = &native->uc_mcontext->__fs;
		switch (index) {
			case 0: return &fs->__fpu_xmm0;
			case 1: return &fs->__fpu_xmm1;
			case 2: return &fs->__fpu_xmm2;
			case 3: return &fs->__fpu_xmm3;
			case 4: return &fs->__fpu_xmm4;
			case 5: return &fs->__fpu_xmm5;
			case 6: return &fs->__fpu_xmm6;
			case 7: return &fs->__fpu_xmm7;
			case 8: return &fs->__fpu_xmm8;
			case 9: return &fs->__fpu_xmm9;
			case 10: return &fs->__fpu_xmm10;
			case 11: return &fs->__fpu_xmm11;
			case 12: return &fs->__fpu_xmm12;
			case 13: return &fs->__fpu_xmm13;
			case 14: return &fs->__fpu_xmm14;
			case 15: return &fs->__fpu_xmm15;
			default: return nullptr;
		}
	}
#else
	ucontext_t* native;

	[[nodiscard]] uint64_t Rip() const {
		return static_cast<uint64_t>(native->uc_mcontext.gregs[REG_RIP]);
	}
	void Advance(size_t length) {
		native->uc_mcontext.gregs[REG_RIP] += static_cast<greg_t>(length);
	}
	void SetRip(uint64_t rip) { native->uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(rip); }
	[[nodiscard]] void* Xmm(uint8_t index) const {
		if (native->uc_mcontext.fpregs == nullptr) {
			return nullptr;
		}
		return native->uc_mcontext.fpregs->_xmm[index].element;
	}

	[[nodiscard]] auto& Gpr(uint8_t index) const {
		constexpr int registers[] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
		                             REG_RSI, REG_RDI, REG_R8,  REG_R9,  REG_R10, REG_R11,
		                             REG_R12, REG_R13, REG_R14, REG_R15};
		return native->uc_mcontext.gregs[registers[index]];
	}

	void ClearUpperYmm(uint8_t index) const {
		// Linux signal frames use the standard XSAVE layout. An absent AVX component
		// already restores the architectural initial value (all zeroes).
		auto*    state    = reinterpret_cast<uint8_t*>(native->uc_mcontext.fpregs);
		uint32_t magic    = 0;
		uint32_t size     = 0;
		uint64_t features = 0;
		std::memcpy(&magic, state + 464, sizeof(magic));
		if (magic != 0x46505853) {
			return;
		}
		std::memcpy(&size, state + 480, sizeof(size));
		if (size < 832) {
			return;
		}
		std::memcpy(&features, state + 512, sizeof(features));
		if ((features & 4) != 0) {
			std::memset(state + 576 + index * 16, 0, 16);
		}
	}
#endif

	void LoadGprs(uint64_t (&gpr)[16]) const {
		for (uint8_t i = 0; i < 16; ++i) {
			gpr[i] = static_cast<uint64_t>(Gpr(i));
		}
	}
};

#if !defined(__APPLE__)

static bool TryEmulateShaNi(Context& context) {
	const auto* rip = reinterpret_cast<const uint8_t*>(context.Rip());
	ShaNiInsn   insn {};
	if (!DecodeShaNiInsn(rip, insn)) {
		return false;
	}

	const uint8_t modrm_byte = rip[insn.modrm_offset];
	const uint8_t dest_index = ShaNiRegIndex(modrm_byte, insn.rex, true);
	auto*         dest_xmm   = context.Xmm(dest_index);
	auto*         xmm0       = context.Xmm(0);
	if (dest_xmm == nullptr || xmm0 == nullptr) {
		return false;
	}

	XmmWords dest {};
	XmmWords src2 {};
	XmmWords xmm0_words {};
	std::memcpy(&dest, dest_xmm, sizeof(dest));
	std::memcpy(&xmm0_words, xmm0, sizeof(xmm0_words));

	if (ShaNiModrmIsRegister(modrm_byte)) {
		const uint8_t src_index = ShaNiRegIndex(modrm_byte, insn.rex, false);
		auto*         src_xmm   = context.Xmm(src_index);
		if (src_xmm == nullptr) {
			return false;
		}
		std::memcpy(&src2, src_xmm, sizeof(src2));
	} else {
		uint64_t    gpr[16] {};
		const void* source = nullptr;
		context.LoadGprs(gpr);
		if (!ResolveShaNiMemoryAddress(rip, insn, gpr, source)) {
			return false;
		}
		std::memcpy(&src2, source, sizeof(src2));
	}

	if (!ExecuteShaNiInsn(insn, src2, xmm0_words, dest)) {
		return false;
	}

	std::memcpy(dest_xmm, &dest, sizeof(dest));
	context.Advance(insn.length);
	return true;
}

#endif

static bool TryEmulateSse4a(Context& context) {
	const auto*   rip    = reinterpret_cast<const uint8_t*>(context.Rip());
	const uint8_t prefix = rip[0];
	if (prefix != 0x66 && prefix != 0xf2) {
		return false;
	}

	size_t  offset = 1;
	uint8_t rex    = 0;
	if ((rip[offset] & 0xf0u) == 0x40u) {
		rex = rip[offset++];
	}
	if (rip[offset] != 0x0f) {
		return false;
	}
	const bool register_form = rip[offset + 1] == 0x79;
	if (rip[offset + 1] != 0x78 && !register_form) {
		return false;
	}

	const uint8_t modrm = rip[offset + 2];
	if ((modrm & 0xc0u) != 0xc0u) {
		return false;
	}

	const uint8_t reg = ((modrm >> 3u) & 0x07u) | ((rex & 0x04u) << 1u);
	const uint8_t rm  = (modrm & 0x07u) | ((rex & 0x01u) << 3u);

	// Immediate EXTRQ encodes its destination in r/m; the two-register form uses reg.
	uint8_t dest_index = reg;
	if (prefix == 0x66 && !register_form) {
		dest_index = rm;
	}
	auto* dest_xmm = context.Xmm(dest_index);
	auto* src_xmm  = context.Xmm(rm);
	if (dest_xmm == nullptr || src_xmm == nullptr) {
		return false;
	}
	uint64_t dest[2] {};
	uint64_t source[2] {};
	std::memcpy(dest, dest_xmm, sizeof(dest));
	std::memcpy(source, src_xmm, sizeof(source));
	uint8_t length             = 0;
	uint8_t index              = 0;
	size_t  instruction_length = offset + 3;
	if (register_form) {
		// EXTRQ controls occupy the low quadword; INSERTQ uses the upper quadword.
		const uint64_t controls = source[prefix == 0x66 ? 0 : 1];
		length                  = static_cast<uint8_t>(controls);
		index                   = static_cast<uint8_t>(controls >> 8u);
	} else {
		length = rip[offset + 3];
		index  = rip[offset + 4];
		instruction_length += 2;
	}
	if (prefix == 0x66) {
		dest[0] = ExtractBitField(dest[0], length, index);
		dest[1] = 0;
	} else {
		dest[0] = InsertBitField(dest[0], source[0], length, index);
	}
	std::memcpy(dest_xmm, dest, sizeof(dest));
	context.Advance(instruction_length);
	return true;
}

static bool ReadTscAux(uint32_t& auxiliary) {
	uint32_t eax = 0;
	uint32_t ebx = 0;
	uint32_t ecx = 0;
	uint32_t edx = 0;
	if (!__get_cpuid(0x80000001u, &eax, &ebx, &ecx, &edx) || (edx & (1u << 27u)) == 0) {
		return false;
	}
	asm volatile("rdtscp" : "=a"(eax), "=d"(edx), "=c"(auxiliary));
	return true;
}

static void FlushCacheLine(uint64_t address, ZydisRegister segment) {
	// Preserve FS/GS addressing rather than treating a segment-relative offset as a pointer.
	if (segment == ZYDIS_REGISTER_FS) {
		asm volatile("clflush %%fs:(%0)" : : "r"(address) : "memory");
	} else if (segment == ZYDIS_REGISTER_GS) {
		asm volatile("clflush %%gs:(%0)" : : "r"(address) : "memory");
	} else {
		_mm_clflush(reinterpret_cast<const void*>(address));
	}
}

static bool TryEmulateCpuExtensions(Context& context) {
	ZydisDecoder            decoder {};
	ZydisDecodedInstruction instruction {};
	ZydisDecodedOperand     operands[ZYDIS_MAX_OPERAND_COUNT] {};
	if (!ZYAN_SUCCESS(
	        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)) ||
	    !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, reinterpret_cast<const void*>(context.Rip()),
	                                         ZYDIS_MAX_INSTRUCTION_LENGTH, &instruction,
	                                         operands))) {
		return false;
	}
	if (instruction.mnemonic == ZYDIS_MNEMONIC_RDPID &&
	    operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
		const auto reg_class = ZydisRegisterGetClass(operands[0].reg.value);
		const auto index     = ZydisRegisterGetId(operands[0].reg.value);
		uint32_t   auxiliary = 0;
		if ((reg_class != ZYDIS_REGCLASS_GPR32 && reg_class != ZYDIS_REGCLASS_GPR64) || index < 0 ||
		    index >= 16 || !ReadTscAux(auxiliary)) {
			return false;
		}
		// RDPID and guest RDTSCP observe the same host TSC_AUX; only RDPID's destination changes.
		context.Gpr(static_cast<uint8_t>(index)) = auxiliary;
	} else if (instruction.mnemonic == ZYDIS_MNEMONIC_CLWB &&
	           operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
		ZydisRegisterContext registers {};
		for (uint8_t i = 0; i < 16; ++i) {
			const auto value                         = static_cast<uint64_t>(context.Gpr(i));
			registers.values[ZYDIS_REGISTER_RAX + i] = value;
			registers.values[ZYDIS_REGISTER_EAX + i] = static_cast<uint32_t>(value);
		}
		uint64_t address = 0;
		if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddressEx(&instruction, &operands[0], context.Rip(),
		                                             &registers, &address))) {
			return false;
		}
		FlushCacheLine(address, operands[0].mem.segment);
	} else {
		return false;
	}
	context.Advance(instruction.length);
	return true;
}

#if !defined(__APPLE__)

static bool TryEmulateMonitorxMwaitx(Context& context) {
	const auto* rip = reinterpret_cast<const uint8_t*>(context.Rip());
	if (rip[0] != 0x0f || rip[1] != 0x01 || (rip[2] != 0xfa && rip[2] != 0xfb)) {
		return false;
	}

	// Approximate AMD MONITORX/MWAITX as no-op/yield.
	if (rip[2] == 0xfb) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		SwitchToThread();
#else
		::sched_yield();
#endif
	}
	context.Advance(3);
	return true;
}

static uint32_t ReciprocalSquareRoot(uint32_t bits) {
	const uint32_t magnitude = bits & 0x7fffffffu;
	const uint32_t exponent  = magnitude & 0x7f800000u;
	if (exponent == 0) {
		// RSQRT treats denormals as signed zero regardless of MXCSR.DAZ.
		return (bits & 0x80000000u) | 0x7f800000u;
	}
	if (magnitude > 0x7f800000u) {
		return bits | 0x00400000u; // Quiet NaNs without raising an exception.
	}
	if ((bits & 0x80000000u) != 0) {
		return 0xffc00000u;
	}
	if (magnitude == 0x7f800000u) {
		return 0;
	}

	// A deterministic accurate estimate meets the instruction's relative-error
	// bound without relying on the host vendor's approximation table.
	const __m128d input  = _mm_set_sd(static_cast<double>(std::bit_cast<float>(bits)));
	const __m128d result = _mm_div_sd(_mm_set_sd(1.0), _mm_sqrt_sd(input, input));
	return std::bit_cast<uint32_t>(_mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(), result)));
}

static bool TryEmulateReciprocalSquareRoot(Context& context) {
	const auto* rip            = reinterpret_cast<const uint8_t*>(context.Rip());
	size_t      prefix_size    = 0;
	uint8_t     dest_extension = 0;
	uint8_t     src_extension  = 0;
	if (rip[0] == 0xc5 && (rip[1] & 0x7fu) == 0x70u) {
		prefix_size    = 2;
		dest_extension = (~rip[1] & 0x80u) >> 4u;
	} else if (rip[0] == 0xc4 && (rip[1] & 0x1fu) == 1 && (rip[2] & 0x7fu) == 0x70u) {
		prefix_size    = 3;
		dest_extension = (~rip[1] & 0x80u) >> 4u;
		src_extension  = (~rip[1] & 0x20u) >> 2u;
	} else {
		return false;
	}
	if (rip[prefix_size] != 0x52 || (rip[prefix_size + 1] & 0xc0u) != 0xc0u) {
		return false;
	}

	const uint8_t modrm    = rip[prefix_size + 1];
	const uint8_t dest     = ((modrm >> 3u) & 7u) | dest_extension;
	const uint8_t source   = (modrm & 7u) | src_extension;
	auto*         dest_xmm = context.Xmm(dest);
	auto*         src_xmm  = context.Xmm(source);
	if (dest_xmm == nullptr || src_xmm == nullptr) {
		return false;
	}
	XmmWords result {};
	std::memcpy(&result, src_xmm, sizeof(result));
	// RSQRT ignores the rounding mode and never changes guest exception flags.
	// Mask host exceptions while calculating, then restore the handler's state.
	const uint32_t mxcsr = _mm_getcsr();
	_mm_setcsr(0x1f80);
	for (auto& word: result.w) {
		word = ReciprocalSquareRoot(word);
	}
	_mm_setcsr(mxcsr);
	std::memcpy(dest_xmm, &result, sizeof(result));
	context.ClearUpperYmm(dest);
	context.Advance(prefix_size + 2);
	return true;
}

#endif

static bool IsPlainWideStore(const ZydisDecodedInstruction& instruction,
                             const ZydisDecodedOperand*     operands) {
	switch (instruction.mnemonic) {
		case ZYDIS_MNEMONIC_VMOVUPS:
		case ZYDIS_MNEMONIC_VMOVAPS:
		case ZYDIS_MNEMONIC_VMOVUPD:
		case ZYDIS_MNEMONIC_VMOVAPD:
		case ZYDIS_MNEMONIC_VMOVDQU:
		case ZYDIS_MNEMONIC_VMOVDQA: break;
		default: return false;
	}
	return instruction.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX &&
	       instruction.operand_count_visible == 2 &&
	       operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY && operands[0].size == 256 &&
	       operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
	       operands[1].reg.value >= ZYDIS_REGISTER_YMM0 &&
	       operands[1].reg.value <= ZYDIS_REGISTER_YMM15;
}

// fs:/gs: accesses (TLS) are not moved: re-encoding lost the segment in the test title and the
// moved load read address 0.
static bool UsesSegmentBase(const ZydisDecodedInstruction& instruction,
                            const ZydisDecodedOperand*     operands) {
	for (uint32_t i = 0; i < instruction.operand_count; i++) {
		if (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
		    (operands[i].mem.segment == ZYDIS_REGISTER_FS ||
		     operands[i].mem.segment == ZYDIS_REGISTER_GS)) {
			return true;
		}
	}
	return false;
}

// Encodes at runtime_address; RIP-relative memory operands carry absolute addresses.
static bool EncodeAt(ZydisEncoderRequest& request, uint64_t runtime_address, uint8_t* out,
                     uint64_t capacity, uint64_t* length) {
	ZyanUSize size = capacity;
	if (!ZYAN_SUCCESS(
	        ZydisEncoderEncodeInstructionAbsolute(&request, out, &size, runtime_address))) {
		return false;
	}
	*length = size;
	return true;
}

// next_ip is the address after the original instruction, the base of a RIP-relative operand.
static void SetMemoryOperand(ZydisEncoderOperand& op, const ZydisDecodedOperand& mem,
                             uint64_t next_ip, int64_t extra_displacement) {
	op.type             = ZYDIS_OPERAND_TYPE_MEMORY;
	op.mem.base         = mem.mem.base;
	op.mem.index        = mem.mem.index;
	op.mem.scale        = mem.mem.scale;
	op.mem.displacement = (mem.mem.base == ZYDIS_REGISTER_RIP ? static_cast<int64_t>(next_ip) : 0) +
	                      mem.mem.disp.value + extra_displacement;
	op.mem.size         = 16;
}

// Short stores patched to ud2: site -> trampoline. Written while modules load, read from the
// signal handler, so it is a fixed lock-free open-addressing table.
namespace {
struct WideStoreTrap {
	std::atomic<uint64_t> site {0};
	std::atomic<uint64_t> trampoline {0};
	std::atomic<uint32_t> hits {0};
};
// KYTY_WIDE_STORE_HOT names a profile: sites redirected often are appended to it, and on the
// next start those sites are patched by moving the following instructions into the trampoline
// instead of through a signal per execution.
constexpr uint32_t WideStoreHotHits    = 1024;
int                g_wide_store_hot_fd = -1;

const std::vector<uint64_t>& WideStoreHotSites() {
	static const std::vector<uint64_t> sites = [] {
		std::vector<uint64_t> result;
		const char*           path = std::getenv("KYTY_WIDE_STORE_HOT");
		if (path == nullptr) {
			return result;
		}
		if (FILE* file = std::fopen(path, "r"); file != nullptr) {
			unsigned long long site = 0;
			while (std::fscanf(file, "%llx", &site) == 1) {
				result.push_back(site);
			}
			std::fclose(file);
		}
		std::sort(result.begin(), result.end());
		result.erase(std::unique(result.begin(), result.end()), result.end());
		g_wide_store_hot_fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
		return result;
	}();
	return sites;
}

// Async-signal-safe: formats with no allocation and writes with one write(). Sites already on the
// list (loaded at startup, sorted, read-only afterwards) are not appended again.
void RecordHotWideStore(uint64_t site) {
	if (g_wide_store_hot_fd < 0) {
		return;
	}
	const auto& known = WideStoreHotSites();
	if (std::binary_search(known.begin(), known.end(), site)) {
		return;
	}
	char buffer[20];
	int  length      = 0;
	buffer[length++] = '0';
	buffer[length++] = 'x';
	for (int shift = 60; shift >= 0; shift -= 4) {
		buffer[length++] = "0123456789abcdef"[(site >> shift) & 0xfu];
	}
	buffer[length++] = '\n';
	(void)!write(g_wide_store_hot_fd, buffer, static_cast<size_t>(length));
}
constexpr uint64_t                               WideStoreTrapCapacity = 1u << 15u;
std::array<WideStoreTrap, WideStoreTrapCapacity> g_wide_store_traps {};
std::atomic<uint64_t>                            g_wide_store_trap_hits {0};

uint64_t TrapSlot(uint64_t site) {
	return (site * 0x9E3779B97F4A7C15ull) >> 49u; // 15 bits
}
} // namespace

static bool RegisterWideStoreTrap(uint64_t site, uint64_t trampoline) {
	for (uint64_t i = 0; i < WideStoreTrapCapacity; i++) {
		auto&    entry    = g_wide_store_traps[(TrapSlot(site) + i) % WideStoreTrapCapacity];
		uint64_t expected = 0;
		if (entry.site.load(std::memory_order_acquire) == site) {
			entry.trampoline.store(trampoline, std::memory_order_release);
			return true;
		}
		if (entry.site.compare_exchange_strong(expected, site, std::memory_order_acq_rel)) {
			entry.trampoline.store(trampoline, std::memory_order_release);
			return true;
		}
	}
	return false;
}

static WideStoreTrap* FindWideStoreTrapEntry(uint64_t site) {
	for (uint64_t i = 0; i < WideStoreTrapCapacity; i++) {
		auto&      entry = g_wide_store_traps[(TrapSlot(site) + i) % WideStoreTrapCapacity];
		const auto key   = entry.site.load(std::memory_order_acquire);
		if (key == site) {
			return &entry;
		}
		if (key == 0) {
			return nullptr;
		}
	}
	return nullptr;
}

WideStoreSplitResult SplitWideStores(uint64_t address, uint64_t size, uint64_t* cursor,
                                     uint64_t trampoline_end) {
	WideStoreSplitResult result;
	ZydisDecoder         decoder {};
	if (!ZYAN_SUCCESS(
	        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
		return result;
	}
	constexpr uint64_t JumpSize      = 5;
	constexpr uint64_t ShortJumpSize = 2;

	// Stores shorter than a rel32 jump get a rel8 jump to a "cave": a run of int3 padding
	// between functions (never executed), which holds the rel32 jump to the trampoline.
	struct Cave {
		uint64_t start = 0;
		uint64_t size  = 0;
		uint64_t used  = 0;
	};
	// Also usable: nop padding that directly follows an unconditional ret/jmp/ud2, which
	// execution cannot fall into (branches target the aligned code after it).
	std::vector<Cave>     caves;
	std::vector<uint64_t> branch_targets;
	std::vector<uint64_t> candidates; // offsets of VEX vector moves, checked in the phases
	bool                  after_unconditional = false;
	uint64_t          nop_start           = 0;
	uint64_t          nop_size            = 0;
	const auto            end_nops            = [&]() {
		if (nop_size >= ShortJumpSize) {
			caves.push_back({nop_start, nop_size, 0});
		}
		nop_size = 0;
	};
	for (uint64_t offset = 0; offset < size;) {
		const auto*             code = reinterpret_cast<const uint8_t*>(address + offset);
		ZydisDecoderContext     decode_context {};
		ZydisDecodedInstruction instruction {};
		if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, &decode_context, code,
		                                                size - offset, &instruction))) {
			end_nops();
			after_unconditional = false;
			++offset;
			continue;
		}
		if (instruction.mnemonic == ZYDIS_MNEMONIC_INT3 && instruction.length == 1) {
			end_nops();
			const uint64_t start = address + offset;
			uint64_t       run   = 0;
			while (offset + run < size && code[run] == 0xCC) {
				++run;
			}
			if (run >= ShortJumpSize) {
				caves.push_back({start, run, 0});
			}
			offset += run;
			after_unconditional = true;
			continue;
		}
		if (instruction.mnemonic == ZYDIS_MNEMONIC_NOP && (after_unconditional || nop_size != 0)) {
			if (nop_size == 0) {
				nop_start = address + offset;
			}
			nop_size += instruction.length;
			after_unconditional = false;
			offset += instruction.length;
			continue;
		}
		end_nops();
		if (instruction.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX) {
			switch (instruction.mnemonic) {
				case ZYDIS_MNEMONIC_VMOVUPS:
				case ZYDIS_MNEMONIC_VMOVAPS:
				case ZYDIS_MNEMONIC_VMOVUPD:
				case ZYDIS_MNEMONIC_VMOVAPD:
				case ZYDIS_MNEMONIC_VMOVDQU:
				case ZYDIS_MNEMONIC_VMOVDQA: candidates.push_back(offset); break;
				default: break;
			}
		}
		if (instruction.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE &&
		    (instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE) != 0) {
			ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT] {};
			ZyanU64             target = 0;
			if (ZYAN_SUCCESS(ZydisDecoderDecodeOperands(&decoder, &decode_context, &instruction,
			                                            ops, instruction.operand_count)) &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
			    ZYAN_SUCCESS(
			        ZydisCalcAbsoluteAddress(&instruction, &ops[0], address + offset, &target))) {
				branch_targets.push_back(target);
			}
		}
		after_unconditional = instruction.mnemonic == ZYDIS_MNEMONIC_RET ||
		                      instruction.mnemonic == ZYDIS_MNEMONIC_UD2 ||
		                      (instruction.mnemonic == ZYDIS_MNEMONIC_JMP);
		offset += instruction.length;
	}
	end_nops();
	std::sort(caves.begin(), caves.end(),
	          [](const Cave& a, const Cave& b) { return a.start < b.start; });
	std::sort(branch_targets.begin(), branch_targets.end());
	const auto& hot_sites = WideStoreHotSites();

	// Finds a chain of dead-byte slots from a short jump at next_ip: every hop is a rel8 jump
	// (2 bytes) and the last slot holds the rel32 jump (5 bytes). Reserves the bytes it uses.
	constexpr uint32_t MaxHops = 4;
	const auto find_path = [&](uint64_t next_ip, std::array<uint64_t, MaxHops>& path) -> uint32_t {
		struct Node {
			uint64_t next_ip;
			uint32_t depth;
			int32_t  parent;
			size_t   cave;
		};
		std::vector<Node> nodes {{next_ip, 0, -1, SIZE_MAX}};
		for (size_t n = 0; n < nodes.size(); n++) {
			const auto     node = nodes[n];
			const uint64_t low  = node.next_ip >= 128 ? node.next_ip - 128 : 0;
			const uint64_t high = node.next_ip + 127;
			auto           it =
			    std::lower_bound(caves.begin(), caves.end(), low, [](const Cave& cave, uint64_t v) {
				    return cave.start + cave.size <= v;
			    });
			for (; it != caves.end() && it->start <= high; ++it) {
				const size_t   index = static_cast<size_t>(it - caves.begin());
				const uint64_t slot  = it->start + it->used;
				if (slot < low || slot > high) {
					continue;
				}
				bool on_path = false;
				for (int32_t k = static_cast<int32_t>(n); k >= 0 && !on_path;
				     k         = nodes[static_cast<size_t>(k)].parent) {
					on_path = nodes[static_cast<size_t>(k)].cave == index;
				}
				if (on_path) {
					continue;
				}
				const uint64_t free = it->size - it->used;
				if (free >= JumpSize) {
					// Reserve: the terminal slot, then each hop back to the start.
					uint32_t count  = node.depth + 1;
					path[count - 1] = slot;
					it->used += JumpSize;
					uint32_t i = count - 1;
					for (int32_t k = static_cast<int32_t>(n); k > 0;
					     k         = nodes[static_cast<size_t>(k)].parent) {
						auto& hop = caves[nodes[static_cast<size_t>(k)].cave];
						path[--i] = hop.start + hop.used;
						hop.used += ShortJumpSize;
					}
					return count;
				}
				if (free >= ShortJumpSize && node.depth + 1 < MaxHops && nodes.size() < 4096) {
					nodes.push_back(
					    {slot + ShortJumpSize, node.depth + 1, static_cast<int32_t>(n), index});
				}
			}
		}
		return 0;
	};

	// Phase 0 patches stores that hold a rel32 jump; their dead bytes become more slots. Phase 1
	// patches the short ones.
	for (int phase = 0; phase < 2; phase++) {
		if (phase == 1) {
			std::sort(caves.begin(), caves.end(),
			          [](const Cave& a, const Cave& b) { return a.start < b.start; });
		}
		// Bytes before skip_until were rewritten by a moved-instruction patch in this phase.
		uint64_t skip_until = 0;
		for (const uint64_t start: candidates) {
			if (start < skip_until) {
				continue;
			}
			uint64_t                offset = start;
			auto*                   code   = reinterpret_cast<uint8_t*>(address + offset);
			ZydisDecodedInstruction instruction {};
			ZydisDecodedOperand     operands[ZYDIS_MAX_OPERAND_COUNT] {};
			if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code, size - offset, &instruction,
			                                         operands))) {
				continue;
			}
			const uint64_t length = instruction.length;
			offset += length;
			if (!IsPlainWideStore(instruction, operands) || (phase == 0) != (length >= JumpSize)) {
				continue;
			}
			result.candidates++;
			uint64_t                      cave = 0;
			std::array<uint64_t, MaxHops> path {};
			uint32_t                      hops = 0;
			// No padding in reach: the store becomes ud2, and the illegal-instruction handler sends
			// execution to the trampoline (see TryRedirectWideStore). Costs a signal per execution.
			bool trap = false;
			if (length < JumpSize) {
				hops = length >= ShortJumpSize
				           ? find_path(reinterpret_cast<uint64_t>(code) + ShortJumpSize, path)
				           : 0;
				cave = hops != 0 ? path[hops - 1] : 0;
				if (cave == 0) {
					trap = length >= ShortJumpSize;
					if (!trap) {
						result.too_short++;
						continue;
					}
				}
			}
			// A hot trapped site (from the profile) moves its following instructions into the
			// trampoline: up to four, no control flow, not a direct branch target, re-encodable.
			uint64_t                moved_original = 0;
			std::array<uint64_t, 4> moved_ips {};
			uint32_t                moved_count = 0;
			// Moving instructions is only safe when nothing jumps into them. The sweep sees direct
			// branch targets only, not jump tables, computed jumps or landing pads, so by default
			// only sites from the hot profile are moved. KYTY_WIDE_STORE_MOVE=all also moves at
			// unprofiled sites.
			static const bool move_all = [] {
				const char* value = std::getenv("KYTY_WIDE_STORE_MOVE");
				return value != nullptr && std::strcmp(value, "all") == 0;
			}();
			if (trap && (move_all || std::binary_search(hot_sites.begin(), hot_sites.end(),
			                                            reinterpret_cast<uint64_t>(code)))) {
				while (length + moved_original < JumpSize && moved_count < moved_ips.size()) {
					const uint64_t ip = reinterpret_cast<uint64_t>(code) + length + moved_original;
					ZydisDecodedInstruction next {};
					ZydisDecodedOperand     next_ops[ZYDIS_MAX_OPERAND_COUNT] {};
					if (ip >= address + size ||
					    std::binary_search(branch_targets.begin(), branch_targets.end(), ip) ||
					    !ZYAN_SUCCESS(
					        ZydisDecoderDecodeFull(&decoder, reinterpret_cast<uint8_t*>(ip),
					                               address + size - ip, &next, next_ops)) ||
					    next.meta.category == ZYDIS_CATEGORY_RET ||
					    next.meta.category == ZYDIS_CATEGORY_SYSTEM ||
					    next.meta.category == ZYDIS_CATEGORY_INTERRUPT ||
					    next.meta.category == ZYDIS_CATEGORY_CALL ||
					    IsPlainWideStore(next, next_ops) || UsesSegmentBase(next, next_ops)) {
						break;
					}
					// A relative jmp/jcc (a loop's back edge) is re-encoded with its absolute
					// target and ends the moved sequence; other control flow is not moved.
					const bool branch = next.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE;
					if (branch && ((next.attributes & ZYDIS_ATTRIB_IS_RELATIVE) == 0 ||
					               (next.meta.category != ZYDIS_CATEGORY_UNCOND_BR &&
					                next.meta.category != ZYDIS_CATEGORY_COND_BR))) {
						break;
					}
					moved_ips[moved_count++] = ip;
					moved_original += next.length;
					if (branch) {
						break;
					}
				}
				if (length + moved_original >= JumpSize) {
					trap = false;
				} else {
					moved_count    = 0;
					moved_original = 0;
				}
			}
			const auto& mem = operands[0];
			if (mem.mem.segment != ZYDIS_REGISTER_DS && mem.mem.segment != ZYDIS_REGISTER_SS &&
			    mem.mem.segment != ZYDIS_REGISTER_NONE) {
				result.unsupported++;
				continue;
			}
			const auto ymm = operands[1].reg.value;
			const auto xmm =
			    static_cast<ZydisRegister>(ZYDIS_REGISTER_XMM0 + (ymm - ZYDIS_REGISTER_YMM0));

			// Low half: vmovups xmmword [mem], xmmN
			ZydisEncoderRequest low {};
			low.machine_mode    = ZYDIS_MACHINE_MODE_LONG_64;
			low.mnemonic        = ZYDIS_MNEMONIC_VMOVUPS;
			low.operand_count   = 2;
			const uint64_t site = reinterpret_cast<uint64_t>(code);
			const uint64_t back = site + length;
			SetMemoryOperand(low.operands[0], mem, back, 0);
			low.operands[1].type      = ZYDIS_OPERAND_TYPE_REGISTER;
			low.operands[1].reg.value = xmm;
			// High half: vextractf128 xmmword [mem + 16], ymmN, 1
			ZydisEncoderRequest high {};
			high.machine_mode  = ZYDIS_MACHINE_MODE_LONG_64;
			high.mnemonic      = ZYDIS_MNEMONIC_VEXTRACTF128;
			high.operand_count = 3;
			SetMemoryOperand(high.operands[0], mem, back, 16);
			high.operands[1].type      = ZYDIS_OPERAND_TYPE_REGISTER;
			high.operands[1].reg.value = ymm;
			high.operands[2].type      = ZYDIS_OPERAND_TYPE_IMMEDIATE;
			high.operands[2].imm.u     = 1;

			const uint64_t trampoline = *cursor;
			uint8_t        buffer[128];
			uint64_t       low_length  = 0;
			uint64_t       high_length = 0;
			if (!EncodeAt(low, trampoline, buffer, sizeof(buffer), &low_length) ||
			    !EncodeAt(high, trampoline + low_length, buffer + low_length,
			              sizeof(buffer) - low_length, &high_length)) {
				result.unsupported++;
				continue;
			}
			uint64_t moved_length = 0;
			bool     moved_ok     = true;
			for (uint32_t i = 0; i < moved_count && moved_ok; i++) {
				const auto              ip = moved_ips[i];
				ZydisDecodedInstruction insn {};
				ZydisDecodedOperand     ops[ZYDIS_MAX_OPERAND_COUNT] {};
				ZydisEncoderRequest     request {};
				moved_ok =
				    ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, reinterpret_cast<uint8_t*>(ip),
				                                        address + size - ip, &insn, ops)) &&
				    ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(
				        &insn, ops, insn.operand_count_visible, &request));
				if (!moved_ok) {
					break;
				}
				// Nops (alignment padding that execution falls through) are dropped: re-encoding
				// a 15-byte "nop cs:[rax + rax]" produced a load of address 0 in the test title.
				if (insn.mnemonic == ZYDIS_MNEMONIC_NOP) {
					continue;
				}
				for (uint32_t k = 0; k < request.operand_count; k++) {
					auto& op = request.operands[k];
					if (op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RIP) {
						op.mem.displacement += static_cast<int64_t>(ip + insn.length);
					}
				}
				if ((insn.attributes & ZYDIS_ATTRIB_IS_RELATIVE) != 0 &&
				    insn.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
					ZyanU64 target = 0;
					moved_ok = ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&insn, &ops[0], ip, &target));
					request.operands[0].imm.u = target;
					request.branch_type       = ZYDIS_BRANCH_TYPE_NONE;
					request.branch_width      = ZYDIS_BRANCH_WIDTH_NONE;
					if (!moved_ok) {
						break;
					}
				}
				const uint64_t used    = low_length + high_length + moved_length;
				uint64_t       encoded = 0;
				moved_ok               = EncodeAt(request, trampoline + used, buffer + used,
				                                  sizeof(buffer) - used, &encoded);
				moved_length += encoded;
			}
			if (!moved_ok) {
				result.unsupported++;
				continue;
			}
			const uint64_t prologue = low_length + high_length + moved_length;
			const uint64_t body     = prologue + JumpSize;
			if (trampoline + body > trampoline_end) {
				result.unsupported++;
				continue;
			}
			const uint64_t jump_from = cave != 0 ? cave : site;
			const int64_t  to_tramp =
			    static_cast<int64_t>(trampoline) - static_cast<int64_t>(jump_from + JumpSize);
			const int64_t to_back = static_cast<int64_t>(back + moved_original) -
			                        static_cast<int64_t>(trampoline + prologue + JumpSize);
			if (to_tramp < INT32_MIN || to_tramp > INT32_MAX || to_back < INT32_MIN ||
			    to_back > INT32_MAX) {
				result.unsupported++;
				continue;
			}
			auto* out = reinterpret_cast<uint8_t*>(trampoline);
			std::memcpy(out, buffer, prologue);
			out[prologue]     = 0xE9;
			const auto back32 = static_cast<int32_t>(to_back);
			std::memcpy(out + prologue + 1, &back32, sizeof(back32));
			*cursor += body;
			result.trampoline_bytes += body;

			const auto tramp32 = static_cast<int32_t>(to_tramp);
			if (cave != 0) {
				// site: jmp rel8 -> [hop: jmp rel8 ->]... slot: jmp rel32 -> trampoline
				auto* slot = reinterpret_cast<uint8_t*>(cave);
				slot[0]    = 0xE9;
				std::memcpy(slot + 1, &tramp32, sizeof(tramp32));
				uint64_t from = site;
				for (uint32_t h = 0; h < hops; h++) {
					auto* jump = reinterpret_cast<uint8_t*>(from);
					jump[0]    = 0xEB;
					jump[1]    = static_cast<uint8_t>(
					    static_cast<int8_t>(static_cast<int64_t>(path[h]) -
					                        static_cast<int64_t>(from + ShortJumpSize)));
					from = path[h];
				}
				std::memset(code + ShortJumpSize, 0xCC, length - ShortJumpSize);
				result.via_cave++;
				result.patched++;
				continue;
			}
			if (trap) {
				if (!RegisterWideStoreTrap(site, trampoline)) {
					result.unsupported++;
					continue;
				}
				code[0] = 0x0F; // ud2
				code[1] = 0x0B;
				std::memset(code + ShortJumpSize, 0xCC, length - ShortJumpSize);
				result.trapped++;
				result.patched++;
				continue;
			}
			// The original instruction (with any moved ones) becomes the jump; the remaining bytes
			// are never executed.
			code[0] = 0xE9;
			std::memcpy(code + 1, &tramp32, sizeof(tramp32));
			std::memset(code + JumpSize, 0xCC, length + moved_original - JumpSize);
			if (moved_count != 0) {
				result.relocated++;
				skip_until = offset + moved_original;
			}
			if (phase == 0 && length >= JumpSize + ShortJumpSize) {
				caves.push_back({site + JumpSize, length - JumpSize, 0});
			}
			result.patched++;
		}
	}
	return result;
}

bool TryEmulate(void* native_context) {
	if (native_context == nullptr) {
		return false;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	Context context {static_cast<PCONTEXT>(native_context)};
#elif defined(__APPLE__)
	auto* saved_context = static_cast<ucontext_t*>(native_context);
	if (saved_context->uc_mcontext == nullptr) {
		return false;
	}
	Context context {saved_context};
#else
	Context context {static_cast<ucontext_t*>(native_context)};
#endif
	if (auto* trap = FindWideStoreTrapEntry(context.Rip()); trap != nullptr) {
		const auto trampoline = trap->trampoline.load(std::memory_order_acquire);
		if (trap->hits.fetch_add(1, std::memory_order_relaxed) + 1 == WideStoreHotHits) {
			RecordHotWideStore(context.Rip());
		}
		const auto hits = g_wide_store_trap_hits.fetch_add(1, std::memory_order_relaxed) + 1;
		if ((hits & (hits - 1)) == 0) {
			LOGF("[wide-store-trap] hits=%llu last site=0x%llx\n",
			     static_cast<unsigned long long>(hits),
			     static_cast<unsigned long long>(context.Rip()));
		}
		context.SetRip(trampoline);
		return true;
	}
#if !defined(__APPLE__)
	return TryEmulateReciprocalSquareRoot(context) || TryEmulateMonitorxMwaitx(context) ||
	       TryEmulateSse4a(context) || TryEmulateShaNi(context) || TryEmulateCpuExtensions(context);
#else
	return TryEmulateSse4a(context) || TryEmulateCpuExtensions(context);
#endif
}

} // namespace Loader::X64InstructionEmulator
