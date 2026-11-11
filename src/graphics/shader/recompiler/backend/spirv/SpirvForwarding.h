#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

// Shrinks the function bodies of an emitted module that the driver would spend seconds on.
//
// The emitter keeps every guest register as a uint and masks each vector write by the lanes that
// execute: v = select(exec, new, old). A consumer that is itself masked by a condition that
// implies exec only ever reads `new`, so it can read it directly and the select, the bitcasts
// around it and the old value's chain become dead. The pass forwards such reads, cancels
// bitcast pairs and drops what no longer has a use. Modules below the size threshold are left
// alone (min_body_words of function-body words): the driver compiles them in milliseconds.
inline constexpr size_t ForwardingMinBodyWords = 30000;
void ForwardMaskedSelects(std::vector<uint32_t>& module, size_t min_body_words = ForwardingMinBodyWords);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
