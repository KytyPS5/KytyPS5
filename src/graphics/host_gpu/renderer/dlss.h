#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DLSS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DLSS_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace Libs::Graphics {
struct GraphicContext;
namespace Pssr {
struct Request;
}
} // namespace Libs::Graphics

// NVIDIA DLSS super resolution through the driver's NGX library and the user's model.
namespace Libs::Graphics::Dlss {

struct D3D11Resource;
struct D3D12Resource;
struct Feature;

// The NGX parameter object, declared in its interface's order.
class Parameter {
public:
	virtual void Set(const char* name, unsigned long long value)        = 0;
	virtual void Set(const char* name, float value)                     = 0;
	virtual void Set(const char* name, double value)                    = 0;
	virtual void Set(const char* name, unsigned int value)              = 0;
	virtual void Set(const char* name, int value)                       = 0;
	virtual void Set(const char* name, D3D11Resource* value)            = 0;
	virtual void Set(const char* name, D3D12Resource* value)            = 0;
	virtual void Set(const char* name, void* value)                     = 0;
	virtual int  Get(const char* name, unsigned long long* value) const = 0;
	virtual int  Get(const char* name, float* value) const              = 0;
	virtual int  Get(const char* name, double* value) const             = 0;
	virtual int  Get(const char* name, unsigned int* value) const       = 0;
	virtual int  Get(const char* name, int* value) const                = 0;
	virtual int  Get(const char* name, D3D11Resource** value) const     = 0;
	virtual int  Get(const char* name, D3D12Resource** value) const     = 0;
	virtual int  Get(const char* name, void** value) const              = 0;
	virtual void Reset()                                                = 0;

protected:
	~Parameter() = default;
};

// An image view NGX reads, or writes when read_write is set.
struct Resource {
	VkImageView             view;
	VkImage                 image;
	VkImageSubresourceRange range;
	VkFormat                format;
	uint32_t                width;
	uint32_t                height;
	int32_t                 type; // 0 is an image view
	uint8_t                 read_write;
};

struct Inputs {
	Resource  color;
	Resource  depth;
	Resource  motion;
	Resource* exposure; // null when absent
	Resource  output;
};

// What a feature fixes at creation: its quality mode and the render sizes that mode takes.
struct Creation {
	uint32_t output_width;
	uint32_t output_height;
	uint32_t flags; // Pssr::Flags fixed at creation
	int32_t  mode;
	uint32_t min_width;
	uint32_t min_height;
	uint32_t max_width; // the render size the feature is created for
	uint32_t max_height;

	bool operator==(const Creation&) const = default;
};

[[nodiscard]] bool Accepts(const Pssr::Request& request);
// Whether a feature made for creation takes the request's output, flags and render size.
[[nodiscard]] bool Takes(const Creation& creation, const Pssr::Request& request,
                         const Inputs& inputs);
// The quality mode whose render range holds the request's render size, with the optimal size
// nearest to it; nullopt when no mode takes the size.
[[nodiscard]] std::optional<Creation> Choose(Parameter& capabilities, const Pssr::Request& request,
                                             const Inputs& inputs);
void SetCreation(Parameter& parameters, const Creation& creation, uint32_t preset);
void SetEvaluation(Parameter& parameters, const Pssr::Request& request, Inputs& inputs, bool reset);

// Loads the driver's NGX library in Pro mode with a DLSS folder. Call before the instance.
void Load();
// Adds the instance or device extensions DLSS needs that `available` has.
void AppendExtensions(std::vector<const char*>&                   out,
                      const std::vector<vk::ExtensionProperties>& available, bool device);
// Starts DLSS on the device. It stays off when the model or the GPU can't run it.
void Init(const GraphicContext& graphics, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
          PFN_vkGetDeviceProcAddr get_device_proc_addr);
// Call after every feature is released and before the device is destroyed.
void                     Shutdown();
[[nodiscard]] Parameter* Capabilities(); // null while DLSS is off

// Both record into command. A null feature or false means DLSS can't take the request.
[[nodiscard]] Feature* CreateFeature(vk::CommandBuffer command, const Creation& creation);
[[nodiscard]] bool     Evaluate(vk::CommandBuffer command, const Feature* feature,
                                const Pssr::Request& request, Inputs& inputs, bool reset);
void                   ReleaseFeature(Feature* feature);

} // namespace Libs::Graphics::Dlss

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DLSS_H_
