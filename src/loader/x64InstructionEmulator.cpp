#include "loader/x64InstructionEmulator.h"

#include "common/common.h"
#include "common/crashDiagnostics.h"
#include "common/fatalLog.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#elif !defined(__APPLE__)
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

static uint32_t Rol32(uint32_t value, unsigned int shift) {
	shift &= 31u;
	return (value << shift) | (value >> (32u - shift));
}

static uint32_t Rotr32(uint32_t value, unsigned int shift) {
	shift &= 31u;
	return (value >> shift) | (value << (32u - shift));
}

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
	const uint32_t w16 = Rol32(dest.w[3] ^ w13, 1u);
	const uint32_t w17 = Rol32(dest.w[2] ^ w14, 1u);
	const uint32_t w18 = Rol32(dest.w[1] ^ w15, 1u);
	const uint32_t w19 = Rol32(dest.w[0] ^ w16, 1u);
	dest.w[3]          = w16;
	dest.w[2]          = w17;
	dest.w[1]          = w18;
	dest.w[0]          = w19;
}

static void Sha1Nexte(XmmWords& dest, const XmmWords& src2) {
	const uint32_t tmp = Rol32(dest.w[3], 30u);
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
		uint32_t term = Sha1RoundFunc(group, b, c, d) + Rol32(a, 5u) + w[round] + k;
		if (round > 0u) {
			term += e;
		}
		const uint32_t a1 = term;
		e                 = d;
		d                 = c;
		c                 = Rol32(b, 30u);
		b                 = a;
		a                 = a1;
	}

	dest.w[3] = a;
	dest.w[2] = b;
	dest.w[1] = c;
	dest.w[0] = d;
}

static uint32_t Sha256Sigma0(uint32_t x) {
	return Rotr32(x, 7u) ^ Rotr32(x, 18u) ^ (x >> 3u);
}

static uint32_t Sha256Sigma1(uint32_t x) {
	return Rotr32(x, 17u) ^ Rotr32(x, 19u) ^ (x >> 10u);
}

static uint32_t Sha256Sum0(uint32_t x) {
	return Rotr32(x, 2u) ^ Rotr32(x, 13u) ^ Rotr32(x, 22u);
}

static uint32_t Sha256Sum1(uint32_t x) {
	return Rotr32(x, 6u) ^ Rotr32(x, 11u) ^ Rotr32(x, 25u);
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

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

static void LoadXmmWordsWin(const M128A* xmm, XmmWords& out) {
	out.w[0] = static_cast<uint32_t>(xmm->Low);
	out.w[1] = static_cast<uint32_t>(xmm->Low >> 32u);
	out.w[2] = static_cast<uint32_t>(xmm->High);
	out.w[3] = static_cast<uint32_t>(xmm->High >> 32u);
}

static void StoreXmmWordsWin(M128A* xmm, const XmmWords& in) {
	xmm->Low  = static_cast<uint64_t>(in.w[0]) | (static_cast<uint64_t>(in.w[1]) << 32u);
	xmm->High = static_cast<uint64_t>(in.w[2]) | (static_cast<uint64_t>(in.w[3]) << 32u);
}

#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

static M128A* GetContextXmm(PCONTEXT context, uint8_t index) {
	if (context == nullptr || index >= 16) {
		return nullptr;
	}

	return &context->Xmm0 + index;
}

static uint64_t* GetGpReg(PCONTEXT context, uint8_t index) {
	if (context == nullptr) {
		return nullptr;
	}
	switch (index & 0x0fu) {
		case 0: return &context->Rax;
		case 1: return &context->Rcx;
		case 2: return &context->Rdx;
		case 3: return &context->Rbx;
		case 4: return &context->Rsp;
		case 5: return &context->Rbp;
		case 6: return &context->Rsi;
		case 7: return &context->Rdi;
		case 8: return &context->R8;
		case 9: return &context->R9;
		case 10: return &context->R10;
		case 11: return &context->R11;
		case 12: return &context->R12;
		case 13: return &context->R13;
		case 14: return &context->R14;
		case 15: return &context->R15;
		default: return nullptr;
	}
}

static void LoadContextGprsWin(PCONTEXT context, uint64_t (&gpr)[16]) {
	gpr[0]  = context->Rax;
	gpr[1]  = context->Rcx;
	gpr[2]  = context->Rdx;
	gpr[3]  = context->Rbx;
	gpr[4]  = context->Rsp;
	gpr[5]  = context->Rbp;
	gpr[6]  = context->Rsi;
	gpr[7]  = context->Rdi;
	gpr[8]  = context->R8;
	gpr[9]  = context->R9;
	gpr[10] = context->R10;
	gpr[11] = context->R11;
	gpr[12] = context->R12;
	gpr[13] = context->R13;
	gpr[14] = context->R14;
	gpr[15] = context->R15;
}

static bool TryEmulateShaNi(PCONTEXT context) {
	if (context == nullptr) {
		return false;
	}

	const auto* rip = reinterpret_cast<const uint8_t*>(context->Rip);
	ShaNiInsn   insn {};
	if (!DecodeShaNiInsn(rip, insn)) {
		return false;
	}

	const uint8_t modrm_byte = rip[insn.modrm_offset];
	const uint8_t dest_index = ShaNiRegIndex(modrm_byte, insn.rex, true);
	auto*         dest_xmm   = GetContextXmm(context, dest_index);
	auto*         xmm0       = GetContextXmm(context, 0);
	if (dest_xmm == nullptr || xmm0 == nullptr) {
		return false;
	}

	XmmWords dest {};
	XmmWords src2 {};
	XmmWords xmm0_words {};
	LoadXmmWordsWin(dest_xmm, dest);
	LoadXmmWordsWin(xmm0, xmm0_words);

	if (ShaNiModrmIsRegister(modrm_byte)) {
		const uint8_t src_index = ShaNiRegIndex(modrm_byte, insn.rex, false);
		auto*         src_xmm   = GetContextXmm(context, src_index);
		if (src_xmm == nullptr) {
			return false;
		}
		LoadXmmWordsWin(src_xmm, src2);
	} else {
		uint64_t    gpr[16] {};
		const void* source = nullptr;
		LoadContextGprsWin(context, gpr);
		if (!ResolveShaNiMemoryAddress(rip, insn, gpr, source)) {
			return false;
		}
		std::memcpy(&src2, source, sizeof(src2));
	}

	if (!ExecuteShaNiInsn(insn, src2, xmm0_words, dest)) {
		return false;
	}

	StoreXmmWordsWin(dest_xmm, dest);
	context->Rip += insn.length;
	return true;
}

static bool TryEmulateSse4a(PCONTEXT context) {
	if (context == nullptr) {
		return false;
	}

	const auto* rip = reinterpret_cast<const uint8_t*>(context->Rip);

	const uint8_t prefix = rip[0];
	if (prefix != 0x66 && prefix != 0xf2) {
		return false;
	}

	size_t  offset = 1;
	uint8_t rex    = 0;
	if ((rip[offset] & 0xf0u) == 0x40u) {
		rex = rip[offset];
		offset++;
	}

	if (rip[offset] != 0x0f || rip[offset + 1] != 0x78) {
		return false;
	}

	auto modrm = rip[offset + 2];
	if ((modrm & 0xc0u) != 0xc0u) {
		return false;
	}

	const uint8_t reg    = ((modrm >> 3u) & 0x07u) | ((rex & 0x04u) << 1u);
	const uint8_t rm     = (modrm & 0x07u) | ((rex & 0x01u) << 3u);
	const uint8_t length = rip[offset + 3];
	const uint8_t index  = rip[offset + 4];

	// AMD SSE4a immediate-form EXTRQ/INSERTQ. PS5 code can execute these natively on AMD hardware,
	// while Intel hosts raise an illegal-instruction exception.
	if (prefix == 0x66) {
		auto* dst = GetContextXmm(context, rm);
		if (dst == nullptr) {
			return false;
		}

		dst->Low  = ExtractBitField(dst->Low, length, index);
		dst->High = 0;
		context->Rip += offset + 5;
		return true;
	}

	auto* dst = GetContextXmm(context, reg);
	auto* src = GetContextXmm(context, rm);
	if (dst == nullptr || src == nullptr) {
		return false;
	}

	dst->Low = InsertBitField(dst->Low, src->Low, length, index);
	context->Rip += offset + 5;
	return true;
}

static bool TryEmulateMonitorxMwaitx(PCONTEXT context) {
	if (context == nullptr) {
		return false;
	}

	const auto* rip = reinterpret_cast<const uint8_t*>(context->Rip);
	if (rip[0] != 0x0f || rip[1] != 0x01 || (rip[2] != 0xfa && rip[2] != 0xfb)) {
		return false;
	}

	// AMD MONITORX/MWAITX are used by PS5 code in wait loops. Intel hosts can raise an illegal-
	// instruction exception, so approximate them as a no-op/yield pair.
	if (rip[2] == 0xfb) {
		SwitchToThread();
	}
	context->Rip += 3;
	return true;
}

struct DecodedMemOp {
	size_t  length       = 0;
	uint8_t reg          = 0;
	bool    is_xmm       = false;
	bool    is_write     = false;
	bool    is_compare   = false;
	bool    has_modrm    = false;
	bool    is_memory    = false;
	uint8_t operand_bits = 64; // 8/16/32/64 for GP; ignored for XMM zeroing
	uint64_t immediate   = 0;
	bool     sign_extend_immediate = false;
};

static bool DecodeMemOp(const uint8_t* rip, DecodedMemOp* out) {
	if (rip == nullptr || out == nullptr) {
		return false;
	}

	size_t  i           = 0;
	uint8_t rex         = 0;
	bool    op_size_16  = false;
	bool    repnz       = false;
	bool    repz        = false;
	bool    addr_size32 = false;

	// Legacy prefixes (cap to avoid runaway).
	for (int guard = 0; guard < 14; ++guard) {
		const uint8_t b = rip[i];
		if (b == 0x66) {
			op_size_16 = true;
			++i;
			continue;
		}
		if (b == 0x67) {
			addr_size32 = true;
			++i;
			continue;
		}
		if (b == 0xf2) {
			repnz = true;
			++i;
			continue;
		}
		if (b == 0xf3) {
			repz = true;
			++i;
			continue;
		}
		if (b == 0x2e || b == 0x36 || b == 0x3e || b == 0x26 || b == 0x64 || b == 0x65) {
			++i;
			continue;
		}
		break;
	}
	(void)addr_size32;

	if ((rip[i] & 0xf0u) == 0x40u) {
		rex = rip[i++];
	}

	DecodedMemOp decoded {};
	decoded.is_write = false;

	auto parse_modrm = [&](size_t at) -> bool {
		const uint8_t modrm = rip[at++];
		const uint8_t mod   = static_cast<uint8_t>((modrm >> 6u) & 0x3u);
		const uint8_t rm    = static_cast<uint8_t>(modrm & 0x7u);
		decoded.reg         = static_cast<uint8_t>(((modrm >> 3u) & 0x7u) | ((rex & 0x04u) << 1u));
		decoded.has_modrm   = true;
		if (mod == 3) {
			decoded.is_memory = false;
			decoded.length    = at;
			return true;
		}
		decoded.is_memory = true;
		if (rm == 4) {
			const uint8_t sib  = rip[at++];
			const uint8_t base = static_cast<uint8_t>(sib & 0x7u);
			if (mod == 0 && base == 5) {
				at += 4; // disp32 with no base
			} else if (mod == 1) {
				at += 1;
			} else if (mod == 2) {
				at += 4;
			}
		} else if (mod == 0 && rm == 5) {
			at += 4; // RIP-relative or abs disp32
		} else if (mod == 1) {
			at += 1;
		} else if (mod == 2) {
			at += 4;
		}
		decoded.length = at;
		return true;
	};

	const uint8_t op0 = rip[i++];

	// Two-byte opcodes.
	if (op0 == 0x0f) {
		const uint8_t op1 = rip[i++];
		// movzx / movsx
		if (op1 == 0xb6 || op1 == 0xb7 || op1 == 0xbe || op1 == 0xbf) {
			decoded.is_write     = false;
			decoded.operand_bits = (rex & 0x08u) != 0 ? 64 : (op_size_16 ? 16 : 32);
			if (!parse_modrm(i)) {
				return false;
			}
			*out = decoded;
			return decoded.is_memory;
		}
		// movups/movaps/movdqa/movdqu loads: 0F 10/28/6F (reg <- mem)
		if (op1 == 0x10 || op1 == 0x28 || op1 == 0x6f) {
			decoded.is_write = false;
			decoded.is_xmm   = true;
			if (!parse_modrm(i)) {
				return false;
			}
			*out = decoded;
			return decoded.is_memory;
		}
		// stores 0F 11/29/7F
		if (op1 == 0x11 || op1 == 0x29 || op1 == 0x7f) {
			decoded.is_write = true;
			decoded.is_xmm   = true;
			if (!parse_modrm(i)) {
				return false;
			}
			*out = decoded;
			return decoded.is_memory;
		}
		return false;
	}

	// VEX-prefixed AVX instructions (c5 = 2-byte, c4 = 3-byte).
	// Guest code (e.g. TLOU eboot) stores 256-bit vectors into GPU-tracked memory
	// (vmovups [rbx], ymm0 = c5 fc 11 03). Without this, soft-continue rejects the
	// opcode and the guest thread SoftIdles → no one signals mid-IB fences → hang.
	if (op0 == 0xc5 || op0 == 0xc4) {
		uint8_t  vex_r  = 0;
		uint8_t  vex_l  = 0;
		uint8_t  vex_pp = 0;
		uint16_t map    = 0x0f;
		if (op0 == 0xc5) {
			const uint8_t b2 = rip[i++];
			vex_r  = static_cast<uint8_t>((b2 >> 7u) & 1u);
			vex_l  = static_cast<uint8_t>((b2 >> 2u) & 1u);
			vex_pp = static_cast<uint8_t>(b2 & 3u);
		} else {
			const uint8_t b2 = rip[i++];
			const uint8_t b3 = rip[i++];
			vex_r  = static_cast<uint8_t>((b2 >> 7u) & 1u);
			vex_l  = static_cast<uint8_t>((b3 >> 2u) & 1u);
			vex_pp = static_cast<uint8_t>(b3 & 3u);
			const uint8_t mmmmm = static_cast<uint8_t>(b2 & 0x1fu);
			map = mmmmm == 1 ? 0x0f : (mmmmm == 2 ? 0x0f38 : (mmmmm == 3 ? 0x0f3a : 0));
		}
		(void)vex_l;
		(void)vex_pp;
		if (map == 0) {
			return false;
		}

		const uint8_t op1 = rip[i++];

		// VEX loads: 0F 10/28/6F (reg <- mem)
		if (map == 0x0f && (op1 == 0x10 || op1 == 0x28 || op1 == 0x6f)) {
			decoded.is_write = false;
			decoded.is_xmm   = true;
			if (!parse_modrm(i)) {
				return false;
			}
			// VEX.R is inverted: R=1 means no high-bit extension.
			decoded.reg = static_cast<uint8_t>(((~vex_r & 1u) << 3u) | (decoded.reg & 7u));
			*out = decoded;
			return decoded.is_memory;
		}
		// VEX stores: 0F 11/29/7F (mem <- reg)
		if (map == 0x0f && (op1 == 0x11 || op1 == 0x29 || op1 == 0x7f)) {
			decoded.is_write = true;
			decoded.is_xmm   = true;
			if (!parse_modrm(i)) {
				return false;
			}
			*out = decoded;
			return decoded.is_memory;
		}
		return false;
	}

	// GP moves.
	if (op0 == 0x8b) { // mov r, r/m
		decoded.is_write     = false;
		decoded.operand_bits = (rex & 0x08u) != 0 ? 64 : (op_size_16 ? 16 : 32);
		if (!parse_modrm(i)) {
			return false;
		}
		*out = decoded;
		return decoded.is_memory;
	}
	if (op0 == 0x8a) { // mov r8, r/m8
		decoded.is_write     = false;
		decoded.operand_bits = 8;
		if (!parse_modrm(i)) {
			return false;
		}
		*out = decoded;
		return decoded.is_memory;
	}
	if (op0 == 0x89) { // mov r/m, r
		decoded.is_write = true;
		if (!parse_modrm(i)) {
			return false;
		}
		*out = decoded;
		return decoded.is_memory;
	}
	if (op0 == 0x88) { // mov r/m8, r8
		decoded.is_write = true;
		if (!parse_modrm(i)) {
			return false;
		}
		*out = decoded;
		return decoded.is_memory;
	}
	if (op0 == 0x80 || op0 == 0x81 || op0 == 0x83) { // cmp r/m, imm
		const auto modrm = rip[i];
		if (((modrm >> 3u) & 0x7u) != 7u) {
			return false;
		}

		decoded.is_compare   = true;
		decoded.operand_bits = op0 == 0x80 ? 8 : ((rex & 0x08u) != 0 ? 64
		                                                               : (op_size_16 ? 16 : 32));
		if (!parse_modrm(i)) {
			return false;
		}

		const size_t immediate_size =
		    op0 == 0x80 || op0 == 0x83 ? 1 : (op_size_16 ? 2 : 4);
		for (size_t byte = 0; byte < immediate_size; byte++) {
			decoded.immediate |= static_cast<uint64_t>(rip[decoded.length + byte]) << (byte * 8u);
		}
		decoded.sign_extend_immediate = op0 == 0x83 || (op0 == 0x81 && decoded.operand_bits == 64);
		decoded.length += immediate_size;
		*out = decoded;
		return decoded.is_memory;
	}
	if (op0 == 0x63) { // movsxd
		decoded.is_write     = false;
		decoded.operand_bits = 64;
		if (!parse_modrm(i)) {
			return false;
		}
		*out = decoded;
		return decoded.is_memory;
	}
	if (op0 == 0xc7) { // mov r/m, imm32
		decoded.is_write = true;
		if (!parse_modrm(i)) {
			return false;
		}
		decoded.length += (rex & 0x08u) != 0 ? 4 : (op_size_16 ? 2 : 4);
		*out = decoded;
		return decoded.is_memory;
	}
	if (op0 == 0xc6) { // mov r/m8, imm8
		decoded.is_write = true;
		if (!parse_modrm(i)) {
			return false;
		}
		decoded.length += 1;
		*out = decoded;
		return decoded.is_memory;
	}

	(void)repz;
	(void)repnz;

	return false;
}

static void LogPoisonReject(const char* reason, PCONTEXT context, uint64_t fault_vaddr,
                            bool is_write) {
	static std::atomic<uint32_t> reject_n {0};
	const uint32_t               n = reject_n.fetch_add(1, std::memory_order_relaxed) + 1;
	if (n > 16) {
		return;
	}

	const uint64_t rip = context != nullptr ? context->Rip : 0;
	char           line[768] {};
	int            length = std::snprintf(
        line, sizeof(line),
        "MemoryTrace: poison reject n=%u reason=%s %s vaddr=0x%016" PRIx64
        " rip=0x%016" PRIx64,
        n, reason != nullptr ? reason : "unknown", is_write ? "Write" : "Read", fault_vaddr, rip);
	if (length < 0) {
		return;
	}
	if (length >= static_cast<int>(sizeof(line))) {
		length = static_cast<int>(sizeof(line) - 1);
	}

	size_t readable = 0;
	if (context != nullptr && rip != 0) {
		MEMORY_BASIC_INFORMATION mbi {};
		if (VirtualQuery(reinterpret_cast<const void*>(rip), &mbi, sizeof(mbi)) != 0 &&
		    mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0) {
			const auto region_start = reinterpret_cast<uint64_t>(mbi.BaseAddress);
			const auto region_size  = static_cast<uint64_t>(mbi.RegionSize);
			const auto region_end =
			    region_start <= UINT64_MAX - region_size ? region_start + region_size : UINT64_MAX;
			if (rip < region_end) {
				const auto available = region_end - rip;
				readable = available < 15 ? static_cast<size_t>(available) : 15;
			}
		}
	}

	int written = std::snprintf(line + length, sizeof(line) - static_cast<size_t>(length),
	                            " bytes=");
	if (written > 0) {
		length += written;
	}
	if (readable == 0) {
		std::snprintf(line + length, sizeof(line) - static_cast<size_t>(length), "unavailable");
	} else {
		const auto* bytes = reinterpret_cast<const uint8_t*>(rip);
		for (size_t i = 0; i < readable && length < static_cast<int>(sizeof(line) - 4); i++) {
			written = std::snprintf(line + length, sizeof(line) - static_cast<size_t>(length),
			                        "%s%02" PRIx32, i == 0 ? "" : " ",
			                        static_cast<uint32_t>(bytes[i]));
			if (written <= 0) {
				break;
			}
			length += written;
		}
	}

	Common::LogFatalToFile(line);
	std::fprintf(stderr, "%s\n", line);
	std::fflush(stderr);
}

static void ZeroGpDest(PCONTEXT context, uint8_t reg, uint8_t bits) {
	uint64_t* gp = GetGpReg(context, reg);
	if (gp == nullptr) {
		return;
	}
	if (bits >= 64) {
		*gp = 0;
	} else if (bits == 32) {
		*gp = 0; // 32-bit writes zero-extend
	} else if (bits == 16) {
		*gp = (*gp & ~uint64_t {0xffff}) | 0;
	} else {
		// 8-bit: low byte; ignore AH complexity for poison soft-continue
		*gp = (*gp & ~uint64_t {0xff}) | 0;
	}
}

static void ZeroXmmDest(PCONTEXT context, uint8_t reg) {
	M128A* xmm = GetContextXmm(context, reg);
	if (xmm == nullptr) {
		return;
	}
	xmm->Low  = 0;
	xmm->High = 0;
}

static void SetPoisonCompareFlags(PCONTEXT context, const DecodedMemOp& op) {
	if (context == nullptr) {
		return;
	}

	const uint64_t width_mask =
	    op.operand_bits >= 64 ? UINT64_MAX : ((uint64_t {1} << op.operand_bits) - 1u);
	const uint64_t sign_bit = uint64_t {1} << (op.operand_bits - 1u);
	const uint64_t left      = 0;
	uint64_t       right     = op.immediate;

	if (op.sign_extend_immediate) {
		const auto signed_value =
		    op.immediate & (op.operand_bits == 64 ? UINT64_MAX : ((uint64_t {1} << 32u) - 1u));
		if (op.operand_bits == 64) {
			right = static_cast<uint64_t>(static_cast<int64_t>(signed_value));
		} else if (op.operand_bits == 32) {
			right = static_cast<uint32_t>(static_cast<int32_t>(signed_value));
		} else {
			right = static_cast<uint16_t>(static_cast<int16_t>(signed_value));
		}
	}

	right &= width_mask;
	const auto result = (left - right) & width_mask;

	uint8_t parity = static_cast<uint8_t>(result);
	parity ^= static_cast<uint8_t>(parity >> 4u);
	parity ^= static_cast<uint8_t>(parity >> 2u);
	parity ^= static_cast<uint8_t>(parity >> 1u);

	constexpr uint32_t status_flags = 0x000008d5u; // OF, SF, ZF, AF, PF, CF
	uint32_t           flags        = context->EFlags & ~status_flags;
	if (left < right) {
		flags |= 0x00000001u; // CF
	}
	if ((parity & 1u) == 0) {
		flags |= 0x00000004u; // PF
	}
	if (((left ^ right ^ result) & 0x10u) != 0) {
		flags |= 0x00000010u; // AF
	}
	if (result == 0) {
		flags |= 0x00000040u; // ZF
	}
	if ((result & sign_bit) != 0) {
		flags |= 0x00000080u; // SF
	}
	if (((left ^ right) & (left ^ result) & sign_bit) != 0) {
		flags |= 0x00000800u; // OF
	}
	context->EFlags = flags;
}

bool TrySoftContinuePoisonAccess(void* native_context, uint64_t fault_vaddr, bool is_write,
                                 bool force, bool allow_system_module) {
	auto* context = static_cast<PCONTEXT>(native_context);
	if (context == nullptr) {
		LogPoisonReject("null-context", context, fault_vaddr, is_write);
		return false;
	}

	// Never soft-continue inside foreign modules (ntdll / kernel32 / …) unless the caller
	// explicitly opts in after orphan-commit failed (otherwise stack walks loop forever).
	{
		if (!allow_system_module && context->Rip >= 0x00007FF000000000ull) {
			LogPoisonReject("system-rip", context, fault_vaddr, is_write);
			return false;
		}
		HMODULE self  = GetModuleHandleA(nullptr);
		HMODULE owner = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       reinterpret_cast<LPCSTR>(context->Rip), &owner) != 0 &&
		    owner != nullptr && self != nullptr && owner != self) {
			if (!allow_system_module) {
				LogPoisonReject("foreign-module", context, fault_vaddr, is_write);
				return false;
			}
		}
	}

	const bool poisonish =
	    fault_vaddr == 0 || fault_vaddr == UINT64_MAX || fault_vaddr >= (1ull << 47);
	if (!poisonish && !force) {
		LogPoisonReject("not-poison", context, fault_vaddr, is_write);
		return false;
	}

	MEMORY_BASIC_INFORMATION mbi {};
	if (VirtualQuery(reinterpret_cast<const void*>(context->Rip), &mbi, sizeof(mbi)) == 0) {
		LogPoisonReject("rip-query-failed", context, fault_vaddr, is_write);
		return false;
	}
	if (mbi.State != MEM_COMMIT) {
		LogPoisonReject("rip-not-committed", context, fault_vaddr, is_write);
		return false;
	}
	if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
		LogPoisonReject("rip-protected", context, fault_vaddr, is_write);
		return false;
	}

	const auto*  rip = reinterpret_cast<const uint8_t*>(context->Rip);
	DecodedMemOp op {};
	if (!DecodeMemOp(rip, &op)) {
		LogPoisonReject("opcode-undecodable", context, fault_vaddr, is_write);
		return false;
	}
	if (op.length == 0 || op.length > 15) {
		LogPoisonReject("decoded-length-invalid", context, fault_vaddr, is_write);
		return false;
	}
	if (op.is_write != is_write) {
		// Still allow skip if decode says write but VEH said read (or vice versa) for poison.
		// Prefer VEH classification for zeroing vs nop-store.
	}

	if (op.is_compare) {
		SetPoisonCompareFlags(context, op);
	} else if (!is_write) {
		if (op.is_xmm) {
			ZeroXmmDest(context, op.reg);
		} else {
			ZeroGpDest(context, op.reg, op.operand_bits);
		}
	}

	// Guard against infinite soft-continue loops on the same host RIP *and* fault
	// address. A walk that progresses (e.g. ntdll scanning a descending unmapped
	// range, fault_vaddr changes each iteration) is legitimate and must be allowed
	// to finish. Only a true spin (same RIP + same fault_vaddr) is refused so the
	// real crash surfaces instead of a silent stalled pipeline.
	{
		const auto original_rip = static_cast<uint64_t>(context->Rip);
		static uint64_t last_rip       = 0;
		static uint64_t last_fault     = 0;
		static uint32_t same_site_skip = 0;
		if (last_rip == original_rip && last_fault == fault_vaddr) {
			if (++same_site_skip > 128) {
				last_rip       = 0;
				last_fault     = 0;
				same_site_skip = 0;
				LogPoisonReject("soft-continue-loop", context, fault_vaddr, is_write);
				return false;
			}
		} else {
			last_rip       = original_rip;
			last_fault     = fault_vaddr;
			same_site_skip = 1;
		}
	}

	context->Rip += op.length;

	static std::atomic<uint32_t> soft_n {0};
	const uint32_t               n = soft_n.fetch_add(1, std::memory_order_relaxed) + 1;
	if (n <= 16) {
		const auto original_rip = static_cast<uint64_t>(context->Rip - op.length);
		char       line[384] {};
		std::snprintf(
		    line, sizeof(line),
		    "MemoryTrace: poison soft-continue n=%u %s vaddr=0x%016" PRIx64
		    " rip=0x%016" PRIx64 " len=%zu",
		    n, is_write ? "Write" : "Read", fault_vaddr, original_rip, op.length);
		Common::LogFatalToFile(line);
		std::fprintf(stderr, "%s\n", line);
		std::fflush(stderr);
		// Do NOT FlushHleRingToFatal here: first soft-continue runs inside VEH and a
		// ring flush/stack walk can nest an ntdll AV → SoftIdle before CONTINUE_EXECUTION
		// (TLOU after VideoOut VRR). Keep a light breadcrumb only.
		if (n == 1) {
			Common::NoteHaltReason("poison_soft_continue", "first AV soft-continued");
		}
	}
	return true;
}

bool DescribeGuestAbortTrap(uint64_t rip, char* detail, size_t detail_size) {
	if (detail == nullptr || detail_size == 0) {
		return false;
	}
	detail[0] = '\0';

	MEMORY_BASIC_INFORMATION mbi {};
	if (VirtualQuery(reinterpret_cast<const void*>(rip), &mbi, sizeof(mbi)) == 0 ||
	    mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
		return false;
	}
	const auto region_start = reinterpret_cast<uint64_t>(mbi.BaseAddress);
	const auto region_size  = static_cast<uint64_t>(mbi.RegionSize);
	const auto region_end =
	    region_start <= UINT64_MAX - region_size ? region_start + region_size : UINT64_MAX;
	if (rip > UINT64_MAX - 2 || rip + 2 > region_end) {
		return false;
	}

	const auto* bytes = reinterpret_cast<const uint8_t*>(rip);
	if (bytes[0] == 0xcd) {
		std::snprintf(detail, detail_size,
		              "int 0x%02" PRIx32 " (guest abort/trap, not poison memop)",
		              static_cast<uint32_t>(bytes[1]));
		return true;
	}
	if (bytes[0] == 0xcc) {
		std::snprintf(detail, detail_size, "int3 (guest abort/trap)");
		return true;
	}
	if (bytes[0] == 0x0f && bytes[1] == 0x0b) {
		std::snprintf(detail, detail_size, "ud2 (guest abort/trap)");
		return true;
	}
	return false;
}

#elif !defined(__APPLE__)

// Linux signal contexts expose registers through ucontext_t.

static uint32_t* GetContextXmm(ucontext_t* context, uint8_t index) {
	if (context == nullptr || index >= 16) {
		return nullptr;
	}

	auto* fpregs = context->uc_mcontext.fpregs;
	if (fpregs == nullptr) {
		return nullptr;
	}

	return static_cast<uint32_t*>(fpregs->_xmm[index].element);
}

static void LoadContextGprsLin(ucontext_t* context, uint64_t (&gpr)[16]) {
	gpr[0]  = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_RAX]);
	gpr[1]  = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_RCX]);
	gpr[2]  = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_RDX]);
	gpr[3]  = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_RBX]);
	gpr[4]  = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_RSP]);
	gpr[5]  = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_RBP]);
	gpr[6]  = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_RSI]);
	gpr[7]  = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_RDI]);
	gpr[8]  = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_R8]);
	gpr[9]  = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_R9]);
	gpr[10] = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_R10]);
	gpr[11] = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_R11]);
	gpr[12] = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_R12]);
	gpr[13] = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_R13]);
	gpr[14] = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_R14]);
	gpr[15] = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_R15]);
}

static void LoadXmmWordsLin(const uint32_t* xmm, XmmWords& out) {
	out.w[0] = xmm[0];
	out.w[1] = xmm[1];
	out.w[2] = xmm[2];
	out.w[3] = xmm[3];
}

static void StoreXmmWordsLin(uint32_t* xmm, const XmmWords& in) {
	xmm[0] = in.w[0];
	xmm[1] = in.w[1];
	xmm[2] = in.w[2];
	xmm[3] = in.w[3];
}

static bool TryEmulateShaNi(ucontext_t* context) {
	if (context == nullptr) {
		return false;
	}

	auto&       rip_reg = context->uc_mcontext.gregs[REG_RIP];
	const auto* rip     = reinterpret_cast<const uint8_t*>(rip_reg);
	ShaNiInsn   insn {};
	if (!DecodeShaNiInsn(rip, insn)) {
		return false;
	}

	const uint8_t modrm_byte = rip[insn.modrm_offset];
	const uint8_t dest_index = ShaNiRegIndex(modrm_byte, insn.rex, true);
	auto*         dest_xmm   = GetContextXmm(context, dest_index);
	auto*         xmm0       = GetContextXmm(context, 0);
	if (dest_xmm == nullptr || xmm0 == nullptr) {
		return false;
	}

	XmmWords dest {};
	XmmWords src2 {};
	XmmWords xmm0_words {};
	LoadXmmWordsLin(dest_xmm, dest);
	LoadXmmWordsLin(xmm0, xmm0_words);

	if (ShaNiModrmIsRegister(modrm_byte)) {
		const uint8_t src_index = ShaNiRegIndex(modrm_byte, insn.rex, false);
		auto*         src_xmm   = GetContextXmm(context, src_index);
		if (src_xmm == nullptr) {
			return false;
		}
		LoadXmmWordsLin(src_xmm, src2);
	} else {
		uint64_t    gpr[16] {};
		const void* source = nullptr;
		LoadContextGprsLin(context, gpr);
		if (!ResolveShaNiMemoryAddress(rip, insn, gpr, source)) {
			return false;
		}
		std::memcpy(&src2, source, sizeof(src2));
	}

	if (!ExecuteShaNiInsn(insn, src2, xmm0_words, dest)) {
		return false;
	}

	StoreXmmWordsLin(dest_xmm, dest);
	rip_reg += static_cast<greg_t>(insn.length);
	return true;
}

static uint64_t GetXmmLow(const uint32_t* xmm) {
	return static_cast<uint64_t>(xmm[0]) | (static_cast<uint64_t>(xmm[1]) << 32u);
}

static void SetXmmLow(uint32_t* xmm, uint64_t value) {
	xmm[0] = static_cast<uint32_t>(value);
	xmm[1] = static_cast<uint32_t>(value >> 32u);
}

static void SetXmmHigh(uint32_t* xmm, uint64_t value) {
	xmm[2] = static_cast<uint32_t>(value);
	xmm[3] = static_cast<uint32_t>(value >> 32u);
}

static bool TryEmulateSse4a(ucontext_t* context) {
	if (context == nullptr) {
		return false;
	}

	auto& rip_reg = context->uc_mcontext.gregs[REG_RIP];

	const auto* rip = reinterpret_cast<const uint8_t*>(rip_reg);

	const uint8_t prefix = rip[0];
	if (prefix != 0x66 && prefix != 0xf2) {
		return false;
	}

	size_t  offset = 1;
	uint8_t rex    = 0;
	if ((rip[offset] & 0xf0u) == 0x40u) {
		rex = rip[offset];
		offset++;
	}

	if (rip[offset] != 0x0f || rip[offset + 1] != 0x78) {
		return false;
	}

	auto modrm = rip[offset + 2];
	if ((modrm & 0xc0u) != 0xc0u) {
		return false;
	}

	const uint8_t reg    = ((modrm >> 3u) & 0x07u) | ((rex & 0x04u) << 1u);
	const uint8_t rm     = (modrm & 0x07u) | ((rex & 0x01u) << 3u);
	const uint8_t length = rip[offset + 3];
	const uint8_t index  = rip[offset + 4];

	// AMD SSE4a immediate-form EXTRQ/INSERTQ.
	if (prefix == 0x66) {
		auto* dst = GetContextXmm(context, rm);
		if (dst == nullptr) {
			return false;
		}

		SetXmmLow(dst, ExtractBitField(GetXmmLow(dst), length, index));
		SetXmmHigh(dst, 0);
		rip_reg += static_cast<greg_t>(offset + 5);
		return true;
	}

	auto* dst = GetContextXmm(context, reg);
	auto* src = GetContextXmm(context, rm);
	if (dst == nullptr || src == nullptr) {
		return false;
	}

	SetXmmLow(dst, InsertBitField(GetXmmLow(dst), GetXmmLow(src), length, index));
	rip_reg += static_cast<greg_t>(offset + 5);
	return true;
}

static bool TryEmulateMonitorxMwaitx(ucontext_t* context) {
	if (context == nullptr) {
		return false;
	}

	auto& rip_reg = context->uc_mcontext.gregs[REG_RIP];

	const auto* rip = reinterpret_cast<const uint8_t*>(rip_reg);
	if (rip[0] != 0x0f || rip[1] != 0x01 || (rip[2] != 0xfa && rip[2] != 0xfb)) {
		return false;
	}

	// Approximate AMD MONITORX/MWAITX as no-op/yield.
	if (rip[2] == 0xfb) {
		::sched_yield();
	}
	rip_reg += 3;
	return true;
}

#endif

bool TryEmulate(void* native_context) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	auto* context = static_cast<PCONTEXT>(native_context);
	return TryEmulateMonitorxMwaitx(context) || TryEmulateSse4a(context) ||
	       TryEmulateShaNi(context);
#elif !defined(__APPLE__)
	auto* context = static_cast<ucontext_t*>(native_context);
	return TryEmulateMonitorxMwaitx(context) || TryEmulateSse4a(context) ||
	       TryEmulateShaNi(context);
#else
	(void)native_context;
	return false;
#endif
}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
bool TrySoftContinuePoisonAccess(void* native_context, uint64_t fault_vaddr, bool is_write,
                                 bool force, bool allow_system_module) {
	(void)native_context;
	(void)fault_vaddr;
	(void)is_write;
	(void)force;
	(void)allow_system_module;
	return false;
}

bool DescribeGuestAbortTrap(uint64_t rip, char* detail, size_t detail_size) {
	(void)rip;
	if (detail != nullptr && detail_size > 0) {
		detail[0] = '\0';
	}
	return false;
}
#endif

} // namespace Loader::X64InstructionEmulator
