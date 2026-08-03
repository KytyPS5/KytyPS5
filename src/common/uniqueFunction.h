#ifndef KYTY_COMMON_UNIQUEFUNCTION_H_
#define KYTY_COMMON_UNIQUEFUNCTION_H_

#include <memory>
#include <type_traits>
#include <utility>

namespace Common {

template <typename Result, typename... Args>
class UniqueFunction {
	class CallableBase {
	public:
		virtual ~CallableBase()               = default;
		virtual Result Invoke(Args&&... args) = 0;
	};

	template <typename Function>
	class Callable final: public CallableBase {
	public:
		explicit Callable(Function function): m_function(std::move(function)) {}

		Result Invoke(Args&&... args) override { return m_function(std::forward<Args>(args)...); }

	private:
		Function m_function;
	};

public:
	UniqueFunction() = default;

	template <typename Function>
	UniqueFunction(Function&& function)
	    : m_callable(std::make_unique<Callable<std::decay_t<Function>>>(
	          std::forward<Function>(function))) {}

	UniqueFunction(UniqueFunction&&) noexcept            = default;
	UniqueFunction& operator=(UniqueFunction&&) noexcept = default;
	UniqueFunction(const UniqueFunction&)                = delete;
	UniqueFunction& operator=(const UniqueFunction&)     = delete;

	Result operator()(Args... args) const {
		return m_callable->Invoke(std::forward<Args>(args)...);
	}

	explicit operator bool() const noexcept { return m_callable != nullptr; }

private:
	std::unique_ptr<CallableBase> m_callable;
};

} // namespace Common

#endif // KYTY_COMMON_UNIQUEFUNCTION_H_
