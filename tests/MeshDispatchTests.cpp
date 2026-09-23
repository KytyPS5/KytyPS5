#include "graphics/host_gpu/renderer/meshDispatch.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace {

using Libs::Graphics::MeshDispatchSlice;
using Libs::Graphics::SplitMeshDispatch;

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "MeshDispatchTests: failed: %s\n", message);
    std::abort();
  }
}

// Verifies that the slices are within every host limit, do not overlap, and
// together cover the requested grid exactly once.
void CheckCoverage(const std::vector<MeshDispatchSlice> &slices,
                   uint32_t groups, uint32_t instances, uint32_t max_groups,
                   uint32_t max_instances, uint32_t max_total) {
  std::unordered_set<uint64_t> covered;
  covered.reserve(static_cast<size_t>(groups) * instances);
  for (const auto &slice : slices) {
    Check(slice.group_count != 0u && slice.instance_count != 0u,
          "a slice has an empty dimension");
    Check(slice.group_offset <= groups && slice.instance_offset <= instances &&
              slice.group_count <= groups - slice.group_offset &&
              slice.instance_count <= instances - slice.instance_offset,
          "a slice runs past the requested grid");
    Check(max_groups == 0u || slice.group_count <= max_groups,
          "a slice exceeds the group dimension limit");
    Check(max_instances == 0u || slice.instance_count <= max_instances,
          "a slice exceeds the instance dimension limit");
    Check(max_total == 0u ||
              static_cast<uint64_t>(slice.group_count) * slice.instance_count <=
                  max_total,
          "a slice exceeds the total workgroup limit");
    for (uint32_t group = slice.group_offset;
         group < slice.group_offset + slice.group_count; group++) {
      for (uint32_t instance = slice.instance_offset;
           instance < slice.instance_offset + slice.instance_count;
           instance++) {
        const auto cell = (static_cast<uint64_t>(group) << 32) | instance;
        Check(covered.insert(cell).second, "slices overlap or repeat a cell");
      }
    }
  }
  Check(covered.size() == static_cast<size_t>(groups) * instances,
        "slices do not cover the requested grid exactly once");
}

void TestWithinLimits() {
  const auto slices = SplitMeshDispatch(100, 50, 65535, 65535, 65535);
  Check(slices.size() == 1, "a draw within limits was split");
  Check(slices[0].group_offset == 0u && slices[0].group_count == 100u &&
            slices[0].instance_offset == 0u && slices[0].instance_count == 50u,
        "within-limits slice does not match the draw");
  CheckCoverage(slices, 100, 50, 65535, 65535, 65535);
}

void TestInstanceCountAbovePerDimensionLimit() {
  // Astro's Playroom reports "1x68734" on AMD: one group, 68734 instances,
  // beyond the 65535 per-dimension limit reported by the host.
  const auto slices = SplitMeshDispatch(1, 68734, 65535, 65535, 65535);
  Check(slices.size() == 2u, "oversized instance count did not split");
  Check(slices[0].group_count == 1u && slices[0].instance_count == 65535u,
        "first instance slice is wrong");
  Check(slices[1].instance_offset == 65535u &&
            slices[1].instance_count == 3199u,
        "trailing instance slice is wrong");
  CheckCoverage(slices, 1, 68734, 65535, 65535, 65535);
}

void TestGroupCountAbovePerDimensionLimit() {
  const auto slices = SplitMeshDispatch(200000, 7, 65535, 65535, 65535);
  // Four group chunks (65535, 65535, 65535, 3395). The first three keep the
  // total under the host limit one instance at a time; the last fits all
  // seven instances in a single slice.
  Check(slices.size() == 22u,
        "oversized group count produced the wrong slice count");
  CheckCoverage(slices, 200000, 7, 65535, 65535, 65535);
}

void TestTotalCountCapsInstanceChunks() {
  const auto slices = SplitMeshDispatch(100, 1000, 65535, 65535, 65535);
  // The total limit of 65535 allows at most 655 instances per 100-group slice.
  Check(slices.size() == 2u,
        "total-limit capping produced the wrong slice count");
  Check(slices[0].instance_count == 655u && slices[1].instance_count == 345u,
        "total-limit capping split instances incorrectly");
  CheckCoverage(slices, 100, 1000, 65535, 65535, 65535);
}

void TestDegenerateCounts() {
  Check(SplitMeshDispatch(0, 10, 65535, 65535, 65535).empty(),
        "zero groups produced a non-empty dispatch");
  Check(SplitMeshDispatch(10, 0, 65535, 65535, 65535).empty(),
        "zero instances produced a non-empty dispatch");
  const auto single = SplitMeshDispatch(1, 1, 65535, 65535, 65535);
  Check(single.size() == 1u && single[0].group_count == 1u &&
            single[0].instance_count == 1u,
        "single-workgroup draw was not preserved");
}

void TestZeroAndTinyLimitsClampToOne() {
  // Hosts must report limits of at least one, but the splitting accepts zero
  // and treats it as one instead of dividing by zero.
  const auto one_group = SplitMeshDispatch(4, 1, 0, 0, 0);
  Check(one_group.size() == 4u, "zero limits did not clamp to one");
  CheckCoverage(one_group, 4, 1, 0, 0, 0);
  const auto tiny_limits = SplitMeshDispatch(3, 300, 2, 1, 2);
  Check(tiny_limits.size() == 600u,
        "tiny limits produced the wrong slice count");
  CheckCoverage(tiny_limits, 3, 300, 2, 1, 2);
}

} // namespace

int main() {
  TestWithinLimits();
  TestInstanceCountAbovePerDimensionLimit();
  TestGroupCountAbovePerDimensionLimit();
  TestTotalCountCapsInstanceChunks();
  TestDegenerateCounts();
  TestZeroAndTinyLimitsClampToOne();
  std::puts("MeshDispatchTests: all cases passed");
  return 0;
}
