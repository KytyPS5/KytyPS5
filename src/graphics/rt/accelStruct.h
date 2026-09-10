#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_ACCELSTRUCT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_ACCELSTRUCT_H_

#include "graphics/rt/bvhNode.h"

// Deliberately not "graphics/host_gpu/vulkanCommon.h": this subsystem isn't wired into the main
// kyty target yet (it takes raw Vulkan/VMA handles rather than GraphicContext, same reasoning as
// bvhNodeParser.h staying dependency-free), so it defines the same dynamic-dispatch macros
// vulkanCommon.h would rather than depending on it. Defining these identically twice in one
// translation unit is harmless (redefinition with an identical token sequence is legal), so this
// stays safe even after integration.
#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_EXCEPTIONS

#include <vulkan/vulkan.hpp>

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

// Resources backing a single built bottom-level acceleration structure. No RAII: this is
// pre-integration scaffolding, and lifetime/ownership will be dictated by whatever eventually
// wires it into the renderer. Destroy with DestroyBlasBuildResources.
struct BlasBuildResources {
	vk::Buffer                   vertex_buffer      = nullptr;
	VmaAllocation                vertex_allocation  = nullptr;
	vk::Buffer                   scratch_buffer     = nullptr;
	VmaAllocation                scratch_allocation = nullptr;
	vk::Buffer                   backing_buffer     = nullptr;
	VmaAllocation                backing_allocation = nullptr;
	vk::AccelerationStructureKHR handle             = nullptr;
};

// Packs each triangle's 3 vertex positions consecutively (9 floats per triangle: v0, v1, v2),
// the layout VkAccelerationStructureGeometryTrianglesDataKHR expects when indexType is eNoneKHR.
// RDNA2 triangle BVH nodes already store raw per-triangle positions rather than shared/indexed
// vertices (see BvhTriangleNode), so there is nothing to deduplicate.
std::vector<float> FlattenTrianglesToVertexBuffer(std::span<const BvhTriangleNode> triangles);

// Queries how large the scratch and backing buffers must be to build a BLAS from
// `triangle_count` un-indexed triangles. Touches the device but allocates nothing.
vk::AccelerationStructureBuildSizesInfoKHR QueryBlasBuildSizes(vk::Device device,
                                                               uint32_t   triangle_count);

// Allocates the vertex/scratch/backing buffers and the acceleration structure object, uploads
// `triangles` into the vertex buffer, and records the build command into `command_buffer`.
// Does not submit or wait: recording is kept separate from submission so this can eventually be
// batched into an existing command buffer/queue submission model instead of dictating its own.
// The vertex buffer is written via a mapped host-visible+device-local allocation rather than a
// staging buffer + copy, since this is a rare one-shot build rather than a per-frame hot path;
// revisit if this ever needs to run on a GPU without a host-visible device-local heap.
// The returned resources must stay valid until the recorded commands finish executing on the
// device, and must eventually be passed to DestroyBlasBuildResources.
BlasBuildResources RecordBlasBuild(vk::PhysicalDevice physical_device, vk::Device device,
                                   VmaAllocator allocator, vk::CommandBuffer command_buffer,
                                   std::span<const BvhTriangleNode> triangles);

void DestroyBlasBuildResources(VmaAllocator allocator, vk::Device device,
                               BlasBuildResources& resources);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_ACCELSTRUCT_H_ */
