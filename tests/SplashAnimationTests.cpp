#include "graphics/presentation/splashAnimation.h"

#include <gtest/gtest.h>

namespace {

using Libs::Graphics::AnimateSplash;
using Libs::Graphics::RevealSplash;
using Libs::Graphics::SplashLayer;

// Checks that `layer` shows the image scaled by `scale`.
void ExpectScaled(const SplashLayer& layer, float scale) {
	EXPECT_NEAR(layer.span, 1.0f / scale, 1e-6f);
}

TEST(SplashAnimation, BackgroundGrows) {
	const auto start = AnimateSplash(0.0f, false);
	ExpectScaled(start[0], 0.95f);
	EXPECT_FLOAT_EQ(start[0].alpha, 1.0f);
	EXPECT_FLOAT_EQ(start[1].alpha, 0.0f);

	ExpectScaled(AnimateSplash(0.35f, false)[0], 0.98736162f);
	ExpectScaled(AnimateSplash(0.7f, false)[0], 0.99681323f);
	ExpectScaled(AnimateSplash(1.4f, false)[0], 1.0f);
}

TEST(SplashAnimation, RevealWidensFromTheCenter) {
	EXPECT_FLOAT_EQ(RevealSplash(0.0f, 0.5f, 0.5f), 0.0f);
	EXPECT_FLOAT_EQ(RevealSplash(0.0f, 1.0f, 1.0f), 0.0f);
	EXPECT_FLOAT_EQ(RevealSplash(0.35f, 0.5f, 0.5f), 1.0f);

	// The soft edge 0.14 s in, the same distance from the center across and down the 16:9 image.
	constexpr float EDGE = 0.20080405f;
	EXPECT_NEAR(RevealSplash(0.14f, 0.5f + EDGE, 0.5f), 0.53746875f, 1e-5f);
	EXPECT_NEAR(RevealSplash(0.14f, 0.5f, 0.5f + EDGE * 16.0f / 9.0f), 0.53746875f, 1e-5f);

	// The corners show last, together, and fully from about 0.47 s.
	EXPECT_NEAR(RevealSplash(0.28f, 0.0f, 0.0f), 0.77636391f, 1e-5f);
	EXPECT_NEAR(RevealSplash(0.28f, 1.0f, 1.0f), 0.77636391f, 1e-5f);
	EXPECT_NEAR(RevealSplash(0.4f, 1.0f, 0.0f), 0.98870057f, 1e-5f);
	EXPECT_FLOAT_EQ(RevealSplash(0.5f, 0.0f, 1.0f), 1.0f);
}

TEST(SplashAnimation, LogoEasesInWhileBackgroundZooms) {
	EXPECT_FLOAT_EQ(AnimateSplash(1.4f, false)[1].alpha, 0.0f);
	EXPECT_FLOAT_EQ(AnimateSplash(2.5f, false)[1].alpha, 0.5f);
	EXPECT_FLOAT_EQ(AnimateSplash(3.05f, false)[1].alpha, 0.875f);

	ExpectScaled(AnimateSplash(7.4f, false)[0], 1.05f);
	ExpectScaled(AnimateSplash(10.4f, false)[0], 1.0875f);
	const auto zoomed = AnimateSplash(60.0f, false);
	ExpectScaled(zoomed[0], 1.1f);
	ExpectScaled(zoomed[1], 1.0f);
	EXPECT_FLOAT_EQ(zoomed[1].alpha, 1.0f);
}

TEST(SplashAnimation, LogoFirstShowsAtOnceAndGrowsWithTheBackground) {
	for (const float age: {0.0f, 0.7f}) {
		const auto layers = AnimateSplash(age, true);
		EXPECT_FLOAT_EQ(layers[1].alpha, 1.0f);
		EXPECT_FLOAT_EQ(layers[1].span, layers[0].span);
	}
	const auto zoomed = AnimateSplash(7.4f, true);
	ExpectScaled(zoomed[1], 1.0f);
	EXPECT_FLOAT_EQ(zoomed[1].alpha, 1.0f);
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
