#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMOUNTSLOTS_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMOUNTSLOTS_H_

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Libs::SaveData {

class SaveDataMountSlots {
public:
	static constexpr int    BUSY  = -2;
	static constexpr int    FULL  = -1;
	static constexpr size_t COUNT = 16;

	[[nodiscard]] int FindAvailable(const std::filesystem::path& directory) const {
		int available = FULL;
		for (size_t index = 0; index < m_directories.size(); index++) {
			const auto& mounted = m_directories[index];
			if (mounted.has_value()) {
				std::error_code error;
				if (*mounted == directory || std::filesystem::equivalent(*mounted, directory, error)) {
					return BUSY;
				}
			}
			if (!mounted.has_value() && available == FULL) {
				available = static_cast<int>(index);
			}
		}
		return available;
	}

	void Mount(size_t slot, const std::filesystem::path& directory) {
		m_directories[slot] = directory;
	}

	[[nodiscard]] const std::filesystem::path& Directory(size_t slot) const {
		return *m_directories[slot];
	}

	void Release(size_t slot) {
		if (slot < m_directories.size()) {
			m_directories[slot].reset();
		}
	}

	[[nodiscard]] int Find(std::string_view mount_point) const {
		for (size_t index = 0; index < m_directories.size(); index++) {
			if (m_directories[index].has_value() && MountPoint(index) == mount_point) {
				return static_cast<int>(index);
			}
		}
		return FULL;
	}

	[[nodiscard]] static std::string MountPoint(size_t slot) {
		return "/savedata" + std::to_string(slot);
	}

	[[nodiscard]] bool Empty() const {
		for (const auto& directory: m_directories) {
			if (directory.has_value()) {
				return false;
			}
		}
		return true;
	}

private:
	std::array<std::optional<std::filesystem::path>, COUNT> m_directories;
};

} // namespace Libs::SaveData

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMOUNTSLOTS_H_ */
