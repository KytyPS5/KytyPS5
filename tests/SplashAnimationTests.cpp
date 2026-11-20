#include "graphics/presentation/splashAnimation.h"

#include <gtest/gtest.h>

namespace {

using Libs::Graphics::AnimateSplash;
using Libs::Graphics::SplashLayer;

// Checks that `layer` shows the image scaled by `scale` with its point (x, y) fixed.
void ExpectScaledAbout(const SplashLayer& layer, float scale, float x, float y) {
	EXPECT_FLOAT_EQ(layer.u1 - layer.u0, 1.0f / scale);
	EXPECT_FLOAT_EQ(layer.v1 - layer.v0, 1.0f / scale);
	EXPECT_NEAR(layer.u0 + x * (layer.u1 - layer.u0), x, 1e-6f);
	EXPECT_NEAR(layer.v0 + y * (layer.v1 - layer.v0), y, 1e-6f);
}

TEST(SplashAnimation, BackgroundFadesInAndGrows) {
	const auto start = AnimateSplash(0.0f, false);
	ExpectScaledAbout(start[0], 0.965f, 0.13f, 0.2f);
	EXPECT_FLOAT_EQ(start[1].alpha, 0.0f);

	EXPECT_FLOAT_EQ(AnimateSplash(0.1f, false)[0].alpha, 0.125f);
	EXPECT_FLOAT_EQ(AnimateSplash(0.3f, false)[0].alpha, 0.875f);
	ExpectScaledAbout(AnimateSplash(0.7f, false)[0], 1.0f - 0.035f / 16.0f, 0.13f, 0.2f);
	ExpectScaledAbout(AnimateSplash(1.05f, false)[0], 1.0f - 0.035f / 256.0f, 0.13f, 0.2f);
}

TEST(SplashAnimation, LogoEasesInWhileBackgroundZooms) {
	EXPECT_FLOAT_EQ(AnimateSplash(2.5f, false)[1].alpha, 0.5f);
	EXPECT_FLOAT_EQ(AnimateSplash(3.05f, false)[1].alpha, 0.875f);

	ExpectScaledAbout(AnimateSplash(7.4f, false)[0], 1.05f, 0.5f, 0.5f);
	ExpectScaledAbout(AnimateSplash(10.4f, false)[0], 1.0875f, 0.5f, 0.5f);
	const auto zoomed = AnimateSplash(60.0f, false);
	ExpectScaledAbout(zoomed[0], 1.1f, 0.5f, 0.5f);
	ExpectScaledAbout(zoomed[1], 1.0f, 0.0f, 0.0f);
	EXPECT_FLOAT_EQ(zoomed[1].alpha, 1.0f);
}

TEST(SplashAnimation, LogoFirstGrowsInWithTheBackground) {
	for (const float age: {0.1f, 0.7f}) {
		const auto layers = AnimateSplash(age, true);
		EXPECT_FLOAT_EQ(layers[1].alpha, layers[0].alpha);
		EXPECT_FLOAT_EQ(layers[1].u0, layers[0].u0);
		EXPECT_FLOAT_EQ(layers[1].v0, layers[0].v0);
		EXPECT_FLOAT_EQ(layers[1].u1, layers[0].u1);
		EXPECT_FLOAT_EQ(layers[1].v1, layers[0].v1);
	}
	const auto zoomed = AnimateSplash(7.4f, true);
	ExpectScaledAbout(zoomed[1], 1.0f, 0.0f, 0.0f);
	EXPECT_FLOAT_EQ(zoomed[1].alpha, 1.0f);
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
