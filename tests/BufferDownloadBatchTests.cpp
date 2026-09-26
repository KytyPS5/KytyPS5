#include "common/alignment.h"
#include "graphics/host_gpu/renderer/cache/bufferDownloadBatch.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using Libs::Graphics::BufferDownloadCopy;
using Libs::Graphics::BufferDownloadRange;
using Libs::Graphics::SplitBufferDownload;

namespace {

constexpr uint64_t kAlign = 64;

void Check(bool value, const char* text) {
  if (!value) {
    std::fprintf(stderr, "BufferDownloadBatchTests: failed: %s\n", text);
    std::abort();
  }
}

// Prove the split is lossless: every input range must be tiled by consecutive copies that
// start where the previous one ended and cover the whole range. A range that had to be split
// legitimately becomes several copies, so this compares a flattened stream rather than
// matching ranges to copies one for one. Every batch must also fit the staging buffer.
void CheckCoversExactly(const std::vector<BufferDownloadRange>& ranges, uint64_t capacity,
                        const char* what) {
  const auto                 batches = SplitBufferDownload(ranges, capacity, kAlign);
  std::vector<BufferDownloadCopy> flat;
  for (const auto& batch: batches) {
    Check(batch.total_size <= capacity, what);
    for (const auto& copy: batch.copies) {
      Check(copy.dst_offset % kAlign == 0, what);
      flat.push_back(copy);
    }
  }
  for (const auto& range: ranges) {
    const uint64_t begin   = range.src_offset;
    const uint64_t end     = range.src_offset + range.size;
    uint64_t       cursor  = begin;
    uint64_t       covered = 0;
    for (const auto& copy: flat) {
      if (copy.src_offset < begin || copy.src_offset >= end) {
        continue;
      }
      Check(copy.src_offset == cursor, what);
      cursor += copy.size;
      covered += copy.size;
    }
    Check(covered == range.size, what);
  }
}

void CheckEmptyDownload() {
  Check(SplitBufferDownload({}, 1024 * 1024, kAlign).empty(), "an empty download stages nothing");
}

void CheckDownloadThatFitsStaysInOneBatch() {
  const std::vector<BufferDownloadRange> ranges {{0, 4096}, {8192, 512}};
  const auto                             batches = SplitBufferDownload(ranges, 1024 * 1024, kAlign);
  Check(batches.size() == 1, "a download that fits is staged in one batch");
  Check(batches[0].copies.size() == 2, "both ranges are in the single batch");
  // Both sizes are already whole lines, so the total is their plain sum.
  Check(batches[0].total_size == 4608, "the batch total is the padded sum");
  CheckCoversExactly(ranges, 1024 * 1024, "a download that fits is covered exactly");
}

void CheckPackedRangesStayOnSeparateCacheLines() {
  const std::vector<BufferDownloadRange> ranges {{0, 100}, {200, 100}};
  const auto                             batches = SplitBufferDownload(ranges, 1024 * 1024, kAlign);
  Check(batches.size() == 1, "two small ranges share a batch");
  // A 100 byte range is padded up to the next whole line, so the second starts at 128
  // rather than at 100. This is the packing the renderer did before the split existed.
  Check(batches[0].copies[0].dst_offset == 0, "the first range starts at the batch base");
  Check(batches[0].copies[1].dst_offset == 128, "the second range is padded to a line");
  Check(batches[0].total_size == Common::AlignUp(uint64_t {228}, kAlign), "the tail is padded too");
  CheckCoversExactly(ranges, 1024 * 1024, "packed ranges are covered exactly");
}

void CheckDownloadLargerThanCapacityIsSplit() {
  const std::vector<BufferDownloadRange> ranges {{0, 1000}, {1000, 1000}, {2000, 1000}};
  const uint64_t                         capacity = 1024;
  const auto                             batches = SplitBufferDownload(ranges, capacity, kAlign);
  Check(batches.size() > 1, "a download larger than the capacity needs several batches");
  CheckCoversExactly(ranges, capacity, "an oversized download is covered exactly");
}

void CheckSingleRangeLargerThanCapacityIsSplit() {
  // The case that used to abort the process: one contiguous dirty range bigger than the
  // whole staging buffer, which no single reservation can ever satisfy.
  const std::vector<BufferDownloadRange> ranges {{0, 1000}};
  const auto                             batches = SplitBufferDownload(ranges, 256, kAlign);
  Check(batches.size() > 1, "a range bigger than the capacity is split");
  for (const auto& batch: batches) {
    Check(batch.total_size <= 256, "each split piece fits the staging buffer");
    for (const auto& copy: batch.copies) {
      Check(copy.size <= 256, "no single copy exceeds the capacity");
    }
  }
  CheckCoversExactly(ranges, 256, "an oversized single range is covered exactly");
}

void CheckUnusableParametersStageNothing() {
  const std::vector<BufferDownloadRange> ranges {{0, 100}};
  Check(SplitBufferDownload(ranges, 32, kAlign).empty(),
        "a capacity below the alignment cannot stage anything");
  Check(SplitBufferDownload(ranges, 1024, 0).empty(), "a zero alignment stages nothing");
  Check(SplitBufferDownload(ranges, 0, kAlign).empty(), "a zero capacity stages nothing");
}

void CheckTailSmallerThanAlignmentIsNotDropped() {
  const std::vector<BufferDownloadRange> ranges {{0, 65}};
  const auto                             batches = SplitBufferDownload(ranges, 64, kAlign);
  Check(!batches.empty(), "a sub-alignment tail still gets staged");
  CheckCoversExactly(ranges, 64, "a sub-alignment tail is covered exactly");
}

void CheckManySmallRangesPackGreedily() {
  std::vector<BufferDownloadRange> ranges;
  for (uint64_t i = 0; i < 40; i++) {
    ranges.push_back({i * 128, 64});
  }
  const auto batches = SplitBufferDownload(ranges, 1024, kAlign);
  Check(batches.size() > 1, "forty 64-byte ranges do not all fit 1 KiB");
  CheckCoversExactly(ranges, 1024, "many small ranges are covered exactly");
}

} // namespace

int main() {
  CheckEmptyDownload();
  CheckDownloadThatFitsStaysInOneBatch();
  CheckPackedRangesStayOnSeparateCacheLines();
  CheckDownloadLargerThanCapacityIsSplit();
  CheckSingleRangeLargerThanCapacityIsSplit();
  CheckUnusableParametersStageNothing();
  CheckTailSmallerThanAlignmentIsNotDropped();
  CheckManySmallRangesPackGreedily();
  std::printf("BufferDownloadBatchTests: all cases passed\n");
  return 0;
}
