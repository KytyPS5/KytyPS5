#include "graphics/host_gpu/renderer/pipeline/pipelineHelpers.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/imageHeap.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/window.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fmt/format.h>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace Libs::Graphics {

namespace {

// A helper exits after this many pipelines (see PipelineHelpers): NVIDIA's driver starts its
// dictionary after a few hundred.
constexpr uint32_t PipelinesPerHelper = 100;
// Pipelines a helper is given at once: one, so that urgent jobs wait for no queued job.
constexpr uint32_t JobsPerHelper = 1;
// A helper without work for this long exits.
constexpr auto IdleHelper = std::chrono::seconds(20);
// After this many helpers that ended without a result, no more are started.
constexpr uint32_t MaxBrokenHelpers = 3;
// A helper that returns nothing for this long while it has work is stuck in the driver: it is
// ended, and its jobs are created in the emulator.
constexpr auto StuckHelper = std::chrono::seconds(300);
// How often the counts are logged while helpers work.
constexpr auto ProgressPeriod = std::chrono::seconds(10);

enum class Message : uint32_t { Job = 1, Result = 2 };

struct MessageHeader {
	uint32_t size = 0; // Of the payload that follows.
	Message  type = Message::Job;
};

template <typename... Args>
void HelperLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	Log::WriteToConsoleAndLog(message);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

bool WriteAll(HANDLE handle, const void* data, size_t size) {
	const auto* bytes = static_cast<const uint8_t*>(data);
	while (size != 0) {
		DWORD written = 0;
		const auto chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 30u));
		if (!::WriteFile(handle, bytes, chunk, &written, nullptr) || written == 0) {
			return false;
		}
		bytes += written;
		size -= written;
	}
	return true;
}

bool ReadAll(HANDLE handle, void* data, size_t size) {
	auto* bytes = static_cast<uint8_t*>(data);
	while (size != 0) {
		DWORD read = 0;
		const auto chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 30u));
		if (!::ReadFile(handle, bytes, chunk, &read, nullptr) || read == 0) {
			return false;
		}
		bytes += read;
		size -= read;
	}
	return true;
}

// A message whose payload is `head` followed by `tail`.
bool Send(HANDLE handle, Message type, std::span<const uint8_t> head, std::span<const uint8_t> tail) {
	const MessageHeader header {.size = static_cast<uint32_t>(head.size() + tail.size()), .type = type};
	return WriteAll(handle, &header, sizeof(header)) && WriteAll(handle, head.data(), head.size()) &&
	       WriteAll(handle, tail.data(), tail.size());
}

bool Receive(HANDLE handle, Message& type, std::vector<uint8_t>& payload) {
	MessageHeader header;
	if (!ReadAll(handle, &header, sizeof(header))) {
		return false;
	}
	type = header.type;
	payload.resize(header.size);
	return ReadAll(handle, payload.data(), payload.size());
}

template <typename T> void Append(std::vector<uint8_t>& bytes, const T& value) {
	const auto* begin = reinterpret_cast<const uint8_t*>(&value);
	bytes.insert(bytes.end(), begin, begin + sizeof(T));
}

template <typename T> bool Take(std::span<const uint8_t>& bytes, T& value) {
	if (bytes.size() < sizeof(T)) {
		return false;
	}
	std::memcpy(&value, bytes.data(), sizeof(T));
	bytes = bytes.subspan(sizeof(T));
	return true;
}

// Quotes an argument for CommandLineToArgvW.
std::wstring QuoteArgument(std::wstring_view argument) {
	if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
		return std::wstring(argument);
	}
	std::wstring quoted = L"\"";
	size_t       slashes = 0;
	for (const auto c: argument) {
		if (c == L'\\') {
			slashes++;
			continue;
		}
		quoted.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
		slashes = 0;
		quoted.push_back(c);
	}
	quoted.append(slashes * 2, L'\\');
	quoted.push_back(L'"');
	return quoted;
}

// ASCII.
std::wstring Widen(std::string_view text) {
	return {text.begin(), text.end()};
}

std::string HexBytes(std::span<const uint8_t> bytes) {
	std::string text;
	for (const auto byte: bytes) {
		text += fmt::format("{:02x}", byte);
	}
	return text;
}

#endif

} // namespace

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

// The helpers' disk caches (<root>/<emulator pid>/<n>) of emulators that no longer run: ended from
// outside, their helpers die with them and leave their disk caches behind.
static void RemoveAbandonedDiskCaches(const std::filesystem::path& root) {
	std::error_code error;
	for (std::filesystem::directory_iterator entry(root, error), end; !error && entry != end;
	     entry.increment(error)) {
		const auto name   = entry->path().filename().string();
		uint32_t   pid    = 0;
		const auto parsed = std::from_chars(name.data(), name.data() + name.size(), pid);
		if (parsed.ec != std::errc {} || parsed.ptr != name.data() + name.size() ||
		    pid == ::GetCurrentProcessId()) {
			continue;
		}
		HANDLE     process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
		const bool running = process != nullptr && ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
		if (process != nullptr) {
			::CloseHandle(process);
		}
		if (!running) {
			std::error_code ignored;
			std::filesystem::remove_all(entry->path(), ignored);
		}
	}
}

// Creates the compute pipeline of one job in a cache of its own and returns that cache's data.
static bool CompileHelperJob(GraphicContext& graphics, std::span<const uint8_t> bytes, std::vector<uint8_t>& data) {
	ShaderModule          module;
	std::vector<uint32_t> spirv;
	bool                  ok = DecodeComputeModule(bytes, module, spirv);
	if (!ok) {
		return false;
	}
	// An error of the driver or of the module fails this job alone.
	Common::SoftExitScope   soft_exit;
	PipelineCache::Pipeline pipeline;
	vk::PipelineCache       cache = nullptr;
	try {
		module.module = CompileSPV(spirv, graphics.device);
		const vk::PipelineCacheCreateInfo create {};
		ok = graphics.device.createPipelineCache(&create, nullptr, &cache) == vk::Result::eSuccess;
		if (ok) {
			CreatePipelineInternal(graphics, pipeline, module, cache);
			size_t size = 0;
			ok = graphics.device.getPipelineCacheData(cache, &size, nullptr) == vk::Result::eSuccess;
			data.resize(size);
			ok = ok && graphics.device.getPipelineCacheData(cache, &size, data.data()) == vk::Result::eSuccess;
			data.resize(size);
		}
	} catch (const std::exception& exception) {
		HelperLog("Pipeline helper: cs {:016x}: {}", module.info.shader_hash, exception.what());
		ok = false;
	}
	graphics.device.destroyPipeline(pipeline.pipeline, nullptr);
	graphics.device.destroyPipelineLayout(pipeline.pipeline_layout, nullptr);
	graphics.device.destroyDescriptorSetLayout(pipeline.descriptor_set_layout, nullptr);
	graphics.device.destroyPipelineCache(cache, nullptr);
	graphics.device.destroyShaderModule(module.module, nullptr);
	return ok;
}

struct PipelineHelpers::Impl {
	struct Job {
		uint64_t             ticket = 0;
		std::vector<uint8_t> module;
	};
	struct Helper {
		uint32_t                     number    = 0;
		HANDLE                       process   = nullptr;
		HANDLE                       jobs      = nullptr; // Our end of its job pipe.
		HANDLE                       results   = nullptr; // Our end of its result pipe.
		std::filesystem::path        disk_cache;
		std::thread                  reader;
		std::unordered_set<uint64_t> in_flight;
		uint32_t                     sent     = 0;
		uint32_t                     results_received = 0;
		bool                         retiring = false; // Gets no more jobs.
		bool                         exited   = false; // Its reader saw the pipe close.
		std::chrono::steady_clock::time_point last_job;
		// The last result, or the job that gave it work again.
		std::chrono::steady_clock::time_point last_progress;
		bool                                  terminated = false;
	};

	VulkanDeviceIdentity identity;
	uint32_t             max_processes = 0;
	ResultCallback       on_result;
	std::wstring         exe;
	std::wstring         arguments; // The emulator's own, for the helper's device.
	std::filesystem::path disk_cache_root;
	HANDLE               job_object = nullptr;

	mutable std::mutex                   mutex;
	std::condition_variable              changed;
	std::deque<Job>                      queue;
	std::vector<std::unique_ptr<Helper>> helpers;
	uint32_t                             started  = 0;
	uint32_t                             broken   = 0;
	uint64_t                             queued   = 0;
	uint64_t                             done     = 0;
	uint64_t                             failed   = 0;
	uint64_t                             withdrawn = 0;
	bool                                 stopping = false;
	// Results and lost jobs are not reported any more.
	std::atomic<bool>                    closing {false};
	std::chrono::steady_clock::time_point last_log;
	uint64_t                             logged_done = UINT64_MAX;
	std::thread                          manager;

	~Impl() {
		{
			std::scoped_lock lock(mutex);
			stopping = true;
		}
		changed.notify_all();
		if (manager.joinable()) {
			manager.join();
		}
		// What helpers compile now is not waited for.
		closing = true;
		for (auto& helper: helpers) {
			::TerminateProcess(helper->process, 0);
		}
		for (auto& helper: helpers) {
			::WaitForSingleObject(helper->process, 5000);
			if (helper->reader.joinable()) {
				helper->reader.join();
			}
			Close(*helper);
		}
		if (job_object != nullptr) {
			::CloseHandle(job_object);
		}
	}

	static void Close(Helper& helper) {
		for (auto* handle: {&helper.jobs, &helper.results, &helper.process}) {
			if (*handle != nullptr) {
				::CloseHandle(*handle);
				*handle = nullptr;
			}
		}
		std::error_code error;
		std::filesystem::remove_all(helper.disk_cache, error);
	}

	// Without the lock.
	std::unique_ptr<Helper> StartHelper(uint32_t number) {
		auto helper        = std::make_unique<Helper>();
		helper->number     = number;
		helper->disk_cache = disk_cache_root / std::to_string(number);
		std::error_code error;
		std::filesystem::remove_all(helper->disk_cache, error);
		std::filesystem::create_directories(helper->disk_cache, error);

		SECURITY_ATTRIBUTES inherit {.nLength = sizeof(SECURITY_ATTRIBUTES), .bInheritHandle = TRUE};
		HANDLE child_jobs = nullptr;
		HANDLE child_results = nullptr;
		constexpr DWORD PipeSize = 4u << 20u;
		if (!::CreatePipe(&child_jobs, &helper->jobs, &inherit, PipeSize)) {
			return nullptr;
		}
		if (!::CreatePipe(&helper->results, &child_results, &inherit, PipeSize)) {
			::CloseHandle(child_jobs);
			::CloseHandle(helper->jobs);
			return nullptr;
		}
		::SetHandleInformation(helper->jobs, HANDLE_FLAG_INHERIT, 0);
		::SetHandleInformation(helper->results, HANDLE_FLAG_INHERIT, 0);
		// Its output goes nowhere.
		HANDLE output = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
		                              OPEN_EXISTING, 0, nullptr);

		const auto spec = fmt::format(
		    "in={};out={};vendor={};device={};driver={};uuid={};family={}",
		    reinterpret_cast<uintptr_t>(child_jobs), reinterpret_cast<uintptr_t>(child_results),
		    identity.vendor_id, identity.device_id, identity.driver_version,
		    HexBytes(identity.cache_uuid), identity.queue_family);
		auto command = QuoteArgument(exe) + L" --pipeline-helper " + QuoteArgument(Widen(spec)) + arguments;

		// The environment, with a driver disk cache of its own.
		std::wstring environment;
		if (auto* strings = ::GetEnvironmentStringsW(); strings != nullptr) {
			for (const wchar_t* entry = strings; *entry != 0; entry += std::wcslen(entry) + 1) {
				if (std::wstring_view(entry).starts_with(L"__GL_SHADER_DISK_CACHE_PATH=")) {
					continue;
				}
				environment.append(entry);
				environment.push_back(L'\0');
			}
			::FreeEnvironmentStringsW(strings);
		}
		environment += L"__GL_SHADER_DISK_CACHE_PATH=" + helper->disk_cache.wstring();
		environment.push_back(L'\0');
		environment.push_back(L'\0');

		std::array<HANDLE, 3> inherited {child_jobs, child_results, output};
		const auto inherited_count = output != INVALID_HANDLE_VALUE ? 3u : 2u;
		SIZE_T     attributes_size = 0;
		::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributes_size);
		std::vector<uint8_t> attributes(attributes_size);
		STARTUPINFOEXW       startup {};
		startup.StartupInfo.cb = sizeof(startup);
		startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
		bool created = false;
		PROCESS_INFORMATION info {};
		if (::InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributes_size) &&
		    ::UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
		                                inherited.data(), inherited_count * sizeof(HANDLE), nullptr,
		                                nullptr)) {
			if (output != INVALID_HANDLE_VALUE) {
				startup.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
				startup.StartupInfo.hStdInput  = nullptr;
				startup.StartupInfo.hStdOutput = output;
				startup.StartupInfo.hStdError  = output;
			}
			// The priority of the emulator's own threads: below it, Demon's Souls' busy job threads
			// left all its helpers about two cores, and its loading took longer than without them.
			DWORD priority = NORMAL_PRIORITY_CLASS;
			created = ::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
			                           CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
			                               CREATE_UNICODE_ENVIRONMENT | priority,
			                           environment.data(), nullptr, &startup.StartupInfo, &info) != 0;
			::DeleteProcThreadAttributeList(startup.lpAttributeList);
		}
		::CloseHandle(child_jobs);
		::CloseHandle(child_results);
		if (output != INVALID_HANDLE_VALUE) {
			::CloseHandle(output);
		}
		if (!created) {
			HelperLog("Pipeline helpers: could not start a helper ({})", ::GetLastError());
			Close(*helper);
			return nullptr;
		}
		helper->process = info.hProcess;
		// It dies with the emulator.
		::AssignProcessToJobObject(job_object, info.hProcess);
		// A helper may run on every processor of the emulator, also on the second threads of the cores
		// kept for the pinned GPU threads, which the emulator's other threads leave idle.
		// Windows may run a windowless process of low priority on efficiency cores alone.
		PROCESS_POWER_THROTTLING_STATE throttling {.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
		                                           .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
		                                           .StateMask   = 0};
		::SetProcessInformation(info.hProcess, ProcessPowerThrottling, &throttling, sizeof(throttling));
		::ResumeThread(info.hThread);
		::CloseHandle(info.hThread);
		helper->last_job      = std::chrono::steady_clock::now();
		helper->last_progress = helper->last_job;
		return helper;
	}

	// The helper's results, until its pipe closes.
	void Read(Helper& helper) {
		std::vector<uint8_t> payload;
		Message              type = Message::Result;
		while (Receive(helper.results, type, payload)) {
			if (type != Message::Result) {
				continue;
			}
			std::span<const uint8_t> bytes(payload);
			Result                   result;
			uint32_t                 ok = 0;
			if (!Take(bytes, result.ticket) || !Take(bytes, ok)) {
				break;
			}
			result.ok = ok != 0;
			result.cache_data.assign(bytes.begin(), bytes.end());
			{
				std::scoped_lock lock(mutex);
				helper.in_flight.erase(result.ticket);
				helper.results_received++;
				helper.last_progress = std::chrono::steady_clock::now();
				(result.ok ? done : failed)++;
			}
			changed.notify_all();
			if (!closing) {
				on_result(std::move(result));
			}
		}
		std::vector<uint64_t> lost;
		{
			std::scoped_lock lock(mutex);
			helper.exited = true;
			lost.assign(helper.in_flight.begin(), helper.in_flight.end());
			helper.in_flight.clear();
			failed += lost.size();
			if (helper.results_received == 0) {
				broken++;
			}
		}
		changed.notify_all();
		for (const auto ticket: lost) {
			if (!closing) {
				on_result({.ticket = ticket, .ok = false});
			}
		}
	}

	// Under the lock.
	void Progress(std::chrono::steady_clock::time_point now) {
		if (now - last_log < ProgressPeriod || done + failed == logged_done) {
			return;
		}
		last_log    = now;
		logged_done = done + failed;
		HelperLog("Pipeline helpers: {} compute pipelines queued, {} compiled, {} failed, {} taken back, "
		          "{} waiting; {} helpers started, {} running",
		          queued, done, failed, withdrawn, queue.size(), started, helpers.size());
	}

	void Fail(std::deque<Job>& jobs) {
		for (auto& job: jobs) {
			on_result({.ticket = job.ticket, .ok = false});
		}
	}

	void Manage() {
		Common::Thread::LowerCurrentPriority();
		std::unique_lock lock(mutex);
		while (!stopping) {
			const auto now = std::chrono::steady_clock::now();
			// Helpers that exited.
			for (auto it = helpers.begin(); it != helpers.end();) {
				if ((*it)->exited) {
					auto helper = std::move(*it);
					it          = helpers.erase(it);
					lock.unlock();
					::WaitForSingleObject(helper->process, 5000);
					helper->reader.join();
					Close(*helper);
					lock.lock();
				} else {
					++it;
				}
			}
			if (broken >= MaxBrokenHelpers && !queue.empty()) {
				auto jobs = std::move(queue);
				queue.clear();
				failed += jobs.size();
				lock.unlock();
				Fail(jobs);
				lock.lock();
				continue;
			}
			// Helpers done with their share (or idle) get their job pipe closed and exit; one that
			// is stuck is ended.
			for (auto& helper: helpers) {
				if (!helper->in_flight.empty() && !helper->terminated &&
				    now - helper->last_progress >= StuckHelper) {
					HelperLog("Pipeline helpers: helper {} returned nothing for {} s, ended", helper->number,
					          std::chrono::duration_cast<std::chrono::seconds>(StuckHelper).count());
					helper->terminated = true;
					helper->retiring   = true;
					::TerminateProcess(helper->process, 1);
				}
				if (!helper->retiring && queue.empty() && helper->in_flight.empty() &&
				    now - helper->last_job >= IdleHelper) {
					helper->retiring = true;
				}
				if (helper->retiring && helper->in_flight.empty() && helper->jobs != nullptr) {
					::CloseHandle(helper->jobs);
					helper->jobs = nullptr;
				}
			}
			// Jobs go to a helper with room, or to one started for them.
			bool sent = false;
			while (!queue.empty()) {
				Helper* target = nullptr;
				for (auto& helper: helpers) {
					if (!helper->retiring && !helper->exited && helper->in_flight.size() < JobsPerHelper &&
					    (target == nullptr || helper->in_flight.size() < target->in_flight.size())) {
						target = helper.get();
					}
				}
				if (target == nullptr) {
					uint32_t live = 0;
					for (auto& helper: helpers) {
						live += helper->retiring ? 0u : 1u;
					}
					if (live >= max_processes || broken >= MaxBrokenHelpers) {
						break;
					}
					const auto number = started++;
					lock.unlock();
					auto helper = StartHelper(number);
					lock.lock();
					if (helper == nullptr) {
						broken++;
						continue;
					}
					auto* raw = helper.get();
					helpers.push_back(std::move(helper));
					raw->reader = std::thread([this, raw] { Read(*raw); });
					continue;
				}
				auto job = std::move(queue.front());
				queue.pop_front();
				if (target->in_flight.empty()) {
					target->last_progress = now;
				}
				target->in_flight.insert(job.ticket);
				target->last_job = now;
				if (++target->sent >= PipelinesPerHelper) {
					target->retiring = true;
				}
				const auto handle = target->jobs;
				lock.unlock();
				std::vector<uint8_t> head;
				Append(head, job.ticket);
				// A helper that died: its reader reports the job.
				(void)Send(handle, Message::Job, head, job.module);
				lock.lock();
				sent = true;
			}
			Progress(now);
			if (!sent) {
				changed.wait_for(lock, std::chrono::seconds(1));
			}
		}
	}
};

PipelineHelpers::PipelineHelpers(std::unique_ptr<Impl> impl): m_impl(std::move(impl)) {}

PipelineHelpers::~PipelineHelpers() = default;

std::unique_ptr<PipelineHelpers> PipelineHelpers::Start(const GraphicContext& graphics,
                                                        uint32_t processes, ResultCallback on_result) {
	if (processes == 0 || !graphics.pipeline_cache_lookup_enabled) {
		return nullptr;
	}
	auto impl           = std::make_unique<Impl>();
	impl->identity      = GetVulkanDeviceIdentity(graphics);
	impl->max_processes = processes;
	impl->on_result     = std::move(on_result);
	std::array<wchar_t, 32768> exe {};
	::GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
	impl->exe = exe.data();
	int   count = 0;
	auto* argv  = ::CommandLineToArgvW(::GetCommandLineW(), &count);
	for (int i = 1; argv != nullptr && i < count; i++) {
		impl->arguments += L" " + QuoteArgument(argv[i]);
	}
	if (argv != nullptr) {
		::LocalFree(argv);
	}
	std::error_code error;
	impl->disk_cache_root = std::filesystem::temp_directory_path(error) / "kyty_pipeline_helpers" /
	                        std::to_string(::GetCurrentProcessId());
	if (error) {
		return nullptr;
	}
	RemoveAbandonedDiskCaches(impl->disk_cache_root.parent_path());
	impl->job_object = ::CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (impl->job_object == nullptr ||
	    !::SetInformationJobObject(impl->job_object, JobObjectExtendedLimitInformation, &limits,
	                               sizeof(limits))) {
		return nullptr;
	}
	auto* raw     = impl.get();
	raw->manager  = std::thread([raw] { raw->Manage(); });
	HelperLog("Pipeline helpers: up to {} processes compile compute pipelines", processes);
	return std::unique_ptr<PipelineHelpers>(new PipelineHelpers(std::move(impl)));
}

void PipelineHelpers::Queue(uint64_t ticket, std::vector<uint8_t>&& module, bool urgent) {
	{
		std::scoped_lock lock(m_impl->mutex);
		if (urgent) {
			m_impl->queue.push_front({.ticket = ticket, .module = std::move(module)});
		} else {
			m_impl->queue.push_back({.ticket = ticket, .module = std::move(module)});
		}
		m_impl->queued++;
		m_impl->logged_done = UINT64_MAX;
	}
	m_impl->changed.notify_all();
}

bool PipelineHelpers::Withdraw(uint64_t ticket) {
	std::scoped_lock lock(m_impl->mutex);
	auto& queue = m_impl->queue;
	const auto found = std::ranges::find(queue, ticket, &Impl::Job::ticket);
	if (found == queue.end()) {
		return false;
	}
	queue.erase(found);
	m_impl->withdrawn++;
	return true;
}

void PipelineHelpers::LogProgress() const {
	std::scoped_lock lock(m_impl->mutex);
	HelperLog("Pipeline helpers: {} compute pipelines queued, {} compiled, {} failed, {} taken back, "
	          "{} waiting; {} helpers started, {} running",
	          m_impl->queued, m_impl->done, m_impl->failed, m_impl->withdrawn, m_impl->queue.size(),
	          m_impl->started, m_impl->helpers.size());
}

uint32_t PipelineHelperProcesses(const GraphicContext& graphics) {
	if (const char* value = std::getenv("KYTY_PIPELINE_HELPERS"); value != nullptr) {
		uint32_t   count  = 0;
		const auto end    = value + std::strlen(value);
		const auto parsed = std::from_chars(value, end, count);
		return parsed.ec == std::errc {} && parsed.ptr == end ? count : 0u;
	}
	// Measured with NVIDIA's driver only.
	constexpr uint32_t NvidiaVendor = 0x10deu;
	if (graphics.GetPhysicalDeviceProperties().vendorID != NvidiaVendor) {
		return 0;
	}
	DWORD_PTR process_mask = 0;
	DWORD_PTR system_mask  = 0;
	if (!::GetProcessAffinityMask(::GetCurrentProcess(), &process_mask, &system_mask)) {
		return 0;
	}
	// 10 on 24 logical processors (measured with Demon's Souls), and fewer on smaller processors,
	// where the game needs the rest.
	const auto processors = static_cast<uint32_t>(std::popcount(process_mask));
	return processors / 2u > 2u ? processors / 2u - 2u : 1u;
}

// The helper process.

namespace {

struct HelperSpec {
	HANDLE               jobs    = nullptr;
	HANDLE               results = nullptr;
	VulkanDeviceIdentity identity;
};

bool ParseSpec(std::string_view text, HelperSpec& spec) {
	uint32_t found = 0;
	while (!text.empty()) {
		const auto end   = std::min(text.find(';'), text.size());
		const auto field = text.substr(0, end);
		text             = text.substr(std::min(end + 1, text.size()));
		const auto equal = field.find('=');
		if (equal == std::string_view::npos) {
			return false;
		}
		const auto key   = field.substr(0, equal);
		const auto value = field.substr(equal + 1);
		const auto number = [&](auto& out) {
			return std::from_chars(value.data(), value.data() + value.size(), out).ec == std::errc {};
		};
		uintptr_t handle = 0;
		bool      ok     = true;
		if (key == "in") {
			ok         = number(handle);
			spec.jobs  = reinterpret_cast<HANDLE>(handle);
		} else if (key == "out") {
			ok           = number(handle);
			spec.results = reinterpret_cast<HANDLE>(handle);
		} else if (key == "vendor") {
			ok = number(spec.identity.vendor_id);
		} else if (key == "device") {
			ok = number(spec.identity.device_id);
		} else if (key == "driver") {
			ok = number(spec.identity.driver_version);
		} else if (key == "family") {
			ok = number(spec.identity.queue_family);
		} else if (key == "uuid") {
			ok = value.size() == spec.identity.cache_uuid.size() * 2;
			for (size_t i = 0; ok && i < spec.identity.cache_uuid.size(); i++) {
				ok = std::from_chars(value.data() + i * 2, value.data() + i * 2 + 2,
				                     spec.identity.cache_uuid[i], 16)
				         .ec == std::errc {};
			}
		} else {
			continue;
		}
		if (!ok) {
			return false;
		}
		found++;
	}
	return found == 7 && spec.jobs != nullptr && spec.results != nullptr;
}

} // namespace

int RunPipelineHelper(std::string_view spec_text, Config::ConfigOptions config) {
	HelperSpec spec;
	if (!ParseSpec(spec_text, spec)) {
		return 2;
	}
	// The emulator's options give the device; its log file is its own.
	config.printf_direction = Config::LogDirection::Silent;
	config.printf_output_file.clear();
	Config::Initialize();
	Config::Load(config);
	Log::Initialize();

	GraphicContext graphics;
	std::string    error;
	if (!CreateHeadlessVulkan(graphics, &spec.identity, error)) {
		HelperLog("Pipeline helper: {}", error);
		return 3;
	}
	graphics.image_heap_layout = ImageHeap::CreateLayout(graphics.device);

	std::mutex                        mutex;
	std::condition_variable           available;
	std::deque<std::vector<uint8_t>>  jobs;
	bool                              closed = false;
	std::mutex                        send_mutex;
	// Jobs come in order; the emulator closes the pipe once this helper has had its share.
	std::thread reader([&] {
		std::vector<uint8_t> payload;
		Message              type = Message::Job;
		while (Receive(spec.jobs, type, payload)) {
			if (type == Message::Job) {
				std::scoped_lock lock(mutex);
				jobs.push_back(std::move(payload));
				payload = {};
			}
			available.notify_one();
		}
		{
			std::scoped_lock lock(mutex);
			closed = true;
		}
		available.notify_all();
	});
	for (;;) {
		std::vector<uint8_t> job;
		{
			std::unique_lock lock(mutex);
			available.wait(lock, [&] { return !jobs.empty() || closed; });
			if (jobs.empty()) {
				break;
			}
			job = std::move(jobs.front());
			jobs.pop_front();
		}
		std::span<const uint8_t> bytes(job);
		uint64_t                 ticket = 0;
		if (!Take(bytes, ticket)) {
			continue;
		}
		std::vector<uint8_t> data;
		const bool           ok = CompileHelperJob(graphics, bytes, data);
		std::vector<uint8_t> head;
		Append(head, ticket);
		Append(head, static_cast<uint32_t>(ok ? 1u : 0u));
		std::scoped_lock lock(send_mutex);
		if (!Send(spec.results, Message::Result, head, ok ? std::span<const uint8_t>(data)
		                                                  : std::span<const uint8_t> {})) {
			break;
		}
	}
	// The reader ends when the emulator closes the pipe, or dies.
	::CloseHandle(spec.results);
	reader.detach();
	std::fflush(nullptr);
	std::_Exit(0);
}

#else

struct PipelineHelpers::Impl {};

PipelineHelpers::PipelineHelpers(std::unique_ptr<Impl> impl): m_impl(std::move(impl)) {}

PipelineHelpers::~PipelineHelpers() = default;

std::unique_ptr<PipelineHelpers> PipelineHelpers::Start(const GraphicContext& /*graphics*/,
                                                        uint32_t /*processes*/,
                                                        ResultCallback /*on_result*/) {
	return nullptr;
}

void PipelineHelpers::Queue(uint64_t /*ticket*/, std::vector<uint8_t>&& /*module*/, bool /*urgent*/) {}

bool PipelineHelpers::Withdraw(uint64_t /*ticket*/) {
	return false;
}

void PipelineHelpers::LogProgress() const {}

uint32_t PipelineHelperProcesses(const GraphicContext& /*graphics*/) {
	return 0;
}

int RunPipelineHelper(std::string_view /*spec*/, Config::ConfigOptions /*config*/) {
	return 1;
}

#endif

} // namespace Libs::Graphics
