#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVBUILDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVBUILDER_H_

#include "common/common.h"

#include <initializer_list>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

class Builder {
public:
	Builder();
	~Builder() = default;
	KYTY_CLASS_DEFAULT_COPY(Builder);

	uint32_t AllocateId();

	void AddCapability(std::initializer_list<uint32_t> operands);
	void AddExtension(const char* name);
	void AddExtInstImport(uint32_t id, const char* name);
	void AddMemoryModel(std::initializer_list<uint32_t> operands);
	void AddEntryPoint(uint32_t execution_model, uint32_t entry_point, const char* name,
	                   const std::vector<uint32_t>& interfaces);
	void AddExecutionMode(std::initializer_list<uint32_t> operands);
	void AddName(uint32_t target, const char* name);
	void AddAnnotation(std::initializer_list<uint32_t> words);
	void AddType(std::initializer_list<uint32_t> words);
	void AddFunction(std::initializer_list<uint32_t> words);
	void AddFunction(const std::vector<uint32_t>& words);

	[[nodiscard]] std::vector<uint32_t> Build() const;

private:
	static void AppendInstruction(std::vector<uint32_t>* section, uint32_t opcode,
	                              const std::vector<uint32_t>& operands);
	static void AppendInstruction(std::vector<uint32_t>* section, uint32_t opcode,
	                              std::initializer_list<uint32_t> operands);
	static void AppendString(std::vector<uint32_t>* words, const char* text);

	uint32_t              m_next_id = 1;
	std::vector<uint32_t> m_capabilities;
	std::vector<uint32_t> m_extensions;
	std::vector<uint32_t> m_ext_inst_imports;
	std::vector<uint32_t> m_memory_model;
	std::vector<uint32_t> m_entry_points;
	std::vector<uint32_t> m_execution_modes;
	std::vector<uint32_t> m_debug;
	std::vector<uint32_t> m_annotations;
	std::vector<uint32_t> m_types;
	std::vector<uint32_t> m_functions;
};

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVBUILDER_H_ */
