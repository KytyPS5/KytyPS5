#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DYNAMICSTATE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DYNAMICSTATE_H_

#include "common/assert.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace Libs::Graphics {

struct GraphicContext;

struct StencilOps {
	vk::StencilOp fail_op {};
	vk::StencilOp pass_op {};
	vk::StencilOp depth_fail_op {};
	vk::CompareOp compare_op {};

	bool operator==(const StencilOps& other) const {
		return fail_op == other.fail_op && pass_op == other.pass_op &&
		       depth_fail_op == other.depth_fail_op && compare_op == other.compare_op;
	}
};

struct DynamicState {
	struct {
		bool viewports : 1;
		bool scissors  : 1;

		bool depth_test_enabled  : 1;
		bool depth_write_enabled : 1;
		bool depth_compare_op    : 1;

		bool depth_bounds_test_enabled : 1;
		bool depth_bounds              : 1;

		bool depth_bias_enabled : 1;
		bool depth_bias         : 1;

		bool stencil_test_enabled       : 1;
		bool stencil_front_ops          : 1;
		bool stencil_front_reference    : 1;
		bool stencil_front_write_mask   : 1;
		bool stencil_front_compare_mask : 1;
		bool stencil_back_ops           : 1;
		bool stencil_back_reference     : 1;
		bool stencil_back_write_mask    : 1;
		bool stencil_back_compare_mask  : 1;

		bool cull_mode  : 1;
		bool front_face : 1;

		bool blend_constants       : 1;
		bool line_width            : 1;
		bool feedback_loop_aspects : 1;
	} dirty_state {};

	std::array<vk::Viewport, 16> viewports {};
	std::array<vk::Rect2D, 16>   scissors {};
	uint32_t                   num_viewports {};
	uint32_t                   num_scissors {};

	bool          depth_test_enabled {};
	bool          depth_write_enabled {};
	vk::CompareOp depth_compare_op {};

	bool  depth_bounds_test_enabled {};
	float depth_bounds_min {};
	float depth_bounds_max {};

	bool  depth_bias_enabled {};
	float depth_bias_constant {};
	float depth_bias_clamp {};
	float depth_bias_slope {};

	bool       stencil_test_enabled {};
	StencilOps stencil_front_ops {};
	uint32_t   stencil_front_reference {};
	uint32_t   stencil_front_write_mask {};
	uint32_t   stencil_front_compare_mask {};
	StencilOps stencil_back_ops {};
	uint32_t   stencil_back_reference {};
	uint32_t   stencil_back_write_mask {};
	uint32_t   stencil_back_compare_mask {};

	vk::CullModeFlags cull_mode {};
	vk::FrontFace     front_face {};

	std::array<float, 4> blend_constants {};
	float                line_width {};
	vk::ImageAspectFlags feedback_loop_aspects {};
	uint64_t             generation {}; // see Generation()

	/// Commits the dynamic state to the provided command buffer.
	void Commit(const GraphicContext& graphics, CommandRecorder cmdbuf);

	/// Takes the values of `values` (a copy captured after a draw set them): only those that
	/// differ are marked for the next Commit() (native XPR draws, RenderExecutor::NativeXprEmit).
	void Assign(const DynamicState& values) {
		SetViewports(std::span(values.viewports).first(values.num_viewports));
		SetScissors(std::span(values.scissors).first(values.num_scissors));
		SetDepthTestEnabled(values.depth_test_enabled);
		SetDepthWriteEnabled(values.depth_write_enabled);
		SetDepthCompareOp(values.depth_compare_op);
		SetDepthBoundsTestEnabled(values.depth_bounds_test_enabled);
		SetDepthBounds(values.depth_bounds_min, values.depth_bounds_max);
		SetDepthBiasEnabled(values.depth_bias_enabled);
		SetDepthBias(values.depth_bias_constant, values.depth_bias_clamp, values.depth_bias_slope);
		SetStencilTestEnabled(values.stencil_test_enabled);
		SetStencilOps(values.stencil_front_ops, values.stencil_back_ops);
		SetStencilReferences(values.stencil_front_reference, values.stencil_back_reference);
		SetStencilWriteMasks(values.stencil_front_write_mask, values.stencil_back_write_mask);
		SetStencilCompareMasks(values.stencil_front_compare_mask, values.stencil_back_compare_mask);
		SetBlendConstants(values.blend_constants);
		SetCullMode(values.cull_mode);
		SetFrontFace(values.front_face);
		SetLineWidth(values.line_width);
		SetAttachmentFeedbackLoopAspects(values.feedback_loop_aspects);
	}

	/// Invalidates all dynamic state to be flushed into the next command buffer.
	void Invalidate() {
		std::memset(&dirty_state, 0xFF, sizeof(dirty_state));
		++generation;
	}

	/// Advances at every Invalidate() (a new command buffer, a helper pipeline) and whenever a draw binds its
	/// pipeline, descriptors and index buffer (MoveGeneration): a path that binds graphics state itself (table
	/// draws, RenderExecutor::TableDraw) can tell whether anything else bound state since.
	[[nodiscard]] uint64_t Generation() const noexcept { return generation; }
	void                   MoveGeneration() noexcept { ++generation; }

	void SetViewports(std::span<const vk::Viewport> viewports_) {
		EXIT_IF(viewports_.size() > viewports.size());
		if (!std::ranges::equal(std::span(viewports).first(num_viewports), viewports_)) {
			std::ranges::copy(viewports_, viewports.begin());
			num_viewports         = static_cast<uint32_t>(viewports_.size());
			dirty_state.viewports = true;
		}
	}

	void SetScissors(std::span<const vk::Rect2D> scissors_) {
		EXIT_IF(scissors_.size() > scissors.size());
		if (!std::ranges::equal(std::span(scissors).first(num_scissors), scissors_)) {
			std::ranges::copy(scissors_, scissors.begin());
			num_scissors         = static_cast<uint32_t>(scissors_.size());
			dirty_state.scissors = true;
		}
	}

	void SetDepthTestEnabled(const bool enabled) {
		if (depth_test_enabled != enabled) {
			depth_test_enabled             = enabled;
			dirty_state.depth_test_enabled = true;
		}
	}

	void SetDepthWriteEnabled(const bool enabled) {
		if (depth_write_enabled != enabled) {
			depth_write_enabled             = enabled;
			dirty_state.depth_write_enabled = true;
		}
	}

	void SetDepthCompareOp(const vk::CompareOp compare_op) {
		if (depth_compare_op != compare_op) {
			depth_compare_op             = compare_op;
			dirty_state.depth_compare_op = true;
		}
	}

	void SetDepthBoundsTestEnabled(const bool enabled) {
		if (depth_bounds_test_enabled != enabled) {
			depth_bounds_test_enabled             = enabled;
			dirty_state.depth_bounds_test_enabled = true;
		}
	}

	void SetDepthBounds(const float min, const float max) {
		if (depth_bounds_min != min || depth_bounds_max != max) {
			depth_bounds_min         = min;
			depth_bounds_max         = max;
			dirty_state.depth_bounds = true;
		}
	}

	void SetDepthBiasEnabled(const bool enabled) {
		if (depth_bias_enabled != enabled) {
			depth_bias_enabled             = enabled;
			dirty_state.depth_bias_enabled = true;
		}
	}

	void SetDepthBias(const float constant, const float clamp, const float slope) {
		if (depth_bias_constant != constant || depth_bias_clamp != clamp ||
		    depth_bias_slope != slope) {
			depth_bias_constant    = constant;
			depth_bias_clamp       = clamp;
			depth_bias_slope       = slope;
			dirty_state.depth_bias = true;
		}
	}

	void SetStencilTestEnabled(const bool enabled) {
		if (stencil_test_enabled != enabled) {
			stencil_test_enabled             = enabled;
			dirty_state.stencil_test_enabled = true;
		}
	}

	void SetStencilOps(const StencilOps& front_ops, const StencilOps& back_ops) {
		if (stencil_front_ops != front_ops) {
			stencil_front_ops             = front_ops;
			dirty_state.stencil_front_ops = true;
		}
		if (stencil_back_ops != back_ops) {
			stencil_back_ops             = back_ops;
			dirty_state.stencil_back_ops = true;
		}
	}

	void SetStencilReferences(const uint32_t front_reference, const uint32_t back_reference) {
		if (stencil_front_reference != front_reference) {
			stencil_front_reference             = front_reference;
			dirty_state.stencil_front_reference = true;
		}
		if (stencil_back_reference != back_reference) {
			stencil_back_reference             = back_reference;
			dirty_state.stencil_back_reference = true;
		}
	}

	void SetStencilWriteMasks(const uint32_t front_write_mask, const uint32_t back_write_mask) {
		if (stencil_front_write_mask != front_write_mask) {
			stencil_front_write_mask             = front_write_mask;
			dirty_state.stencil_front_write_mask = true;
		}
		if (stencil_back_write_mask != back_write_mask) {
			stencil_back_write_mask             = back_write_mask;
			dirty_state.stencil_back_write_mask = true;
		}
	}

	void SetStencilCompareMasks(const uint32_t front_compare_mask,
	                            const uint32_t back_compare_mask) {
		if (stencil_front_compare_mask != front_compare_mask) {
			stencil_front_compare_mask             = front_compare_mask;
			dirty_state.stencil_front_compare_mask = true;
		}
		if (stencil_back_compare_mask != back_compare_mask) {
			stencil_back_compare_mask             = back_compare_mask;
			dirty_state.stencil_back_compare_mask = true;
		}
	}

	void SetCullMode(const vk::CullModeFlags cull_mode_) {
		if (cull_mode != cull_mode_) {
			cull_mode             = cull_mode_;
			dirty_state.cull_mode = true;
		}
	}

	void SetFrontFace(const vk::FrontFace front_face_) {
		if (front_face != front_face_) {
			front_face             = front_face_;
			dirty_state.front_face = true;
		}
	}

	void SetBlendConstants(const std::array<float, 4> blend_constants_) {
		if (blend_constants != blend_constants_) {
			blend_constants             = blend_constants_;
			dirty_state.blend_constants = true;
		}
	}

	void SetLineWidth(const float width) {
		if (line_width != width) {
			line_width             = width;
			dirty_state.line_width = true;
		}
	}

	void SetAttachmentFeedbackLoopAspects(const vk::ImageAspectFlags aspects) {
		if (feedback_loop_aspects != aspects) {
			feedback_loop_aspects             = aspects;
			dirty_state.feedback_loop_aspects = true;
		}
	}
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DYNAMICSTATE_H_
