// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <filesystem>
#include <memory>
namespace Common {
class ArchiveReader;
std::shared_ptr<ArchiveReader> OpenPkgArchive(const std::filesystem::path& path);
} // namespace Common
