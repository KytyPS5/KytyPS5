#include "common/abi.h"
#include "graphics/host_gpu/hostMemory.h"
#include "common/logging/log.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <atomic>
#include <cstring>
#include <cinttypes>

namespace Libs {

// libSceHmd2 and libSceVrTracker2 drive PlayStation VR2. No headset is attached to the emulator
// and nothing here talks to one: the imports are registered so the guest stops running into the
// unresolved import stub, whose call is invisible to the title and whose result is
// indistinguishable from success.
//
// The function names were recovered from the imported NIDs with the usual name + suffix SHA-1
// hash; the entries still called *Unknown_* did not fall out of it. The stubs take six integer
// arguments they do not use - the real prototypes are unknown, and logging the argument registers
// is what makes the call sites legible - and the first few calls of each are logged.
//
// Only the two size queries need real answers. Everything else a PSVR2 title asks for during
// start-up it either ignores (Red Matter, PPSA16509, overwrites the device information with a
// hardcoded 2000x2040 panel right after asking for it) or only needs to not fail.

namespace LibHmd2 {

LIB_VERSION("Hmd2", 1, "Hmd2", 1, 1);

constexpr int HMD2_OK = 0;

// These are polled every frame; the log only needs to show that they are being called.
constexpr int HMD2_LOG_CALLS = 8;

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_HMD2_STUBS(X)                                                                         \
	X(Hmd2Initialize, "c812oYs7Vsc")                                                               \
	X(Hmd2Open, "f3kPeoTZnIE")                                                                       \
	X(Hmd2Close, "oPhtjySuHa8")                                                                    \
	X(Hmd2SetVibration, "Al4qjNREVQQ")                                                             \
	X(Hmd2ReprojectionInitialize, "C0rPwER-yxg")                                                   \
	X(Hmd2ReprojectionTerminate, "4Q11W4M2h5Q")                                                    \
	X(Hmd2ReprojectionSetParam, "xMo9ENEu2E0")                                                     \
	X(Hmd2ReprojectionSetTiming, "FkQX7rjFomk")                                                    \
	X(Hmd2ReprojectionEnableVrMode, "VVvFh51o20s")                                                 \
	X(Hmd2ReprojectionDisableVrMode, "wj1kOyNF4vM")                                                \
	X(Hmd2ReprojectionBeginFrame, "Ocf081WpBpA")                                                   \
	X(Hmd2Unknown_SVEGp1D7qHA, "SVEG+1D7qHA")                                                      \
	X(Hmd2Unknown_gF8plvc7GuQ, "gF8+lvc7GuQ")                                                      \
	X(Hmd2Unknown_hA9LshbSkzw, "hA9LshbSkzw")                                                      \
	X(Hmd2Unknown_lAoFUedcfqA, "lAoFUedcfqA")                                                      \
	X(Hmd2Unknown_retcpmuRMhk, "retc+-uRMhk")

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_HMD2_DEFINE(name, nid)                                                                \
	static int KYTY_SYSV_ABI name(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, \
	                              uint64_t a5) {                                                   \
		static std::atomic_int calls = 0;                                                          \
		if (calls.fetch_add(1) < HMD2_LOG_CALLS) {                                                 \
			LOGF("Hmd2 stub: %s (%s) a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64     \
			     " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n",                 \
			     #name, nid, a0, a1, a2, a3, a4, a5);                                              \
			LOGF("\t caller = 0x%016" PRIx64 "\n",                                                         \
			     reinterpret_cast<uint64_t>(__builtin_return_address(0)));                                 \
		}                                                                                          \
		return HMD2_OK;                                                                            \
	}

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_HMD2_REGISTER(name, nid) LIB_FUNC(nid, name);

// sceHmd2GetDeviceInformation(SceHmd2DeviceInformation* info). The layout is not documented, but
// Red Matter (PPSA16509) reads two fields out of it every frame, both at 0x27ce5 and 0x2b77b in
// its eboot, which take the struct at rbp-0x38: a
// byte at +0x14 that is compared against 1 and drives the "headset is being worn" state, and a
// dword at +0x00 that has to be zero: at 0x2b77f a non-zero value keeps the session in state 1,
// zero advances it to state 2, which is what xrWaitFrame at 0x1cc60 requires. The rest
// is cleared so the title sees a stable device rather than whatever was on its stack; the frame
// keeps its stack canary at rbp-0x18, so the struct is no larger than 0x20 bytes.
constexpr uint64_t HMD2_DEVICE_INFORMATION_SIZE = 0x20;
constexpr uint32_t HMD2_DEVICE_STATUS_OK        = 0;
constexpr uint8_t  HMD2_DEVICE_MOUNTED          = 1;

static int KYTY_SYSV_ABI Hmd2GetDeviceInformation(uint64_t info, uint64_t a1, uint64_t a2) {
	static std::atomic_int calls = 0;
	if (calls.fetch_add(1) < HMD2_LOG_CALLS) {
		LOGF("Hmd2 stub: Hmd2GetDeviceInformation (bIi4YUfSRys) info=0x%016" PRIx64
		     " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 "\n",
		     info, a1, a2);
	}

	if (info < 0x10000 ||
	    !Graphics::HostMemoryRangeIsWritable(info, HMD2_DEVICE_INFORMATION_SIZE)) {
		return HMD2_OK;
	}

	auto* bytes = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(info));
	std::memset(bytes, 0, HMD2_DEVICE_INFORMATION_SIZE);
	*reinterpret_cast<uint32_t*>(bytes) = HMD2_DEVICE_STATUS_OK;
	bytes[0x14]                         = HMD2_DEVICE_MOUNTED;

	return HMD2_OK;
}

// sceHmd2ReprojectionQueryBufferSizeAlign and its display buffer counterpart report how much
// memory the reprojection buffers need. They return the pair in rax:rdx rather than through out
// parameters - the call site in Red Matter (PPSA16509) reads the size straight out of rax and the
// alignment out of rdx, clamping the alignment up to 64 KiB - so a stub that returns a plain int
// hands the engine a size of 0, it asks the kernel for a 0 byte block, and the title aborts with
// "Ran out of memory allocating 0 bytes with alignment 0".
//
// Nothing renders to these buffers, so the size only has to be plausible: two 2000x2040 eye
// buffers at 8 bytes per pixel, which is what the engine assumes the panel is.
struct Hmd2BufferSizeAlign {
	uint64_t size;
	uint64_t align;
};

constexpr uint64_t HMD2_REPROJECTION_BUFFER_SIZE  = 64ULL * 1024 * 1024;
constexpr uint64_t HMD2_REPROJECTION_BUFFER_ALIGN = 64ULL * 1024;

static Hmd2BufferSizeAlign KYTY_SYSV_ABI Hmd2ReprojectionQueryBufferSizeAlign(uint64_t a0,
                                                                              uint64_t a1,
                                                                              uint64_t a2) {
	LOGF("Hmd2 stub: Hmd2ReprojectionQueryBufferSizeAlign (U-CnbmeyYaA) a0=0x%016" PRIx64
	     " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " -> size=0x%016" PRIx64 " align=0x%016" PRIx64
	     "\n",
	     a0, a1, a2, HMD2_REPROJECTION_BUFFER_SIZE, HMD2_REPROJECTION_BUFFER_ALIGN);

	return {HMD2_REPROJECTION_BUFFER_SIZE, HMD2_REPROJECTION_BUFFER_ALIGN};
}

static Hmd2BufferSizeAlign KYTY_SYSV_ABI Hmd2ReprojectionQueryDisplayBufferSizeAlign(uint64_t a0,
                                                                                     uint64_t a1,
                                                                                     uint64_t a2) {
	LOGF("Hmd2 stub: Hmd2ReprojectionQueryDisplayBufferSizeAlign (-C2nkoEYOnU) a0=0x%016" PRIx64
	     " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " -> size=0x%016" PRIx64 " align=0x%016" PRIx64
	     "\n",
	     a0, a1, a2, HMD2_REPROJECTION_BUFFER_SIZE, HMD2_REPROJECTION_BUFFER_ALIGN);

	return {HMD2_REPROJECTION_BUFFER_SIZE, HMD2_REPROJECTION_BUFFER_ALIGN};
}

KYTY_HMD2_STUBS(KYTY_HMD2_DEFINE)

LIB_DEFINE(InitHmd2_1) {
	KYTY_HMD2_STUBS(KYTY_HMD2_REGISTER)
	LIB_FUNC("bIi4YUfSRys", Hmd2GetDeviceInformation);
	LIB_FUNC("U-CnbmeyYaA", Hmd2ReprojectionQueryBufferSizeAlign);
	LIB_FUNC("-C2nkoEYOnU", Hmd2ReprojectionQueryDisplayBufferSizeAlign);
}

#undef KYTY_HMD2_REGISTER
#undef KYTY_HMD2_DEFINE
#undef KYTY_HMD2_STUBS

} // namespace LibHmd2

namespace LibVrTracker2 {

LIB_VERSION("VrTracker2", 1, "VrTracker2", 1, 1);

constexpr int VR_TRACKER2_OK = 0;

constexpr int VR_TRACKER2_LOG_CALLS = 8;

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_VR_TRACKER2_STUBS(X)                                                                  \
	X(VrTracker2Initialize, "6Jy73SRfG-o")                                                         \
	X(VrTracker2Finalize, "IQ3UD6SZbXo")                                                           \
	X(VrTracker2RegisterDevice, "Dog+g25QYjw")                                                     \
	X(VrTracker2UnregisterDevice, "kFt4MB3SUEk")                                                   \
	X(VrTracker2GetResult, "J4Vh3VVX0iU")                                                          \
	X(VrTracker2Unknown_UVCMLmSmEas, "UVCMLmS-Eas")                                                \
	X(VrTracker2Unknown_f7G97dWnEis, "f7G97dWnEis")                                                \
	X(VrTracker2Unknown_snYs7NfmRKk, "snYs7Nf-RKk")

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_VR_TRACKER2_DEFINE(name, nid)                                                         \
	static int KYTY_SYSV_ABI name(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, \
	                              uint64_t a5) {                                                   \
		static std::atomic_int calls = 0;                                                          \
		if (calls.fetch_add(1) < VR_TRACKER2_LOG_CALLS) {                                          \
			LOGF("VrTracker2 stub: %s (%s) a0=0x%016" PRIx64 " a1=0x%016" PRIx64                   \
			     " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64   \
			     "\n",                                                                             \
			     #name, nid, a0, a1, a2, a3, a4, a5);                                              \
			LOGF("\t caller = 0x%016" PRIx64 "\n",                                                         \
			     reinterpret_cast<uint64_t>(__builtin_return_address(0)));                                 \
		}                                                                                          \
		return VR_TRACKER2_OK;                                                                     \
	}

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_VR_TRACKER2_REGISTER(name, nid) LIB_FUNC(nid, name);

// sceVrTracker2QueryMemory(const param*, SceVrTracker2MemoryInfo* out). The call site at 0x32cc9
// fills out+0x00 with the size of the struct, then reads the work memory size from out+0x04 and
// its alignment from out+0x08, both 32 bit, allocates that much and clears it before calling
// sceVrTracker2Initialize. Reporting nothing leaves the tracker with a 0 byte work buffer.
constexpr uint64_t VR_TRACKER2_MEMORY_INFO_SIZE = 0x20;
constexpr uint32_t VR_TRACKER2_WORK_MEMORY_SIZE = 1U * 1024 * 1024;
constexpr uint32_t VR_TRACKER2_WORK_MEMORY_ALIGN = 0x4000;

static int KYTY_SYSV_ABI VrTracker2QueryMemory(uint64_t param, uint64_t out) {
	static std::atomic_int calls = 0;
	if (calls.fetch_add(1) < VR_TRACKER2_LOG_CALLS) {
		LOGF("VrTracker2 stub: VrTracker2QueryMemory (TwqZnaIjWv4) param=0x%016" PRIx64
		     " out=0x%016" PRIx64 " -> size=0x%08" PRIx32 " align=0x%08" PRIx32 "\n",
		     param, out, VR_TRACKER2_WORK_MEMORY_SIZE, VR_TRACKER2_WORK_MEMORY_ALIGN);
	}

	if (out < 0x10000 || !Graphics::HostMemoryRangeIsWritable(out, VR_TRACKER2_MEMORY_INFO_SIZE)) {
		return VR_TRACKER2_OK;
	}

	auto* fields = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(out));
	fields[1]    = VR_TRACKER2_WORK_MEMORY_SIZE;
	fields[2]    = VR_TRACKER2_WORK_MEMORY_ALIGN;

	return VR_TRACKER2_OK;
}

KYTY_VR_TRACKER2_STUBS(KYTY_VR_TRACKER2_DEFINE)

LIB_DEFINE(InitVrTracker2_1) {
	KYTY_VR_TRACKER2_STUBS(KYTY_VR_TRACKER2_REGISTER)
	LIB_FUNC("TwqZnaIjWv4", VrTracker2QueryMemory);
}

#undef KYTY_VR_TRACKER2_REGISTER
#undef KYTY_VR_TRACKER2_DEFINE
#undef KYTY_VR_TRACKER2_STUBS

} // namespace LibVrTracker2

} // namespace Libs
