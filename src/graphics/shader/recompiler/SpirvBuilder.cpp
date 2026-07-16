#include "graphics/shader/recompiler/SpirvBuilder.h"

#include <cstdio>
#include <cstring>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

static constexpr size_t InitialSpirvSectionReserve         = 4096;
static constexpr size_t InitialSpirvFunctionSectionReserve = 450000;

static void AppendInstructionWords(std::vector<uint32_t>* section, const uint32_t* words,
                                   size_t words_num) {
	if (words_num == 0) {
		return;
	}
	const auto opcode     = words[0];
	const auto word_count = static_cast<uint32_t>(words_num);
	section->push_back((word_count << 16u) | opcode);
	section->insert(section->end(), words + 1, words + words_num);
}

Builder::Builder() {
	m_debug.reserve(InitialSpirvSectionReserve);
	m_annotations.reserve(InitialSpirvSectionReserve);
	m_types.reserve(InitialSpirvSectionReserve);
	m_functions.reserve(InitialSpirvFunctionSectionReserve);
}

uint32_t Builder::AllocateId() {
	return m_next_id++;
}

void Builder::AppendString(std::vector<uint32_t>* words, const char* text) {
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
		words->push_back(word);
	}
}

void Builder::AppendInstruction(std::vector<uint32_t>* section, uint32_t opcode,
                                const std::vector<uint32_t>& operands) {
	const uint32_t word_count = static_cast<uint32_t>(operands.size() + 1u);
	section->push_back((word_count << 16u) | opcode);
	section->insert(section->end(), operands.begin(), operands.end());
}

void Builder::AppendInstruction(std::vector<uint32_t>* section, uint32_t opcode,
                                std::initializer_list<uint32_t> operands) {
	const uint32_t word_count = static_cast<uint32_t>(operands.size() + 1u);
	section->push_back((word_count << 16u) | opcode);
	section->insert(section->end(), operands.begin(), operands.end());
}

void Builder::AddCapability(std::initializer_list<uint32_t> operands) {
	AppendInstruction(&m_capabilities, 17u, operands);
}

void Builder::AddExtension(const char* name) {
	std::vector<uint32_t> operands;
	AppendString(&operands, name);
	AppendInstruction(&m_extensions, 10u, operands);
}

void Builder::AddExtInstImport(uint32_t id, const char* name) {
	std::vector<uint32_t> operands = {id};
	AppendString(&operands, name);
	AppendInstruction(&m_ext_inst_imports, 11u, operands);
}

void Builder::AddMemoryModel(std::initializer_list<uint32_t> operands) {
	AppendInstruction(&m_memory_model, 14u, operands);
}

void Builder::AddEntryPoint(uint32_t execution_model, uint32_t entry_point, const char* name,
                            const std::vector<uint32_t>& interfaces) {
	std::vector<uint32_t> operands = {execution_model, entry_point};
	AppendString(&operands, name);
	operands.insert(operands.end(), interfaces.begin(), interfaces.end());
	AppendInstruction(&m_entry_points, 15u, operands);
}

void Builder::AddExecutionMode(std::initializer_list<uint32_t> operands) {
	AppendInstruction(&m_execution_modes, 16u, operands);
}

void Builder::AddName(uint32_t target, const char* name) {
	std::vector<uint32_t> operands = {target};
	AppendString(&operands, name);
	AppendInstruction(&m_debug, 5u, operands);
}

void Builder::AddAnnotation(std::initializer_list<uint32_t> words) {
	AppendInstructionWords(&m_annotations, words.begin(), words.size());
}

void Builder::AddType(std::initializer_list<uint32_t> words) {
	AppendInstructionWords(&m_types, words.begin(), words.size());
}

void Builder::AddFunction(std::initializer_list<uint32_t> words) {
	AppendInstructionWords(&m_functions, words.begin(), words.size());
}

void Builder::AddFunction(const std::vector<uint32_t>& words) {
	AppendInstructionWords(&m_functions, words.data(), words.size());
}

std::vector<uint32_t> Builder::Build() const {
	if (m_debug.size() > InitialSpirvSectionReserve) {
		std::printf("SPIR-V builder reserve exceeded: section=debug size=%zu reserve=%zu\n",
		            m_debug.size(), InitialSpirvSectionReserve);
	}
	if (m_annotations.size() > InitialSpirvSectionReserve) {
		std::printf("SPIR-V builder reserve exceeded: section=annotations size=%zu reserve=%zu\n",
		            m_annotations.size(), InitialSpirvSectionReserve);
	}
	if (m_types.size() > InitialSpirvSectionReserve) {
		std::printf("SPIR-V builder reserve exceeded: section=types size=%zu reserve=%zu\n",
		            m_types.size(), InitialSpirvSectionReserve);
	}
	if (m_functions.size() > InitialSpirvFunctionSectionReserve) {
		std::printf("SPIR-V builder reserve exceeded: section=functions size=%zu reserve=%zu\n",
		            m_functions.size(), InitialSpirvFunctionSectionReserve);
	}

	std::vector<uint32_t> module;
	module.reserve(5u + m_capabilities.size() + m_extensions.size() + m_ext_inst_imports.size() +
	               m_memory_model.size() + m_entry_points.size() + m_execution_modes.size() +
	               m_debug.size() + m_annotations.size() + m_types.size() + m_functions.size());

	module.push_back(0x07230203u);
	module.push_back(0x00010300u);
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
	module.insert(module.end(), m_types.begin(), m_types.end());
	module.insert(module.end(), m_functions.begin(), m_functions.end());

	return module;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
