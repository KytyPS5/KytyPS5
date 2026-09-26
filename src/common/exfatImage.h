#ifndef COMMON_EXFAT_IMAGE_H_
#define COMMON_EXFAT_IMAGE_H_

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Common {

// Read-only support for raw exFAT volume images (not VHD/VHDX containers).
class ExfatImage {
public:
	struct Entry {
		std::string name;
		uint32_t    first_cluster = 0;
		uint64_t    data_length   = 0;
		uint64_t    valid_length  = 0;
		bool        contiguous    = false;
		bool        directory     = false;
		std::shared_ptr<std::vector<uint32_t>> clusters;
	};

	bool Open(const std::filesystem::path& path, std::string* error);
	bool ReadFile(std::string_view path, size_t max_size, std::vector<uint8_t>* data,
	              std::string* error);
	bool Find(std::string_view path, Entry* entry);
	bool PrepareForRead(Entry* entry);
	bool List(std::string_view path, std::vector<Entry>* entries);
	bool Read(const Entry& entry, uint64_t position, void* data, uint32_t length,
	          uint32_t* bytes_read);

private:
	bool ReadAt(uint64_t offset, void* data, size_t length);
	bool ClusterOffset(uint32_t cluster, uint64_t* offset) const;
	bool NextCluster(uint32_t cluster, uint32_t* next);
	bool ReadDirectory(const Entry& directory, std::vector<Entry>* entries);

	std::ifstream m_file;
	std::mutex    m_mutex;
	uint64_t      m_image_size = 0;
	uint64_t      m_sector_size = 0;
	uint64_t      m_cluster_size = 0;
	uint64_t      m_fat_offset = 0;
	uint64_t      m_heap_offset = 0;
	uint64_t      m_fat_length = 0;
	uint32_t      m_cluster_count = 0;
	uint32_t      m_root_cluster = 0;
	std::vector<uint8_t> m_fat_cache;
	std::mutex m_cache_mutex;
	std::unordered_map<uint32_t, std::shared_ptr<std::vector<uint32_t>>> m_chain_cache;
	std::unordered_map<uint32_t, std::vector<Entry>> m_directory_cache;
};

bool IsExfatImagePath(const std::filesystem::path& path);
bool MountExfatImage(const std::filesystem::path& image, std::string* error);
bool ResolveExfatPath(const std::filesystem::path& path, std::shared_ptr<ExfatImage>* image,
                      std::string* relative_path);

} // namespace Common

#endif
