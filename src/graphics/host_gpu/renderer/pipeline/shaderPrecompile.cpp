#include "graphics/host_gpu/renderer/pipeline/shaderPrecompile.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <type_traits>
#include <vector>

namespace Libs::Graphics::ShaderPrecompile {

namespace {

constexpr char     Magic[8]      = {'K', 'Y', 'T', 'Y', 'S', 'H', 'D', 'R'};
constexpr uint32_t FormatVersion = 2;

std::mutex            g_mutex;
std::ofstream         g_out;
bool                  g_open = false;
std::atomic<uint64_t> g_count {0};

void Put(std::vector<uint8_t>& out, const void* data, size_t bytes) {
	const auto* src = static_cast<const uint8_t*>(data);
	out.insert(out.end(), src, src + bytes);
}

template <typename T>
void PutPod(std::vector<uint8_t>& out, const T& value) {
	static_assert(std::is_trivially_copyable_v<T>);
	Put(out, &value, sizeof(T));
}

void PutBytes(std::vector<uint8_t>& out, const void* data, size_t bytes) {
	PutPod(out, static_cast<uint32_t>(bytes));
	if (bytes != 0) {
		Put(out, data, bytes);
	}
}

// The three InputInfo structs are written field by field on purpose. Each embeds a
// ShaderStageRuntime (a pointer plus vectors) that must not be serialized, and it sits in the
// middle of ShaderVertexInputInfo; ShaderComputeInputInfo also inherits members, so it is not
// standard-layout and memcpy-around-the-hole via offsetof would be undefined.
void PutInfo(std::vector<uint8_t>& out, const ShaderVertexInputInfo& info) {
	PutPod(out, info.resources);
	PutPod(out, info.resources_dst);
	PutPod(out, info.buffers);
	PutPod(out, info.resources_num);
	PutPod(out, info.fetch_attrib_reg);
	PutPod(out, info.fetch_buffer_reg);
	PutPod(out, info.buffers_num);
	PutPod(out, info.scratch_size_dwords);
	PutPod(out, info.pa_cl_vs_out_cntl);
	PutPod(out, info.clip_space);
	PutPod(out, info.mesh);
	PutPod(out, info.fetch_external);
	PutPod(out, info.fetch_embedded);
	PutPod(out, info.wave_size);
}

void PutInfo(std::vector<uint8_t>& out, const ShaderPixelInputInfo& info) {
	PutPod(out, info.interpolator_settings);
	PutPod(out, info.input_num);
	PutPod(out, info.ps_system_input_base);
	PutPod(out, info.custom_interpolation_mask);
	PutPod(out, info.ps_perspective_center_vgpr);
	PutPod(out, info.target_output_mode);
	PutPod(out, info.target_export_mapping);
	PutPod(out, info.scratch_size_dwords);
	PutPod(out, info.wave_size);
	PutPod(out, info.ps_pos_x);
	PutPod(out, info.ps_pos_y);
	PutPod(out, info.ps_pos_z);
	PutPod(out, info.ps_pos_w);
	PutPod(out, info.ps_front_face);
	PutPod(out, info.ps_ancillary);
	PutPod(out, info.ps_no_perspective);
	PutPod(out, info.ps_pixel_kill_enable);
	PutPod(out, info.ps_depth_export_enable);
	PutPod(out, info.ps_sample_mask_export_enable);
	PutPod(out, info.ps_sample_shading);
	PutPod(out, info.ps_early_z);
	PutPod(out, info.ps_execute_on_noop);
}

void PutInfo(std::vector<uint8_t>& out, const ShaderComputeInputInfo& info) {
	PutPod(out, info.threads_num);
	PutPod(out, info.lds_size_dwords);
	PutPod(out, static_cast<const ShaderWorkgroupInputInfo&>(info).scratch_size_dwords);
	PutPod(out, info.host_subgroup_size);
	PutPod(out, info.wave_size);
	PutPod(out, info.dispatch_threads_num);
	PutPod(out, info.group_id);
	PutPod(out, info.dispatch_thread_dimensions);
	PutPod(out, info.thread_ids_num);
	PutPod(out, info.workgroup_register);
	PutPod(out, info.tg_size_en);
}

template <typename Info>
void RecordImpl(const ShaderParams& params, const ShaderRecompiler::CompileOptions& options,
                const ShaderRecompiler::IR::ResourceSpecialization& specialization,
                uint32_t push_data_start_dword, const Info& info) {
	std::scoped_lock lock(g_mutex);
	if (!g_open) {
		return;
	}

	std::vector<uint8_t> body;
	body.reserve(4096);
	PutPod(body, static_cast<uint8_t>(options.stage));
	PutPod(body, options.shader_hash);
	PutPod(body, push_data_start_dword);
	PutPod(body, options.user_data_base);
	PutPod(body, options.wave_size);

	PutBytes(body, params.code.data(), params.code.size_bytes());
	PutBytes(body, params.back_code.data(), params.back_code.size_bytes());
	PutBytes(body, params.user_data.data(), params.user_data_count * sizeof(uint32_t));

	PutPod(body, static_cast<uint32_t>(specialization.buffers.size()));
	for (const auto& buffer: specialization.buffers) {
		PutPod(body, buffer);
	}
	PutPod(body, static_cast<uint32_t>(specialization.images.size()));
	for (const auto& image: specialization.images) {
		PutPod(body, image);
	}

	PutInfo(body, info);

	std::vector<uint8_t> record;
	record.reserve(body.size() + sizeof(uint32_t));
	PutPod(record, static_cast<uint32_t>(body.size()));
	Put(record, body.data(), body.size());

	g_out.write(reinterpret_cast<const char*>(record.data()),
	            static_cast<std::streamsize>(record.size()));
	if (!g_out) {
		return;
	}
	// Flushed per record: an emulator crashes far more often than it exits cleanly, and a set
	// that only survives a graceful shutdown would rarely exist when it matters.
	g_out.flush();
	g_count.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

void Open(const std::filesystem::path& path, const std::string& recompiler_key, bool append) {
	std::scoped_lock lock(g_mutex);
	if (g_open || path.empty()) {
		return;
	}
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);

	g_out.open(path,
	           append ? (std::ios::binary | std::ios::app) : (std::ios::binary | std::ios::trunc));
	if (!g_out) {
		return;
	}
	if (!append) {
		std::vector<uint8_t> header;
		Put(header, Magic, sizeof(Magic));
		PutPod(header, FormatVersion);
		PutBytes(header, recompiler_key.data(), recompiler_key.size());
		g_out.write(reinterpret_cast<const char*>(header.data()),
		            static_cast<std::streamsize>(header.size()));
		if (!g_out) {
			g_out.close();
			return;
		}
		g_out.flush();
	}
	g_open = true;
}

void Close() {
	std::scoped_lock lock(g_mutex);
	if (!g_open) {
		return;
	}
	g_out.flush();
	g_out.close();
	g_open = false;
}

void Record(const ShaderParams& params, const ShaderRecompiler::CompileOptions& options,
            const ShaderRecompiler::IR::ResourceSpecialization& specialization,
            uint32_t push_data_start_dword, const ShaderVertexInputInfo& info) {
	RecordImpl(params, options, specialization, push_data_start_dword, info);
}

void Record(const ShaderParams& params, const ShaderRecompiler::CompileOptions& options,
            const ShaderRecompiler::IR::ResourceSpecialization& specialization,
            uint32_t push_data_start_dword, const ShaderPixelInputInfo& info) {
	RecordImpl(params, options, specialization, push_data_start_dword, info);
}

void Record(const ShaderParams& params, const ShaderRecompiler::CompileOptions& options,
            const ShaderRecompiler::IR::ResourceSpecialization& specialization,
            uint32_t push_data_start_dword, const ShaderComputeInputInfo& info) {
	RecordImpl(params, options, specialization, push_data_start_dword, info);
}

namespace {

struct Reader {
	const uint8_t* p   = nullptr;
	const uint8_t* end = nullptr;
	bool           ok  = true;

	void Take(void* dst, size_t bytes) {
		if (!ok || static_cast<size_t>(end - p) < bytes) {
			ok = false;
			return;
		}
		std::memcpy(dst, p, bytes);
		p += bytes;
	}
	template <typename T>
	void Into(T& dst) {
		static_assert(std::is_trivially_copyable_v<T>);
		Take(&dst, sizeof(T));
	}
	template <typename T>
	T Pod() {
		static_assert(std::is_trivially_copyable_v<T>);
		T value {};
		Take(&value, sizeof(T));
		return value;
	}
	std::vector<uint32_t> Words() {
		const auto            bytes = Pod<uint32_t>();
		std::vector<uint32_t> out;
		if (!ok || bytes % sizeof(uint32_t) != 0 || bytes > static_cast<size_t>(end - p)) {
			ok = false;
			return out;
		}
		out.resize(bytes / sizeof(uint32_t));
		Take(out.data(), bytes);
		return out;
	}
	std::string Text() {
		const auto  bytes = Pod<uint32_t>();
		std::string out;
		if (!ok || bytes > static_cast<size_t>(end - p)) {
			ok = false;
			return out;
		}
		out.resize(bytes);
		Take(out.data(), bytes);
		return out;
	}
};

void TakeInfo(Reader& r, ShaderVertexInputInfo& info) {
	r.Into(info.resources);
	r.Into(info.resources_dst);
	r.Into(info.buffers);
	info.resources_num       = r.Pod<int>();
	info.fetch_attrib_reg    = r.Pod<int>();
	info.fetch_buffer_reg    = r.Pod<int>();
	info.buffers_num         = r.Pod<int>();
	info.scratch_size_dwords = r.Pod<uint32_t>();
	info.pa_cl_vs_out_cntl   = r.Pod<uint32_t>();
	info.clip_space          = r.Pod<ShaderClipSpaceTransform>();
	info.mesh                = r.Pod<ShaderMeshInputInfo>();
	info.fetch_external      = r.Pod<bool>();
	info.fetch_embedded      = r.Pod<bool>();
	info.wave_size           = r.Pod<uint32_t>();
}

void TakeInfo(Reader& r, ShaderPixelInputInfo& info) {
	r.Into(info.interpolator_settings);
	info.input_num                  = r.Pod<uint32_t>();
	info.ps_system_input_base       = r.Pod<uint32_t>();
	info.custom_interpolation_mask  = r.Pod<uint32_t>();
	info.ps_perspective_center_vgpr = r.Pod<uint32_t>();
	r.Into(info.target_output_mode);
	info.target_export_mapping        = r.Pod<decltype(info.target_export_mapping)>();
	info.scratch_size_dwords          = r.Pod<uint32_t>();
	info.wave_size                    = r.Pod<uint32_t>();
	info.ps_pos_x                     = r.Pod<bool>();
	info.ps_pos_y                     = r.Pod<bool>();
	info.ps_pos_z                     = r.Pod<bool>();
	info.ps_pos_w                     = r.Pod<bool>();
	info.ps_front_face                = r.Pod<bool>();
	info.ps_ancillary                 = r.Pod<bool>();
	info.ps_no_perspective            = r.Pod<bool>();
	info.ps_pixel_kill_enable         = r.Pod<bool>();
	info.ps_depth_export_enable       = r.Pod<bool>();
	info.ps_sample_mask_export_enable = r.Pod<bool>();
	info.ps_sample_shading            = r.Pod<bool>();
	info.ps_early_z                   = r.Pod<bool>();
	info.ps_execute_on_noop           = r.Pod<bool>();
}

void TakeInfo(Reader& r, ShaderComputeInputInfo& info) {
	r.Into(info.threads_num);
	info.lds_size_dwords     = r.Pod<uint32_t>();
	info.scratch_size_dwords = r.Pod<uint32_t>();
	info.host_subgroup_size  = r.Pod<uint32_t>();
	info.wave_size           = r.Pod<uint32_t>();
	r.Into(info.dispatch_threads_num);
	r.Into(info.group_id);
	info.dispatch_thread_dimensions = r.Pod<bool>();
	info.thread_ids_num             = r.Pod<int>();
	info.workgroup_register         = r.Pod<int>();
	info.tg_size_en                 = r.Pod<bool>();
}

} // namespace

std::vector<PermutationRecord> Load(const std::filesystem::path& path,
                                    const std::string&           recompiler_key) {
	std::vector<PermutationRecord> records;
	std::ifstream                  in(path, std::ios::binary);
	if (!in) {
		return records;
	}
	const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)),
	                                std::istreambuf_iterator<char>());
	in.close();
	Reader r {blob.data(), blob.data() + blob.size(), true};

	char magic[sizeof(Magic)] {};
	r.Take(magic, sizeof(magic));
	if (!r.ok || std::memcmp(magic, Magic, sizeof(Magic)) != 0 ||
	    r.Pod<uint32_t>() != FormatVersion || r.Text() != recompiler_key) {
		return records;
	}

	size_t accepted_bytes = static_cast<size_t>(r.p - blob.data());
	while (r.ok && r.p != r.end) {
		const auto size = r.Pod<uint32_t>();
		if (!r.ok || static_cast<size_t>(r.end - r.p) < size) {
			break; // torn trailing record from a crash; keep everything before it
		}
		Reader body {r.p, r.p + size, true};
		r.p += size;

		PermutationRecord record;
		record.stage                 = static_cast<ShaderType>(body.Pod<uint8_t>());
		record.hash                  = body.Pod<uint64_t>();
		record.push_data_start_dword = body.Pod<uint32_t>();
		record.user_data_base        = body.Pod<uint32_t>();
		record.wave_size             = body.Pod<uint32_t>();
		record.code                  = body.Words();
		record.back_code             = body.Words();
		record.user_data             = body.Words();

		const auto buffers = body.Pod<uint32_t>();
		for (uint32_t i = 0; body.ok && i < buffers; i++) {
			record.specialization.buffers.push_back(
			    body.Pod<ShaderRecompiler::IR::ResourceSpecialization::Buffer>());
		}
		const auto images = body.Pod<uint32_t>();
		for (uint32_t i = 0; body.ok && i < images; i++) {
			record.specialization.images.push_back(
			    body.Pod<ShaderRecompiler::IR::ResourceSpecialization::Image>());
		}

		switch (record.stage) {
			case ShaderType::Vertex:
			case ShaderType::Mesh: {
				ShaderVertexInputInfo info;
				TakeInfo(body, info);
				record.info = info;
				break;
			}
			case ShaderType::Pixel: {
				ShaderPixelInputInfo info;
				TakeInfo(body, info);
				record.info = info;
				break;
			}
			case ShaderType::Compute: {
				ShaderComputeInputInfo info;
				TakeInfo(body, info);
				record.info = info;
				break;
			}
			default: body.ok = false; break;
		}

		if (!body.ok) {
			break;
		}
		records.push_back(std::move(record));
		accepted_bytes = static_cast<size_t>(r.p - blob.data());
	}

	// The caller reopens this file in append mode when anything was loaded, so a tail dropped
	// above has to go now. Left in place it is worse than lost bytes: the torn record's length
	// prefix survives the tear, so once later records sit behind it the length reads as
	// satisfiable again and the next load parses that span as one record, splicing the torn
	// remains together with the start of a real one.
	if (!records.empty() && accepted_bytes < blob.size()) {
		std::error_code ec;
		std::filesystem::resize_file(path, accepted_bytes, ec);
	}
	return records;
}

uint64_t RecordedCount() {
	return g_count.load(std::memory_order_relaxed);
}

} // namespace Libs::Graphics::ShaderPrecompile
