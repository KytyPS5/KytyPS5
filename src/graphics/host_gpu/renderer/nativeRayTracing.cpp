#include "graphics/host_gpu/renderer/nativeRayTracing.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "gpu_tiler_shaders/hle_blas_refit_spv.h"
#include "gpu_tiler_shaders/hle_tlas_spv.h"
#include "gpu_tiler_shaders/rt_blas_walk_spv.h"
#include "gpu_tiler_shaders/rt_tlas_walk_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

namespace Libs::Graphics {

namespace {

// QueueCapacity and MissingCapacity are kQueueCapacity and kMissingCapacity of rt_tlas_walk.comp.
constexpr uint32_t QueueCapacity    = 1u << 16u;
constexpr uint32_t InstanceCapacity = 1u << 16u;
constexpr uint32_t TableEntries     = 1u << 17u;
constexpr uint32_t SlotCapacity     = 1u << 15u;
constexpr uint32_t MissingCapacity  = 256;
constexpr uint32_t MaxJobs          = 512;
// The host table of BLASes whose builds the host saw (kHostTableEntries in rt_tlas_walk.comp),
// after the walk's table, and the geometries of the layouts of one preparation's builds.
constexpr uint32_t HostTableEntries = 1u << 14u;
constexpr uint32_t HostTableProbes  = 32;
constexpr uint32_t LayoutCapacity   = 1u << 16u;
// The root of every BLAS of a TLAS built by hle_tlas.comp (kBlasRoot).
constexpr uint32_t GuestBlasRoot    = 0x25;
constexpr uint32_t ChangedCapacity  = 1024;
constexpr uint32_t PoolCapacity     = 1u << 24u;
// The largest queue one walk takes from the pool (kMaxQueue in rt_blas_walk.comp).
constexpr uint64_t PoolQueueMax     = uint64_t {1} << 20u;
constexpr uint64_t BuildScratchSize = uint64_t {64} << 20u;
constexpr uint32_t MaxRetries       = 8;
// Preparations between the reports of a BLAS that failed MaxRetries times (about 3 seconds).
constexpr uint64_t LateRetryPrepares = 64;
// Preparations a changed slot stays rebuilt before each use (about 30 frames).
constexpr uint32_t DynamicPrepares  = 120;
// A slot no TLAS referenced for this many frames is evicted (its BLAS left the scene or its
// memory was unmapped); one that comes back is counted and built again as a new BLAS, so objects
// the camera turns away from for a while keep their shadows when it turns back.
constexpr uint64_t EvictFrames      = 1800;
// Eviction scans run every this many preparations and evict at most MaxEvictions slots each
// (every one costs GPU updates in the stream).
constexpr uint64_t EvictScanPrepares = 8;
constexpr uint32_t MaxEvictions     = 256;
// The most triangles one slot holds (both geometries) and bytes its memory takes. The whole scene
// of GTA V holds under two million triangles in about 200 MiB; a BLAS counted bigger was read
// while the guest was rewriting it (a count of tens of millions once reserved 8 GB of VRAM).
constexpr uint64_t MaxSlotTriangles = uint64_t {1} << 21u;
constexpr uint64_t MaxSlotBytes     = uint64_t {256} << 20u;
// The slots used least recently are evicted when the arena passes 7/8 of its budget, down to
// 3/4 of it. A slot used in the last LruMinAge frames is never evicted that way.
constexpr uint64_t LruMinAge        = 60;
// Moving slots out of a nearly empty chunk: when the arena reserves more than DefragMinWaste
// (and half of what it uses) over what it uses, MaxMoves slots move per eviction scan; a pass
// that gives up, or takes longer than DefragMaxPrepares preparations, rests DefragRest ones.
constexpr uint64_t DefragMinWaste   = uint64_t {192} << 20u;
constexpr uint32_t MaxMoves         = 8;
constexpr uint64_t DefragMaxPrepares = 2048;
constexpr uint64_t DefragRest       = 1024;
// The default budget is this share of the largest device local heap, within these limits.
constexpr uint64_t BudgetMin        = uint64_t {512} << 20u;
constexpr uint64_t BudgetMax        = uint64_t {4096} << 20u;
// BLASes whose failed reports are remembered at most; the table entries of the exhausted ones
// are released when the map is full.
constexpr size_t   MaxRetryKeys     = 4096;
// A table entry whose BLAS was evicted (or forgotten): lookups probe past it and new keys reuse
// it (see FindSlot in rt_tlas_walk.comp). Real keys are multiples of 8.
constexpr uint64_t TableTombstone   = 1;

constexpr uint32_t StateSize      = 96;
constexpr uint32_t MissingStride  = 48;
constexpr uint32_t JobStride      = 80;
constexpr uint32_t SlotStride     = 64;
constexpr uint32_t TableStride    = 16;
constexpr uint32_t ContextStride  = 64;
constexpr uint32_t InstanceStride = 64;

constexpr uint32_t ModeCount  = 0;
constexpr uint32_t ModeBuild  = 1;
constexpr uint32_t ModeVerify = 2;
constexpr uint32_t ModeGather = 3;
// Gathered builds of a slot before it walks the guest BVH again.
constexpr uint32_t MaxGathers = 30;
// The guest memory of a BLAS sce::Psr decompresses holds less than one triangle per this many
// bytes (GTA V: 54 bytes or more per triangle, header and boxes included).
constexpr uint64_t DecompressBytesPerTriangle = 32;

// RtState (rt_common.inc).
struct RtState {
	uint64_t header_va;
	uint64_t instance_base;
	uint32_t node_limit;
	uint32_t instance_count;
	uint32_t box_count;
	uint32_t overflow;
	uint32_t missing_count;
	uint32_t dispatch[3];
	uint32_t unknown_blas;
	uint32_t inactive;
	uint32_t max_depth;
	uint32_t verify_count;
	uint32_t verify[3];
	uint32_t changed_count;
	uint32_t pool_cursor;
	uint32_t pool_overflow;
	uint32_t pad[2];
};
static_assert(sizeof(RtState) == StateSize);
constexpr uint32_t StateDispatch = offsetof(RtState, dispatch);
constexpr uint32_t StateVerify   = offsetof(RtState, verify);
// RtState::overflow: part of the TLAS is in memory the host has not cached (kUnmapped).
constexpr uint32_t StateUnmapped = 4u;
// RtState::overflow: the walk's box node queue (1) or the host TLAS (2) was full.
constexpr uint32_t StateFull = 3u;

// The readback area: the state, the missing BLASes, the build jobs with their results and the
// slots whose BLAS changed.
constexpr uint64_t ReadbackMissing = StateSize;
constexpr uint64_t ReadbackJobs    = ReadbackMissing + uint64_t {MissingCapacity} * MissingStride;
constexpr uint64_t ReadbackChanged = ReadbackJobs + uint64_t {MaxJobs} * JobStride;
// The slots the preparation's TLAS referenced (the verify list: each once, SlotCapacity at most).
constexpr uint64_t ReadbackUsed    = ReadbackChanged + uint64_t {ChangedCapacity} * 4;
constexpr uint64_t ReadbackSize    = ReadbackUsed + uint64_t {SlotCapacity} * 4;

// TlasPush::header_offset when the walk finds the header address in its state (kHeaderInState in
// rt_tlas_walk.comp).
constexpr uint32_t HeaderInState = 0xffffffffu;

// The descriptors of the walks (RT_RECORD_PAGES in rt_common.inc): the BDA page table, then the
// fault bitmap of the fault buffer.
constexpr uint32_t WalkBindings = 2;

struct TlasPush {
	uint64_t user_ptr_va;
	uint64_t state;
	uint64_t queue;
	uint64_t instance_nodes;
	uint64_t instances;
	uint64_t table;
	uint64_t slots;
	uint64_t missing;
	uint64_t context;
	uint64_t tlas_address;
	uint64_t verify_list;
	uint32_t header_offset;
	uint32_t root;
	uint32_t instance_capacity;
	uint32_t table_mask;
	uint32_t prepare_id;
	uint32_t verify;
	// The layout of the instance records (Decoder::RayTracingLayout::Pack()).
	std::array<uint32_t, 4> layout;
};
static_assert(sizeof(TlasPush) == 128 && offsetof(TlasPush, layout) % 16 == 0);

struct HleTlasPush {
	uint64_t input_va;
	uint64_t header_va;
	uint64_t scratch;
	uint32_t count;
	uint32_t stride;
	uint32_t format;
	uint32_t padding;
};
static_assert(sizeof(HleTlasPush) == 40);

// The instance and node bounds of hle_tlas.comp (32 bytes each).
constexpr uint64_t HleTlasScratchSize = uint64_t {4096 + 1365} * 32;

struct HleRefitPush {
	uint64_t jobs;
	uint64_t pool;
	uint64_t state;
	uint32_t mode;
	uint32_t job_count;
	uint32_t pool_capacity;
	uint32_t layout;
};

constexpr uint32_t HleRefitGeometryStride = 96;
constexpr uint32_t HleRefitBlasStride     = 16;
constexpr uint64_t HleRefitJobsSize       = uint64_t {4} << 20u;
constexpr uint32_t HleRefitPoolCapacity   = 1u << 22u;
constexpr uint32_t HleRefitStateSize      = 16;

struct BlasPush {
	uint64_t jobs;
	uint64_t pool;
	uint64_t slots;
	uint64_t state;
	uint64_t changed;
	uint32_t mode;
	uint32_t pool_capacity;
	uint32_t job_count;
	uint32_t changed_capacity;
	uint32_t prepare_id;
	// BlasNodeCountAt80: the BLAS headers hold their node count at +80 (see RayTracingLayout).
	uint32_t flags;
};

constexpr uint32_t BlasNodeCountAt80 = 1;

// After the entry table of a slot (16 bytes per triangle), 32-byte aligned: the alpha records of
// its alpha tested triangles, 32 bytes each, that the RT passes fill (see AlphaTest() of the ray
// query emitter) and a walk build clears.
constexpr uint64_t AlphaRecordsOffset(uint32_t capacity0, uint32_t capacity1) {
	return ((uint64_t {capacity0} + capacity1) * 16 + 31u) & ~uint64_t {31};
}

struct Job {
	uint64_t blas_va;
	uint32_t root;
	uint32_t slot;
	uint64_t vertices0;
	uint64_t vertices1;
	uint64_t entries;
	uint32_t capacity0;
	uint32_t capacity1;
	uint32_t result;
	uint32_t header_layout;
	uint64_t layout;
	uint32_t layout_count;
	uint32_t mismatches;
	uint32_t leaves;
	uint32_t pad2;
};

// Job::result: the build did not match its layout, or the memory did not hold the BLAS the layout
// describes any more.
constexpr uint32_t JobLayoutMismatch = 16;
constexpr uint32_t JobLayoutStale    = 32;

// The host table hashes keys like FindSlot in rt_tlas_walk.comp.
uint32_t TableHash(uint64_t key) {
	return static_cast<uint32_t>(key >> 6u) * 0x9e3779b1u ^ static_cast<uint32_t>(key >> 32u) * 0x85ebca6bu;
}
static_assert(sizeof(Job) == JobStride);

struct SlotInfo {
	uint64_t as_address;
	uint64_t entry_table;
	uint32_t capacity0;
	uint32_t capacity1;
	uint32_t count0;
	uint32_t count1;
	uint32_t signature;
	uint32_t state;
	uint64_t blas_va;
	uint32_t root;
	uint32_t stamp;
	uint32_t leaf_count;
	uint32_t rebase;
};
static_assert(sizeof(SlotInfo) == SlotStride);

struct MissingEntry {
	uint64_t blas_va;
	uint32_t root;
	uint32_t table_index;
	uint32_t leaf_count;
	uint32_t box_count;
	uint32_t opaque_count;
	uint32_t alpha_count;
	uint32_t node_limit;
	uint32_t flags;
	uint32_t pad[2];
};
static_assert(sizeof(MissingEntry) == MissingStride);

constexpr vk::BufferUsageFlags DataFlags =
    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress |
    vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc |
    vk::BufferUsageFlagBits::eIndirectBuffer;
constexpr vk::BufferUsageFlags AsFlags =
    DataFlags | vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
    vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;

constexpr vk::PipelineStageFlags2 AsBuildStage =
    vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;

void MemoryBarrier(const CommandRecorder& command, vk::PipelineStageFlags2 src_stages,
                   vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stages,
                   vk::AccessFlags2 dst_access) {
	vk::MemoryBarrier2 barrier {};
	barrier.srcStageMask  = src_stages;
	barrier.srcAccessMask = src_access;
	barrier.dstStageMask  = dst_stages;
	barrier.dstAccessMask = dst_access;
	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
}

// vkCmdUpdateBuffer of a small array (at most 64 KiB, dword multiples).
void UpdateBuffer(const CommandRecorder& command, vk::Buffer buffer, uint64_t offset,
                  const void* data, uint32_t size) {
	EXIT_IF(size == 0 || size > 65536 || (size & 3u) != 0 || (offset & 3u) != 0);
	const auto* copy = command.CopyArray(static_cast<const uint8_t*>(data), size);
	command.Custom([buffer, offset, copy, size](vk::CommandBuffer c) {
		c.updateBuffer(buffer, offset, size, copy);
	});
}

// BLASes the guest changed lately are rebuilt before every use (see Prepare()): a fast build.
// Once they stop changing they are built once more for tracing.
constexpr vk::BuildAccelerationStructureFlagsKHR BlasFlags =
    vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
constexpr vk::BuildAccelerationStructureFlagsKHR FastBlasFlags =
    vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
// BLASes the guest refits every frame are gathered again from the refit vertices with the same
// triangles (see RecordBuilds()): their builds allow updates, and a gathered build updates the
// last build in place instead of building it again. A walk now and then builds them anew.
constexpr vk::BuildAccelerationStructureFlagsKHR UpdatableBlasFlags =
    BlasFlags | vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate;
// A refit BLAS the guest changes again within the frame (dynamic) is gathered and updated before
// each use: fast builds that allow updates.
constexpr vk::BuildAccelerationStructureFlagsKHR FastUpdatableBlasFlags =
    FastBlasFlags | vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate;

// Non-indexed triangles (3 floats per vertex): geometry 0 opaque, geometry 1 alpha tested (the
// guest alpha test runs once per candidate).
vk::AccelerationStructureGeometryKHR TriangleGeometry(uint64_t address, uint32_t capacity,
                                                      bool opaque) {
	vk::AccelerationStructureGeometryKHR geometry {};
	geometry.geometryType = vk::GeometryTypeKHR::eTriangles;
	geometry.flags        = opaque ? vk::GeometryFlagBitsKHR::eOpaque
	                               : vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation;
	auto& triangles                    = geometry.geometry.triangles;
	triangles.sType                    = vk::StructureType::eAccelerationStructureGeometryTrianglesDataKHR;
	triangles.vertexFormat             = vk::Format::eR32G32B32Sfloat;
	triangles.vertexData.deviceAddress = address;
	triangles.vertexStride             = 12;
	triangles.maxVertex                = capacity * 3 - 1;
	triangles.indexType                = vk::IndexType::eNoneKHR;
	return geometry;
}

} // namespace

namespace {
// Set when the device is created, before any shader compiles.
std::atomic<bool> g_device_support {false};

// A counter of the "[RT]" summaries. One thread at a time adds to it (the execution thread; the
// readbacks under m_mutex), so an increment is a load and a store, without a locked instruction.
struct RtCounter {
	std::atomic<uint64_t> total {0};
	uint64_t              reported = 0; // The summary's own.

	void Add() noexcept { total.store(total.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }
	uint64_t TakeDelta() noexcept {
		const auto now   = total.load(std::memory_order_relaxed);
		const auto delta = now - reported;
		reported         = now;
		return delta;
	}
};

constexpr int64_t RtSummaryNs = 10000000000;

struct RtStats {
	// Dispatches whose guest TLAS the host walked (Prepare()).
	RtCounter native;
	// NativeRayTracing::GuestDispatch, with the last shader of each.
	std::array<RtCounter, static_cast<size_t>(NativeRayTracing::GuestDispatch::Count)>             guest;
	std::array<std::atomic<uint64_t>, static_cast<size_t>(NativeRayTracing::GuestDispatch::Count)> last_guest {};
	uint32_t                                                                                        guest_calls = 0;
	// Walks that published no context (rt_tlas_walk.comp): with a null TLAS header (read through
	// the user data pointer, so the program without the patch could not be chosen) or with a header
	// that is not that of a TLAS. Their dispatches take the guest traversal. The last program.
	RtCounter             walks_null_header;
	RtCounter             walks_bad_header;
	std::atomic<uint64_t> last_unpublished {0};
	// Walks that found part of the TLAS in memory the host had not cached yet: they traced what
	// they read, and the host caches the rest for the next walks. The last program.
	RtCounter             walks_uncached;
	std::atomic<uint64_t> last_uncached {0};
	// Walks that found more box nodes or instances than the host structures hold (the rest is
	// left out). The last program.
	RtCounter             walks_overflow;
	std::atomic<uint64_t> last_overflow {0};
	// --rt-stats: rays sent back to the guest traversal.
	std::atomic<uint64_t>                guest_rays {0};
	uint64_t                             guest_rays_reported = 0;
	std::atomic<uint64_t>                last_guest_rays {0};
	ShaderRecompiler::RayTracingPrograms programs_reported {};
	std::atomic<int64_t>                 next_summary_ns {0};
};

RtStats g_rt_stats;

std::string RtShader(uint64_t shader_hash) {
	return shader_hash == 0 ? std::string("unknown") : fmt::format("0x{:016x}", shader_hash);
}

// Every 10 s (whoever calls first after that), when something did not trace natively: why.
void SummarizeRayTracing() {
	auto&      stats = g_rt_stats;
	const auto now   = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
	auto next = stats.next_summary_ns.load(std::memory_order_relaxed);
	if (now < next || !stats.next_summary_ns.compare_exchange_strong(next, now + RtSummaryNs)) {
		return;
	}
	using Kind           = NativeRayTracing::GuestDispatch;
	const auto native    = stats.native.TakeDelta();
	const auto guest     = [&](Kind kind) { return stats.guest[static_cast<size_t>(kind)].TakeDelta(); };
	const auto last      = [&](Kind kind) {
		return RtShader(stats.last_guest[static_cast<size_t>(kind)].load(std::memory_order_relaxed));
	};
	const auto null_tlas  = guest(Kind::NullTlas);
	const auto unpatched  = guest(Kind::Unpatched);
	const auto null_header = stats.walks_null_header.TakeDelta();
	const auto bad_header  = stats.walks_bad_header.TakeDelta();
	const auto uncached    = stats.walks_uncached.TakeDelta();
	const auto overflow    = stats.walks_overflow.TakeDelta();
	const auto unpublished = null_header + bad_header;
	const auto rays_total = stats.guest_rays.load(std::memory_order_relaxed);
	const auto rays       = rays_total - stats.guest_rays_reported;
	stats.guest_rays_reported  = rays_total;
	const auto programs        = ShaderRecompiler::GetRayTracingPrograms();
	const auto new_unpatched   = programs.unpatched - stats.programs_reported.unpatched;
	const auto new_guest_sites = programs.guest_sites - stats.programs_reported.guest_sites;
	stats.programs_reported    = programs;
	// The first call starts the period; dispatches without a TLAS alone trace nothing.
	if (next == 0 || unpatched + unpublished + uncached + overflow + rays + new_unpatched + new_guest_sites == 0) {
		return;
	}
	const auto log = [](const std::string& line) { ::Log::WriteToConsoleAndLog(line + "\n"); };
	log(fmt::format("[RT] summary of the last 10 s: {} dispatches traced through host TLASes, {} walked the guest BVH",
	                native, null_tlas + unpatched));
	if (unpatched != 0) {
		log(fmt::format("[RT]   {} of programs whose BVH walk the patch does not recognize (last cs {})", unpatched,
		                last(Kind::Unpatched)));
	}
	if (null_tlas != 0) {
		log(fmt::format("[RT]   {} without a TLAS (the program without the patch)", null_tlas));
	}
	if (unpublished != 0) {
		log(fmt::format("[RT]   {} TLAS walks published no context ({} with a null header, {} with a header that is "
		                "not a TLAS; last cs {}): their dispatches walked the guest BVH",
		                unpublished, null_header, bad_header,
		                RtShader(stats.last_unpublished.load(std::memory_order_relaxed))));
	}
	if (uncached != 0) {
		log(fmt::format("[RT]   {} TLAS walks found TLAS memory not cached yet (last cs {}): their rays missed what "
		                "it holds, cached for the next walks",
		                uncached, RtShader(stats.last_uncached.load(std::memory_order_relaxed))));
	}
	if (overflow != 0) {
		log(fmt::format("[RT]   {} TLAS walks found more box nodes or instances than the host structures hold "
		                "(last cs {}): their rays missed the rest",
		                overflow, RtShader(stats.last_overflow.load(std::memory_order_relaxed))));
	}
	if (rays != 0) {
		log(fmt::format("[RT]   {} rays went back to the guest traversal (last cs {})", rays,
		                RtShader(stats.last_guest_rays.load(std::memory_order_relaxed))));
	}
	if (new_unpatched + new_guest_sites != 0) {
		log(fmt::format("[RT]   programs compiled so far: {} patched, {} whose BVH walk the patch does not recognize "
		                "(last cs {}), {} with sites that always walk the guest BVH (last cs {})",
		                programs.patched, programs.unpatched, RtShader(programs.last_unpatched), programs.guest_sites,
		                RtShader(programs.last_guest_sites)));
	}
}
} // namespace

void NativeRayTracing::NoteGuestDispatch(GuestDispatch kind, uint64_t shader_hash) noexcept {
	auto& stats = g_rt_stats;
	stats.guest[static_cast<size_t>(kind)].Add();
	stats.last_guest[static_cast<size_t>(kind)].store(shader_hash, std::memory_order_relaxed);
	// Without dispatches traced natively there are no readbacks to summarize from.
	if ((++stats.guest_calls & 63u) == 0) {
		SummarizeRayTracing();
	}
}

void NativeRayTracing::NoteGuestRays(uint64_t shader_hash, uint64_t rays) {
	g_rt_stats.guest_rays.fetch_add(rays, std::memory_order_relaxed);
	g_rt_stats.last_guest_rays.store(shader_hash, std::memory_order_relaxed);
	SummarizeRayTracing();
}

void NativeRayTracing::SetDeviceSupport(bool supported) noexcept {
	g_device_support.store(supported, std::memory_order_relaxed);
}

bool NativeRayTracing::PatchShaders() noexcept {
	return g_device_support.load(std::memory_order_relaxed);
}

bool NativeRayTracing::FindTlasSource(const ShaderRecompiler::IR::ShaderInfo& info,
                                      TlasSource& source) noexcept {
	// Only the programs the recompiler patched trace through the host structures.
	if (!info.ray_query || !PatchShaders()) {
		return false;
	}
	source = {info.ray_query_user_sgpr, info.ray_query_header_offset, info.ray_query_root, info.ray_query_layout,
	          info.ray_query_header_slots, info.ray_query_header_vsharp};
	return true;
}

uint64_t NativeRayTracing::UserPointer(const TlasSource& source, uint64_t pointer,
                                       const std::function<bool(uint64_t, uint64_t)>& readable) {
	if (source.header_vsharp == ShaderRecompiler::IR::ShaderInfo::NoRayQuerySlot) {
		return pointer;
	}
	std::array<uint32_t, 2> vsharp {};
	if (pointer > UINT64_MAX - source.header_vsharp || !readable(pointer + source.header_vsharp, sizeof(vsharp))) {
		return 0;
	}
	std::memcpy(vsharp.data(), reinterpret_cast<const void*>(pointer + source.header_vsharp), sizeof(vsharp));
	return uint64_t {vsharp[0]} | (uint64_t {vsharp[1] & 0xffffu} << 32u);
}

bool NativeRayTracing::FindGuestTlasSource(const ShaderRecompiler::IR::ShaderInfo& info,
                                           TlasSource& source) noexcept {
	if (FindTlasSource(info, source)) {
		return true;
	}
	// A layout is never all zero (Decoder::RayTracingLayout::Pack()).
	if (!info.bvh || info.ray_query_layout == std::array<uint32_t, 4> {}) {
		return false;
	}
	source = {info.ray_query_user_sgpr, info.ray_query_header_offset, info.ray_query_root, info.ray_query_layout,
	          {ShaderRecompiler::IR::ShaderInfo::NoRayQuerySlot, ShaderRecompiler::IR::ShaderInfo::NoRayQuerySlot},
	          info.ray_query_header_vsharp};
	return true;
}

NativeRayTracing::GuestTlas NativeRayTracing::ReadGuestTlas(
    uint64_t header, const TlasSource& source, const std::function<bool(uint64_t, uint64_t)>& readable,
    std::vector<std::pair<uint64_t, uint64_t>>& blases) {
	// As rt_tlas_walk.comp reads the TLAS (Decoder::RayTracingLayout::Pack() for the layout).
	if (header == 0 || (header & 255u) != 0) {
		return {};
	}
	for (const auto& [known, tlas]: m_guest_tlases) {
		if (known == header) {
			return tlas;
		}
	}
	constexpr uint64_t HeaderSize = 256;
	// Larger TLASes are not plausible (the walk's queue holds no more box nodes).
	constexpr uint64_t MaxExtent = uint64_t {64} * 1024 * 1024;
	if (header > UINT64_MAX - MaxExtent || !readable(header, HeaderSize)) {
		return {};
	}
	const auto load = [](uint64_t address, auto& value) {
		std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
	};
	const auto& layout = source.layout;
	// The node limit of a BVH header (BlasNodeLimit() in rt_blas_walk.comp).
	const auto node_limit_at = [&](uint64_t bvh) {
		uint32_t count = 0;
		load(bvh + (((layout[3] >> 29u) & 1u) != 0 ? 80 : 88), count);
		return ((layout[3] >> 29u) & 1u) != 0 ? count - 1u : count;
	};
	const uint32_t node_limit = node_limit_at(header);
	uint64_t       instance_offset = 0;
	load(header + ((layout[1] >> 16u) & 15u) * 8u, instance_offset);
	if (node_limit >= QueueCapacity || instance_offset >= MaxExtent) {
		return {};
	}
	const auto nodes_end = header + (uint64_t {node_limit} + 1) * 64;
	if (!readable(header, nodes_end - header)) {
		return {};
	}
	// The box nodes from the root (no more than the TLAS has) and the instance leaves they reach
	// (the largest index); inline instances have their record in the header.
	const bool     inline_layout = ((layout[3] >> 19u) & 1u) != 0;
	const uint32_t inline_type   = (layout[3] >> 16u) & 7u;
	std::vector<uint32_t> queue {source.root};
	std::vector<uint32_t> leaves;
	bool                  inline_leaf   = false;
	int64_t               last_instance = -1;
	uint32_t              instances     = 0;
	for (size_t i = 0; i < queue.size() && i <= node_limit; i++) {
		const auto address = header + (uint64_t {queue[i] >> 3u} << 6u);
		if (address + 16 > nodes_end) {
			continue;
		}
		std::array<uint32_t, 4> children {};
		load(address, children);
		for (const auto child: children) {
			const auto type = child & 7u;
			if (child == 0xffffffffu) {
				continue;
			}
			if (type == 4u || type == 5u) {
				queue.push_back(child);
			} else if (type < 4u || (inline_layout && type == inline_type)) {
				instances++;
				if (inline_layout && type == inline_type) {
					inline_leaf = true;
				} else {
					leaves.push_back(child >> 3u);
					last_instance = std::max<int64_t>(last_instance, child >> 3u);
				}
			}
		}
	}
	const auto stride        = uint64_t {128} + ((layout[1] >> 30u) & 3u) * 32u;
	const auto instances_end = header + instance_offset + static_cast<uint64_t>(last_instance + 1) * stride;
	const auto inline_end    = inline_layout ? header + (layout[3] & 0xffu) * 8u + 4u + stride : 0;
	const auto end = std::min(std::max({nodes_end, instances_end, inline_end}), header + MaxExtent);

	// The BLASes of the instances (LoadGuestInstance() in rt_common.inc), when the CPU wrote them as
	// well (one in ASTRO BOT before Go-Go Archipelago): their nodes, and the table of the geometry
	// of each triangle node of a Psr BLAS (rt_blas_walk.comp).
	std::vector<uint64_t> blas_list;
	const auto            record_blas = [&](uint64_t record) {
		const auto dword = ((layout[1] >> 20u) & 15u) | (((layout[3] >> 20u) & 3u) << 4u);
		// Records of 128 bytes are read up to 64 bytes.
		if ((dword + 2u) * 4u > (stride > 128u ? 160u : 64u) || !readable(record, (dword + 2u) * 4u)) {
			return;
		}
		std::array<uint32_t, 2> words {};
		load(record + dword * 4u, words);
		const auto blas = ((layout[1] >> 24u) & 1u) != 0
		                      ? (uint64_t {words[0]} | (uint64_t {words[1] & 0x3fffffu} << 32u)) << 3u
		                      : uint64_t {words[0]} | (uint64_t {words[1]} << 32u);
		if (blas != 0 && (blas & 255u) == 0) {
			blas_list.push_back(blas);
		}
	};
	for (const auto leaf: leaves) {
		record_blas(header + instance_offset + uint64_t {leaf} * stride);
	}
	if (inline_leaf) {
		record_blas(header + (layout[3] & 0xffu) * 8u + ((layout[3] >> 28u) & 1u) * 4u);
	}
	std::sort(blas_list.begin(), blas_list.end());
	blas_list.erase(std::unique(blas_list.begin(), blas_list.end()), blas_list.end());
	for (const auto blas: blas_list) {
		if (blas > UINT64_MAX - MaxExtent || !readable(blas, HeaderSize)) {
			continue;
		}
		const auto limit = node_limit_at(blas);
		if (limit >= MaxExtent / 64) {
			continue;
		}
		auto                    blas_end = blas + (uint64_t {limit} + 1) * 64;
		std::array<uint32_t, 2> magic {};
		uint32_t                version = 0;
		uint32_t                table   = 0;
		load(blas, magic);
		load(blas + 252, version);
		load(blas + 40, table);
		if (magic[0] == 0x5f525350u && magic[1] == 0x4c485642u && version == 9u && table < MaxExtent) {
			blas_end = std::max(blas_end, blas + table + (uint64_t {limit} + 1) * 4);
		}
		blases.emplace_back(blas, std::min(blas_end, blas + MaxExtent));
	}
	const GuestTlas tlas {end, instances};
	m_guest_tlases[m_guest_tlas_next++ % m_guest_tlases.size()] = {header, tlas};
	return tlas;
}

NativeRayTracing::Allocation NativeRayTracing::Arena::Allocate(uint64_t size, uint64_t alignment) {
	// Sizes and chunk starts are multiples of 256, so are all offsets.
	EXIT_IF(alignment == 0 || alignment > 256 || (256 % alignment) != 0);
	size = (size + 255u) & ~uint64_t {255};
	const auto carve = [&](Chunk& chunk, uint64_t offset, uint64_t length) {
		chunk.free_ranges.erase(offset);
		if (length > size) {
			chunk.free_ranges.emplace(offset + size, length - size);
		}
		chunk.free_bytes -= size;
		m_used += size;
		return Allocation {chunk.buffer.get(), offset, chunk.buffer->BufferDeviceAddress() + offset, size};
	};
	if (size <= ChunkSize / 2) {
		// The fullest chunk with a range that fits, the smallest such range in it: the other
		// chunks empty out and are released. The chunk being drained takes nothing.
		Chunk*   best        = nullptr;
		uint64_t best_offset = 0;
		uint64_t best_length = 0;
		for (auto& chunk: m_chunks) {
			if (chunk->dedicated || chunk->buffer.get() == m_draining || chunk->free_bytes < size ||
			    (best != nullptr && chunk->free_bytes >= best->free_bytes)) {
				continue;
			}
			uint64_t offset = 0;
			uint64_t length = UINT64_MAX;
			for (const auto& [range_offset, range_length]: chunk->free_ranges) {
				if (range_length >= size && range_length < length) {
					offset = range_offset;
					length = range_length;
				}
			}
			if (length != UINT64_MAX) {
				best        = chunk.get();
				best_offset = offset;
				best_length = length;
			}
		}
		if (best != nullptr) {
			return carve(*best, best_offset, best_length);
		}
	}
	auto chunk       = std::make_unique<Chunk>();
	chunk->dedicated = size > ChunkSize / 2;
	chunk->buffer    = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
	                                            AsFlags, chunk->dedicated ? size : ChunkSize);
	chunk->free_ranges.emplace(0, chunk->buffer->Size());
	chunk->free_bytes = chunk->buffer->Size();
	m_reserved += chunk->buffer->Size();
	m_chunks.push_back(std::move(chunk));
	return carve(*m_chunks.back(), 0, m_chunks.back()->buffer->Size());
}

void NativeRayTracing::Arena::Free(const Allocation& allocation) {
	if (allocation.buffer == nullptr) {
		return;
	}
	const auto it = std::find_if(m_chunks.begin(), m_chunks.end(), [&](const auto& chunk) {
		return chunk->buffer.get() == allocation.buffer;
	});
	EXIT_IF(it == m_chunks.end());
	auto& chunk  = **it;
	auto  offset = allocation.offset;
	auto  length = allocation.size;
	m_used -= length;
	chunk.free_bytes += length;
	// Merges with the free ranges around it.
	auto next = chunk.free_ranges.lower_bound(offset);
	if (next != chunk.free_ranges.end() && offset + length == next->first) {
		length += next->second;
		next = chunk.free_ranges.erase(next);
	}
	if (next != chunk.free_ranges.begin()) {
		const auto previous = std::prev(next);
		if (previous->first + previous->second == offset) {
			offset = previous->first;
			length += previous->second;
			chunk.free_ranges.erase(previous);
		}
	}
	chunk.free_ranges.emplace(offset, length);
	if (chunk.free_bytes == chunk.buffer->Size()) {
		// Nothing allocated: released, unless it is the only empty standard chunk.
		const auto empty = std::count_if(m_chunks.begin(), m_chunks.end(), [](const auto& c) {
			return !c->dedicated && c->free_bytes == c->buffer->Size();
		});
		if (chunk.dedicated || chunk.buffer.get() == m_draining || empty > 1) {
			if (chunk.buffer.get() == m_draining) {
				m_draining = nullptr;
			}
			m_reserved -= chunk.buffer->Size();
			m_chunks.erase(it);
		}
	}
}

bool NativeRayTracing::Arena::BeginDrain() {
	uint64_t total_free = 0;
	for (const auto& chunk: m_chunks) {
		if (!chunk->dedicated) {
			total_free += chunk->free_bytes;
		}
	}
	const Chunk* best      = nullptr;
	uint64_t     best_used = 0;
	for (const auto& chunk: m_chunks) {
		const auto used = chunk->buffer->Size() - chunk->free_bytes;
		if (chunk->dedicated || used == 0 || (best != nullptr && used >= best_used)) {
			continue;
		}
		// The other chunks must hold what it has, with some room to spare for fragmentation.
		if (total_free - chunk->free_bytes < used + ChunkSize / 4) {
			continue;
		}
		best      = chunk.get();
		best_used = used;
	}
	m_draining = best != nullptr ? best->buffer.get() : nullptr;
	return best != nullptr;
}

NativeRayTracing::NativeRayTracing(GraphicContext& graphics, CommandScheduler& scheduler,
                                   BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_buffer_cache(buffer_cache),
      m_arena(graphics, scheduler) {
	m_enabled = graphics.native_ray_tracing_enabled;
	if (!m_enabled) {
		return;
	}
	CreatePipelines();
	CreateBuffers();
	CreateTlas(InstanceCapacity);
	// The memory of the BLASes is limited: KYTY_RT_ARENA_MB (0: no limit), else a quarter of the
	// largest device local heap.
	if (const char* limit = std::getenv("KYTY_RT_ARENA_MB"); limit != nullptr && limit[0] != 0) {
		m_budget = std::strtoull(limit, nullptr, 10) << 20u;
	} else {
		const auto& memory = m_graphics.physical_device_memory_properties;
		uint64_t    heap   = 0;
		for (uint32_t i = 0; i < memory.memoryHeapCount; i++) {
			if (memory.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
				heap = std::max<uint64_t>(heap, memory.memoryHeaps[i].size);
			}
		}
		m_budget = std::clamp(heap / 4, BudgetMin, BudgetMax);
	}
	LOGF("NativeRayTracing: enabled, BLAS memory budget %llu MiB\n",
	     static_cast<unsigned long long>(m_budget >> 20u));
}

NativeRayTracing::~NativeRayTracing() {
	if (!m_enabled) {
		return;
	}
	auto device = m_graphics.device;
	for (auto& slot: m_slot_list) {
		if (slot && slot->handle) {
			device.destroyAccelerationStructureKHR(slot->handle);
		}
	}
	for (auto& retired: m_retired) {
		if (retired.handle) {
			device.destroyAccelerationStructureKHR(retired.handle);
		}
	}
	if (m_tlas) {
		device.destroyAccelerationStructureKHR(m_tlas);
	}
	device.destroyPipeline(m_tlas_pipeline);
	device.destroyPipeline(m_blas_pipeline);
	device.destroyPipelineLayout(m_tlas_layout);
	device.destroyPipelineLayout(m_blas_layout);
	device.destroyPipeline(m_hle_tlas_pipeline);
	device.destroyPipelineLayout(m_hle_tlas_layout);
	device.destroyPipeline(m_hle_refit_pipeline);
	device.destroyPipelineLayout(m_hle_refit_layout);
	device.destroyDescriptorSetLayout(m_descriptor_layout);
}

void NativeRayTracing::CreatePipelines() {
	auto device = m_graphics.device;
	// The BDA page table, and the fault and use bitmaps the walks record their pages in (the HLE
	// passes only read the page table).
	std::array<vk::DescriptorSetLayoutBinding, WalkBindings> bindings {};
	for (uint32_t i = 0; i < WalkBindings; i++) {
		bindings[i] = {i, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr};
	}
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = WalkBindings;
	layout_info.pBindings    = bindings.data();
	RequireVulkanSuccess(device.createDescriptorSetLayout(&layout_info, nullptr, &m_descriptor_layout),
	                     "create native ray tracing descriptor layout");
	const auto make = [&](std::span<const uint32_t> code, uint32_t push_size,
	                      vk::PipelineLayout& layout, vk::Pipeline& pipeline, const char* name) {
		const vk::PushConstantRange push {vk::ShaderStageFlagBits::eCompute, 0, push_size};
		vk::PipelineLayoutCreateInfo info {};
		info.setLayoutCount         = 1;
		info.pSetLayouts            = &m_descriptor_layout;
		info.pushConstantRangeCount = 1;
		info.pPushConstantRanges    = &push;
		RequireVulkanSuccess(device.createPipelineLayout(&info, nullptr, &layout),
		                     "create native ray tracing pipeline layout");
		const auto module = CompileSPV(code, device);
		vk::PipelineShaderStageCreateInfo stage {};
		stage.stage  = vk::ShaderStageFlagBits::eCompute;
		stage.module = module;
		stage.pName  = "main";
		vk::ComputePipelineCreateInfo pipeline_info {};
		pipeline_info.stage  = stage;
		pipeline_info.layout = layout;
		const auto result = device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &pipeline);
		device.destroyShaderModule(module, nullptr);
		RequireVulkanSuccess(result, "create native ray tracing pipeline");
		SetVulkanObjectNameF(device, pipeline, "%s", name);
	};
	make(RT_TLAS_WALK_SPV, sizeof(TlasPush), m_tlas_layout, m_tlas_pipeline, "RT TLAS walk");
	make(RT_BLAS_WALK_SPV, sizeof(BlasPush), m_blas_layout, m_blas_pipeline, "RT BLAS walk");
	make(HLE_TLAS_SPV, sizeof(HleTlasPush), m_hle_tlas_layout, m_hle_tlas_pipeline, "HLE TLAS build");
	make(HLE_BLAS_REFIT_SPV, sizeof(HleRefitPush), m_hle_refit_layout, m_hle_refit_pipeline,
	     "HLE BLAS refit");
}

void NativeRayTracing::CreateBuffers() {
	const auto make = [&](uint64_t size, vk::BufferUsageFlags flags, const char* name) {
		auto buffer = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
		                                       flags, size);
		SetVulkanObjectNameF(m_graphics.device, buffer->Handle(), "%s", name);
		return buffer;
	};
	m_state          = make(StateSize, DataFlags, "RT state");
	m_queue          = make(uint64_t {QueueCapacity} * 4, DataFlags, "RT queue");
	m_instance_nodes = make(uint64_t {InstanceCapacity} * 4, DataFlags, "RT instance nodes");
	m_instances      = make(uint64_t {InstanceCapacity} * InstanceStride, AsFlags, "RT instances");
	m_table          = make(uint64_t {TableEntries + HostTableEntries} * TableStride, DataFlags, "RT BLAS table");
	m_slots          = make(uint64_t {SlotCapacity} * SlotStride, DataFlags, "RT slots");
	m_missing        = make(uint64_t {MissingCapacity} * MissingStride, DataFlags, "RT missing");
	m_context        = make(uint64_t {MaxContexts} * ContextStride, DataFlags, "RT context");
	m_pool           = make(uint64_t {PoolCapacity} * 4, DataFlags, "RT walk pool");
	m_verify_list    = make(uint64_t {SlotCapacity} * 4, DataFlags, "RT verify list");
	m_changed        = make(uint64_t {ChangedCapacity} * 4, DataFlags, "RT changed");
	m_jobs           = make(uint64_t {MaxJobs} * JobStride, DataFlags, "RT jobs");
	m_layouts        = make(uint64_t {LayoutCapacity} * 16, DataFlags, "RT build layouts");
	m_host_keys.assign(HostTableEntries, 0);
	m_build_scratch  = make(BuildScratchSize, AsFlags, "RT build scratch");
	m_hle_tlas_scratch = make(HleTlasScratchSize, DataFlags, "HLE TLAS bounds");
	m_hle_jobs       = make(HleRefitJobsSize, DataFlags, "HLE refit jobs");
	m_hle_pool       = make(uint64_t {HleRefitPoolCapacity} * 4, DataFlags, "HLE refit pool");
	m_hle_state      = make(HleRefitStateSize, DataFlags, "HLE refit state");
	for (auto& readback: m_readbacks) {
		readback.buffer = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                           AllFlags, ReadbackSize);
	}
	m_context_address = m_context->BufferDeviceAddress();
}

void NativeRayTracing::CreateTlas(uint32_t capacity) {
	auto device = m_graphics.device;
	vk::AccelerationStructureGeometryKHR geometry {};
	geometry.geometryType                       = vk::GeometryTypeKHR::eInstances;
	geometry.geometry.instances.sType           = vk::StructureType::eAccelerationStructureGeometryInstancesDataKHR;
	geometry.geometry.instances.arrayOfPointers = VK_FALSE;
	vk::AccelerationStructureBuildGeometryInfoKHR info {};
	info.type          = vk::AccelerationStructureTypeKHR::eTopLevel;
	info.flags         = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
	info.mode          = vk::BuildAccelerationStructureModeKHR::eBuild;
	info.geometryCount = 1;
	info.pGeometries   = &geometry;
	vk::AccelerationStructureBuildSizesInfoKHR sizes {};
	device.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice, &info,
	                                             &capacity, &sizes);
	m_tlas_storage = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
	                                          AsFlags, sizes.accelerationStructureSize);
	vk::AccelerationStructureCreateInfoKHR create {};
	create.buffer = m_tlas_storage->Handle();
	create.size   = sizes.accelerationStructureSize;
	create.type   = vk::AccelerationStructureTypeKHR::eTopLevel;
	RequireVulkanSuccess(device.createAccelerationStructureKHR(&create, nullptr, &m_tlas),
	                     "create native TLAS");
	vk::AccelerationStructureDeviceAddressInfoKHR address_info {};
	address_info.accelerationStructure = m_tlas;
	m_tlas_address      = device.getAccelerationStructureAddressKHR(address_info);
	m_tlas_scratch_size = sizes.buildScratchSize;
	m_tlas_capacity     = capacity;
	EXIT_IF(m_tlas_scratch_size > BuildScratchSize);
}

bool NativeRayTracing::ComputeSlotSizes(uint32_t opaque, uint32_t alpha, SlotSizes& sizes, bool exact) {
	auto device = m_graphics.device;
	// Room to grow (unless the counts are capacities already); the 64-bit sums cannot overflow.
	const uint64_t capacity0 = exact ? opaque : uint64_t {opaque} + opaque / 8 + 16;
	const uint64_t capacity1 = exact ? alpha : uint64_t {alpha} + alpha / 8 + 16;
	if (capacity0 + capacity1 > MaxSlotTriangles) {
		return false;
	}
	sizes.capacity0 = static_cast<uint32_t>(capacity0);
	sizes.capacity1 = static_cast<uint32_t>(capacity1);
	const std::array geometries {TriangleGeometry(0, sizes.capacity0, true),
	                             TriangleGeometry(0, sizes.capacity1, false)};
	const std::array counts {sizes.capacity0, sizes.capacity1};
	vk::AccelerationStructureBuildGeometryInfoKHR info {};
	info.type          = vk::AccelerationStructureTypeKHR::eBottomLevel;
	info.flags         = BlasFlags;
	info.mode          = vk::BuildAccelerationStructureModeKHR::eBuild;
	info.geometryCount = static_cast<uint32_t>(geometries.size());
	info.pGeometries   = geometries.data();
	vk::AccelerationStructureBuildSizesInfoKHR build {};
	device.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice, &info,
	                                             counts.data(), &build);
	{
		// Every kind of build fits, and an update.
		for (const auto flags: {FastBlasFlags, UpdatableBlasFlags, FastUpdatableBlasFlags}) {
			auto other_info  = info;
			other_info.flags = flags;
			vk::AccelerationStructureBuildSizesInfoKHR other {};
			device.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice,
			                                             &other_info, counts.data(), &other);
			build.accelerationStructureSize = std::max(build.accelerationStructureSize, other.accelerationStructureSize);
			build.buildScratchSize = std::max({build.buildScratchSize, other.buildScratchSize, other.updateScratchSize});
		}
	}
	sizes.storage   = (build.accelerationStructureSize + 255u) & ~uint64_t {255};
	sizes.vertices0 = (uint64_t {sizes.capacity0} * 36 + 255u) & ~uint64_t {255};
	sizes.vertices1 = (uint64_t {sizes.capacity1} * 36 + 255u) & ~uint64_t {255};
	sizes.entries   = (AlphaRecordsOffset(sizes.capacity0, sizes.capacity1) + uint64_t {sizes.capacity1} * 32 + 255u) &
	                ~uint64_t {255};
	sizes.scratch   = build.buildScratchSize;
	// A build that does not fit in the scratch buffer could never run (and would stall the queue).
	if (sizes.scratch > BuildScratchSize || sizes.Total() > MaxSlotBytes) {
		return false;
	}
	return m_budget == 0 || m_arena.Used() + sizes.Total() <= m_budget;
}

uint64_t NativeRayTracing::SlotBytes(const Slot& slot) noexcept {
	return slot.storage.size + slot.vertices0.size + slot.vertices1.size + slot.entries.size;
}

void NativeRayTracing::AllocateSlot(Slot& slot, const SlotSizes& sizes) {
	auto device        = m_graphics.device;
	slot.capacity0     = sizes.capacity0;
	slot.capacity1     = sizes.capacity1;
	slot.storage       = m_arena.Allocate(sizes.storage, 256);
	slot.vertices0     = m_arena.Allocate(sizes.vertices0, 256);
	slot.vertices1     = m_arena.Allocate(sizes.vertices1, 256);
	slot.entries       = m_arena.Allocate(sizes.entries, 256);
	slot.build_scratch = sizes.scratch;
	slot.built         = false;
	slot.updatable     = false;
	vk::AccelerationStructureCreateInfoKHR create {};
	create.buffer = slot.storage.buffer->Handle();
	create.offset = slot.storage.offset;
	create.size   = sizes.storage;
	create.type   = vk::AccelerationStructureTypeKHR::eBottomLevel;
	RequireVulkanSuccess(device.createAccelerationStructureKHR(&create, nullptr, &slot.handle),
	                     "create native BLAS");
	vk::AccelerationStructureDeviceAddressInfoKHR address_info {};
	address_info.accelerationStructure = slot.handle;
	slot.as_address = device.getAccelerationStructureAddressKHR(address_info);
}

bool NativeRayTracing::CreateSlot(const PendingSlot& pending) {
	// Its build was seen after the walk reported it: the walk finds the host's slot first, the
	// entry the walk claimed is released.
	if (const auto it = m_slots_by_blas.find(pending.blas_va); it != m_slots_by_blas.end()) {
		for (const auto index: it->second) {
			if (m_slot_list[index]->root == pending.root) {
				m_retry.push_back(pending.table_index);
				return true;
			}
		}
	}
	// Implausibly large (see MaxSlotTriangles) or over the budget: reported again later.
	SlotSizes sizes;
	if (!ComputeSlotSizes(pending.opaque, pending.alpha, sizes)) {
		return false;
	}
	uint32_t index = 0;
	if (!m_free_slots.empty()) {
		index = m_free_slots.back();
		m_free_slots.pop_back();
	} else if (m_slot_list.size() < SlotCapacity) {
		index = static_cast<uint32_t>(m_slot_list.size());
		m_slot_list.emplace_back();
	} else {
		return false;
	}
	auto slot            = std::make_unique<Slot>();
	slot->blas_va        = pending.blas_va;
	slot->root           = pending.root;
	slot->table_index    = pending.table_index;
	slot->leaf_count     = pending.leaf_count;
	slot->last_use_frame = m_frame;
	AllocateSlot(*slot, sizes);
	slot->needs_build = true;
	m_slots_by_blas[slot->blas_va].push_back(index);
	m_build_queue.push_back(index);
	m_slot_list[index] = std::move(slot);
	// Counted: a later failure of the same BLAS starts over.
	m_retries.erase(pending.blas_va ^ (uint64_t {pending.root} << 48u));
	return true;
}

void NativeRayTracing::RetireResources(Slot& slot, uint32_t slot_index) {
	Retired retired {};
	retired.tick        = m_scheduler.CurrentTick();
	retired.prepare_id  = m_prepare_id;
	retired.handle      = slot.handle;
	retired.allocations = {slot.storage, slot.vertices0, slot.vertices1, slot.entries};
	retired.slot        = slot_index;
	slot.handle         = nullptr;
	slot.as_address     = 0;
	slot.storage = slot.vertices0 = slot.vertices1 = slot.entries = {};
	for (const auto& allocation: retired.allocations) {
		m_retired_bytes += allocation.size;
	}
	m_retired.push_back(retired);
}

void NativeRayTracing::DestroyRetired() {
	auto device = m_graphics.device;
	while (!m_retired.empty()) {
		auto& retired = m_retired.front();
		// Every readback recorded before the release has been processed once the ring went round:
		// none can name the slot index any more.
		const bool readbacks_done = retired.slot == UINT32_MAX ||
		                            m_prepare_id > retired.prepare_id + m_readbacks.size();
		if (!readbacks_done || !m_scheduler.IsFree(retired.tick)) {
			break;
		}
		if (retired.handle) {
			device.destroyAccelerationStructureKHR(retired.handle);
		}
		for (const auto& allocation: retired.allocations) {
			m_retired_bytes -= allocation.size;
			m_arena.Free(allocation);
		}
		if (retired.slot != UINT32_MAX) {
			m_slot_list[retired.slot].reset();
			m_free_slots.push_back(retired.slot);
		}
		m_retired.pop_front();
	}
}

void NativeRayTracing::EvictSlot(uint32_t index) {
	auto& slot       = *m_slot_list[index];
	slot.evicted     = true;
	slot.needs_build = false;
	if (slot.host_table_index != UINT32_MAX) {
		m_host_keys[slot.host_table_index] = TableTombstone;
		m_host_table_writes.emplace_back(slot.host_table_index, TableTombstone, 0);
		slot.host_table_index = UINT32_MAX;
	}
	if (const auto it = m_slots_by_blas.find(slot.blas_va); it != m_slots_by_blas.end()) {
		std::erase(it->second, index);
		if (it->second.empty()) {
			m_slots_by_blas.erase(it);
		}
	}
	RetireResources(slot, index);
}

void NativeRayTracing::EvictUnused(std::vector<uint32_t>& evicted) {
	if (m_frame > EvictFrames) {
		const auto limit = m_frame - EvictFrames;
		for (uint32_t index = 0; index < m_slot_list.size() && evicted.size() < MaxEvictions; index++) {
			auto* slot = m_slot_list[index].get();
			if (slot == nullptr || slot->evicted || slot->last_use_frame >= limit) {
				continue;
			}
			EvictSlot(index);
			evicted.push_back(index);
		}
	}
	EvictOverBudget(evicted);
}

void NativeRayTracing::EvictOverBudget(std::vector<uint32_t>& evicted) {
	// What the retired slots hold is on its way out.
	const auto live = m_arena.Used() - m_retired_bytes;
	if (m_budget == 0 || live <= m_budget / 8 * 7 || m_frame <= LruMinAge) {
		return;
	}
	const auto                                 limit = m_frame - LruMinAge;
	std::vector<std::pair<uint64_t, uint32_t>> candidates;
	for (uint32_t index = 0; index < m_slot_list.size(); index++) {
		const auto* slot = m_slot_list[index].get();
		if (slot != nullptr && !slot->evicted && slot->last_use_frame < limit) {
			candidates.push_back({slot->last_use_frame, index});
		}
	}
	std::sort(candidates.begin(), candidates.end());
	auto current = live;
	for (const auto& [frame, index]: candidates) {
		if (current <= m_budget / 4 * 3 || evicted.size() >= MaxEvictions) {
			break;
		}
		current -= std::min(current, SlotBytes(*m_slot_list[index]));
		EvictSlot(index);
		evicted.push_back(index);
	}
}

void NativeRayTracing::Defragment(std::vector<uint32_t>& moved) {
	const auto used = m_arena.Used();
	if (m_arena.Draining() == nullptr) {
		if (m_prepare_id < m_defrag_after ||
		    m_arena.Reserved() < used + std::max(DefragMinWaste, used / 2) || !m_arena.BeginDrain()) {
			return;
		}
		m_defrag_started = m_prepare_id;
	}
	const Buffer* chunk   = m_arena.Draining();
	const auto    before  = moved.size();
	const auto    touches = [&](const Allocation& allocation) { return allocation.buffer == chunk; };
	for (uint32_t index = 0; index < m_slot_list.size() && moved.size() - before < MaxMoves; index++) {
		auto* slot = m_slot_list[index].get();
		if (slot == nullptr || slot->evicted || slot->handle == nullptr ||
		    !(touches(slot->storage) || touches(slot->vertices0) || touches(slot->vertices1) ||
		      touches(slot->entries))) {
			continue;
		}
		// The same capacities, in memory outside the chunk.
		SlotSizes sizes;
		if (!ComputeSlotSizes(slot->capacity0, slot->capacity1, sizes, true)) {
			continue;
		}
		RetireResources(*slot, UINT32_MAX);
		AllocateSlot(*slot, sizes);
		slot->needs_build = true;
		m_build_queue.push_back(index);
		moved.push_back(index);
	}
	if (moved.size() != before) {
		return;
	}
	// Nothing left to move: the chunk is released once the retired slots are destroyed; a pass
	// that cannot finish rests.
	const bool waiting = std::any_of(m_retired.begin(), m_retired.end(), [&](const Retired& retired) {
		return std::any_of(retired.allocations.begin(), retired.allocations.end(), touches);
	});
	if (!waiting || m_prepare_id - m_defrag_started > DefragMaxPrepares) {
		m_arena.EndDrain();
		m_defrag_after = m_prepare_id + DefragRest;
	}
}

void NativeRayTracing::ReportAgain(uint64_t blas_va, uint32_t root, uint32_t table_index) {
	if (m_retries.size() >= MaxRetryKeys) {
		// Forget the failures: the entries of the exhausted ones are released so they can be
		// reported again, the others are queued anyway.
		for (const auto& [key, state]: m_retries) {
			if (state.count > MaxRetries) {
				m_retry.push_back(state.table_index);
			}
		}
		m_retries.clear();
	}
	auto& state       = m_retries[blas_va ^ (uint64_t {root} << 48u)];
	state.table_index = table_index;
	if (++state.count <= MaxRetries) {
		m_retry.push_back(table_index);
	} else {
		// Not given up for good: it is reported again later.
		m_late_retries.push_back({m_prepare_id + LateRetryPrepares, table_index});
	}
}

void NativeRayTracing::RecordBuilds(const CommandRecorder& command) {
	auto&                 readback = m_readbacks[m_readback_index];
	std::vector<Job>      jobs;
	std::vector<uint32_t> slots;
	uint64_t              scratch   = 0;
	uint64_t              pool_use  = 0;
	// The layouts of the jobs that have one.
	std::vector<std::array<uint32_t, 4>> layouts;
	constexpr uint64_t    Alignment = 256;
	// A slot gathers (see below) when the guest only refit its BLAS since the last host build: the
	// first build after a refit pass, or, for a slot the guest changes again within the frame
	// (dynamic), every build of the same refit pass.
	const auto gathers = [&](const Slot* slot) {
		return slot != nullptr && slot->built && slot->refit_seq == m_refit_seq && slot->gathers < MaxGathers &&
		       (slot->built_refit_seq + 1 == m_refit_seq ||
		        (slot->built_refit_seq == m_refit_seq && slot->dynamic_until != 0));
	};
	{
		Common::LockGuard lock(m_mutex);
		// The gathers of the last refit first (the animated BLASes the frame traces: cheap, no
		// walk), then the oldest builds (a slot never built counts as the oldest): the queue used
		// to be served last in, first out, and under a long backlog (a hundred or more animated
		// BLASes asking for a build every frame) the slots deep in it, such as a ped that just got
		// out of a vehicle, kept the shape of their last build for tens of frames. The queue is
		// served from its end.
		std::stable_sort(m_build_queue.begin(), m_build_queue.end(), [&](uint32_t a, uint32_t b) {
			const auto* slot_a = m_slot_list[a].get();
			const auto* slot_b = m_slot_list[b].get();
			if (gathers(slot_a) != gathers(slot_b)) {
				return !gathers(slot_a);
			}
			const auto age = [&](const Slot* slot) {
				return slot == nullptr || slot->built_frame == UINT64_MAX ? uint64_t {0} : slot->built_frame;
			};
			return age(slot_a) > age(slot_b);
		});
		// Slots whose walks did not fit in the pool this time: they keep their place in the queue.
		std::vector<uint32_t> deferred;
		while (!m_build_queue.empty() && jobs.size() < MaxJobs) {
			const auto index = m_build_queue.back();
			if (m_slot_list[index] == nullptr || m_slot_list[index]->evicted ||
			    !m_slot_list[index]->needs_build) {
				// Released or built since it was queued.
				m_build_queue.pop_back();
				continue;
			}
			auto&      slot  = *m_slot_list[index];
			const auto size  = (slot.build_scratch + Alignment - 1) & ~(Alignment - 1);
			if (scratch + size > BuildScratchSize ||
			    (!slot.layout.empty() && layouts.size() + slot.layout.size() > LayoutCapacity)) {
				break;
			}
			// The walks of the builds take a queue each from the pool, which the counts and the
			// checks of the preparation need too: the builds use up to half of it, the rest waits
			// for the next preparation (the first walk always goes, or a BLAS whose walk needs
			// a large queue would wait forever behind the gathers). A gathered build (see below)
			// does not walk, so a walk that does not fit does not hold back the gathers behind it
			// (the refit BLASes of the frame: peds and vehicles must not trace last frame's shape).
			const bool     gathered = gathers(&slot);
			const uint64_t queue =
			    gathered ? 0
			    : slot.leaf_count != 0
			        ? std::min<uint64_t>(uint64_t {slot.leaf_count} + slot.leaf_count / 2 + 64, PoolQueueMax)
			        : PoolQueueMax;
			m_build_queue.pop_back();
			if (pool_use != 0 && pool_use + queue > PoolCapacity / 2) {
				deferred.push_back(index);
				continue;
			}
			pool_use += queue;
			slot.needs_build = false;
			scratch += size;
			Job job {slot.blas_va, slot.root, index, slot.vertices0.address,
			         slot.vertices1.address, slot.entries.address, slot.capacity0,
			         slot.capacity1, 0, 0, 0, 0, 0};
			if (!slot.layout.empty()) {
				job.header_layout = slot.header_layout;
				job.layout        = m_layouts->BufferDeviceAddress() + uint64_t {layouts.size()} * 16;
				job.layout_count  = static_cast<uint32_t>(slot.layout.size());
				layouts.insert(layouts.end(), slot.layout.begin(), slot.layout.end());
			}
			jobs.push_back(job);
			slots.push_back(index);
		}
		// Back at the end of the queue, in their order (the oldest last).
		m_build_queue.insert(m_build_queue.end(), deferred.rbegin(), deferred.rend());
	}
	if (jobs.empty()) {
		return;
	}
	// A BLAS the guest only refit since the last host build keeps its triangles and their order:
	// its vertices are gathered again through its entry table instead of walking the guest BVH.
	// Gathers go after the walks; walked again now and then.
	uint32_t walk_count = 0;
	{
		std::vector<Job>      walk_jobs, gather_jobs;
		std::vector<uint32_t> walk_slots, gather_slots;
		for (size_t i = 0; i < jobs.size(); i++) {
			auto&      slot   = *m_slot_list[slots[i]];
			const bool gather = gathers(&slot);
			slot.gathers         = gather ? slot.gathers + 1 : 0;
			slot.built_refit_seq = m_refit_seq;
			(gather ? gather_jobs : walk_jobs).push_back(jobs[i]);
			(gather ? gather_slots : walk_slots).push_back(slots[i]);
		}
		walk_count = static_cast<uint32_t>(walk_jobs.size());
		jobs       = std::move(walk_jobs);
		slots      = std::move(walk_slots);
		jobs.insert(jobs.end(), gather_jobs.begin(), gather_jobs.end());
		slots.insert(slots.end(), gather_slots.begin(), gather_slots.end());
	}
	// The slot infos (state 0 until built) and table entries (slot + 1) of the walked slots
	// (gathered ones keep theirs: the gather reads their triangle counts).
	for (uint32_t i = 0; i < walk_count; i++) {
		const auto     index = slots[i];
		const auto&    slot = *m_slot_list[index];
		const SlotInfo info {.as_address  = slot.as_address,
		                     .entry_table = slot.entries.address,
		                     .capacity0   = slot.capacity0,
		                     .capacity1   = slot.capacity1,
		                     .blas_va     = slot.blas_va,
		                     .root        = slot.root,
		                     // The walk takes a queue of about as many nodes from the pool (0: as
		                     // many as the BLAS header allows).
		                     .leaf_count  = slot.leaf_count};
		UpdateBuffer(command, m_slots->Handle(), uint64_t {index} * SlotStride, &info, sizeof(info));
		// The walk may place other triangles: the alpha records start over.
		if (slot.capacity1 != 0) {
			command.fillBuffer(slot.entries.buffer->Handle(),
			                   slot.entries.offset + AlphaRecordsOffset(slot.capacity0, slot.capacity1),
			                   uint64_t {slot.capacity1} * 32, 0);
		}
		// A slot the host table holds has no entry in the walk's table.
		if (slot.table_index != UINT32_MAX) {
			const uint32_t encoded = index + 1;
			UpdateBuffer(command, m_table->Handle(), uint64_t {slot.table_index} * TableStride + 8,
			             &encoded, sizeof(encoded));
		}
		// A layout build writes only the positions its triangles take: the others stay inactive
		// (NaN vertices, entries of ~0).
		if (!slot.layout.empty()) {
			command.fillBuffer(slot.vertices0.buffer->Handle(), slot.vertices0.offset, slot.vertices0.size,
			                   0x7fc00000u);
			command.fillBuffer(slot.vertices1.buffer->Handle(), slot.vertices1.offset, slot.vertices1.size,
			                   0x7fc00000u);
			command.fillBuffer(slot.entries.buffer->Handle(), slot.entries.offset, slot.entries.size,
			                   0xffffffffu);
		}
	}
	for (size_t done = 0; done < layouts.size();) {
		const auto chunk = std::min<size_t>(layouts.size() - done, 4096);
		UpdateBuffer(command, m_layouts->Handle(), uint64_t {done} * 16, &layouts[done],
		             static_cast<uint32_t>(chunk * 16));
		done += chunk;
	}
	UpdateBuffer(command, m_jobs->Handle(), 0, jobs.data(),
	             static_cast<uint32_t>(jobs.size() * JobStride));
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
	              vk::PipelineStageFlagBits2::eComputeShader,
	              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
	const BlasPush push {m_jobs->BufferDeviceAddress(),    m_pool->BufferDeviceAddress(),
	                     m_slots->BufferDeviceAddress(),   m_state->BufferDeviceAddress(),
	                     m_changed->BufferDeviceAddress(), ModeBuild,
	                     PoolCapacity,                     walk_count,
	                     ChangedCapacity,                  m_prepare_id,
	                     m_blas_flags};
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_blas_pipeline);
	command.pushConstants(m_blas_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
	if (walk_count != jobs.size()) {
		auto gather      = push;
		gather.jobs      = m_jobs->BufferDeviceAddress() + uint64_t {walk_count} * JobStride;
		gather.mode      = ModeGather;
		gather.job_count = static_cast<uint32_t>(jobs.size()) - walk_count;
		// A workgroup per 1024 triangles of the largest job (one workgroup per job left the
		// thousands of triangles of a vehicle to 256 invocations in turn).
		uint32_t largest = 0;
		for (size_t i = walk_count; i < jobs.size(); i++) {
			largest = std::max(largest, jobs[i].capacity0 + jobs[i].capacity1);
		}
		const uint32_t groups = std::clamp<uint32_t>((largest + 1023u) / 1024u, 1u, 64u);
		command.pushConstants(m_blas_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(gather), &gather);
		command.dispatch(gather.job_count, groups, 1);
		command.pushConstants(m_blas_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
	}
	if (walk_count != 0) {
		command.dispatch(walk_count, 1, 1);
	}
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eComputeShader,
	              vk::AccessFlagBits2::eShaderWrite, AsBuildStage | vk::PipelineStageFlagBits2::eTransfer,
	              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead);

	// One build per slot, all in one command (distinct scratch ranges).
	struct BuildRecord {
		vk::AccelerationStructureKHR handle;
		uint64_t                     vertices0;
		uint64_t                     vertices1;
		uint64_t                     scratch;
		uint32_t                     capacity0;
		uint32_t                     capacity1;
		uint32_t                     fast;
		// 1: allows updates, 2: updates the last build.
		uint32_t                     update;
	};
	std::vector<BuildRecord> records;
	uint64_t                 scratch_offset = 0;
	const auto               scratch_base   = m_build_scratch->BufferDeviceAddress();
	for (size_t i = 0; i < slots.size(); i++) {
		auto&      slot     = *m_slot_list[slots[i]];
		const bool fast     = slot.dynamic_until != 0;
		const bool gathered = i >= walk_count;
		// A BLAS the guest refits (gathered now, or walked between gathers) allows updates; a
		// gathered build updates the last build when it has the same flags (fast or not).
		const bool     updatable = slot.refit_seq == m_refit_seq;
		const uint32_t update =
		    gathered && updatable && slot.updatable && slot.fast_built == fast ? 2u : updatable ? 1u : 0u;
		slot.fast_built = fast;
		slot.updatable  = updatable;
		records.push_back({slot.handle, slot.vertices0.address, slot.vertices1.address,
		                   scratch_base + scratch_offset, slot.capacity0, slot.capacity1,
		                   fast ? 1u : 0u, update});
		scratch_offset += (slot.build_scratch + Alignment - 1) & ~(Alignment - 1);
	}
	const auto* copy  = command.CopyArray(records.data(), records.size());
	const auto  count = static_cast<uint32_t>(records.size());
	command.Custom([copy, count](vk::CommandBuffer c) {
		std::vector<vk::AccelerationStructureGeometryKHR>              geometries(count * 2);
		std::vector<vk::AccelerationStructureBuildGeometryInfoKHR>     infos(count);
		std::vector<vk::AccelerationStructureBuildRangeInfoKHR>        ranges(count * 2);
		std::vector<const vk::AccelerationStructureBuildRangeInfoKHR*> range_pointers(count);
		for (uint32_t i = 0; i < count; i++) {
			geometries[i * 2]     = TriangleGeometry(copy[i].vertices0, copy[i].capacity0, true);
			geometries[i * 2 + 1] = TriangleGeometry(copy[i].vertices1, copy[i].capacity1, false);
			auto& info                     = infos[i];
			info.type                      = vk::AccelerationStructureTypeKHR::eBottomLevel;
			info.flags = copy[i].fast != 0 ? (copy[i].update != 0 ? FastUpdatableBlasFlags : FastBlasFlags)
			             : copy[i].update != 0 ? UpdatableBlasFlags
			                                   : BlasFlags;
			info.mode  = copy[i].update == 2 ? vk::BuildAccelerationStructureModeKHR::eUpdate
			                                 : vk::BuildAccelerationStructureModeKHR::eBuild;
			info.srcAccelerationStructure  = copy[i].update == 2 ? copy[i].handle : nullptr;
			info.dstAccelerationStructure  = copy[i].handle;
			info.geometryCount             = 2;
			info.pGeometries               = &geometries[i * 2];
			info.scratchData.deviceAddress = copy[i].scratch;
			ranges[i * 2].primitiveCount     = copy[i].capacity0;
			ranges[i * 2 + 1].primitiveCount = copy[i].capacity1;
			range_pointers[i]                = &ranges[i * 2];
		}
		c.buildAccelerationStructuresKHR(count, infos.data(), range_pointers.data());
	});
	MemoryBarrier(command, AsBuildStage, vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
	              AsBuildStage | vk::PipelineStageFlagBits2::eComputeShader |
	                  vk::PipelineStageFlagBits2::eTransfer,
	              vk::AccessFlagBits2::eAccelerationStructureReadKHR | vk::AccessFlagBits2::eShaderRead |
	                  vk::AccessFlagBits2::eAccelerationStructureWriteKHR |
	                  vk::AccessFlagBits2::eTransferWrite);
	// Built: the TLAS walk may reference them.
	constexpr uint32_t built = 1;
	for (const auto index: slots) {
		UpdateBuffer(command, m_slots->Handle(), uint64_t {index} * SlotStride + offsetof(SlotInfo, state),
		             &built, sizeof(built));
		m_slot_list[index]->built       = true;
		m_slot_list[index]->built_frame = m_frame;
	}
	readback.build_count = static_cast<uint32_t>(slots.size());
	readback.build_slots = std::move(slots);
}

void NativeRayTracing::Prepare(const CommandRecorder& command, uint64_t user_pointer,
                               const TlasSource& source, const vk::DescriptorBufferInfo& flattened_srt,
                               uint64_t frame, uint64_t shader_hash) {
	if (!m_enabled) {
		return;
	}
	g_rt_stats.native.Add();
	auto& readback = m_readbacks[m_readback_index];
	if (readback.tick != 0) {
		// The ring is full: the oldest readback must complete first (rare).
		m_scheduler.Wait(readback.tick);
		m_scheduler.PopPendingOperations();
	}
	readback.build_count = 0;
	readback.build_slots.clear();
	const auto source_key = source.user_sgpr << 16u | source.header_offset;
	readback.source_key   = source_key;
	readback.frame        = frame;
	readback.shader_hash  = shader_hash;
	uint32_t              tlas_count = 0;
	std::vector<uint32_t> retry;
	// Slots released this preparation (index and table entry), and slots that got new memory
	// (their infos must not point at the old structure any more).
	std::vector<std::pair<uint32_t, uint32_t>> evicted;
	std::vector<std::pair<uint32_t, SlotInfo>> reallocated;
	std::vector<uint32_t>                      moved;
	std::vector<std::tuple<uint32_t, uint64_t, uint32_t>> host_writes;
	{
		Common::LockGuard lock(m_mutex);
		m_frame = frame;
		DestroyRetired();
		for (const auto index: m_give_up) {
			if (index < m_slot_list.size() && m_slot_list[index] != nullptr && !m_slot_list[index]->evicted) {
				evicted.push_back({index, m_slot_list[index]->table_index});
				EvictSlot(index);
			}
		}
		m_give_up.clear();
		moved.insert(moved.end(), m_moved_slots.begin(), m_moved_slots.end());
		m_moved_slots.clear();
		for (const auto& pending: m_new_slots) {
			if (!CreateSlot(pending)) {
				// No free slot: reported again later (a few times).
				ReportAgain(pending.blas_va, pending.root, pending.table_index);
			}
		}
		m_new_slots.clear();
		for (const auto& [index, opaque, alpha]: m_grow) {
			if (m_slot_list[index] == nullptr || m_slot_list[index]->evicted) {
				continue;
			}
			auto& slot = *m_slot_list[index];
			// An implausible or too large count (see ComputeSlotSizes) keeps the slot as it is:
			// its builds drop the triangles that do not fit.
			SlotSizes sizes;
			if (!ComputeSlotSizes(opaque, alpha, sizes)) {
				continue;
			}
			// The recorded work may still use the old structure: it is destroyed later.
			RetireResources(slot, UINT32_MAX);
			AllocateSlot(slot, sizes);
			slot.leaf_count  = 0;
			slot.needs_build = true;
			m_build_queue.push_back(index);
			moved.push_back(index);
		}
		m_grow.clear();
		if (m_prepare_id - m_last_scan_prepare >= EvictScanPrepares) {
			m_last_scan_prepare = m_prepare_id;
			std::vector<uint32_t> indices;
			EvictUnused(indices);
			for (const auto index: indices) {
				evicted.push_back({index, m_slot_list[index]->table_index});
			}
			Defragment(moved);
		}
		for (const auto index: moved) {
			const auto& slot = *m_slot_list[index];
			if (slot.evicted) {
				// Released by the scan above after it got new memory.
				continue;
			}
			reallocated.push_back({index, SlotInfo {.as_address  = slot.as_address,
			                                        .entry_table = slot.entries.address,
			                                        .capacity0   = slot.capacity0,
			                                        .capacity1   = slot.capacity1,
			                                        .blas_va     = slot.blas_va,
			                                        .root        = slot.root}});
		}
		std::erase_if(m_late_retries, [&](const std::pair<uint64_t, uint32_t>& late) {
			if (late.first > m_prepare_id) {
				return false;
			}
			m_retry.push_back(late.second);
			return true;
		});
		retry.swap(m_retry);
		host_writes.swap(m_host_table_writes);
		// Slots the guest changed lately are rebuilt before each use: the verification finds a
		// change only after the TLAS of that preparation was built.
		std::erase_if(m_dynamic, [&](uint32_t index) {
			auto* released = m_slot_list[index].get();
			if (released == nullptr || released->evicted) {
				return true;
			}
			auto& slot = *released;
			if (slot.dynamic_until <= m_prepare_id) {
				slot.dynamic_until = 0;
				// No longer changing: built once more for tracing.
				if (slot.fast_built && !slot.needs_build) {
					slot.needs_build = true;
					m_build_queue.push_back(index);
				}
				return true;
			}
			// Before every preparation, not once a frame: the guest may change it between the ray
			// tracing passes of a frame.
			if (!slot.needs_build) {
				slot.needs_build = true;
				m_build_queue.push_back(index);
			}
			return false;
		});
		// The count of the last walk of the source, or of the TLAS the CPU read for this dispatch
		// (NoteTlasInstances()) when larger: the first walks of a new one got a host TLAS of the
		// size of the source's previous one (ASTRO BOT's island in Go-Go Archipelago: 1024 of its
		// 4242 instances, in a pass of 64 dispatches in one frame).
		const auto last = std::max(m_last_instance_counts[source_key], std::exchange(m_next_instances, 0u));
		tlas_count      = std::min(m_tlas_capacity, std::max(1024u, (last + last / 4 + 255u) &
		                                                           ~255u));
	}
	m_prepare_id++;

	// Earlier guest work wrote the BVHs; earlier ray queries read the TLAS this rebuilds.
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eAllCommands,
	              vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
	              vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer |
	                  AsBuildStage,
	              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
	                  vk::AccessFlagBits2::eTransferWrite |
	                  vk::AccessFlagBits2::eAccelerationStructureWriteKHR);
	if (!m_cleared) {
		// The buffers start clear (empty table, invalid contexts).
		m_cleared = true;
		for (const auto* buffer: {m_table.get(), m_slots.get(), m_context.get()}) {
			command.fillBuffer(buffer->Handle(), 0, buffer->Size(), 0);
		}
		// Patched shaders find the context through the BDA page table.
		UpdateBuffer(command, m_buffer_cache.GetBdaPageTableBuffer()->Handle(),
		             BufferCache::NATIVE_RT_CONTEXT_PAGE * sizeof(uint64_t), &m_context_address,
		             sizeof(m_context_address));
	}
	// Table entries to forget become tombstones: the probe chains through them stay intact and
	// the walk reports the BLAS again (or reuses the entry for another key).
	const auto forget = [&](uint32_t table_index) {
		const std::array<uint64_t, 2> tombstone {TableTombstone, 0};
		UpdateBuffer(command, m_table->Handle(), uint64_t {table_index} * TableStride, tombstone.data(),
		             sizeof(tombstone));
	};
	for (const auto index: retry) {
		forget(index);
	}
	// Released slots: no table entry finds them and no TLAS may reference them (state 0).
	for (const auto& [index, table_index]: evicted) {
		constexpr SlotInfo cleared {};
		UpdateBuffer(command, m_slots->Handle(), uint64_t {index} * SlotStride, &cleared, sizeof(cleared));
		if (table_index != UINT32_MAX) {
			forget(table_index);
		}
	}
	// The host table, after the evictions released their entries.
	for (const auto& [index, key, encoded]: host_writes) {
		const std::array<uint32_t, 4> entry {static_cast<uint32_t>(key), static_cast<uint32_t>(key >> 32u),
		                                     encoded, 0};
		UpdateBuffer(command, m_table->Handle(), uint64_t {TableEntries + index} * TableStride, entry.data(),
		             sizeof(entry));
	}
	for (const auto& [index, info]: reallocated) {
		UpdateBuffer(command, m_slots->Handle(), uint64_t {index} * SlotStride, &info, sizeof(info));
	}
	command.fillBuffer(m_state->Handle(), 0, StateSize, 0);
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
	              vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
	              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
	                  vk::AccessFlagBits2::eTransferWrite);
	// The header address as the guest's own load reads it, from the flattened SRT (its user data,
	// read on the CPU, with the dwords the GPU wrote copied in), into the state the walk starts
	// from. Through the BDA page table the walk would read a cache buffer over that memory,
	// synchronized only when something binds it, or no buffer at all (zero, Astro Bot).
	const auto in_srt = [&](uint32_t slot) {
		return slot != ShaderRecompiler::IR::ShaderInfo::NoRayQuerySlot &&
		       (uint64_t {slot} + 1) * sizeof(uint32_t) <= flattened_srt.range;
	};
	const bool header_in_state = flattened_srt.buffer && in_srt(source.header_slots[0]) &&
	                             in_srt(source.header_slots[1]);
	if (header_in_state) {
		const std::array<vk::BufferCopy, 2> words {{
		    {flattened_srt.offset + uint64_t {source.header_slots[0]} * sizeof(uint32_t),
		     offsetof(RtState, header_va), sizeof(uint32_t)},
		    {flattened_srt.offset + uint64_t {source.header_slots[1]} * sizeof(uint32_t),
		     offsetof(RtState, header_va) + sizeof(uint32_t), sizeof(uint32_t)},
		}};
		command.copyBuffer(flattened_srt.buffer, m_state->Handle(), static_cast<uint32_t>(words.size()),
		                   words.data());
		MemoryBarrier(command, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
		              vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
		              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
		                  vk::AccessFlagBits2::eTransferWrite);
	}

	// The walks record the pages without a buffer that they read in the fault buffer, which must
	// start clear (the program's own device-address accesses usually cleared it already).
	m_buffer_cache.PrepareFaultBuffer();
	auto*       page_table   = m_buffer_cache.GetBdaPageTableBuffer();
	const auto* fault_buffer = m_buffer_cache.GetFaultBuffer();
	const std::array<vk::DescriptorBufferInfo, WalkBindings> infos {{
	    {page_table->Handle(), 0, page_table->Size()},
	    {fault_buffer->Handle(), 0, BufferCache::WRITE_BITMAP_OFFSET},
	}};
	std::array<vk::WriteDescriptorSet, WalkBindings> writes {};
	for (uint32_t i = 0; i < WalkBindings; i++) {
		writes[i].dstBinding      = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[i].pBufferInfo     = &infos[i];
	}
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_blas_layout, 0, WalkBindings,
	                             writes.data());

	// The BVHs of the traversal's layout (Decoder::RayTracingLayout::Pack() word 3).
	m_blas_flags = ((source.layout[3] >> 29u) & 1u) != 0 ? BlasNodeCountAt80 : 0u;
	RecordBuilds(command);

	command.fillBuffer(m_instances->Handle(), 0, uint64_t {tlas_count} * InstanceStride, 0);
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
	              vk::PipelineStageFlagBits2::eComputeShader,
	              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_tlas_layout, 0, WalkBindings,
	                             writes.data());

	const TlasPush tlas_push {
	    .user_ptr_va       = user_pointer,
	    .state             = m_state->BufferDeviceAddress(),
	    .queue             = m_queue->BufferDeviceAddress(),
	    .instance_nodes    = m_instance_nodes->BufferDeviceAddress(),
	    .instances         = m_instances->BufferDeviceAddress(),
	    .table             = m_table->BufferDeviceAddress(),
	    .slots             = m_slots->BufferDeviceAddress(),
	    .missing           = m_missing->BufferDeviceAddress(),
	    .context           = m_context_address,
	    .tlas_address      = m_tlas_address,
	    .verify_list       = m_verify_list->BufferDeviceAddress(),
	    .header_offset     = header_in_state ? HeaderInState : source.header_offset,
	    .root              = source.root,
	    .instance_capacity = tlas_count,
	    .table_mask        = TableEntries - 1,
	    .prepare_id        = m_prepare_id,
	    .verify            = 1u,
	    // The facing of each instance follows the game's triangle test (the layout's flip bit: GTA
	    // V 01.011's accepts, with flags bit 3 clear, the triangles whose test sign is set, the
	    // host's back faces; not flipping them culled the wrong face of every instance without both
	    // faces, and single sided geometry such as peds, roofs and walls cast no shadow).
	    .layout            = source.layout,
	};
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_tlas_pipeline);
	command.pushConstants(m_tlas_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(tlas_push),
	                      &tlas_push);
	command.dispatch(1, 1, 1);
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
	              vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eDrawIndirect |
	                  AsBuildStage,
	              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
	                  vk::AccessFlagBits2::eIndirectCommandRead);

	// Counts the leaves of the BLASes the walk found missing, and checks the built ones it uses.
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_blas_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_blas_layout, 0, WalkBindings,
	                             writes.data());
	BlasPush blas_push {m_missing->BufferDeviceAddress(), m_pool->BufferDeviceAddress(),
	                    m_slots->BufferDeviceAddress(),   m_state->BufferDeviceAddress(),
	                    m_changed->BufferDeviceAddress(), ModeCount,
	                    PoolCapacity,                     0,
	                    ChangedCapacity,                  m_prepare_id,
	                    m_blas_flags};
	command.pushConstants(m_blas_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(blas_push),
	                      &blas_push);
	command.dispatchIndirect(m_state->Handle(), StateDispatch);
	blas_push.jobs = m_verify_list->BufferDeviceAddress();
	blas_push.mode = ModeVerify;
	command.pushConstants(m_blas_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(blas_push),
	                      &blas_push);
	command.dispatchIndirect(m_state->Handle(), StateVerify);

	// The TLAS over the walk's instances (inactive ones have a null BLAS).
	{
		const auto handle    = m_tlas;
		const auto instances = m_instances->BufferDeviceAddress();
		const auto scratch   = m_build_scratch->BufferDeviceAddress();
		command.Custom([handle, instances, scratch, tlas_count](vk::CommandBuffer c) {
			vk::AccelerationStructureGeometryKHR geometry {};
			geometry.geometryType = vk::GeometryTypeKHR::eInstances;
			geometry.geometry.instances.sType =
			    vk::StructureType::eAccelerationStructureGeometryInstancesDataKHR;
			geometry.geometry.instances.arrayOfPointers    = VK_FALSE;
			geometry.geometry.instances.data.deviceAddress = instances;
			vk::AccelerationStructureBuildGeometryInfoKHR info {};
			info.type                      = vk::AccelerationStructureTypeKHR::eTopLevel;
			info.flags                     = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
			info.mode                      = vk::BuildAccelerationStructureModeKHR::eBuild;
			info.dstAccelerationStructure  = handle;
			info.geometryCount             = 1;
			info.pGeometries               = &geometry;
			info.scratchData.deviceAddress = scratch;
			vk::AccelerationStructureBuildRangeInfoKHR range {};
			range.primitiveCount      = tlas_count;
			const auto* range_pointer = &range;
			c.buildAccelerationStructuresKHR(1, &info, &range_pointer);
		});
	}
	MemoryBarrier(command, AsBuildStage | vk::PipelineStageFlagBits2::eComputeShader,
	              vk::AccessFlagBits2::eAccelerationStructureWriteKHR | vk::AccessFlagBits2::eShaderWrite,
	              vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
	              vk::AccessFlagBits2::eAccelerationStructureReadKHR | vk::AccessFlagBits2::eShaderRead |
	                  vk::AccessFlagBits2::eTransferRead);

	// Reads back the state, the counted missing BLASes, the build results and the changed slots.
	const std::array<vk::BufferCopy, 5> regions {{
	    {0, 0, StateSize},
	    {0, ReadbackMissing, uint64_t {MissingCapacity} * MissingStride},
	    {0, ReadbackJobs, uint64_t {MaxJobs} * JobStride},
	    {0, ReadbackChanged, uint64_t {ChangedCapacity} * 4},
	    {0, ReadbackUsed, uint64_t {SlotCapacity} * 4},
	}};
	command.copyBuffer(m_state->Handle(), readback.buffer->Handle(), 1, &regions[0]);
	command.copyBuffer(m_missing->Handle(), readback.buffer->Handle(), 1, &regions[1]);
	command.copyBuffer(m_jobs->Handle(), readback.buffer->Handle(), 1, &regions[2]);
	command.copyBuffer(m_changed->Handle(), readback.buffer->Handle(), 1, &regions[3]);
	command.copyBuffer(m_verify_list->Handle(), readback.buffer->Handle(), 1, &regions[4]);
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
	              vk::PipelineStageFlagBits2::eAllCommands,
	              vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
	readback.tick = m_scheduler.CurrentTick();
	m_scheduler.DeferHostOperation([this, &readback] { ProcessReadback(readback); });
	m_readback_index = (m_readback_index + 1) % m_readbacks.size();
}

void NativeRayTracing::ProcessReadback(Readback& readback) {
	readback.buffer->Invalidate(0, ReadbackSize);
	const auto* bytes = readback.buffer->Mapped().data();
	RtState     state {};
	std::memcpy(&state, bytes, sizeof(state));
	const auto               missing_count = std::min(state.missing_count, MissingCapacity);
	std::vector<PendingSlot> pending;
	std::vector<uint32_t>    retry;
	Common::LockGuard        lock(m_mutex);
	// The walk ends without publishing the context (nor the indirect BLAS dispatch) when the header
	// is not that of a TLAS; it publishes what it read when part of the TLAS is not cached yet.
	if (state.dispatch[1] == 0) {
		(state.header_va == 0 ? g_rt_stats.walks_null_header : g_rt_stats.walks_bad_header).Add();
		g_rt_stats.last_unpublished.store(readback.shader_hash, std::memory_order_relaxed);
	} else if ((state.overflow & StateUnmapped) != 0) {
		g_rt_stats.walks_uncached.Add();
		g_rt_stats.last_uncached.store(readback.shader_hash, std::memory_order_relaxed);
	}
	if ((state.overflow & StateFull) != 0) {
		g_rt_stats.walks_overflow.Add();
		g_rt_stats.last_overflow.store(readback.shader_hash, std::memory_order_relaxed);
	}
	SummarizeRayTracing();
	for (uint32_t i = 0; i < missing_count; i++) {
		MissingEntry entry {};
		std::memcpy(&entry, bytes + ReadbackMissing + uint64_t {i} * MissingStride, sizeof(entry));
		if (entry.flags != 0 || entry.leaf_count == 0) {
			// Not countable now (memory not written yet, a too large BLAS): forget the entry so
			// the walk reports it again, a few times.
			ReportAgain(entry.blas_va, entry.root, entry.table_index);
			continue;
		}
		m_new_slots.push_back({entry.blas_va, entry.root, entry.table_index, entry.leaf_count,
		                       entry.opaque_count, entry.alpha_count});
	}
	for (uint32_t i = 0; i < readback.build_count && i < readback.build_slots.size(); i++) {
		Job job {};
		std::memcpy(&job, bytes + ReadbackJobs + uint64_t {i} * JobStride, sizeof(job));
		if ((job.result & 1u) == 0 && ((job.result >> 1u) & 5u) != 0) {
			// The walk found no room: in the pool (other jobs of the preparation took it) or in
			// its queue (the BLAS has more nodes than when it was counted). The slot misses
			// triangles: built again, after a queue overflow with a queue of the size the BLAS
			// header allows.
			const auto index = readback.build_slots[i];
			if (m_slot_list[index] != nullptr && !m_slot_list[index]->evicted) {
				auto& slot = *m_slot_list[index];
				if (((job.result >> 1u) & 1u) != 0) {
					slot.leaf_count = 0;
				}
				if (!slot.needs_build) {
					slot.needs_build = true;
					m_build_queue.push_back(index);
				}
			}
		}
		if ((job.result & (JobLayoutMismatch | JobLayoutStale)) != 0) {
			// The guest BLAS is not (or no longer) laid out as its build said: given up, the walks
			// find what the memory holds. A later build there is laid out again.
			const auto index = readback.build_slots[i];
			if (m_slot_list[index] != nullptr && !m_slot_list[index]->evicted &&
			    !m_slot_list[index]->layout.empty()) {
				m_give_up.push_back(index);
			}
			continue;
		}
		if (job.leaves != 0 && (job.result & 0xfu) == 0) {
			// A complete walk: the next one takes a queue of about as many nodes (a slot that
			// grew or overflowed its queue walked with the largest one).
			const auto index = readback.build_slots[i];
			if (m_slot_list[index] != nullptr && !m_slot_list[index]->evicted) {
				m_slot_list[index]->leaf_count = job.leaves;
			}
		}
		if ((job.result & 0xfu) == 0 && job.layout == 0) {
			// A complete walk of a slot with far more room than the BLAS has (a decompressed BLAS
			// sized from its memory): it gets its exact size, like a grown slot.
			const auto index = readback.build_slots[i];
			if (m_slot_list[index] != nullptr && !m_slot_list[index]->evicted) {
				const auto& slot = *m_slot_list[index];
				if (slot.capacity0 > job.capacity0 * 2 + 1024 || slot.capacity1 > job.capacity1 * 2 + 1024) {
					m_grow.push_back({index, job.capacity0, job.capacity1});
				}
			}
		}
		if ((job.result & 1u) != 0) {
			// More triangles than the slot holds (the guest rebuilt the BLAS bigger): the job
			// came back with the counts. A slot released since needs nothing.
			const auto index = readback.build_slots[i];
			if (m_slot_list[index] != nullptr && !m_slot_list[index]->evicted) {
				m_grow.push_back({index, job.capacity0, job.capacity1});
			}
		}
	}
	const auto changed = std::min(state.changed_count, ChangedCapacity);
	for (uint32_t i = 0; i < changed; i++) {
		uint32_t index = 0;
		std::memcpy(&index, bytes + ReadbackChanged + uint64_t {i} * 4, sizeof(index));
		if (index >= m_slot_list.size() || m_slot_list[index] == nullptr ||
		    m_slot_list[index]->evicted) {
			continue;
		}
		auto& slot = *m_slot_list[index];
		if (slot.dynamic_until == 0) {
			m_dynamic.push_back(index);
		}
		slot.dynamic_until = m_prepare_id + DynamicPrepares;
		if (!slot.needs_build) {
			slot.needs_build = true;
			m_build_queue.push_back(index);
		}
	}
	// The slots the TLAS referenced were used in the preparation's frame.
	const auto used = std::min(state.verify_count, SlotCapacity);
	for (uint32_t i = 0; i < used; i++) {
		uint32_t index = 0;
		std::memcpy(&index, bytes + ReadbackUsed + uint64_t {i} * 4, sizeof(index));
		if (index < m_slot_list.size() && m_slot_list[index] != nullptr &&
		    !m_slot_list[index]->evicted) {
			auto& slot          = *m_slot_list[index];
			slot.last_use_frame = std::max(slot.last_use_frame, readback.frame);
		}
	}
	m_last_instance_counts[readback.source_key] = state.instance_count;
	readback.tick = 0;
}

uint32_t NativeRayTracing::HleTlasHeader(uint32_t count, HleTlasFormat format,
                                         std::array<uint32_t, 32>& header) noexcept {
	// Larger lists keep the game's own build (CommandProcessor::HleTlasDispatch()).
	count          = std::min(count, HleTlasMaxInstances);
	uint32_t nodes = 0;
	for (uint32_t n = std::max((count + 3u) / 4u, 1u);; n = (n + 3u) / 4u) {
		nodes += n;
		if (n == 1u) {
			break;
		}
	}
	const uint32_t instance_offset = 256u + nodes * 128u;
	header                         = {};
	header[0]                      = 0x5f525350u; // "PSR_BVHL"
	header[1]                      = 0x4c485642u;
	header[4]                      = instance_offset + count * 128u;
	if (format == HleTlasFormat::Base252) {
		header[2]  = 0xbbe6e938u;
		header[6]  = 0x24u;
		header[10] = instance_offset;
		header[20] = count + 4u;
		header[29] = count;
	} else {
		header[2]  = 0x5d1cba8cu;
		header[6]  = 0x00020024u;
		header[12] = instance_offset;
		header[22] = count + 4u;
		header[28] = count;
	}
	return header[4];
}

void NativeRayTracing::RecordHleTlas(const CommandRecorder& command, uint64_t input_va,
                                     uint32_t count, uint32_t stride, uint64_t header_va,
                                     HleTlasFormat format) {
	if (!m_enabled || header_va == 0 || (count != 0 && input_va == 0)) {
		return;
	}
	// The game's CPU wrote the list; earlier GPU work wrote the BLASes the build reads and read
	// the TLAS it replaces.
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eAllCommands,
	              vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
	              vk::PipelineStageFlagBits2::eComputeShader,
	              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
	auto* page_table = m_buffer_cache.GetBdaPageTableBuffer();
	const vk::DescriptorBufferInfo page_info {page_table->Handle(), 0, page_table->Size()};
	vk::WriteDescriptorSet write {};
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eStorageBuffer;
	write.pBufferInfo     = &page_info;
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_hle_tlas_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_hle_tlas_layout, 0, 1, &write);
	const HleTlasPush push {input_va,
	                        header_va,
	                        m_hle_tlas_scratch->BufferDeviceAddress(),
	                        count,
	                        stride,
	                        static_cast<uint32_t>(format),
	                        0};
	command.pushConstants(m_hle_tlas_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
	command.dispatch(1, 1, 1);
	MemoryBarrier(command, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
	              vk::PipelineStageFlagBits2::eAllCommands,
	              vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
}

void NativeRayTracing::NoteGuestBlasBuilds(std::span<const GuestBlasBuild> builds) {
	if (!m_enabled) {
		return;
	}
	Common::LockGuard lock(m_mutex);
	for (const auto& build: builds) {
		if (build.blas_va == 0 || build.geometries.empty()) {
			continue;
		}
		// Each geometry's triangles follow those of the earlier geometries of its opacity.
		std::vector<std::array<uint32_t, 4>> layout;
		std::array<uint64_t, 2>              totals {};
		for (const auto& [triangles, opaque]: build.geometries) {
			auto& total = totals[opaque ? 0 : 1];
			layout.push_back({static_cast<uint32_t>(total), triangles, opaque ? 1u : 0u, 0u});
			total += triangles;
		}
		if (totals[0] + totals[1] == 0 || totals[0] + totals[1] > MaxSlotTriangles) {
			continue;
		}
		const auto capacity0 = static_cast<uint32_t>(std::max<uint64_t>(totals[0], 1));
		const auto capacity1 = static_cast<uint32_t>(std::max<uint64_t>(totals[1], 1));
		// A BLAS with a slot already (built before, or found by a walk): laid out again.
		uint32_t index = UINT32_MAX;
		if (const auto it = m_slots_by_blas.find(build.blas_va); it != m_slots_by_blas.end()) {
			for (const auto candidate: it->second) {
				if (m_slot_list[candidate]->root == GuestBlasRoot) {
					index = candidate;
				}
			}
		}
		if (index != UINT32_MAX) {
			auto& slot = *m_slot_list[index];
			if (slot.capacity0 != capacity0 || slot.capacity1 != capacity1) {
				SlotSizes sizes;
				if (!ComputeSlotSizes(capacity0, capacity1, sizes, true)) {
					continue;
				}
				// The recorded work may still use the old structure: it is destroyed later.
				RetireResources(slot, UINT32_MAX);
				AllocateSlot(slot, sizes);
				m_moved_slots.push_back(index);
			}
			slot.layout        = std::move(layout);
			slot.header_layout = build.header_layout;
			// Leaves hold one triangle or more: the walk's queue is sized for as many (a walk
			// that needs more reports it, see RecordBuilds()).
			slot.leaf_count = static_cast<uint32_t>(totals[0] + totals[1]);
			if (!slot.needs_build) {
				slot.needs_build = true;
				m_build_queue.push_back(index);
			}
			continue;
		}
		SlotSizes sizes;
		if (!ComputeSlotSizes(capacity0, capacity1, sizes, true)) {
			continue;
		}
		if (const auto created = CreateHostSlot(build.blas_va, sizes); created != UINT32_MAX) {
			auto& slot         = *m_slot_list[created];
			slot.layout        = std::move(layout);
			slot.header_layout = build.header_layout;
			slot.leaf_count    = static_cast<uint32_t>(totals[0] + totals[1]);
		}
	}
}

uint32_t NativeRayTracing::FindHostSlotIndex(uint64_t blas_va) const {
	if (const auto it = m_slots_by_blas.find(blas_va); it != m_slots_by_blas.end()) {
		for (const auto candidate: it->second) {
			if (m_slot_list[candidate]->root == GuestBlasRoot) {
				return candidate;
			}
		}
	}
	return UINT32_MAX;
}

uint32_t NativeRayTracing::CreateHostSlot(uint64_t blas_va, const SlotSizes& sizes) {
	// A host table entry: the first free (or released) one of the key's probe chain.
	const uint64_t key  = blas_va ^ (uint64_t {GuestBlasRoot} << 48u);
	const uint32_t hash = TableHash(key);
	uint32_t       host = UINT32_MAX;
	for (uint32_t probe = 0; probe < HostTableProbes; probe++) {
		const uint32_t candidate = (hash + probe) & (HostTableEntries - 1);
		if (m_host_keys[candidate] == 0 || m_host_keys[candidate] == TableTombstone) {
			host = candidate;
			break;
		}
	}
	if (host == UINT32_MAX) {
		return UINT32_MAX;
	}
	uint32_t index = UINT32_MAX;
	if (!m_free_slots.empty()) {
		index = m_free_slots.back();
		m_free_slots.pop_back();
	} else if (m_slot_list.size() < SlotCapacity) {
		index = static_cast<uint32_t>(m_slot_list.size());
		m_slot_list.emplace_back();
	} else {
		return UINT32_MAX;
	}
	auto slot              = std::make_unique<Slot>();
	slot->blas_va          = blas_va;
	slot->root             = GuestBlasRoot;
	slot->table_index      = UINT32_MAX;
	slot->host_table_index = host;
	slot->last_use_frame   = m_frame;
	AllocateSlot(*slot, sizes);
	slot->needs_build = true;
	m_slots_by_blas[blas_va].push_back(index);
	m_build_queue.push_back(index);
	m_slot_list[index] = std::move(slot);
	m_host_keys[host]  = key;
	m_host_table_writes.emplace_back(host, key, index + 1);
	return index;
}

void NativeRayTracing::NoteGuestBlasDecompressed(uint64_t blas_va, uint64_t memory_size) {
	if (!m_enabled || blas_va == 0) {
		return;
	}
	Common::LockGuard lock(m_mutex);
	if (const auto index = FindHostSlotIndex(blas_va); index != UINT32_MAX) {
		// The memory holds another BLAS now: no layout and no refits of the old one; the walk
		// builds it again (and grows the slot if needed).
		auto& slot     = *m_slot_list[index];
		slot.layout.clear();
		slot.refit_seq = 0;
		if (!slot.needs_build) {
			slot.needs_build = true;
			m_build_queue.push_back(index);
		}
		return;
	}
	// Its triangle counts are not known before the decompression ran on the GPU: the slot takes
	// as many triangles of each opacity as the BLAS memory can hold (a triangle takes more than
	// DecompressBytesPerTriangle bytes of it), and its first build gives it its exact size.
	const auto bound = static_cast<uint32_t>(
	    std::clamp<uint64_t>(memory_size / DecompressBytesPerTriangle, 1, MaxSlotTriangles / 2));
	SlotSizes sizes;
	if (memory_size == 0 || !ComputeSlotSizes(bound, bound, sizes, true)) {
		return;
	}
	if (const auto created = CreateHostSlot(blas_va, sizes); created != UINT32_MAX) {
		m_slot_list[created]->leaf_count = bound;
	}
}

void NativeRayTracing::RecordHleRefit(const CommandRecorder& command, std::span<const uint64_t> blases,
                                      std::span<const HleRefitGeometry> geometries, uint32_t header_layout,
                                      bool arguments) {
	if (!m_enabled || blases.empty()) {
		return;
	}
	static_assert(sizeof(HleRefitGeometry) == HleRefitGeometryStride);
	if (!arguments) {
		Common::LockGuard lock(m_mutex);
		m_refit_seq++;
		for (const auto blas: blases) {
			if (const auto it = m_slots_by_blas.find(blas); it != m_slots_by_blas.end()) {
				for (const auto index: it->second) {
					// Built again (gathered) at the next preparation, so the frame that refit it
					// traces the new shape.
					auto& slot     = *m_slot_list[index];
					slot.refit_seq = m_refit_seq;
					if (slot.built && !slot.needs_build) {
						slot.needs_build = true;
						m_build_queue.push_back(index);
					}
				}
			}
		}
	}
	const auto upload = [&](uint64_t offset, const uint8_t* data, uint64_t size) {
		for (uint64_t done = 0; done < size;) {
			const auto chunk = static_cast<uint32_t>(std::min<uint64_t>(size - done, 65536));
			UpdateBuffer(command, m_hle_jobs->Handle(), offset + done, data + done, chunk);
			done += chunk;
		}
	};
	auto* page_table = m_buffer_cache.GetBdaPageTableBuffer();
	const vk::DescriptorBufferInfo page_info {page_table->Handle(), 0, page_table->Size()};
	vk::WriteDescriptorSet write {};
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eStorageBuffer;
	write.pBufferInfo     = &page_info;
	const auto jobs = m_hle_jobs->BufferDeviceAddress();
	// One pass of the refit with the jobs of `batch_geometries` and `batch_blases` in the job buffer:
	// the vertices of the geometries, then the boxes of the BLASes.
	const auto pass = [&](std::span<const HleRefitGeometry> batch_geometries, std::span<const uint64_t> batch_blases) {
		const uint64_t geometry_bytes = uint64_t {HleRefitGeometryStride} * batch_geometries.size();
		const uint64_t blas_offset    = (geometry_bytes + 255u) & ~uint64_t {255};
		const uint64_t blas_bytes     = uint64_t {HleRefitBlasStride} * batch_blases.size();
		// Earlier work read the jobs of the last pass and wrote what this one reads.
		MemoryBarrier(command, vk::PipelineStageFlagBits2::eAllCommands,
		              vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
		              vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eComputeShader,
		              vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eShaderRead |
		                  vk::AccessFlagBits2::eShaderWrite);
		if (geometry_bytes != 0) {
			upload(0, reinterpret_cast<const uint8_t*>(batch_geometries.data()), geometry_bytes);
		}
		if (blas_bytes != 0) {
			std::vector<uint64_t> blas_jobs(batch_blases.size() * 2);
			for (size_t i = 0; i < batch_blases.size(); i++) {
				blas_jobs[i * 2] = batch_blases[i];
			}
			upload(blas_offset, reinterpret_cast<const uint8_t*>(blas_jobs.data()), blas_bytes);
		}
		command.fillBuffer(m_hle_state->Handle(), 0, HleRefitStateSize, 0);
		MemoryBarrier(command, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
		              vk::PipelineStageFlagBits2::eComputeShader,
		              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
		command.bindPipeline(vk::PipelineBindPoint::eCompute, m_hle_refit_pipeline);
		command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_hle_refit_layout, 0, 1, &write);
		HleRefitPush push {jobs,
		                   m_hle_pool->BufferDeviceAddress(),
		                   m_hle_state->BufferDeviceAddress(),
		                   0,
		                   static_cast<uint32_t>(batch_geometries.size()),
		                   HleRefitPoolCapacity,
		                   header_layout};
		if (!batch_geometries.empty()) {
			command.pushConstants(m_hle_refit_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
			command.dispatch(static_cast<uint32_t>(batch_geometries.size()), 1, 1);
			// The boxes enclose the new vertices.
			MemoryBarrier(command, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
			              vk::PipelineStageFlagBits2::eComputeShader,
			              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
		}
		if (!batch_blases.empty()) {
			push.jobs      = jobs + blas_offset;
			push.mode      = arguments ? 2 : 1;
			push.job_count = static_cast<uint32_t>(batch_blases.size());
			command.pushConstants(m_hle_refit_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
			command.dispatch(static_cast<uint32_t>(batch_blases.size()), 1, 1);
		}
		MemoryBarrier(command, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
		              vk::PipelineStageFlagBits2::eAllCommands,
		              vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
	};
	const uint64_t geometry_bytes = uint64_t {HleRefitGeometryStride} * geometries.size();
	const uint64_t blas_bytes     = uint64_t {HleRefitBlasStride} * blases.size();
	if (((geometry_bytes + 255u) & ~uint64_t {255}) + blas_bytes <= HleRefitJobsSize) {
		pass(geometries, blases);
		return;
	}
	// More jobs than the job buffer holds: the vertices of every geometry first, as many geometries
	// as it holds at a time, then the boxes of the BLASes the same way.
	constexpr size_t GeometryBatch = HleRefitJobsSize / HleRefitGeometryStride;
	constexpr size_t BlasBatch     = HleRefitJobsSize / HleRefitBlasStride;
	for (size_t first = 0; first < geometries.size(); first += GeometryBatch) {
		pass(geometries.subspan(first, std::min(GeometryBatch, geometries.size() - first)), {});
	}
	for (size_t first = 0; first < blases.size(); first += BlasBatch) {
		pass({}, blases.subspan(first, std::min(BlasBatch, blases.size() - first)));
	}
}

} // namespace Libs::Graphics
