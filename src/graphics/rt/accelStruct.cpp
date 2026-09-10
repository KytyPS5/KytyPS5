#include "graphics/rt/accelStruct.h"

#include "common/assert.h"

#include <cstring>
#include <vk_mem_alloc.h>

namespace Libs::Graphics {

std::vector<float> FlattenTrianglesToVertexBuffer(std::span<const BvhTriangleNode> triangles) {
std::vector<float> vertices;
vertices.reserve(triangles.size() * 9);
for (const auto& triangle: triangles) {
for (const auto& vertex: triangle.vertices) {
vertices.push_back(vertex[0]);
vertices.push_back(vertex[1]);
vertices.push_back(vertex[2]);
}
}
return vertices;
}

namespace {

vk::AccelerationStructureGeometryKHR TriangleGeometry(
    const vk::AccelerationStructureGeometryTrianglesDataKHR& triangles_data) {
vk::AccelerationStructureGeometryKHR geometry {};
geometry.geometryType       = vk::GeometryTypeKHR::eTriangles;
geometry.geometry.triangles = triangles_data;
geometry.flags              = vk::GeometryFlagBitsKHR::eOpaque;
return geometry;
}

// The scratch buffer's device address must satisfy this device-specific minimum alignment; a
// plain vmaCreateBuffer only guarantees the alignment the driver reports via buffer memory
// requirements, which is unrelated. Confirmed against the Vulkan spec's
// VkAccelerationStructureBuildGeometryInfoKHR::scratchData valid usage requirements.
uint32_t QueryScratchAlignment(vk::PhysicalDevice physical_device) {
vk::PhysicalDeviceAccelerationStructurePropertiesKHR as_properties {};
as_properties.sType = vk::StructureType::ePhysicalDeviceAccelerationStructurePropertiesKHR;

vk::PhysicalDeviceProperties2 properties2 {};
properties2.sType = vk::StructureType::ePhysicalDeviceProperties2;
properties2.pNext = &as_properties;

physical_device.getProperties2(&properties2);
return as_properties.minAccelerationStructureScratchOffsetAlignment;
}

} // namespace

vk::AccelerationStructureBuildSizesInfoKHR QueryBlasBuildSizes(vk::Device device,
                                                                uint32_t   triangle_count) {
vk::AccelerationStructureGeometryTrianglesDataKHR triangles_data {};
triangles_data.vertexFormat = vk::Format::eR32G32B32Sfloat;
triangles_data.vertexStride = 3 * sizeof(float);
triangles_data.maxVertex    = triangle_count * 3 - 1;
triangles_data.indexType    = vk::IndexType::eNoneKHR;

const auto geometry = TriangleGeometry(triangles_data);

vk::AccelerationStructureBuildGeometryInfoKHR build_info {};
build_info.type          = vk::AccelerationStructureTypeKHR::eBottomLevel;
build_info.flags         = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
build_info.mode          = vk::BuildAccelerationStructureModeKHR::eBuild;
build_info.geometryCount = 1;
build_info.pGeometries   = &geometry;

return device.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice,
                                                      build_info, triangle_count);
}

BlasBuildResources RecordBlasBuild(vk::PhysicalDevice physical_device, vk::Device device,
                                    VmaAllocator                      allocator,
                                    vk::CommandBuffer                command_buffer,
                                    std::span<const BvhTriangleNode> triangles) {
const auto triangle_count = static_cast<uint32_t>(triangles.size());
const auto vertex_data    = FlattenTrianglesToVertexBuffer(triangles);
const auto vertex_bytes   = vertex_data.size() * sizeof(float);

BlasBuildResources resources {};

// Vertex buffer: mapped host-visible+device-local memory, written directly (see the "does not
// submit or wait" comment on the declaration for why no staging buffer is used here).
{
vk::BufferCreateInfo buffer_info {};
buffer_info.size  = vertex_bytes;
buffer_info.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress |
                     vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;

VmaAllocationCreateInfo allocation_info {};
allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

VmaAllocationInfo allocation_result {};
VkBuffer          native_buffer = VK_NULL_HANDLE;
const auto        result        = static_cast<vk::Result>(vmaCreateBuffer(
    allocator, static_cast<const VkBufferCreateInfo*>(buffer_info), &allocation_info,
    &native_buffer, &resources.vertex_allocation, &allocation_result));
EXIT_IF(result != vk::Result::eSuccess);
resources.vertex_buffer = native_buffer;
std::memcpy(allocation_result.pMappedData, vertex_data.data(), vertex_bytes);
}

const auto build_sizes = QueryBlasBuildSizes(device, triangle_count);

// Scratch buffer.
{
vk::BufferCreateInfo buffer_info {};
buffer_info.size  = build_sizes.buildScratchSize;
buffer_info.usage =
    vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eStorageBuffer;

VmaAllocationCreateInfo allocation_info {};
allocation_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

VkBuffer   native_buffer = VK_NULL_HANDLE;
const auto result        = static_cast<vk::Result>(vmaCreateBufferWithAlignment(
    allocator, static_cast<const VkBufferCreateInfo*>(buffer_info), &allocation_info,
    QueryScratchAlignment(physical_device), &native_buffer, &resources.scratch_allocation,
    nullptr));
EXIT_IF(result != vk::Result::eSuccess);
resources.scratch_buffer = native_buffer;
}

// Backing buffer for the acceleration structure object itself.
{
vk::BufferCreateInfo buffer_info {};
buffer_info.size  = build_sizes.accelerationStructureSize;
buffer_info.usage = vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR;

VmaAllocationCreateInfo allocation_info {};
allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

VkBuffer   native_buffer = VK_NULL_HANDLE;
const auto result        = static_cast<vk::Result>(
    vmaCreateBuffer(allocator, static_cast<const VkBufferCreateInfo*>(buffer_info),
                     &allocation_info, &native_buffer, &resources.backing_allocation, nullptr));
EXIT_IF(result != vk::Result::eSuccess);
resources.backing_buffer = native_buffer;
}

vk::AccelerationStructureCreateInfoKHR create_info {};
create_info.buffer = resources.backing_buffer;
create_info.size   = build_sizes.accelerationStructureSize;
create_info.type   = vk::AccelerationStructureTypeKHR::eBottomLevel;
EXIT_IF(device.createAccelerationStructureKHR(&create_info, nullptr, &resources.handle) !=
        vk::Result::eSuccess);

vk::BufferDeviceAddressInfo vertex_address_info {};
vertex_address_info.buffer = resources.vertex_buffer;
const auto vertex_address  = device.getBufferAddress(vertex_address_info);

vk::BufferDeviceAddressInfo scratch_address_info {};
scratch_address_info.buffer = resources.scratch_buffer;
const auto scratch_address  = device.getBufferAddress(scratch_address_info);

vk::AccelerationStructureGeometryTrianglesDataKHR triangles_data {};
triangles_data.vertexFormat             = vk::Format::eR32G32B32Sfloat;
triangles_data.vertexData.deviceAddress = vertex_address;
triangles_data.vertexStride             = 3 * sizeof(float);
triangles_data.maxVertex                = static_cast<uint32_t>(vertex_data.size() / 3 - 1);
triangles_data.indexType                = vk::IndexType::eNoneKHR;

const auto geometry = TriangleGeometry(triangles_data);

vk::AccelerationStructureBuildGeometryInfoKHR build_geometry_info {};
build_geometry_info.type                      = vk::AccelerationStructureTypeKHR::eBottomLevel;
build_geometry_info.flags     = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
build_geometry_info.mode                     = vk::BuildAccelerationStructureModeKHR::eBuild;
build_geometry_info.dstAccelerationStructure = resources.handle;
build_geometry_info.geometryCount            = 1;
build_geometry_info.pGeometries              = &geometry;
build_geometry_info.scratchData.deviceAddress = scratch_address;

vk::AccelerationStructureBuildRangeInfoKHR range_info {};
range_info.primitiveCount                                         = triangle_count;
const vk::AccelerationStructureBuildRangeInfoKHR* range_info_ptr = &range_info;

command_buffer.buildAccelerationStructuresKHR(build_geometry_info, range_info_ptr);

return resources;
}

void DestroyBlasBuildResources(VmaAllocator allocator, vk::Device device,
                                BlasBuildResources& resources) {
if (resources.handle) {
device.destroyAccelerationStructureKHR(resources.handle);
resources.handle = nullptr;
}
if (resources.backing_buffer) {
vmaDestroyBuffer(allocator, resources.backing_buffer, resources.backing_allocation);
resources.backing_buffer     = nullptr;
resources.backing_allocation = nullptr;
}
if (resources.scratch_buffer) {
vmaDestroyBuffer(allocator, resources.scratch_buffer, resources.scratch_allocation);
resources.scratch_buffer     = nullptr;
resources.scratch_allocation = nullptr;
}
if (resources.vertex_buffer) {
vmaDestroyBuffer(allocator, resources.vertex_buffer, resources.vertex_allocation);
resources.vertex_buffer     = nullptr;
resources.vertex_allocation = nullptr;
}
}

} // namespace Libs::Graphics
