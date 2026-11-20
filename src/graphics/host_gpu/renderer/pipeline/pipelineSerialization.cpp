#include "common/logging/log.h"
#include "common/parallelFor.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/Serialize.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <mutex>
#include <ranges>
#include <xxhash.h>
#include <zstd.h>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// The shader store: a header, then zstd-compressed records of everything a run created first.
// Records are only appended, so a store cut short by a crash keeps every complete record.
namespace Libs::Graphics {

namespace {

using ShaderRecompiler::IR::IoAll;
using ShaderRecompiler::IR::Reader;
using ShaderRecompiler::IR::Writer;

enum class Record : uint8_t { Module, GraphicsPipeline, TessellationLayout };

// Precedes each record's zstd frame.
struct Frame {
	uint32_t compressed_size = 0;
	uint32_t raw_size        = 0;
	uint64_t checksum        = 0; // XXH3 of the compressed bytes.
};

// Explicit compatibility version for the shader store format and recompiler logic.
// Bump this constant when the IR serialization, SPIR-V recompiler passes, or cache structures
// change.
constexpr uint32_t SHADER_CACHE_COMPATIBILITY_VERSION = 1;

uint64_t CacheCompatibilityStamp() {
	// KYTY_SHADER_CACHE_STAMP=N keeps the shader cache across rebuilds.
	if (const char* env = std::getenv("KYTY_SHADER_CACHE_STAMP"); env != nullptr && env[0] != 0) {
		const auto value = std::strtoull(env, nullptr, 10);
		if (value != 0) {
			return value;
		}
	}
	// Magic "KPS5" (0x4B505335) in high 32 bits, explicit compatibility version in low 32 bits.
	return (0x4B505335ULL << 32) | static_cast<uint64_t>(SHADER_CACHE_COMPATIBILITY_VERSION);
}

// What every record depends on besides its own fields; empty when the build is unknown.
std::vector<uint8_t> StoreHeader(const GraphicContext& graphics) {
	const auto stamp = CacheCompatibilityStamp();
	Writer     header;
	IoAll(header, stamp, SpirvProfile(graphics),
	      graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize);
	return std::move(header.bytes);
}

uint32_t GetOptimalShaderWorkerThreads() {
	uint32_t cpu_threads = std::thread::hardware_concurrency();
	if (cpu_threads == 0) {
		cpu_threads = 4;
	}

	uint64_t available_ram_mb = 4096;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	MEMORYSTATUSEX mem_status {};
	mem_status.dwLength = sizeof(mem_status);
	if (GlobalMemoryStatusEx(&mem_status)) {
		available_ram_mb = mem_status.ullAvailPhys / (1024 * 1024);
	}
#elif defined(__linux__)
	const long pages     = sysconf(_SC_AVPHYS_PAGES);
	const long page_size = sysconf(_SC_PAGESIZE);
	if (pages > 0 && page_size > 0) {
		available_ram_mb = (static_cast<uint64_t>(pages) * page_size) / (1024 * 1024);
	}
#endif

	const uint64_t usable_ram_mb =
	    (available_ram_mb > 512) ? (available_ram_mb - 512) : available_ram_mb;
	const uint32_t max_threads_by_ram =
	    static_cast<uint32_t>(std::max<uint64_t>(1, usable_ram_mb / 256));

	const uint32_t target_cpu_threads = (cpu_threads > 4) ? (cpu_threads - 1) : cpu_threads;
	return std::clamp(target_cpu_threads, 1u, max_threads_by_ram);
}

void ReportShaderProgress(uint32_t current, uint32_t total, SDL_Window* window) {
	static std::mutex progress_mutex;
	static auto       last_update_time = std::chrono::steady_clock::now();

	const auto now      = std::chrono::steady_clock::now();
	const bool is_first = (current <= 1);
	const bool is_last  = (current >= total);

	std::scoped_lock lock(progress_mutex);
	if (!is_first && !is_last && (now - last_update_time) < std::chrono::milliseconds(40)) {
		return;
	}
	last_update_time = now;

	const float percent =
	    total > 0 ? (static_cast<float>(current) * 100.0f / static_cast<float>(total)) : 100.0f;
	const std::string text =
	    fmt::format("Preparando shaders: {} de {} ({:.1f}%)", current, total, percent);

	::printf("\r%-65s", text.c_str());
	::fflush(stdout);

	if (window != nullptr) {
		SDL_SetWindowTitle(window, ("Kyty - " + text).c_str());
	}
}

// Record fields, shared by writing and reading.
template <class S, class Key>
void IoKey(S& io, Key& key) {
	auto& [stage, hash, user_data_count, static_state] = key;
	IoAll(io, stage, hash, user_data_count, static_state);
}

template <class S, class Module>
void IoModule(S& io, Module& stage) {
	// The handle comes from the SPIR-V; stored programs have no function code.
	auto& [id, handle, info, specialization, function_code, vertex_formats, rect_parameters,
	       mesh_task] = stage;
	IoAll(io, id, info, specialization, vertex_formats, rect_parameters, mesh_task);
}

} // namespace

void ShaderStoreWriter::Open(const std::filesystem::path& path, std::span<const uint8_t> header,
                             uint64_t kept) {
	Common::File::CreateDirectories(path.parent_path());
	if (kept == 0) {
		if (m_file.Create(path)) {
			m_file.Write(header.data(), static_cast<uint32_t>(header.size()));
			m_file.Flush();
		}
	} else if (m_file.Open(path, Common::File::Mode::ReadWrite)) {
		m_file.Truncate(kept);
		m_file.Seek(kept);
	}
}

ShaderStoreWriter::ShaderStoreWriter() {
	m_thread = std::jthread([this](std::stop_token stop) {
		std::unique_lock lock(m_mutex);
		// Once stop is requested the wait returns false only when the queue is empty.
		while (m_ready.wait(lock, stop, [this] { return !m_jobs.empty(); })) {
			const auto job = std::move(m_jobs.front());
			m_jobs.pop();
			lock.unlock();
			job();
			lock.lock();
		}
	});
}

void ShaderStoreWriter::Append(std::vector<uint8_t>&& record) {
	Post([this, record = std::move(record)] {
		std::vector<uint8_t> bytes(sizeof(Frame) + ZSTD_compressBound(record.size()));
		const auto size = ZSTD_compress(bytes.data() + sizeof(Frame), bytes.size() - sizeof(Frame),
		                                record.data(), record.size(), 3);
		EXIT_IF(ZSTD_isError(size));
		const Frame frame {.compressed_size = static_cast<uint32_t>(size),
		                   .raw_size        = static_cast<uint32_t>(record.size()),
		                   .checksum        = XXH3_64bits(bytes.data() + sizeof(Frame), size)};
		std::memcpy(bytes.data(), &frame, sizeof(frame));
		// A failed write leaves a partial record; the next run drops it and everything after it.
		m_file.Write(bytes.data(), static_cast<uint32_t>(sizeof(Frame) + size));
		m_file.Flush();
	});
}

void ShaderStoreWriter::Post(Common::UniqueFunction<void>&& job) {
	{
		std::scoped_lock lock(m_mutex);
		m_jobs.push(std::move(job));
	}
	m_ready.notify_one();
}

void PipelineCache::WarmUp(const std::filesystem::path& path) {
	if (path.empty()) {
		return;
	}
	const auto header = StoreHeader(m_graphics);
	if (header.empty()) {
		return;
	}
	const auto           start = std::chrono::steady_clock::now();
	std::vector<uint8_t> bytes;
	if (Common::File file;
	    Common::File::IsFileExisting(path) && file.Open(path, Common::File::Mode::Read)) {
		bytes.resize(file.Size());
		uint32_t read = 0;
		file.Read(bytes.data(), static_cast<uint32_t>(bytes.size()), &read);
		bytes.resize(read);
	}

	auto       records = std::span<const uint8_t>(bytes);
	const bool reusable =
	    records.size() >= header.size() && std::ranges::equal(records.first(header.size()), header);
	if (reusable) {
		records = records.subspan(header.size());
		while (LoadRecord(records)) {
		}
	}
	// A damaged record ends the store; the records before it stay.
	m_store.Open(path, header, reusable ? bytes.size() - records.size() : 0);

	// Collect uncompiled modules that were loaded from records
	std::vector<ShaderModule*> pending_modules;
	size_t                     compute_count = 0;
	for (auto& [key, program]: m_program_cache) {
		for (auto& module: program.modules) {
			if (module.module == nullptr && !module.function_code.empty()) {
				pending_modules.push_back(&module);
				if (module.info.stage == ShaderType::Compute) {
					compute_count++;
				}
			}
		}
	}

	const uint32_t total_items =
	    static_cast<uint32_t>(pending_modules.size() + m_graphics_pipelines.size() + compute_count);
	// Several threads compiling pipelines at once trips a crash inside NVIDIA's
	// libnvidia-glvkspirv, so that driver warms up serially; elsewhere the work stays spread
	// over the pool.
	const bool            is_nvidia = m_graphics.GetPhysicalDeviceProperties().vendorID == 0x10DEu;
	const uint32_t        num_threads = is_nvidia ? 1u : GetOptimalShaderWorkerThreads();
	std::atomic<uint32_t> completed   = 0;

	if (total_items > 0) {
		Log::WriteToConsoleAndLog(fmt::format("Shader cache: preparando {} shaders y pipelines "
		                                      "(concurrencia: {} hilos, archivo: {:.1f} MB)\n",
		                                      total_items, num_threads,
		                                      static_cast<double>(bytes.size()) / 1'000'000.0));
		ReportShaderProgress(0, total_items, m_graphics.window);

		// Step 1: Compile all SPIR-V shader modules in parallel
		Common::ParallelFor(pending_modules.size(), num_threads, [&](size_t i) {
			auto* mod   = pending_modules[i];
			mod->module = CompileSPV(mod->function_code, m_graphics.device);
			std::vector<uint32_t>().swap(mod->function_code);
			const uint32_t done = ++completed;
			ReportShaderProgress(done, total_items, m_graphics.window);
		});

		// Step 2: Create all graphics and compute pipelines
		const auto                                modules = MapModules();
		std::vector<Common::UniqueFunction<void>> jobs;
		jobs.reserve(m_graphics_pipelines.size() + compute_count);
		for (auto& node: m_graphics_pipelines) {
			jobs.emplace_back([this, &modules, &node, &completed, total_items] {
				CreateGraphicsPipeline(node.second, node.first, modules);
				const uint32_t done = ++completed;
				ReportShaderProgress(done, total_items, m_graphics.window);
			});
		}
		// A compute pipeline depends only on its module.
		for (const auto* module: modules | std::views::values) {
			if (module != nullptr && module->info.stage == ShaderType::Compute) {
				jobs.emplace_back([this, module, &pipeline = m_compute_pipelines[module->id],
				                   &completed, total_items] {
					CreatePipelineInternal(m_graphics, pipeline, *module, m_driver_cache);
					const uint32_t done = ++completed;
					ReportShaderProgress(done, total_items, m_graphics.window);
				});
			}
		}
		if (is_nvidia) {
			for (auto& job: jobs) {
				job();
			}
		} else {
			Common::ParallelFor(jobs.size(), num_threads, [&](size_t i) { jobs[i](); });
		}

		::printf("\r%-65s\n", fmt::format("Preparando shaders: {} de {} (100.0%) - Completado",
		                                  total_items, total_items)
		                          .c_str());
		::fflush(stdout);

		if (m_graphics.window != nullptr) {
			SDL_SetWindowTitle(m_graphics.window, "Kyty");
		}
	} else {
		Log::WriteToConsoleAndLog(
		    "Shader cache: no hay shaders previos guardados para preparar.\n");
	}

	// A key with a stage missing from the store keeps no pipeline.
	std::erase_if(m_graphics_pipelines,
	              [](const auto& entry) { return entry.second.pipeline == nullptr; });
	if (!m_graphics_pipelines.empty() || !m_compute_pipelines.empty()) {
		CheckpointDriverCache();
	}

	const auto modules = MapModules();
	Log::WriteToConsoleAndLog(fmt::format("Shader cache: preparación obligatoria finalizada ({} "
	                                      "programas, {} módulos, {} pipelines en {} ms)\n",
	                                      m_program_cache.size(), modules.size() - 1,
	                                      m_graphics_pipelines.size() + m_compute_pipelines.size(),
	                                      std::chrono::duration_cast<std::chrono::milliseconds>(
	                                          std::chrono::steady_clock::now() - start)
	                                          .count()));
}

bool PipelineCache::LoadRecord(std::span<const uint8_t>& records) {
	Frame frame;
	if (records.size() < sizeof(frame)) {
		return false;
	}
	std::memcpy(&frame, records.data(), sizeof(frame));
	if (records.size() - sizeof(frame) < frame.compressed_size) {
		return false;
	}
	const auto compressed = records.subspan(sizeof(frame), frame.compressed_size);
	if (XXH3_64bits(compressed.data(), compressed.size()) != frame.checksum ||
	    ZSTD_getFrameContentSize(compressed.data(), compressed.size()) != frame.raw_size) {
		return false;
	}
	std::vector<uint8_t> raw(frame.raw_size);
	if (ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size()) !=
	    raw.size()) {
		return false;
	}
	Reader     reader {raw};
	auto       type     = Record::Module;
	const auto complete = [&] { return reader.ok && reader.bytes.empty(); };
	IoAll(reader, type);
	switch (type) {
		case Record::Module: {
			ProgramKey            key;
			std::vector<uint8_t>  plan;
			ShaderModule          module;
			std::vector<uint32_t> spirv;
			IoKey(reader, key);
			IoAll(reader, plan);
			IoModule(reader, module);
			IoAll(reader, spirv);
			if (!complete()) {
				return false;
			}
			const auto [program, inserted] = m_program_cache.try_emplace(std::move(key));
			if (inserted &&
			    !ShaderRecompiler::IR::LoadResourcePlan(plan, program->second.resource_plan)) {
				m_program_cache.erase(program);
				return false;
			}
			module.function_code = std::move(spirv);
			program->second.modules.push_back(std::move(module));
			break;
		}
		case Record::GraphicsPipeline: {
			GraphicsPipelineKey key;
			reader.Raw(&key, sizeof(key));
			if (!complete()) {
				return false;
			}
			// WarmUp creates the pipeline once every record is loaded.
			m_graphics_pipelines.try_emplace(key);
			break;
		}
		case Record::TessellationLayout: {
			uint64_t                    key = 0;
			ShaderTessellationInputInfo layout;
			IoAll(reader, key, layout);
			if (!complete()) {
				return false;
			}
			m_tess_layouts.try_emplace(key, layout);
			break;
		}
		default: return false;
	}
	records = records.subspan(sizeof(frame) + compressed.size());
	return true;
}

// Records are encoded here, since programs and modules change later.
void PipelineCache::StoreModule(const ProgramKey& key, const Program& program,
                                const ShaderModule& module, const std::vector<uint32_t>& spirv) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::Module);
	IoKey(record, key);
	IoAll(record, ShaderRecompiler::IR::SaveResourcePlan(program.resource_plan));
	IoModule(record, module);
	IoAll(record, spirv);
	m_store.Append(std::move(record.bytes));
}

void PipelineCache::StorePipeline(const GraphicsPipelineKey& key) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::GraphicsPipeline);
	record.Raw(&key, sizeof(key));
	m_store.Append(std::move(record.bytes));
}

void PipelineCache::StoreLayout(uint64_t key, const ShaderTessellationInputInfo& layout) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::TessellationLayout, key, layout);
	m_store.Append(std::move(record.bytes));
}

PipelineCache::ModuleMap PipelineCache::MapModules() const {
	ModuleMap modules {{0, nullptr}};
	for (const auto& program: m_program_cache | std::views::values) {
		for (const auto& module: program.modules) {
			modules.emplace(module.id, &module);
		}
	}
	return modules;
}

bool PipelineCache::CreateGraphicsPipeline(Pipeline& pipeline, const GraphicsPipelineKey& key,
                                           const ModuleMap& modules) {
	std::array<const ShaderModule*, 4> stages {};
	for (size_t i = 0; i < stages.size(); i++) {
		const auto found = modules.find(key.stage_ids[i]);
		if (found == modules.end()) {
			return false;
		}
		stages[i] = found->second;
	}
	CreatePipelineInternal(m_graphics, pipeline, key, stages, m_driver_cache);
	return true;
}

} // namespace Libs::Graphics
