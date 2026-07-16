#include "graphics/shader/shaderBindings.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"

namespace Libs::Graphics {

ResourceDescriptorType ShaderClassifyResourceDescriptor(const uint32_t* desc) {
	EXIT_IF(desc == nullptr);

	const auto raw_type = (desc[3] >> 28u) & 0xfu;
	switch (static_cast<Prospero::ImageType>(raw_type)) {
		case Prospero::ImageType::kColor1D:
		case Prospero::ImageType::kColor2D:
		case Prospero::ImageType::kColor3D:
		case Prospero::ImageType::kCube:
		case Prospero::ImageType::kColor1DArray:
		case Prospero::ImageType::kColor2DArray:
		case Prospero::ImageType::kColor2DMsaa:
		case Prospero::ImageType::kColor2DMsaaArray: return ResourceDescriptorType::Texture;
		default: break;
	}

	// The mixed resource tag shares a byte with texture fields, so texture type wins.
	switch (static_cast<Prospero::DescriptorKind>((desc[5] >> 27u) & 0x3u)) {
		case Prospero::DescriptorKind::kBuffer: return ResourceDescriptorType::Buffer;
		case Prospero::DescriptorKind::kSampler: return ResourceDescriptorType::Sampler;
		case Prospero::DescriptorKind::kUnused: return ResourceDescriptorType::Unused;
		default: break;
	}

	return (raw_type & Prospero::GpuEnumValue(Prospero::ImageType::kColor1D)) == 0
	           ? ResourceDescriptorType::Buffer
	           : ResourceDescriptorType::Unused;
}

} // namespace Libs::Graphics
