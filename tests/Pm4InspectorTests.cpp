#include "graphics/guest_gpu/pm4.h"
#include "graphics/guest_gpu/pm4Inspector.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {

namespace Pm4 = Libs::Graphics::Pm4;

using Libs::Graphics::Pm4::InspectionStatus;
using Libs::Graphics::Pm4::InspectSubmission;
using Libs::Graphics::Pm4::MemoryReader;
using Libs::Graphics::Pm4::QueueRegisterState;
using Libs::Graphics::Pm4::RegisterSpace;
using Libs::Graphics::Pm4::SerializeSubmissionInspection;
using Libs::Graphics::Pm4::SubmissionMetadata;

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "Pm4InspectorTests: failed: %s\n", message);
    std::abort();
  }
}

SubmissionMetadata Metadata(bool reset = false) {
  SubmissionMetadata metadata;
  metadata.capture_id = 17;
  metadata.frame = 3;
  metadata.queue = 0;
  metadata.reset_state = reset;
  return metadata;
}

void TestDirectRegistersAndStatePersistence() {
  QueueRegisterState state;
  const std::vector<uint32_t> first = {
      KYTY_PM4(4, Libs::Graphics::Pm4::IT_SET_CONTEXT_REG, 0),
      0x70000020u,
      0x11223344u,
      0x55667788u,
  };
  auto result = InspectSubmission(Metadata(true), "dcb", 0x1000, first, &state);
  Check(result.buffers.size() == 1 && result.buffers[0].packets.size() == 1,
        "direct register packet was not captured");
  Check(result.buffers[0].packets[0].status == InspectionStatus::Known,
        "direct register packet was not classified as known");
  Check(state.Get(RegisterSpace::Context, 0x20).value_or(0) == 0x11223344u &&
            state.Get(RegisterSpace::Context, 0x21).value_or(0) == 0x55667788u,
        "direct context writes were not normalized or persisted");

  const std::vector<uint32_t> second = {
      KYTY_PM4(3, Libs::Graphics::Pm4::IT_SET_SH_REG, 0), 7, 0xaabbccddu};
  result = InspectSubmission(Metadata(false), "dcb", 0x2000, second, &state);
  Check(result.state_at_submit.context_registers == 2,
        "queue state did not persist between submissions");
  Check(state.Get(RegisterSpace::Shader, 7).value_or(0) == 0xaabbccddu,
        "shader register write was not tracked");

  const std::vector<uint32_t> empty;
  result = InspectSubmission(Metadata(true), "dcb", 0x3000, empty, &state);
  Check(result.state_before_reset.context_registers == 2 &&
            result.state_before_reset.shader_registers == 1,
        "state before reset was not reported");
  Check(result.state_at_submit.context_registers == 0 &&
            result.state_after.shader_registers == 0,
        "submission reset did not clear tracked queue state");
}

void TestIndirectCommandBufferSnapshot() {
  constexpr uint64_t nested_address = 0x00400000u;
  const std::vector<uint32_t> nested = {
      KYTY_PM4(3, Libs::Graphics::Pm4::IT_SET_SH_REG, 0), 4, 0x12345678u};
  const MemoryReader reader = [&nested](uint64_t address, uint32_t size_dw,
                                        std::vector<uint32_t> *words) {
    if (address != nested_address || size_dw != nested.size()) {
      return false;
    }
    *words = nested;
    return true;
  };
  const std::vector<uint32_t> root = {
      KYTY_PM4(4, Libs::Graphics::Pm4::IT_INDIRECT_BUFFER, 0),
      static_cast<uint32_t>(nested_address),
      static_cast<uint32_t>(nested_address >> 32u),
      static_cast<uint32_t>(nested.size()),
  };
  QueueRegisterState state;
  auto result =
      InspectSubmission(Metadata(true), "dcb", 0x1000, root, &state, reader);
  Check(result.buffers.size() == 2,
        "nested indirect command buffer was not copied");
  const auto &reference = result.buffers[0].packets[0].references[0];
  Check(reference.snapshot_status == InspectionStatus::Known &&
            reference.nested_buffer_id.value_or(0) == 1,
        "nested buffer reference was not linked to its snapshot");
  Check(state.Get(RegisterSpace::Shader, 4).value_or(0) == 0x12345678u,
        "nested command buffer did not update tracked state");
}

void TestIndirectRegisterSnapshot() {
  constexpr uint64_t pairs_address = 0x00500000u;
  const std::vector<uint32_t> pairs = {0x70000010u, 0x10101010u, 0x70000011u,
                                       0x20202020u};
  const MemoryReader reader = [&pairs](uint64_t address, uint32_t size_dw,
                                       std::vector<uint32_t> *words) {
    if (address != pairs_address || size_dw != pairs.size()) {
      return false;
    }
    *words = pairs;
    return true;
  };
  const std::vector<uint32_t> root = {
      KYTY_PM4(5, Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT, 0),
      static_cast<uint32_t>(pairs_address),
      static_cast<uint32_t>(pairs_address >> 32u),
      0,
      2,
  };
  QueueRegisterState state;
  const auto result =
      InspectSubmission(Metadata(true), "dcb", 0x1000, root, &state, reader);
  Check(result.buffers[0].packets[0].register_writes.size() == 2,
        "indirect register pairs were not decoded");
  Check(state.Get(RegisterSpace::Context, 0x10).value_or(0) == 0x10101010u &&
            state.Get(RegisterSpace::Context, 0x11).value_or(0) == 0x20202020u,
        "indirect register state was not normalized");
}

void TestMalformedAndUnknownPacketsRemainDiagnostic() {
  QueueRegisterState state;
  const std::vector<uint32_t> truncated = {
      KYTY_PM4(4, Libs::Graphics::Pm4::IT_SET_CONTEXT_REG, 0), 0x20};
  auto result = InspectSubmission(Metadata(), "dcb", 0x1000, truncated, &state);
  Check(result.buffers[0].packets[0].status ==
            InspectionStatus::UnreadableAtCaptureTime,
        "truncated packet was not marked unreadable");

  const std::vector<uint32_t> unknown = {KYTY_PM4(2, 0xfe, 0), 0};
  result = InspectSubmission(Metadata(), "dcb", 0x2000, unknown, &state);
  Check(result.buffers[0].packets[0].status == InspectionStatus::Unknown,
        "unknown Type-3 opcode was misclassified");

  const std::vector<uint32_t> unsupported = {0x00000000u, 0};
  result = InspectSubmission(Metadata(), "dcb", 0x3000, unsupported, &state);
  Check(result.buffers[0].packets[0].status == InspectionStatus::Unsupported,
        "unsupported packet type was misclassified");
}

void TestJsonSchema() {
  QueueRegisterState state;
  const std::vector<uint32_t> commands = {0x80000000u};
  const auto result =
      InspectSubmission(Metadata(true), "dcb", 0x1234, commands, &state);
  const auto json =
      nlohmann::json::parse(SerializeSubmissionInspection(result));
  Check(json.at("schema") == "kyty.pm4.submission.v1",
        "JSON schema identifier is missing");
  Check(json.at("capture").at("queue_kind") == "graphics",
        "JSON queue kind is incorrect");
  Check(json.at("buffers").at(0).at("packets").at(0).at("status") == "Known",
        "JSON packet classification is incorrect");
  Check(json.at("capture_scope").at("resource_registration_map") ==
            "not available",
        "JSON does not disclose the unavailable registration map");
}

void TestCaptureFilesAndQueueIsolation() {
  const auto unique =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto folder = std::filesystem::current_path() /
                      ("kyty_pm4_inspector_" + std::to_string(unique));

  Pm4::SubmissionInspector inspector(folder);
  const std::vector<uint32_t> graphics = {
      KYTY_PM4(3, Libs::Graphics::Pm4::IT_SET_CONTEXT_REG, 0),
      0x70000020u,
      0x11223344u,
  };
  const std::vector<uint32_t> compute = {0x80000000u};
  Check(inspector.CaptureGraphics(4, true, 0x1000, graphics),
        "graphics capture file was not written");
  Check(inspector.CaptureAsyncCompute(4, 0, 0x2000, compute),
        "compute capture file was not written");

  const auto graphics_path = folder / "000000_f00004_q00_dcb.json";
  const auto compute_path = folder / "000001_f00004_q00_acb.json";
  Check(std::filesystem::is_regular_file(graphics_path) &&
            std::filesystem::is_regular_file(compute_path),
        "capture filenames do not identify queue roles");

  std::ifstream input(compute_path);
  const auto json = nlohmann::json::parse(input);
  Check(json.at("tracked_register_state").at("at_submit").at(
            "context_registers") == 0,
        "graphics state leaked into a compute queue with the same numeric ID");
  input.close();

  std::error_code error;
  std::filesystem::remove_all(folder, error);
  Check(!error, "temporary capture directory could not be removed");
}

} // namespace

int main() {
  TestDirectRegistersAndStatePersistence();
  TestIndirectCommandBufferSnapshot();
  TestIndirectRegisterSnapshot();
  TestMalformedAndUnknownPacketsRemainDiagnostic();
  TestJsonSchema();
  TestCaptureFilesAndQueueIsolation();
  return 0;
}
