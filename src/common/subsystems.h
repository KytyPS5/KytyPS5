#ifndef KYTY_COMMON_SUBSYSTEMS_H_
#define KYTY_COMMON_SUBSYSTEMS_H_

#include "common/common.h"
#include "common/singleton.h"

#include <initializer_list>
#include <memory>

namespace Common {

class Subsystem;
class SubsystemsListPrivate;
class SubsystemPrivate;

class SubsystemsList {
public:
	SubsystemsList();
	virtual ~SubsystemsList();
	void SetArgs(int argc, char* argv[]);

	// void Add(Subsystem* s, const char* name, ...);
	void Add(Subsystem* s, std::initializer_list<Subsystem*> deps);

	bool InitAll(bool print_msg = false);
	void DestroyAll(bool print_msg = false);

	int*   GetArgc();
	char** GetArgv();

	[[nodiscard]] const char* GetFailName() const;
	[[nodiscard]] const char* GetFailMsg() const;

	void ShutdownAll();

	static SubsystemsList* Instance() { return Common::Singleton<SubsystemsList>::Instance(); }

	KYTY_CLASS_NO_COPY(SubsystemsList);

private:
	std::unique_ptr<SubsystemsListPrivate> m_p;
};

using SubsystemsListSingleton = Common::Singleton<SubsystemsList>;

class Subsystem {
public:
	Subsystem();
	virtual ~Subsystem();

	virtual const char* Id()                                       = 0;
	virtual void        Init(SubsystemsList* parent)               = 0;
	virtual void        Destroy(SubsystemsList* parent)            = 0;
	virtual void        UnexpectedShutdown(SubsystemsList* parent) = 0;

	friend class SubsystemsListPrivate;

	KYTY_CLASS_NO_COPY(Subsystem);

protected:
	void Fail(const char* format, ...) KYTY_FORMAT_PRINTF(2, 3);

private:
	std::unique_ptr<SubsystemPrivate> m_p;
};

#define KYTY_SUBSYSTEM_DEFINE(s)                                                                   \
	class s##Subsystem: public Common::Subsystem {                                                 \
	public:                                                                                        \
		static Subsystem* Instance() { return Common::Singleton<s##Subsystem>::Instance(); }       \
		const char*       Id() { return #s; }                                                      \
		void              Init(Common::SubsystemsList* parent);                                    \
		void              Destroy(Common::SubsystemsList* parent);                                 \
		void              UnexpectedShutdown(Common::SubsystemsList* parent);                      \
	};                                                                                             \
	typedef Common::Singleton<s##Subsystem> s##SubsystemSingleton;

#define KYTY_SUBSYSTEM_INIT(s)                                                                     \
	void s##Subsystem::Init([[maybe_unused]] Common::SubsystemsList* parent)
#define KYTY_SUBSYSTEM_DESTROY(s)                                                                  \
	void s##Subsystem::Destroy([[maybe_unused]] Common::SubsystemsList* parent)
#define KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(s)                                                      \
	void s##Subsystem::UnexpectedShutdown([[maybe_unused]] Common::SubsystemsList* parent)

// #define KYTY_SUBSYSTEM_ADD(list, s, ...) list.push_back(s##SubsystemSingleton::Instance(), #s,
// __VA_ARGS__); #define KYTY_SUBSYSTEM_ADD2(list, ...) list.Add2(s##SubsystemSingleton::Instance(),
// __VA_ARGS__);

} // namespace Common

#endif /* KYTY_COMMON_SUBSYSTEMS_H_ */
