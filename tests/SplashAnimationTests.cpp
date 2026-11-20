#include "graphics/presentation/splashAnimation.h"

#include <gtest/gtest.h>

namespace {

using Libs::Graphics::AnimateSplash;

TEST(SplashAnimation, BackgroundFadesInAndGrowsFromTheTopLeft) {
	const auto start = AnimateSplash(0.0f, false);
	EXPECT_FLOAT_EQ(start.background_alpha, 0.0f);
	EXPECT_FLOAT_EQ(start.background_uv0, 0.0f);
	EXPECT_FLOAT_EQ(start.background_uv1, 1.0f / 0.965f);
	EXPECT_FLOAT_EQ(start.logo_alpha, 0.0f);

	EXPECT_FLOAT_EQ(AnimateSplash(0.4f, false).background_alpha, 1.0f);
	EXPECT_FLOAT_EQ(AnimateSplash(0.1f, false).background_alpha, 0.125f);

	EXPECT_FLOAT_EQ(AnimateSplash(0.7f, false).background_uv1, 1.0f / (1.0f - 0.035f / 16.0f));

	const auto grown = AnimateSplash(1.4f, false);
	EXPECT_FLOAT_EQ(grown.background_uv0, 0.0f);
	EXPECT_FLOAT_EQ(grown.background_uv1, 1.0f);
	EXPECT_FLOAT_EQ(grown.logo_alpha, 0.0f);
}

TEST(SplashAnimation, LogoEasesInWhileBackgroundZooms) {
	EXPECT_FLOAT_EQ(AnimateSplash(2.5f, false).logo_alpha, 0.5f);
	EXPECT_FLOAT_EQ(AnimateSplash(3.05f, false).logo_alpha, 0.875f);
	EXPECT_FLOAT_EQ(AnimateSplash(3.6f, false).logo_alpha, 1.0f);

	const auto middle = AnimateSplash(7.4f, false);
	EXPECT_FLOAT_EQ(middle.background_uv0, 0.5f - 0.5f / 1.05f);
	EXPECT_FLOAT_EQ(middle.background_uv1, 0.5f + 0.5f / 1.05f);

	for (const float age: {13.4f, 60.0f}) {
		const auto zoomed = AnimateSplash(age, false);
		EXPECT_FLOAT_EQ(zoomed.background_uv0, 0.5f - 0.5f / 1.1f);
		EXPECT_FLOAT_EQ(zoomed.background_uv1, 0.5f + 0.5f / 1.1f);
		EXPECT_FLOAT_EQ(zoomed.logo_uv1, 1.0f);
		EXPECT_FLOAT_EQ(zoomed.logo_alpha, 1.0f);
	}
}

TEST(SplashAnimation, LogoFirstGrowsInWithTheBackground) {
	for (const float age: {0.0f, 0.1f, 0.7f, 1.4f}) {
		const auto frame = AnimateSplash(age, true);
		EXPECT_FLOAT_EQ(frame.logo_alpha, frame.background_alpha);
		EXPECT_FLOAT_EQ(frame.logo_uv1, frame.background_uv1);
	}
	const auto zoomed = AnimateSplash(7.4f, true);
	EXPECT_FLOAT_EQ(zoomed.logo_alpha, 1.0f);
	EXPECT_FLOAT_EQ(zoomed.logo_uv1, 1.0f);
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
