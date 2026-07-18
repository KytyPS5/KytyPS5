#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_ASYNCPIPELINEBUILDER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_ASYNCPIPELINEBUILDER_H_

#include "graphics/host_gpu/renderer/pipelineCache.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include <vulkan/vulkan_core.h>
#include <thread>
#include <stop_token>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <condition_variable>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <fstream>
#include <algorithm>
#include <shared_mutex>
#include <chrono>

namespace Libs::Graphics {

extern std::shared_mutex g_pipeline_cache_mutex;

class AsyncPipelineBuilder {
public:
	enum class Status {
		NotFound,
		Pending,
		Completed,
		Error
	};

	struct Result {
		Status           status          = Status::Pending;
		VkPipeline       pipeline        = nullptr;
		VkPipelineLayout pipeline_layout = nullptr;
	};

	AsyncPipelineBuilder(VkDevice device, VkPhysicalDevice physical_device, const std::string& cache_path)
	    : m_device(device), m_physical_device(physical_device), m_cache_path(cache_path),
	      m_last_save_time(std::chrono::steady_clock::now()) {

		LoadCache();

		// Spawn background worker threads
		unsigned int thread_count = std::max(1u, std::thread::hardware_concurrency());
		for (unsigned int i = 0; i < thread_count; ++i) {
			m_workers.emplace_back([this](std::stop_token stop_token) {
				WorkerLoop(stop_token);
			});
		}
	}

	~AsyncPipelineBuilder() {
		{
			std::lock_guard<std::mutex> lock(m_queue_mutex);
			m_stop = true;
		}
		m_cv.notify_all();
		m_workers.clear(); // std::jthread automatically joins

		SaveCache();

		if (m_pipeline_cache != nullptr) {
			std::unique_lock<std::shared_mutex> lock(g_pipeline_cache_mutex);
			vkDestroyPipelineCache(m_device, m_pipeline_cache, nullptr);
		}
	}

	VkPipelineCache GetVkPipelineCache() const {
		return m_pipeline_cache;
	}

	Status QueryPipeline(const PipelineCache::GraphicsPipelineKey& key, VkPipeline& out_pipeline, VkPipelineLayout& out_layout) {
		std::lock_guard<std::mutex> lock(m_map_mutex);
		auto                        it = m_results.find(key);
		if (it != m_results.end()) {
			out_pipeline = it->second.pipeline;
			out_layout   = it->second.pipeline_layout;
			return it->second.status;
		}
		return Status::NotFound;
	}

	void RequestPipeline(
	    const PipelineCache::GraphicsPipelineKey& key,
	    VkRenderPass                              render_pass,
	    const ShaderVertexInputInfo&              vs_input_info,
	    std::vector<uint32_t>                     vs_shader,
	    const ShaderPixelInputInfo&               ps_input_info,
	    std::vector<uint32_t>                     ps_shader,
	    const PipelineStaticParameters&           static_params,
	    uint32_t vs_hash0, uint32_t vs_crc32,
	    uint32_t ps_hash0, uint32_t ps_crc32,
	    bool ps_active) {

		{
			std::lock_guard<std::mutex> lock(m_map_mutex);
			if (m_results.find(key) != m_results.end()) {
				return; // Already requested or compiled
			}
			m_results[key] = {Status::Pending, nullptr, nullptr};
		}

		// Push compilation task
		{
			std::lock_guard<std::mutex> lock(m_queue_mutex);
			if (m_stop) {
				return;
			}
			m_tasks.emplace([=, this, vs_shader = std::move(vs_shader), ps_shader = std::move(ps_shader)]() {
				PipelineCache::GraphicsPipeline pipeline_result {};

				// Call the compilation function with our pipeline cache
				CreatePipelineInternal(
				    &pipeline_result,
				    render_pass,
				    &vs_input_info,
				    vs_shader,
				    &ps_input_info,
				    ps_shader,
				    static_params,
				    vs_hash0, vs_crc32,
				    ps_hash0, ps_crc32,
				    ps_active,
				    m_pipeline_cache
				);

				{
					std::lock_guard<std::mutex> map_lock(m_map_mutex);
					if (pipeline_result.pipeline != nullptr) {
						m_results[key] = {Status::Completed, pipeline_result.pipeline, pipeline_result.pipeline_layout};
						m_cache_dirty = true;
					} else {
						m_results[key] = {Status::Error, nullptr, nullptr};
					}
				}

				SaveCachePeriodically();
			});
		}
		m_cv.notify_one();
	}

	void SaveCachePeriodically() {
		auto now = std::chrono::steady_clock::now();
		bool should_save = false;
		{
			std::lock_guard<std::mutex> lock(m_map_mutex);
			if (m_cache_dirty && std::chrono::duration_cast<std::chrono::seconds>(now - m_last_save_time).count() >= 10) {
				should_save = true;
				m_cache_dirty = false;
				m_last_save_time = now;
			}
		}

		if (should_save) {
			std::lock_guard<std::mutex> lock(m_queue_mutex);
			if (!m_stop) {
				m_tasks.emplace([this, now]() {
					std::lock_guard<std::mutex> lock(m_save_mutex);
					if (!SaveCache()) {
						std::lock_guard<std::mutex> map_lock(m_map_mutex);
						m_cache_dirty = true;
					}
				});
				m_cv.notify_one();
			}
		}
	}

	void MarkCacheDirty() {
		std::lock_guard<std::mutex> lock(m_map_mutex);
		m_cache_dirty = true;
	}

private:
	void LoadCache() {
		VkPipelineCacheCreateInfo create_info {};
		create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

		std::vector<char> cache_data;
		std::ifstream     file(m_cache_path, std::ios::binary | std::ios::ate);
		if (file.is_open()) {
			std::streamsize size = file.tellg();
			file.seekg(0, std::ios::beg);
			cache_data.resize(size);
			if (file.read(cache_data.data(), size)) {
				create_info.initialDataSize = size;
				create_info.pInitialData    = cache_data.data();
			}
		}

		vkCreatePipelineCache(m_device, &create_info, nullptr, &m_pipeline_cache);
	}

	bool SaveCache() {
		if (m_pipeline_cache == nullptr) {
			return false;
		}

		std::unique_lock<std::shared_mutex> lock(g_pipeline_cache_mutex);

		size_t cache_size = 0;
		vkGetPipelineCacheData(m_device, m_pipeline_cache, &cache_size, nullptr);
		if (cache_size > 0) {
			std::vector<char> cache_data(cache_size);
			if (vkGetPipelineCacheData(m_device, m_pipeline_cache, &cache_size, cache_data.data()) == VK_SUCCESS) {
				std::ofstream file(m_cache_path, std::ios::binary);
				if (file.is_open()) {
					file.write(cache_data.data(), cache_size);
					return true;
				}
			}
		}
		return false;
	}

	void WorkerLoop(std::stop_token stop_token) {
		while (!stop_token.stop_requested()) {
			std::function<void()> task;
			{
				std::unique_lock<std::mutex> lock(m_queue_mutex);
				m_cv.wait(lock, [this, &stop_token]() {
					return m_stop || !m_tasks.empty() || stop_token.stop_requested();
				});

				if (m_stop || stop_token.stop_requested()) {
					return;
				}

				if (!m_tasks.empty()) {
					task = std::move(m_tasks.front());
					m_tasks.pop();
				}
			}

			if (task) {
				task();
			}
		}
	}

	VkDevice         m_device;
	VkPhysicalDevice m_physical_device;
	std::string      m_cache_path;
	VkPipelineCache  m_pipeline_cache = nullptr;

	std::vector<std::jthread>         m_workers;
	std::queue<std::function<void()>> m_tasks;
	std::mutex                        m_queue_mutex;
	std::condition_variable           m_cv;
	bool                              m_stop = false;

	std::mutex                                                                                           m_map_mutex;
	std::unordered_map<PipelineCache::GraphicsPipelineKey, Result, PipelineCache::GraphicsPipelineKeyHash> m_results;

	std::mutex                                            m_save_mutex;
	bool                                                  m_cache_dirty = false;
	std::chrono::steady_clock::time_point                 m_last_save_time;
};

extern std::unique_ptr<AsyncPipelineBuilder> g_AsyncPipelineBuilder;

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_ASYNCPIPELINEBUILDER_H_
