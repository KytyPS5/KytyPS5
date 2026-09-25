#include "common/exfatImage.h"

#include "common/stringUtils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>

namespace Common {
namespace {

uint16_t U16(const uint8_t* p) {
	return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

uint32_t U32(const uint8_t* p) {
	return uint32_t(U16(p)) | (uint32_t(U16(p + 2)) << 16);
}

uint64_t U64(const uint8_t* p) {
	return uint64_t(U32(p)) | (uint64_t(U32(p + 4)) << 32);
}

bool SameName(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) !=
		    std::tolower(static_cast<unsigned char>(b[i]))) {
			return false;
		}
	}
	return true;
}

void SetError(std::string* error, const char* message) {
	if (error != nullptr) {
		*error = message;
	}
}

std::mutex                        g_mount_mutex;
std::shared_ptr<ExfatImage>       g_mounted_image;
std::filesystem::path             g_mounted_path;

} // namespace

bool ExfatImage::ReadAt(uint64_t offset, void* data, size_t length) {
	if (offset > m_image_size || length > m_image_size - offset ||
	    offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
		return false;
	}
	std::lock_guard lock(m_mutex);
	m_file.clear();
	m_file.seekg(static_cast<std::streamoff>(offset));
	m_file.read(static_cast<char*>(data), static_cast<std::streamsize>(length));
	return m_file.good() || (length == 0 && !m_file.bad());
}

bool ExfatImage::Open(const std::filesystem::path& path, std::string* error) {
	std::error_code ec;
	m_image_size = std::filesystem::file_size(path, ec);
	if (ec || m_image_size < 512) {
		SetError(error, "image is missing or too small");
		return false;
	}
	m_file.open(path, std::ios::binary);
	if (!m_file) {
		SetError(error, "cannot open image");
		return false;
	}
	std::array<uint8_t, 512> boot {};
	if (!ReadAt(0, boot.data(), boot.size()) ||
	    std::memcmp(boot.data() + 3, "EXFAT   ", 8) != 0 || U16(boot.data() + 510) != 0xaa55) {
		SetError(error, "not a raw exFAT volume image");
		return false;
	}
	const auto sector_shift  = boot[108];
	const auto cluster_shift = boot[109];
	if (sector_shift < 9 || sector_shift > 12 || cluster_shift > 25 - sector_shift ||
	    boot[110] != 1 || U16(boot.data() + 104) != 0x0100) {
		SetError(error, "unsupported exFAT geometry");
		return false;
	}
	m_sector_size   = uint64_t(1) << sector_shift;
	m_cluster_size  = m_sector_size << cluster_shift;
	m_fat_offset    = uint64_t(U32(boot.data() + 80)) * m_sector_size;
	m_fat_length    = uint64_t(U32(boot.data() + 84)) * m_sector_size;
	m_heap_offset   = uint64_t(U32(boot.data() + 88)) * m_sector_size;
	m_cluster_count = U32(boot.data() + 92);
	m_root_cluster  = U32(boot.data() + 96);
	const auto volume_sectors = U64(boot.data() + 72);
	if (volume_sectors == 0 || volume_sectors > m_image_size / m_sector_size) {
		SetError(error, "invalid exFAT volume length");
		return false;
	}
	m_image_size = volume_sectors * m_sector_size;
	if (m_cluster_count == 0 || m_root_cluster < 2 ||
	    uint64_t(m_root_cluster) >= uint64_t(m_cluster_count) + 2 ||
	    m_fat_offset > m_image_size || m_fat_length > m_image_size - m_fat_offset ||
	    (uint64_t(m_cluster_count) + 2) * 4 > m_fat_length ||
	    m_heap_offset > m_image_size ||
	    uint64_t(m_cluster_count) > (m_image_size - m_heap_offset) / m_cluster_size) {
		SetError(error, "invalid exFAT volume bounds");
		return false;
	}
	const auto fat_bytes = (uint64_t(m_cluster_count) + 2) * 4;
	if (fat_bytes <= 64ULL * 1024 * 1024) {
		m_fat_cache.resize(static_cast<size_t>(fat_bytes));
		if (!ReadAt(m_fat_offset, m_fat_cache.data(), m_fat_cache.size())) {
			SetError(error, "cannot read exFAT allocation table");
			return false;
		}
	}
	return true;
}

bool ExfatImage::ClusterOffset(uint32_t cluster, uint64_t* offset) const {
	if (cluster < 2 || uint64_t(cluster) >= uint64_t(m_cluster_count) + 2) {
		return false;
	}
	*offset = m_heap_offset + uint64_t(cluster - 2) * m_cluster_size;
	return *offset <= m_image_size && m_cluster_size <= m_image_size - *offset;
}

bool ExfatImage::NextCluster(uint32_t cluster, uint32_t* next) {
	if (cluster < 2 || uint64_t(cluster) >= uint64_t(m_cluster_count) + 2) {
		return false;
	}
	if (!m_fat_cache.empty()) {
		*next = U32(m_fat_cache.data() + size_t(cluster) * 4);
		return true;
	}
	uint8_t value[4] {};
	if (!ReadAt(m_fat_offset + uint64_t(cluster) * 4, value, sizeof(value))) {
		return false;
	}
	*next = U32(value);
	return true;
}

bool ExfatImage::PrepareForRead(Entry* entry) {
	if (entry->contiguous || entry->clusters != nullptr || entry->data_length == 0) {
		return true;
	}
	std::lock_guard cache_lock(m_cache_mutex);
	if (const auto it = m_chain_cache.find(entry->first_cluster);
	    it != m_chain_cache.end()) {
		entry->clusters = it->second;
		return true;
	}
	const uint64_t count = (entry->data_length - 1) / m_cluster_size + 1;
	if (count > m_cluster_count) {
		return false;
	}
	auto chain = std::make_shared<std::vector<uint32_t>>();
	chain->reserve(static_cast<size_t>(count));
	uint32_t cluster = entry->first_cluster;
	for (uint64_t i = 0; i < count; ++i) {
		uint64_t offset = 0;
		if (!ClusterOffset(cluster, &offset)) {
			return false;
		}
		chain->push_back(cluster);
		if (i + 1 < count && (!NextCluster(cluster, &cluster) || cluster >= 0xfffffff8)) {
			return false;
		}
	}
	entry->clusters = std::move(chain);
	m_chain_cache[entry->first_cluster] = entry->clusters;
	return true;
}

bool ExfatImage::ReadDirectory(const Entry& directory, std::vector<Entry>* entries) {
	entries->clear();
	if (!directory.directory) {
		return false;
	}
	if (directory.data_length == 0 && directory.first_cluster == 0) {
		return true;
	}
	{
		std::lock_guard cache_lock(m_cache_mutex);
		if (const auto it = m_directory_cache.find(directory.first_cluster);
		    it != m_directory_cache.end()) {
			*entries = it->second;
			return true;
		}
	}
	// Directory entries are small; cap malformed images before allocating memory.
	constexpr uint64_t max_directory_bytes = 64ULL * 1024 * 1024;
	std::vector<uint8_t> bytes;
	uint32_t cluster = directory.first_cluster;
	uint64_t remaining = directory.data_length;
	for (uint64_t i = 0; i < m_cluster_count && bytes.size() < max_directory_bytes; ++i) {
		uint64_t offset = 0;
		if (!ClusterOffset(cluster, &offset)) {
			return false;
		}
		const auto size = static_cast<size_t>(directory.data_length == 0
		                                          ? m_cluster_size
		                                          : std::min(m_cluster_size, remaining));
		if (size > max_directory_bytes - bytes.size()) {
			return false;
		}
		const auto old = bytes.size();
		bytes.resize(old + size);
		if (!ReadAt(offset, bytes.data() + old, size)) {
			return false;
		}
		if (directory.data_length != 0) {
			remaining -= size;
			if (remaining == 0) {
				break;
			}
		}
		if (directory.contiguous) {
			++cluster;
		} else if (!NextCluster(cluster, &cluster)) {
			return false;
		} else if (cluster >= 0xfffffff8) {
			if (directory.data_length != 0 && remaining != 0) {
				return false;
			}
			break;
		}
	}
	for (size_t i = 0; i + 32 <= bytes.size();) {
		const auto* primary = bytes.data() + i;
		if (primary[0] == 0) {
			break;
		}
		if (primary[0] != 0x85) {
			i += 32;
			continue;
		}
		const auto secondaries = primary[1];
		if (secondaries < 2 || secondaries > 18 ||
		    size_t(secondaries + 1) * 32 > bytes.size() - i) {
			return false;
		}
		const auto* stream = primary + 32;
		if (stream[0] != 0xc0) {
			i += size_t(secondaries + 1) * 32;
			continue;
		}
		const auto name_length = stream[3];
		std::u16string name;
		for (unsigned j = 2; j <= secondaries && name.size() < name_length; ++j) {
			const auto* part = primary + j * 32;
			if (part[0] != 0xc1) {
				return false;
			}
			for (unsigned k = 0; k < 15 && name.size() < name_length; ++k) {
				name.push_back(static_cast<char16_t>(U16(part + 2 + k * 2)));
			}
		}
		if (name.size() != name_length || name.empty()) {
			return false;
		}
		Entry entry;
		try {
			entry.name = Utf16ToUtf8(name);
		} catch (...) {
			return false;
		}
		if (entry.name == "." || entry.name == ".." ||
		    entry.name.find_first_of("/\\\0", 0, 3) != std::string::npos) {
			return false;
		}
		entry.first_cluster = U32(stream + 20);
		entry.data_length = U64(stream + 24);
		entry.valid_length = U64(stream + 8);
		entry.contiguous = (stream[1] & 2) != 0;
		entry.directory = (U16(primary + 4) & 0x10) != 0;
		if (entry.valid_length > entry.data_length ||
		    (entry.data_length != 0 && (entry.first_cluster < 2 ||
		     uint64_t(entry.first_cluster) >= uint64_t(m_cluster_count) + 2))) {
			return false;
		}
		entries->push_back(std::move(entry));
		i += size_t(secondaries + 1) * 32;
	}
	{
		std::lock_guard cache_lock(m_cache_mutex);
		m_directory_cache[directory.first_cluster] = *entries;
	}
	return true;
}

bool ExfatImage::Find(std::string_view path, Entry* entry) {
	Entry current;
	current.directory = true;
	current.first_cluster = m_root_cluster;
	if (path.empty() || path == "/") {
		*entry = current;
		return true;
	}
	while (!path.empty()) {
		while (!path.empty() && (path.front() == '/' || path.front() == '\\')) {
			path.remove_prefix(1);
		}
		if (path.empty()) {
			break;
		}
		auto slash = path.find_first_of("/\\");
		auto name = path.substr(0, slash);
		if (name == "." || name == ".." || !current.directory) {
			return false;
		}
		std::vector<Entry> children;
		if (!ReadDirectory(current, &children)) {
			return false;
		}
		auto it = std::find_if(children.begin(), children.end(),
		                       [name](const Entry& child) { return SameName(child.name, name); });
		if (it == children.end()) {
			return false;
		}
		current = *it;
		if (slash == std::string_view::npos) {
			break;
		}
		path.remove_prefix(slash + 1);
	}
	*entry = std::move(current);
	return true;
}

bool ExfatImage::List(std::string_view path, std::vector<Entry>* entries) {
	Entry directory;
	return Find(path, &directory) && ReadDirectory(directory, entries);
}

bool ExfatImage::Read(const Entry& entry, uint64_t position, void* data, uint32_t length,
                      uint32_t* bytes_read) {
	*bytes_read = 0;
	if (entry.directory || position >= entry.data_length) {
		return !entry.directory;
	}
	auto remaining = std::min<uint64_t>(length, entry.data_length - position);
	auto* out = static_cast<uint8_t*>(data);
	while (remaining != 0) {
		const auto index = position / m_cluster_size;
		const auto in_cluster = position % m_cluster_size;
		const auto amount = static_cast<size_t>(std::min(remaining, m_cluster_size - in_cluster));
		const auto cluster = entry.contiguous
		                         ? uint64_t(entry.first_cluster) + index
		                         : (entry.clusters != nullptr && index < entry.clusters->size()
		                                ? uint64_t((*entry.clusters)[static_cast<size_t>(index)])
		                                : uint64_t(0));
		uint64_t offset = 0;
		if (cluster > UINT32_MAX || !ClusterOffset(static_cast<uint32_t>(cluster), &offset)) {
			return false;
		}
		const auto valid = position < entry.valid_length
		                       ? static_cast<size_t>(std::min<uint64_t>(amount, entry.valid_length - position))
		                       : 0;
		if (valid != 0 && !ReadAt(offset + in_cluster, out, valid)) {
			return false;
		}
		std::memset(out + valid, 0, amount - valid);
		out += amount;
		position += amount;
		remaining -= amount;
		*bytes_read += static_cast<uint32_t>(amount);
	}
	return true;
}

bool ExfatImage::ReadFile(std::string_view path, size_t max_size, std::vector<uint8_t>* data,
                          std::string* error) {
	Entry entry;
	if (!Find(path, &entry) || entry.directory || entry.data_length > max_size ||
	    entry.data_length > UINT32_MAX) {
		SetError(error, "file missing or exceeds size limit");
		return false;
	}
	if (!PrepareForRead(&entry)) {
		SetError(error, "invalid exFAT cluster chain");
		return false;
	}
	data->resize(static_cast<size_t>(entry.data_length));
	uint32_t read = 0;
	if (!Read(entry, 0, data->data(), static_cast<uint32_t>(data->size()), &read) ||
	    read != data->size()) {
		SetError(error, "cannot read image file");
		return false;
	}
	return true;
}

bool IsExfatImagePath(const std::filesystem::path& path) {
	auto extension = path.extension().string();
	return SameName(extension, ".exfat");
}

bool MountExfatImage(const std::filesystem::path& path, std::string* error) {
	auto image = std::make_shared<ExfatImage>();
	if (!image->Open(path, error)) {
		return false;
	}
	ExfatImage::Entry e;
	if (!image->Find("eboot.bin", &e) || e.directory ||
	    !image->Find("sce_sys/param.json", &e) || e.directory) {
		SetError(error, "image root must contain eboot.bin and sce_sys/param.json");
		return false;
	}
	std::lock_guard lock(g_mount_mutex);
	g_mounted_path  = std::filesystem::absolute(path).lexically_normal();
	g_mounted_image = std::move(image);
	return true;
}

bool ResolveExfatPath(const std::filesystem::path& path, std::shared_ptr<ExfatImage>* image,
                      std::string* relative_path) {
	std::lock_guard lock(g_mount_mutex);
	if (g_mounted_image == nullptr) {
		return false;
	}
	const auto full = std::filesystem::absolute(path).lexically_normal();
	auto       rel  = full.lexically_relative(g_mounted_path);
	if (rel.empty()) {
		return false;
	}
	for (const auto& part: rel) {
		if (part == "..") {
			return false;
		}
	}
	*image         = g_mounted_image;
	*relative_path = rel == "." ? "" : rel.generic_string();
	return true;
}

} // namespace Common
