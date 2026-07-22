#include "graphics/regression/frameRegression.h"

#include "common/file.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <system_error>
#include <unordered_set>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics::Regression {
namespace {

constexpr uint32_t SchemaVersion = 1;
constexpr int      FailureExitCode = 2;
constexpr uint64_t MaxManifestBytes = 4ull * 1024ull * 1024ull;
constexpr size_t   MaxManifestFrames = 4096;
constexpr size_t   MaxArtifactPathLength = 1024;

struct Digest {
	uint64_t low  = 0;
	uint64_t high = 0;
	bool operator==(const Digest&) const = default;
};

struct FrameRecord {
	uint64_t    ordinal   = 0;
	uint32_t    width     = 0;
	uint32_t    height    = 0;
	uint32_t    row_pitch = 0;
	uint64_t    byte_size = 0;
	PixelFormat format    = PixelFormat::Rgba8Unorm;
	Digest      digest;
	std::string raw_file;
	std::string image_file;
	std::string status;
	Digest      expected;
	bool        has_expected = false;
};

std::string Hex(uint64_t value) {
	std::array<char, 17> text {};
	std::snprintf(text.data(), text.size(), "%016" PRIx64, value);
	return text.data();
}

bool ParseHex(const std::string& text, uint64_t& value) {
	if (text.size() != 16) {
		return false;
	}
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
	return result.ec == std::errc {} && result.ptr == text.data() + text.size();
}

const char* PixelFormatName(PixelFormat format) {
	switch (format) {
		case PixelFormat::Rgba8Unorm: return "rgba8_unorm";
		case PixelFormat::Rgba8Srgb: return "rgba8_srgb";
		case PixelFormat::Bgra8Unorm: return "bgra8_unorm";
		case PixelFormat::Bgra8Srgb: return "bgra8_srgb";
		case PixelFormat::A2b10g10r10Unorm: return "a2b10g10r10_unorm";
		case PixelFormat::A2r10g10b10Unorm: return "a2r10g10b10_unorm";
		case PixelFormat::Rgba16Float: return "rgba16_float";
	}
	return "unknown";
}

bool ParsePixelFormat(const std::string& text, PixelFormat& format) {
	for (const auto candidate: {PixelFormat::Rgba8Unorm, PixelFormat::Rgba8Srgb,
	                            PixelFormat::Bgra8Unorm, PixelFormat::Bgra8Srgb,
	                            PixelFormat::A2b10g10r10Unorm,
	                            PixelFormat::A2r10g10b10Unorm, PixelFormat::Rgba16Float}) {
		if (text == PixelFormatName(candidate)) {
			format = candidate;
			return true;
		}
	}
	return false;
}

bool IsSafeArtifactPath(const std::string& text) {
	const std::filesystem::path path(text);
	if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
		return false;
	}
	for (const auto& component: path) {
		if (component == "..") {
			return false;
		}
	}
	return true;
}

bool ReadText(const std::filesystem::path& path, std::string& text) {
	Common::File file;
	if (!file.Open(path, Common::File::Mode::Read) || file.Size() > MaxManifestBytes) {
		if (!file.IsInvalid()) {
			file.Close();
		}
		return false;
	}
	text.resize(static_cast<size_t>(file.Size()));
	uint32_t read = 0;
	if (!text.empty()) {
		file.Read(text.data(), static_cast<uint32_t>(text.size()), &read);
	}
	file.Close();
	return read == text.size();
}

bool WriteBytes(const std::filesystem::path& path, const void* data, uint64_t size) {
	if (!path.parent_path().empty() && !Common::File::CreateDirectories(path.parent_path())) {
		return false;
	}
	Common::File file;
	if (!file.Create(path)) {
		return false;
	}
	const auto* bytes = static_cast<const uint8_t*>(data);
	while (size != 0) {
		const auto chunk = static_cast<uint32_t>(
		    std::min<uint64_t>(size, std::numeric_limits<uint32_t>::max()));
		uint32_t written = 0;
		file.Write(bytes, chunk, &written);
		if (written != chunk) {
			file.Close();
			return false;
		}
		bytes += chunk;
		size -= chunk;
	}
	const bool flushed = file.Flush();
	file.Close();
	return flushed;
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
	return WriteBytes(path, text.data(), text.size());
}

std::filesystem::path FrameDirectory(const std::filesystem::path& manifest) {
	return manifest.parent_path() / (manifest.stem().string() + "_frames");
}

std::string ComparablePath(const std::filesystem::path& path) {
	std::error_code error;
	auto normalized = std::filesystem::absolute(path, error);
	if (error) {
		normalized = path;
	}
	auto text = normalized.lexically_normal().generic_string();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	std::transform(text.begin(), text.end(), text.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
	return text;
}

bool SameOrWithin(const std::filesystem::path& candidate,
	              const std::filesystem::path& directory) {
	const auto path = ComparablePath(candidate);
	auto       root = ComparablePath(directory);
	if (path == root) {
		return true;
	}
	if (!root.empty() && root.back() != '/') {
		root.push_back('/');
	}
	return path.starts_with(root);
}

std::string FrameName(uint64_t ordinal, const char* extension) {
	char name[64] = {};
	std::snprintf(name, sizeof(name), "frame_%08" PRIu64 ".%s", ordinal, extension);
	return name;
}

bool SavePpm(const std::filesystem::path& path, const FrameView& frame,
	         std::span<const uint8_t> expected = {}) {
	const bool rgba = frame.format == PixelFormat::Rgba8Unorm ||
	                  frame.format == PixelFormat::Rgba8Srgb;
	const bool bgra = frame.format == PixelFormat::Bgra8Unorm ||
	                  frame.format == PixelFormat::Bgra8Srgb;
	if ((!rgba && !bgra) || (!expected.empty() && expected.size() != frame.bytes.size())) {
		return false;
	}
	std::vector<uint8_t> rgb;
	rgb.reserve(static_cast<size_t>(frame.width) * frame.height * 3u);
	for (uint32_t y = 0; y < frame.height; y++) {
		const auto row = static_cast<size_t>(y) * frame.row_pitch;
		for (uint32_t x = 0; x < frame.width; x++) {
			const auto offset = row + static_cast<size_t>(x) * 4u;
			const auto channel = [&](uint32_t index) {
				const auto actual = frame.bytes[offset + index];
				return expected.empty() ? actual
				                        : static_cast<uint8_t>(std::abs(
				                              static_cast<int>(actual) - expected[offset + index]));
			};
			if (rgba) {
				rgb.push_back(channel(0));
				rgb.push_back(channel(1));
				rgb.push_back(channel(2));
			} else {
				rgb.push_back(channel(2));
				rgb.push_back(channel(1));
				rgb.push_back(channel(0));
			}
		}
	}
	char header[96] = {};
	const auto header_size = std::snprintf(header, sizeof(header), "P6\n%u %u\n255\n", frame.width,
	                                       frame.height);
	if (header_size <= 0) {
		return false;
	}
	std::vector<uint8_t> ppm(static_cast<size_t>(header_size) + rgb.size());
	std::copy_n(reinterpret_cast<const uint8_t*>(header), header_size, ppm.begin());
	std::copy(rgb.begin(), rgb.end(), ppm.begin() + header_size);
	return WriteBytes(path, ppm.data(), ppm.size());
}

bool SameLayout(const FrameRecord& expected, const FrameRecord& actual) {
	return expected.width == actual.width && expected.height == actual.height &&
	       expected.row_pitch == actual.row_pitch && expected.byte_size == actual.byte_size &&
	       expected.format == actual.format;
}

FrameRecord ParseRecord(const nlohmann::json& json, std::string& error) {
	FrameRecord record;
	const auto unsigned_field = [&json](const char* name, uint64_t& value) {
		if (!json.contains(name) || !json[name].is_number_unsigned()) {
			return false;
		}
		value = json[name].get<uint64_t>();
		return true;
	};
	uint64_t width = 0;
	uint64_t height = 0;
	uint64_t row_pitch = 0;
	if (!json.is_object() || !unsigned_field("ordinal", record.ordinal) ||
	    !unsigned_field("width", width) || !unsigned_field("height", height) ||
	    !unsigned_field("row_pitch", row_pitch) ||
	    !unsigned_field("byte_size", record.byte_size) || width > UINT32_MAX ||
	    height > UINT32_MAX || row_pitch > UINT32_MAX || !json.contains("format") ||
	    !json["format"].is_string() || !json.contains("hash_low") ||
	    !json["hash_low"].is_string() || !json.contains("hash_high") ||
	    !json["hash_high"].is_string()) {
		error = "invalid frame record";
		return {};
	}
	record.width     = static_cast<uint32_t>(width);
	record.height    = static_cast<uint32_t>(height);
	record.row_pitch = static_cast<uint32_t>(row_pitch);
	if (!ParsePixelFormat(json["format"].get<std::string>(), record.format) ||
	    !ParseHex(json["hash_low"].get<std::string>(), record.digest.low) ||
	    !ParseHex(json["hash_high"].get<std::string>(), record.digest.high)) {
		error = "invalid frame format or digest";
		return {};
	}
	for (const auto* name: {"raw_file", "image_file"}) {
		if (json.contains(name) && !json[name].is_string()) {
			error = "invalid frame artifact path";
			return {};
		}
	}
	if ((json.contains("raw_file") &&
	     json["raw_file"].get_ref<const std::string&>().size() > MaxArtifactPathLength) ||
	    (json.contains("image_file") &&
	     json["image_file"].get_ref<const std::string&>().size() > MaxArtifactPathLength)) {
		error = "frame artifact path is too long";
		return {};
	}
	record.raw_file   = json.contains("raw_file") ? json["raw_file"].get<std::string>() : "";
	record.image_file = json.contains("image_file") ? json["image_file"].get<std::string>() : "";
	if ((!record.raw_file.empty() && !IsSafeArtifactPath(record.raw_file)) ||
	    (!record.image_file.empty() && !IsSafeArtifactPath(record.image_file))) {
		error = "unsafe frame artifact path";
		return {};
	}
	const auto bytes_per_pixel = BytesPerPixel(record.format);
	if (record.width == 0 || record.height == 0 || bytes_per_pixel == 0 ||
	    record.width > UINT32_MAX / bytes_per_pixel ||
	    record.row_pitch != record.width * bytes_per_pixel ||
	    record.byte_size != static_cast<uint64_t>(record.row_pitch) * record.height) {
		error = "invalid frame dimensions";
		return {};
	}
	return record;
}

nlohmann::ordered_json RecordJson(const FrameRecord& record) {
	nlohmann::ordered_json json;
	json["ordinal"] = record.ordinal;
	if (record.status == "missing") {
		json["status"] = record.status;
		return json;
	}
	json["width"]      = record.width;
	json["height"]     = record.height;
	json["row_pitch"]  = record.row_pitch;
	json["byte_size"]  = record.byte_size;
	json["format"]     = PixelFormatName(record.format);
	json["hash_low"]   = Hex(record.digest.low);
	json["hash_high"]  = Hex(record.digest.high);
	if (!record.raw_file.empty()) {
		json["raw_file"] = record.raw_file;
	}
	if (!record.image_file.empty()) {
		json["image_file"] = record.image_file;
	}
	if (!record.status.empty()) {
		json["status"] = record.status;
	}
	if (record.has_expected) {
		json["expected_hash_low"]  = Hex(record.expected.low);
		json["expected_hash_high"] = Hex(record.expected.high);
	}
	return json;
}

} // namespace

uint32_t BytesPerPixel(PixelFormat format) {
	switch (format) {
		case PixelFormat::Rgba8Unorm:
		case PixelFormat::Rgba8Srgb:
		case PixelFormat::Bgra8Unorm:
		case PixelFormat::Bgra8Srgb:
		case PixelFormat::A2b10g10r10Unorm:
		case PixelFormat::A2r10g10b10Unorm: return 4;
		case PixelFormat::Rgba16Float: return 8;
	}
	return 0;
}

struct Session::Private {
	std::map<uint64_t, FrameRecord> expected;
	std::map<uint64_t, FrameRecord> actual;
	bool                            failed    = false;
	bool                            finalized = false;
};

Session::Session(Options options)
    : m_options(std::move(options)), m_private(std::make_unique<Private>()) {}

Session::~Session() = default;

std::unique_ptr<Session> Session::Create(Options options, std::string& error) {
	error.clear();
	auto session = std::unique_ptr<Session>(new Session(std::move(options)));
	if (!session->Initialize(error)) {
		return {};
	}
	return session;
}

bool Session::Initialize(std::string& error) {
	if (m_options.baseline.empty() || m_options.frame_ordinals.empty() ||
	    (m_options.mode == Mode::Compare && m_options.report.empty())) {
		error = "regression baseline, report, and frame ordinals are required";
		return false;
	}
	if (m_options.frame_ordinals.size() > MaxManifestFrames) {
		error = "too many regression frame ordinals";
		return false;
	}
	std::sort(m_options.frame_ordinals.begin(), m_options.frame_ordinals.end());
	if (std::adjacent_find(m_options.frame_ordinals.begin(), m_options.frame_ordinals.end()) !=
	    m_options.frame_ordinals.end()) {
		error = "regression frame ordinals must be unique";
		return false;
	}
	if (m_options.mode == Mode::Record) {
		if (Common::File::IsFileExisting(m_options.baseline)) {
			error = "refusing to overwrite an existing regression baseline";
			return false;
		}
		return true;
	}
	if (ComparablePath(m_options.baseline) == ComparablePath(m_options.report) ||
	    ComparablePath(FrameDirectory(m_options.baseline)) ==
	        ComparablePath(FrameDirectory(m_options.report)) ||
	    SameOrWithin(m_options.report, FrameDirectory(m_options.baseline))) {
		error = "regression baseline and report outputs collide";
		return false;
	}

	std::string source;
	if (!ReadText(m_options.baseline, source)) {
		error = "cannot read regression baseline";
		return false;
	}
	auto json = nlohmann::json::parse(source, nullptr, false);
	if (json.is_discarded() || !json.is_object() || !json.contains("schema_version") ||
	    !json["schema_version"].is_number_unsigned() ||
	    json["schema_version"].get<uint64_t>() != SchemaVersion || !json.contains("algorithm") ||
	    !json["algorithm"].is_string() ||
	    json["algorithm"].get<std::string>() != "xxh3_128" || !json.contains("complete") ||
	    !json["complete"].is_boolean() || !json["complete"].get<bool>() ||
	    !json.contains("frames") || !json["frames"].is_array() ||
	    json["frames"].size() > MaxManifestFrames) {
		error = "invalid or incomplete regression baseline";
		return false;
	}
	for (const auto& entry: json["frames"]) {
		auto record = ParseRecord(entry, error);
		if (!error.empty()) {
			return false;
		}
		if (!m_private->expected.emplace(record.ordinal, std::move(record)).second) {
			error = "duplicate frame in regression baseline";
			return false;
		}
	}
	for (const auto ordinal: m_options.frame_ordinals) {
		if (!m_private->expected.contains(ordinal)) {
			error = "requested frame is missing from regression baseline";
			return false;
		}
	}
	std::unordered_set<std::string> protected_artifacts;
	for (const auto& [ordinal, record]: m_private->expected) {
		(void)ordinal;
		if (!record.raw_file.empty()) {
			protected_artifacts.insert(
			    ComparablePath(m_options.baseline.parent_path() / record.raw_file));
		}
		if (!record.image_file.empty()) {
			protected_artifacts.insert(
			    ComparablePath(m_options.baseline.parent_path() / record.image_file));
		}
	}
	if (protected_artifacts.contains(ComparablePath(m_options.report))) {
		error = "regression report would overwrite a baseline artifact";
		return false;
	}
	if (m_options.save_raw_frames) {
		const auto output_directory = FrameDirectory(m_options.report);
		for (const auto ordinal: m_options.frame_ordinals) {
			for (const auto& name: {FrameName(ordinal, "raw"), FrameName(ordinal, "ppm"),
			                       "diff_" + FrameName(ordinal, "ppm")}) {
				if (protected_artifacts.contains(ComparablePath(output_directory / name))) {
					error = "regression output would overwrite a baseline artifact";
					return false;
				}
			}
		}
	}
	return true;
}

bool Session::WantsFrame(uint64_t ordinal) const {
	return std::binary_search(m_options.frame_ordinals.begin(), m_options.frame_ordinals.end(),
	                          ordinal) &&
	       !m_private->actual.contains(ordinal);
}

bool Session::Observe(const FrameView& frame, std::string& error) {
	error.clear();
	if (!WantsFrame(frame.ordinal)) {
		error = "unexpected or duplicate regression frame";
		return false;
	}
	const auto bytes_per_pixel = BytesPerPixel(frame.format);
	if (frame.width == 0 || frame.height == 0 || bytes_per_pixel == 0 ||
	    frame.width > UINT32_MAX / bytes_per_pixel ||
	    frame.row_pitch != frame.width * bytes_per_pixel ||
	    frame.bytes.size() != static_cast<uint64_t>(frame.row_pitch) * frame.height) {
		error = "invalid regression frame layout";
		return false;
	}
	const auto hash = XXH3_128bits(frame.bytes.data(), frame.bytes.size());
	FrameRecord record;
	record.ordinal   = frame.ordinal;
	record.width     = frame.width;
	record.height    = frame.height;
	record.row_pitch = frame.row_pitch;
	record.byte_size = frame.bytes.size();
	record.format    = frame.format;
	record.digest    = {.low = hash.low64, .high = hash.high64};

	const auto& manifest = m_options.mode == Mode::Record ? m_options.baseline : m_options.report;
	if (m_options.save_raw_frames) {
		const auto directory = FrameDirectory(manifest);
		record.raw_file = (directory.filename() / FrameName(frame.ordinal, "raw")).generic_string();
		if (!WriteBytes(manifest.parent_path() / record.raw_file, frame.bytes.data(),
		                frame.bytes.size())) {
			error = "cannot write regression raw frame";
			return false;
		}
		if (frame.format == PixelFormat::Rgba8Unorm || frame.format == PixelFormat::Rgba8Srgb ||
		    frame.format == PixelFormat::Bgra8Unorm || frame.format == PixelFormat::Bgra8Srgb) {
			record.image_file =
			    (directory.filename() / FrameName(frame.ordinal, "ppm")).generic_string();
			if (!SavePpm(manifest.parent_path() / record.image_file, frame)) {
				error = "cannot write regression preview image";
				return false;
			}
		}
	}

	bool diff_write_failed = false;
	if (m_options.mode == Mode::Record) {
		record.status = "recorded";
	} else {
		const auto& expected = m_private->expected.at(frame.ordinal);
		record.expected     = expected.digest;
		record.has_expected = true;
		const bool match    = SameLayout(expected, record) && expected.digest == record.digest;
		record.status       = match ? "match" : "mismatch";
		m_private->failed   = m_private->failed || !match;
		if (!match && m_options.save_raw_frames && !expected.raw_file.empty() &&
		    SameLayout(expected, record)) {
			Common::File expected_file;
			const auto expected_path = m_options.baseline.parent_path() / expected.raw_file;
			if (expected_file.Open(expected_path, Common::File::Mode::Read) &&
			    expected_file.Size() == frame.bytes.size() && frame.bytes.size() <= UINT32_MAX) {
				std::vector<uint8_t> expected_bytes(frame.bytes.size());
				uint32_t              read = 0;
				expected_file.Read(expected_bytes.data(), static_cast<uint32_t>(expected_bytes.size()),
				                   &read);
				expected_file.Close();
				if (read == expected_bytes.size()) {
					const auto diff = FrameDirectory(m_options.report) /
					                  ("diff_" + FrameName(frame.ordinal, "ppm"));
					if (!SavePpm(diff, frame, expected_bytes)) {
						record.status     = "mismatch_diff_write_failed";
						diff_write_failed = true;
					}
				}
			} else if (!expected_file.IsInvalid()) {
				expected_file.Close();
			}
		}
	}
	m_private->actual.emplace(frame.ordinal, std::move(record));
	if (!WriteManifest(Complete(), error)) {
		return false;
	}
	if (diff_write_failed) {
		error = "cannot write regression difference image";
		return false;
	}
	return true;
}

bool Session::Complete() const {
	return m_private->actual.size() == m_options.frame_ordinals.size();
}

bool Session::WriteManifest(bool complete, std::string& error) const {
	nlohmann::ordered_json json;
	json["schema_version"] = SchemaVersion;
	json["algorithm"]      = "xxh3_128";
	json["mode"]           = m_options.mode == Mode::Record ? "record" : "compare";
	json["complete"]       = complete;
	json["passed"]         = complete && !m_private->failed;
	json["frames"]         = nlohmann::ordered_json::array();
	for (const auto ordinal: m_options.frame_ordinals) {
		if (const auto it = m_private->actual.find(ordinal); it != m_private->actual.end()) {
			json["frames"].push_back(RecordJson(it->second));
		} else {
			FrameRecord missing;
			missing.ordinal = ordinal;
			missing.status  = "missing";
			json["frames"].push_back(RecordJson(missing));
		}
	}
	const auto& path = m_options.mode == Mode::Record ? m_options.baseline : m_options.report;
	if (!WriteText(path, json.dump(2) + "\n")) {
		error = "cannot write regression manifest";
		return false;
	}
	return true;
}

int Session::Finalize(std::string& error) {
	error.clear();
	if (m_private->finalized) {
		return ExitCode();
	}
	m_private->finalized = true;
	if (!Complete()) {
		m_private->failed = true;
	}
	if (!WriteManifest(Complete(), error)) {
		m_private->failed = true;
		return FailureExitCode;
	}
	return ExitCode();
}

int Session::ExitCode() const {
	return m_private->failed ? FailureExitCode : 0;
}

} // namespace Libs::Graphics::Regression
