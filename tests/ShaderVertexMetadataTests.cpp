#include "graphics/shader/shaderVertexMetadata.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ShaderVertexMetadataTests: failed: %s\n", text);
		std::abort();
	}
}

struct Fixture {
	std::array<uint16_t, static_cast<size_t>(AgcDirectResourceType::Last) + 1> offsets {};
	ShaderUserData user_data {};
	ShaderSemantic semantic {};
	ShaderMappedData mapped {};

	Fixture() {
		offsets.fill(AGC_ILLEGAL_DIRECT_OFFSET);
		offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexBufferTable)] = 2;
		offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexAttribDescTable)] = 4;
		user_data.direct_resource_offset = offsets.data();
		user_data.direct_resource_count  = static_cast<uint16_t>(offsets.size());
		mapped.user_data           = &user_data;
		mapped.input_semantics     = &semantic;
		mapped.num_input_semantics = 1;
	}
};

void CheckRejected(const ShaderMappedData& data, const char* text) {
	ShaderVertexMetadata output;
	output.vertex_buffer_reg     = 37;
	output.vertex_attrib_reg     = 41;
	output.input_semantics_count = 7;
	std::string error;
	Check(!ShaderReadVertexMetadata(data, 64, &output, &error), text);
	Check(!error.empty(), "metadata rejection omitted its diagnostic");
	Check(output.vertex_buffer_reg == 37 && output.vertex_attrib_reg == 41 &&
	          output.input_semantics_count == 7,
	      "metadata rejection changed the prior output");
}

void TestValidAndHostileMetadata() {
	Fixture fixture;
	ShaderVertexMetadata output;
	std::string error;
	Check(ShaderReadVertexMetadata(fixture.mapped, 64, &output, &error),
	      "valid AGC vertex metadata was rejected");
	Check(output.vertex_buffer_reg == 2 && output.vertex_attrib_reg == 4 &&
	          output.input_semantics_count == 1,
	      "valid AGC vertex metadata was decoded incorrectly");

	auto unreadable = fixture.mapped;
	unreadable.user_data = reinterpret_cast<ShaderUserData*>(uintptr_t {1});
	CheckRejected(unreadable, "unreadable AGC user-data header was accepted");

	Fixture excessive_resources;
	excessive_resources.user_data.direct_resource_count =
	    static_cast<uint16_t>(excessive_resources.offsets.size() + 1);
	CheckRejected(excessive_resources.mapped, "excessive direct-resource count was accepted");

	Fixture excessive_semantics;
	excessive_semantics.mapped.num_input_semantics = ShaderVertexInputInfo::RES_MAX + 1;
	CheckRejected(excessive_semantics.mapped, "excessive vertex semantic count was accepted");

	Fixture excessive_register;
	excessive_register.offsets[
	    static_cast<size_t>(AgcDirectResourceType::PtrVertexBufferTable)] = 63;
	CheckRejected(excessive_register.mapped, "out-of-domain vertex table SGPR was accepted");

	Fixture bad_semantics;
	bad_semantics.mapped.input_semantics =
	    reinterpret_cast<ShaderSemantic*>(uintptr_t {1});
	CheckRejected(bad_semantics.mapped, "unreadable vertex semantic array was accepted");
}

void TestCrossPageMetadata() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SYSTEM_INFO system_info {};
	GetSystemInfo(&system_info);
	const auto page_size = static_cast<uint64_t>(system_info.dwPageSize);
	auto* pages = static_cast<uint8_t*>(
	    VirtualAlloc(nullptr, page_size * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	Check(pages != nullptr, "failed to allocate metadata test pages");
	DWORD old_protect = 0;
	Check(VirtualProtect(pages + page_size, page_size, PAGE_NOACCESS, &old_protect) != 0,
	      "failed to protect metadata test page");
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	const auto native_page_size = sysconf(_SC_PAGESIZE);
	Check(native_page_size > 0, "failed to query page size");
	const auto page_size = static_cast<uint64_t>(native_page_size);
	auto* pages = static_cast<uint8_t*>(
	    mmap(nullptr, page_size * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	Check(pages != MAP_FAILED, "failed to allocate metadata test pages");
	Check(mprotect(pages + page_size, page_size, PROT_NONE) == 0,
	      "failed to protect metadata test page");
#else
	return;
#endif

	Fixture fixture;
	auto header_crossing = fixture.mapped;
	header_crossing.user_data = reinterpret_cast<ShaderUserData*>(
	    pages + page_size - sizeof(ShaderUserData) + 1);
	CheckRejected(header_crossing, "cross-page AGC user-data header was accepted");

	auto offsets_crossing = fixture.mapped;
	fixture.user_data.direct_resource_offset =
	    reinterpret_cast<uint16_t*>(pages + page_size - sizeof(uint16_t));
	CheckRejected(offsets_crossing, "cross-page direct-resource offsets were accepted");

	fixture.user_data.direct_resource_offset = fixture.offsets.data();
	auto semantics_crossing = fixture.mapped;
	semantics_crossing.input_semantics =
	    reinterpret_cast<ShaderSemantic*>(pages + page_size - sizeof(uint16_t));
	CheckRejected(semantics_crossing, "cross-page vertex semantics were accepted");

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	Check(VirtualFree(pages, 0, MEM_RELEASE) != 0, "failed to free metadata test pages");
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	Check(munmap(pages, page_size * 2) == 0, "failed to free metadata test pages");
#endif
}

} // namespace

int main() {
	TestValidAndHostileMetadata();
	TestCrossPageMetadata();
	std::puts("ShaderVertexMetadataTests: all cases passed");
	return 0;
}
