#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_JIT_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_JIT_H_

#include "common/abi.h"

namespace Loader::Jit {

#pragma pack(1)

struct JmpWithIndex {
	void SetIndex(uint32_t index) { *reinterpret_cast<uint32_t*>(&code[1]) = index; }

	void SetFunc(void* handler) {
		auto func_addr = reinterpret_cast<int64_t>(handler);
		auto rip_addr  = reinterpret_cast<int64_t>(&code[10]);
		auto offset64  = func_addr - rip_addr;
		auto offset32  = static_cast<uint32_t>(static_cast<uint64_t>(offset64) & 0xffffffffu);

		*reinterpret_cast<uint32_t*>(&code[6]) = offset32;
	}

	static uint64_t GetSize() { return 16; }

	// 68 00 00 00 00          push     <index>
	// E9 E0 FF FF FF          jmp      <handler>
	uint8_t code[16] = {0x68, 0x00, 0x00, 0x00, 0x00, 0xE9, 0x00, 0x00,
	                    0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
};

struct CallPlt {
	explicit CallPlt(uint32_t table_size) {
		for (uint32_t index = 0; index < table_size; index++) {
			auto* c = new (&code[32] + JmpWithIndex::GetSize() * index) JmpWithIndex;
			c->SetIndex(index);
			c->SetFunc(this);
		}
	}

	void SetPltGot(uint64_t vaddr) { *reinterpret_cast<uint64_t*>(&code[2]) = vaddr; }

	uint64_t GetAddr(uint32_t index) {
		return reinterpret_cast<uint64_t>(&code[32] + JmpWithIndex::GetSize() * index);
	}

	static uint64_t GetSize(uint32_t table_size) {
		return 32 + JmpWithIndex::GetSize() * table_size;
	}

	// 0:  49 bb 88 77 66 55 44    movabs r11,0x1122334455667788
	// 7:  33 22 11
	// a:  41 ff 73 08             push   QWORD PTR [r11+0x8]
	// e:  41 ff 63 10             jmp    QWORD PTR [r11+0x10]
	uint8_t code[32] = {0x49, 0xBB, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x41, 0xFF,
	                    0x73, 0x08, 0x41, 0xFF, 0x63, 0x10, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
};

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
		auto rip_addr  = reinterpret_cast<int64_t>(&code[5]);
		auto offset64  = func_addr - rip_addr;
		auto offset32  = static_cast<uint32_t>(static_cast<uint64_t>(offset64) & 0xffffffffu);

		*reinterpret_cast<uint32_t*>(&code[1]) = offset32;
	}

	void SetOutputReg(uint8_t reg) { code[7] = 0xc0u | (reg & 7u); }

	static uint64_t GetSize() { return 9; }

	// call func
	// mov rax,rax
	// nop
	uint8_t code[9] = {0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0xC0, 0x90};
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

struct SafeCall {
	using func_t = KYTY_MS_ABI uint8_t* (*)();

	void SetFunc(func_t func) { *reinterpret_cast<func_t*>(&code[0x22]) = func; }

	static uint64_t GetSize() { return 0x1000; }

	uint8_t code[0x90] = {
	    /*00*/ 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00, // sub    rsp,0x80
	    /*07*/ 0x9c,                                     // pushfq
	    /*08*/ 0x51,                                     // push   rcx
	    /*09*/ 0x52,                                     // push   rdx
	    /*0a*/ 0x41, 0x50,                               // push   r8
	    /*0c*/ 0x41, 0x51,                               // push   r9
	    /*0e*/ 0x41, 0x52,                               // push   r10
	    /*10*/ 0x41, 0x53,                               // push   r11
	    /*12*/ 0x53,                                     // push   rbx
	    /*13*/ 0x57,                                     // push   rdi
	    /*14*/ 0x56,                                     // push   rsi
	    /*15*/ 0x48, 0x89, 0xe3,                         // mov    rbx,rsp
	    /*18*/ 0x48, 0x83, 0xe4, 0xf0,                   // and    rsp,0xfffffffffffffff0
	    /*1c*/ 0x48, 0x83, 0xec, 0x20,                   // sub    rsp,0x20
	    /*20*/ 0x48, 0xb9, 0x88, 0x77, 0x66, 0x55, 0x44,
	    0x33,        0x22, 0x11, // movabs rcx,0x1122334455667788
	    /*2a*/ 0xff, 0xd1,       // call   rcx
	    /*2c*/ 0x48, 0x89, 0xc1, // mov    rcx,rax
	    /*2f*/ 0x48, 0x89, 0xdc, // mov    rsp,rbx
	    /*32*/ 0x48, 0x89, 0xc8, // mov    rax,rcx
	    /*35*/ 0x5e,             // pop    rsi
	    /*36*/ 0x5f,             // pop    rdi
	    /*37*/ 0x5b,             // pop    rbx
	    /*38*/ 0x41, 0x5b,       // pop    r11
	    /*3a*/ 0x41, 0x5a,       // pop    r10
	    /*3c*/ 0x41, 0x59,       // pop    r9
	    /*3e*/ 0x41, 0x58,       // pop    r8
	    /*40*/ 0x5a,             // pop    rdx
	    /*41*/ 0x59,             // pop    rcx
	    /*42*/ 0x9d,             // popfq
	    /*43*/ 0x48, 0x81, 0xc4, 0x80, 0x00, 0x00,
	    0x00,        // add    rsp,0x80
	    /*4a*/ 0xc3, // ret
	};
};

#pragma pack()

} // namespace Loader::Jit

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_JIT_H_ */
