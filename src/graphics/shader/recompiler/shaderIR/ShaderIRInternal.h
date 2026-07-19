#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_SHADERIR_SHADERIRINTERNAL_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_SHADERIR_SHADERIRINTERNAL_H_

#include "graphics/shader/recompiler/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

void SetError(std::string* error, const char* message);

Opcode LookupIrOpcode(Decoder::Opcode opcode);
bool   IsImplemented(Decoder::Opcode opcode);
bool   IsReversedBinary(Decoder::Opcode opcode);
bool   IsVectorCarryOutOpcode(Decoder::Opcode opcode);
bool   ScalarResultWritesSccNonZero(Decoder::Opcode opcode);
bool   ScalarResultIs64Bit(Decoder::Opcode opcode);

Operand  MakeSccOperand();
Operand  MakeImmediateU32(uint32_t value);
Operand  MakePcRelativeU32(uint32_t value);
bool     AppendScalarResultSccNonZero(const Decoder::Instruction& decoded, BasicBlock* block,
                                      std::string* error);
uint32_t VectorByteConvertIndex(Decoder::Opcode opcode);
void     CopyOperandModifiers(const Decoder::Operand& decoded, Operand& operand);
bool LowerRegisterOperand(const Decoder::Operand& decoded, Operand* operand, std::string* error);
bool LowerSourceOperand(const Decoder::Operand& decoded, Operand* operand, std::string* error);
void ApplyDppDestinationMask(const Decoder::Instruction& decoded, Operand& dst);
uint32_t         ResourceIndexFromOperand(const Decoder::Operand& operand);
MemoryInfo       MemoryInfoFromDecoded(const Decoder::Instruction& decoded, ResourceKind kind);
uint32_t         RawScalarLoadBase(const Decoder::Operand& operand);
void             ClearRegisterOffsetModifiers(Decoder::Operand& operand);
bool             TryGetScalarDestinationCode(const Decoder::Operand& operand, uint32_t& code);
bool             TryOffsetScalarDestination(const Decoder::Operand& operand, uint32_t index,
                                            Decoder::Operand* result);
Decoder::Operand OffsetDecodedRegister(const Decoder::Operand& operand, uint32_t index);
MemoryInfo       OffsetMemoryInfo(const Decoder::Instruction& decoded, ResourceKind kind,
                                  uint32_t dword_index);
uint32_t         TypedBufferComponentOffsetBytes(const Decoder::Instruction& decoded,
                                                 uint32_t                    component_index);
uint32_t         TypedBufferFormatComponentCount(const Decoder::Instruction& decoded);
bool             LowerMoveImmediateU32(uint32_t pc, const Decoder::Operand& dst, uint32_t value,
                                       BasicBlock* block, std::string* error);
MemoryInfo   OffsetBufferMemoryInfo(const Decoder::Instruction& decoded, uint32_t component_index);
MemoryInfo   ByteOffsetMemoryInfo(const Decoder::Instruction& decoded, ResourceKind kind,
                                  uint32_t byte_offset);
ResourceKind FlatSegmentResourceKind(uint32_t segment);
const Decoder::Operand& DecodedSourceAt(const Decoder::Instruction& decoded, uint32_t index);
Decoder::Operand        M0Operand();

bool IsMemoryOpcode(Decoder::Opcode opcode);
bool LowerMemoryInstruction(const Decoder::Instruction& decoded, BasicBlock* block,
                            std::string* error);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_SHADERIR_SHADERIRINTERNAL_H_
