#pragma once

namespace Libs::Graphics::ShaderRecompiler {

struct CompileOptions;
struct ShaderSource;

void PrepareCallTargets(ShaderSource& source, const CompileOptions& options);

} // namespace Libs::Graphics::ShaderRecompiler
