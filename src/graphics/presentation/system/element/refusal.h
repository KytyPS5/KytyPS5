#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_REFUSAL_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_REFUSAL_H_

#include <cstdint>
#include <optional>

namespace Libs::Graphics::SystemUi {

/// Why the element layer refused a handle or a build.
enum class ElementError : uint8_t {
	None,            // the handle refers to nothing
	Stale,           // made in an earlier frame
	OutOfRange,      // names a node the frame doesn't have
	AlreadyParented, // a child that already has a parent, or is listed twice
	NotBegun,        // the frame has not had Begin called yet
	// Kept last, as refusal.cpp checks every reason has a bit of its own.
	DuplicateKey, // two children of one node share a key
};

/// Logs that the element layer refused something for the reason `why`, so a tree that quietly drew
/// nothing can be traced. Most callers drop the refusal, and a refused tree simply doesn't show.
/// Each reason is logged once per run, as a refusal usually repeats every frame. None isn't logged,
/// since a none handle is usually what an earlier refusal handed on, or an optional part left
/// unset. Returns whether this was the first report of the reason.
bool ReportRefusal(ElementError why);

/// Refuses a build for the reason `why`. It is logged as ReportRefusal logs it, and recorded in the
/// innermost RefusalScope open on this thread, if there is one, so a builder needn't take a
/// parameter for its reason.
void RefuseBuild(ElementError why);

/// Records the refusals made on this thread while it is open, for a caller that wants to know why
/// something it built came back as a none handle. Everyone else opens none and the refusal is only
/// logged. Scopes nest, only the innermost records, and they must end in the reverse order they
/// were opened, as local variables do.
class RefusalScope {
public:
	/// Starts recording refusals on this thread. A scope that isn't named ends on the same line, so
	/// the compiler warns about one.
	[[nodiscard]] RefusalScope();

	/// Stops recording, and puts back the scope that was recording before.
	~RefusalScope();

	RefusalScope(const RefusalScope&)            = delete;
	RefusalScope& operator=(const RefusalScope&) = delete;
	RefusalScope(RefusalScope&&)                 = delete;
	RefusalScope& operator=(RefusalScope&&)      = delete;

	/// The first refusal since the scope opened, or nothing. The first and not the last, since a
	/// refused element becomes a none handle and every parent built from it is then refused as
	/// None, which says nothing about the cause.
	[[nodiscard]] std::optional<ElementError> First() const { return m_first; }

private:
	/// Records a refusal, which only RefuseBuild makes.
	friend void RefuseBuild(ElementError why);

	std::optional<ElementError> m_first;
	RefusalScope*               m_previous;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_REFUSAL_H_
