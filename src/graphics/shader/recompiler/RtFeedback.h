#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_RTFEEDBACK_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_RTFEEDBACK_H_

// Ray tracing call-table feedback.
//
// A ray tracing kernel calls hit functions through a table that can hold hundreds of records, most of
// which a frame never reaches. Inlining every record makes a kernel the driver cannot compile in
// reasonable time, so only the records the GPU has been seen to use are inlined. A call whose record is
// not inlined logs its index here and returns; the host picks the index up and retranslates the kernel
// with the record added.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::RtFeedback {

// A call site gets a region of this many dwords in the host-visible usage buffer: one flag per record.
constexpr uint32_t RegionDwords = 4096u;
constexpr uint32_t RegionCount  = 256u;
// Tables at most this long are inlined whole, as before.
constexpr uint32_t FeedbackMinRecords = 64u;
// `S_CMP_EQ_U32 <magic | slot>, <index register>`: the translator turns this into a store of 1 to
// usage[slot][index register >> 4]; the compare itself is harmless. Slot is at most RegionCount - 1.
constexpr uint32_t LogMagic = 0xa5a50000u;
constexpr uint32_t LogMask  = 0xffffff00u;

struct Buffer {
	std::atomic<uint64_t>  device_address {0};
	std::atomic<uint32_t*> host {nullptr};
};

inline Buffer& GetBuffer() {
	static Buffer buffer;
	return buffer;
}

inline uint32_t SlotFor(uint64_t shader_hash, uint32_t pc) {
	static std::mutex mutex;
	static std::map<std::pair<uint64_t, uint32_t>, uint32_t> slots;
	std::lock_guard lock(mutex);
	const auto key = std::make_pair(shader_hash, pc);
	if (const auto found = slots.find(key); found != slots.end()) return found->second;
	// A hash modulo the region count aliases unrelated call sites. Assign regions
	// uniquely for this process; exhaustion falls back to complete expansion.
	if (slots.size() >= RegionCount) return UINT32_MAX;
	const auto slot = static_cast<uint32_t>(slots.size());
	slots.emplace(key, slot);
	return slot;
}
// Indices the GPU has logged for this slot, ascending.
inline std::vector<uint32_t> UsedRecords(uint32_t slot) {
	std::vector<uint32_t> used;
	const uint32_t*       host = GetBuffer().host.load();
	if (host == nullptr) return used;
	const uint32_t* region = host + uint64_t {slot} * RegionDwords;
	for (uint32_t index = 0; index < RegionDwords; ++index) {
		if (region[index] != 0u) used.push_back(index);
	}
	return used;
}

// Indices the GPU has logged for this slot, ascending; the region is cleared so each log is seen once. A log lost
// to the race with the GPU is logged again by the next call to the same record.
inline std::vector<uint32_t> DrainLogged(uint32_t slot) {
	std::vector<uint32_t> used;
	uint32_t*             host = GetBuffer().host.load();
	if (host == nullptr) return used;
	uint32_t* region = host + uint64_t {slot} * RegionDwords;
	for (uint32_t index = 0; index < RegionDwords; ++index) {
		if (region[index] != 0u) {
			used.push_back(index);
			region[index] = 0u;
		}
	}
	return used;
}

// Programs the cache replaced because the used set grew. A pipeline compile that has not started yet for one of
// them is pointless: it is dropped when its turn comes.
struct Superseded {
	std::mutex                    mutex;
	std::vector<uint64_t>         ids;
};

inline Superseded& GetSuperseded() {
	static Superseded superseded;
	return superseded;
}

inline void MarkSuperseded(uint64_t program_id) {
	auto& s = GetSuperseded();
	std::lock_guard lock(s.mutex);
	s.ids.push_back(program_id);
}

inline bool IsSuperseded(uint64_t program_id) {
	auto& s = GetSuperseded();
	std::lock_guard lock(s.mutex);
	for (const auto id: s.ids) {
		if (id == program_id) return true;
	}
	return false;
}

// The used set survives restarts so a kernel does not have to grow from nothing every run: one small
// text file per call site (_PipelineCache\rtrec_<hash>_<pc>.txt, one record index per line).
inline std::string PersistPath(uint64_t shader_hash, uint32_t pc) {
	char name[96];
	std::snprintf(name, sizeof(name), "_PipelineCache/rtrec_%016llx_%08x.txt",
	              static_cast<unsigned long long>(shader_hash), pc);
	return name;
}

inline const std::vector<uint32_t>& LoadPersisted(uint64_t shader_hash, uint32_t pc) {
	static std::mutex                                           mutex;
	static std::map<std::pair<uint64_t, uint32_t>, std::vector<uint32_t>> cache;
	std::lock_guard                                             lock(mutex);
	const auto key = std::make_pair(shader_hash, pc);
	auto       it  = cache.find(key);
	if (it == cache.end()) {
		std::vector<uint32_t> records;
		if (FILE* f = std::fopen(PersistPath(shader_hash, pc).c_str(), "r")) {
			unsigned value = 0;
			while (std::fscanf(f, "%u", &value) == 1) records.push_back(value);
			std::fclose(f);
		}
		it = cache.emplace(key, std::move(records)).first;
	}
	return it->second;
}

inline void SavePersisted(uint64_t shader_hash, uint32_t pc, const std::vector<uint32_t>& records) {
	std::error_code error;
	std::filesystem::create_directories("_PipelineCache", error);
	if (FILE* f = std::fopen(PersistPath(shader_hash, pc).c_str(), "w")) {
		for (const auto index: records) std::fprintf(f, "%u\n", index);
		std::fclose(f);
	}
}

// Shared-body call sites inline one copy per called FUNCTION, not per record: the table reorders its records from
// scene to scene while the set of functions stays the same. The functions the GPU has been seen to call (their
// low address dwords) are learned here. They are NOT written to disk: the functions sit at different addresses
// in every run, so a set from an earlier run would only name unrelated memory.
struct LearnedStore {
	std::mutex                                                  mutex;
	std::map<std::pair<uint64_t, uint32_t>, std::set<uint32_t>> sets;

	std::set<uint32_t>& Get(uint64_t shader_hash, uint32_t pc) {
		const auto key = std::make_pair(shader_hash, pc);
		return sets[key];
	}
};

inline LearnedStore& GetLearnedStore() {
	static LearnedStore store;
	return store;
}

inline std::set<uint32_t> LearnedFunctions(uint64_t shader_hash, uint32_t pc) {
	auto&           store = GetLearnedStore();
	std::lock_guard lock(store.mutex);
	return store.Get(shader_hash, pc);
}

// Adds a function; true when it was not known.
inline bool AddLearnedFunction(uint64_t shader_hash, uint32_t pc, uint32_t low) {
	auto&           store = GetLearnedStore();
	std::lock_guard lock(store.mutex);
	auto&           known = store.Get(shader_hash, pc);
	return known.insert(low).second;
}

// The functions are at different addresses in every run but their code is the same, so what survives a restart is
// the hash of each function's body: a run starts out with every function a previous run saw the GPU call, found
// again in the current table by its hash. One hash per line (hex) in _PipelineCache/rtfn_<hash>_<pc>.txt.
struct HashStore {
	std::mutex                                                  mutex;
	std::map<std::pair<uint64_t, uint32_t>, std::set<uint64_t>> sets;

	static std::string Path(uint64_t shader_hash, uint32_t pc) {
		char name[96];
		std::snprintf(name, sizeof(name), "_PipelineCache/rtfn_%016llx_%08x.txt",
		              static_cast<unsigned long long>(shader_hash), pc);
		return name;
	}

	std::set<uint64_t>& Get(uint64_t shader_hash, uint32_t pc) {
		const auto key = std::make_pair(shader_hash, pc);
		auto       it  = sets.find(key);
		if (it == sets.end()) {
			std::set<uint64_t> loaded;
			if (FILE* f = std::fopen(Path(shader_hash, pc).c_str(), "r")) {
				unsigned long long value = 0;
				while (std::fscanf(f, "%llx", &value) == 1) loaded.insert(value);
				std::fclose(f);
			}
			it = sets.emplace(key, std::move(loaded)).first;
		}
		return it->second;
	}
};

inline HashStore& GetHashStore() {
	static HashStore store;
	return store;
}

inline std::set<uint64_t> PersistedFunctionHashes(uint64_t shader_hash, uint32_t pc) {
	auto&           store = GetHashStore();
	std::lock_guard lock(store.mutex);
	return store.Get(shader_hash, pc);
}

// Adds hashes; the file is rewritten when the set grew.
inline void AddPersistedFunctionHashes(uint64_t shader_hash, uint32_t pc, const std::vector<uint64_t>& hashes) {
	auto&           store = GetHashStore();
	std::lock_guard lock(store.mutex);
	auto&           known = store.Get(shader_hash, pc);
	bool            grown = false;
	for (const auto hash: hashes) grown |= known.insert(hash).second;
	if (!grown) return;
	std::error_code error;
	std::filesystem::create_directories("_PipelineCache", error);
	if (FILE* f = std::fopen(HashStore::Path(shader_hash, pc).c_str(), "w")) {
		for (const auto hash: known) std::fprintf(f, "%llx\n", static_cast<unsigned long long>(hash));
		std::fclose(f);
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::RtFeedback

#endif
