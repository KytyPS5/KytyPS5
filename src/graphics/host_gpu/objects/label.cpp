#include "graphics/host_gpu/objects/label.h"

#include "common/assert.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace Libs::Graphics {

enum LabelStatus {
	New,
	Active,
	ActiveDeleted,
	NotActive,
};

struct LabelCallbacks {
	LabelCallback callback_1           = nullptr;
	LabelCallback callback_2           = nullptr;
	uint64_t      args[LABEL_ARGS_MAX] = {};
};

struct LabelEvent final {
	vk::Device device = nullptr;
	vk::Event  event  = nullptr;

	~LabelEvent() {
		if (event != nullptr) {
			device.destroyEvent(event, nullptr);
		}
	}
};

struct LabelSubmission {
	std::shared_ptr<LabelEvent> completion;
	LabelCallbacks              callbacks;
};

struct Label {
	vk::Device                   device = nullptr;
	LabelStatus                  status = LabelStatus::New;
	LabelCallbacks               callbacks;
	std::vector<LabelSubmission> submissions;
};

class LabelManager {
public:
	LabelManager() {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
		Common::Thread t(ThreadRun, this);
		t.Detach();
	}
	~LabelManager() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(LabelManager);

	Label* Create(GraphicContext* ctx, LabelCallback callback_1, LabelCallback callback_2,
	              const uint64_t* args);
	void   Delete(Label* label);
	void   Set(CommandBuffer* buffer, Label* label);
	void   Drain();

private:
	static void ThreadRun(void* data);

	bool        Remove(Label* label);
	static void Destroy(Label* label);

	Common::Mutex       m_mutex;
	Common::CondVar     m_cond_var;
	std::vector<Label*> m_labels;
	uint64_t            m_callbacks_in_flight = 0;
};

static LabelManager*     g_label_manager     = nullptr;
static thread_local bool g_in_label_callback = false;

class LabelCallbackScope final {
public:
	LabelCallbackScope() {
		if (g_in_label_callback) {
			EXIT("recursive GPU label callback\n");
		}
		g_in_label_callback = true;
	}
	~LabelCallbackScope() {
		if (!g_in_label_callback) {
			EXIT("GPU label callback scope is not active\n");
		}
		g_in_label_callback = false;
	}
};

void LabelManager::ThreadRun(void* data) {
	auto* manager = static_cast<LabelManager*>(data);

	for (;;) {
		manager->m_mutex.Lock();

		uint64_t active_count = 0;

		std::vector<Label*>          deleted_labels;
		std::vector<LabelCallbacks>  fired_labels;
		std::vector<LabelSubmission> finished_submissions;
		deleted_labels.reserve(manager->m_labels.size());

		for (auto& label: manager->m_labels) {
			for (auto it = label->submissions.begin(); it != label->submissions.end();) {
				active_count++;

				if (it->completion == nullptr || it->completion->device == nullptr ||
				    it->completion->event == nullptr) {
					EXIT("GPU label submission has no completion event\n");
				}
				const auto status = it->completion->device.getEventStatus(it->completion->event);
				switch (status) {
					case vk::Result::eEventSet:
						fired_labels.push_back(it->callbacks);
						finished_submissions.push_back(*it);
						it = label->submissions.erase(it);
						break;
					case vk::Result::eEventReset: ++it; break;
					default: EXIT("vkGetEventStatus returned an unexpected result\n");
				}
			}

			if (label->submissions.empty()) {
				switch (label->status) {
					case LabelStatus::ActiveDeleted: deleted_labels.push_back(label); break;
					case LabelStatus::Active: label->status = LabelStatus::NotActive; break;
					default: break;
				}
			}
		}

		if (active_count == 0) {
			manager->m_cond_var.Wait(&manager->m_mutex);
		}
		if (fired_labels.size() > UINT64_MAX - manager->m_callbacks_in_flight) {
			EXIT("GPU label callback count overflow\n");
		}
		manager->m_callbacks_in_flight += fired_labels.size();

		for (auto& label: deleted_labels) {
			bool removed = manager->Remove(label);
			EXIT_NOT_IMPLEMENTED(!removed);
		}

		manager->m_mutex.Unlock();

		// Each completion event is shared with the recording command buffer's fence retainer.
		// The event is destroyed only after both the label thread observed it and that command
		// buffer completed, even when either side wins the race.
		(void)finished_submissions;

		for (auto& label: deleted_labels) {
			Destroy(label);
		}

		for (auto& label: fired_labels) {
			LabelCallbackScope callback_scope;

			if (label.callback_1 != nullptr) {
				(void)label.callback_1(label.args);
			}

			if (label.callback_2 != nullptr) {
				label.callback_2(label.args);
			}
		}

		if (!fired_labels.empty()) {
			Common::LockGuard lock(manager->m_mutex);
			if (manager->m_callbacks_in_flight < fired_labels.size()) {
				EXIT("GPU label callback count underflow\n");
			}
			manager->m_callbacks_in_flight -= fired_labels.size();
			manager->m_cond_var.SignalAll();
		}

		Common::Thread::SleepMicro(100);
	}
}

Label* LabelManager::Create(GraphicContext* ctx, LabelCallback callback_1, LabelCallback callback_2,
                            const uint64_t* args) {
	EXIT_IF(ctx == nullptr);

	Common::LockGuard lock(m_mutex);

	auto* label = new Label;

	label->status               = LabelStatus::New;
	label->device               = ctx->device;
	label->callbacks.callback_1 = callback_1;
	label->callbacks.callback_2 = callback_2;

	if (args != nullptr) {
		for (int i = 0; i < LABEL_ARGS_MAX; i++) {
			label->callbacks.args[i] = args[i];
		}
	}

	m_labels.push_back(label);

	return label;
}

bool LabelManager::Remove(Label* label) {
	EXIT_IF(label == nullptr);
	EXIT_IF(label->device == nullptr);

	Common::LockGuard lock(m_mutex);

	const auto it = std::find(m_labels.begin(), m_labels.end(), label);
	EXIT_NOT_IMPLEMENTED(it == m_labels.end());

	EXIT_NOT_IMPLEMENTED(label->status != LabelStatus::NotActive &&
	                     label->status != LabelStatus::Active &&
	                     label->status != LabelStatus::ActiveDeleted);

	if (!label->submissions.empty()) {
		label->status = LabelStatus::ActiveDeleted;

		return false;
	}

	m_labels.erase(it);

	return true;
}

void LabelManager::Destroy(Label* label) {
	EXIT_IF(label == nullptr);
	EXIT_IF(label->device == nullptr);

	EXIT_NOT_IMPLEMENTED(!label->submissions.empty());

	delete label;
}

void LabelManager::Delete(Label* label) {
	if (Remove(label)) {
		Destroy(label);
	}
}

void LabelManager::Set(CommandBuffer* buffer, Label* label) {
	EXIT_IF(label == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());
	EXIT_IF(label->device == nullptr);

	Common::LockGuard lock(m_mutex);

	const auto it = std::find(m_labels.begin(), m_labels.end(), label);
	EXIT_NOT_IMPLEMENTED(it == m_labels.end());

	EXIT_NOT_IMPLEMENTED(label->status != LabelStatus::New &&
	                     label->status != LabelStatus::NotActive &&
	                     label->status != LabelStatus::Active);

	label->status = LabelStatus::Active;

	LabelSubmission submission {};
	submission.callbacks          = label->callbacks;
	submission.completion         = std::make_shared<LabelEvent>();
	submission.completion->device = label->device;

	auto vk_buffer = buffer->Handle();

	EXIT_NOT_IMPLEMENTED(vk_buffer == nullptr);

	vk::EventCreateInfo create_info {};
	create_info.sType = vk::StructureType::eEventCreateInfo;
	create_info.pNext = nullptr;
	create_info.flags = {};

	const auto create_result =
	    label->device.createEvent(&create_info, nullptr, &submission.completion->event);
	if (create_result != vk::Result::eSuccess || submission.completion->event == nullptr) {
		EXIT("failed to create label event: %s (%d)\n", VulkanToString(create_result).c_str(),
		     static_cast<int>(create_result));
	}
	buffer->RetainResourceUntilFence(submission.completion);

	// Labels can be reused before an earlier end-of-pipe event has been
	// observed by the polling thread. Capture a separate Vulkan event and
	// callback snapshot for each set so older writes are not lost.
	const auto reset_result = label->device.resetEvent(submission.completion->event);
	if (reset_result != vk::Result::eSuccess) {
		EXIT("failed to reset label event: %s (%d)\n", VulkanToString(reset_result).c_str(),
		     static_cast<int>(reset_result));
	}
	vk_buffer.setEvent(submission.completion->event, vk::PipelineStageFlagBits::eBottomOfPipe);

	label->submissions.push_back(submission);

	m_cond_var.SignalAll();
}

void LabelManager::Drain() {
	m_mutex.Lock();
	for (;;) {
		const bool pending = std::any_of(m_labels.begin(), m_labels.end(), [](const Label* label) {
			return !label->submissions.empty();
		});
		if (!pending && m_callbacks_in_flight == 0) {
			m_mutex.Unlock();
			return;
		}
		m_cond_var.SignalAll();
		m_cond_var.Wait(&m_mutex);
	}
}

void LabelInit() {
	EXIT_IF(g_label_manager != nullptr);

	g_label_manager = new LabelManager;
}

Label* LabelCreate(GraphicContext* ctx, LabelCallback callback_1, LabelCallback callback_2,
                   const uint64_t* args) {
	EXIT_IF(g_label_manager == nullptr);

	return g_label_manager->Create(ctx, callback_1, callback_2, args);
}

void LabelDelete(Label* label) {
	EXIT_IF(g_label_manager == nullptr);

	g_label_manager->Delete(label);
}

void LabelSet(CommandBuffer* buffer, Label* label) {
	EXIT_IF(g_label_manager == nullptr);

	g_label_manager->Set(buffer, label);
}

void LabelDrain() {
	EXIT_IF(g_label_manager == nullptr);
	g_label_manager->Drain();
}

bool LabelInCallback() noexcept {
	return g_in_label_callback;
}

} // namespace Libs::Graphics
