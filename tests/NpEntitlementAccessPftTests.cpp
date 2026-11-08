// Tests for the trial state that NpEntitlementAccess (NID eDXKe9FndlE) gives a
// game.

#include "common/abi.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <iostream>

namespace Libs::LibNpEntitlementAccessPft {
void InitNet_1_NpEntitlementAccessPft(Loader::SymbolDatabase *s);
} // namespace Libs::LibNpEntitlementAccessPft

namespace {

using TrialState = int(KYTY_SYSV_ABI *)(uint32_t *);

constexpr int NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER = -2122514430; // 0x817D0002

int g_failures = 0;

void Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    g_failures++;
  }
}

TrialState FindTrialState(const Loader::SymbolDatabase &symbols) {
  const auto *record =
      symbols.FindByNid("eDXKe9FndlE", Loader::SymbolType::Func);
  Check(record != nullptr, "the trial state function is registered");
  return record != nullptr ? reinterpret_cast<TrialState>(record->vaddr)
                           : nullptr;
}

// A game reads 0 as "not a trial", 1 as "trial", and anything else as a failed
// check, after which it falls back to its trial version.
void TestTheFullGameIsNotATrial(TrialState trial_state) {
  uint32_t state = 0xffffffffu;
  Check(trial_state(&state) == 0, "the call succeeds");
  Check(state == 0, "the state is 0: not a trial");
}

void TestANullStateIsAParameterError(TrialState trial_state) {
  Check(trial_state(nullptr) == NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER,
        "a null state is a parameter error");
}

} // namespace

int main() {
  Loader::SymbolDatabase symbols;
  Libs::LibNpEntitlementAccessPft::InitNet_1_NpEntitlementAccessPft(&symbols);
  const auto trial_state = FindTrialState(symbols);
  if (trial_state != nullptr) {
    TestTheFullGameIsNotATrial(trial_state);
    TestANullStateIsAParameterError(trial_state);
  }
  if (g_failures != 0) {
    std::cerr << g_failures << " NP entitlement trial state check(s) failed\n";
    return 1;
  }
  std::cout << "NP entitlement trial state tests passed\n";
  return 0;
}
