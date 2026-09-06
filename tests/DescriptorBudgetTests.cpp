#include "graphics/host_gpu/renderer/pipeline/DescriptorBudget.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace Libs::Graphics;

namespace {
void Check(bool valid, const char* message) {
	if (!valid) {
		std::fprintf(stderr, "DescriptorBudgetTests: %s\n", message);
		std::abort();
	}
}

VkPhysicalDeviceLimits GenerousLimits() {
	VkPhysicalDeviceLimits limits {};
	for (auto* value: {
	         &limits.maxPerStageDescriptorSamplers, &limits.maxPerStageDescriptorUniformBuffers,
	         &limits.maxPerStageDescriptorStorageBuffers, &limits.maxPerStageDescriptorSampledImages,
	         &limits.maxPerStageDescriptorStorageImages, &limits.maxPerStageDescriptorInputAttachments,
	         &limits.maxPerStageResources, &limits.maxDescriptorSetSamplers,
	         &limits.maxDescriptorSetUniformBuffers, &limits.maxDescriptorSetUniformBuffersDynamic,
	         &limits.maxDescriptorSetStorageBuffers, &limits.maxDescriptorSetStorageBuffersDynamic,
	         &limits.maxDescriptorSetSampledImages, &limits.maxDescriptorSetStorageImages,
	         &limits.maxDescriptorSetInputAttachments,
	     }) {
		*value = UINT32_MAX;
	}
	return limits;
}

void ExpectFailure(std::span<const DescriptorBudgetBinding> bindings,
                   const VkPhysicalDeviceLimits& limits, const char* name,
                   uint64_t required, uint64_t limit, VkShaderStageFlags stage,
                   uint32_t color_attachments = 0) {
	const auto failure = ValidateDescriptorBudget(bindings, limits, color_attachments);
	Check(failure.has_value(), "over-budget layout was accepted");
	Check(std::strcmp(failure->limit_name, name) == 0 && failure->required == required &&
	          failure->limit == limit && failure->stage == stage &&
	          failure->resource_type != nullptr && failure->resource_type[0] != '\0',
	      "budget failure lost its exact limit, count, stage, or resource type");
}

void TestNativeArraysAndAuxiliaryBuffers() {
	auto limits = GenerousLimits();
	const std::array bindings {
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128, VK_SHADER_STAGE_COMPUTE_BIT},
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16, VK_SHADER_STAGE_COMPUTE_BIT},
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4, VK_SHADER_STAGE_COMPUTE_BIT},
	};
	limits.maxPerStageDescriptorStorageBuffers = 131;
	limits.maxPerStageDescriptorStorageImages = 20;
	limits.maxPerStageResources = 151;
	Check(!ValidateDescriptorBudget(bindings, limits), "exact array/auxiliary budget was rejected");
	limits.maxPerStageDescriptorStorageBuffers = 130;
	ExpectFailure(bindings, limits, "maxPerStageDescriptorStorageBuffers", 131, 130,
	              VK_SHADER_STAGE_COMPUTE_BIT);
	limits.maxPerStageDescriptorStorageBuffers = 131;
	limits.maxDescriptorSetStorageImages = 19;
	ExpectFailure(bindings, limits, "maxDescriptorSetStorageImages", 20, 19, 0);
}

void TestGraphicsStagesAndSharedVisibility() {
	auto limits = GenerousLimits();
	const std::array separate {
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64, VK_SHADER_STAGE_VERTEX_BIT},
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64, VK_SHADER_STAGE_FRAGMENT_BIT},
	};
	limits.maxPerStageDescriptorSampledImages = 64;
	limits.maxPerStageResources = 64;
	limits.maxDescriptorSetSampledImages = 128;
	Check(!ValidateDescriptorBudget(separate, limits), "VS and PS were incorrectly added per stage");
	limits.maxDescriptorSetSampledImages = 127;
	ExpectFailure(separate, limits, "maxDescriptorSetSampledImages", 128, 127, 0);
	const std::array shared {DescriptorBudgetBinding {
	    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT}};
	limits.maxDescriptorSetSampledImages = 64;
	Check(!ValidateDescriptorBudget(shared, limits), "shared visibility multiplied the layout total");
	limits.maxPerStageDescriptorSampledImages = 63;
	ExpectFailure(shared, limits, "maxPerStageDescriptorSampledImages", 64, 63,
	              VK_SHADER_STAGE_VERTEX_BIT);
}

void TestSamplersAndCombinedImages() {
	auto limits = GenerousLimits();
	std::vector<DescriptorBudgetBinding> bindings {
	    {VK_DESCRIPTOR_TYPE_SAMPLER, 128, VK_SHADER_STAGE_COMPUTE_BIT}};
	limits.maxPerStageResources = 0;
	limits.maxPerStageDescriptorSamplers = 128;
	Check(!ValidateDescriptorBudget(bindings, limits), "separate samplers consumed the resource budget");
	bindings.push_back({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8, VK_SHADER_STAGE_COMPUTE_BIT});
	limits.maxPerStageDescriptorSamplers = 136;
	limits.maxPerStageDescriptorSampledImages = 8;
	limits.maxPerStageResources = 8;
	Check(!ValidateDescriptorBudget(bindings, limits), "combined image samplers counted as two resources");
	limits.maxPerStageDescriptorSamplers = 135;
	ExpectFailure(bindings, limits, "maxPerStageDescriptorSamplers", 136, 135,
	              VK_SHADER_STAGE_COMPUTE_BIT);
	limits.maxPerStageDescriptorSamplers = 136;
	limits.maxPerStageResources = 7;
	ExpectFailure(bindings, limits, "maxPerStageResources", 8, 7, VK_SHADER_STAGE_COMPUTE_BIT);
	limits.maxPerStageResources = 8;
	limits.maxDescriptorSetSamplers = 135;
	ExpectFailure(bindings, limits, "maxDescriptorSetSamplers", 136, 135, 0);
}

void TestTexelBufferAndInputTypes() {
	auto limits = GenerousLimits();
	const std::array bindings {
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 5, VK_SHADER_STAGE_FRAGMENT_BIT},
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3, VK_SHADER_STAGE_FRAGMENT_BIT},
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 7, VK_SHADER_STAGE_FRAGMENT_BIT},
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2, VK_SHADER_STAGE_FRAGMENT_BIT},
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 4, VK_SHADER_STAGE_FRAGMENT_BIT},
	};
	limits.maxPerStageDescriptorUniformBuffers = 0;
	limits.maxPerStageDescriptorStorageBuffers = 0;
	limits.maxPerStageResources = 21;
	Check(!ValidateDescriptorBudget(bindings, limits), "texel descriptors were charged as buffer descriptors");
	limits.maxPerStageDescriptorSampledImages = 7;
	ExpectFailure(bindings, limits, "maxPerStageDescriptorSampledImages", 8, 7, VK_SHADER_STAGE_FRAGMENT_BIT);
	limits.maxPerStageDescriptorSampledImages = 8;
	limits.maxPerStageDescriptorStorageImages = 8;
	ExpectFailure(bindings, limits, "maxPerStageDescriptorStorageImages", 9, 8, VK_SHADER_STAGE_FRAGMENT_BIT);
	limits.maxPerStageDescriptorStorageImages = 9;
	limits.maxDescriptorSetInputAttachments = 3;
	ExpectFailure(bindings, limits, "maxDescriptorSetInputAttachments", 4, 3, 0);
}

void TestDynamicBuffers() {
	for (const bool storage: {false, true}) {
		auto limits = GenerousLimits();
		const std::array bindings {
		    DescriptorBudgetBinding {storage ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		                             1, VK_SHADER_STAGE_COMPUTE_BIT},
		    DescriptorBudgetBinding {storage ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		                             2, VK_SHADER_STAGE_COMPUTE_BIT},
		};
		auto& stage_limit = storage ? limits.maxPerStageDescriptorStorageBuffers : limits.maxPerStageDescriptorUniformBuffers;
		auto& static_limit = storage ? limits.maxDescriptorSetStorageBuffers : limits.maxDescriptorSetUniformBuffers;
		auto& dynamic_limit = storage ? limits.maxDescriptorSetStorageBuffersDynamic : limits.maxDescriptorSetUniformBuffersDynamic;
		stage_limit = 3;
		static_limit = 1;
		dynamic_limit = 2;
		limits.maxPerStageResources = 3;
		Check(!ValidateDescriptorBudget(bindings, limits), "dynamic descriptors consumed the static layout budget");
		stage_limit = 2;
		ExpectFailure(bindings, limits, storage ? "maxPerStageDescriptorStorageBuffers" : "maxPerStageDescriptorUniformBuffers",
		              3, 2, VK_SHADER_STAGE_COMPUTE_BIT);
		stage_limit = 3;
		dynamic_limit = 1;
		ExpectFailure(bindings, limits, storage ? "maxDescriptorSetStorageBuffersDynamic" : "maxDescriptorSetUniformBuffersDynamic",
		              2, 1, 0);
	}
}

void TestFragmentColorAttachments() {
	auto limits = GenerousLimits();
	const std::array bindings {
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3, VK_SHADER_STAGE_FRAGMENT_BIT},
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4, VK_SHADER_STAGE_VERTEX_BIT},
	};
	limits.maxPerStageResources = 4;
	Check(!ValidateDescriptorBudget(bindings, limits, 1), "color attachment affected the vertex stage budget");
	ExpectFailure(bindings, limits, "maxPerStageResources", 5, 4, VK_SHADER_STAGE_FRAGMENT_BIT, 2);
}

void TestWideCountsAndUnsupportedTypes() {
	auto limits = GenerousLimits();
	Check(!ValidateDescriptorBudget({}, VkPhysicalDeviceLimits {}), "empty layout consumed resources");
	const std::array large {
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, UINT32_MAX, VK_SHADER_STAGE_COMPUTE_BIT},
	    DescriptorBudgetBinding {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
	};
	ExpectFailure(large, limits, "maxPerStageDescriptorStorageBuffers", uint64_t {UINT32_MAX} + 1u,
	              UINT32_MAX, VK_SHADER_STAGE_COMPUTE_BIT);
	const std::array unsupported {DescriptorBudgetBinding {
	    VK_DESCRIPTOR_TYPE_MUTABLE_EXT, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
	ExpectFailure(unsupported, limits, "unsupportedDescriptorType", 1, 0, VK_SHADER_STAGE_COMPUTE_BIT);
}
} // namespace

int main() {
	TestNativeArraysAndAuxiliaryBuffers();
	TestGraphicsStagesAndSharedVisibility();
	TestSamplersAndCombinedImages();
	TestTexelBufferAndInputTypes();
	TestDynamicBuffers();
	TestFragmentColorAttachments();
	TestWideCountsAndUnsupportedTypes();
	std::puts("DescriptorBudgetTests: passed");
}
