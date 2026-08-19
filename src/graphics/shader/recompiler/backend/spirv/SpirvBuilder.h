#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVBUILDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVBUILDER_H_

#include "common/common.h"

#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

struct TypeAnnotation {
	uint32_t              opcode = 0;
	std::vector<uint32_t> operands;
};

struct DeferredPhi {
	size_t word_offset    = 0;
	size_t incoming_count = 0;
};

class Builder {
public:
	explicit Builder(uint32_t version = 0x00010300u);
	~Builder() = default;
	KYTY_CLASS_DEFAULT_COPY(Builder);

	uint32_t AllocateId();
	void     RequireCapability(uint32_t capability);
	void     RequireExtension(const char* name);
	uint32_t Import(const char* name);
	uint32_t Type(uint32_t opcode, std::initializer_list<uint32_t> operands = {});
	uint32_t Type(uint32_t opcode, const std::vector<uint32_t>& operands);
	uint32_t DecoratedType(uint32_t opcode, std::initializer_list<uint32_t> operands,
	                       std::initializer_list<TypeAnnotation> annotations);
	uint32_t Constant(uint32_t opcode, uint32_t type,
	                  std::initializer_list<uint32_t> operands = {});
	uint32_t Constant(uint32_t opcode, uint32_t type, const std::vector<uint32_t>& operands);
	uint32_t DefineGlobalVariable(uint32_t pointer_type, uint32_t storage_class);
	void     DefineGlobalVariable(uint32_t id, uint32_t pointer_type, uint32_t storage_class);

	void        AddMemoryModel(std::initializer_list<uint32_t> operands);
	void        AddEntryPoint(uint32_t execution_model, uint32_t entry_point, const char* name,
	                          const std::vector<uint32_t>& interfaces);
	void        AddExecutionMode(std::initializer_list<uint32_t> operands);
	void        AddName(uint32_t target, const char* name);
	void        AddAnnotation(std::initializer_list<uint32_t> words);
	void        AddFunction(std::initializer_list<uint32_t> words);
	void        AddFunction(const std::vector<uint32_t>& words);
	DeferredPhi AddDeferredPhi(uint32_t type, uint32_t result, size_t incoming_count);
	void        PatchDeferredPhi(DeferredPhi phi, size_t incoming, uint32_t value, uint32_t parent);

	[[nodiscard]] std::vector<uint32_t> Build() const;

private:
	static void AppendInstruction(std::vector<uint32_t>& section, uint32_t opcode,
	                              const std::vector<uint32_t>& operands);
	static void AppendInstruction(std::vector<uint32_t>& section, uint32_t opcode,
	                              std::initializer_list<uint32_t> operands);
	static void AppendString(std::vector<uint32_t>& words, const char* text);
	void        AddCapability(std::initializer_list<uint32_t> operands);
	void        AddExtension(const char* name);
	void        AddExtInstImport(uint32_t id, const char* name);
	void        AddType(std::initializer_list<uint32_t> words);

	uint32_t                                  m_next_id = 1;
	uint32_t                                  m_version = 0;
	std::vector<uint32_t>                     m_capabilities;
	std::vector<uint32_t>                     m_extensions;
	std::vector<uint32_t>                     m_ext_inst_imports;
	std::vector<uint32_t>                     m_memory_model;
	std::vector<uint32_t>                     m_entry_points;
	std::vector<uint32_t>                     m_execution_modes;
	std::vector<uint32_t>                     m_debug;
	std::vector<uint32_t>                     m_annotations;
	std::vector<uint32_t>                     m_declarations;
	std::vector<uint32_t>                     m_functions;
	std::set<uint32_t>                        m_required_capabilities;
	std::set<std::string>                     m_required_extensions;
	std::map<std::string, uint32_t>           m_import_ids;
	std::map<std::vector<uint32_t>, uint32_t> m_declaration_ids;
	size_t                                    m_unpatched_phi_incomings = 0;
};

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVBUILDER_H_ */
