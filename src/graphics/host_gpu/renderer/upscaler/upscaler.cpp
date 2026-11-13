#include "graphics/host_gpu/renderer/upscaler/upscaler.h"

#include "graphics/host_gpu/renderer/upscaler/fsr.h"

namespace Libs::Graphics::Upscaler {
namespace {

class DlssFeature final: public Feature {
public:
	explicit DlssFeature(Dlss::Feature* feature): m_feature(feature) {}
	~DlssFeature() override { Dlss::ReleaseFeature(m_feature); }

	bool Evaluate(vk::CommandBuffer command, const Pssr::Request& request, Dlss::Inputs& inputs,
	              bool reset) override {
		return Dlss::Evaluate(command, m_feature, request, inputs, reset);
	}

private:
	Dlss::Feature* m_feature;
};

class FsrFeature final: public Feature {
public:
	explicit FsrFeature(Fsr::Context* context): m_context(context) {}
	~FsrFeature() override { Fsr::ReleaseContext(m_context); }

	bool Evaluate(vk::CommandBuffer command, const Pssr::Request& request, Dlss::Inputs& inputs,
	              bool reset) override {
		return Fsr::Evaluate(command, m_context, request, inputs, reset);
	}

private:
	Fsr::Context* m_context;
};

} // namespace

bool Enabled() {
	return Dlss::Capabilities() != nullptr || Fsr::Enabled();
}

std::optional<Dlss::Creation> Choose(const Pssr::Request& request, const Dlss::Inputs& inputs) {
	return Fsr::Enabled() ? Fsr::Choose(request, inputs)
	                      : Dlss::Choose(*Dlss::Capabilities(), request, inputs);
}

std::unique_ptr<Feature> Create(vk::CommandBuffer command, const Dlss::Creation& creation) {
	if (Fsr::Enabled()) {
		auto* context = Fsr::CreateContext(creation);
		return context != nullptr ? std::make_unique<FsrFeature>(context) : nullptr;
	}
	auto* feature = Dlss::CreateFeature(command, creation);
	return feature != nullptr ? std::make_unique<DlssFeature>(feature) : nullptr;
}

} // namespace Libs::Graphics::Upscaler
