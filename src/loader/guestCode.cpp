#include "loader/guestCode.h"

#include "graphics/host_gpu/hostMemory.h"
#include "loader/elf.h"
#include "loader/runtimeLinker.h"

namespace Loader::GuestCode {

bool Readable(uint64_t address, uint64_t size) {
	uint64_t readable = 0;
	return Libs::Graphics::HostMemoryQueryReadable(address, size, readable) && readable >= size;
}

std::vector<uint64_t> Find(const Program& program, const Pattern& pattern, size_t limit) {
	std::vector<uint64_t> found;
	const auto*           elf = program.elf.get();
	if (elf == nullptr || elf->GetEhdr() == nullptr || elf->GetPhdr() == nullptr ||
	    !pattern.Valid()) {
		return found;
	}
	const auto* phdr = elf->GetPhdr();
	for (size_t i = 0; i < elf->GetEhdr()->e_phnum && found.size() < limit; i++) {
		if (phdr[i].p_type != PT_LOAD || (phdr[i].p_flags & PF_X) == 0 || phdr[i].p_filesz == 0) {
			continue;
		}
		const auto address = program.base_vaddr + phdr[i].p_vaddr;
		if (!Readable(address, phdr[i].p_filesz)) {
			continue;
		}
		const std::span code(reinterpret_cast<const uint8_t*>(address), phdr[i].p_filesz);
		for (const auto offset: pattern.Find(code, limit - found.size())) {
			found.push_back(address + offset);
		}
	}
	return found;
}

std::optional<uint64_t> FindUnique(const Program& program, const Pattern& pattern) {
	const auto found = Find(program, pattern, 2);
	return found.size() == 1 ? std::optional(found[0]) : std::nullopt;
}

} // namespace Loader::GuestCode
