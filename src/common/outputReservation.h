#ifndef KYTY_COMMON_OUTPUTRESERVATION_H_
#define KYTY_COMMON_OUTPUTRESERVATION_H_

#include "common/common.h"

#include <filesystem>
#include <memory>
#include <string>

namespace Common {

class OutputReservation {
public:
	static std::unique_ptr<OutputReservation> Acquire(const std::filesystem::path& output,
	                                                   std::string& error);
	static std::filesystem::path LockPath(const std::filesystem::path& output);
	static bool IsReservedPath(const std::filesystem::path& output);

	~OutputReservation();
	KYTY_CLASS_NO_COPY(OutputReservation);

private:
	OutputReservation();

	struct Private;
	std::unique_ptr<Private> m_private;
};

} // namespace Common

#endif // KYTY_COMMON_OUTPUTRESERVATION_H_
