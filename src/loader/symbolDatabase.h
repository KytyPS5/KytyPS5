#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_SYMBOLDATABASE_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_SYMBOLDATABASE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/stringUtils.h"

#include <unordered_map>
#include <vector>

namespace Loader {

enum class SymbolType {
	Unknown,
	Func,
	Object,
	TlsModule,
	NoType,
};

struct SymbolRecord {
	std::string name;
	std::string dbg_name;
	uint64_t    vaddr;
};

struct SymbolResolve {
	std::string name;
	std::string library;
	int         library_version;
	std::string module;
	int         module_version_major;
	int         module_version_minor;
	SymbolType  type;
};

class SymbolDatabase {
public:
	SymbolDatabase()          = default;
	virtual ~SymbolDatabase() = default;

	void Add(const SymbolResolve& s, uint64_t vaddr);
	void Add(const SymbolResolve& s, uint64_t vaddr, const std::string& dbg_name);

	[[nodiscard]] const SymbolRecord* Find(const SymbolResolve& s) const;
	[[nodiscard]] const SymbolRecord* FindByNid(const std::string& nid, SymbolType type) const;
	[[nodiscard]] const SymbolRecord* FindByName(const std::string& name, SymbolType type) const;

	void DbgDump(const std::string& folder, const std::string& file_name);

	KYTY_CLASS_NO_COPY(SymbolDatabase);

	static std::string GenerateName(const SymbolResolve& s);

private:
	std::vector<SymbolRecord>               m_symbols;
	std::unordered_map<std::string, size_t> m_map;
};

} // namespace Loader

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_SYMBOLDATABASE_H_ */
