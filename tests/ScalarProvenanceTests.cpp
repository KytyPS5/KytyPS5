#include "graphics/shader/recompiler/ScalarProvenance.h"
#include "graphics/shader/recompiler/SrtWalker.h"

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {

using namespace Libs::Graphics::ShaderRecompiler::IR;
using Libs::Graphics::ShaderType;

void Check(bool condition, const char* message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

Operand Sgpr(uint32_t reg) {
	Operand operand;
	operand.kind      = OperandKind::Register;
	operand.reg.file  = RegisterFile::Scalar;
	operand.reg.index = reg;
	return operand;
}

Operand Vgpr(uint32_t reg) {
	Operand operand;
	operand.kind      = OperandKind::Register;
	operand.reg.file  = RegisterFile::Vector;
	operand.reg.index = reg;
	return operand;
}

Operand Imm(uint32_t value) {
	Operand operand;
	operand.kind = OperandKind::ImmediateU32;
	operand.imm  = value;
	return operand;
}

Operand M0() {
	Operand operand;
	operand.kind     = OperandKind::Register;
	operand.reg.file = RegisterFile::M0;
	return operand;
}

Instruction Move(uint32_t pc, uint32_t dst, uint32_t src) {
	Instruction inst;
	inst.pc        = pc;
	inst.op        = Opcode::MoveU32;
	inst.dst       = Sgpr(dst);
	inst.src[0]    = Sgpr(src);
	inst.src_count = 1;
	return inst;
}

Instruction MoveImmediate(uint32_t pc, uint32_t dst, uint32_t value) {
	auto inst   = Move(pc, dst, 0);
	inst.src[0] = Imm(value);
	return inst;
}

Instruction Move64(uint32_t pc, uint32_t dst, uint32_t src) {
	auto inst = Move(pc, dst, src);
	inst.op   = Opcode::MoveU64;
	return inst;
}

Instruction WriteLane(uint32_t pc, uint32_t dst, uint32_t src, uint32_t lane) {
	Instruction inst;
	inst.pc        = pc;
	inst.op        = Opcode::WriteLaneU32;
	inst.dst       = Vgpr(dst);
	inst.src[0]    = Sgpr(src);
	inst.src[1]    = Imm(lane);
	inst.src_count = 2;
	return inst;
}

Instruction ReadLane(uint32_t pc, uint32_t dst, uint32_t src, uint32_t lane) {
	Instruction inst;
	inst.pc        = pc;
	inst.op        = Opcode::ReadLaneU32;
	inst.dst       = Sgpr(dst);
	inst.src[0]    = Vgpr(src);
	inst.src[1]    = Imm(lane);
	inst.src_count = 2;
	return inst;
}

Instruction BufferUse(uint32_t pc, uint32_t base_sgpr) {
	Instruction inst;
	inst.pc              = pc;
	inst.op              = Opcode::BufferLoadDword;
	inst.memory.kind     = ResourceKind::Buffer;
	inst.memory.resource = base_sgpr / 4;
	return inst;
}

const ScalarValue& Value(const Program& program, uint32_t id) {
	Check(id < program.provenance.values.size(), "invalid scalar value ID");
	return program.provenance.values[id];
}

bool ReadHostMemory(void*, uint64_t address, uint32_t* value) {
	if (address == 0 || value == nullptr) {
		return false;
	}
	std::memcpy(value, reinterpret_cast<const void*>(address), sizeof(*value));
	return true;
}

struct ReadCounter {
	uint32_t reads = 0;
};

bool CountedHostMemory(void* userdata, uint64_t address, uint32_t* value) {
	auto* counter = static_cast<ReadCounter*>(userdata);
	if (counter != nullptr) {
		counter->reads++;
	}
	return ReadHostMemory(nullptr, address, value);
}

void SetPointer(std::vector<uint32_t>* user_data, uint32_t reg, const void* pointer) {
	const auto address     = reinterpret_cast<uint64_t>(pointer);
	(*user_data)[reg]      = static_cast<uint32_t>(address);
	(*user_data)[reg + 1u] = static_cast<uint32_t>(address >> 32u);
}

void TestPerUseDescriptorDefinitions() {
	Program program;
	program.blocks.resize(1);
	auto& instructions = program.blocks[0].instructions;
	instructions.push_back(BufferUse(0, 0));
	for (uint32_t i = 0; i < 4; i++) {
		instructions.push_back(Move(4 + i * 4, i, 20 + i));
	}
	instructions.push_back(BufferUse(20, 0));

	std::string error;
	Check(BuildScalarProvenance(&program, &error), error.c_str());
	const auto* before = GetDescriptorSource(program, instructions[0].memory.resource_source);
	const auto* after  = GetDescriptorSource(program, instructions[5].memory.resource_source);
	Check(before != nullptr && after != nullptr, "descriptor sources were not attached");
	Check(*before != *after, "descriptor source was incorrectly shared across SGPR redefinition");
	for (uint32_t i = 0; i < 4; i++) {
		Check(Value(program, before->dwords[i]).op == ScalarValueOp::UserData &&
		          Value(program, before->dwords[i]).imm == i,
		      "pre-copy descriptor does not use the reaching user-data definition");
		Check(Value(program, after->dwords[i]).op == ScalarValueOp::UserData &&
		          Value(program, after->dwords[i]).imm == 20 + i,
		      "post-copy descriptor does not use the copied user-data definition");
	}
}

void TestCfgPhi() {
	Program program;
	program.blocks.resize(4);
	program.blocks[0].successors   = {1, 2};
	program.blocks[1].predecessors = {0};
	program.blocks[1].successors   = {3};
	program.blocks[2].predecessors = {0};
	program.blocks[2].successors   = {3};
	program.blocks[3].predecessors = {1, 2};
	program.blocks[1].instructions.push_back(MoveImmediate(4, 0, 0x11111111));
	program.blocks[2].instructions.push_back(MoveImmediate(8, 0, 0x22222222));
	program.blocks[3].instructions.push_back(BufferUse(12, 0));

	std::string error;
	Check(BuildScalarProvenance(&program, &error), error.c_str());
	const auto* source =
	    GetDescriptorSource(program, program.blocks[3].instructions[0].memory.resource_source);
	Check(source != nullptr, "merged descriptor source was not attached");
	const auto& phi = Value(program, source->dwords[0]);
	Check(phi.op == ScalarValueOp::Phi && phi.phi_args.size() == 2,
	      "differing CFG definitions did not create a provenance phi");
	Check(Value(program, phi.phi_args[0]).op == ScalarValueOp::Constant &&
	          Value(program, phi.phi_args[1]).op == ScalarValueOp::Constant,
	      "provenance phi does not reference both constant definitions");
	Check(BuildSrtPlan(&program, &error) && program.srt.dynamic_sources.size() == 1 &&
	          program.srt.dynamic_sources[0] ==
	              program.blocks[3].instructions[0].memory.resource_source,
	      "acyclic control-flow descriptor phi was not classified dynamic");
}

void TestDiamondReadPathsAreDynamic() {
	std::array<uint32_t, 1> left  = {0x11111111u};
	std::array<uint32_t, 1> right = {0x22222222u};
	Program                 program;
	program.blocks.resize(4);
	program.blocks[0].successors   = {1, 2};
	program.blocks[1].predecessors = {0};
	program.blocks[1].successors   = {3};
	program.blocks[2].predecessors = {0};
	program.blocks[2].successors   = {3};
	program.blocks[3].predecessors = {1, 2};
	const auto load                = [](uint32_t pc, uint32_t base) {
		Instruction inst;
		inst.pc              = pc;
		inst.op              = Opcode::SLoadDword;
		inst.dst             = Sgpr(0);
		inst.src[0]          = Imm(0);
		inst.src_count       = 1;
		inst.memory.kind     = ResourceKind::ScalarBuffer;
		inst.memory.resource = base;
		return inst;
	};
	program.blocks[1].instructions = {load(4, 4)};
	program.blocks[2].instructions = {load(8, 6)};
	program.blocks[3].instructions = {BufferUse(12, 0)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	const auto source = program.blocks[3].instructions[0].memory.resource_source;
	Check(program.srt.reads.empty() && program.srt.dynamic_reads.empty() &&
	          program.srt.dynamic_sources.size() == 1 && program.srt.dynamic_sources[0] == source,
	      "inactive diamond read paths were incorrectly flattened");
	std::array<uint32_t, 8> user_data     = {};
	const auto              left_address  = reinterpret_cast<uint64_t>(left.data());
	const auto              right_address = reinterpret_cast<uint64_t>(right.data());
	user_data[4]                          = static_cast<uint32_t>(left_address);
	user_data[5]                          = static_cast<uint32_t>(left_address >> 32u);
	user_data[6]                          = static_cast<uint32_t>(right_address);
	user_data[7]                          = static_cast<uint32_t>(right_address >> 32u);
	ReadCounter      counter;
	DescriptorValue  descriptor;
	const SrtRuntime runtime {user_data, 0, CountedHostMemory, &counter};
	Check(!EvaluateDescriptorSource(program, source, 12, runtime, &descriptor, &error) &&
	          counter.reads == 0 && error.find("GPU-dynamic") != std::string::npos,
	      "dynamic descriptor evaluation touched an inactive read path");
}

void TestEquivalentConstantPhiIsStatic() {
	Program program;
	program.blocks.resize(4);
	program.blocks[0].successors   = {1, 2};
	program.blocks[1].predecessors = {0};
	program.blocks[1].successors   = {3};
	program.blocks[2].predecessors = {0};
	program.blocks[2].successors   = {3};
	program.blocks[3].predecessors = {1, 2};
	Instruction add;
	add.op                         = Opcode::IAddU32;
	add.dst                        = Sgpr(0);
	add.src[0]                     = Imm(1);
	add.src[1]                     = Imm(1);
	add.src_count                  = 2;
	program.blocks[1].instructions = {add};
	program.blocks[2].instructions = {MoveImmediate(8, 0, 2)};
	program.blocks[3].instructions = {BufferUse(12, 0)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	const auto source = program.blocks[3].instructions[0].memory.resource_source;
	Check(program.srt.dynamic_sources.empty(),
	      "equivalent constant phi was unnecessarily classified dynamic");
	const std::array<uint32_t, 4> user_data = {};
	DescriptorValue               descriptor;
	const SrtRuntime              runtime {user_data, 0, nullptr, nullptr};
	Check(EvaluateDescriptorSource(program, source, 12, runtime, &descriptor, &error) &&
	          descriptor.dwords[0] == 2,
	      "equivalent constant phi did not evaluate as one static value");
}

void TestWideMoveInvalidatesAndCopiesBothDwords() {
	Program program;
	program.blocks.resize(1);
	auto& instructions = program.blocks[0].instructions;
	instructions.push_back(Move64(0, 0, 20));
	instructions.push_back(Move64(4, 2, 22));
	instructions.push_back(BufferUse(8, 0));

	std::string error;
	Check(BuildScalarProvenance(&program, &error), error.c_str());
	const auto* source = GetDescriptorSource(program, instructions.back().memory.resource_source);
	Check(source != nullptr &&
	          DescriptorSourceResolved(program, instructions.back().memory.resource_source),
	      "wide descriptor copy did not produce a resolved source");
	for (uint32_t i = 0; i < 4; i++) {
		Check(Value(program, source->dwords[i]).op == ScalarValueOp::UserData &&
		          Value(program, source->dwords[i]).imm == 20 + i,
		      "wide move did not copy both scalar dwords");
	}
}

void TestScalarCarryChain() {
	Program program;
	program.blocks.resize(1);
	Instruction low;
	low.pc                         = 0;
	low.op                         = Opcode::ScalarAddCarryU32;
	low.dst                        = Sgpr(4);
	low.src[0]                     = Sgpr(0);
	low.src[1]                     = Imm(16);
	low.src[2]                     = Imm(0);
	low.src_count                  = 3;
	Instruction high               = low;
	high.pc                        = 4;
	high.dst                       = Sgpr(5);
	high.src[0]                    = Sgpr(1);
	high.src[1]                    = Imm(0);
	high.src[2].kind               = OperandKind::Register;
	high.src[2].reg.file           = RegisterFile::Scc;
	high.src[2].reg.index          = 0;
	program.blocks[0].instructions = {low, high, BufferUse(8, 4)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error), error.c_str());
	const auto* source =
	    GetDescriptorSource(program, program.blocks[0].instructions.back().memory.resource_source);
	Check(source != nullptr, "carry-chain descriptor source was not attached");
	const auto& low_value  = Value(program, source->dwords[0]);
	const auto& high_value = Value(program, source->dwords[1]);
	Check(low_value.op == ScalarValueOp::AddCarry && high_value.op == ScalarValueOp::AddCarry,
	      "scalar add/addc pair was not retained");
	Check(Value(program, high_value.args[2]).op == ScalarValueOp::Carry,
	      "high pointer dword does not consume the low add carry");
}

void TestReadConstDefinition() {
	Program program;
	program.blocks.resize(1);
	Instruction load;
	load.pc              = 4;
	load.op              = Opcode::SLoadDword;
	load.dst             = Sgpr(16);
	load.src[0]          = Imm(8);
	load.src_count       = 1;
	load.memory.kind     = ResourceKind::ScalarBuffer;
	load.memory.resource = 4;
	load.memory.offset   = 16;
	program.blocks[0].instructions.push_back(load);
	program.blocks[0].instructions.push_back(BufferUse(8, 16));

	std::string error;
	Check(BuildScalarProvenance(&program, &error), error.c_str());
	const auto* source =
	    GetDescriptorSource(program, program.blocks[0].instructions[1].memory.resource_source);
	Check(source != nullptr, "read-constant descriptor source was not attached");
	const auto& read = Value(program, source->dwords[0]);
	Check(read.op == ScalarValueOp::ReadConst && read.imm == 16,
	      "raw scalar load was not represented as ReadConst");
	Check(Value(program, read.args[0]).op == ScalarValueOp::UserData &&
	          Value(program, read.args[0]).imm == 4 && Value(program, read.args[1]).imm == 5,
	      "ReadConst pointer does not retain its user-data roots");
	Check(Value(program, read.args[2]).op == ScalarValueOp::Constant &&
	          Value(program, read.args[2]).imm == 8,
	      "ReadConst dynamic/immediate offset operand was lost");
}

void TestReadConstBufferAndValueNumbering() {
	Program program;
	program.blocks.resize(1);
	Instruction load;
	load.op              = Opcode::SBufferLoadDword;
	load.dst             = Sgpr(16);
	load.src[0]          = Imm(3);
	load.src_count       = 1;
	load.memory.kind     = ResourceKind::ScalarBuffer;
	load.memory.resource = 1;
	load.memory.offset   = 12;
	program.blocks[0].instructions.push_back(load);
	load.pc  = 4;
	load.dst = Sgpr(17);
	program.blocks[0].instructions.push_back(load);
	program.blocks[0].instructions.push_back(BufferUse(8, 16));

	std::string error;
	Check(BuildScalarProvenance(&program, &error), error.c_str());
	const auto* source =
	    GetDescriptorSource(program, program.blocks[0].instructions.back().memory.resource_source);
	Check(source != nullptr, "ReadConstBuffer source was not attached");
	Check(source->dwords[0] == source->dwords[1],
	      "identical read-constant values at different PCs were not "
	      "value-numbered");
	const auto& read = Value(program, source->dwords[0]);
	Check(read.op == ScalarValueOp::ReadConstBuffer && read.args[3] != 0 && read.args[4] != 0,
	      "ReadConstBuffer lost descriptor dword 3 or its offset");
}

void TestReadLaneDescriptorSpill() {
	std::array<uint32_t, 80> table {};
	table[72] = 0x89abcdefu;

	Program program;
	program.wave_size       = 64;
	program.user_data_count = 28;
	program.blocks.resize(1);
	for (uint32_t component = 0; component < 4; component++) {
		Instruction load;
		load.pc                     = 0x14;
		load.op                     = Opcode::SBufferLoadDword;
		load.dst                    = Sgpr(84 + component);
		load.src[0]                 = Imm(0);
		load.src_count              = 1;
		load.memory.kind            = ResourceKind::ScalarBuffer;
		load.memory.resource        = 6;
		load.memory.offset          = 288 + component * 4;
		load.memory.component_index = component;
		load.memory.component_count = 4;
		program.blocks[0].instructions.push_back(load);
	}
	program.blocks[0].instructions.push_back(WriteLane(0x20, 11, 86, 2));
	program.blocks[0].instructions.push_back(WriteLane(0x44, 11, 85, 1));
	program.blocks[0].instructions.push_back(WriteLane(0x60, 11, 84, 0));
	program.blocks[0].instructions.push_back(MoveImmediate(0xb0, 84, 0));
	program.blocks[0].instructions.push_back(ReadLane(0x510, 0, 11, 0));
	Instruction sample;
	sample.pc                     = 0x868;
	sample.op                     = Opcode::ImageSample;
	sample.memory.kind            = ResourceKind::Image;
	sample.memory.resource        = 0;
	sample.memory.sampler         = 4;
	sample.memory.image_dimension =
	    Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D;
	program.blocks[0].instructions.push_back(sample);

	std::string error;
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	const auto source_id = program.blocks[0].instructions.back().memory.resource_source;
	const auto* source   = GetDescriptorSource(program, source_id);
	Check(source != nullptr && DescriptorSourceResolved(program, source_id),
	      "readlane descriptor spill was not resolved");
	Check(Value(program, source->dwords[0]).op == ScalarValueOp::ReadConstBuffer,
	      "readlane did not recover the pre-overwrite SRT value");
	std::vector<uint32_t> user_data(28);
	SetPointer(&user_data, 24, table.data());
	user_data[26] = sizeof(table);
	DescriptorValue descriptor;
	const SrtRuntime runtime {user_data, 0, ReadHostMemory, nullptr};
	Check(EvaluateDescriptorSource(program, source_id, sample.pc, runtime, &descriptor, &error),
	      error.c_str());
	Check(descriptor.dwords[0] == table[72],
	      "readlane descriptor spill evaluated the overwritten scalar value");
}

void TestReadLaneVectorOverwriteInvalidatesSpill() {
	Program program;
	program.wave_size       = 64;
	program.user_data_count = 8;
	program.blocks.resize(1);
	Instruction overwrite;
	overwrite.pc        = 4;
	overwrite.op        = Opcode::MoveU32;
	overwrite.dst       = Vgpr(11);
	overwrite.src[0]    = Imm(0);
	overwrite.src_count = 1;
	program.blocks[0].instructions = {WriteLane(0, 11, 4, 0), overwrite,
	                                  ReadLane(8, 0, 11, 0), BufferUse(12, 0)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error), error.c_str());
	const auto source = program.blocks[0].instructions.back().memory.resource_source;
	Check(GetDescriptorSource(program, source) != nullptr &&
	          !DescriptorSourceResolved(program, source),
	      "vector overwrite left stale writelane provenance");
}

void TestReadLanePhi() {
	Program program;
	program.wave_size       = 32;
	program.user_data_count = 8;
	program.blocks.resize(4);
	program.blocks[0].successors   = {1, 2};
	program.blocks[1].predecessors = {0};
	program.blocks[1].successors   = {3};
	program.blocks[2].predecessors = {0};
	program.blocks[2].successors   = {3};
	program.blocks[3].predecessors = {1, 2};
	program.blocks[1].instructions = {WriteLane(0, 11, 4, 0)};
	program.blocks[2].instructions = {WriteLane(4, 11, 5, 0)};
	program.blocks[3].instructions = {ReadLane(8, 0, 11, 0), BufferUse(12, 0)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error), error.c_str());
	const auto* source =
	    GetDescriptorSource(program, program.blocks[3].instructions.back().memory.resource_source);
	Check(source != nullptr && Value(program, source->dwords[0]).op == ScalarValueOp::Phi &&
	          DescriptorSourceResolved(
	              program, program.blocks[3].instructions.back().memory.resource_source),
	      "readlane definitions were not merged across CFG predecessors");
}

void TestReadLaneLoopConvergence() {
	Program program;
	program.wave_size       = 64;
	program.user_data_count = 8;
	program.blocks.resize(3);
	program.blocks[0].successors   = {1};
	program.blocks[1].predecessors = {0, 2};
	program.blocks[1].successors   = {2};
	program.blocks[2].predecessors = {1};
	program.blocks[2].successors   = {1};
	program.blocks[0].instructions = {WriteLane(0, 11, 4, 0)};
	program.blocks[1].instructions = {ReadLane(4, 0, 11, 0), BufferUse(8, 0)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error), error.c_str());
	const auto source = program.blocks[1].instructions.back().memory.resource_source;
	Check(DescriptorSourceResolved(program, source),
	      "unchanged writelane spill did not converge across a loop backedge");
}

void TestReadLaneWideAndRelativeWritesInvalidateSpill() {
	const auto check_invalidated = [](Opcode op, const char* message) {
		Program program;
		program.wave_size       = 64;
		program.user_data_count = 8;
		program.blocks.resize(1);
		Instruction overwrite;
		overwrite.pc        = 4;
		overwrite.op        = op;
		overwrite.dst       = Vgpr(11);
		overwrite.src[0]    = Imm(0);
		overwrite.src_count = 1;
		program.blocks[0].instructions = {WriteLane(0, 12, 4, 0), overwrite,
		                                  ReadLane(8, 0, 12, 0), BufferUse(12, 0)};

		std::string error;
		Check(BuildScalarProvenance(&program, &error), error.c_str());
		const auto source = program.blocks[0].instructions.back().memory.resource_source;
		Check(!DescriptorSourceResolved(program, source), message);
	};

	check_invalidated(Opcode::UMadU64U32,
	                  "64-bit vector destination left stale high-half lane provenance");
	check_invalidated(Opcode::MoveRelDestU32,
	                  "relative vector destination left stale lane provenance");
}

void TestReadLaneModuloAndDynamicLane() {
	Program modulo;
	modulo.wave_size       = 32;
	modulo.user_data_count = 8;
	modulo.blocks.resize(1);
	modulo.blocks[0].instructions = {WriteLane(0, 11, 4, 33), ReadLane(4, 0, 11, 65),
	                                 BufferUse(8, 0)};
	std::string error;
	Check(BuildScalarProvenance(&modulo, &error), error.c_str());
	const auto* modulo_source =
	    GetDescriptorSource(modulo, modulo.blocks[0].instructions.back().memory.resource_source);
	Check(modulo_source != nullptr &&
	          Value(modulo, modulo_source->dwords[0]).op == ScalarValueOp::UserData &&
	          Value(modulo, modulo_source->dwords[0]).imm == 4,
	      "readlane/writelane did not wrap the lane index to wave size");

	Program dynamic;
	dynamic.wave_size       = 64;
	dynamic.user_data_count = 8;
	dynamic.blocks.resize(1);
	auto write    = WriteLane(0, 11, 4, 0);
	write.src[1]  = Sgpr(7);
	dynamic.blocks[0].instructions = {write, ReadLane(4, 0, 11, 0), BufferUse(8, 0)};
	Check(BuildScalarProvenance(&dynamic, &error), error.c_str());
	Check(!DescriptorSourceResolved(
	          dynamic, dynamic.blocks[0].instructions.back().memory.resource_source),
	      "dynamic writelane selector retained unsafe lane provenance");
}

void TestUnresolvedSourceIsMarked() {
	Program program;
	program.blocks.resize(1);
	Instruction unknown;
	unknown.op                     = Opcode::BitwiseNotU64;
	unknown.dst                    = Sgpr(0);
	program.blocks[0].instructions = {unknown, BufferUse(4, 0)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error), error.c_str());
	const auto source = program.blocks[0].instructions.back().memory.resource_source;
	Check(GetDescriptorSource(program, source) != nullptr &&
	          !DescriptorSourceResolved(program, source),
	      "unknown descriptor tuple was incorrectly marked resolved");
}

void TestUnsupportedWideWriteInvalidatesBothDwords() {
	Program program;
	program.blocks.resize(1);
	Instruction unsupported;
	unsupported.op                 = Opcode::BitwiseNotU64;
	unsupported.dst                = Sgpr(0);
	unsupported.src[0]             = Sgpr(20);
	unsupported.src_count          = 1;
	program.blocks[0].instructions = {Move(0, 0, 20), Move(4, 1, 21), unsupported,
	                                  BufferUse(12, 0)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error), error.c_str());
	const auto* source =
	    GetDescriptorSource(program, program.blocks[0].instructions.back().memory.resource_source);
	Check(source != nullptr && source->dwords[0] == ScalarProvenance::Unknown &&
	          source->dwords[1] == ScalarProvenance::Unknown,
	      "unsupported wide write left a stale descriptor half");
}

void TestCyclicPhiIsUnresolved() {
	Program program;
	program.blocks.resize(2);
	program.blocks[0].successors   = {1};
	program.blocks[1].predecessors = {0, 1};
	program.blocks[1].successors   = {1};
	Instruction increment;
	increment.op                   = Opcode::IAddU32;
	increment.dst                  = Sgpr(0);
	increment.src[0]               = Sgpr(0);
	increment.src[1]               = Imm(1);
	increment.src_count            = 2;
	program.blocks[1].instructions = {BufferUse(4, 0), increment};

	std::string error;
	Check(BuildScalarProvenance(&program, &error), error.c_str());
	const auto source = program.blocks[1].instructions[0].memory.resource_source;
	Check(GetDescriptorSource(program, source) != nullptr &&
	          !DescriptorSourceResolved(program, source),
	      "loop-carried cyclic descriptor provenance was incorrectly resolved");
	Check(BuildSrtPlan(&program, &error) && program.srt.dynamic_sources.size() == 1 &&
	          program.srt.dynamic_sources[0] == source,
	      "SRT planning did not isolate a cyclic descriptor as a dynamic source");
}

void TestNestedSrtWalk() {
	std::array<uint32_t, 4> child         = {0x11111111u, 0x22334455u, 0, 0};
	std::array<uint32_t, 4> root          = {};
	const auto              child_address = reinterpret_cast<uint64_t>(child.data());
	root[0]                               = static_cast<uint32_t>(child_address);
	root[1]                               = static_cast<uint32_t>(child_address >> 32u);

	Program program;
	program.blocks.resize(1);
	for (uint32_t i = 0; i < 2; i++) {
		Instruction load;
		load.pc              = i * 4;
		load.op              = Opcode::SLoadDword;
		load.dst             = Sgpr(8 + i);
		load.src[0]          = Imm(0);
		load.src_count       = 1;
		load.memory.kind     = ResourceKind::ScalarBuffer;
		load.memory.resource = 4;
		load.memory.offset   = i * 4;
		program.blocks[0].instructions.push_back(load);
	}
	Instruction child_load;
	child_load.pc              = 8;
	child_load.op              = Opcode::SLoadDword;
	child_load.dst             = Sgpr(16);
	child_load.src[0]          = Imm(0);
	child_load.src_count       = 1;
	child_load.memory.kind     = ResourceKind::ScalarBuffer;
	child_load.memory.resource = 8;
	child_load.memory.offset   = 6; // RDNA2 SMEM ignores the low two address bits.
	program.blocks[0].instructions.push_back(child_load);
	program.blocks[0].instructions.push_back(BufferUse(12, 16));

	std::string error;
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	Check(program.srt.reads.size() == 3 && program.srt.dynamic_reads.empty(),
	      "nested SRT reads were not compacted and deduplicated");
	std::vector<uint32_t> user_data(64);
	SetPointer(&user_data, 4, root.data());
	std::vector<uint32_t> flat;
	const SrtRuntime      runtime {user_data, 0, ReadHostMemory, nullptr};
	Check(WalkSrt(program, runtime, &flat, &error), error.c_str());
	Check(flat.size() == 3 && flat.back() == child[1],
	      "nested SRT walker read the wrong aligned dword");
}

void TestDynamicReadIsNotFlattened() {
	std::array<uint32_t, 4> table = {0xaabbccddu, 0x12345678u, 0, 0};
	Program                 program;
	program.blocks.resize(1);
	Instruction load;
	load.op                        = Opcode::SLoadDword;
	load.dst                       = Sgpr(16);
	load.src[0]                    = Sgpr(6);
	load.src_count                 = 1;
	load.memory.kind               = ResourceKind::ScalarBuffer;
	load.memory.resource           = 4;
	program.blocks[0].instructions = {load, BufferUse(4, 16)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	Check(program.srt.reads.empty() && program.srt.dynamic_reads.size() == 1,
	      "dynamic ReadConst was assigned a flat-buffer offset");
	std::vector<uint32_t> user_data(64);
	SetPointer(&user_data, 4, table.data());
	user_data[6] = 4;
	DescriptorValue  descriptor;
	const auto       source = program.blocks[0].instructions[1].memory.resource_source;
	const SrtRuntime runtime {user_data, 0, ReadHostMemory, nullptr};
	Check(EvaluateDescriptorSource(program, source, 4, runtime, &descriptor, &error),
	      error.c_str());
	Check(descriptor.dwords[0] == table[1], "dynamic ReadConst evaluated the wrong dword");
}

void TestReadConstBufferWalk() {
	std::array<uint32_t, 4> table = {0x76543210u, 0, 0, 0};
	Program                 program;
	program.blocks.resize(1);
	Instruction load;
	load.op                        = Opcode::SBufferLoadDword;
	load.dst                       = Sgpr(16);
	load.src[0]                    = Imm(0);
	load.src_count                 = 1;
	load.memory.kind               = ResourceKind::ScalarBuffer;
	load.memory.resource           = 1;
	program.blocks[0].instructions = {load, BufferUse(4, 16)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	std::vector<uint32_t> user_data(64);
	SetPointer(&user_data, 4, table.data());
	user_data[6] = 4;
	std::vector<uint32_t> flat;
	const SrtRuntime      runtime {user_data, 0, ReadHostMemory, nullptr};
	Check(WalkSrt(program, runtime, &flat, &error), error.c_str());
	Check(flat.size() == 1 && flat[0] == table[0], "ReadConstBuffer walker result was wrong");
}

void TestM0ScalarOffset() {
	std::array<uint32_t, 2> table = {0x01234567u, 0x89abcdefu};
	Program                 program;
	program.blocks.resize(1);
	Instruction set_m0;
	set_m0.op        = Opcode::MoveU32;
	set_m0.dst       = M0();
	set_m0.src[0]    = Imm(4);
	set_m0.src_count = 1;
	Instruction load;
	load.pc                        = 4;
	load.op                        = Opcode::SLoadDword;
	load.dst                       = Sgpr(16);
	load.src[0]                    = M0();
	load.src_count                 = 1;
	load.memory.kind               = ResourceKind::ScalarBuffer;
	load.memory.resource           = 4;
	program.blocks[0].instructions = {set_m0, load, BufferUse(8, 16)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	Check(program.srt.reads.size() == 1 && program.srt.dynamic_reads.empty(),
	      "constant M0 offset was not flattened");
	std::vector<uint32_t> user_data(64);
	SetPointer(&user_data, 4, table.data());
	std::vector<uint32_t> flat;
	const SrtRuntime      runtime {user_data, 0, ReadHostMemory, nullptr};
	Check(WalkSrt(program, runtime, &flat, &error), error.c_str());
	Check(flat.size() == 1 && flat[0] == table[1], "M0 scalar offset evaluated incorrectly");
}

void TestM0ArithmeticPreservesScc() {
	Program program;
	program.blocks.resize(1);
	Instruction low;
	low.op                         = Opcode::ScalarAddCarryU32;
	low.dst                        = M0();
	low.src[0]                     = Sgpr(0);
	low.src[1]                     = Imm(1);
	low.src[2]                     = Imm(0);
	low.src_count                  = 3;
	Instruction high               = low;
	high.pc                        = 4;
	high.dst                       = Sgpr(16);
	high.src[0]                    = Imm(5);
	high.src[1]                    = Imm(0);
	high.src[2].kind               = OperandKind::Register;
	high.src[2].reg.file           = RegisterFile::Scc;
	program.blocks[0].instructions = {low,
	                                  high,
	                                  MoveImmediate(8, 17, 0),
	                                  MoveImmediate(12, 18, 0),
	                                  MoveImmediate(16, 19, 0),
	                                  BufferUse(20, 16)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	const std::array<uint32_t, 1> user_data = {0xffffffffu};
	const auto       source = program.blocks[0].instructions.back().memory.resource_source;
	DescriptorValue  descriptor;
	const SrtRuntime runtime {user_data, 0, nullptr, nullptr};
	Check(EvaluateDescriptorSource(program, source, 20, runtime, &descriptor, &error),
	      error.c_str());
	Check(descriptor.dwords[0] == 6, "arithmetic M0 destination lost its SCC carry side effect");
}

void TestShaderRelativePointer() {
	std::array<uint32_t, 4> table = {};
	Program                 program;
	program.blocks.resize(1);
	Instruction getpc_low          = MoveImmediate(0x20, 0, 0x24);
	getpc_low.src[0].kind          = OperandKind::PcRelativeU32;
	Instruction getpc_high         = MoveImmediate(0x20, 1, 0);
	program.blocks[0].instructions = {getpc_low, getpc_high, BufferUse(0x28, 0)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	const auto      source = program.blocks[0].instructions.back().memory.resource_source;
	DescriptorValue descriptor;
	const auto      shader_base             = reinterpret_cast<uint64_t>(table.data()) - 0x24;
	const std::array<uint32_t, 4> user_data = {};
	const SrtRuntime              runtime {user_data, shader_base, nullptr, nullptr};
	Check(EvaluateDescriptorSource(program, source, 0x28, runtime, &descriptor, &error),
	      error.c_str());
	const auto expected = reinterpret_cast<uint64_t>(table.data());
	Check(descriptor.dwords[0] == static_cast<uint32_t>(expected) &&
	          descriptor.dwords[1] == static_cast<uint32_t>(expected >> 32u),
	      "shader-relative pointer lost its high half or carry");
}

void TestSmemComponentAlignment() {
	std::array<uint32_t, 2> table = {0x11111111u, 0x22222222u};
	std::vector<uint32_t>   user_data(20);
	SetPointer(&user_data, 4, table.data());

	Program raw;
	raw.blocks.resize(1);
	Instruction raw_load;
	raw_load.pc                = 4;
	raw_load.op                = Opcode::SLoadDword;
	raw_load.dst               = Sgpr(16);
	raw_load.src[0]            = Imm(2);
	raw_load.src_count         = 1;
	raw_load.memory.kind       = ResourceKind::ScalarBuffer;
	raw_load.memory.resource   = 4;
	raw_load.memory.offset     = 2;
	raw.blocks[0].instructions = {raw_load, BufferUse(12, 16)};
	std::string error;
	Check(BuildScalarProvenance(&raw, &error) && BuildSrtPlan(&raw, &error), error.c_str());
	std::vector<uint32_t> flat;
	const SrtRuntime      runtime {user_data, 0, ReadHostMemory, nullptr};
	Check(WalkSrt(raw, runtime, &flat, &error), error.c_str());
	Check(flat.size() == 1 && flat[0] == table[0],
	      "raw S_LOAD did not clear each address component's low bits");

	Program buffer;
	buffer.blocks.resize(1);
	Instruction buffer_load       = raw_load;
	buffer_load.op                = Opcode::SBufferLoadDword;
	buffer_load.memory.resource   = 1;
	buffer.blocks[0].instructions = {buffer_load, BufferUse(12, 16)};
	user_data[6]                  = sizeof(table);
	Check(BuildScalarProvenance(&buffer, &error) && BuildSrtPlan(&buffer, &error), error.c_str());
	Check(WalkSrt(buffer, runtime, &flat, &error), error.c_str());
	Check(flat.size() == 1 && flat[0] == table[1],
	      "S_BUFFER_LOAD did not align the summed final address");

	buffer.blocks[0].instructions[0].src[0] = Imm(0);
	user_data[4] += 2;
	Check(BuildScalarProvenance(&buffer, &error) && BuildSrtPlan(&buffer, &error), error.c_str());
	Check(WalkSrt(buffer, runtime, &flat, &error), error.c_str());
	Check(flat.size() == 1 && flat[0] == table[0],
	      "S_BUFFER_LOAD did not clear the descriptor base low bits before "
	      "addition");
}

void TestReadConstSignedAddress() {
	std::array<uint32_t, 2> table = {0x11111111u, 0x22222222u};
	std::vector<uint32_t>   user_data(24);
	SetPointer(&user_data, 4, table.data() + 1);

	Program program;
	program.blocks.resize(1);
	Instruction load;
	load.pc                        = 4;
	load.op                        = Opcode::SLoadDword;
	load.dst                       = Sgpr(16);
	load.src[0]                    = Sgpr(20);
	load.src_count                 = 1;
	load.memory.kind               = ResourceKind::ScalarBuffer;
	load.memory.resource           = 4;
	load.memory.offset             = static_cast<uint32_t>(-8);
	program.blocks[0].instructions = {MoveImmediate(0, 20, 4), load, BufferUse(8, 16)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	std::vector<uint32_t> flat;
	const SrtRuntime      runtime {user_data, 0, ReadHostMemory, nullptr};
	Check(WalkSrt(program, runtime, &flat, &error), error.c_str());
	Check(flat.size() == 1 && flat[0] == table[0],
	      "signed S_LOAD offset resolved the wrong SBASE-relative address");

	user_data[4] = 0;
	user_data[5] = 0;
	Check(!WalkSrt(program, runtime, &flat, &error) &&
	          error.find("outside the 48-bit address space") != std::string::npos,
	      "signed S_LOAD address underflow did not fail closed");
}

void TestReadConstBufferBounds() {
	std::array<uint32_t, 2> table = {0x11111111u, 0x22222222u};
	Program                 program;
	program.stage       = ShaderType::Compute;
	program.shader_hash = 0x1234;
	program.blocks.resize(1);
	Instruction load;
	load.pc                        = 0x40;
	load.op                        = Opcode::SBufferLoadDword;
	load.dst                       = Sgpr(16);
	load.src[0]                    = Imm(0);
	load.src_count                 = 1;
	load.memory.kind               = ResourceKind::ScalarBuffer;
	load.memory.resource           = 1;
	load.memory.offset             = sizeof(table);
	program.blocks[0].instructions = {load, BufferUse(0x44, 16)};
	std::string error;
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	std::vector<uint32_t> user_data(20);
	SetPointer(&user_data, 4, table.data());
	user_data[5] |= 4u << 16u;
	user_data[6] = 2;
	ReadCounter           counter;
	std::vector<uint32_t> flat;
	const SrtRuntime      runtime {user_data, 0, CountedHostMemory, &counter};
	Check(!WalkSrt(program, runtime, &flat, &error),
	      "out-of-range S_BUFFER_LOAD unexpectedly succeeded");
	Check(counter.reads == 0 && error.find("exceeds size=8") != std::string::npos &&
	          error.find("hash=0x0000000000001234") != std::string::npos &&
	          error.find("stage=compute") != std::string::npos &&
	          error.find("pc=0x00000044") != std::string::npos,
	      "S_BUFFER_LOAD bounds failure read memory or lost shader context");
}

void TestSubBorrowPointerChain() {
	Program program;
	program.blocks.resize(1);
	Instruction low;
	low.op        = Opcode::ScalarSubBorrowU32;
	low.dst       = Sgpr(16);
	low.src[0]    = Sgpr(0);
	low.src[1]    = Imm(4);
	low.src_count = 2;
	Instruction high;
	high.pc                        = 4;
	high.op                        = Opcode::ScalarSubBorrowCarryU32;
	high.dst                       = Sgpr(17);
	high.src[0]                    = Sgpr(1);
	high.src[1]                    = Imm(0);
	high.src[2].kind               = OperandKind::Register;
	high.src[2].reg.file           = RegisterFile::Scc;
	high.src_count                 = 3;
	program.blocks[0].instructions = {low, high, MoveImmediate(8, 18, 0), MoveImmediate(12, 19, 0),
	                                  BufferUse(16, 16)};
	std::string error;
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	const std::array<uint32_t, 2> user_data = {2, 5};
	DescriptorValue               descriptor;
	const auto       source = program.blocks[0].instructions.back().memory.resource_source;
	const SrtRuntime runtime {user_data, 0, nullptr, nullptr};
	Check(EvaluateDescriptorSource(program, source, 16, runtime, &descriptor, &error),
	      error.c_str());
	Check(descriptor.dwords[0] == 0xfffffffeu && descriptor.dwords[1] == 4,
	      "scalar subtraction/borrow pointer chain evaluated incorrectly");
}

void TestCommonScalarPointerOps() {
	Program program;
	program.blocks.resize(1);
	const auto binary = [](Opcode op, uint32_t dst, Operand lhs, Operand rhs) {
		Instruction inst;
		inst.op        = op;
		inst.dst       = Sgpr(dst);
		inst.src[0]    = lhs;
		inst.src[1]    = rhs;
		inst.src_count = 2;
		return inst;
	};
	Instruction add3;
	add3.op                        = Opcode::IAdd3U32;
	add3.dst                       = Sgpr(19);
	add3.src[0]                    = Sgpr(3);
	add3.src[1]                    = Imm(1);
	add3.src[2]                    = Imm(2);
	add3.src_count                 = 3;
	program.blocks[0].instructions = {binary(Opcode::BitwiseXorU32, 16, Sgpr(0), Sgpr(1)),
	                                  binary(Opcode::BitwiseAndNotU32, 17, Sgpr(0), Sgpr(1)),
	                                  binary(Opcode::IMulU32, 18, Sgpr(2), Imm(4)), add3,
	                                  BufferUse(16, 16)};
	std::string error;
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	const std::array<uint32_t, 4> user_data = {0xf0f0u, 0x0ff0u, 7u, 9u};
	DescriptorValue               descriptor;
	const auto       source = program.blocks[0].instructions.back().memory.resource_source;
	const SrtRuntime runtime {user_data, 0, nullptr, nullptr};
	Check(EvaluateDescriptorSource(program, source, 16, runtime, &descriptor, &error),
	      error.c_str());
	Check(descriptor.dwords[0] == (user_data[0] ^ user_data[1]) &&
	          descriptor.dwords[1] == (user_data[0] & ~user_data[1]) &&
	          descriptor.dwords[2] == 28 && descriptor.dwords[3] == 12,
	      "common scalar pointer operations evaluated incorrectly");
}

void TestBitFieldMaskDescriptor() {
	Program program;
	program.blocks.resize(1);
	auto mask      = MoveImmediate(4, 29, 0);
	mask.op        = Opcode::BitFieldMaskU32;
	mask.src[0]    = Imm(12);
	mask.src[1]    = Imm(12);
	mask.src_count = 2;
	auto high      = MoveImmediate(8, 30, 0x05500000u);
	high.op        = Opcode::MoveU64;
	program.blocks[0].instructions = {MoveImmediate(0, 28, 0x92u), mask, high,
	                                  BufferUse(12, 28)};

	std::string error;
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	DescriptorValue descriptor;
	const SrtRuntime runtime {{}, 0, nullptr, nullptr};
	Check(EvaluateDescriptorSource(program,
	                               program.blocks[0].instructions.back().memory.resource_source,
	                               16, runtime, &descriptor, &error),
	      error.c_str());
	Check(descriptor.dwords[0] == 0x92u && descriptor.dwords[1] == 0x00fff000u &&
	          descriptor.dwords[2] == 0x05500000u && descriptor.dwords[3] == 0,
	      "production sampler bit-field mask evaluated incorrectly");

	mask.src[0] = Imm(0);
	program.blocks[0].instructions = {MoveImmediate(0, 28, 0x92u), mask, high,
	                                  BufferUse(12, 28)};
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	Check(EvaluateDescriptorSource(program,
	                               program.blocks[0].instructions.back().memory.resource_source,
	                               12, runtime, &descriptor, &error),
	      error.c_str());
	Check(descriptor.dwords[1] == 0, "zero-width bit-field mask was not zero");

	mask.src[0] = Imm(31);
	mask.src[1] = Imm(31);
	program.blocks[0].instructions = {MoveImmediate(0, 28, 0x92u), mask, high,
	                                  BufferUse(12, 28)};
	Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error), error.c_str());
	Check(EvaluateDescriptorSource(program,
	                               program.blocks[0].instructions.back().memory.resource_source,
	                               12, runtime, &descriptor, &error),
	      error.c_str());
	Check(descriptor.dwords[1] == 0x80000000u,
	      "maximum bit-field mask count/offset evaluated incorrectly");
}

} // namespace

int main() {
	try {
		TestPerUseDescriptorDefinitions();
		TestCfgPhi();
		TestDiamondReadPathsAreDynamic();
		TestEquivalentConstantPhiIsStatic();
		TestWideMoveInvalidatesAndCopiesBothDwords();
		TestScalarCarryChain();
		TestReadConstDefinition();
		TestReadConstBufferAndValueNumbering();
		TestReadLaneDescriptorSpill();
		TestReadLaneVectorOverwriteInvalidatesSpill();
		TestReadLanePhi();
		TestReadLaneLoopConvergence();
		TestReadLaneWideAndRelativeWritesInvalidateSpill();
		TestReadLaneModuloAndDynamicLane();
		TestUnresolvedSourceIsMarked();
		TestUnsupportedWideWriteInvalidatesBothDwords();
		TestCyclicPhiIsUnresolved();
		TestNestedSrtWalk();
		TestDynamicReadIsNotFlattened();
		TestReadConstBufferWalk();
		TestM0ScalarOffset();
		TestM0ArithmeticPreservesScc();
		TestShaderRelativePointer();
		TestSmemComponentAlignment();
		TestReadConstSignedAddress();
		TestReadConstBufferBounds();
		TestSubBorrowPointerChain();
		TestCommonScalarPointerOps();
		TestBitFieldMaskDescriptor();
		std::cout << "ScalarProvenanceTests: all cases passed\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ScalarProvenanceTests: failed: " << e.what() << '\n';
		return 1;
	}
}
