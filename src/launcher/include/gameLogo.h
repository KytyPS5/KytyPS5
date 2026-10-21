#pragma once

#include "gameContent.h"

#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

#include <QImage>
#include <QRect>

#include <cstdint>
#include <cstring>
#include <vector>

// Loads the game's title logo from sce_sys/pic2.dds (BC7/BC3/BC1) or pic2.png.
// Returns a null image when the file is missing or only holds an empty transparent canvas.
namespace GameLogo {

inline uint32_t ReadU32(const QByteArray& data, int offset) {
	uint32_t value = 0;
	std::memcpy(&value, data.constData() + offset, sizeof(value));
	return value;
}

inline QImage DecodeDds(const QByteArray& data) {
	if (data.size() < 128 || ReadU32(data, 0) != 0x20534444 || ReadU32(data, 4) != 124) {
		return {};
	}
	const uint32_t height = ReadU32(data, 12);
	const uint32_t width  = ReadU32(data, 16);
	if (width == 0 || height == 0 || width > 8192 || height > 8192) {
		return {};
	}

	const uint32_t fourcc = ReadU32(data, 84);
	int            offset = 128;
	int            kind   = 0; // 1 = BC1, 3 = BC3, 7 = BC7
	if (fourcc == 0x30315844) {
		if (data.size() < 148) {
			return {};
		}
		const uint32_t format = ReadU32(data, 128);
		kind                  = (format == 98 || format == 99) ? 7 : (format == 77 || format == 78) ? 3
		                                                          : (format == 71 || format == 72) ? 1
		                                                                                           : 0;
		offset                = 148;
	} else if (fourcc == 0x35545844) {
		kind = 3;
	} else if (fourcc == 0x31545844) {
		kind = 1;
	}
	if (kind == 0) {
		return {};
	}

	const uint32_t blocks_x    = (width + 3) / 4;
	const uint32_t blocks_y    = (height + 3) / 4;
	const uint32_t block_bytes = kind == 1 ? 8 : 16;
	if (static_cast<uint64_t>(data.size() - offset) < static_cast<uint64_t>(blocks_x) * blocks_y * block_bytes) {
		return {};
	}

	const int       pitch = static_cast<int>(blocks_x * 4 * 4);
	std::vector<uint8_t> pixels(static_cast<size_t>(blocks_x) * 4 * blocks_y * 4 * 4);
	const auto*     src = reinterpret_cast<const uint8_t*>(data.constData()) + offset;
	for (uint32_t by = 0; by < blocks_y; by++) {
		for (uint32_t bx = 0; bx < blocks_x; bx++) {
			auto* dst = pixels.data() + static_cast<size_t>(by) * 4 * pitch + bx * 16;
			if (kind == 7) {
				bcdec_bc7(src, dst, pitch);
			} else if (kind == 3) {
				bcdec_bc3(src, dst, pitch);
			} else {
				bcdec_bc1(src, dst, pitch);
			}
			src += block_bytes;
		}
	}

	const QImage full(pixels.data(), static_cast<int>(width), static_cast<int>(height), pitch,
	                  QImage::Format_RGBA8888);
	return full.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// Crops to the visible pixels; a null image means nothing visible was found.
inline QImage CropVisible(const QImage& image) {
	if (image.isNull()) {
		return {};
	}
	int         left = image.width(), top = image.height(), right = -1, bottom = -1;
	qint64      visible = 0;
	for (int y = 0; y < image.height(); y++) {
		const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
		for (int x = 0; x < image.width(); x++) {
			if (qAlpha(line[x]) > 24) {
				visible++;
				left   = qMin(left, x);
				right  = qMax(right, x);
				top    = qMin(top, y);
				bottom = qMax(bottom, y);
			}
		}
	}
	if (visible < 64 || right < left || bottom < top) {
		return {};
	}
	return image.copy(QRect(left, top, right - left + 1, bottom - top + 1));
}

inline QImage Load(const QString& base) {
	if (base.isEmpty()) {
		return {};
	}
	constexpr uint64_t MaxSize = 64u << 20u;
	QImage             image   = DecodeDds(GameContent::ReadFile(base, QStringLiteral("sce_sys/pic2.dds"), MaxSize));
	if (image.isNull()) {
		image.loadFromData(GameContent::ReadFile(base, QStringLiteral("sce_sys/pic2.png"), MaxSize));
		image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	}
	return CropVisible(image);
}

} // namespace GameLogo
