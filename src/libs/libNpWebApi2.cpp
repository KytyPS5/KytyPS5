#include "common/abi.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>

namespace Libs {

namespace LibNpWebApi2 {

LIB_NAME("NpWebApi2", "NpWebApi2");

constexpr int NP_WEBAPI2_ERROR_INVALID_ARGUMENT       = -2141899774; /* 0x80553402 */
constexpr int NP_WEBAPI2_ERROR_INVALID_LIB_CONTEXT_ID = -2141899773; /* 0x80553403 */
constexpr int NP_WEBAPI2_ERROR_LIB_CONTEXT_NOT_FOUND  = -2141899772; /* 0x80553404 */
constexpr int NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND      = -2141899770; /* 0x80553406 */
constexpr int NP_WEBAPI2_ERROR_NOT_SIGNED_IN          = -2141899769; /* 0x80553407 */

struct NpWebApi2MemoryPoolStats {
	size_t  pool_size;
	size_t  max_inuse_size;
	size_t  current_inuse_size;
	int32_t reserved;
};

static std::mutex            g_np_webapi2_context_mutex;
static std::map<int, size_t> g_np_webapi2_contexts;

struct NpWebApi2ResponseInformationOption {
	int32_t http_status;
	char*   error_object;
	size_t  error_object_size;
	size_t  response_data_size;
};

struct NpWebApi2Request {
	std::string api_group;
	std::string path;
	std::string method;
	std::string response;
	size_t      read_offset = 0;
};

static std::map<int64_t, NpWebApi2Request>& NpWebApi2Requests() {
	static std::map<int64_t, NpWebApi2Request> requests;
	return requests;
}

static int KYTY_SYSV_ABI NpWebApi2Initialize(int lib_http_ctx_id, size_t pool_size) {
	PRINT_NAME();

	LOGF("\t lib_http_ctx_id = %d\n"
	     "\t pool_size       = %" PRIu64 "\n",
	     lib_http_ctx_id, pool_size);

	static int id = 0;

	if (pool_size > std::numeric_limits<size_t>::max() - 0x3fff) {
		return NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
	}
	std::lock_guard lock(g_np_webapi2_context_mutex);
	g_np_webapi2_contexts[++id] = (pool_size + 0x3fff) & ~size_t {0x3fff};
	return id;
}

static int KYTY_SYSV_ABI NpWebApi2GetMemoryPoolStats(int lib_ctx_id,
                                                   NpWebApi2MemoryPoolStats* stats) {
	if (stats == nullptr) {
		return NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
	}
	if (lib_ctx_id <= 0) {
		return NP_WEBAPI2_ERROR_INVALID_LIB_CONTEXT_ID;
	}
	std::lock_guard lock(g_np_webapi2_context_mutex);
	const auto context = g_np_webapi2_contexts.find(lib_ctx_id);
	if (context == g_np_webapi2_contexts.end()) {
		return NP_WEBAPI2_ERROR_LIB_CONTEXT_NOT_FOUND;
	}
	// Requests currently use host storage, not allocations from the library pool.
	*stats = {context->second, 0, 0, 0};
	return OK;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventCreateHandle(int lib_ctx_id) {
	PRINT_NAME();

	LOGF("\t lib_ctx_id = %d\n", lib_ctx_id);

	static int handle = 0;

	return ++handle;
}

static int KYTY_SYSV_ABI NpWebApi2CreateUserContext(int lib_ctx_id, int user_id) {
	PRINT_NAME();

	LOGF("\t lib_ctx_id = %d\n"
	     "\t user_id    = %d\n",
	     lib_ctx_id, user_id);

	static int user_context = 1000;

	return ++user_context;
}

static int KYTY_SYSV_ABI NpWebApi2DeleteUserContext(int user_context_id) {
	PRINT_NAME();

	LOGF("\t user_context_id = %d\n", user_context_id);

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2CreateRequest(int user_context_id, const char* api_group,
                                                const char* path, const char* method,
                                                const void* content_parameter,
                                                int64_t*    request_id) {
	PRINT_NAME();

	LOGF("\t user_context_id   = %d\n", user_context_id);
	LOGF("\t api_group         = %s\n", api_group != nullptr ? api_group : "(null)");
	LOGF("\t path              = %s\n", path != nullptr ? path : "(null)");
	LOGF("\t method            = %s\n", method != nullptr ? method : "(null)");
	LOGF("\t content_parameter = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(content_parameter));
	LOGF("\t request_id        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(request_id));

	static int64_t next_request_id = 1000;

	if (request_id != nullptr) {
		*request_id = next_request_id++;
		NpWebApi2Request request {};
		request.api_group                = (api_group != nullptr ? api_group : "");
		request.path                     = (path != nullptr ? path : "");
		request.method                   = (method != nullptr ? method : "");
		NpWebApi2Requests()[*request_id] = request;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2AbortRequest(int64_t request_id) {
	PRINT_NAME();

	LOGF("\t request_id = %" PRIi64 "\n", request_id);

	return (NpWebApi2Requests().find(request_id) == NpWebApi2Requests().end()
	            ? NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND
	            : 0);
}

static int KYTY_SYSV_ABI
NpWebApi2SendRequest(int64_t request_id, const void* data, size_t data_size,
                     NpWebApi2ResponseInformationOption* response_info_option) {
	PRINT_NAME();

	LOGF("\t request_id           = %" PRIi64 "\n", request_id);
	LOGF("\t data                 = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
	LOGF("\t data_size            = %" PRIu64 "\n", data_size);
	LOGF("\t response_info_option = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(response_info_option));

	auto request = NpWebApi2Requests().find(request_id);
	if (request == NpWebApi2Requests().end()) {
		return NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND;
	}

	request->second.response.clear();
	request->second.read_offset = 0;

	if (response_info_option != nullptr) {
		response_info_option->http_status        = 0;
		response_info_option->response_data_size = request->second.response.size();
		if (response_info_option->error_object != nullptr &&
		    response_info_option->error_object_size > 0) {
			response_info_option->error_object[0] = '\0';
		}
	}

	return NP_WEBAPI2_ERROR_NOT_SIGNED_IN;
}

static int KYTY_SYSV_ABI NpWebApi2ReadData(int64_t request_id, void* data, size_t size) {
	PRINT_NAME();

	LOGF("\t request_id = %" PRIi64 "\n", request_id);
	LOGF("\t data       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
	LOGF("\t size       = %" PRIu64 "\n", size);

	if (data == nullptr || size == 0) {
		return NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
	}

	auto request = NpWebApi2Requests().find(request_id);
	if (request == NpWebApi2Requests().end()) {
		return NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND;
	}

	const auto remaining = request->second.response.size() - request->second.read_offset;
	const auto to_copy   = (remaining < size ? remaining : size);
	if (to_copy > 0) {
		std::memcpy(data, request->second.response.data() + request->second.read_offset, to_copy);
		request->second.read_offset += to_copy;
	}

	return static_cast<int>(to_copy);
}

static int KYTY_SYSV_ABI NpWebApi2DeleteRequest(int64_t request_id) {
	PRINT_NAME();

	LOGF("\t request_id = %" PRIi64 "\n", request_id);

	NpWebApi2Requests().erase(request_id);

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2AddHttpRequestHeader(int64_t request_id, const char* field_name,
                                                       const char* value) {
	PRINT_NAME();

	LOGF("\t request_id = %" PRIi64 "\n", request_id);
	LOGF("\t field_name = %s\n", field_name != nullptr ? field_name : "(null)");
	LOGF("\t value      = %s\n", value != nullptr ? value : "(null)");

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2GetHttpResponseHeaderValueLength(int64_t     request_id,
                                                                   const char* field_name,
                                                                   size_t*     value_length) {
	PRINT_NAME();

	LOGF("\t request_id   = %" PRIi64 "\n", request_id);
	LOGF("\t field_name   = %s\n", field_name != nullptr ? field_name : "(null)");
	LOGF("\t value_length = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(value_length));

	if (field_name == nullptr || value_length == nullptr) {
		return NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
	}
	if (NpWebApi2Requests().find(request_id) == NpWebApi2Requests().end()) {
		return NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND;
	}

	*value_length = (std::strcmp(field_name, "Content-Type") == 0 ||
	                         std::strcmp(field_name, "content-type") == 0
	                     ? std::strlen("application/json; charset=utf-8") + 1
	                     : 0);

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2GetHttpResponseHeaderValue(int64_t     request_id,
                                                             const char* field_name, char* value,
                                                             size_t value_size) {
	PRINT_NAME();

	LOGF("\t request_id = %" PRIi64 "\n", request_id);
	LOGF("\t field_name = %s\n", field_name != nullptr ? field_name : "(null)");
	LOGF("\t value      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(value));
	LOGF("\t value_size = %" PRIu64 "\n", value_size);

	if (field_name == nullptr || value == nullptr || value_size == 0) {
		return NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
	}
	if (NpWebApi2Requests().find(request_id) == NpWebApi2Requests().end()) {
		return NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND;
	}

	const char* header = "";
	if (std::strcmp(field_name, "Content-Type") == 0 ||
	    std::strcmp(field_name, "content-type") == 0) {
		header = "application/json; charset=utf-8";
	}

	const auto copy_size =
	    std::strlen(header) < value_size - 1 ? std::strlen(header) : value_size - 1;
	std::memcpy(value, header, copy_size);
	value[copy_size] = '\0';

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventDeleteHandle(int lib_ctx_id, int handle_id) {
	PRINT_NAME();

	LOGF("\t lib_ctx_id = %d\n", lib_ctx_id);
	LOGF("\t handle_id  = %d\n", handle_id);

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventDeletePushContext(int         user_context_id,
                                                             const void* push_context_id) {
	PRINT_NAME();

	LOGF("\t user_context_id = %d\n", user_context_id);
	LOGF("\t push_context_id = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(push_context_id));

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventCreateFilter(int lib_ctx_id, int handle_id,
                                                        const char* np_service_name,
                                                        uint32_t    np_service_label,
                                                        const void* filter_param,
                                                        size_t      filter_param_num) {
	PRINT_NAME();

	LOGF("\t lib_ctx_id       = %d\n", lib_ctx_id);
	LOGF("\t handle_id        = %d\n", handle_id);
	LOGF("\t np_service_name  = %s\n", np_service_name != nullptr ? np_service_name : "(null)");
	LOGF("\t np_service_label = %" PRIu32 "\n", np_service_label);
	LOGF("\t filter_param     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(filter_param));
	LOGF("\t filter_param_num = %" PRIu64 "\n", filter_param_num);

	static int filter_id = 1;

	return filter_id++;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventRegisterCallback(int user_context_id, int filter_id,
                                                            void* callback, void* user_arg) {
	PRINT_NAME();

	LOGF("\t user_context_id = %d\n", user_context_id);
	LOGF("\t filter_id       = %d\n", filter_id);
	LOGF("\t callback        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(callback));
	LOGF("\t user_arg        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(user_arg));

	static int callback_id = 1;

	return callback_id++;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventDeleteFilter(int lib_ctx_id, int filter_id) {
	PRINT_NAME();

	LOGF("\t lib_ctx_id = %d\n", lib_ctx_id);
	LOGF("\t filter_id  = %d\n", filter_id);

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventUnregisterCallback(int user_context_id,
                                                              int callback_id) {
	PRINT_NAME();

	LOGF("\t user_context_id = %d\n", user_context_id);
	LOGF("\t callback_id     = %d\n", callback_id);

	return 0;
}

static void KYTY_SYSV_ABI NpWebApi2CheckTimeout() {
	// Timeout processing is an internal maintenance tick in Prospero. Requests
	// complete synchronously in this implementation, so there is no pending
	// timeout state to advance.
}

static int KYTY_SYSV_ABI NpWebApi2Terminate(int lib_ctx_id) {
	PRINT_NAME();

	LOGF("\t lib_ctx_id = %d\n", lib_ctx_id);

	if (lib_ctx_id <= 0) {
		return NP_WEBAPI2_ERROR_INVALID_LIB_CONTEXT_ID;
	}
	std::lock_guard lock(g_np_webapi2_context_mutex);
	return g_np_webapi2_contexts.erase(lib_ctx_id) != 0 ? OK : NP_WEBAPI2_ERROR_LIB_CONTEXT_NOT_FOUND;
}

LIB_DEFINE(InitNet_1_NpWebApi2) {
	LIB_FUNC("+o9816YQhqQ", LibNpWebApi2::NpWebApi2Initialize);
	LIB_FUNC("Xweb+naPZ8Y", LibNpWebApi2::NpWebApi2GetMemoryPoolStats);
	LIB_FUNC("WV1GwM32NgY", LibNpWebApi2::NpWebApi2PushEventCreateHandle);
	LIB_FUNC("sk54bi6FtYM", LibNpWebApi2::NpWebApi2CreateUserContext);
	LIB_FUNC("9X9+cneTGUU", LibNpWebApi2::NpWebApi2DeleteUserContext);
	LIB_FUNC("3EI-OSJ65Xc", LibNpWebApi2::NpWebApi2CreateRequest);
	LIB_FUNC("lQOCF84lvzw", LibNpWebApi2::NpWebApi2SendRequest);
	LIB_FUNC("OOY9+ObfKec", LibNpWebApi2::NpWebApi2ReadData);
	LIB_FUNC("zpiPsH7dbFQ", LibNpWebApi2::NpWebApi2AbortRequest);
	LIB_FUNC("vvzWO-DvG1s", LibNpWebApi2::NpWebApi2DeleteRequest);
	LIB_FUNC("egOOvrnF6mI", LibNpWebApi2::NpWebApi2AddHttpRequestHeader);
	LIB_FUNC("HwP3aM+c85c", LibNpWebApi2::NpWebApi2GetHttpResponseHeaderValueLength);
	LIB_FUNC("hksbskNToEA", LibNpWebApi2::NpWebApi2GetHttpResponseHeaderValue);
	LIB_FUNC("fIATVMo4Y1w", LibNpWebApi2::NpWebApi2PushEventDeleteHandle);
	LIB_FUNC("QafxeZM3WK4", LibNpWebApi2::NpWebApi2PushEventDeletePushContext);
	LIB_FUNC("MsaFhR+lPE4", LibNpWebApi2::NpWebApi2PushEventCreateFilter);
	LIB_FUNC("fY3QqeNkF8k", LibNpWebApi2::NpWebApi2PushEventRegisterCallback);
	LIB_FUNC("KJdPcOGmK58", LibNpWebApi2::NpWebApi2PushEventDeleteFilter);
	LIB_FUNC("hOnIlcGrO6g", LibNpWebApi2::NpWebApi2PushEventUnregisterCallback);
	LIB_FUNC("3Tt9zL3tkoc", LibNpWebApi2::NpWebApi2CheckTimeout);
	LIB_FUNC("bEvXpcEk200", LibNpWebApi2::NpWebApi2Terminate);
}

} // namespace LibNpWebApi2

} // namespace Libs
