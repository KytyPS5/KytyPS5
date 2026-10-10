// AJM instance ids must stay unique among live instances after the index
// counter wraps.
#include "libs/audio.h"
#include "libs/errno.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace {

void Check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "AjmInstanceTests: %s\n", message);
    std::abort();
  }
}

constexpr uint32_t CODEC_AT9 = 1;
constexpr uint32_t INDEX_COUNT = 0x4000;

void TestWrappedIdsSkipLiveInstances() {
  using namespace Libs::Audio::Ajm;
  uint32_t context = 0;
  Check(AjmInitialize(0, &context) == OK, "initialize failed");

  // A long-lived voice, then enough short-lived ones to wrap the index counter
  // twice.
  uint32_t live = 0;
  Check(AjmInstanceCreate(context, CODEC_AT9, 0, &live) == OK,
        "create live failed");
  for (uint32_t i = 0; i < 2 * INDEX_COUNT; i++) {
    uint32_t transient = 0;
    Check(AjmInstanceCreate(context, CODEC_AT9, 0, &transient) == OK,
          "create failed");
    Check(transient != live, "wrapped instance id reused a live instance");
    Check(AjmInstanceDestroy(context, transient) == OK, "destroy failed");
  }

  // Ids handed out while many instances are live are all distinct.
  std::unordered_set<uint32_t> ids{live};
  for (uint32_t i = 0; i < 64; i++) {
    uint32_t id = 0;
    Check(AjmInstanceCreate(context, CODEC_AT9, 0, &id) == OK,
          "create batch failed");
    Check(ids.insert(id).second, "duplicate live instance id");
  }
  for (const auto id : ids) {
    Check(AjmInstanceDestroy(context, id) == OK, "destroy batch failed");
  }
  Check(AjmFinalize(context) == OK, "finalize failed");
}

} // namespace

int main() {
  TestWrappedIdsSkipLiveInstances();
  std::puts("AjmInstanceTests: all cases passed");
  return 0;
}
