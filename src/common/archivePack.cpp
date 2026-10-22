// Kept out of archive.cpp so the statically linked emulator does not pull in the writer and the
// zstd compressor.
#include "common/archive.h"

#include "common/stringUtils.h"

#include <algorithm>
#include <zarchive/zarchivewriter.h>

namespace Common {

bool PackArchive(const std::filesystem::path& directory, const std::filesystem::path& archive,
                 const std::function<bool(uint64_t done, uint64_t total)>& progress) {
	namespace fs = std::filesystem;
	struct Output {
		File file;
		bool ok = true;
	} output;
	// Create the archive before walking, so a re-run started while a cancelled pack is still
	// walking sees it exists instead of losing its archive to that pack's cleanup.
	if (!output.file.Create(archive)) {
		return false;
	}
	struct Entry {
		fs::path path;
		uint64_t size;
		bool     is_file;
	};
	std::vector<Entry> entries;
	uint64_t           total = 0;
	std::error_code    error;
	// Follow directory links, as the emulator does when it runs the folder.
	fs::recursive_directory_iterator it(directory, fs::directory_options::follow_directory_symlink,
	                                    error);
	for (; !error && it != fs::end(it); it.increment(error)) {
		const bool is_file = !it->is_directory(error);
		const auto size    = is_file && !error ? it->file_size(error) : 0;
		if (error) {
			break;
		}
		entries.push_back({it->path(), size, is_file});
		total += size;
	}
	output.ok = !error;
	ZArchiveWriter writer(
	    [](int32_t, void*) {},
	    [](const void* data, size_t size, void* opaque) {
		    auto&    out     = *static_cast<Output*>(opaque);
		    uint32_t written = 0;
		    out.file.Write(data, static_cast<uint32_t>(size), &written);
		    out.ok = out.ok && written == size;
	    },
	    &output);
	std::vector<uint8_t> buffer(1u << 20u);
	uint64_t             done = 0;
	for (size_t index = 0; output.ok && index < entries.size(); index++) {
		const auto& entry = entries[index];
		const auto  name  = PathToGenericString(entry.path.lexically_relative(directory));
		File        input;
		output.ok = entry.is_file ? writer.StartNewFile(name.c_str()) &&
		                                input.Open(entry.path, File::Mode::Read)
		                          : writer.MakeDir(name.c_str());
		for (uint64_t left = entry.size; output.ok && left != 0;) {
			uint32_t read = 0;
			input.Read(buffer.data(), static_cast<uint32_t>(std::min<uint64_t>(left, buffer.size())),
			           &read);
			writer.AppendData(buffer.data(), read);
			done += read;
			left -= read;
			output.ok = output.ok && read != 0 && progress(done, total);
		}
	}
	if (output.ok) {
		writer.Finalize();
	}
	output.ok = output.ok && output.file.Flush();
	output.file.Close();
	if (!output.ok) {
		File::DeleteFile(archive);
	}
	return output.ok;
}

} // namespace Common
