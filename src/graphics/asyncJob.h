#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_ASYNCJOB_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_ASYNCJOB_H_

#include "common/common.h"
#include "common/profiler.h"
#include "common/threads.h"

#include <functional>
#include <string>
#include <utility>

namespace Libs::Graphics {

class AsyncJob final {
public:
	using Task = std::function<void()>;

	explicit AsyncJob(std::string thread_name = {})
	    : m_thread_name(std::move(thread_name)), m_worker_thread(WorkerEntry, this) {}

	~AsyncJob() {
		{
			Common::LockGuard lock(m_mutex);
			m_stop_requested = true;
			m_task_available_condition.Signal();
		}

		m_worker_thread.Join();
	}

	KYTY_CLASS_NO_COPY(AsyncJob);

	void Execute(Task task) {
		Common::LockGuard lock(m_mutex);
		while (m_is_busy) {
			m_idle_condition.Wait(&m_mutex);
		}

		m_task    = std::move(task);
		m_is_busy = true;
		m_task_available_condition.Signal();
	}

	void Wait() {
		Common::LockGuard lock(m_mutex);
		while (m_is_busy) {
			m_idle_condition.Wait(&m_mutex);
		}
	}

private:
	static void WorkerEntry(void* data) { static_cast<AsyncJob*>(data)->WorkerLoop(); }

	void WorkerLoop() {
		if (!m_thread_name.empty()) {
			KYTY_PROFILER_THREAD(m_thread_name.c_str());
		}

		for (;;) {
			Task task;
			{
				Common::LockGuard lock(m_mutex);
				while (!m_is_busy && !m_stop_requested) {
					m_task_available_condition.Wait(&m_mutex);
				}

				if (!m_is_busy) {
					return;
				}

				task = std::move(m_task);
			}

			task();

			{
				Common::LockGuard lock(m_mutex);
				m_is_busy = false;
				m_idle_condition.SignalAll();
			}
		}
	}

	std::string     m_thread_name;
	Common::Mutex   m_mutex;
	Common::CondVar m_task_available_condition;
	Common::CondVar m_idle_condition;
	Task            m_task;
	bool            m_is_busy        = false;
	bool            m_stop_requested = false;
	Common::Thread  m_worker_thread;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_ASYNCJOB_H_ */
