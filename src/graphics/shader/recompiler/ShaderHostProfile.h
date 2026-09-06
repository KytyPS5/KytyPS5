#ifndef KYTY_SHADER_RECOMPILER_SHADER_HOST_PROFILE_H_
#define KYTY_SHADER_RECOMPILER_SHADER_HOST_PROFILE_H_

namespace Libs::Graphics::ShaderRecompiler {

// Features describe the logical device actually used for compilation. An
// offline/default profile is unknown and must not imply native FP64 support.
struct ShaderHostProfile {
	bool known                                     = false;
	bool float64                                   = false;
	bool fma_float64                               = false;
	bool rte_float64                               = false;
	bool rte_float32                               = false;
	bool signed_zero_inf_nan_preserve_float64       = false;
};

} // namespace Libs::Graphics::ShaderRecompiler

#endif
