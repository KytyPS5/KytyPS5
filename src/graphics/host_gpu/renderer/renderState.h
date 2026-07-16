#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERSTATE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERSTATE_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace Libs::Graphics {

class CommandBuffer;
struct DepthStencilVulkanImage;
struct RenderTextureVulkanImage;
struct ShaderVertexInputInfo;
struct VulkanImage;

namespace HW {
class Context;
class Shader;
class UserConfig;
struct RenderTarget;
struct ScanModeControl;
struct ScreenViewport;
} // namespace HW

static constexpr uint32_t RENDER_COLOR_ATTACHMENTS_MAX = 8;

inline constexpr bool depth_msaa_single_sample_compatible(uint32_t encoded_fragments) {
	return encoded_fragments == 1 || encoded_fragments == 2;
}

inline constexpr bool color_msaa_single_sample_compatible(uint32_t encoded_samples,
                                                          uint32_t encoded_fragments) {
	return encoded_samples == encoded_fragments &&
	       depth_msaa_single_sample_compatible(encoded_fragments);
}

inline constexpr bool depth_htile_stencil_acceleration_compatible(bool has_stencil, bool has_htile,
                                                                  bool acceleration_disabled) {
	return acceleration_disabled || (has_stencil && has_htile);
}

// Pack structs to guarantee the uniqueness of object representation.
#pragma pack(push, 1)

struct PipelineStencilStaticState {
	VkStencilOp failOp      = VK_STENCIL_OP_KEEP;
	VkStencilOp passOp      = VK_STENCIL_OP_KEEP;
	VkStencilOp depthFailOp = VK_STENCIL_OP_KEEP;
	VkCompareOp compareOp   = VK_COMPARE_OP_NEVER;
};

struct PipelineStencilDynamicState {
	uint32_t compareMask = 0;
	uint32_t writeMask   = 0;
	uint32_t reference   = 0;
};

inline constexpr bool stencil_face_accesses_attachment(const PipelineStencilStaticState&  state,
                                                       const PipelineStencilDynamicState& dynamic) {
	return state.compareOp != VK_COMPARE_OP_ALWAYS ||
	       (dynamic.writeMask != 0 &&
	        (state.failOp != VK_STENCIL_OP_KEEP || state.passOp != VK_STENCIL_OP_KEEP ||
	         state.depthFailOp != VK_STENCIL_OP_KEEP));
}

struct PipelineStaticParameters {
	float                      viewport_scale[3]        = {};
	float                      viewport_offset[3]       = {};
	bool                       negative_one_to_one      = false;
	int                        scissor_ltrb[4]          = {0};
	VkPrimitiveTopology        topology                 = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	bool                       with_depth               = false;
	bool                       depth_test_enable        = false;
	bool                       depth_write_enable       = false;
	VkCompareOp                depth_compare_op         = VK_COMPARE_OP_NEVER;
	bool                       depth_bounds_test_enable = false;
	float                      depth_min_bounds         = 0.0f;
	float                      depth_max_bounds         = 0.0f;
	bool                       stencil_test_enable      = false;
	PipelineStencilStaticState stencil_front;
	PipelineStencilStaticState stencil_back;
	uint32_t                   color_count                                        = 1;
	uint32_t                   color_mask[RENDER_COLOR_ATTACHMENTS_MAX]           = {};
	bool                       cull_front                                         = false;
	bool                       cull_back                                          = false;
	bool                       face                                               = false;
	uint8_t                    color_srcblend[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    color_comb_fcn[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    color_destblend[RENDER_COLOR_ATTACHMENTS_MAX]      = {};
	uint8_t                    alpha_srcblend[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    alpha_comb_fcn[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    alpha_destblend[RENDER_COLOR_ATTACHMENTS_MAX]      = {};
	bool                       separate_alpha_blend[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	bool                       blend_enable[RENDER_COLOR_ATTACHMENTS_MAX]         = {};
	bool                       blend_bypass[RENDER_COLOR_ATTACHMENTS_MAX]         = {};
	float                      blend_color_red                                    = 0.0f;
	float                      blend_color_green                                  = 0.0f;
	float                      blend_color_blue                                   = 0.0f;
	float                      blend_color_alpha                                  = 0.0f;

	bool operator==(const PipelineStaticParameters& other) const noexcept;
};

struct PipelineDynamicParameters {
	bool vk_dynamic_state_viewport               = false;
	bool vk_dynamic_state_scissor                = false;
	bool vk_dynamic_state_line_width             = false;
	bool vk_dynamic_state_stencil_compare_mask   = false;
	bool vk_dynamic_state_stencil_write_mask     = false;
	bool vk_dynamic_state_stencil_reference      = false;
	bool vk_dynamic_state_color_write_enable_ext = false;
	bool stencil_test_enable                     = false;

	float viewport_scale[3]  = {};
	float viewport_offset[3] = {};
	int   scissor_ltrb[4]    = {0};

	float    line_width                                       = 1.0f;
	uint32_t color_write_count                                = 1;
	bool     color_write_enable[RENDER_COLOR_ATTACHMENTS_MAX] = {true, true};

	PipelineStencilDynamicState stencil_front;
	PipelineStencilDynamicState stencil_back;
};

#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<PipelineStencilStaticState>);
static_assert(std::is_standard_layout_v<PipelineStencilStaticState>);
static_assert(alignof(PipelineStencilStaticState) == 1);
static_assert(sizeof(PipelineStencilStaticState) == sizeof(VkStencilOp) * 3 + sizeof(VkCompareOp));
static_assert(std::is_trivially_copyable_v<PipelineStencilDynamicState>);
static_assert(std::is_standard_layout_v<PipelineStencilDynamicState>);
static_assert(alignof(PipelineStencilDynamicState) == 1);
static_assert(sizeof(PipelineStencilDynamicState) == sizeof(uint32_t) * 3);
static_assert(std::is_trivially_copyable_v<PipelineStaticParameters>);
static_assert(std::is_standard_layout_v<PipelineStaticParameters>);
static_assert(alignof(PipelineStaticParameters) == 1);
static_assert(sizeof(PipelineStaticParameters) ==
              sizeof(float[3]) + sizeof(float[3]) + sizeof(bool) + sizeof(int[4]) +
                  sizeof(VkPrimitiveTopology) + sizeof(bool) * 3 + sizeof(VkCompareOp) +
                  sizeof(bool) + sizeof(float) * 2 + sizeof(bool) +
                  sizeof(PipelineStencilStaticState) * 2 + sizeof(uint32_t) +
                  sizeof(uint32_t[RENDER_COLOR_ATTACHMENTS_MAX]) + sizeof(bool) * 3 +
                  sizeof(uint8_t[RENDER_COLOR_ATTACHMENTS_MAX]) * 6 +
                  sizeof(bool[RENDER_COLOR_ATTACHMENTS_MAX]) * 3 + sizeof(float) * 4);
static_assert(std::is_trivially_copyable_v<PipelineDynamicParameters>);
static_assert(std::is_standard_layout_v<PipelineDynamicParameters>);
static_assert(alignof(PipelineDynamicParameters) == 1);
static_assert(sizeof(PipelineDynamicParameters) ==
              sizeof(bool) * 8 + sizeof(float[3]) + sizeof(float[3]) + sizeof(int[4]) +
                  sizeof(float) + sizeof(uint32_t) + sizeof(bool[RENDER_COLOR_ATTACHMENTS_MAX]) +
                  sizeof(PipelineStencilDynamicState) * 2);

struct RenderDepthInfo {
	VkFormat                    format                   = VK_FORMAT_UNDEFINED;
	uint32_t                    width                    = 0;
	uint32_t                    height                   = 0;
	bool                        htile                    = false;
	uint64_t                    depth_buffer_size        = 0;
	uint64_t                    depth_buffer_vaddr       = 0;
	uint64_t                    depth_tile_swizzle       = 0;
	uint64_t                    stencil_buffer_size      = 0;
	uint64_t                    stencil_buffer_vaddr     = 0;
	uint64_t                    stencil_tile_swizzle     = 0;
	uint64_t                    htile_buffer_size        = 0;
	uint64_t                    htile_buffer_vaddr       = 0;
	uint64_t                    htile_tile_swizzle       = 0;
	bool                        depth_clear_enable       = false;
	bool                        depth_load_clear_enable  = false;
	bool                        depth_meta_clear_enable  = false;
	float                       depth_clear_value        = 0.0f;
	bool                        depth_test_enable        = false;
	bool                        depth_write_enable       = false;
	VkCompareOp                 depth_compare_op         = VK_COMPARE_OP_NEVER;
	bool                        depth_bounds_test_enable = false;
	float                       depth_min_bounds         = 0.0f;
	float                       depth_max_bounds         = 0.0f;
	bool                        stencil_clear_enable     = false;
	uint8_t                     stencil_clear_value      = 0;
	bool                        stencil_test_enable      = false;
	PipelineStencilStaticState  stencil_static_front;
	PipelineStencilStaticState  stencil_static_back;
	PipelineStencilDynamicState stencil_dynamic_front;
	PipelineStencilDynamicState stencil_dynamic_back;
	DepthStencilVulkanImage*    vulkan_buffer = nullptr;
	uint64_t                    vaddr[3]      = {};
	uint64_t                    size[3]       = {};
	int                         vaddr_num     = 0;
};

inline bool depth_attachment_read_only(const RenderDepthInfo* depth) {
	EXIT_IF(depth == nullptr);

	const bool stencil_write =
	    depth->stencil_test_enable &&
	    (depth->stencil_dynamic_front.writeMask != 0 || depth->stencil_dynamic_back.writeMask != 0);

	return !depth->depth_load_clear_enable && !depth->stencil_clear_enable &&
	       !depth->depth_write_enable && !stencil_write;
}

inline VkImageLayout depth_attachment_layout(const RenderDepthInfo* depth) {
	return depth_attachment_read_only(depth) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
	                                         : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}

enum class RenderColorType {
	NoColorOutput,
	DisplayBuffer,
	RenderTexture,
};

struct RenderColorInfo {
	RenderColorType   type               = RenderColorType::NoColorOutput;
	VulkanImage*      vulkan_buffer      = nullptr;
	VkImageView       vulkan_view        = nullptr;
	VkFormat          format             = VK_FORMAT_UNDEFINED;
	VkExtent2D        extent             = {};
	uint32_t          base_mip_level     = 0;
	uint64_t          base_addr          = 0;
	uint64_t          buffer_size        = 0;
	uint32_t          target_slot        = 0;
	bool              color_clear_enable = false;
	VkClearColorValue color_clear_value {};
};

struct ScissorRect {
	int left   = 0;
	int top    = 0;
	int right  = 0;
	int bottom = 0;
};

enum class CommandBufferDebugOp : uint32_t {
	DispatchDirect,
	DrawIndex,
	DrawIndexAuto,
	EopWrite,
	EopInterrupt,
	EopWriteBack,
	EopFlip,
	EopWriteBackFlip,
	EopOnlyFlip,
	Unknown,
};

uint32_t                 render_target_mask_slot(uint32_t mask, uint32_t slot);
uint32_t                 render_target_first_bound_slot(const HW::Context& hw);
bool                     graphics_debug_dump_enabled();
void                     uc_print(const char* func, const HW::UserConfig& uc);
void                     uc_check(const HW::UserConfig& uc);
void                     sh_print(const char* func, const HW::Shader& uc);
void                     sh_check(const HW::Shader& uc);
std::vector<std::string> rt_print(const char* func, const HW::RenderTarget& rt);
bool                     RenderIsColorTileModeLinear(uint32_t tile_mode);
void                     hw_print(const HW::Context& hw);
void                     hw_check(const HW::Context& hw);
void                     LogDrawPhase(const char* draw_name, const char* phase);
ScissorRect calc_final_scissor(const HW::ScreenViewport& vp, const HW::ScanModeControl& smc,
                               VkExtent2D extent);
int32_t     ResolveVertexOffset(uint32_t index_offset, const ShaderVertexInputInfo& vs_input_info);

void GraphicsRenderTextureBarrier(VkCommandBuffer vk_buffer, VulkanImage* image);
void GraphicsRenderColorImageBarrier(VkCommandBuffer vk_buffer, VulkanImage* image,
                                     VkImageLayout new_layout);
void GraphicsRenderDepthStencilImageBarrier(VkCommandBuffer vk_buffer, VulkanImage* image,
                                            VkImageLayout new_layout);
void GraphicsRenderDepthStencilBarrier(VkCommandBuffer vk_buffer, VulkanImage* image);
void ResolveRenderDepthTarget(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw,
                              RenderDepthInfo* r);
void ResolveRenderColorTarget(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw,
                              RenderColorInfo* r, uint32_t render_target_slice_offset = 0,
                              uint32_t render_target_slot            = UINT32_MAX,
                              bool     ignore_target_mask            = false,
                              bool     reuse_existing_render_texture = false);
void MarkRenderTargetGpuWritten(const RenderColorInfo& target);
void MarkRenderTargetGpuWritten(const RenderDepthInfo& target);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERSTATE_H_
