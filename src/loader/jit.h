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
	// Fallback only: the call pushes its return address into the guest red zone, so
	// patched sites use Jmp9 + TlsSiteTrampoline whenever a trampoline can be placed.
	uint8_t code[9] = {0x48, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0xC0};
};

// Replaces a 9-byte `mov r64, fs:[0]` with a jump to its TlsSiteTrampoline. Unlike a call it
// writes nothing to the stack, so the guest red zone below rsp stays intact. REX.W neutralizes
// a retained 0x66 prefix the same way Call9 does.
struct Jmp9 {
	void SetTarget(uint64_t target) {
		const auto rip_addr                    = reinterpret_cast<uint64_t>(&code[6]);
		*reinterpret_cast<uint32_t*>(&code[2]) = static_cast<uint32_t>(target - rip_addr);
	}

	static uint64_t GetSize() { return 9; }

	uint8_t code[9] = {
	    /*00*/ 0x48, 0xe9, 0x00, 0x00, 0x00, 0x00, // rex.W jmp dc <jmp9+0x6>
	    /*06*/ 0x90,                               // nop
	    /*07*/ 0x90,                               // nop
	    /*08*/ 0x90,                               // nop
	};
};

// Per-site trampoline: moves rsp below the 128-byte red zone with flag-preserving LEAs, calls
// the TLS handler (SafeCall or the register's TlsRegStub, which leave every register but the
// destination intact), restores rsp and jumps back behind the patched instruction.
struct TlsSiteTrampoline {
	void SetHandler(uint64_t handler) {
		const auto rip_addr                       = reinterpret_cast<uint64_t>(&code[0x0a]);
		*reinterpret_cast<uint32_t*>(&code[0x06]) = static_cast<uint32_t>(handler - rip_addr);
	}
	void SetReturn(uint64_t resume) {
		const auto rip_addr                       = reinterpret_cast<uint64_t>(&code[0x17]);
		*reinterpret_cast<uint32_t*>(&code[0x13]) = static_cast<uint32_t>(resume - rip_addr);
	}

	static uint64_t GetSize() { return 32; }

	uint8_t code[32] = {
	    /*00*/ 0x48, 0x8d, 0x64, 0x24, 0x80,                         // lea rsp,[rsp-0x80]
	    /*05*/ 0xe8, 0x00, 0x00, 0x00, 0x00,                         // call c9 <trampoline+0xa>
	    /*0a*/ 0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00,       // lea rsp,[rsp+0x80]
	    /*12*/ 0xe9, 0x00, 0x00, 0x00, 0x00,                         // jmp d6 <jmp9>
	    /*17*/ 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, // padding
	};
};

struct TlsRegStub {
	template <class Handler>
	void SetFunc(Handler func) {
		auto func_addr = reinterpret_cast<int64_t>(reinterpret_cast<void*>(func));
		auto rip_addr  = reinterpret_cast<int64_t>(&code[0x0b]);
		auto offset64  = func_addr - rip_addr;
		auto offset32  = static_cast<uint32_t>(static_cast<uint64_t>(offset64) & 0xffffffffu);

		*reinterpret_cast<uint32_t*>(&code[0x07]) = offset32;
	}

	void SetOutputReg(uint8_t reg) { code[0x0d] = 0xc0u | (reg & 7u); }

	static uint64_t GetOffset(uint8_t reg) {
		return 0x100 + static_cast<uint64_t>(reg) * GetSize();
	}
	static uint64_t GetSize() { return 32; }

	// The stack adjustments use LEA so the guest's flags survive, as they would across the
	// replaced mov.
	uint8_t code[32] = {
	    /*00*/ 0x48, 0x8d, 0x64, 0x24, 0x80,                   // lea rsp,[rsp-0x80]
	    /*05*/ 0x50,                                           // push rax
	    /*06*/ 0xe8, 0x00, 0x00, 0x00, 0x00,                   // call b2 <regstub+0xb>
	    /*0b*/ 0x48, 0x89, 0xc0,                               // mov rax,rax
	    /*0e*/ 0x58,                                           // pop rax
	    /*0f*/ 0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00, // lea rsp,[rsp+0x80]
	    /*17*/ 0xc3,                                           // ret
	    /*18*/ 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, // padding
	};
};

// Calls an MS-ABI handler from patched guest code (a `mov r64, fs:[0]` becomes a jump or call
// into this stub), so besides the integer registers the handler may clobber it must also
// preserve the guest's flags and vector and x87 state: xmm0-5 are volatile under the MS ABI
// and any host memcpy / vzeroupper on the handler's slow path clears the upper halves of every
// ymm register. The stub saves x87, SSE and AVX state with XSAVE (mask 0x7; components the OS
// has not enabled are ignored) into a 64-byte aligned 832-byte area and restores it with
// XRSTOR. The XSAVE header must be zero before the first XSAVE, so it is cleared explicitly.
// The outer stack adjustments use LEA so the flags are intact when pushfq saves them. The stub
// must stay below 0x100 bytes: TlsRegStub entries follow it in the same page.
struct SafeCall {
	using func_t = KYTY_MS_ABI uint8_t* (*)();

	void SetFunc(func_t func) { *reinterpret_cast<func_t*>(&code[0x71]) = func; }

	static uint64_t GetSize() { return 0x1000; }

	uint8_t code[0xb0] = {
	    /*00*/ 0x48, 0x8d, 0x64, 0x24, 0x80,                   // lea rsp,[rsp-0x80]
	    /*05*/ 0x9c,                                           // pushf
	    /*06*/ 0x51,                                           // push rcx
	    /*07*/ 0x52,                                           // push rdx
	    /*08*/ 0x41, 0x50,                                     // push r8
	    /*0a*/ 0x41, 0x51,                                     // push r9
	    /*0c*/ 0x41, 0x52,                                     // push r10
	    /*0e*/ 0x41, 0x53,                                     // push r11
	    /*10*/ 0x53,                                           // push rbx
	    /*11*/ 0x57,                                           // push rdi
	    /*12*/ 0x56,                                           // push rsi
	    /*13*/ 0x48, 0x89, 0xe3,                               // mov rbx,rsp
	    /*16*/ 0x48, 0x83, 0xe4, 0xc0,                         // and rsp,0xffffffffffffffc0
	    /*1a*/ 0x48, 0x81, 0xec, 0x60, 0x03, 0x00, 0x00,       // sub rsp,0x360
	    /*21*/ 0x31, 0xc0,                                     // xor eax,eax
	    /*23*/ 0x48, 0x89, 0x84, 0x24, 0x20, 0x02, 0x00, 0x00, // mov [rsp+0x220],rax
	    /*2b*/ 0x48, 0x89, 0x84, 0x24, 0x28, 0x02, 0x00, 0x00, // mov [rsp+0x228],rax
	    /*33*/ 0x48, 0x89, 0x84, 0x24, 0x30, 0x02, 0x00, 0x00, // mov [rsp+0x230],rax
	    /*3b*/ 0x48, 0x89, 0x84, 0x24, 0x38, 0x02, 0x00, 0x00, // mov [rsp+0x238],rax
	    /*43*/ 0x48, 0x89, 0x84, 0x24, 0x40, 0x02, 0x00, 0x00, // mov [rsp+0x240],rax
	    /*4b*/ 0x48, 0x89, 0x84, 0x24, 0x48, 0x02, 0x00, 0x00, // mov [rsp+0x248],rax
	    /*53*/ 0x48, 0x89, 0x84, 0x24, 0x50, 0x02, 0x00, 0x00, // mov [rsp+0x250],rax
	    /*5b*/ 0x48, 0x89, 0x84, 0x24, 0x58, 0x02, 0x00, 0x00, // mov [rsp+0x258],rax
	    /*63*/ 0xb8, 0x07, 0x00, 0x00, 0x00,                   // mov eax,0x7
	    /*68*/ 0x31, 0xd2,                                     // xor edx,edx
	    /*6a*/ 0x0f, 0xae, 0x64, 0x24, 0x20,                   // xsave [rsp+0x20]
	    /*6f*/ 0x48, 0xb9, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22,
	    0x11,                                                  // movabs rcx,0x1122334455667788
	    /*79*/ 0xff, 0xd1,                                     // call rcx
	    /*7b*/ 0x48, 0x89, 0xc1,                               // mov rcx,rax
	    /*7e*/ 0xb8, 0x07, 0x00, 0x00, 0x00,                   // mov eax,0x7
	    /*83*/ 0x31, 0xd2,                                     // xor edx,edx
	    /*85*/ 0x0f, 0xae, 0x6c, 0x24, 0x20,                   // xrstor [rsp+0x20]
	    /*8a*/ 0x48, 0x89, 0xdc,                               // mov rsp,rbx
	    /*8d*/ 0x48, 0x89, 0xc8,                               // mov rax,rcx
	    /*90*/ 0x5e,                                           // pop rsi
	    /*91*/ 0x5f,                                           // pop rdi
	    /*92*/ 0x5b,                                           // pop rbx
	    /*93*/ 0x41, 0x5b,                                     // pop r11
	    /*95*/ 0x41, 0x5a,                                     // pop r10
	    /*97*/ 0x41, 0x59,                                     // pop r9
	    /*99*/ 0x41, 0x58,                                     // pop r8
	    /*9b*/ 0x5a,                                           // pop rdx
	    /*9c*/ 0x59,                                           // pop rcx
	    /*9d*/ 0x9d,                                           // popf
	    /*9e*/ 0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00, // lea rsp,[rsp+0x80]
	    /*a6*/ 0xc3,                                           // ret
	    /*a7*/ 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, // padding
	};
	static_assert(0xb0 <= 0x100, "SafeCall must fit below the first TlsRegStub");
};

#pragma pack()

} // namespace Loader::Jit

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_JIT_H_ */
