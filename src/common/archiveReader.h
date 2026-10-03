// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "common/file.h"

#include <optional>
#include <string_view>
namespace Common {
// Shared interface for read-only archive backends. Guest paths/cursors remain in archive.cpp.
class ArchiveReader {
public:
	struct Entry {
		uint64_t id;
		uint64_t size;
		bool     is_file;
	};
	virtual ~ArchiveReader()                                                       = default;
	virtual std::optional<Entry>        Find(std::string_view member)              = 0;
	virtual std::vector<File::DirEntry> List(std::string_view member)              = 0;
	virtual uint64_t Read(uint64_t id, uint64_t offset, uint32_t size, void* data) = 0;
};
} // namespace Common
