#include "loader/gamePatch.h"

#include "common/stringUtils.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/elf.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Loader::GamePatch {

namespace {

struct Write {
	std::string          patch_name;
	uint64_t             address = 0;
	std::vector<uint8_t> expected;
	std::vector<uint8_t> replacement;
};

struct Plan {
	std::string              title_id;
	std::string              game_version;
	std::string              process;
	std::vector<Write>       writes;
	std::vector<std::string> patch_names;
};

using Json = nlohmann::json;

bool Fail(std::string* error, std::string message) {
	*error = std::move(message);
	return false;
}

const Json::string_t* StringField(const Json& object, const char* name) {
	const auto value = object.find(name);
	return value == object.end() ? nullptr : value->get_ptr<const Json::string_t*>();
}

const Json::array_t* ArrayField(const Json& object, const char* name) {
	const auto value = object.find(name);
	return value == object.end() ? nullptr : value->get_ptr<const Json::array_t*>();
}

bool ParseBytes(std::string_view text, std::vector<uint8_t>* bytes) {
	if (text.empty() || (text.size() % 2) != 0) {
		return false;
	}

	bytes->resize(text.size() / 2);
	for (size_t index = 0; index < bytes->size(); index++) {
		unsigned int value      = 0;
		const char*  begin      = text.data() + index * 2;
		const auto [end, error] = std::from_chars(begin, begin + 2, value, 16);
		if (error != std::errc {} || end != begin + 2) {
			return false;
		}
		(*bytes)[index] = static_cast<uint8_t>(value);
	}
	return true;
}

bool ReadJson(const std::filesystem::path& path, Json* root, std::string* error) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return Fail(error, "could not read patch plan");
	}

	*root = Json::parse(file, nullptr, false);
	return root->is_object() || Fail(error, "patch plan is not valid JSON");
}

bool LoadPlan(const std::filesystem::path& path, Plan* plan, std::string* error) {
	Json root;
	if (!ReadJson(path, &root, error)) {
		return false;
	}

	const auto* title_id     = StringField(root, "title_id");
	const auto* game_version = StringField(root, "game_version");
	const auto* process      = StringField(root, "process");
	const auto* patches      = ArrayField(root, "patches");
	if (title_id == nullptr || game_version == nullptr || process == nullptr ||
	    patches == nullptr) {
		return Fail(error, "invalid patch plan");
	}

	plan->title_id     = *title_id;
	plan->game_version = *game_version;
	plan->process      = *process;

	for (const auto& patch_json: *patches) {
		const auto enabled = patch_json.find("enabled");
		if (enabled != patch_json.end() && enabled->is_boolean() && !enabled->get<bool>()) {
			continue;
		}

		const auto* name   = StringField(patch_json, "name");
		const auto* writes = ArrayField(patch_json, "writes");
		if (name == nullptr || writes == nullptr) {
			return Fail(error, "invalid patch entry");
		}

		plan->patch_names.push_back(*name);
		for (const auto& write_json: *writes) {
			const auto* expected    = StringField(write_json, "expected");
			const auto* replacement = StringField(write_json, "replacement");
			if (expected == nullptr || replacement == nullptr) {
				return Fail(error, "invalid patch write");
			}

			Write write {
			    .patch_name = *name,
			};
			if (!ParseBytes(*expected, &write.expected) ||
			    !ParseBytes(*replacement, &write.replacement) ||
			    write.expected.size() != write.replacement.size()) {
				return Fail(error, "invalid patch bytes");
			}

			plan->writes.push_back(std::move(write));
		}
	}
	return true;
}

bool ValidateTarget(const Plan& plan, const Program* program, std::string* error) {
	if (program == nullptr || program->elf == nullptr || program->base_vaddr == 0) {
		return Fail(error, "main executable is not loaded");
	}

	std::string title_id;
	std::string game_version;
	if (!SystemContentParamSfoGetString("TITLE_ID", &title_id) ||
	    !SystemContentParamSfoGetString("APP_VER", &game_version)) {
		return Fail(error, "game metadata is incomplete");
	}
	if (!Common::EqualNoCase(program->file_name.filename().string(), plan.process) ||
	    !Common::EqualNoCase(title_id, plan.title_id) || game_version != plan.game_version) {
		return Fail(error, "patch plan does not match the loaded game");
	}
	return true;
}

bool ResolveWrite(const Program& program, Write* write, std::string* error) {
	const auto* ehdr = program.elf->GetEhdr();
	const auto* phdr = program.elf->GetPhdr();

	uint64_t match       = 0;
	size_t   match_count = 0;
	for (Elf64_Half index = 0; index < ehdr->e_phnum; index++) {
		const auto& segment = phdr[index];
		const bool  loaded  = segment.p_type == PT_LOAD || segment.p_type == PT_OS_RELRO;
		if (!loaded || segment.p_filesz < write->expected.size()) {
			continue;
		}

		const auto  segment_address = program.base_vaddr + segment.p_vaddr;
		const auto* begin           = reinterpret_cast<const uint8_t*>(segment_address);
		const auto* end             = begin + segment.p_filesz;
		for (auto* current = begin; current < end;) {
			const auto* found =
			    std::search(current, end, write->expected.begin(), write->expected.end());
			if (found == end) {
				break;
			}

			const auto address = reinterpret_cast<uint64_t>(found);
			if (match == 0) {
				match = address;
			}
			match_count++;
			current = found + 1;
		}
	}

	::printf("Game patch: found %zu entries for '%s'\n", match_count, write->patch_name.c_str());
	if (match == 0) {
		return Fail(error, "original bytes not found for '" + write->patch_name + "'");
	}
	write->address = match;
	return true;
}

bool PrepareWrites(Plan* plan, const Program& program, std::string* error) {
	for (auto& write: plan->writes) {
		if (!ResolveWrite(program, &write, error)) {
			return false;
		}
	}
	return true;
}

bool ApplyWrites(Plan* plan, std::string* error) {
	for (auto& write: plan->writes) {
		const auto size = write.replacement.size();
		std::memcpy(reinterpret_cast<void*>(write.address), write.replacement.data(), size);
		if (!Common::VirtualMemory::FlushInstructionCache(write.address, size)) {
			return Fail(error, "could not finalize patch");
		}
	}
	return true;
}

} // namespace

bool Apply(const std::filesystem::path& plan_path, Program* program) {
	Plan        plan;
	std::string error;
	if (LoadPlan(plan_path, &plan, &error) && ValidateTarget(plan, program, &error) &&
	    PrepareWrites(&plan, *program, &error) && ApplyWrites(&plan, &error)) {
		for (const auto& name: plan.patch_names) {
			::printf("Successfully applied patch: %s\n", name.c_str());
		}
		return true;
	}

	::printf("Game patch error: %s\n", error.c_str());
	return false;
}

} // namespace Loader::GamePatch
