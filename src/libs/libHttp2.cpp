#include "common/abi.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "kernel/eventQueue.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace Libs {

namespace LibHttp2 {

LIB_NAME("Http2", "Http2");

namespace EventQueue = LibKernel::EventQueue;

constexpr int HTTP2_ERROR_INVALID_ID    = -2122641152; /* 0x817B1100 */
constexpr int HTTP2_ERROR_BEFORE_SEND   = -2122641307; /* 0x817B1065 */
constexpr int HTTP2_ERROR_TIMEOUT       = -2122641304; /* 0x817B1068 */
constexpr int HTTP2_ERROR_NULL_POINTER  = -2122640859; /* 0x817B1225 */
constexpr int HTTP2_ERROR_INVALID_VALUE = -2122640898; /* 0x817B11FE */

struct Http2Options {
	bool     auth_enabled               = false;
	bool     auto_redirect              = false;
	bool     inflate_gzip               = false;
	uint32_t ssl_options                = 0;
	uint32_t resolve_timeout_us         = 0;
	uint32_t connect_timeout_us         = 0;
	uint32_t connection_wait_timeout_us = 0;
	uint32_t send_timeout_us            = 0;
	uint32_t recv_timeout_us            = 0;
	uint32_t timeout_us                 = 0;
	void*    ssl_callback               = nullptr;
	void*    ssl_callback_arg           = nullptr;
	void*    redirect_callback          = nullptr;
	void*    redirect_user_arg          = nullptr;
};

struct Http2Context {
	int    libnet_mem_id           = 0;
	int    libssl_ctx_id           = 0;
	size_t pool_size               = 0;
	int    max_concurrent_requests = 0;
};

struct Http2MemoryPoolStats {
	size_t  pool_size;
	size_t  max_inuse_size;
	size_t  current_inuse_size;
	int32_t reserved;
};

struct Http2Template {
	int          context_id = 0;
	std::string  user_agent;
	int          http_ver           = 0;
	int          is_auto_proxy_conf = 0;
	Http2Options options;
};

struct Http2Request {
	int                                              tmpl_id = 0;
	std::string                                      method;
	std::string                                      url;
	uint64_t                                         content_length = 0;
	std::vector<std::pair<std::string, std::string>> headers;
	int                                              send_result = HTTP2_ERROR_BEFORE_SEND;
	int                                              status_code = 0;
	std::string                                      response_headers;
	std::string                                      response_body;
	size_t                                           read_offset  = 0;
	int                                              async_result = HTTP2_ERROR_BEFORE_SEND;
	int                                              async_event  = 0;
	Http2Options                                     options;
};

struct Http2AsyncResult {
	int     event_type;
	int     req_id;
	int     result;
	uint8_t padding[4];
	void*   reserved;
};

struct Http2TriggerUserEventArgs {
	EventQueue::KernelEqueue eq;
	int                      uniq_id;
	uint8_t                  padding[4];
	void*                    udata;
	void*                    reserved;
};

static_assert(sizeof(Http2TriggerUserEventArgs) == 32);

static int g_http2_next_context_id  = 1;
static int g_http2_next_template_id = 1;
static int g_http2_next_request_id  = 1;

static std::map<int, Http2Context>  g_http2_contexts;
static std::map<int, Http2Template> g_http2_templates;
static std::map<int, Http2Request>  g_http2_requests;

static Http2Options* GetHttp2Options(int id) {
	auto request = g_http2_requests.find(id);
	if (request != g_http2_requests.end()) {
		return &request->second.options;
	}

	auto tmpl = g_http2_templates.find(id);
	if (tmpl != g_http2_templates.end()) {
		return &tmpl->second.options;
	}

	return nullptr;
}

static int KYTY_SYSV_ABI Http2Init(int libnet_mem_id, int libssl_ctx_id, size_t pool_size,
                                   int max_concurrent_request) {
	PRINT_NAME();

	LOGF("\t libnet_mem_id          = %d\n"
	     "\t libssl_ctx_id          = %d\n"
	     "\t pool_size              = %" PRIu64 "\n"
	     "\t max_concurrent_request = %d\n",
	     libnet_mem_id, libssl_ctx_id, pool_size, max_concurrent_request);

	const auto id        = g_http2_next_context_id++;
	g_http2_contexts[id] = {libnet_mem_id, libssl_ctx_id, pool_size, max_concurrent_request};

	return id;
}

static int KYTY_SYSV_ABI Http2CreateTemplate(int lib_http2_ctx_id, const char* user_agent,
                                             int http_ver, int is_auto_proxy_conf) {
	PRINT_NAME();

	LOGF("\t lib_http2_ctx_id   = %d\n", lib_http2_ctx_id);
	LOGF("\t user_agent         = %s\n", user_agent != nullptr ? user_agent : "(null)");
	LOGF("\t http_ver           = %d\n", http_ver);
	LOGF("\t is_auto_proxy_conf = %d\n", is_auto_proxy_conf);

	if (g_http2_contexts.find(lib_http2_ctx_id) == g_http2_contexts.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	const auto tmpl_id         = g_http2_next_template_id++;
	g_http2_templates[tmpl_id] = {lib_http2_ctx_id, user_agent != nullptr ? user_agent : "",
	                              http_ver, is_auto_proxy_conf};

	return tmpl_id;
}

static int KYTY_SYSV_ABI Http2DeleteTemplate(int tmpl_id) {
	PRINT_NAME();

	LOGF("\t tmpl_id = %d\n", tmpl_id);

	auto tmpl = g_http2_templates.find(tmpl_id);
	if (tmpl == g_http2_templates.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	for (auto request = g_http2_requests.begin(); request != g_http2_requests.end();) {
		if (request->second.tmpl_id == tmpl_id) {
			request = g_http2_requests.erase(request);
		} else {
			++request;
		}
	}

	g_http2_templates.erase(tmpl);

	return 0;
}

static int KYTY_SYSV_ABI Http2Term(int lib_http2_ctx_id) {
	PRINT_NAME();

	LOGF("\t lib_http2_ctx_id = %d\n", lib_http2_ctx_id);

	auto context = g_http2_contexts.find(lib_http2_ctx_id);
	if (context == g_http2_contexts.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	for (auto tmpl = g_http2_templates.begin(); tmpl != g_http2_templates.end();) {
		if (tmpl->second.context_id == lib_http2_ctx_id) {
			const auto tmpl_id = tmpl->first;
			for (auto request = g_http2_requests.begin(); request != g_http2_requests.end();) {
				if (request->second.tmpl_id == tmpl_id) {
					request = g_http2_requests.erase(request);
				} else {
					++request;
				}
			}
			tmpl = g_http2_templates.erase(tmpl);
		} else {
			++tmpl;
		}
	}

	g_http2_contexts.erase(context);

	return 0;
}

static int KYTY_SYSV_ABI Http2GetMemoryPoolStats(int lib_http2_ctx_id, Http2MemoryPoolStats* stats) {
	PRINT_NAME();

	if (stats == nullptr) {
		return HTTP2_ERROR_INVALID_VALUE;
	}

	const auto context = g_http2_contexts.find(lib_http2_ctx_id);
	if (context == g_http2_contexts.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	// Requests use host storage, not allocations from the library pool
	*stats = {context->second.pool_size, 0, 0, 0};
	return 0;
}

static int KYTY_SYSV_ABI Http2CreateRequestWithURL(int tmpl_id, const char* method, const char* url,
                                                   uint64_t content_length) {
	PRINT_NAME();

	LOGF("\t tmpl_id        = %d\n", tmpl_id);
	LOGF("\t method         = %s\n", method != nullptr ? method : "(null)");
	LOGF("\t url            = %s\n", url != nullptr ? url : "(null)");
	LOGF("\t content_length = %" PRIu64 "\n", content_length);

	if (method == nullptr || url == nullptr) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	if (g_http2_templates.find(tmpl_id) == g_http2_templates.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	const auto req_id        = g_http2_next_request_id++;
	g_http2_requests[req_id] = {tmpl_id, method, url, content_length};

	return req_id;
}

static int KYTY_SYSV_ABI Http2AddRequestHeader(int id, const char* name, const char* value,
                                               uint32_t mode) {
	PRINT_NAME();

	LOGF("\t id    = %d\n", id);
	LOGF("\t name  = %s\n", name != nullptr ? name : "(null)");
	LOGF("\t value = %s\n", value != nullptr ? value : "(null)");
	LOGF("\t mode  = %" PRIu32 "\n", mode);

	if (name == nullptr || value == nullptr) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	request->second.headers.emplace_back(name, value);

	return 0;
}

static int KYTY_SYSV_ABI Http2SetRequestContentLength(int id, uint64_t content_length) {
	PRINT_NAME();

	LOGF("\t id             = %d\n", id);
	LOGF("\t content_length = %" PRIu64 "\n", content_length);

	auto request = g_http2_requests.find(id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	request->second.content_length = content_length;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetAuthEnabled(int id, int is_enable) {
	PRINT_NAME();

	LOGF("\t id        = %d\n", id);
	LOGF("\t is_enable = %d\n", is_enable);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->auth_enabled = is_enable != 0;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetAutoRedirect(int id, int enable) {
	PRINT_NAME();

	LOGF("\t id     = %d\n", id);
	LOGF("\t enable = %d\n", enable);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->auto_redirect = enable != 0;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetInflateGZIPEnabled(int id, int enable) {
	PRINT_NAME();

	LOGF("\t id     = %d\n", id);
	LOGF("\t enable = %d\n", enable);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->inflate_gzip = enable != 0;

	return 0;
}

static int KYTY_SYSV_ABI Http2SslEnableOption(int id, uint32_t ssl_flags) {
	PRINT_NAME();

	LOGF("\t id        = %d\n", id);
	LOGF("\t ssl_flags = 0x%08" PRIx32 "\n", ssl_flags);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->ssl_options |= ssl_flags;

	return 0;
}

static int KYTY_SYSV_ABI Http2SslDisableOption(int id, uint32_t ssl_flags) {
	PRINT_NAME();

	LOGF("\t id        = %d\n", id);
	LOGF("\t ssl_flags = 0x%08" PRIx32 "\n", ssl_flags);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->ssl_options &= ~ssl_flags;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetSslCallback(int id, void* cb_func, void* user_arg) {
	PRINT_NAME();

	LOGF("\t id       = %d\n", id);
	LOGF("\t cb_func  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cb_func));
	LOGF("\t user_arg = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(user_arg));

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->ssl_callback     = cb_func;
	options->ssl_callback_arg = user_arg;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetRedirectCallback(int id, void* cb_func, void* user_arg) {
	PRINT_NAME();

	LOGF("\t id       = %d\n", id);
	LOGF("\t cb_func  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cb_func));
	LOGF("\t user_arg = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(user_arg));

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->redirect_callback = cb_func;
	options->redirect_user_arg = user_arg;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetConnectTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n", id);
	LOGF("\t usec = %" PRIu32 "\n", usec);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->connect_timeout_us = usec;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetConnectionWaitTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n", id);
	LOGF("\t usec = %" PRIu32 "\n", usec);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->connection_wait_timeout_us = usec;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetResolveTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n", id);
	LOGF("\t usec = %" PRIu32 "\n", usec);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->resolve_timeout_us = usec;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetResolveRetry(int, int) {
	return 0;
}

static int KYTY_SYSV_ABI Http2SetTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n", id);
	LOGF("\t usec = %" PRIu32 "\n", usec);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->timeout_us = usec;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetSendTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n", id);
	LOGF("\t usec = %" PRIu32 "\n", usec);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->send_timeout_us = usec;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetRecvTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n", id);
	LOGF("\t usec = %" PRIu32 "\n", usec);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->recv_timeout_us = usec;

	return 0;
}

static int KYTY_SYSV_ABI Http2SendRequest(int req_id, const void* post_data, size_t size) {
	PRINT_NAME();

	LOGF("\t req_id    = %d\n", req_id);
	LOGF("\t post_data = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(post_data));
	LOGF("\t size      = %" PRIu64 "\n", size);

	if (post_data == nullptr && size != 0) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	request->second.send_result = HTTP2_ERROR_TIMEOUT;

	return request->second.send_result;
}

// Async operations complete at once; the caller's event queue gets the completion event.
static void Http2TriggerUserEvent(const Http2TriggerUserEventArgs* kqueue_option) {
	if (kqueue_option != nullptr) {
		EventQueue::KernelTriggerUserEvent(kqueue_option->eq, kqueue_option->uniq_id,
		                                   kqueue_option->udata);
	}
}

static int KYTY_SYSV_ABI Http2SendRequestAsync(int req_id, const void* post_data, size_t size,
                                               const Http2TriggerUserEventArgs* kqueue_option,
                                               void* /*option*/) {
	PRINT_NAME();

	LOGF("\t req_id    = %d\n", req_id);
	LOGF("\t post_data = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(post_data));
	LOGF("\t size      = %" PRIu64 "\n", size);

	if (post_data == nullptr && size != 0) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	request->second.send_result  = HTTP2_ERROR_TIMEOUT;
	request->second.async_result = request->second.send_result;
	request->second.async_event  = 0;
	Http2TriggerUserEvent(kqueue_option);

	return 0;
}

static int KYTY_SYSV_ABI Http2WaitAsync(int req_id, Http2AsyncResult* result, uint32_t* /*timeout*/,
                                        void* /*option*/) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);
	LOGF("\t result = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(result));

	if (result == nullptr) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	*result            = {};
	result->event_type = request->second.async_event;
	result->req_id     = req_id;
	result->result     = request->second.async_result;

	return 0;
}

static int KYTY_SYSV_ABI Http2GetStatusCode(int req_id, int* status_code) {
	PRINT_NAME();

	LOGF("\t req_id      = %d\n", req_id);
	LOGF("\t status_code = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(status_code));

	if (status_code == nullptr) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	*status_code = 0;

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	const int send_result = request->second.send_result;
	if (send_result != 0) {
		return send_result;
	}

	*status_code = request->second.status_code;
	return 0;
}

static int KYTY_SYSV_ABI Http2GetResponseContentLength(int req_id, int* result,
                                                       uint64_t* content_length) {
	PRINT_NAME();

	LOGF("\t req_id         = %d\n", req_id);
	LOGF("\t result         = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(result));
	LOGF("\t content_length = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(content_length));

	if (result == nullptr || content_length == nullptr) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	*result         = 0;
	*content_length = 0;

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	const int send_result = request->second.send_result;
	if (send_result != 0) {
		*result = -1;
		return send_result;
	}

	*content_length = request->second.response_body.size();
	return 0;
}

static int KYTY_SYSV_ABI Http2GetAllResponseHeaders(int req_id, char** header,
                                                    size_t* header_size) {
	PRINT_NAME();

	LOGF("\t req_id      = %d\n", req_id);
	LOGF("\t header      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(header));
	LOGF("\t header_size = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(header_size));

	if (header == nullptr || header_size == nullptr) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	*header      = nullptr;
	*header_size = 0;

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	const int send_result = request->second.send_result;
	if (send_result != 0) {
		return send_result;
	}

	*header      = const_cast<char*>(request->second.response_headers.c_str());
	*header_size = request->second.response_headers.size();
	return 0;
}

static int KYTY_SYSV_ABI Http2ReadData(int req_id, void* data, size_t size) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);
	LOGF("\t data   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
	LOGF("\t size   = %" PRIu64 "\n", size);

	if (data == nullptr && size != 0) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	const int send_result = request->second.send_result;
	if (send_result != 0) {
		return send_result;
	}

	const auto& body = request->second.response_body;
	const auto  remaining =
	    request->second.read_offset < body.size() ? body.size() - request->second.read_offset : 0;
	const auto to_copy = size < remaining ? size : remaining;

	if (to_copy != 0) {
		std::memcpy(data, body.data() + request->second.read_offset, to_copy);
		request->second.read_offset += to_copy;
	}

	return static_cast<int>(to_copy);
}

static int KYTY_SYSV_ABI Http2ReadDataAsync(int req_id, void* data, size_t size,
                                            const Http2TriggerUserEventArgs* kqueue_option,
                                            void* /*option*/) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);
	LOGF("\t data   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
	LOGF("\t size   = %" PRIu64 "\n", size);

	if (data == nullptr && size != 0) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	const int send_result = request->second.send_result;
	if (send_result != 0) {
		request->second.async_result = send_result;
	} else {
		const auto& body      = request->second.response_body;
		const auto  remaining = request->second.read_offset < body.size()
		                            ? body.size() - request->second.read_offset
		                            : 0;
		const auto  to_copy   = size < remaining ? size : remaining;

		if (to_copy != 0) {
			std::memcpy(data, body.data() + request->second.read_offset, to_copy);
			request->second.read_offset += to_copy;
		}

		request->second.async_result = static_cast<int>(to_copy);
	}
	request->second.async_event = 1;
	Http2TriggerUserEvent(kqueue_option);

	return 0;
}

static int KYTY_SYSV_ABI Http2DeleteRequest(int req_id) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	g_http2_requests.erase(request);

	return 0;
}

LIB_DEFINE(InitNet_1_Http2) {
	LIB_FUNC("3JCe3lCbQ8A", LibHttp2::Http2Init);
	LIB_FUNC("YiBUtz-pGkc", LibHttp2::Http2Term);
	LIB_FUNC("otUQuZa-mv0", LibHttp2::Http2GetMemoryPoolStats);
	LIB_FUNC("+wCt7fCijgk", LibHttp2::Http2CreateTemplate);
	LIB_FUNC("pDom5-078DA", LibHttp2::Http2DeleteTemplate);
	LIB_FUNC("mmyOCxQMVYQ", LibHttp2::Http2CreateRequestWithURL);
	LIB_FUNC("nrPfOE8TQu0", LibHttp2::Http2AddRequestHeader);
	LIB_FUNC("FSAFOzi0FpM", LibHttp2::Http2SetRequestContentLength);
	LIB_FUNC("jjFahkBPCYs", LibHttp2::Http2SetAuthEnabled);
	LIB_FUNC("b9AvoIaOuHI", LibHttp2::Http2SetAutoRedirect);
	LIB_FUNC("uRosf8GQbHQ", LibHttp2::Http2SetInflateGZIPEnabled);
	LIB_FUNC("B37SruheQ5Y", LibHttp2::Http2SslDisableOption);
	LIB_FUNC("EWcwMpbr5F8", LibHttp2::Http2SslEnableOption);
	LIB_FUNC("BJgi0CH7al4", LibHttp2::Http2SetRedirectCallback);
	LIB_FUNC("izvHhqgDt44", LibHttp2::Http2SetRecvTimeOut);
	LIB_FUNC("XPtW45xiLHk", LibHttp2::Http2SetSendTimeOut);
	LIB_FUNC("-HIO4VT87v8", LibHttp2::Http2SetConnectTimeOut);
	LIB_FUNC("n8hMLe31OPA", LibHttp2::Http2SetConnectionWaitTimeOut);
	LIB_FUNC("ACjtE27aErY", LibHttp2::Http2SetResolveTimeOut);
	LIB_FUNC("Gcjh+CisAZM", LibHttp2::Http2SetResolveRetry);
	LIB_FUNC("VYMxTcBqSE0", LibHttp2::Http2SetTimeOut);
	LIB_FUNC("YrWX+DhPHQY", LibHttp2::Http2SetSslCallback);
	LIB_FUNC("rbqZig38AT8", LibHttp2::Http2SendRequest);
	LIB_FUNC("A+NVAFu4eCg", LibHttp2::Http2SendRequestAsync);
	LIB_FUNC("o0DBQpFE13o", LibHttp2::Http2GetResponseContentLength);
	LIB_FUNC("9XYJwCf3lEA", LibHttp2::Http2GetStatusCode);
	LIB_FUNC("-rdXUi2XW90", LibHttp2::Http2GetAllResponseHeaders);
	LIB_FUNC("QygCNNmbGss", LibHttp2::Http2ReadData);
	LIB_FUNC("bGN-6zbo7ms", LibHttp2::Http2ReadDataAsync);
	LIB_FUNC("c8D9qIjo8EY", LibHttp2::Http2DeleteRequest);
	LIB_FUNC("MOp-AUhdfi8", LibHttp2::Http2WaitAsync);
}

} // namespace LibHttp2

} // namespace Libs
