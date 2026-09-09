#pragma once

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace Libs::Graphics {

struct DescriptorBudgetBinding {
	VkDescriptorType   type;
	uint32_t           count;
	VkShaderStageFlags stages;
};

struct DescriptorBudgetFailure {
	const char*        limit_name;
	const char*        resource_type;
	VkShaderStageFlags stage; // Zero denotes the complete pipeline layout.
	uint64_t           required;
	uint64_t           limit;
};

namespace DescriptorBudgetDetail {
struct Counts {
	uint64_t samplers        = 0;
	uint64_t sampled_images  = 0;
	uint64_t storage_images  = 0;
	uint64_t uniform_buffers = 0;
	uint64_t storage_buffers = 0;
	uint64_t uniform_dynamic = 0;
	uint64_t storage_dynamic = 0;
	uint64_t inputs          = 0;
	uint64_t resources       = 0;

	bool Add(VkDescriptorType type, uint32_t count) {
		switch (type) {
			case VK_DESCRIPTOR_TYPE_SAMPLER:
				samplers += count;
				return true; // Separate samplers do not consume maxPerStageResources.
			case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
				samplers += count;
				sampled_images += count;
				break; // Both typed budgets, but only one resource per array element.
			case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
			case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: sampled_images += count; break;
			case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
			case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: storage_images += count; break;
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: uniform_buffers += count; break;
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: storage_buffers += count; break;
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: uniform_dynamic += count; break;
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: storage_dynamic += count; break;
			case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: inputs += count; break;
			default: return false;
		}
		resources += count;
		return true;
	}
};
} // namespace DescriptorBudgetDetail

// Ordinary/push descriptor layouts only: no update-after-bind, mutable descriptors,
// inline uniform blocks, or immutable YCbCr conversion samplers are emitted by this backend.
// Extension descriptor types are rejected instead of being silently omitted from the budget.
// Counts describe all set layouts together; a binding visible to two stages counts once
// towards a maxDescriptorSet limit and separately towards each stage's limit.
// Vulkan 1.3 pipeline-layout VUIDs 03016-03035 define the typed budgets:
// https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineLayoutCreateInfo.html
// maxPerStageResources additionally includes actual fragment color attachments (not depth):
// https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceLimits.html
[[nodiscard]] inline std::optional<DescriptorBudgetFailure>
ValidateDescriptorBudget(std::span<const DescriptorBudgetBinding> bindings,
                         const VkPhysicalDeviceLimits& limits,
                         uint32_t fragment_color_attachments = 0) {
	using DescriptorBudgetDetail::Counts;
	Counts total;
	std::array<Counts, 32> stages {};
	for (const auto& binding: bindings) {
		if (binding.count == 0u) {
			continue;
		}
		if (!total.Add(binding.type, binding.count)) {
			return DescriptorBudgetFailure {"unsupportedDescriptorType", "extension descriptors",
			                                binding.stages, binding.count, 0u};
		}
		for (uint32_t index = 0; index < stages.size(); index++) {
			if ((binding.stages & (uint32_t {1} << index)) != 0u) {
				stages[index].Add(binding.type, binding.count);
			}
		}
	}
	// The core fragment stage bit is 1 << 4. Color attachments have no descriptor
	// type and do not contribute to any maxDescriptorSet or typed per-stage limit.
	stages[4].resources += fragment_color_attachments;
	const auto check = [](const char* name, const char* type, VkShaderStageFlags stage,
	                      uint64_t required, uint32_t limit)
	    -> std::optional<DescriptorBudgetFailure> {
		if (required > limit) {
			return DescriptorBudgetFailure {name, type, stage, required, limit};
		}
		return std::nullopt;
	};
	for (uint32_t index = 0; index < stages.size(); index++) {
		const auto stage = uint32_t {1} << index;
		const auto& count = stages[index];
		for (const auto& failure: {
		         check("maxPerStageDescriptorSamplers", "samplers", stage, count.samplers,
		               limits.maxPerStageDescriptorSamplers),
		         check("maxPerStageDescriptorUniformBuffers", "uniform buffers", stage,
		               count.uniform_buffers + count.uniform_dynamic, limits.maxPerStageDescriptorUniformBuffers),
		         check("maxPerStageDescriptorStorageBuffers", "storage buffers", stage,
		               count.storage_buffers + count.storage_dynamic, limits.maxPerStageDescriptorStorageBuffers),
		         check("maxPerStageDescriptorSampledImages", "sampled images / uniform texel buffers", stage,
		               count.sampled_images, limits.maxPerStageDescriptorSampledImages),
		         check("maxPerStageDescriptorStorageImages", "storage images / storage texel buffers", stage,
		               count.storage_images, limits.maxPerStageDescriptorStorageImages),
		         check("maxPerStageDescriptorInputAttachments", "input attachments", stage, count.inputs,
		               limits.maxPerStageDescriptorInputAttachments),
		         check("maxPerStageResources", "resources including color attachments", stage, count.resources,
		               limits.maxPerStageResources),
		     }) {
			if (failure) {
				return failure;
			}
		}
	}
	// Static and dynamic buffer totals have separate layout limits (03029-03032),
	// while their per-stage limits above are shared.
	for (const auto& failure: {
	         check("maxDescriptorSetSamplers", "samplers", 0u, total.samplers, limits.maxDescriptorSetSamplers),
	         check("maxDescriptorSetUniformBuffers", "uniform buffers", 0u, total.uniform_buffers,
	               limits.maxDescriptorSetUniformBuffers),
	         check("maxDescriptorSetUniformBuffersDynamic", "dynamic uniform buffers", 0u, total.uniform_dynamic,
	               limits.maxDescriptorSetUniformBuffersDynamic),
	         check("maxDescriptorSetStorageBuffers", "storage buffers", 0u, total.storage_buffers,
	               limits.maxDescriptorSetStorageBuffers),
	         check("maxDescriptorSetStorageBuffersDynamic", "dynamic storage buffers", 0u, total.storage_dynamic,
	               limits.maxDescriptorSetStorageBuffersDynamic),
	         check("maxDescriptorSetSampledImages", "sampled images / uniform texel buffers", 0u, total.sampled_images,
	               limits.maxDescriptorSetSampledImages),
	         check("maxDescriptorSetStorageImages", "storage images / storage texel buffers", 0u, total.storage_images,
	               limits.maxDescriptorSetStorageImages),
	         check("maxDescriptorSetInputAttachments", "input attachments", 0u, total.inputs,
	               limits.maxDescriptorSetInputAttachments),
	     }) {
		if (failure) {
			return failure;
		}
	}
	return std::nullopt;
}

} // namespace Libs::Graphics
