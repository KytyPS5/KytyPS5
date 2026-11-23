#include "common/abi.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "kernel/descriptors.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>

namespace Libs {

namespace LibGameUpdate {
void InitNet_1_GameUpdate(Loader::SymbolDatabase* s);
}
namespace LibHttp {
void InitNet_1_Http(Loader::SymbolDatabase* s);
}
namespace LibHttp2 {
void InitNet_1_Http2(Loader::SymbolDatabase* s);
}
namespace LibNpAuth {
void InitNet_1_NpAuth(Loader::SymbolDatabase* s);
}
namespace LibNpCommerce {
void InitNet_1_NpCommerce(Loader::SymbolDatabase* s);
}
namespace LibNpEntitlementAccess {
void InitNet_1_NpEntitlementAccess(Loader::SymbolDatabase* s);
}
namespace LibNpEntitlementAccessPft {
void InitNet_1_NpEntitlementAccessPft(Loader::SymbolDatabase* s);
}
namespace LibNpGameIntent {
void InitNet_1_NpGameIntent(Loader::SymbolDatabase* s);
}
namespace LibNpTrophy2 {
void InitNet_1_NpTrophy2(Loader::SymbolDatabase* s);
}
namespace LibNpUniversalDataSystem {
void InitNet_1_NpUniversalDataSystem(Loader::SymbolDatabase* s);
}
namespace LibNpWebApi2 {
void InitNet_1_NpWebApi2(Loader::SymbolDatabase* s);
}
namespace LibSharePlay {
void InitPlatform_1_SharePlay(Loader::SymbolDatabase* s);
}

namespace Network::Net {
struct NetEtherAddr;
} // namespace Network::Net

namespace LibNet {

LIB_NAME("Net", "Net");

static thread_local int   g_net_errno = 0;
static constexpr uint32_t g_in6addr_any[4] {};

namespace Net = Network::Net;

KYTY_SYSV_ABI int* GetNetErrorAddr() {
	return &g_net_errno;
}

static int FinishNetCall(int result) {
	if (result < 0) {
		g_net_errno = result - Network::NET_ERROR_EPERM + 1;
	}
	return result;
}

static int KYTY_SYSV_ABI NetInit() {
	return FinishNetCall(Net::NetInit());
}

static int PosixToNetError(int error) {
	if (error >= Posix::POSIX_EPERM && error <= Posix::POSIX_ECANCELED) {
		return Network::NET_ERROR_EPERM + error - Posix::POSIX_EPERM;
	}

	switch (error) {
		case Posix::POSIX_EPROTO: return Network::NET_ERROR_EPROTO;
		case Posix::POSIX_EADHOC: return Network::NET_ERROR_EADHOC;
		case Posix::POSIX_EINACTIVEDISABLED: return Network::NET_ERROR_EINACTIVEDISABLED;
		case Posix::POSIX_ENETNODATA: return Network::NET_ERROR_ENODATA;
		case Posix::POSIX_ENETDESC: return Network::NET_ERROR_EDESC;
		case Posix::POSIX_ENETDESCTIMEDOUT: return Network::NET_ERROR_EDESCTIMEDOUT;
		case Posix::POSIX_ENETINTR: return Network::NET_ERROR_ENETINTR;
		case Posix::POSIX_ERETURN: return Network::NET_ERROR_ERETURN;
		default: return Network::NET_ERROR_EIO;
	}
}

static int FinishSocketCall(int result) {
	return FinishNetCall(result < 0 ? PosixToNetError(*Posix::GetErrorAddr()) : result);
}

int KYTY_SYSV_ABI NetSocket(const char* name, int family, int type, int protocol) {
	LOGF("\t name = %s\n", name != nullptr ? name : "<null>");

	return FinishSocketCall(Net::Socket(family, type, protocol));
}

int KYTY_SYSV_ABI NetAccept(int s, void* addr, uint32_t* addrlen) {
	return FinishSocketCall(Net::Accept(s, addr, addrlen));
}

int KYTY_SYSV_ABI NetBind(int s, const void* addr, uint32_t addrlen) {
	return FinishSocketCall(Net::Bind(s, addr, addrlen));
}

int KYTY_SYSV_ABI NetConnect(int s, const void* addr, uint32_t addrlen) {
	return FinishSocketCall(Net::Connect(s, addr, addrlen));
}

int KYTY_SYSV_ABI NetListen(int s, int backlog) {
	return FinishSocketCall(Net::Listen(s, backlog));
}

int KYTY_SYSV_ABI NetShutdown(int s, int how) {
	return FinishSocketCall(Net::Shutdown(s, how));
}

int KYTY_SYSV_ABI NetGetsockname(int s, void* addr, uint32_t* addrlen) {
	return FinishSocketCall(Net::Getsockname(s, addr, addrlen));
}

int KYTY_SYSV_ABI NetPoolCreate(const char* name, int size, int flags) {
	return FinishNetCall(Net::NetPoolCreate(name, size, flags));
}

int KYTY_SYSV_ABI NetPoolDestroy(int memid) {
	return FinishNetCall(Net::NetPoolDestroy(memid));
}

int KYTY_SYSV_ABI NetGetMemoryPoolStats(int memid, Net::NetMemoryPoolStats* stats) {
	return FinishNetCall(Net::NetGetMemoryPoolStats(memid, stats));
}

int KYTY_SYSV_ABI NetResolverCreate(const char* name, int memid, int flags) {
	return FinishNetCall(Net::NetResolverCreate(name, memid, flags));
}

int KYTY_SYSV_ABI NetResolverDestroy(int rid) {
	return FinishNetCall(Net::NetResolverDestroy(rid));
}

int KYTY_SYSV_ABI NetResolverAbort(int rid, int flags) {
	return FinishNetCall(Net::NetResolverAbort(rid, flags));
}

int KYTY_SYSV_ABI NetResolverStartNtoa(int rid, const char* hostname, void* addr, int timeout,
                                       int retry, int flags) {
	return FinishNetCall(Net::NetResolverStartNtoa(rid, hostname, addr, timeout, retry, flags));
}

int KYTY_SYSV_ABI NetResolverStartNtoaMultipleRecords(int rid, const char* hostname,
                                                      Net::NetResolverInfo* info, int timeout,
                                                      int retry, int flags) {
	return FinishNetCall(
	    Net::NetResolverStartNtoaMultipleRecords(rid, hostname, info, timeout, retry, flags));
}

int KYTY_SYSV_ABI NetInetPton(int af, const char* src, void* dst) {
	return FinishNetCall(Net::NetInetPton(af, src, dst));
}

const char* KYTY_SYSV_ABI NetInetNtop(int af, const void* src, char* dst, uint32_t size) {
	const char* result = Net::NetInetNtop(af, src, dst, size);
	if (result == nullptr) {
		FinishNetCall(PosixToNetError(*Posix::GetErrorAddr()));
	}
	return result;
}

int KYTY_SYSV_ABI NetEtherNtostr(const Net::NetEtherAddr* n, char* str, size_t len) {
	return FinishNetCall(Net::NetEtherNtostr(n, str, len));
}

int KYTY_SYSV_ABI NetGetMacAddress(Net::NetEtherAddr* addr, int flags) {
	return FinishNetCall(Net::NetGetMacAddress(addr, flags));
}

int KYTY_SYSV_ABI NetGetSockInfo(int s, void* info, int n, int flags) {
	return FinishNetCall(Net::NetGetSockInfo(s, info, n, flags));
}

int KYTY_SYSV_ABI NetEpollCreate(const char* name, int flags) {
	return FinishSocketCall(Net::EpollCreate(name, flags));
}

int KYTY_SYSV_ABI NetEpollControl(int eid, int op, int id, const Net::NetEpollEvent* event) {
	return FinishSocketCall(Net::EpollControl(eid, op, id, event));
}

int KYTY_SYSV_ABI NetEpollWait(int eid, Net::NetEpollEvent* events, int maxevents, int timeout) {
	return FinishSocketCall(Net::EpollWait(eid, events, maxevents, timeout));
}

int KYTY_SYSV_ABI NetEpollDestroy(int eid) {
	return FinishSocketCall(Net::EpollDestroy(eid));
}

int KYTY_SYSV_ABI NetSocketClose(int s) {
	return FinishNetCall(Net::SocketClose(s));
}

int KYTY_SYSV_ABI NetGetsockopt(int s, int level, int optname, void* optval, uint32_t* optlen) {
	return FinishSocketCall(Net::Getsockopt(s, level, optname, optval, optlen));
}

int KYTY_SYSV_ABI NetSetsockopt(int s, int level, int optname, const void* optval,
                                uint32_t optlen) {
	return FinishSocketCall(Net::Setsockopt(s, level, optname, optval, optlen));
}

int KYTY_SYSV_ABI NetSendto(int s, const void* buf, size_t len, int flags, const void* addr,
                         uint32_t addrlen) {
	const auto size = std::min<size_t>(len, std::numeric_limits<int>::max());
	return FinishSocketCall(
	    static_cast<int>(Net::Sendto(s, buf, size, flags | 0x20000, addr, addrlen)));
}

int KYTY_SYSV_ABI NetSend(int s, const void* buf, size_t len, int flags) {
	return NetSendto(s, buf, len, flags, nullptr, 0);
}

int KYTY_SYSV_ABI NetRecv(int s, void* buf, size_t len, int flags) {
	const auto size = std::min<size_t>(len, std::numeric_limits<int>::max());
	return FinishSocketCall(static_cast<int>(Net::Recv(s, buf, size, flags)));
}

int KYTY_SYSV_ABI NetRecvfrom(int s, void* buf, size_t len, int flags, void* addr,
                               uint32_t* addrlen) {
	const auto size = std::min<size_t>(len, std::numeric_limits<int>::max());
	return FinishSocketCall(static_cast<int>(Net::Recvfrom(s, buf, size, flags, addr, addrlen)));
}

uint32_t KYTY_SYSV_ABI NetHtonl(uint32_t host32) {
	return ((host32 & 0x000000ffu) << 24u) | ((host32 & 0x0000ff00u) << 8u) |
	       ((host32 & 0x00ff0000u) >> 8u) | ((host32 & 0xff000000u) >> 24u);
}

uint16_t KYTY_SYSV_ABI NetHtons(uint16_t host16) {
	return static_cast<uint16_t>(((host16 & 0x00ffu) << 8u) | ((host16 & 0xff00u) >> 8u));
}

uint32_t KYTY_SYSV_ABI NetNtohl(uint32_t net32) {
	return NetHtonl(net32);
}

uint16_t KYTY_SYSV_ABI NetNtohs(uint16_t net16) {
	return NetHtons(net16);
}

LIB_DEFINE(InitNet_1_Net) {
	LIB_OBJECT("ZRAJo-A-ukc", &LibNet::g_in6addr_any);

	LIB_FUNC("Nlev7Lg8k3A", LibNet::NetInit);
	LIB_FUNC("HQOwnfMGipQ", LibNet::GetNetErrorAddr);
	LIB_FUNC("PIWqhn9oSxc", LibNet::NetAccept);
	LIB_FUNC("bErx49PgxyY", LibNet::NetBind);
	LIB_FUNC("OXXX4mUk3uk", LibNet::NetConnect);
	LIB_FUNC("dgJBaeJnGpo", LibNet::NetPoolCreate);
	LIB_FUNC("K7RlrTkI-mw", LibNet::NetPoolDestroy);
	LIB_FUNC("rMyh97BU5pY", LibNet::NetGetMemoryPoolStats);
	LIB_FUNC("C4UgDHHPvdw", LibNet::NetResolverCreate);
	LIB_FUNC("kJlYH5uMAWI", LibNet::NetResolverDestroy);
	LIB_FUNC("AzqoBha7js4", LibNet::NetResolverAbort);
	LIB_FUNC("Nd91WaWmG2w", LibNet::NetResolverStartNtoa);
	LIB_FUNC("RCCY01Xd+58", LibNet::NetResolverStartNtoaMultipleRecords);
	LIB_FUNC("8Kcp5d-q1Uo", LibNet::NetInetPton);
	LIB_FUNC("9vA2aW+CHuA", LibNet::NetInetNtop);
	LIB_FUNC("v6M4txecCuo", LibNet::NetEtherNtostr);
	LIB_FUNC("6Oc0bLsIYe0", LibNet::NetGetMacAddress);
	LIB_FUNC("hLuXdjHnhiI", LibNet::NetGetSockInfo);
	LIB_FUNC("hoOAofhhRvE", LibNet::NetGetsockname);
	LIB_FUNC("SF47kB2MNTo", LibNet::NetEpollCreate);
	LIB_FUNC("ZVw46bsasAk", LibNet::NetEpollControl);
	LIB_FUNC("drjIbDbA7UQ", LibNet::NetEpollWait);
	LIB_FUNC("Inp1lfL+Jdw", LibNet::NetEpollDestroy);
	LIB_FUNC("kOj1HiAGE54", LibNet::NetListen);
	LIB_FUNC("TSM6whtekok", LibNet::NetShutdown);
	LIB_FUNC("Q4qBuN-c0ZM", LibNet::NetSocket);
	LIB_FUNC("45ggEzakPJQ", LibNet::NetSocketClose);
	LIB_FUNC("xphrZusl78E", LibNet::NetGetsockopt);
	LIB_FUNC("2mKX2Spso7I", LibNet::NetSetsockopt);
	LIB_FUNC("beRjXBn-z+o", LibNet::NetSend);
	LIB_FUNC("gvD1greCu0A", LibNet::NetSendto);
	LIB_FUNC("9wO9XrMsNhc", LibNet::NetRecv);
	LIB_FUNC("304ooNZxWDY", LibNet::NetRecvfrom);
	LIB_FUNC("9T2pDF2Ryqg", LibNet::NetHtonl);
	LIB_FUNC("iWQWrwiSt8A", LibNet::NetHtons);
	LIB_FUNC("pQGpHYopAIY", LibNet::NetNtohl);
	LIB_FUNC("Rbvt+5Y2iEw", LibNet::NetNtohs);
}

} // namespace LibNet

namespace LibSsl {

LIB_NAME("Ssl", "Ssl");

namespace Ssl = Network::Ssl;

LIB_DEFINE(InitNet_1_Ssl) {
	LIB_FUNC("hdpVEUDFW3s", Ssl::SslInit);
	LIB_FUNC("0K1yQ6Lv-Yc", Ssl::SslTerm);
	LIB_FUNC("TDfQqO-gMbY", Ssl::SslGetCaCerts);
	LIB_FUNC("qIvLs0gYxi0", Ssl::SslFreeCaCerts);
	LIB_FUNC("-PoIzr3PEk0", Ssl::SslGetMemoryPoolStats);
}

} // namespace LibSsl

namespace LibNetCtl {

LIB_NAME("NetCtl", "NetCtl");

namespace NetCtl = Network::NetCtl;

LIB_DEFINE(InitNet_1_NetCtl) {
	LIB_FUNC("gky0+oaNM4k", NetCtl::NetCtlInit);
	LIB_FUNC("JO4yuTuMoKI", NetCtl::NetCtlGetNatInfo);
	LIB_FUNC("iQw3iQPhvUQ", NetCtl::NetCtlCheckCallback);
	LIB_FUNC("uBPlr0lbuiI", NetCtl::NetCtlGetState);
	LIB_FUNC("+lxqIKeU9UY", NetCtl::NetCtlGetStateV6);
	LIB_FUNC("UJ+Z7Q+4ck0", NetCtl::NetCtlRegisterCallback);
	LIB_FUNC("Rqm2OnZMCz0", NetCtl::NetCtlUnregisterCallback);
	LIB_FUNC("0cBgduPRR+M", NetCtl::NetCtlGetResult);
	LIB_FUNC("obuxdTiwkF8", NetCtl::NetCtlGetInfo);
	LIB_FUNC("Z4wwCFiBELQ", NetCtl::NetCtlTerm);
}

} // namespace LibNetCtl

namespace LibNpManager {

LIB_NAME("NpManager", "NpManager");

namespace NpManager = Network::NpManager;

LIB_DEFINE(InitNet_1_NpManager) {
	LIB_FUNC("3Zl8BePTh9Y", NpManager::NpCheckCallback);
	LIB_FUNC("Ec63y59l9tw", NpManager::NpSetNpTitleId);
	LIB_FUNC("A2CQ3kgSopQ", NpManager::NpSetContentRestriction);
	LIB_FUNC("VfRSmPmj8Q8", NpManager::NpRegisterStateCallback);
	LIB_FUNC("qQJfO8HAiaY", NpManager::NpRegisterStateCallbackA);
	LIB_FUNC("M3wFXbYQtAA", NpManager::NpUnregisterStateCallback);
	LIB_FUNC("uFJpaKNBAj4", NpManager::NpRegisterGamePresenceCallback);
	LIB_FUNC("GImICnh+boA", NpManager::NpRegisterPlusEventCallback);
	LIB_FUNC("+yqjab2fUJA", NpManager::NpRegisterPremiumEventCallback);
	LIB_FUNC("hw5KNqAAels", NpManager::NpRegisterNpReachabilityStateCallback);
	LIB_FUNC("p-o74CnoNzY", NpManager::NpGetNpId);
	LIB_FUNC("XDncXQIJUSk", NpManager::NpGetOnlineId);
	LIB_FUNC("rbknaUjpqWo", NpManager::NpGetAccountIdA);
	LIB_FUNC("VgYczPGB5ss", NpManager::NpGetUserIdByAccountId);
	LIB_FUNC("JT+t00a3TxA", NpManager::NpGetAccountCountryA);
	LIB_FUNC("+4DegjBqV1g", NpManager::NpGetAccountAge);
	LIB_FUNC("3Tcz5bNCfZQ", NpManager::NpGetAccountLanguage2);
	LIB_FUNC("GpLQDNKICac", NpManager::NpCreateRequest);
	LIB_FUNC("eiqMCt9UshI", NpManager::NpCreateAsyncRequest);
	LIB_FUNC("S7QTn72PrDw", NpManager::NpDeleteRequest);
	LIB_FUNC("OzKvTvg3ZYU", NpManager::NpAbortRequest);
	LIB_FUNC("2rsFmlGWleQ", NpManager::NpCheckNpAvailability);
	LIB_FUNC("KfGZg2y73oM", NpManager::NpCheckNpReachability);
	LIB_FUNC("uqcPJLWL08M", NpManager::NpPollAsync);
	LIB_FUNC("O80NrhUOPGY", NpManager::NpCheckPremium);
	LIB_FUNC("eQH7nWPcAgc", NpManager::NpGetState);
	LIB_FUNC("e-ZuhGEoeC4", NpManager::NpGetNpReachabilityState);
	LIB_FUNC("Oad3rvY-NJQ", NpManager::NpHasSignedUp);
}

} // namespace LibNpManager

namespace LibNpPartner {

LIB_NAME("libSceNpPartner001", "libSceNpPartner001");

namespace {
bool g_np_partner_initialized = false;
}

// sceNpEAAccessInitialize/Terminate are no-arg on PS5 (AnyPS5 research, docs/dev/debt/
// libSceNpPartner001.md). UFC 6 (PPSA23566) imports both and calls Initialize at startup.
static int KYTY_SYSV_ABI NpEAAccessInitialize() {
	PRINT_NAME();

	g_np_partner_initialized = true;
	return OK;
}

static int KYTY_SYSV_ABI NpEAAccessTerminate() {
	PRINT_NAME();

	g_np_partner_initialized = false;
	return OK;
}

// From AnyPS5's PPSA34015 disassembly: (int userId, bool* result); EA Access has no
// server here, so report no subscription.
static int KYTY_SYSV_ABI NpHasEAAccessSubscription(int /*user_id*/, bool* has_subscription) {
	PRINT_NAME();

	if (has_subscription == nullptr) {
		return OK;
	}
	*has_subscription = false;
	return OK;
}

static int KYTY_SYSV_ABI NpHasEAAccessSubscriptionAbortRequest() {
	PRINT_NAME();
	return OK;
}

LIB_DEFINE(InitNet_1_NpPartner) {
	LIB_FUNC("pMxXhNozUX8", NpEAAccessTerminate);
	LIB_FUNC("pQfYTZHznMc", NpHasEAAccessSubscriptionAbortRequest);
	LIB_FUNC("7CxI50-xlCk", NpEAAccessInitialize);
	LIB_FUNC("+OnbUs1CV0M", NpHasEAAccessSubscription);
}

} // namespace LibNpPartner

namespace LibNpSessionSignaling {

LIB_NAME("NpSessionSignaling", "NpSessionSignaling");

constexpr int NP_SESSION_SIGNALING_ERROR_NOT_INITIALIZED     = -2141900031; /* 0x80553301 */
constexpr int NP_SESSION_SIGNALING_ERROR_ALREADY_INITIALIZED = -2141900030; /* 0x80553302 */

// While initialized, the library keeps its command event queue and both ends of its event socket
// pair open.
static std::array<int, 3> g_descriptors {};

static int KYTY_SYSV_ABI NpSessionSignalingInitialize(void* param) {
	PRINT_NAME();

	LOGF("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));

	if (g_descriptors[0] != 0) {
		return NP_SESSION_SIGNALING_ERROR_ALREADY_INITIALIZED;
	}
	for (auto& d: g_descriptors) {
		d = LibKernel::Descriptors::Allocate();
	}
	return OK;
}

static int KYTY_SYSV_ABI NpSessionSignalingTerminate() {
	PRINT_NAME();

	if (g_descriptors[0] == 0) {
		return NP_SESSION_SIGNALING_ERROR_NOT_INITIALIZED;
	}
	for (auto& d: g_descriptors) {
		LibKernel::Descriptors::Release(d);
		d = 0;
	}
	return OK;
}

LIB_DEFINE(InitNet_1_NpSessionSignaling) {
	LIB_FUNC("ysmw6J-P8Ak", NpSessionSignalingInitialize);
	LIB_FUNC("CqJuNXo5yiM", NpSessionSignalingTerminate);
}

} // namespace LibNpSessionSignaling

namespace LibShare {

LIB_NAME("Share", "Share");

static int KYTY_SYSV_ABI ShareInitialize(size_t heap_size, int thread_priority,
                                         uint64_t affinity_mask) {
	PRINT_NAME();

	LOGF("\t heap_size       = %" PRIu64 "\n"
	     "\t thread_priority = %d\n"
	     "\t affinity_mask   = 0x%016" PRIx64 "\n",
	     heap_size, thread_priority, affinity_mask);

	return 0;
}

static int KYTY_SYSV_ABI ShareFeaturePermit(uint32_t feature_flags) {
	PRINT_NAME();

	LOGF("\t feature_flags = 0x%08" PRIx32 "\n", feature_flags);

	return 0;
}

static int KYTY_SYSV_ABI ShareTerminate() {
	PRINT_NAME();

	return 0;
}

LIB_DEFINE(InitNet_1_Share) {
	LIB_FUNC("nBDD66kiFW8", LibShare::ShareInitialize);
	LIB_FUNC("YBiIdcDPrxs", LibShare::ShareFeaturePermit);
	LIB_FUNC("0IL1keINExQ", LibShare::ShareTerminate);
}

} // namespace LibShare

namespace LibJson2 {
LIB_DEFINE(InitNet_1_Json2);
} // namespace LibJson2

namespace LibRandom {

LIB_NAME("Random", "Random");

constexpr int  RANDOM_ERROR_INVALID = 0x817c0016;
constexpr auto RANDOM_MAX_SIZE      = 64u;

static int KYTY_SYSV_ABI RandomGetRandomNumber(void* buf, size_t size) {
	PRINT_NAME();

	if ((buf == nullptr && size != 0) || size > RANDOM_MAX_SIZE) {
		return RANDOM_ERROR_INVALID;
	}

	static uint32_t seed = 0x9e3779b9u;
	auto*           out  = static_cast<uint8_t*>(buf);
	for (size_t i = 0; i < size; i++) {
		seed   = seed * 1664525u + 1013904223u;
		out[i] = static_cast<uint8_t>(seed >> 24u);
	}

	return 0;
}

LIB_DEFINE(InitPlatform_1_Random) {
	LIB_FUNC("PI7jIZj4pcE", LibRandom::RandomGetRandomNumber);
}

} // namespace LibRandom

namespace LibRemoteplay {

LIB_NAME("Remoteplay", "Remoteplay");

static int KYTY_SYSV_ABI RemoteplayInitialize(void* heap, size_t heap_size) {
	PRINT_NAME();

	LOGF("\t heap      = 0x%016" PRIx64 "\n"
	     "\t heap_size = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(heap), static_cast<uint64_t>(heap_size));

	return 0;
}

static int KYTY_SYSV_ABI RemoteplayTerminate() {
	PRINT_NAME();

	return 0;
}

static int KYTY_SYSV_ABI RemoteplayGetConnectionStatus(int user_id, int* status) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n", user_id);

	if (status == nullptr) {
		return -1;
	}

	*status = 0;

	return 0;
}

LIB_DEFINE(InitPlatform_1_Remoteplay) {
	LIB_FUNC("k1SwgkMSOM8", LibRemoteplay::RemoteplayInitialize);
	LIB_FUNC("BOwybKVa3Do", LibRemoteplay::RemoteplayTerminate);
	LIB_FUNC("g3PNjYKWqnQ", LibRemoteplay::RemoteplayGetConnectionStatus);
}

} // namespace LibRemoteplay

namespace LibWebBrowserDialog {

LIB_NAME("WebBrowserDialog", "WebBrowserDialog");

static int KYTY_SYSV_ABI WebBrowserDialogInitialize() {
	PRINT_NAME();

	return 0;
}

static int KYTY_SYSV_ABI WebBrowserDialogTerminate() {
	PRINT_NAME();

	return 0;
}

LIB_DEFINE(InitPlatform_1_WebBrowserDialog) {
	LIB_FUNC("jqb7HntFQFc", LibWebBrowserDialog::WebBrowserDialogInitialize);
	LIB_FUNC("ocHtyBwHfys", LibWebBrowserDialog::WebBrowserDialogTerminate);
}

} // namespace LibWebBrowserDialog

namespace LibGameLiveStreaming {

LIB_NAME("GameLiveStreaming", "GameLiveStreaming");

constexpr int GAME_LIVE_STREAMING_ERROR_INVALID_PARAM = -2136997886;

struct GameLiveStreamingStatus2 {
	int32_t  user_id;
	bool     is_on_air;
	uint32_t spectator_count;
	uint32_t text_message_count;
	uint32_t command_message_count;
	uint8_t  reserved[52];
};

static_assert(sizeof(GameLiveStreamingStatus2) == 72);
static_assert(offsetof(GameLiveStreamingStatus2, is_on_air) == 4);
static_assert(offsetof(GameLiveStreamingStatus2, spectator_count) == 8);

static int KYTY_SYSV_ABI GameLiveStreamingInitialize(size_t heap_size) {
	PRINT_NAME();

	LOGF("\t heap_size = %" PRIu64 "\n", static_cast<uint64_t>(heap_size));

	return 0;
}

static int KYTY_SYSV_ABI GameLiveStreamingTerminate() {
	PRINT_NAME();

	return 0;
}

static int KYTY_SYSV_ABI GameLiveStreamingGetCurrentStatus2(GameLiveStreamingStatus2* status) {
	PRINT_NAME();

	if (status == nullptr) {
		return GAME_LIVE_STREAMING_ERROR_INVALID_PARAM;
	}

	std::memset(status, 0, sizeof(*status));
	status->user_id = -1;
	return OK;
}

static int KYTY_SYSV_ABI GameLiveStreamingGetSocialFeedbackMessagesCount(int type, uint32_t* count) {
	PRINT_NAME();

	if (type < 1 || type > 3 || count == nullptr) {
		return GAME_LIVE_STREAMING_ERROR_INVALID_PARAM;
	}

	*count = 0;
	return OK;
}

LIB_DEFINE(InitPlatform_1_GameLiveStreaming) {
	LIB_FUNC("kvYEw2lBndk", LibGameLiveStreaming::GameLiveStreamingInitialize);
	LIB_FUNC("9yK6Fk8mKOQ", LibGameLiveStreaming::GameLiveStreamingTerminate);
	LIB_FUNC("lK8dLBNp9OE", LibGameLiveStreaming::GameLiveStreamingGetCurrentStatus2);
	LIB_FUNC("yeQKjHETi40", LibGameLiveStreaming::GameLiveStreamingGetSocialFeedbackMessagesCount);
}

} // namespace LibGameLiveStreaming

LIB_DEFINE(InitPlatform_1) {
	LibRandom::InitPlatform_1_Random(s);
	LibRemoteplay::InitPlatform_1_Remoteplay(s);
	LibWebBrowserDialog::InitPlatform_1_WebBrowserDialog(s);
	LibGameLiveStreaming::InitPlatform_1_GameLiveStreaming(s);
	LibSharePlay::InitPlatform_1_SharePlay(s);
}

LIB_DEFINE(InitNet_1) {
	LibNet::InitNet_1_Net(s);
	LibSsl::InitNet_1_Ssl(s);
	LibHttp::InitNet_1_Http(s);
	LibHttp2::InitNet_1_Http2(s);
	LibNetCtl::InitNet_1_NetCtl(s);
	LibNpCommerce::InitNet_1_NpCommerce(s);
	LibNpManager::InitNet_1_NpManager(s);
	LibNpPartner::InitNet_1_NpPartner(s);
	LibNpSessionSignaling::InitNet_1_NpSessionSignaling(s);
	LibNpEntitlementAccess::InitNet_1_NpEntitlementAccess(s);
	LibNpEntitlementAccessPft::InitNet_1_NpEntitlementAccessPft(s);
	LibNpAuth::InitNet_1_NpAuth(s);
	LibNpTrophy2::InitNet_1_NpTrophy2(s);
	LibNpUniversalDataSystem::InitNet_1_NpUniversalDataSystem(s);
	LibNpGameIntent::InitNet_1_NpGameIntent(s);
	LibNpWebApi2::InitNet_1_NpWebApi2(s);
	LibGameUpdate::InitNet_1_GameUpdate(s);
	LibShare::InitNet_1_Share(s);
	LibJson2::InitNet_1_Json2(s);
}

} // namespace Libs
