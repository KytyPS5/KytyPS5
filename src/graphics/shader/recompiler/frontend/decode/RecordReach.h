#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_FRONTEND_DECODE_RECORDREACH_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_FRONTEND_DECODE_RECORDREACH_H_

#include <cstdint>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Decoder {

struct Program;

// The memory a program reaches through device addresses when it works on a table of records and
// every such address comes from the record of its thread. GTA V skins its meshes on the GPU so
// (cs e777d90ed493bd6c, ad33aa3c0aae1311, abb185d80e19fb9a): each thread looks for the record whose
// thread range holds it,
//
//     for (r = first; r < first + count; r++)
//         if (record[r].first_thread <= thread && thread < record[r].first_thread + record[r].threads)
//
// with `first` and `count` in a header buffer, and then reads the vertex
//
//     pointers[record.pointer].base + ((attribute + stride * (thread - record.first_thread)) & ~3) + n
//
// of a vertex buffer whose V# is an entry of a table, with the stride and the attribute offsets in
// fields of the record, and stores the result through other V#s of the table that the record
// names. The host can enumerate that: the header, the records and the table are memory the
// guest's CPU writes.
//
// The V#s of the header, the records and the table are loaded from pointers in user data
// registers; fields are byte offsets into a record.
struct RecordReach {
	static constexpr uint32_t None = 0xffffffffu;

	struct Descriptor {
		// The V# is at the pointer in user data registers `user_sgpr` and + 1, + `offset`.
		uint32_t user_sgpr = None;
		uint32_t offset    = 0;

		bool operator==(const Descriptor&) const = default;
	};
	// An attribute: the reads start at ((the field's value + stride * vertex) & ~3) and end `end`
	// bytes after that. `byte`: the byte of the field's dword that holds the value (4: all of it);
	// `field` None: the value is 0.
	struct Attribute {
		uint32_t field = None;
		uint32_t byte  = 4;
		uint32_t end   = 0;
	};

	Descriptor header;
	// The first record and the record count are the two dwords at this offset of the header.
	uint32_t   header_offset = 0;
	Descriptor records;
	uint32_t   record_size   = 0;
	// Fields: the number of threads and the first thread; the entry of the table whose base the
	// reads start from; the dword whose byte `stride_byte` is the stride.
	uint32_t   threads_field = 0;
	uint32_t   first_field   = 0;
	Descriptor pointers;
	uint32_t   pointer_field = 0;
	uint32_t   stride_field  = 0;
	uint32_t   stride_byte   = 0;
	std::vector<Attribute> attributes;
	// Fields with the entries of the table (16 bytes each) the program stores through.
	std::vector<uint32_t>  store_fields;
	// The instructions it covers: the device-address reads and the stores through table entries.
	std::vector<uint32_t>  read_pcs;
	std::vector<uint32_t>  store_pcs;
	bool                   valid = false;

	// As ShaderInfo keeps it (see Unpack()).
	[[nodiscard]] std::vector<uint32_t> Pack() const;
	[[nodiscard]] static bool           Unpack(const std::vector<uint32_t>& words, RecordReach& reach);
};

// (Inline: the resource tracking tests pack a reach without the decoder.)
inline std::vector<uint32_t> RecordReach::Pack() const {
	std::vector<uint32_t> words {header.user_sgpr, header.offset,  header_offset, records.user_sgpr,
	                             records.offset,   record_size,    threads_field, first_field,
	                             pointers.user_sgpr, pointers.offset, pointer_field, stride_field,
	                             stride_byte,      static_cast<uint32_t>(attributes.size())};
	for (const auto& attribute: attributes) {
		words.insert(words.end(), {attribute.field, attribute.byte, attribute.end});
	}
	words.push_back(static_cast<uint32_t>(store_fields.size()));
	words.insert(words.end(), store_fields.begin(), store_fields.end());
	return words;
}

inline bool RecordReach::Unpack(const std::vector<uint32_t>& words, RecordReach& reach) {
	constexpr size_t Fixed = 14;
	if (words.size() < Fixed + 1u) {
		return false;
	}
	reach            = {};
	reach.header     = {words[0], words[1]};
	reach.header_offset = words[2];
	reach.records    = {words[3], words[4]};
	reach.record_size   = words[5];
	reach.threads_field = words[6];
	reach.first_field   = words[7];
	reach.pointers      = {words[8], words[9]};
	reach.pointer_field = words[10];
	reach.stride_field  = words[11];
	reach.stride_byte   = words[12];
	const size_t attributes = words[13];
	size_t       at         = Fixed;
	if (words.size() < at + attributes * 3u + 1u) {
		return false;
	}
	for (size_t i = 0; i < attributes; i++, at += 3) {
		reach.attributes.push_back({words[at], words[at + 1], words[at + 2]});
	}
	const size_t stores = words[at++];
	if (words.size() != at + stores) {
		return false;
	}
	reach.store_fields.assign(words.begin() + static_cast<ptrdiff_t>(at), words.end());
	reach.valid = true;
	return true;
}

// Recognizes the program by its code; false when any device-address read or table store of the
// program is not of that form.
bool DeriveRecordReach(const Program& program, uint32_t wave_size, RecordReach& reach);

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_FRONTEND_DECODE_RECORDREACH_H_
