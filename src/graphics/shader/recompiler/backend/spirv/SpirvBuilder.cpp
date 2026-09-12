#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"

#include "common/assert.h"

#include <algorithm>
#include <cstring>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

static constexpr size_t InitialSpirvSectionReserve         = 4096;
static constexpr size_t InitialSpirvFunctionSectionReserve = 32768;

static void AppendInstructionWords(std::vector<uint32_t>& section, const uint32_t* words,
                                   size_t words_num) {
	if (words_num == 0) {
		return;
	}
	const auto opcode     = words[0];
	const auto word_count = static_cast<uint32_t>(words_num);
	section.push_back((word_count << spv::WordCountShift) | opcode);
	section.insert(section.end(), words + 1, words + words_num);
}

Builder::Builder(uint32_t version): m_version(version) {
	m_debug.reserve(InitialSpirvSectionReserve);
	m_annotations.reserve(InitialSpirvSectionReserve);
	m_declarations.reserve(InitialSpirvSectionReserve);
	m_functions.reserve(InitialSpirvFunctionSectionReserve);
}

uint32_t Builder::AllocateId() {
	return m_next_id++;
}

void Builder::RequireVersion(uint32_t version) {
	m_version = std::max(m_version, version);
}

void Builder::RequireCapability(spv::Capability capability) {
	if (m_required_capabilities.insert(capability).second) {
		AppendInstruction(m_capabilities, spv::OpCapability, {static_cast<uint32_t>(capability)});
	}
}

void Builder::RequireExtension(const char* name) {
	if (m_required_extensions.emplace(name).second) {
		std::vector<uint32_t> operands;
		AppendString(operands, name);
		AppendInstruction(m_extensions, spv::OpExtension, operands);
	}
}

uint32_t Builder::Import(const char* name) {
	if (const auto it = m_import_ids.find(name); it != m_import_ids.end()) {
		return it->second;
	}
	const auto id = AllocateId();
	m_import_ids.emplace(name, id);
	std::vector<uint32_t> operands = {id};
	AppendString(operands, name);
	AppendInstruction(m_ext_inst_imports, spv::OpExtInstImport, operands);
	return id;
}

uint32_t Builder::Type(spv::Op opcode, std::initializer_list<uint32_t> operands) {
	return Type(opcode, std::vector<uint32_t>(operands));
}

uint32_t Builder::Type(spv::Op opcode, const std::vector<uint32_t>& operands) {
	std::vector<uint32_t> key;
	key.reserve(operands.size() + 2u);
	key.push_back(opcode);
	key.push_back(static_cast<uint32_t>(operands.size()));
	key.insert(key.end(), operands.begin(), operands.end());
	if (const auto it = m_declaration_ids.find(key); it != m_declaration_ids.end()) {
		return it->second;
	}
	const auto id = AllocateId();
	m_declaration_ids.emplace(std::move(key), id);
	std::vector<uint32_t> words {static_cast<uint32_t>(opcode), id};
	words.insert(words.end(), operands.begin(), operands.end());
	AppendInstructionWords(m_declarations, words.data(), words.size());
	return id;
}

uint32_t Builder::DecoratedType(spv::Op opcode, std::initializer_list<uint32_t> operands,
                                std::initializer_list<TypeAnnotation> annotations) {
	if (annotations.size() == 0) {
		return Type(opcode, operands);
	}
	std::vector<uint32_t> key {static_cast<uint32_t>(opcode),
	                           static_cast<uint32_t>(operands.size())};
	key.insert(key.end(), operands.begin(), operands.end());
	key.push_back(static_cast<uint32_t>(annotations.size()));
	for (const auto& annotation: annotations) {
		key.push_back(annotation.opcode);
		key.push_back(static_cast<uint32_t>(annotation.operands.size()));
		key.insert(key.end(), annotation.operands.begin(), annotation.operands.end());
	}
	if (const auto it = m_declaration_ids.find(key); it != m_declaration_ids.end()) {
		return it->second;
	}
	const auto id = AllocateId();
	m_declaration_ids.emplace(std::move(key), id);
	std::vector<uint32_t> words {static_cast<uint32_t>(opcode), id};
	words.insert(words.end(), operands.begin(), operands.end());
	AppendInstructionWords(m_declarations, words.data(), words.size());
	for (const auto& annotation: annotations) {
		words = {static_cast<uint32_t>(annotation.opcode), id};
		words.insert(words.end(), annotation.operands.begin(), annotation.operands.end());
		AppendInstructionWords(m_annotations, words.data(), words.size());
	}
	return id;
}

uint32_t Builder::Constant(spv::Op opcode, uint32_t type,
                           std::initializer_list<uint32_t> operands) {
	return Constant(opcode, type, std::vector<uint32_t>(operands));
}

uint32_t Builder::Constant(spv::Op opcode, uint32_t type, const std::vector<uint32_t>& operands) {
	std::vector<uint32_t> key;
	key.reserve(operands.size() + 2u);
	key.push_back(opcode);
	key.push_back(type);
	key.insert(key.end(), operands.begin(), operands.end());
	if (const auto it = m_declaration_ids.find(key); it != m_declaration_ids.end()) {
		return it->second;
	}
	const auto id = AllocateId();
	m_declaration_ids.emplace(std::move(key), id);
	std::vector<uint32_t> words {static_cast<uint32_t>(opcode), type, id};
	words.insert(words.end(), operands.begin(), operands.end());
	AppendInstructionWords(m_declarations, words.data(), words.size());
	return id;
}

uint32_t Builder::DefineGlobalVariable(uint32_t pointer_type, spv::StorageClass storage_class) {
	const auto id = AllocateId();
	DefineGlobalVariable(id, pointer_type, storage_class);
	return id;
}

void Builder::DefineGlobalVariable(uint32_t id, uint32_t pointer_type,
                                   spv::StorageClass storage_class) {
	AppendInstruction(m_declarations, spv::OpVariable,
	                  {pointer_type, id, static_cast<uint32_t>(storage_class)});
}

void Builder::AppendString(std::vector<uint32_t>& words, const char* text) {
	const auto len        = text != nullptr ? std::strlen(text) : 0;
	const auto word_count = (len + 1u + 3u) / 4u;
	for (size_t i = 0; i < word_count; i++) {
		uint32_t word = 0;
		for (size_t byte = 0; byte < 4; byte++) {
			const auto index = i * 4u + byte;
			if (index < len) {
				word |= static_cast<uint32_t>(static_cast<unsigned char>(text[index]))
				        << (byte * 8u);
			}
		}
		words.push_back(word);
	}
}

void Builder::AppendInstruction(std::vector<uint32_t>& section, spv::Op opcode,
                                const std::vector<uint32_t>& operands) {
	const uint32_t word_count = static_cast<uint32_t>(operands.size() + 1u);
	section.push_back((word_count << spv::WordCountShift) | opcode);
	section.insert(section.end(), operands.begin(), operands.end());
}

void Builder::AppendInstruction(std::vector<uint32_t>& section, spv::Op opcode,
                                std::initializer_list<uint32_t> operands) {
	const uint32_t word_count = static_cast<uint32_t>(operands.size() + 1u);
	section.push_back((word_count << spv::WordCountShift) | opcode);
	section.insert(section.end(), operands.begin(), operands.end());
}

void Builder::AddMemoryModel(std::initializer_list<uint32_t> operands) {
	AppendInstruction(m_memory_model, spv::OpMemoryModel, operands);
}

void Builder::AddEntryPoint(spv::ExecutionModel execution_model, uint32_t entry_point,
                            const char* name, const std::vector<uint32_t>& interfaces) {
	std::vector<uint32_t> operands = {static_cast<uint32_t>(execution_model), entry_point};
	AppendString(operands, name);
	operands.insert(operands.end(), interfaces.begin(), interfaces.end());
	if (m_version >= 0x00010400u) {
		for (size_t offset = 0; offset < m_declarations.size();) {
			const auto count = m_declarations[offset] >> spv::WordCountShift;
			if ((m_declarations[offset] & spv::OpCodeMask) == spv::OpVariable) {
				const auto id = m_declarations[offset + 2u];
				if (std::find(interfaces.begin(), interfaces.end(), id) == interfaces.end()) {
					operands.push_back(id);
				}
			}
			offset += count;
		}
	}
	AppendInstruction(m_entry_points, spv::OpEntryPoint, operands);
}

void Builder::AddExecutionMode(std::initializer_list<uint32_t> operands) {
	AppendInstruction(m_execution_modes, spv::OpExecutionMode, operands);
}

void Builder::AddName(uint32_t target, const char* name) {
	std::vector<uint32_t> operands = {target};
	AppendString(operands, name);
	AppendInstruction(m_debug, spv::OpName, operands);
}

void Builder::AddAnnotation(std::initializer_list<uint32_t> words) {
	AppendInstructionWords(m_annotations, words.begin(), words.size());
}

void Builder::AddFunction(std::initializer_list<uint32_t> words) {
	AppendInstructionWords(m_functions, words.begin(), words.size());
}

void Builder::AddFunction(const std::vector<uint32_t>& words) {
	AppendInstructionWords(m_functions, words.data(), words.size());
}

DeferredPhi Builder::AddDeferredPhi(uint32_t type, uint32_t result, size_t incoming_count) {
	std::vector<uint32_t> words {spv::OpPhi, type, result};
	words.resize(words.size() + incoming_count * 2u);
	const DeferredPhi phi {m_functions.size()};
	AddFunction(words);
	m_unpatched_phi_incomings += incoming_count;
	return phi;
}

void Builder::PatchDeferredPhi(DeferredPhi phi, size_t incoming, uint32_t value, uint32_t parent) {
	const auto incoming_count =
	    ((m_functions.at(phi.word_offset) >> spv::WordCountShift) - 3u) / 2u;
	EXIT_IF(incoming >= incoming_count || value == 0 || parent == 0);
	const auto value_word  = phi.word_offset + 3u + incoming * 2u;
	const auto parent_word = value_word + 1u;
	EXIT_IF(m_functions.at(value_word) != 0 || m_functions.at(parent_word) != 0);
	m_functions[value_word]  = value;
	m_functions[parent_word] = parent;
	m_unpatched_phi_incomings--;
}

std::vector<uint32_t> Builder::Build() const {
	EXIT_IF(m_unpatched_phi_incomings != 0);

	std::vector<uint32_t> module;
	module.reserve(5u + m_capabilities.size() + m_extensions.size() + m_ext_inst_imports.size() +
	               m_memory_model.size() + m_entry_points.size() + m_execution_modes.size() +
	               m_debug.size() + m_annotations.size() + m_declarations.size() +
	               m_functions.size());

	module.push_back(spv::MagicNumber);
	module.push_back(m_version);
	module.push_back(0u);
	module.push_back(m_next_id);
	module.push_back(0u);

	module.insert(module.end(), m_capabilities.begin(), m_capabilities.end());
	module.insert(module.end(), m_extensions.begin(), m_extensions.end());
	module.insert(module.end(), m_ext_inst_imports.begin(), m_ext_inst_imports.end());
	module.insert(module.end(), m_memory_model.begin(), m_memory_model.end());
	module.insert(module.end(), m_entry_points.begin(), m_entry_points.end());
	module.insert(module.end(), m_execution_modes.begin(), m_execution_modes.end());
	module.insert(module.end(), m_debug.begin(), m_debug.end());
	module.insert(module.end(), m_annotations.begin(), m_annotations.end());
	module.insert(module.end(), m_declarations.begin(), m_declarations.end());
	module.insert(module.end(), m_functions.begin(), m_functions.end());

	return module;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
