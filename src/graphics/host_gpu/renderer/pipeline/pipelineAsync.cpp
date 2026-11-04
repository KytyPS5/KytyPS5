#include "graphics/host_gpu/renderer/pipeline/pipelineAsync.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/pipeline/skippedDraws.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/Serialize.h"
#include "graphics/shader/shaderCompiler.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <optional>
#include <type_traits>
#include <utility>
#include <xxhash.h>

// --async-shader-compile: stages and pipelines the cache cannot serve are made on worker threads,
// and the draws that need them are skipped until they are ready. Workers only run code that
// reads nothing but its own copy of the inputs. The cache itself, descriptor materialization and
// everything that reads guest memory stay on the render thread, which applies what the workers
// finished in PumpAsync.
//
// A stage takes two rounds. A worker translates the shader, which needs only the stage's static
// state. That translation waits until a draw names the resources it is compiled for, because
// reading them must happen in the draw. A second worker then emits and creates the module.
namespace Libs::Graphics {

ShaderWorkers::ShaderWorkers(uint32_t threads) {
	for (uint32_t i = 0; i < threads; i++) {
		m_threads.emplace_back([this, i](std::stop_token stop) {
			Common::Thread::LowerPriority();
			std::unique_lock lock(m_mutex);
			// Once stop is requested the wait returns false only when no job may be taken.
			while (m_ready.wait(lock, stop, [this, i] { return CanPick(i); })) {
				const auto job = Pick(i);
				if (!job) {
					continue;
				}
				lock.unlock();
				job->run();
				lock.lock();
			}
		});
	}
}

bool ShaderWorkers::CanPick(uint32_t index) const {
	const auto now     = AskClockMs();
	const auto allowed = [&](const JobPtr& job) {
		return index < BackgroundWorkers ||
		       now - job->asked.load(std::memory_order_relaxed) < StaleAfterMs;
	};
	return std::ranges::any_of(m_urgent_jobs, allowed) || std::ranges::any_of(m_jobs, allowed);
}

ShaderWorkers::JobPtr ShaderWorkers::Pick(uint32_t index) {
	const auto now = AskClockMs();
	for (auto* pool: {&m_urgent_jobs, &m_jobs}) {
		auto best = pool->end();
		for (auto it = pool->begin(); it != pool->end(); ++it) {
			if (index >= BackgroundWorkers &&
			    now - (*it)->asked.load(std::memory_order_relaxed) >= StaleAfterMs) {
				continue;
			}
			// Worker 0 takes the oldest, the others the one asked for last.
			if (best == pool->end() ||
			    (index == 0 ? (*it)->sequence < (*best)->sequence
			                : (*it)->asked.load(std::memory_order_relaxed) >
			                      (*best)->asked.load(std::memory_order_relaxed))) {
				best = it;
			}
		}
		if (best != pool->end()) {
			auto job = std::move(*best);
			// The order in the vector does not matter: the choice scans all of it.
			*best = std::move(pool->back());
			pool->pop_back();
			return job;
		}
	}
	return nullptr;
}

ShaderWorkers::JobPtr ShaderWorkers::Post(Common::UniqueFunction<void>&& run, Priority priority) {
	auto job      = std::make_shared<Job>();
	job->run      = std::move(run);
	job->priority = priority;
	job->Touch();
	{
		std::scoped_lock lock(m_mutex);
		if (m_stopped) {
			return nullptr;
		}
		job->sequence = m_next_sequence++;
		(priority == Priority::Urgent ? m_urgent_jobs : m_jobs).push_back(job);
	}
	m_ready.notify_one();
	return job;
}

size_t ShaderWorkers::Queued() {
	std::scoped_lock lock(m_mutex);
	return m_urgent_jobs.size() + m_jobs.size();
}

void ShaderWorkers::Stop() {
	{
		std::scoped_lock lock(m_mutex);
		m_stopped = true;
		m_jobs.clear();
		m_urgent_jobs.clear();
	}
	for (auto& thread: m_threads) {
		thread.request_stop();
	}
	// A running job finishes first.
	m_threads.clear();
}

uint64_t PipelineCache::FlightId(const ProgramKey& key, uint32_t push_data_cursor,
                                 const ShaderRecompiler::IR::ResourceSpecialization* specialization) {
	static const ShaderRecompiler::IR::ResourceSpecialization none {};
	ShaderRecompiler::IR::Writer                              writer;
	ShaderRecompiler::IR::IoAll(writer, key.stage, key.hash, key.user_data_count, key.static_state,
	                            push_data_cursor, specialization != nullptr,
	                            specialization != nullptr ? *specialization : none);
	return XXH3_64bits(writer.bytes.data(), writer.bytes.size());
}

template <typename InputInfo>
void PipelineCache::RequestAsync(const ProgramKey& key, Program* program,
                                 const ShaderParams& params, const InputInfo& input_info,
                                 const ShaderRecompiler::CompileOptions& options,
                                 uint32_t push_data_cursor, AsyncMode mode) {
	static_assert(std::is_same_v<InputInfo, ShaderVertexInputInfo> ||
	              std::is_same_v<InputInfo, ShaderPixelInputInfo>);
	auto*      async       = m_async.get();
	const auto translating = FlightId(key, UINT32_MAX, nullptr);

	if (program != nullptr) {
		if (mode == AsyncMode::PrefetchOnly) {
			return;
		}
		// The program is known; a draw's resources decide which module it needs. If a translation
		// is waiting, compile it for them.
		if (const auto found = async->translations.find(key); found != async->translations.end()) {
			const auto compiling = FlightId(key, push_data_cursor, &program->specialization);
			if (const auto flying = async->in_flight.find(compiling);
			    flying != async->in_flight.end()) {
				// A draw wants it again: it is the scene on screen.
				flying->second.job->Touch();
				return;
			}
			auto translation = async->translations.extract(found);
			auto inputs      = std::move(translation.mapped().inputs);
			auto job = async->workers.Post([this, async, device = m_graphics.device, inputs, compiling,
			                     push_data_cursor, specialization = program->specialization,
			                     translated = std::move(translation.mapped().translated)]() mutable {
				auto compiled = CompileStage(device, inputs->key, inputs->options,
				                             std::move(*translated), specialization,
				                             push_data_cursor, {});
				async->Finish([this, inputs, compiling, push_data_cursor,
				               pending = std::make_unique<Async::PendingStage>(
				                   device, std::move(compiled))]() mutable {
					m_async->in_flight.erase(compiling);
					auto& cached = m_program_cache.at(inputs->key);
					// Another draw's compilation may have made it first.
					if (cached.FindCandidate(pending->stage.module.specialization,
					                         push_data_cursor) != nullptr) {
						return;
					}
					pending->taken = true;
					AddModule(cached, inputs->key, std::move(pending->stage));
				});
			}, ShaderWorkers::Priority::Urgent);
			if (job) {
				async->in_flight.emplace(compiling, Async::Flight {std::move(job), false});
			}
			return;
		}
	}

	// Nothing to compile yet: translate the shader.
	if (const auto flying = async->in_flight.find(translating); flying != async->in_flight.end()) {
		flying->second.job->Touch();
		return;
	}
	auto inputs = std::make_shared<StageInputs>();
	inputs->key = key;
	inputs->info = input_info;
	auto& info   = std::get<InputInfo>(inputs->info);
	info.stage   = {};
	inputs->code.assign(params.code.begin(), params.code.end());
	inputs->back_code.assign(params.back_code.begin(), params.back_code.end());
	inputs->user_data = params.user_data;
	inputs->profile   = SpirvProfile(m_graphics);
	inputs->options   = options;
	inputs->options.profile   = &inputs->profile;
	inputs->options.user_data = std::span(inputs->user_data).first(params.user_data_count);
	inputs->options.back_code = inputs->back_code;
	if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
		inputs->options.input_info.vertex = &info;
	} else {
		inputs->options.input_info.pixel = &info;
	}

	const bool new_program = program == nullptr;
	auto job = async->workers.Post([this, async, inputs, translating, new_program] {
		auto translated = std::make_unique<ShaderRecompiler::TranslateResult>(
		    ShaderRecompiler::TranslateProgram(inputs->code, inputs->options));
		std::optional<ShaderRecompiler::IR::ResourcePlan> plan;
		if (new_program) {
			plan.emplace(ShaderRecompiler::IR::ExtractResourcePlan(translated->program));
		}
		async->Finish([this, inputs, translating, translated = std::move(translated),
		               plan = std::move(plan)]() mutable {
			m_async->in_flight.erase(translating);
			if (!m_program_cache.contains(inputs->key)) {
				EXIT_IF(!plan);
				m_program_cache.emplace(inputs->key, Program {.resource_plan = std::move(*plan)});
			}
			m_async->translations.insert_or_assign(
			    inputs->key, Async::Translation {inputs, std::move(translated), AskClockMs()});
		});
	});
	if (job) {
		async->in_flight.emplace(translating, Async::Flight {std::move(job), true});
	}
}

template void PipelineCache::RequestAsync<ShaderVertexInputInfo>(
    const ProgramKey&, Program*, const ShaderParams&, const ShaderVertexInputInfo&,
    const ShaderRecompiler::CompileOptions&, uint32_t, AsyncMode);
template void PipelineCache::RequestAsync<ShaderPixelInputInfo>(
    const ProgramKey&, Program*, const ShaderParams&, const ShaderPixelInputInfo&,
    const ShaderRecompiler::CompileOptions&, uint32_t, AsyncMode);

void PipelineCache::PumpAsync() {
	if (m_async == nullptr) {
		return;
	}
	std::vector<Common::UniqueFunction<void>> done;
	{
		std::scoped_lock lock(m_async->done_mutex);
		done.swap(m_async->done);
	}
	for (auto& apply: done) {
		apply();
	}
	m_async->ReportStatus();
}

bool PipelineCache::Async::NoteTargetDraw(uint64_t address) {
	const auto frame = CurrentFrame();
	auto&      seen  = targets[address];
	if (seen.frame != frame) {
		const auto age = frame - seen.frame;
		seen.seen      = age >= 8 ? 0 : static_cast<uint8_t>(seen.seen << age);
		seen.frame     = frame;
	}
	seen.seen |= 1u;
	if (targets.size() > 4096) {
		// Targets of scenes long gone.
		std::erase_if(targets, [frame](const auto& entry) { return frame - entry.second.frame > 600; });
	}
	return std::popcount(static_cast<unsigned>(seen.seen & 0x3Fu)) >= 3;
}

// Whether the module writes buffers, images or memory through addresses, which other draws and
// dispatches may read: a draw with such a module is never skipped, because the writes are lost.
static bool WritesMemory(const ShaderModule& module) {
	if (module.info.has_address_writes) {
		return true;
	}
	const auto& info = module.info.info;
	return std::ranges::any_of(info.buffers, [](const auto& buffer) { return buffer.written; }) ||
	       std::ranges::any_of(info.images, [](const auto& image) { return image.written; });
}

void PipelineCache::Async::ReportStatus() {
	const auto now = AskClockMs();
	if (now - last_report_ms < 2000) {
		return;
	}
	last_report_ms = now;
	// A translation no draw has come back for in a minute belongs to something no longer on
	// screen; a later draw translates it again.
	std::erase_if(translations, [now](const auto& entry) { return now - entry.second.since > 60000; });
	size_t translating = 0;
	for (const auto& [id, flight]: in_flight) {
		translating += flight.translating ? 1u : 0u;
	}
	const size_t compiling = in_flight.size() - translating;
	uint64_t     oldest    = 0;
	for (const auto& [key, translation]: translations) {
		oldest = std::max(oldest, now - translation.since);
	}
	const auto frame   = CurrentFrame();
	size_t     redrawn = 0;
	for (const auto& [address, history]: targets) {
		// Drawn in the last six frames, and in at least three of them.
		redrawn += frame - history.frame < 6 &&
		                   std::popcount(static_cast<unsigned>(history.seen & 0x3Fu)) >= 3
		               ? 1u
		               : 0u;
	}
	const auto skipped_total = TotalDrawsSkippedForShaders();
	const auto skipped       = skipped_total - reported_skipped;
	reported_skipped         = skipped_total;
	const bool busy = !in_flight.empty() || !translations.empty() || pipelines_building != 0 ||
	                  skipped != 0;
	if (busy) {
		std::printf("Async: %zu translating, %zu compiling, %zu waiting (oldest %.0f s), %u pipelines, "
		            "%zu queued, %" PRIu64 " skipped/2s, %zu/%zu redrawn\n",
		            translating, compiling, translations.size(),
		            static_cast<double>(oldest) / 1000.0, pipelines_building, workers.Queued(),
		            skipped, redrawn, targets.size());
	} else if (was_busy) {
		std::printf("Async: idle\n");
	}
	was_busy = busy;
}

bool PipelineCache::EnsureGraphicsPipeline(const CommandBuffer&   command,
                                           const DrawRenderState& state) {
	// A draw that must not be skipped gets its pipeline from GetGraphicsPipeline, on this thread.
	if (m_async == nullptr || !state.deferrable) {
		return true;
	}
	std::array<const ShaderModule*, 4> stages {};
	const auto key = MakeGraphicsPipelineKey(command, state, stages);
	if (std::ranges::any_of(stages, [](const ShaderModule* stage) {
		    return stage != nullptr && WritesMemory(*stage);
	    })) {
		return true;
	}
	auto [iter, inserted] = m_graphics_pipelines.try_emplace(key);
	auto& pipeline        = iter->second;
	if (inserted) {
		pipeline.pending = true;
		m_async->pipelines_building++;
		// The map's nodes do not move, and the stages' modules sit in deques that only grow.
		m_async->workers.Post([this, async = m_async.get(), &graphics = m_graphics,
		                       driver_cache = m_driver_cache, &pipeline, key, stages] {
			{
				Common::DebugCounters::ScopedTimer timer(
				    Common::DebugCounters::Counter::PipelinesCreated,
				    Common::DebugCounters::Counter::PipelineCreateNs,
				    Common::DebugCounters::Peak::PipelineCreateNs);
				CreatePipelineInternal(graphics, pipeline, key, stages, driver_cache);
			}
			async->Finish([this, &pipeline, key] {
				m_async->pipelines_building--;
				pipeline.pending = false;
				StorePipeline(key);
				CheckpointDriverCache();
			});
		}, ShaderWorkers::Priority::Urgent);
	}
	if (pipeline.pending) {
		NoteDrawSkippedForShaders();
		return false;
	}
	return true;
}

} // namespace Libs::Graphics
