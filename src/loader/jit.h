#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_JIT_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_JIT_H_

#include "common/abi.h"

namespace Loader::Jit {

#pragma pack(1)

struct JmpRax {
	template <class Handler>
	void SetFunc(Handler func) {
		*reinterpret_cast<Handler*>(&code[2]) = func;
	}

	// mov rax, 0x1122334455667788
	// jmp rax
	uint8_t code[16] = {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xE0};
};

struct Call9 {
	template <class Handler>
	void SetFunc(Handler func) {
		auto func_addr = reinterpret_cast<int64_t>(reinterpret_cast<void*>(func));
		auto rip_addr  = reinterpret_cast<int64_t>(&code[6]);
		auto offset64  = func_addr - rip_addr;
		auto offset32  = static_cast<uint32_t>(static_cast<uint64_t>(offset64) & 0xffffffffu);

		*reinterpret_cast<uint32_t*>(&code[2]) = offset32;
	}

	void SetOutputReg(uint8_t reg) { code[8] = 0xc0u | (reg & 7u); }

	static uint64_t GetSize() { return 9; }

	// rex.w; call func
	// mov rax,rax
	// REX.W is required because AMD processors honor a retained 0x66 operand-size
	// prefix on a near call and would otherwise execute callw.
	uint8_t code[9] = {0x48, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0xC0};
};

struct TlsRegStub {
	template <class Handler>
	void SetFunc(Handler func) {
		auto func_addr = reinterpret_cast<int64_t>(reinterpret_cast<void*>(func));
		auto rip_addr  = reinterpret_cast<int64_t>(&code[13]);
		auto offset64  = func_addr - rip_addr;
		auto offset32  = static_cast<uint32_t>(static_cast<uint64_t>(offset64) & 0xffffffffu);

		*reinterpret_cast<uint32_t*>(&code[9]) = offset32;
	}

	void SetOutputReg(uint8_t reg) { code[15] = 0xc0u | (reg & 7u); }

	static uint64_t GetOffset(uint8_t reg) {
		return 0x100 + static_cast<uint64_t>(reg) * GetSize();
	}
	static uint64_t GetSize() { return 32; }

	// sub rsp,0x80
	// push rax
	// call safe_call
	// mov <reg>,rax
	// pop rax
	// add rsp,0x80
	// ret
	uint8_t code[32] = {0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x50, 0xE8, 0x00, 0x00,
	                    0x00, 0x00, 0x48, 0x89, 0xC0, 0x58, 0x48, 0x81, 0xC4, 0x80, 0x00,
	                    0x00, 0x00, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
};

// Calls an MS-ABI handler from patched guest code (a `mov r64, fs:[0]` becomes a call into
// this stub), so besides the integer registers the handler may clobber it must also preserve
// the guest's vector and x87 state: xmm0-5 are volatile under the MS ABI and any host memcpy /
// vzeroupper on the handler's slow path clears the upper halves of every ymm register. The
// stub saves x87, SSE and AVX state with XSAVE (mask 0x7; components the OS has not enabled
// are ignored) into a 64-byte aligned 832-byte area and restores it with XRSTOR. The XSAVE
// header must be zero before the first XSAVE, so it is cleared explicitly. The stub must stay
// below 0x100 bytes: TlsRegStub entries follow it in the same page.
struct SafeCall {
	using func_t = KYTY_MS_ABI uint8_t* (*)();

	void SetFunc(func_t func) { *reinterpret_cast<func_t*>(&code[0x73]) = func; }

	static uint64_t GetSize() { return 0x1000; }

	uint8_t code[0xb0] = {
	    /*00*/ 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00,       // sub    rsp,0x80
	    /*07*/ 0x9c,                                           // pushfq
	    /*08*/ 0x51,                                           // push   rcx
	    /*09*/ 0x52,                                           // push   rdx
	    /*0a*/ 0x41, 0x50,                                     // push   r8
	    /*0c*/ 0x41, 0x51,                                     // push   r9
	    /*0e*/ 0x41, 0x52,                                     // push   r10
	    /*10*/ 0x41, 0x53,                                     // push   r11
	    /*12*/ 0x53,                                           // push   rbx
	    /*13*/ 0x57,                                           // push   rdi
	    /*14*/ 0x56,                                           // push   rsi
	    /*15*/ 0x48, 0x89, 0xe3,                               // mov    rbx,rsp
	    /*18*/ 0x48, 0x83, 0xe4, 0xc0,                         // and    rsp,0xffffffffffffffc0
	    /*1c*/ 0x48, 0x81, 0xec, 0x60, 0x03, 0x00, 0x00,       // sub    rsp,0x360
	    /*23*/ 0x31, 0xc0,                                     // xor    eax,eax
	    /*25*/ 0x48, 0x89, 0x84, 0x24, 0x20, 0x02, 0x00, 0x00, // mov [rsp+0x220],rax
	    /*2d*/ 0x48, 0x89, 0x84, 0x24, 0x28, 0x02, 0x00, 0x00, // mov [rsp+0x228],rax
	    /*35*/ 0x48, 0x89, 0x84, 0x24, 0x30, 0x02, 0x00, 0x00, // mov [rsp+0x230],rax
	    /*3d*/ 0x48, 0x89, 0x84, 0x24, 0x38, 0x02, 0x00, 0x00, // mov [rsp+0x238],rax
	    /*45*/ 0x48, 0x89, 0x84, 0x24, 0x40, 0x02, 0x00, 0x00, // mov [rsp+0x240],rax
	    /*4d*/ 0x48, 0x89, 0x84, 0x24, 0x48, 0x02, 0x00, 0x00, // mov [rsp+0x248],rax
	    /*55*/ 0x48, 0x89, 0x84, 0x24, 0x50, 0x02, 0x00, 0x00, // mov [rsp+0x250],rax
	    /*5d*/ 0x48, 0x89, 0x84, 0x24, 0x58, 0x02, 0x00, 0x00, // mov [rsp+0x258],rax
	    /*65*/ 0xb8, 0x07, 0x00, 0x00, 0x00,                   // mov    eax,0x7
	    /*6a*/ 0x31, 0xd2,                                     // xor    edx,edx
	    /*6c*/ 0x0f, 0xae, 0x64, 0x24, 0x20,                   // xsave  [rsp+0x20]
	    /*71*/ 0x48, 0xb9, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33,
	    0x22,        0x11,                               // movabs rcx,0x1122334455667788
	    /*7b*/ 0xff, 0xd1,                               // call   rcx
	    /*7d*/ 0x48, 0x89, 0xc1,                         // mov    rcx,rax
	    /*80*/ 0xb8, 0x07, 0x00, 0x00, 0x00,             // mov    eax,0x7
	    /*85*/ 0x31, 0xd2,                               // xor    edx,edx
	    /*87*/ 0x0f, 0xae, 0x6c, 0x24, 0x20,             // xrstor [rsp+0x20]
	    /*8c*/ 0x48, 0x89, 0xdc,                         // mov    rsp,rbx
	    /*8f*/ 0x48, 0x89, 0xc8,                         // mov    rax,rcx
	    /*92*/ 0x5e,                                     // pop    rsi
	    /*93*/ 0x5f,                                     // pop    rdi
	    /*94*/ 0x5b,                                     // pop    rbx
	    /*95*/ 0x41, 0x5b,                               // pop    r11
	    /*97*/ 0x41, 0x5a,                               // pop    r10
	    /*99*/ 0x41, 0x59,                               // pop    r9
	    /*9b*/ 0x41, 0x58,                               // pop    r8
	    /*9d*/ 0x5a,                                     // pop    rdx
	    /*9e*/ 0x59,                                     // pop    rcx
	    /*9f*/ 0x9d,                                     // popfq
	    /*a0*/ 0x48, 0x81, 0xc4, 0x80, 0x00, 0x00, 0x00, // add    rsp,0x80
	    /*a7*/ 0xc3,                                     // ret
	};
	static_assert(0xb0 <= 0x100, "SafeCall must fit below the first TlsRegStub");
};

#pragma pack()

} // namespace Loader::Jit

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_JIT_H_ */
