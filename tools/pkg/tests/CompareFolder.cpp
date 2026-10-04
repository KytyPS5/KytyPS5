// SPDX-License-Identifier: GPL-2.0-only
// Compare the native PKG transport's bytes with a known-working extracted tree.
#include "common/pkgArchive.h"
#include "common/archiveReader.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <vector>

static int Compare(const std::filesystem::path& package, const std::filesystem::path& folder,
                   const std::string& only) {
    auto reader = Common::OpenPkgArchive(package);
    if (!reader || !std::filesystem::is_directory(folder)) return 2;
    std::vector<std::string> pending {""};
    std::set<std::string> files;
    size_t checked = 0, failures = 0;
    std::vector<char> actual(1024 * 1024), expected(actual.size());
    while (!pending.empty()) {
        auto directory = std::move(pending.back()); pending.pop_back();
        for (const auto& child : reader->List(directory)) {
            auto name = directory.empty() ? child.name : directory + "/" + child.name;
            if (!child.is_file) { pending.push_back(name); continue; }
            files.insert(name);
            if (!only.empty() && name != only) continue;
            auto entry = reader->Find(name);
            auto path = folder / std::filesystem::path(std::u8string(name.begin(), name.end()));
            if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) != entry->size) {
                std::cout << "MISSING_OR_SIZE_MISMATCH " << name << '\n'; ++failures; continue;
            }
            std::ifstream source(path, std::ios::binary);
            bool equal = true;
            for (uint64_t offset = 0; offset < entry->size;) {
                auto count = static_cast<uint32_t>(std::min<uint64_t>(actual.size(), entry->size - offset));
                source.read(expected.data(), count);
                const auto got = reader->Read(entry->id, offset, count, actual.data());
                if (source.gcount() != count || got != count) {
                    std::cout << "SHORT_READ " << name << " offset=" << offset << " pkg=" << got << '\n';
                    equal = false; break;
                }
                auto mismatch = std::mismatch(actual.begin(), actual.begin() + count, expected.begin());
                if (mismatch.first != actual.begin() + count) {
                    std::cout << "BYTE_MISMATCH " << name << " offset=" << offset + (mismatch.first - actual.begin()) << '\n';
                    equal = false; break;
                }
                offset += count;
            }
            ++checked;
            if (!equal) ++failures;
            else std::cout << "MATCH " << name << '\n';
        }
    }
    if (!only.empty() && !files.contains(only)) { std::cout << "MISSING_IN_PKG " << only << '\n'; ++failures; }
    if (only.empty()) {
        for (const auto& item : std::filesystem::recursive_directory_iterator(folder)) {
            if (!item.is_regular_file()) continue;
            auto utf8 = item.path().lexically_relative(folder).generic_u8string();
            std::string name(utf8.begin(), utf8.end());
            if (!files.contains(name)) { std::cout << "EXTRA_IN_FOLDER " << name << '\n'; ++failures; }
        }
    }
    std::cout << "Compared " << checked << " files; differences/errors=" << failures << '\n';
    return failures ? 1 : 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    if (argc != 3 && argc != 4) { std::cerr << "Usage: pkg_compare_folder PACKAGE FOLDER [relative-file]\n"; return 2; }
    try {
        std::string only;
        if (argc == 4) { auto name = std::filesystem::path(argv[3]).generic_u8string(); only.assign(name.begin(), name.end()); }
        return Compare(argv[1], argv[2], only);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
