#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PushDescriptorPolicyTests: failed: %s\n", message);
		std::abort();
	}
}

} // namespace

static_assert(Libs::Graphics::SelectMaxPushDescriptors(vk::DriverId::eMoltenvk, 32) == 0,
              "MoltenVK must lose the push descriptor limit");
static_assert(Libs::Graphics::UsePushDescriptors(4, 0) == false, "a device without push support must not push");
static_assert(Libs::Graphics::UsePushDescriptors(0, 32) == true, "an empty pipeline keeps its previous choice");

int main() {
	using Libs::Graphics::SelectMaxPushDescriptors;
	using Libs::Graphics::UsePushDescriptors;

	Check(SelectMaxPushDescriptors(vk::DriverId::eMoltenvk, 32) == 0, "MoltenVK must not keep the push descriptor path");
	Check(SelectMaxPushDescriptors(vk::DriverId::eNvidiaProprietary, 32) == 32, "push descriptor drivers keep their limit");
	Check(SelectMaxPushDescriptors(vk::DriverId::eMoltenvk, 0) == 0, "a device without push support stays without push support");

	Check(!UsePushDescriptors(4, 0), "no descriptor may be pushed without push descriptor support");
	Check(!UsePushDescriptors(0, 0), "a device without push support must not push an empty pipeline either");
	Check(!UsePushDescriptors(33, 32), "more descriptors than the device limit must not be pushed");
	Check(UsePushDescriptors(32, 32), "the device limit itself stays on the push path");
	Check(UsePushDescriptors(4, 32), "descriptors within the device limit use the push path");
	Check(UsePushDescriptors(0, 32), "a pipeline without binding keeps its previous layout choice");

	std::printf("PushDescriptorPolicyTests: ok\n");
	return 0;
}
