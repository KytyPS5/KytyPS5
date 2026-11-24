#include "common/abi.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "loader/symbolDatabase.h"

#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

namespace Libs {

namespace LibNpAuth {

LIB_NAME("NpAuth", "NpAuth");

constexpr int NP_AUTH_MAX_REQUEST_NUM         = 16;
constexpr int NP_AUTH_REQUEST_ID_OFFSET       = 0x10000000;
constexpr int NP_AUTH_ERROR_INVALID_ARGUMENT  = -2141912319; /* 0x80550301 */
constexpr int NP_AUTH_ERROR_INVALID_SIZE      = -2141912318; /* 0x80550302 */
constexpr int NP_AUTH_ERROR_ABORTED           = -2141912316; /* 0x80550304 */
constexpr int NP_AUTH_ERROR_REQUEST_MAX       = -2141912315; /* 0x80550305 */
constexpr int NP_AUTH_ERROR_REQUEST_NOT_FOUND = -2141912314; /* 0x80550306 */
constexpr int NP_AUTH_ERROR_INVALID_ID        = -2141912313; /* 0x80550307 */
constexpr int NP_AUTH_ERROR_SERVICE_DOWN      = -2141912063; /* 0x80550401 */
constexpr int NP_ERROR_SIGNED_OUT             = -2141913082; /* 0x80550006 */

struct NpAuthGetAuthorizationCodeParameterV3 {
	size_t      size;
	int         user_id;
	uint8_t     padding[4];
	const void* client_id;
	const char* scope;
};

struct NpAuthGetIdTokenParameterV3 {
	size_t      size;
	int         user_id;
	uint8_t     padding[4];
	const void* client_id;
	const void* client_secret;
	const char* scope;
};

static_assert(sizeof(NpAuthGetAuthorizationCodeParameterV3) == 32);
static_assert(sizeof(NpAuthGetIdTokenParameterV3) == 40);

enum class NpAuthRequestState {
	None = 0,
	Ready,
	Aborted,
	Complete,
};

struct NpAuthRequest {
	NpAuthRequestState state  = NpAuthRequestState::None;
	bool               async  = false;
	int                result = 0;
};

static std::mutex                 g_np_auth_request_mutex;
static int                        g_np_auth_active_requests = 0;
static std::vector<NpAuthRequest> g_np_auth_requests;

static int np_auth_request_index(int req_id) {
	return req_id - NP_AUTH_REQUEST_ID_OFFSET - 1;
}

static NpAuthRequest* np_auth_find_request(int req_id) {
	const int index = np_auth_request_index(req_id);
	if (index < 0 || g_np_auth_active_requests == 0 ||
	    static_cast<size_t>(index) >= g_np_auth_requests.size() ||
	    g_np_auth_requests[index].state == NpAuthRequestState::None) {
		return nullptr;
	}

	return &g_np_auth_requests[index];
}

static int np_auth_create_request(bool async) {
	PRINT_NAME();

	LOGF("\t async = %d\n", async ? 1 : 0);

	std::scoped_lock lock(g_np_auth_request_mutex);

	if (g_np_auth_active_requests >= NP_AUTH_MAX_REQUEST_NUM) {
		return NP_AUTH_ERROR_REQUEST_MAX;
	}

	size_t index = 0;
	for (; index < g_np_auth_requests.size(); index++) {
		if (g_np_auth_requests[index].state == NpAuthRequestState::None) {
			g_np_auth_requests[index] = {NpAuthRequestState::Ready, async, 0};
			break;
		}
	}

	if (index == g_np_auth_requests.size()) {
		g_np_auth_requests.push_back({NpAuthRequestState::Ready, async, 0});
	}

	g_np_auth_active_requests++;

	return static_cast<int>(index) + NP_AUTH_REQUEST_ID_OFFSET + 1;
}

static int KYTY_SYSV_ABI NpAuthCreateRequest() {
	return np_auth_create_request(false);
}

static int KYTY_SYSV_ABI NpAuthCreateAsyncRequest(const void* param) {
	PRINT_NAME();

	LOGF("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));

	return np_auth_create_request(true);
}

static int KYTY_SYSV_ABI NpAuthDeleteRequest(int req_id) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);

	std::scoped_lock lock(g_np_auth_request_mutex);

	auto* request = np_auth_find_request(req_id);
	if (request == nullptr) {
		return NP_AUTH_ERROR_REQUEST_NOT_FOUND;
	}

	request->state  = NpAuthRequestState::None;
	request->async  = false;
	request->result = 0;
	g_np_auth_active_requests--;

	return 0;
}

static int KYTY_SYSV_ABI NpAuthAbortRequest(int req_id) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);

	std::scoped_lock lock(g_np_auth_request_mutex);

	auto* request = np_auth_find_request(req_id);
	if (request == nullptr) {
		return NP_AUTH_ERROR_REQUEST_NOT_FOUND;
	}

	if (request->state != NpAuthRequestState::Complete) {
		request->state = NpAuthRequestState::Aborted;
	}

	return 0;
}

static int np_auth_wait_or_poll_async(int req_id, int* result) {
	if (result == nullptr) {
		return NP_AUTH_ERROR_INVALID_ARGUMENT;
	}

	std::scoped_lock lock(g_np_auth_request_mutex);

	auto* request = np_auth_find_request(req_id);
	if (request == nullptr) {
		return NP_AUTH_ERROR_REQUEST_NOT_FOUND;
	}

	if (!request->async || request->state == NpAuthRequestState::Ready) {
		return NP_AUTH_ERROR_INVALID_ID;
	}

	*result = request->result;
	return 0;
}

static int np_auth_complete_unavailable(NpAuthRequest* request, int user_id) {
	if (request->state == NpAuthRequestState::Complete) {
		request->result = NP_AUTH_ERROR_INVALID_ARGUMENT;
		return NP_AUTH_ERROR_INVALID_ARGUMENT;
	}
	if (request->state == NpAuthRequestState::Aborted) {
		request->result = NP_AUTH_ERROR_ABORTED;
		return NP_AUTH_ERROR_ABORTED;
	}

	request->state  = NpAuthRequestState::Complete;
	// Emulating sign-in cannot provide a real authorization code or ID token.
	request->result = Network::NpManager::IsSignedIn(user_id) ? NP_AUTH_ERROR_SERVICE_DOWN
	                                                        : NP_ERROR_SIGNED_OUT;

	return request->async ? 0 : request->result;
}

static int KYTY_SYSV_ABI NpAuthWaitAsync(int req_id, int* result) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);
	LOGF("\t result = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(result));

	return np_auth_wait_or_poll_async(req_id, result);
}

static int KYTY_SYSV_ABI NpAuthPollAsync(int req_id, int* result) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);
	LOGF("\t result = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(result));

	return np_auth_wait_or_poll_async(req_id, result);
}

static int KYTY_SYSV_ABI NpAuthGetAuthorizationCodeV3(
    int req_id, const NpAuthGetAuthorizationCodeParameterV3* param, void* auth_code,
    int* issuer_id) {
	PRINT_NAME();

	LOGF("\t req_id    = %d\n", req_id);
	LOGF("\t param     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	LOGF("\t auth_code = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(auth_code));
	LOGF("\t issuer_id = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(issuer_id));

	if (req_id <= 0 || param == nullptr || auth_code == nullptr) {
		return NP_AUTH_ERROR_INVALID_ARGUMENT;
	}
	if (param->size < sizeof(*param)) {
		return NP_AUTH_ERROR_INVALID_SIZE;
	}
	if (param->client_id == nullptr || param->scope == nullptr) {
		return NP_AUTH_ERROR_INVALID_ARGUMENT;
	}

	memset(auth_code, 0, 136);
	if (issuer_id != nullptr) {
		*issuer_id = 0;
	}

	std::scoped_lock lock(g_np_auth_request_mutex);
	auto*            request = np_auth_find_request(req_id);
	if (request == nullptr) {
		return NP_AUTH_ERROR_REQUEST_NOT_FOUND;
	}
	return np_auth_complete_unavailable(request, param->user_id);
}

static int KYTY_SYSV_ABI NpAuthGetIdTokenV3(int req_id, const NpAuthGetIdTokenParameterV3* param,
                                        void* id_token) {
	PRINT_NAME();

	LOGF("\t req_id   = %d\n", req_id);
	LOGF("\t param    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	LOGF("\t id_token = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(id_token));

	if (req_id <= 0 || param == nullptr || id_token == nullptr) {
		return NP_AUTH_ERROR_INVALID_ARGUMENT;
	}
	if (param->size < sizeof(*param)) {
		return NP_AUTH_ERROR_INVALID_SIZE;
	}
	if (param->client_id == nullptr || param->client_secret == nullptr || param->scope == nullptr) {
		return NP_AUTH_ERROR_INVALID_ARGUMENT;
	}

	memset(id_token, 0, 4104);

	std::scoped_lock lock(g_np_auth_request_mutex);
	auto*            request = np_auth_find_request(req_id);
	if (request == nullptr) {
		return NP_AUTH_ERROR_REQUEST_NOT_FOUND;
	}
	return np_auth_complete_unavailable(request, param->user_id);
}

LIB_DEFINE(InitNet_1_NpAuth) {
	LIB_FUNC("6bwFkosYRQg", LibNpAuth::NpAuthCreateRequest);
	LIB_FUNC("N+mr7GjTvr8", LibNpAuth::NpAuthCreateAsyncRequest);
	LIB_FUNC("H8wG9Bk-nPc", LibNpAuth::NpAuthDeleteRequest);
	LIB_FUNC("cE7wIsqXdZ8", LibNpAuth::NpAuthAbortRequest);
	LIB_FUNC("SK-S7daqJSE", LibNpAuth::NpAuthWaitAsync);
	LIB_FUNC("gjSyfzSsDcE", LibNpAuth::NpAuthPollAsync);
	LIB_FUNC("KI4dHLlTNl0", LibNpAuth::NpAuthGetAuthorizationCodeV3);
	LIB_FUNC("RdsFVsgSpZY", LibNpAuth::NpAuthGetIdTokenV3);
}

} // namespace LibNpAuth

} // namespace Libs
