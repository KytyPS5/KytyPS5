#ifndef KYTY_RUNTIME_DESCRIPTOR_FAULT_H_
#define KYTY_RUNTIME_DESCRIPTOR_FAULT_H_

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler {

// A dedicated trailing word follows the ordinary BDA page bitmap. Its flags
// travel in the unused upper DWORD of the existing download count header.
inline constexpr uint32_t RuntimeDescriptorFaultBytes = sizeof(uint32_t);
enum RuntimeDescriptorFault : uint32_t {
	InvalidBufferFormat = 1u,
	InvalidBufferSelector = 2u,
	InvalidBufferType = 4u,
};

} // namespace Libs::Graphics::ShaderRecompiler

#endif
