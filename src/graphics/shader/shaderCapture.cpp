#include "graphics/shader/shaderCapture.h"

#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "kytyGitVersion.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fmt/format.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string_view>
#include <type_traits>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

// Raw input dumps copy the struct byte for byte, which is only sound for trivially copyable types.
static_assert(std::is_trivially_copyable_v<ShaderPixelInputInfo>);
static_assert(std::is_trivially_copyable_v<ShaderVertexInputInfo>);

constexpr std::array<std::pair<ShaderType, const char*>, 7> STAGE_NAMES {{
    {ShaderType::Vertex, "vs"},
    {ShaderType::Mesh, "ms"},
    {ShaderType::Local, "ls"},
    {ShaderType::TessellationControl, "hs"},
    {ShaderType::TessellationEvaluation, "ds"},
    {ShaderType::Pixel, "ps"},
    {ShaderType::Compute, "cs"},
}};

thread_local ShaderCapture* t_active_capture = nullptr;

std::string Hex64(uint64_t value) {
	return fmt::format("0x{:016x}", value);
}

bool ParseHex64(const nlohmann::json& json, const char* key, uint64_t& out) {
	if (!json.is_object() || !json.contains(key)) {
		return false;
	}
	const auto& value = json[key];
	if (value.is_number_unsigned()) {
		out = value.get<uint64_t>();
		return true;
	}
	if (!value.is_string()) {
		return false;
	}
	const auto text = value.get<std::string>();
	const char* begin = text.c_str();
	if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		begin += 2;
	}
	char* end = nullptr;
	const auto parsed = std::strtoull(begin, &end, 16);
	if (end == begin || *end != '\0') {
		return false;
	}
	out = parsed;
	return true;
}

uint32_t JsonU32(const nlohmann::json& json, const char* key, uint32_t fallback) {
	if (!json.is_object() || !json.contains(key) || !json[key].is_number_unsigned()) {
		return fallback;
	}
	return json[key].get<uint32_t>();
}

bool JsonBool(const nlohmann::json& json, const char* key, bool fallback) {
	if (!json.is_object() || !json.contains(key) || !json[key].is_boolean()) {
		return fallback;
	}
	return json[key].get<bool>();
}

std::string JsonString(const nlohmann::json& json, const char* key) {
	if (!json.is_object() || !json.contains(key) || !json[key].is_string()) {
		return {};
	}
	return json[key].get<std::string>();
}

bool JsonU32Array(const nlohmann::json& json, const char* key, uint32_t* out, size_t count) {
	if (!json.is_object() || !json.contains(key) || !json[key].is_array() ||
	    json[key].size() != count) {
		return false;
	}
	for (size_t i = 0; i < count; i++) {
		if (!json[key][i].is_number_unsigned()) {
			return false;
		}
		out[i] = json[key][i].get<uint32_t>();
	}
	return true;
}

bool WriteBinary(const std::filesystem::path& path, const void* data, size_t size) {
	Common::File file(path);
	if (file.IsInvalid()) {
		return false;
	}
	uint32_t written = 0;
	file.Write(data, static_cast<uint32_t>(size), &written);
	return written == size;
}

bool ReadBinary(const std::filesystem::path& path, std::vector<uint8_t>& out) {
	if (!Common::File::IsFileExisting(path)) {
		return false;
	}
	Common::File file(path, Common::File::Mode::Read);
	if (file.IsInvalid()) {
		return false;
	}
	const auto size = file.Size();
	if (size > UINT32_MAX) {
		return false;
	}
	out.resize(static_cast<size_t>(size));
	uint32_t read = 0;
	if (!out.empty()) {
		file.Read(out.data(), static_cast<uint32_t>(out.size()), &read);
	}
	return read == out.size();
}

bool ReadWords(const std::filesystem::path& path, std::vector<uint32_t>& out, bool required,
               std::string& error) {
	std::vector<uint8_t> bytes;
	if (!ReadBinary(path, bytes)) {
		if (required) {
			error = fmt::format("cannot read {}", Common::PathToString(path));
		}
		return !required;
	}
	if (bytes.size() % sizeof(uint32_t) != 0) {
		error = fmt::format("{} is not a whole number of dwords", Common::PathToString(path));
		return false;
	}
	out.resize(bytes.size() / sizeof(uint32_t));
	if (!bytes.empty()) {
		std::memcpy(out.data(), bytes.data(), bytes.size());
	}
	return true;
}

nlohmann::json ComputeInputToJson(const ShaderComputeInputInfo& info) {
	nlohmann::json json;
	json["kind"]                       = "compute";
	json["workgroup_register"]         = info.workgroup_register;
	json["wave_size"]                  = info.wave_size;
	json["float_mode"]                 = static_cast<uint32_t>(info.float_mode);
	json["host_subgroup_size"]         = info.host_subgroup_size;
	json["thread_ids_num"]             = info.thread_ids_num;
	json["lds_size_dwords"]            = info.lds_size_dwords;
	json["scratch_size_dwords"]        = info.scratch_size_dwords;
	json["dispatch_thread_dimensions"] = info.dispatch_thread_dimensions;
	json["tg_size_en"]                 = info.tg_size_en;
	json["threads_num"] = {info.threads_num[0], info.threads_num[1], info.threads_num[2]};
	json["dispatch_threads_num"] = {info.dispatch_threads_num[0], info.dispatch_threads_num[1],
	                                info.dispatch_threads_num[2]};
	json["group_id"] = {info.group_id[0], info.group_id[1], info.group_id[2]};
	return json;
}

bool ComputeInputFromJson(const nlohmann::json& json, ShaderComputeInputInfo& info) {
	if (!json.is_object()) {
		return false;
	}
	info                            = {};
	info.workgroup_register         = static_cast<int>(JsonU32(json, "workgroup_register", 0));
	info.wave_size                  = JsonU32(json, "wave_size", info.wave_size);
	info.float_mode                 = static_cast<uint8_t>(JsonU32(json, "float_mode", info.float_mode));
	info.host_subgroup_size         = JsonU32(json, "host_subgroup_size", info.host_subgroup_size);
	info.thread_ids_num             = static_cast<int>(JsonU32(json, "thread_ids_num", 0));
	info.lds_size_dwords            = JsonU32(json, "lds_size_dwords", 0);
	info.scratch_size_dwords        = JsonU32(json, "scratch_size_dwords", 0);
	info.dispatch_thread_dimensions = JsonBool(json, "dispatch_thread_dimensions", false);
	info.tg_size_en                 = JsonBool(json, "tg_size_en", false);
	if (!JsonU32Array(json, "threads_num", info.threads_num, 3) ||
	    !JsonU32Array(json, "dispatch_threads_num", info.dispatch_threads_num, 3)) {
		return false;
	}
	if (!json.contains("group_id") || !json["group_id"].is_array() || json["group_id"].size() != 3) {
		return false;
	}
	for (size_t i = 0; i < 3; i++) {
		if (!json["group_id"][i].is_boolean()) {
			return false;
		}
		info.group_id[i] = json["group_id"][i].get<bool>();
	}
	return true;
}

template <typename InputInfo>
std::vector<uint8_t> RawInputBytes(const InputInfo& input) {
	// The runtime pointers belong to this process and mean nothing to a replay.
	InputInfo scrubbed = input;
	scrubbed.stage     = {};
	std::vector<uint8_t> bytes(sizeof(InputInfo));
	std::memcpy(bytes.data(), &scrubbed, sizeof(InputInfo));
	return bytes;
}

} // namespace

const char* ShaderCaptureStageName(ShaderType stage) {
	for (const auto& [type, name]: STAGE_NAMES) {
		if (type == stage) {
			return name;
		}
	}
	return "unknown";
}

bool ShaderCaptureStageFromName(const std::string& name, ShaderType& stage) {
	for (const auto& [type, text]: STAGE_NAMES) {
		if (name == text) {
			stage = type;
			return true;
		}
	}
	return false;
}

uint32_t ShaderCaptureKey(ShaderType stage, uint64_t hash, uint32_t user_data_count,
                          uint32_t code_size_words, std::span<const uint32_t> static_state) {
	XXH3_state_t* state = XXH3_createState();
	XXH3_64bits_reset(state);
	const auto stage_id = static_cast<uint32_t>(stage);
	XXH3_64bits_update(state, &stage_id, sizeof(stage_id));
	XXH3_64bits_update(state, &hash, sizeof(hash));
	XXH3_64bits_update(state, &user_data_count, sizeof(user_data_count));
	XXH3_64bits_update(state, &code_size_words, sizeof(code_size_words));
	if (!static_state.empty()) {
		XXH3_64bits_update(state, static_state.data(), static_state.size_bytes());
	}
	const auto digest = XXH3_64bits_digest(state);
	XXH3_freeState(state);
	return static_cast<uint32_t>(digest ^ (digest >> 32u));
}

ShaderCapture::ShaderCapture(const ShaderCaptureSource& source, const ShaderComputeInputInfo& input) {
	Begin(source, nullptr, 0, &input);
}

ShaderCapture::ShaderCapture(const ShaderCaptureSource& source, const ShaderPixelInputInfo& input) {
	const auto bytes = RawInputBytes(input);
	Begin(source, bytes.data(), bytes.size(), nullptr);
}

ShaderCapture::ShaderCapture(const ShaderCaptureSource& source, const ShaderVertexInputInfo& input) {
	const auto bytes = RawInputBytes(input);
	Begin(source, bytes.data(), bytes.size(), nullptr);
}

ShaderCapture::~ShaderCapture() {
	if (t_active_capture == this) {
		t_active_capture = m_previous;
	}
}

void ShaderCapture::Begin(const ShaderCaptureSource& source, const void* raw_input,
                          size_t raw_input_size, const ShaderComputeInputInfo* compute) {
	const auto root = Config::GetShaderCaptureDir();
	if (root.empty() || source.code.empty()) {
		return;
	}

	const auto key = ShaderCaptureKey(source.stage, source.hash,
	                                  static_cast<uint32_t>(source.user_data.size()),
	                                  static_cast<uint32_t>(source.code.size()), source.static_state);
	m_dir = root / fmt::format("{}_{:016x}_{:08x}", ShaderCaptureStageName(source.stage),
	                           source.hash, key);
	if (!Common::File::CreateDirectories(m_dir) && !Common::File::IsDirectoryExisting(m_dir)) {
		LOGF("ShaderCapture: cannot create %s\n", Common::PathToString(m_dir).c_str());
		m_dir.clear();
		return;
	}

	bool ok = WriteBinary(m_dir / "code.bin", source.code.data(), source.code.size_bytes());
	if (!source.back_code.empty()) {
		ok = WriteBinary(m_dir / "back_code.bin", source.back_code.data(),
		                 source.back_code.size_bytes()) && ok;
	}
	ok = WriteBinary(m_dir / "user_data.bin", source.user_data.data(),
	                 source.user_data.size_bytes()) && ok;
	if (raw_input != nullptr) {
		ok = WriteBinary(m_dir / "input_info.bin", raw_input, raw_input_size) && ok;
	}

	nlohmann::json json;
	json["format"]          = SHADER_CAPTURE_FORMAT;
	json["stage"]           = ShaderCaptureStageName(source.stage);
	json["hash"]            = Hex64(source.hash);
	json["key"]             = fmt::format("{:08x}", key);
	json["wave_size"]       = source.wave_size;
	json["user_data_base"]  = source.user_data_base;
	json["user_data_count"] = static_cast<uint32_t>(source.user_data.size());
	json["code_size_bytes"] = static_cast<uint32_t>(source.code.size_bytes());
	json["back_code_size_bytes"] = static_cast<uint32_t>(source.back_code.size_bytes());
	json["shader_base"]     = Hex64(source.shader_base);
	json["static_state"]    = std::vector<uint32_t>(source.static_state.begin(), source.static_state.end());
	if (compute != nullptr) {
		json["input"] = ComputeInputToJson(*compute);
		json["replay"] = true;
	} else {
		json["input"] = {{"kind", "raw"},
		                 {"file", "input_info.bin"},
		                 {"size", static_cast<uint32_t>(raw_input_size)}};
		// A raw struct dump is the layout of the build that wrote it.
		json["replay"] = true;
		json["replay_note"] = "raw input struct: replay with the same build that captured it";
	}
	json["git_revision"] = KYTY_GIT_REVISION;
	json["git_hash"]     = KYTY_GIT_HASH;
	json["build"]        = KYTY_BUILD_LABEL;
	const auto text      = json.dump(2) + "\n";
	ok                   = WriteBinary(m_dir / "manifest.json", text.data(), text.size()) && ok;

	m_reads = std::make_unique<Common::File>(m_dir / "reads.bin");
	if (m_reads->IsInvalid()) {
		m_reads.reset();
		ok = false;
	}
	if (!ok) {
		LOGF("ShaderCapture: incomplete capture in %s\n", Common::PathToString(m_dir).c_str());
	}

	m_previous       = t_active_capture;
	t_active_capture = this;
}

void ShaderCapture::RecordRead(uint64_t address, std::span<const uint32_t> values, bool ok) {
	if (t_active_capture != nullptr) {
		t_active_capture->Append(address, values, ok);
	}
}

void ShaderCapture::Append(uint64_t address, std::span<const uint32_t> values, bool ok) {
	if (m_reads == nullptr) {
		return;
	}
	// One record: address, dword count, ok flag, then the data (only when the read succeeded).
	std::vector<uint8_t> record(16 + (ok ? values.size_bytes() : 0));
	const uint32_t       dwords = static_cast<uint32_t>(values.size());
	const uint32_t       flag   = ok ? 1u : 0u;
	std::memcpy(record.data(), &address, sizeof(address));
	std::memcpy(record.data() + 8, &dwords, sizeof(dwords));
	std::memcpy(record.data() + 12, &flag, sizeof(flag));
	if (ok && !values.empty()) {
		std::memcpy(record.data() + 16, values.data(), values.size_bytes());
	}
	m_reads->Write(record.data(), static_cast<uint32_t>(record.size()));
	// A fatal exit skips stdio teardown, so every record has to reach the file immediately.
	m_reads->Flush();
}

bool LoadShaderCapture(const std::filesystem::path& dir, ShaderCaptureData& out,
                       std::string& error) {
	out = {};
	std::vector<uint8_t> manifest_bytes;
	if (!ReadBinary(dir / "manifest.json", manifest_bytes)) {
		error = fmt::format("{} has no manifest.json", Common::PathToString(dir));
		return false;
	}
	const auto json = nlohmann::json::parse(manifest_bytes.begin(), manifest_bytes.end(), nullptr, false);
	if (!json.is_object()) {
		error = "manifest.json is not valid JSON";
		return false;
	}
	if (JsonU32(json, "format", 0) != SHADER_CAPTURE_FORMAT) {
		error = fmt::format("unsupported capture format {}", JsonU32(json, "format", 0));
		return false;
	}
	if (!ShaderCaptureStageFromName(JsonString(json, "stage"), out.stage)) {
		error = fmt::format("unknown stage '{}'", JsonString(json, "stage"));
		return false;
	}
	if (!ParseHex64(json, "hash", out.hash)) {
		error = "manifest has no valid hash";
		return false;
	}
	uint64_t key = 0;
	if (ParseHex64(json, "key", key)) {
		out.key = static_cast<uint32_t>(key);
	}
	out.wave_size      = JsonU32(json, "wave_size", 64);
	out.user_data_base = JsonU32(json, "user_data_base", 0);
	ParseHex64(json, "shader_base", out.shader_base);
	out.git_revision = JsonString(json, "git_revision");
	if (json.contains("static_state") && json["static_state"].is_array()) {
		for (const auto& word: json["static_state"]) {
			if (word.is_number_unsigned()) {
				out.static_state.push_back(word.get<uint32_t>());
			}
		}
	}

	if (!ReadWords(dir / "code.bin", out.code, true, error) || out.code.empty()) {
		if (error.empty()) {
			error = "code.bin is empty";
		}
		return false;
	}
	if (!ReadWords(dir / "back_code.bin", out.back_code, false, error) ||
	    !ReadWords(dir / "user_data.bin", out.user_data, false, error)) {
		return false;
	}
	const auto expected_user_data = JsonU32(json, "user_data_count", static_cast<uint32_t>(out.user_data.size()));
	if (expected_user_data != out.user_data.size()) {
		error = fmt::format("user_data.bin has {} dwords, manifest says {}", out.user_data.size(),
		                    expected_user_data);
		return false;
	}

	const auto& input = json.contains("input") ? json["input"] : nlohmann::json();
	const auto  kind  = JsonString(input, "kind");
	if (kind == "compute") {
		if (out.stage != ShaderType::Compute || !ComputeInputFromJson(input, out.compute)) {
			error = "manifest compute input is malformed";
			return false;
		}
	} else if (kind == "raw") {
		out.raw_input      = true;
		out.raw_input_size = JsonU32(input, "size", 0);
		if (!ReadBinary(dir / JsonString(input, "file"), out.raw_input_bytes) ||
		    out.raw_input_bytes.size() != out.raw_input_size) {
			error = "input_info.bin is missing or has the wrong size";
			return false;
		}
	} else {
		error = fmt::format("unknown input kind '{}'", kind);
		return false;
	}

	std::vector<uint8_t> reads;
	if (ReadBinary(dir / "reads.bin", reads)) {
		size_t offset = 0;
		while (reads.size() - offset >= 16) {
			ShaderCaptureRead read;
			uint32_t          dwords = 0;
			uint32_t          flag   = 0;
			std::memcpy(&read.address, reads.data() + offset, sizeof(uint64_t));
			std::memcpy(&dwords, reads.data() + offset + 8, sizeof(uint32_t));
			std::memcpy(&flag, reads.data() + offset + 12, sizeof(uint32_t));
			offset += 16;
			read.ok = flag != 0;
			const size_t payload = read.ok ? static_cast<size_t>(dwords) * sizeof(uint32_t) : 0;
			if (reads.size() - offset < payload) {
				// The writer was killed mid-record. Everything before it is still good.
				break;
			}
			if (read.ok) {
				read.values.resize(dwords);
				if (payload != 0) {
					std::memcpy(read.values.data(), reads.data() + offset, payload);
				}
			} else {
				read.values.assign(dwords, 0u);
			}
			offset += payload;
			out.reads.push_back(std::move(read));
		}
	}
	return true;
}

} // namespace Libs::Graphics
