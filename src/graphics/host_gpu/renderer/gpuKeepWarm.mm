// KYTY_GPU_KEEPWARM, the Metal side (see gpuKeepWarm.h). A queue of its own on the MTLDevice that MoltenVK renders on, a kernel that
// spins on dependent ALU work for ~dispatch_us, and a completion handler that keeps kMinInFlight command buffers in flight until Stop().
// When the guest stops submitting the spin pauses (InFlightPolicy) and the next NoteActivity() resumes it.

#include "graphics/host_gpu/renderer/gpuKeepWarm.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <sys/sysctl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>

namespace Libs::Graphics::GpuKeepWarm {

namespace {

constexpr uint32_t kThreadsPerGroup = 32;
constexpr uint32_t kCalibrationBufs = 8;
constexpr uint32_t kStartIters      = 20000;
constexpr uint32_t kMaxIters        = 400u * 1000u * 1000u;

NSString* const kKernelSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;
kernel void keepwarm(device uint* out [[buffer(0)]], constant uint& iters [[buffer(1)]], uint tid [[thread_position_in_grid]]) {
	uint x = tid * 2654435761u + 1u;
	for (uint i = 0; i < iters; i++) {
		x = x * 1664525u + 1013904223u;
		x ^= x >> 13;
	}
	out[tid] = x;
}
)MSL";

uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The host, not the process: the emulator runs translated by Rosetta, where sysctl still reports the hardware.
bool HostIsAppleSilicon() {
	int    value = 0;
	size_t size  = sizeof(value);
	return sysctlbyname("hw.optional.arm64", &value, &size, nullptr, 0) == 0 && value == 1;
}

struct State {
	id<MTLDevice>               device   = nil;
	id<MTLCommandQueue>         queue    = nil;
	id<MTLComputePipelineState> pipeline = nil;
	id<MTLBuffer>               out      = nil;
	InFlightPolicy              policy;
	uint32_t                    groups    = 0;
	uint64_t                    target_ns = 0;
	std::atomic<uint32_t>       iters {kStartIters};
	std::atomic<uint32_t>       calibrated {0};
	std::atomic<uint64_t>       cb_n {0}, gpu_ns {0}, iters_total {0};
	std::mutex                  mutex;
	std::condition_variable     cv;
	bool                        running = false;
	std::string                 device_name;
};

State g_state;

void Encode(State& s);

void OnDone(State& s, id<MTLCommandBuffer> cb) {
	if (cb.status == MTLCommandBufferStatusCompleted) {
		const double   gpu_s  = cb.GPUEndTime - cb.GPUStartTime;
		const uint64_t gpu_ns = gpu_s > 0 ? static_cast<uint64_t>(gpu_s * 1e9) : 0;
		const uint32_t iters  = s.iters.load();
		s.cb_n++;
		s.gpu_ns += gpu_ns;
		s.iters_total += static_cast<uint64_t>(iters) * s.groups;
		// Calibrate the first buffers to the requested length (the clock may be slow now, so never grow past 2x per step).
		if (gpu_ns > 0 && s.calibrated.load() < kCalibrationBufs) {
			s.calibrated++;
			double scale = static_cast<double>(s.target_ns) / static_cast<double>(gpu_ns);
			scale        = scale > 2.0 ? 2.0 : (scale < 0.5 ? 0.5 : scale);
			double next  = static_cast<double>(iters) * scale;
			next         = next < 1000.0 ? 1000.0 : (next > kMaxIters ? kMaxIters : next);
			s.iters.store(static_cast<uint32_t>(next));
		}
	} else {
		std::fprintf(stderr, "[keepwarm] command buffer failed (status %d), stopping the spin\n", static_cast<int>(cb.status));
		s.policy.Stop();
	}
	const uint32_t again = s.policy.OnCompleted(NowNs());
	for (uint32_t i = 0; i < again; i++) Encode(s);
	{
		std::lock_guard<std::mutex> lock(s.mutex); // no lost wake-up of Stop() waiting for Idle()
	}
	s.cv.notify_all();
}

void Encode(State& s) {
	@autoreleasepool {
		id<MTLCommandBuffer>         cb  = [s.queue commandBuffer];
		id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
		const uint32_t               it  = s.iters.load();
		[enc setComputePipelineState:s.pipeline];
		[enc setBuffer:s.out offset:0 atIndex:0];
		[enc setBytes:&it length:sizeof(it) atIndex:1];
		[enc dispatchThreadgroups:MTLSizeMake(s.groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(kThreadsPerGroup, 1, 1)];
		[enc endEncoding];
		State* sp = &s;
		[cb addCompletedHandler:^(id<MTLCommandBuffer> done) { OnDone(*sp, done); }];
		[cb commit];
	}
}

} // namespace

bool Start(uint32_t groups, uint32_t dispatch_us, const WantedDevice& wanted, std::string& reason) {
	State& s = g_state;
	if (groups == 0) {
		reason = "off";
		return false;
	}
	if (s.running) {
		reason = "already running";
		return false;
	}
	@autoreleasepool {
		std::vector<DeviceInfo> infos;
		NSArray<id<MTLDevice>>* all = MTLCopyAllDevices();
		for (id<MTLDevice> d in all) infos.push_back(DeviceInfo {d.registryID, d.name.UTF8String});
		const Eligibility e = CheckEligibility(HostIsAppleSilicon(), infos, wanted);
		if (!e.ok) {
			reason = e.reason;
			return false;
		}
		s.device = all[e.index];
		NSError*       error   = nil;
		id<MTLLibrary> library = [s.device newLibraryWithSource:kKernelSource options:nil error:&error];
		if (library == nil) {
			reason = std::string("kernel compile failed: ") + error.localizedDescription.UTF8String;
			return false;
		}
		id<MTLFunction> function = [library newFunctionWithName:@"keepwarm"];
		s.pipeline               = [s.device newComputePipelineStateWithFunction:function error:&error];
		if (s.pipeline == nil) {
			reason = "compute pipeline failed";
			return false;
		}
		s.queue = [s.device newCommandQueue];
		s.out   = [s.device newBufferWithLength:kThreadsPerGroup * kMaxGroups * sizeof(uint32_t) options:MTLResourceStorageModeShared];
		if (s.queue == nil || s.out == nil) {
			reason = "queue/buffer failed";
			return false;
		}
		s.device_name = s.device.name.UTF8String;
	}
	s.groups    = groups > kMaxGroups ? kMaxGroups : groups;
	s.target_ns = static_cast<uint64_t>(dispatch_us) * 1000u;
	s.running   = true;
	detail::g_running.store(true);
	const uint32_t first = s.policy.Fill(NowNs());
	for (uint32_t i = 0; i < first; i++) Encode(s);
	return true;
}

void Stop() {
	State& s = g_state;
	if (!s.running) return;
	detail::g_running.store(false);
	s.policy.Stop();
	{
		std::unique_lock<std::mutex> lock(s.mutex);
		s.cv.wait_for(lock, std::chrono::seconds(2), [&s] { return s.policy.Idle(); });
	}
	s.running = false;
}

Stats GetStats() {
	State& s = g_state;
	return Stats {s.cb_n.load(), s.gpu_ns.load(), s.iters_total.load(), s.policy.Pauses(), s.policy.Resumes(), s.policy.PausedNs(NowNs())};
}

const char* DeviceName() {
	return g_state.device_name.c_str();
}

namespace detail {

void NoteActivitySlow() {
	State&         s = g_state;
	const uint32_t n = s.policy.NoteActivity(NowNs());
	for (uint32_t i = 0; i < n; i++) Encode(s);
}

} // namespace detail

} // namespace Libs::Graphics::GpuKeepWarm
