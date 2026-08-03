#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMOUNTSLOTS_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMOUNTSLOTS_H_

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace Libs::SaveData {

class SaveDataMountSlots {
public:
	static constexpr int    BUSY  = -2;
	static constexpr int    FULL  = -1;
	static constexpr size_t COUNT = 16;

	[[nodiscard]] int FindAvailable(std::string_view directory) const {
		int available = FULL;
		for (size_t index = 0; index < m_directories.size(); index++) {
			const auto& mounted = m_directories[index];
			if (mounted == directory) {
				return BUSY;
			}
			if (!mounted.has_value() && available == FULL) {
				available = static_cast<int>(index);
			}
		}
		return available;
	}

	void Mount(size_t slot, std::string_view directory) { m_directories[slot] = directory; }

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
	std::array<std::optional<std::string>, COUNT> m_directories;
};

} // namespace Libs::SaveData

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMOUNTSLOTS_H_ */
