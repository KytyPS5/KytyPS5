// SPDX-License-Identifier: GPL-2.0-only
#include "common/pkgArchive.h"
#include "common/archiveReader.h"
#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <thread>
#include <vector>
#include <iostream>
int main(int argc, char** argv) {
    assert(argc == 2); auto reader = Common::OpenPkgArchive(argv[1]); assert(reader);
    auto boot = reader->Find("eboot.bin"), file = reader->Find("data/sparse.bin");
    assert(boot && file && boot->is_file && file->size > (1ULL << 32));
    assert(reader->Find("sce_sys/param.json"));
    assert(!reader->Find("not-present"));
    assert(!reader->Find("data")->is_file);
    assert(reader->List("data").size() == 1);
    std::array<uint8_t, 32> bytes {};
    assert(reader->Read(boot->id, 0, 4, bytes.data()) == 4);
    assert(bytes[0] == 0x7f && bytes[1] == 'E');
    for (uint64_t offset : {0ULL, 262144ULL - 16, (1ULL << 32) + 123}) {
        assert(reader->Read(file->id, offset, bytes.size(), bytes.data()) == bytes.size());
        for (size_t i = 0; i < bytes.size(); ++i) assert(bytes[i] == i);
    }
    assert(reader->Read(file->id, file->size, 32, bytes.data()) == 0);
    assert(reader->Read(file->id, file->size - 7, 32, bytes.data()) == 7);
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) workers.emplace_back([&] { std::array<uint8_t, 32> b {}; for (int n = 0; n < 30; ++n) { assert(reader->Read(file->id, 0, b.size(), b.data()) == b.size()); for (size_t x=0;x<b.size();++x) assert(b[x] == x); } });
    for (auto& worker : workers) worker.join();
    std::cout << "Bridge probe passed: mount, lookup, directories, cross-block/64-bit/EOF/concurrent reads\n";
}
