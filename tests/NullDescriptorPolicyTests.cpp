#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "NullDescriptorPolicyTests: failed: %s\n", message);
		std::abort();
	}
}

} // namespace

static_assert(Libs::Graphics::UseNativeNullDescriptor(true), "a supported feature must use the native null descriptor");
static_assert(!Libs::Graphics::UseNativeNullDescriptor(false), "an unsupported feature must use the stand-in image");

int main() {
	using Libs::Graphics::UseNativeNullDescriptor;

	Check(UseNativeNullDescriptor(true), "a supported nullDescriptor binds native null descriptors");
	Check(!UseNativeNullDescriptor(false), "an unsupported nullDescriptor (MoltenVK) binds the stand-in image");

	std::printf("NullDescriptorPolicyTests: ok\n");
	return 0;
}
