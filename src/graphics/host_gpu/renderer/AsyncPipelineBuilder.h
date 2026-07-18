#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_ASYNCPIPELINEBUILDER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_ASYNCPIPELINEBUILDER_H_

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <future>
#include <functional>
#include <memory>
#include <unordered_map>
#include <fstream>
#include <iostream>

namespace Libs::Graphics {

enum class PipelineBuildStatus {
    Pending,
    Ready,
    Failed
};

struct PipelineRequest {
    uint64_t request_id;
    std::function<VkPipeline(VkPipelineCache)> compile_func;
};

// Thread-safe Task Queue for async compilation tasks
class PipelineTaskQueue {
public:
    void Push(PipelineRequest request) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(request));
        m_cv.notify_one();
    }

    bool Pop(PipelineRequest& out_request, std::stop_token stop_token) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this, &stop_token] {
            return !m_queue.empty() || stop_token.stop_requested();
        });

        if (stop_token.stop_requested() && m_queue.empty()) {
            return false;
        }

        out_request = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_queue.empty()) {
            m_queue.pop();
        }
    }

private:
    std::queue<PipelineRequest> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
};

class AsyncPipelineBuilder {
public:
    AsyncPipelineBuilder(VkDevice device, VkPhysicalDevice physical_device, const std::string& cache_filepath)
        : m_device(device), m_physical_device(physical_device), m_cache_filepath(cache_filepath), m_pipeline_cache(VK_NULL_HANDLE) {

        LoadPipelineCache();

        // Spawn 4 worker threads using C++20 std::jthread with auto-joining and stop tokens
        unsigned int num_workers = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned int i = 0; i < num_workers; ++i) {
            m_workers.emplace_back([this](std::stop_token stop_token) {
                WorkerLoop(stop_token);
            });
        }
    }

    ~AsyncPipelineBuilder() {
        Shutdown();
    }

    // Request asynchronous compilation of a pipeline
    void RequestPipeline(uint64_t request_id, std::function<VkPipeline(VkPipelineCache)> compile_func) {
        {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            if (m_pipeline_statuses.find(request_id) != m_pipeline_statuses.end()) {
                return; // Already registered or compiling
            }
            m_pipeline_statuses[request_id] = PipelineBuildStatus::Pending;
        }

        m_task_queue.Push({request_id, std::move(compile_func)});
    }

    // Non-blocking query of pipeline status and output handle
    PipelineBuildStatus QueryPipeline(uint64_t request_id, VkPipeline& out_pipeline) {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        auto it = m_pipeline_statuses.find(request_id);
        if (it == m_pipeline_statuses.end()) {
            return PipelineBuildStatus::Failed;
        }

        if (it->second == PipelineBuildStatus::Ready) {
            out_pipeline = m_compiled_pipelines[request_id];
        }
        return it->second;
    }

    // Save and shutdown builder cleanly
    void Shutdown() {
        m_task_queue.Clear();
        m_workers.clear(); // std::jthread automatically requests stop and joins
        SavePipelineCache();

        if (m_pipeline_cache != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(m_device, m_pipeline_cache, nullptr);
            m_pipeline_cache = VK_NULL_HANDLE;
        }
    }

private:
    struct PipelineCacheHeader {
        uint32_t header_length;
        uint32_t header_version;
        uint32_t vendor_id;
        uint32_t device_id;
        uint8_t  pipeline_cache_uuid[VK_UUID_SIZE];
    };

    void WorkerLoop(std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            PipelineRequest request;
            if (!m_task_queue.Pop(request, stop_token)) {
                break; // Stop requested and queue is empty
            }

            // Perform driver-level compilation with Vulkan pipeline cache object
            VkPipeline pipeline = request.compile_func(m_pipeline_cache);

            {
                std::lock_guard<std::mutex> lock(m_status_mutex);
                if (pipeline != VK_NULL_HANDLE) {
                    m_compiled_pipelines[request.request_id] = pipeline;
                    m_pipeline_statuses[request.request_id] = PipelineBuildStatus::Ready;
                } else {
                    m_pipeline_statuses[request.request_id] = PipelineBuildStatus::Failed;
                }
            }
        }
    }

    void LoadPipelineCache() {
        std::ifstream file(m_cache_filepath, std::ios::binary | std::ios::ate);
        std::vector<char> cache_data;

        if (file.is_open()) {
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);

            if (size >= static_cast<std::streamsize>(sizeof(PipelineCacheHeader))) {
                cache_data.resize(size);
                if (file.read(cache_data.data(), size)) {
                    if (ValidateCacheHeader(cache_data.data(), size)) {
                        std::cout << "[AsyncPipelineBuilder] Valid persistent cache found and loaded. Size: " << size << " bytes.\n";
                    } else {
                        std::cerr << "[AsyncPipelineBuilder] Cache header validation failed. Recreating pipeline cache.\n";
                        cache_data.clear();
                    }
                }
            }
            file.close();
        }

        VkPipelineCacheCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        create_info.initialDataSize = cache_data.size();
        create_info.pInitialData = cache_data.empty() ? nullptr : cache_data.data();

        VkResult result = vkCreatePipelineCache(m_device, &create_info, nullptr, &m_pipeline_cache);
        if (result != VK_SUCCESS) {
            std::cerr << "[AsyncPipelineBuilder] Failed to create Vulkan pipeline cache: " << result << "\n";
            m_pipeline_cache = VK_NULL_HANDLE;
        }
    }

    void SavePipelineCache() {
        if (m_pipeline_cache == VK_NULL_HANDLE) return;

        size_t cache_size = 0;
        VkResult result = vkGetPipelineCacheData(m_device, m_pipeline_cache, &cache_size, nullptr);
        if (result != VK_SUCCESS || cache_size == 0) return;

        std::vector<char> cache_data(cache_size);
        result = vkGetPipelineCacheData(m_device, m_pipeline_cache, &cache_size, cache_data.data());
        if (result != VK_SUCCESS) return;

        std::ofstream file(m_cache_filepath, std::ios::binary | std::ios::trunc);
        if (file.is_open()) {
            file.write(cache_data.data(), cache_size);
            file.close();
            std::cout << "[AsyncPipelineBuilder] Saved persistent Vulkan pipeline cache. Size: " << cache_size << " bytes.\n";
        }
    }

    bool ValidateCacheHeader(const char* data, size_t size) {
        if (size < sizeof(PipelineCacheHeader)) return false;

        const auto* file_header = reinterpret_cast<const PipelineCacheHeader*>(data);

        // Fetch physical device properties from current host Vulkan instance
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(m_physical_device, &properties);

        // Validate vendor, device IDs, and UUID to guarantee driver compatibility and avoid device crash
        if (file_header->vendor_id != properties.vendorID) return false;
        if (file_header->device_id != properties.deviceID) return false;
        if (std::memcmp(file_header->pipeline_cache_uuid, properties.pipelineCacheUUID, VK_UUID_SIZE) != 0) return false;

        return true;
    }

    VkDevice m_device;
    VkPhysicalDevice m_physical_device;
    std::string m_cache_filepath;
    VkPipelineCache m_pipeline_cache;

    PipelineTaskQueue m_task_queue;
    std::vector<std::jthread> m_workers;

    std::mutex m_status_mutex;
    std::unordered_map<uint64_t, PipelineBuildStatus> m_pipeline_statuses;
    std::unordered_map<uint64_t, VkPipeline> m_compiled_pipelines;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_ASYNCPIPELINEBUILDER_H_
