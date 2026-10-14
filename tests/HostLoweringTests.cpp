// Tests for the choices the renderer makes per host. No Vulkan device is needed.

#include "graphics/host_gpu/hostLowering.h"
#include "graphics/shader/capturedVertexLayout.h"

#include <iostream>

namespace {

using namespace Libs::Graphics;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

void TestDescriptorsArePushedWhenTheDeviceCan() {
	const auto host = HostGpu::Full();
	Check(ChooseDescriptorDelivery(host, 32, {.descriptors = 32, .storage_buffers = 8}) ==
	          DescriptorDelivery::Pushed,
	      "a set that fits the limit must be pushed");
	Check(ChooseDescriptorDelivery(host, 32, {.descriptors = 33, .storage_buffers = 0}) ==
	          DescriptorDelivery::Sets,
	      "a set over the limit must use a descriptor set");
}

void TestNoPushWithoutTheCapability() {
	auto host                          = HostGpu::Full();
	host.capabilities.push_descriptors = false;
	Check(ChooseDescriptorDelivery(host, 32, {.descriptors = 1, .storage_buffers = 0}) ==
	          DescriptorDelivery::Sets,
	      "a device that cannot push must use descriptor sets");
}

void TestPushedBuffersWithoutSizeAreNotPushed() {
	auto host                               = HostGpu::Full();
	host.faults.pushed_buffers_have_no_size = true;
	Check(ChooseDescriptorDelivery(host, 32, {.descriptors = 4, .storage_buffers = 1}) ==
	          DescriptorDelivery::Sets,
	      "a storage buffer must not be pushed on a driver that gives it no size");
	Check(ChooseDescriptorDelivery(host, 32, {.descriptors = 4, .storage_buffers = 0}) ==
	          DescriptorDelivery::Pushed,
	      "a set without storage buffers is not hit by the defect");
}

// The record of captured vertex outputs: every value has its own words, in the stated order.
void TestCapturedRecordHasNoOverlap() {
	const CapturedVertexLayout layout {
	    .vertices = 3, .primitives = 2, .parameters = 2, .clip_distances = 1, .cull_distances = 2};
	Check(layout.Valid(), "a small layout must be valid");
	Check(layout.VertexWords() == 4u + 8u + 1u + 2u, "words per vertex");
	Check(layout.PositionWord(0) == CapturedVertexLayout::kHeaderWords,
	      "the first vertex follows the header");
	Check(layout.ParameterWord(0, 0) == layout.PositionWord(0) + 4u, "parameters follow position");
	Check(layout.ClipDistanceWord(0, 0) == layout.ParameterWord(0, 1) + 4u,
	      "clip distances follow the last parameter");
	Check(layout.CullDistanceWord(0, 0) == layout.ClipDistanceWord(0, 0) + 1u,
	      "cull distances follow the clip distances");
	Check(layout.PositionWord(1) == layout.CullDistanceWord(0, 1) + 1u,
	      "the next vertex follows the last cull distance");
	Check(layout.PrimitiveWord(0) == layout.PositionWord(3), "primitives follow the vertices");
	Check(layout.RecordWords() == layout.PrimitiveWord(1) + CapturedVertexLayout::kPrimitiveWords,
	      "the record ends after the last primitive");
	Check(layout.VerticesPerRecord() == 6u, "three vertices are drawn per primitive");
}

void TestCapturedLayoutLimits() {
	CapturedVertexLayout layout {.vertices = 1024, .primitives = 1};
	Check(layout.Valid(), "1024 vertices fit ten index bits");
	layout.vertices = 1025;
	Check(!layout.Valid(), "1025 vertices do not fit ten index bits");
	layout = {.vertices = 3, .primitives = 1, .clip_distances = 5, .cull_distances = 4};
	Check(!layout.Valid(), "nine clip and cull distances are over the limit of eight");
	Check(!CapturedVertexLayout {}.Valid(), "an empty layout is not valid");
}

void TestCapturedDrawSizeAndLimit() {
	const CapturedVertexLayout layout {.vertices = 64, .primitives = 126, .parameters = 3};
	const auto                 one = PlanCapturedDraw(layout, 1);
	Check(one.has_value() && one->bytes == layout.RecordBytes() && one->vertices == 378u,
	      "one record: its bytes and three vertices per primitive");
	const uint64_t most = kCapturedDrawByteLimit / layout.RecordBytes();
	Check(PlanCapturedDraw(layout, most).has_value(), "a draw at the byte limit is planned");
	Check(!PlanCapturedDraw(layout, most + 1).has_value(), "a draw over the byte limit is not");
	Check(!PlanCapturedDraw(layout, 0).has_value(), "a draw with no record is not planned");
	Check(!PlanCapturedDraw({}, 1).has_value(), "an invalid layout is not planned");
	// The renderer plans from the record size it has (renderDraw.cpp): the same answer.
	for (const uint64_t records : {uint64_t {1}, most, most + 1}) {
		const auto by_layout = PlanCapturedDraw(layout, records);
		const auto by_size =
		    PlanCapturedDraw(layout.RecordBytes(), layout.VerticesPerRecord(), records);
		Check(by_layout.has_value() == by_size.has_value() &&
		          (!by_size || (by_size->bytes == by_layout->bytes &&
		                        by_size->vertices == by_layout->vertices)),
		      "a draw planned from its record size differs from the plan by layout");
	}
	Check(kCapturedDrawByteLimit >= 500'797'440u,
	      "the largest emulated mesh draw seen in Crash Bandicoot 4 (478 MiB) fits");
}

} // namespace

void TestDepthIsExactWithClampAndClipControl() {
	HostCapabilities host;
	host.depth_clamp       = true;
	host.depth_clip_enable = true;
	Check(HostDepthIsExact(host, true, true) && HostDepthIsExact(host, false, true),
	      "a host with depth clamping and VK_EXT_depth_clip_enable gives every draw exact depth");
}

void TestADrawThatDoesNotClipNeedsDepthClamp() {
	HostCapabilities host;
	host.depth_clamp = true;
	Check(HostDepthIsExact(host, false, true), "depth clamping alone gives a draw that does not clip");
	host.depth_clamp = false;
	Check(!HostDepthIsExact(host, false, false), "a draw that does not clip needs depth clamping");
}

// Without VK_EXT_depth_clip_enable a draw that clips runs without a clamp: exact until polygon
// offset moves its depth past the range.
void TestPolygonOffsetNeedsTheClampWhenADrawClips() {
	HostCapabilities host;
	host.depth_clamp = true;
	Check(HostDepthIsExact(host, true, false), "a draw that clips, without polygon offset, is exact");
	Check(!HostDepthIsExact(host, true, true),
	      "a draw that clips with polygon offset was taken as exact without a clamp");
}

int main() {
	TestDepthIsExactWithClampAndClipControl();
	TestADrawThatDoesNotClipNeedsDepthClamp();
	TestPolygonOffsetNeedsTheClampWhenADrawClips();
	TestDescriptorsArePushedWhenTheDeviceCan();
	TestNoPushWithoutTheCapability();
	TestPushedBuffersWithoutSizeAreNotPushed();
	TestCapturedRecordHasNoOverlap();
	TestCapturedLayoutLimits();
	TestCapturedDrawSizeAndLimit();
	if (g_failures != 0) {
		std::cerr << "HostLoweringTests: " << g_failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "HostLoweringTests: all cases passed\n";
	return 0;
}
