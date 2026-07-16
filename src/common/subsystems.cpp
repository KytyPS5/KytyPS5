#include "common/subsystems.h"

#include "common/assert.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace Common {

class SubsystemPrivate {
public:
	SubsystemPrivate()          = default;
	virtual ~SubsystemPrivate() = default;

	KYTY_CLASS_NO_COPY(SubsystemPrivate);

	bool        failed {false};
	std::string fail_msg;
};

class SubsystemsListPrivate {
public:
	explicit SubsystemsListPrivate(SubsystemsList& p): parent(p) {}

	virtual ~SubsystemsListPrivate() = default;

	void SetArgs(int argc, char** argv) {
		this->m_argc = argc;
		this->m_argv = argv;
	}

	void Add(Subsystem* s, std::initializer_list<Subsystem*> deps) {
		EXIT_IF(!s);

		const char* name = s->Id();

		EXIT_IF(FindByName(name) != nullptr);

		auto nl = std::make_unique<SubsListStruct>();

		nl->s         = s;
		nl->name      = name;
		nl->deps      = nullptr;
		nl->next      = std::move(list);
		nl->prev_init = nullptr;

		auto* node = nl.get();
		list       = std::move(nl);

		for (auto* dep: deps) {
			const char* str = dep->Id();

			auto l      = std::make_unique<DepsListStruct>();
			l->dep_name = str;
			l->next     = std::move(node->deps);

			node->deps = std::move(l);
		}

		node->initialized = false;
	}

	bool InitAll(bool print_msg) {
		while (SubsListStruct* n = FindNextToInitialize()) {
			n->s->Init(&parent);

			if (n->s->m_p->failed) {
				fail_msg  = n->s->m_p->fail_msg.c_str();
				fail_name = n->name;
				return false;
			}

			if (print_msg) {
				printf("Initialized: %s\n", n->name);
			}

			n->initialized = true;

			SubsListStruct* last = last_init;
			last_init            = n;
			n->prev_init         = last;
		}

		return true;
	}

	void DestroyAll(bool print_msg) {
		for (SubsListStruct* n = last_init; n != nullptr; n = n->prev_init) {
			n->s->Destroy(&parent);
			n->initialized = false;

			if (print_msg) {
				printf("Destroyed: %s\n", n->name);
			}
		}

		last_init = nullptr;
	}

	void ShutdownAll() {
		for (SubsListStruct* n = last_init; n != nullptr; n = n->prev_init) {
			n->s->UnexpectedShutdown(&parent);
			n->initialized = false;
		}

		last_init = nullptr;
	}

	struct DepsListStruct {
		const char*                     dep_name;
		std::unique_ptr<DepsListStruct> next;
	};

	struct SubsListStruct {
		Subsystem*                      s;
		const char*                     name;
		std::unique_ptr<DepsListStruct> deps;
		std::unique_ptr<SubsListStruct> next;
		SubsListStruct*                 prev_init;
		bool                            initialized;
	};

	[[nodiscard]] SubsListStruct* FindByName(const char* name) const {
		for (SubsListStruct* n = list.get(); n != nullptr; n = n->next.get()) {
			if (std::strcmp(n->name, name) == 0) {
				return n;
			}
		}

		return nullptr;
	}

	[[nodiscard]] SubsListStruct* FindNextToInitialize() const {
		for (SubsListStruct* n = list.get(); n != nullptr; n = n->next.get()) {
			if (n->initialized) {
				continue;
			}

			DepsListStruct* d = n->deps.get();
			for (; d != nullptr; d = d->next.get()) {
				SubsListStruct* s = FindByName(d->dep_name);
				if ((s == nullptr) || !s->initialized) {
					break;
				}
			}

			if (d == nullptr) {
				return n;
			}
		}

		return nullptr;
	}

	KYTY_CLASS_NO_COPY(SubsystemsListPrivate);

	std::unique_ptr<SubsListStruct> list;
	SubsListStruct*                 last_init = nullptr;
	int                             m_argc    = 0;
	char**                          m_argv    = nullptr;
	const char*                     fail_msg  = nullptr;
	const char*                     fail_name = nullptr;
	SubsystemsList&                 parent;
};

SubsystemsList::SubsystemsList(): m_p(std::make_unique<SubsystemsListPrivate>(*this)) {}

SubsystemsList::~SubsystemsList() = default;

void SubsystemsList::Add(Subsystem* s, std::initializer_list<Subsystem*> deps) {
	m_p->Add(s, deps);
}

bool SubsystemsList::InitAll(bool print_msg) {
	return m_p->InitAll(print_msg);
}

void SubsystemsList::DestroyAll(bool print_msg) {
	m_p->DestroyAll(print_msg);
}

int* SubsystemsList::GetArgc() {
	return &m_p->m_argc;
}

char** SubsystemsList::GetArgv() {
	return m_p->m_argv;
}

Subsystem::Subsystem(): m_p(std::make_unique<SubsystemPrivate>()) {}

Subsystem::~Subsystem() = default;

void Subsystem::Fail(const char* format, ...) {
	va_list args {};
	va_start(args, format);

	va_list args_copy {};
	va_copy(args_copy, args);
	int len = std::vsnprintf(nullptr, 0, format, args_copy);
	va_end(args_copy);

	if (len > 0) {
		std::string msg(static_cast<size_t>(len) + 1, '\0');
		std::vsnprintf(msg.data(), msg.size(), format, args);
		m_p->fail_msg = msg.c_str();
		m_p->failed   = true;
	}

	va_end(args);
}

const char* SubsystemsList::GetFailName() const {
	return m_p->fail_name;
}

const char* SubsystemsList::GetFailMsg() const {
	return m_p->fail_msg;
}

void SubsystemsList::SetArgs(int argc, char* argv[]) {
	m_p->SetArgs(argc, argv);
}

void SubsystemsList::ShutdownAll() {
	m_p->ShutdownAll();
}

} // namespace Common
