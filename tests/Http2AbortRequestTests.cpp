#include "common/abi.h"
#include "loader/symbolDatabase.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace Libs::LibHttp2 {
void InitNet_1_Http2(Loader::SymbolDatabase *symbols);
}

namespace {

// 0x817B1100, the value libSceHttp2 uses for an id it does not know.
constexpr int kHttp2ErrorInvalidId = -2122641152;
// 0x817B1065 and 0x817B1068, the pre-send and timeout results.
constexpr int kHttp2ErrorBeforeSend = -2122641307;
constexpr int kHttp2ErrorTimeout = -2122641304;

int failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "Http2AbortRequestTests:%d: %s\n", __LINE__,        \
                   #condition);                                                \
      failures++;                                                              \
    }                                                                          \
  } while (false)

using Http2Init = int(KYTY_SYSV_ABI *)(int, int, size_t, int);
using Http2CreateTemplate = int(KYTY_SYSV_ABI *)(int, const char *, int, int);
using Http2CreateRequestWithURL = int(KYTY_SYSV_ABI *)(int, const char *,
                                                       const char *, uint64_t);
using Http2AbortRequest = int(KYTY_SYSV_ABI *)(int);
using Http2SendRequest = int(KYTY_SYSV_ABI *)(int, const void *, size_t);
using Http2SendRequestAsync = int(KYTY_SYSV_ABI *)(int, const void *, size_t,
                                                   const void *, void *);
using Http2SetRequestContentLength = int(KYTY_SYSV_ABI *)(int, uint64_t);
using Http2GetResponseContentLength = int(KYTY_SYSV_ABI *)(int, int *, uint64_t *);

template <typename T>
T GetHttp2Function(const Loader::SymbolDatabase &symbols, const char *nid) {
  const auto *record = symbols.FindByNid(nid, Loader::SymbolType::Func);
  CHECK(record != nullptr);
  return record != nullptr ? reinterpret_cast<T>(record->vaddr) : nullptr;
}

// Carmen Sandiego (PPSA25256) and Hot Wheels (PPSA22408) both stop at the logo on
// "Unresolved import stub called: IZ-qjhRqvjk[Http2_v1][Http2_v1.1][Func]".
void TestExportRegistered(const Loader::SymbolDatabase &symbols) {
  CHECK(symbols.FindByNid("IZ-qjhRqvjk", Loader::SymbolType::Func) != nullptr);
}

// An id that was never handed out has to be reported, not accepted.
void TestUnknownRequestRejected(Http2AbortRequest abort) {
  CHECK(abort(0x7fffffff) == kHttp2ErrorInvalidId);
  CHECK(abort(-1) == kHttp2ErrorInvalidId);
}

// Aborting keeps the request alive: DeleteRequest is the call that frees it.
void TestAbortKeepsRequest(Http2AbortRequest abort,
                           Http2SetRequestContentLength set_content_length,
                           int req_id) {
  CHECK(abort(req_id) == 0);
  CHECK(set_content_length(req_id, 0) == 0);
  CHECK(abort(req_id) == 0);
}

// A request aborted before it was sent stays unsent: a later send reports the
// result it already carries instead of a timeout.
void TestAbortBeforeSendPreserved(Http2AbortRequest abort,
                                  Http2SendRequest send,
                                  Http2GetResponseContentLength get_content_length,
                                  int req_id) {
  CHECK(abort(req_id) == 0);
  CHECK(send(req_id, nullptr, 0) == kHttp2ErrorBeforeSend);

  int result = 0;
  uint64_t content_length = 0;
  CHECK(get_content_length(req_id, &result, &content_length) ==
        kHttp2ErrorBeforeSend);
}

// The async path reports the same result and does not overwrite it either.
void TestAsyncAbortBeforeSendPreserved(
    Http2AbortRequest abort, Http2SendRequestAsync send_async,
    Http2GetResponseContentLength get_content_length, int req_id) {
  CHECK(abort(req_id) == 0);
  CHECK(send_async(req_id, nullptr, 0, nullptr, nullptr) == 0);

  int result = 0;
  uint64_t content_length = 0;
  CHECK(get_content_length(req_id, &result, &content_length) ==
        kHttp2ErrorBeforeSend);
}

// An abort after the send step changes nothing: the request already has a result.
void TestAbortAfterSendIgnored(Http2AbortRequest abort, Http2SendRequest send,
                               int req_id) {
  CHECK(send(req_id, nullptr, 0) == kHttp2ErrorTimeout);
  CHECK(abort(req_id) == 0);
  CHECK(send(req_id, nullptr, 0) == kHttp2ErrorTimeout);
}

} // namespace

int main() {
  Loader::SymbolDatabase symbols;
  Libs::LibHttp2::InitNet_1_Http2(&symbols);

  TestExportRegistered(symbols);

  const auto init = GetHttp2Function<Http2Init>(symbols, "3JCe3lCbQ8A");
  const auto create_template =
      GetHttp2Function<Http2CreateTemplate>(symbols, "+wCt7fCijgk");
  const auto create_request =
      GetHttp2Function<Http2CreateRequestWithURL>(symbols, "mmyOCxQMVYQ");
  const auto abort = GetHttp2Function<Http2AbortRequest>(symbols, "IZ-qjhRqvjk");
  const auto send = GetHttp2Function<Http2SendRequest>(symbols, "rbqZig38AT8");
  const auto send_async =
      GetHttp2Function<Http2SendRequestAsync>(symbols, "A+NVAFu4eCg");
  const auto set_content_length =
      GetHttp2Function<Http2SetRequestContentLength>(symbols, "FSAFOzi0FpM");
  const auto get_content_length =
      GetHttp2Function<Http2GetResponseContentLength>(symbols, "o0DBQpFE13o");
  if (init == nullptr || create_template == nullptr ||
      create_request == nullptr || abort == nullptr || send == nullptr ||
      send_async == nullptr || set_content_length == nullptr ||
      get_content_length == nullptr) {
    return 1;
  }

  TestUnknownRequestRejected(abort);

  const int ctx = init(0, 0, 0x10000, 4);
  CHECK(ctx > 0);
  const int tmpl = create_template(ctx, "kyty", 2, 1);
  CHECK(tmpl > 0);
  const int req = create_request(tmpl, "GET", "https://example.com/", 0);
  CHECK(req > 0);

  TestAbortKeepsRequest(abort, set_content_length, req);

  const int unsent = create_request(tmpl, "GET", "https://example.com/", 0);
  CHECK(unsent > 0);
  TestAbortBeforeSendPreserved(abort, send, get_content_length, unsent);

  const int unsent_async = create_request(tmpl, "GET", "https://example.com/", 0);
  CHECK(unsent_async > 0);
  TestAsyncAbortBeforeSendPreserved(abort, send_async, get_content_length,
                                    unsent_async);

  const int sent = create_request(tmpl, "GET", "https://example.com/", 0);
  CHECK(sent > 0);
  TestAbortAfterSendIgnored(abort, send, sent);

  return failures == 0 ? 0 : 1;
}
