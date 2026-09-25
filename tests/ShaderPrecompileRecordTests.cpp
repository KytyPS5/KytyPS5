#include "graphics/host_gpu/renderer/pipeline/shaderPrecompile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <vector>

namespace {

void Check(bool value, const char* text) {
    if (!value) {
        std::printf("FAILED: %s\n", text);
        std::fflush(stdout);
        std::abort();
    }
}

namespace Precompile = Libs::Graphics::ShaderPrecompile;

using Libs::Graphics::ShaderComputeInputInfo;
using Libs::Graphics::ShaderParams;
using Libs::Graphics::ShaderType;
using Libs::Graphics::ShaderRecompiler::CompileOptions;
using Libs::Graphics::ShaderRecompiler::IR::ResourceSpecialization;

std::filesystem::path TempDir() {
    return std::filesystem::temp_directory_path() / "kyty_shader_precompile_tests";
}

constexpr uint64_t DefaultHash = 0xdeadbeefcafef00dull;

struct Sample {
    uint64_t               hash = DefaultHash;
    std::vector<uint32_t>  code {0x11111111u, 0x22222222u, 0x33333333u};
    std::vector<uint32_t>  user_data {1u, 2u, 3u, 4u};
    ResourceSpecialization specialization;
    ShaderComputeInputInfo info {};

    explicit Sample(uint64_t shader_hash = DefaultHash) : hash(shader_hash) {
        ResourceSpecialization::Buffer buffer {};
        buffer.packed_stride = 48;
        specialization.buffers.push_back(buffer);
        ResourceSpecialization::Image image {};
        image.mip_count = 7;
        specialization.images.push_back(image);

        info.threads_num[0]     = 64;
        info.lds_size_dwords    = 256;
        info.host_subgroup_size = 32;
        info.wave_size          = 32;
        info.workgroup_register = 5;
        info.tg_size_en         = true;
    }

    ShaderParams Params() const {
        ShaderParams params {};
        params.code = std::span<const uint32_t>(code.data(), code.size());
        std::ranges::copy(user_data, params.user_data.begin());
        params.user_data_count = static_cast<uint32_t>(user_data.size());
        params.hash            = hash;
        return params;
    }

    CompileOptions Options() const {
        CompileOptions options;
        options.stage          = ShaderType::Compute;
        options.shader_hash    = hash;
        options.user_data      = user_data;
        options.user_data_base = 8;
        options.wave_size      = 32;
        return options;
    }
};

// Assumes the set is already open, the way a running compile does.
void RecordOne(uint64_t hash) {
    const Sample sample {hash};
    Precompile::Record(sample.Params(), sample.Options(), sample.specialization, 12u, sample.info);
}

void WriteOne(const std::filesystem::path& path, const char* key) {
    Precompile::Open(path, key, false);
    RecordOne(DefaultHash);
    Precompile::Close();
}

void TestComputeRecordRoundTrips() {
    const auto path = TempDir() / "round_trip.shaders";
    std::filesystem::remove_all(TempDir());
    WriteOne(path, "test-key");

    const auto records = Precompile::Load(path, "test-key");
    Check(records.size() == 1, "one record written, one read back");

    const auto& record = records.front();
    const Sample sample;
    Check(record.stage == ShaderType::Compute, "stage survives");
    Check(record.hash == 0xdeadbeefcafef00dull, "hash survives");
    Check(record.push_data_start_dword == 12u, "push data offset survives");
    Check(record.user_data_base == 8u, "user data base survives");
    Check(record.wave_size == 32u, "wave size survives");
    Check(record.code == sample.code, "shader code survives");
    Check(record.back_code.empty(), "absent back code stays absent");
    Check(record.user_data == sample.user_data, "user data survives");
    Check(record.specialization.buffers.size() == 1, "buffer specialization count survives");
    Check(record.specialization.buffers.front().packed_stride == 48, "buffer field survives");
    Check(record.specialization.images.size() == 1, "image specialization count survives");
    Check(record.specialization.images.front().mip_count == 7, "image field survives");

    const auto* info = std::get_if<ShaderComputeInputInfo>(&record.info);
    Check(info != nullptr, "compute records carry compute input info");
    Check(info->threads_num[0] == 64, "workgroup size survives");
    Check(info->lds_size_dwords == 256, "lds size survives");
    Check(info->workgroup_register == 5, "workgroup register survives");
    Check(info->tg_size_en, "tg_size_en survives");
}

// The vertex key includes the launch wave size, which is 32 or 64 per title, so a record that
// dropped it would come back at the default of 64 and never match the guest's own lookup on a
// wave32 title.
void TestVertexRecordRoundTrips() {
    const auto path = TempDir() / "vertex.shaders";
    const Sample sample;
    auto         options = sample.Options();
    options.stage        = ShaderType::Vertex;
    Libs::Graphics::ShaderVertexInputInfo info {};
    info.wave_size        = 32;
    info.fetch_embedded   = true;
    Precompile::Open(path, "test-key", false);
    Precompile::Record(sample.Params(), options, sample.specialization, 12u, info);
    Precompile::Close();

    const auto records = Precompile::Load(path, "test-key");
    Check(records.size() == 1, "one vertex record written, one read back");
    const auto* vertex = std::get_if<Libs::Graphics::ShaderVertexInputInfo>(&records[0].info);
    Check(vertex != nullptr, "vertex records carry vertex input info");
    Check(vertex->wave_size == 32, "vertex wave size survives");
    Check(vertex->fetch_embedded, "embedded fetch survives");
}

void TestDifferentKeyIsRejected() {
    const auto path = TempDir() / "wrong_key.shaders";
    WriteOne(path, "written-by-this-build");
    Check(Precompile::Load(path, "a-different-build").empty(),
          "a set written by another recompiler build is not replayed");
}

// The writer appends and flushes per record so a crash mid-run cannot destroy earlier ones; a
// half-written tail must be dropped without losing what came before it.
void TestTruncatedTailKeepsEarlierRecords() {
    const auto path = TempDir() / "truncated.shaders";
    WriteOne(path, "test-key");
    const auto full = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, full - 8);
    Check(Precompile::Load(path, "test-key").empty(),
          "a torn single record is dropped rather than misread");
}

// A warm boot replays the loaded set straight into the program cache, so the guest lookups that
// follow are hits and those permutations are never recorded again. Appending is the only thing
// keeping them, so the set has to come back whole from a load, write, load cycle.
void TestWarmBootKeepsTheReplayedSet() {
    const auto path = TempDir() / "warm_boot.shaders";
    WriteOne(path, "test-key");

    const auto cold = Precompile::Load(path, "test-key");
    Check(cold.size() == 1, "the first run's record loads");

    // The next launch: replay what loaded, record only the one shader it newly needs.
    Precompile::Open(path, "test-key", !cold.empty());
    RecordOne(0x2222u);
    Precompile::Close();

    const auto warm = Precompile::Load(path, "test-key");
    Check(warm.size() == 2, "the replayed record is still in the set beside the new one");
    Check(warm[0].hash == DefaultHash, "the replayed record keeps its identity");
    Check(warm[1].hash == 0x2222u, "the newly compiled record follows it");
}

// Dropping a torn tail on load is not enough on its own: the run that follows opens the same file
// in append mode, so the torn bytes have to be gone. The torn record's length prefix survives the
// tear, so once a later record sits behind it that length looks satisfiable again and the next
// load splices the two into one bogus record rather than stopping.
void TestTornTailDoesNotSwallowLaterRecords() {
    const auto path = TempDir() / "torn_then_append.shaders";
    Precompile::Open(path, "test-key", false);
    RecordOne(0x1111u);
    RecordOne(0x2222u);
    Precompile::Close();
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 8);

    const auto salvaged = Precompile::Load(path, "test-key");
    Check(salvaged.size() == 1, "the intact record ahead of the torn one is kept");

    Precompile::Open(path, "test-key", !salvaged.empty());
    RecordOne(0x3333u);
    Precompile::Close();

    const auto reloaded = Precompile::Load(path, "test-key");
    Check(reloaded.size() == 2, "a record written after a torn tail is still readable");
    Check(reloaded[0].hash == 0x1111u, "the salvaged record is still first");
    Check(reloaded[1].hash == 0x3333u,
          "the appended record reads back as itself, not spliced onto the torn remains");
}

} // namespace

int main() {
    TestComputeRecordRoundTrips();
    TestVertexRecordRoundTrips();
    TestDifferentKeyIsRejected();
    TestTruncatedTailKeepsEarlierRecords();
    TestWarmBootKeepsTheReplayedSet();
    TestTornTailDoesNotSwallowLaterRecords();
    std::filesystem::remove_all(TempDir());
    std::puts("ShaderPrecompileRecordTests: all cases passed");
    return 0;
}

// Keep this standalone target self-contained, as the other focused tests do.
#include "graphics/host_gpu/renderer/pipeline/shaderPrecompile.cpp"
