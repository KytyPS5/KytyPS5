#include "loader/x64InstructionEmulator.h"

#include "common/common.h"

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

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

static M128A* GetContextXmm(PCONTEXT context, uint8_t index) {
	if (context == nullptr || index >= 16) {
		return nullptr;
	}

	return &context->Xmm0 + index;
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
	return TryEmulateMonitorxMwaitx(context) || TryEmulateSse4a(context);
#elif !defined(__APPLE__)
	auto* context = static_cast<ucontext_t*>(native_context);
	return TryEmulateMonitorxMwaitx(context) || TryEmulateSse4a(context);
#else
	(void)native_context;
	return false;
#endif
}

} // namespace Loader::X64InstructionEmulator
