#include "graphics/shader/rectListShader.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/emitter/SpirvBuilder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"
#include "spirv/unified1/spirv.hpp11"

#include <array>
#include <bit>
#include <cstdint>
#include <utility>

namespace Libs::Graphics {

namespace {

using ShaderRecompiler::Spirv::Builder;

constexpr uint32_t SpirvVersion15 = 0x00010500u;

template <typename T>
constexpr uint32_t Word(T value) {
	return static_cast<uint32_t>(value);
}

struct Parameter {
	uint32_t input_location  = 0;
	uint32_t output_location = 0;
	bool     flat            = false;
};

std::vector<Parameter> GetParameters(const ShaderVertexInputInfo& vertex_info,
                                     const ShaderPixelInputInfo*  pixel_info) {
	if (pixel_info == nullptr) {
		return {};
	}
	EXIT_IF(pixel_info->input_num > ShaderVertexInputInfo::RES_MAX);
	EXIT_IF(pixel_info->stage.program == nullptr);

	std::vector<uint32_t> active_inputs;
	for (const auto& input: pixel_info->stage.program->info.inputs) {
		if (input.kind == ShaderRecompiler::IR::StageInputKind::Parameter) {
			active_inputs.push_back(input.location);
		}
	}

	std::vector<Parameter> parameters;
	for (const auto input: active_inputs) {
		const auto input_location = ShaderPixelParameterMappedLocation(*pixel_info, input);
		if ((vertex_info.param_export_mask & (1u << input_location)) != 0) {
			parameters.push_back({input_location,
			                      ShaderPixelParameterLocation(*pixel_info, active_inputs, input),
			                      ShaderPixelParameterIsFlat(*pixel_info, input)});
		}
	}
	return parameters;
}

class RectListEmitter {
public:
	RectListEmitter(const std::vector<Parameter>& parameters_, spv::ExecutionModel model)
	    : parameters(parameters_) {
		builder.AddMemoryModel(
		    {Word(spv::AddressingModel::Logical), Word(spv::MemoryModel::GLSL450)});

		void_type       = Type(spv::Op::OpTypeVoid);
		uint_type       = Type(spv::Op::OpTypeInt, 32u, 0u);
		int_type        = Type(spv::Op::OpTypeInt, 32u, 1u);
		float_type      = Type(spv::Op::OpTypeFloat, 32u);
		vec4_float_type = Type(spv::Op::OpTypeVector, float_type, 4u);
		function_type   = Type(spv::Op::OpTypeFunction, void_type);

		per_vertex_type = Type(spv::Op::OpTypeStruct, vec4_float_type);
		builder.AddAnnotation({Word(spv::Op::OpMemberDecorate), per_vertex_type, 0u,
		                       Word(spv::Decoration::BuiltIn), Word(spv::BuiltIn::Position)});
		builder.AddAnnotation(
		    {Word(spv::Op::OpDecorate), per_vertex_type, Word(spv::Decoration::Block)});

		ptr_input_vec4_float  = Pointer(spv::StorageClass::Input, vec4_float_type);
		ptr_output_vec4_float = Pointer(spv::StorageClass::Output, vec4_float_type);
		if (model == spv::ExecutionModel::TessellationControl) {
			bool_type        = Type(spv::Op::OpTypeBool);
			vec2_bool_type   = Type(spv::Op::OpTypeVector, bool_type, 2u);
			vec2_float_type  = Type(spv::Op::OpTypeVector, float_type, 2u);
			ptr_output_float = Pointer(spv::StorageClass::Output, float_type);
		} else {
			vec3_float_type = Type(spv::Op::OpTypeVector, float_type, 3u);
			ptr_input_float = Pointer(spv::StorageClass::Input, float_type);
		}
	}

	std::vector<uint32_t> EmitControl() {
		DefineEntry(spv::ExecutionModel::TessellationControl);
		const auto float_one = Constant(float_type, std::bit_cast<uint32_t>(1.0f));

		for (uint32_t i = 0; i < 4; i++) {
			Store(Access(ptr_output_float, tess_outer, Int(i)), float_one);
		}
		for (uint32_t i = 0; i < 2; i++) {
			Store(Access(ptr_output_float, tess_inner, Int(i)), float_one);
		}

		std::array<uint32_t, 3> positions {};
		for (uint32_t i = 0; i < positions.size(); i++) {
			positions[i] =
			    Load(vec4_float_type, Access(ptr_input_vec4_float, gl_in, Int(i), Int(0)));
		}

		std::array<uint32_t, 3> coordinate_equal {};
		for (uint32_t i = 0; i < coordinate_equal.size(); i++) {
			const auto left  = Result(spv::Op::OpVectorShuffle, vec2_float_type, positions[i],
			                          positions[i], 0u, 1u);
			const auto right = Result(spv::Op::OpVectorShuffle, vec2_float_type,
			                          positions[(i + 1u) % 3u], positions[(i + 1u) % 3u], 0u, 1u);
			coordinate_equal[i] = Result(spv::Op::OpFOrdEqual, vec2_bool_type, left, right);
		}

		std::array<uint32_t, 3> barycentric {};
		std::array<uint32_t, 3> edge_vertex {};
		const auto float_minus_one = Constant(float_type, std::bit_cast<uint32_t>(-1.0f));
		for (uint32_t i = 0; i < edge_vertex.size(); i++) {
			const auto previous = (i + 2u) % 3u;
			const auto xy       = Result(
			    spv::Op::OpLogicalAnd, bool_type,
			    Result(spv::Op::OpCompositeExtract, bool_type, coordinate_equal[i], 0u),
			    Result(spv::Op::OpCompositeExtract, bool_type, coordinate_equal[previous], 1u));
			const auto yx = Result(
			    spv::Op::OpLogicalAnd, bool_type,
			    Result(spv::Op::OpCompositeExtract, bool_type, coordinate_equal[i], 1u),
			    Result(spv::Op::OpCompositeExtract, bool_type, coordinate_equal[previous], 0u));
			edge_vertex[i] = Result(spv::Op::OpLogicalOr, bool_type, xy, yx);
			barycentric[i] =
			    Result(spv::Op::OpSelect, float_type, edge_vertex[i], float_minus_one, float_one);
		}

		auto vertex_index = Result(spv::Op::OpSelect, int_type, edge_vertex[2], Int(2), Int(0));
		vertex_index = Result(spv::Op::OpSelect, int_type, edge_vertex[1], Int(1), vertex_index);
		const auto invocation = Load(int_type, invocation_id);
		const auto is_fourth  = Result(spv::Op::OpIEqual, bool_type, invocation, Int(3));
		const auto index =
		    Result(spv::Op::OpSMod, int_type,
		           Result(spv::Op::OpIAdd, int_type, vertex_index, invocation), Int(3));

		const auto position3 = Interpolate(positions[0], positions[1], positions[2], barycentric);
		const auto position =
		    Result(spv::Op::OpSelect, vec4_float_type, is_fourth, position3,
		           Load(vec4_float_type, Access(ptr_input_vec4_float, gl_in, index, Int(0))));
		Store(Access(ptr_output_vec4_float, gl_out, invocation, Int(0)), position);

		for (uint32_t i = 0; i < parameters.size(); i++) {
			const auto input0 =
			    Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], Int(0)));
			if (parameters[i].flat) {
				Store(Access(ptr_output_vec4_float, outputs[i], invocation), input0);
				continue;
			}
			const auto input1 =
			    Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], Int(1)));
			const auto input2 =
			    Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], Int(2)));
			const auto input3 = Interpolate(input0, input1, input2, barycentric);
			const auto value =
			    Result(spv::Op::OpSelect, vec4_float_type, is_fourth, input3,
			           Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], index)));
			Store(Access(ptr_output_vec4_float, outputs[i], invocation), value);
		}

		Emit(spv::Op::OpReturn);
		Emit(spv::Op::OpFunctionEnd);
		return builder.Build();
	}

	std::vector<uint32_t> EmitEvaluation() {
		DefineEntry(spv::ExecutionModel::TessellationEvaluation);

		const auto x     = Load(float_type, Access(ptr_input_float, tess_coord, Int(0)));
		const auto y     = Load(float_type, Access(ptr_input_float, tess_coord, Int(1)));
		const auto index = Result(
		    spv::Op::OpIAdd, int_type,
		    Result(spv::Op::OpIMul, int_type, Result(spv::Op::OpConvertFToS, int_type, y), Int(2)),
		    Result(spv::Op::OpConvertFToS, int_type, x));

		const auto position =
		    Load(vec4_float_type, Access(ptr_input_vec4_float, gl_in, index, Int(0)));
		Store(Access(ptr_output_vec4_float, gl_out, Int(0)), position);
		for (uint32_t i = 0; i < parameters.size(); i++) {
			Store(outputs[i],
			      Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], index)));
		}

		Emit(spv::Op::OpReturn);
		Emit(spv::Op::OpFunctionEnd);
		return builder.Build();
	}

private:
	template <typename... Args>
	uint32_t Type(spv::Op opcode, Args... operands) {
		const auto id = builder.AllocateId();
		builder.AddType({Word(opcode), id, Word(operands)...});
		return id;
	}

	uint32_t Constant(uint32_t type, uint32_t value) {
		const auto id = builder.AllocateId();
		builder.AddType({Word(spv::Op::OpConstant), type, id, value});
		return id;
	}

	uint32_t Pointer(spv::StorageClass storage, uint32_t type) {
		return Type(spv::Op::OpTypePointer, storage, type);
	}

	uint32_t Array(uint32_t type, uint32_t size) {
		return Type(spv::Op::OpTypeArray, type, Uint(size));
	}

	template <typename... Args>
	uint32_t Result(spv::Op opcode, uint32_t type, Args... operands) {
		const auto id = builder.AllocateId();
		builder.AddFunction({Word(opcode), type, id, Word(operands)...});
		return id;
	}

	template <typename... Args>
	uint32_t ResultWithoutType(spv::Op opcode, Args... operands) {
		const auto id = builder.AllocateId();
		builder.AddFunction({Word(opcode), id, Word(operands)...});
		return id;
	}

	template <typename... Args>
	void Emit(spv::Op opcode, Args... operands) {
		builder.AddFunction({Word(opcode), Word(operands)...});
	}

	template <typename... Args>
	uint32_t Access(uint32_t pointer_type, uint32_t base, Args... indices) {
		return Result(spv::Op::OpAccessChain, pointer_type, base, Word(indices)...);
	}

	uint32_t Load(uint32_t type, uint32_t pointer) {
		return Result(spv::Op::OpLoad, type, pointer);
	}

	void Store(uint32_t pointer, uint32_t value) { Emit(spv::Op::OpStore, pointer, value); }

	uint32_t Int(uint32_t value) {
		auto& id = int_constants[value];
		if (id == 0) {
			id = Constant(int_type, value);
		}
		return id;
	}

	uint32_t Uint(uint32_t value) {
		auto& id = uint_constants[value];
		if (id == 0) {
			id = Constant(uint_type, value);
		}
		return id;
	}

	uint32_t AddInterface(spv::StorageClass storage, uint32_t type) {
		const auto variable = builder.AllocateId();
		builder.AddType(
		    {Word(spv::Op::OpVariable), Pointer(storage, type), variable, Word(storage)});
		interfaces.push_back(variable);
		return variable;
	}

	void Decorate(uint32_t target, spv::Decoration decoration, uint32_t value) {
		builder.AddAnnotation({Word(spv::Op::OpDecorate), target, Word(decoration), value});
	}

	void DefineEntry(spv::ExecutionModel model) {
		builder.AddCapability({Word(spv::Capability::Shader)});
		builder.AddCapability({Word(spv::Capability::Tessellation)});
		main = Result(spv::Op::OpFunction, void_type, spv::FunctionControlMask::MaskNone,
		              function_type);
		if (model == spv::ExecutionModel::TessellationControl) {
			builder.AddExecutionMode({main, Word(spv::ExecutionMode::OutputVertices), 4u});
		} else {
			builder.AddExecutionMode({main, Word(spv::ExecutionMode::Quads)});
			builder.AddExecutionMode({main, Word(spv::ExecutionMode::SpacingEqual)});
			builder.AddExecutionMode({main, Word(spv::ExecutionMode::VertexOrderCw)});
		}
		DefineInputs(model);
		DefineOutputs(model);
		builder.AddEntryPoint(Word(model), main, "main", interfaces);
		ResultWithoutType(spv::Op::OpLabel);
	}

	void DefineInputs(spv::ExecutionModel model) {
		const auto tess_control = model == spv::ExecutionModel::TessellationControl;
		if (tess_control) {
			invocation_id = AddInterface(spv::StorageClass::Input, int_type);
			Decorate(invocation_id, spv::Decoration::BuiltIn, Word(spv::BuiltIn::InvocationId));
		} else {
			tess_coord = AddInterface(spv::StorageClass::Input, vec3_float_type);
			Decorate(tess_coord, spv::Decoration::BuiltIn, Word(spv::BuiltIn::TessCoord));
		}
		gl_in =
		    AddInterface(spv::StorageClass::Input, Array(per_vertex_type, tess_control ? 3u : 4u));

		inputs.resize(parameters.size());
		std::array<uint32_t, ShaderVertexInputInfo::RES_MAX> locations {};
		for (uint32_t i = 0; i < parameters.size(); i++) {
			const auto location =
			    tess_control ? parameters[i].input_location : parameters[i].output_location;
			if (tess_control && locations[location] != 0) {
				inputs[i] = locations[location];
				continue;
			}
			inputs[i] = AddInterface(spv::StorageClass::Input,
			                         Array(vec4_float_type, tess_control ? 3u : 4u));
			Decorate(inputs[i], spv::Decoration::Location, location);
			locations[location] = inputs[i];
		}
	}

	void DefineOutputs(spv::ExecutionModel model) {
		const auto tess_control = model == spv::ExecutionModel::TessellationControl;
		if (tess_control) {
			gl_out     = AddInterface(spv::StorageClass::Output, Array(per_vertex_type, 4u));
			tess_inner = AddInterface(spv::StorageClass::Output, Array(float_type, 2u));
			Decorate(tess_inner, spv::Decoration::BuiltIn, Word(spv::BuiltIn::TessLevelInner));
			builder.AddAnnotation(
			    {Word(spv::Op::OpDecorate), tess_inner, Word(spv::Decoration::Patch)});
			tess_outer = AddInterface(spv::StorageClass::Output, Array(float_type, 4u));
			Decorate(tess_outer, spv::Decoration::BuiltIn, Word(spv::BuiltIn::TessLevelOuter));
			builder.AddAnnotation(
			    {Word(spv::Op::OpDecorate), tess_outer, Word(spv::Decoration::Patch)});
		} else {
			gl_out = AddInterface(spv::StorageClass::Output, per_vertex_type);
		}

		outputs.resize(parameters.size());
		for (uint32_t i = 0; i < parameters.size(); i++) {
			outputs[i] = AddInterface(spv::StorageClass::Output,
			                          tess_control ? Array(vec4_float_type, 4u) : vec4_float_type);
			Decorate(outputs[i], spv::Decoration::Location, parameters[i].output_location);
		}
	}

	uint32_t Interpolate(uint32_t v0, uint32_t v1, uint32_t v2,
	                     const std::array<uint32_t, 3>& barycentric) {
		const auto p0 = Result(spv::Op::OpVectorTimesScalar, vec4_float_type, v0, barycentric[0]);
		const auto p1 = Result(spv::Op::OpVectorTimesScalar, vec4_float_type, v1, barycentric[1]);
		const auto p2 = Result(spv::Op::OpVectorTimesScalar, vec4_float_type, v2, barycentric[2]);
		return Result(spv::Op::OpFAdd, vec4_float_type, p0,
		              Result(spv::Op::OpFAdd, vec4_float_type, p1, p2));
	}

	Builder                       builder {SpirvVersion15};
	const std::vector<Parameter>& parameters;
	std::vector<uint32_t>         interfaces;
	std::vector<uint32_t>         inputs;
	std::vector<uint32_t>         outputs;
	std::array<uint32_t, 5>       int_constants {};
	std::array<uint32_t, 5>       uint_constants {};
	uint32_t                      main                  = 0;
	uint32_t                      void_type             = 0;
	uint32_t                      bool_type             = 0;
	uint32_t                      uint_type             = 0;
	uint32_t                      int_type              = 0;
	uint32_t                      float_type            = 0;
	uint32_t                      vec2_bool_type        = 0;
	uint32_t                      vec2_float_type       = 0;
	uint32_t                      vec3_float_type       = 0;
	uint32_t                      vec4_float_type       = 0;
	uint32_t                      function_type         = 0;
	uint32_t                      per_vertex_type       = 0;
	uint32_t                      ptr_input_float       = 0;
	uint32_t                      ptr_input_vec4_float  = 0;
	uint32_t                      ptr_output_float      = 0;
	uint32_t                      ptr_output_vec4_float = 0;
	uint32_t                      gl_in                 = 0;
	uint32_t                      gl_out                = 0;
	uint32_t                      tess_inner            = 0;
	uint32_t                      tess_outer            = 0;
	uint32_t                      tess_coord            = 0;
	uint32_t                      invocation_id         = 0;
};

} // namespace

RectListShaders BuildRectListShaders(const ShaderVertexInputInfo& vertex_info,
                                     const ShaderPixelInputInfo*  pixel_info) {
	const auto      parameters = GetParameters(vertex_info, pixel_info);
	RectListEmitter control(parameters, spv::ExecutionModel::TessellationControl);
	RectListEmitter evaluation(parameters, spv::ExecutionModel::TessellationEvaluation);
	return {control.EmitControl(), evaluation.EmitEvaluation()};
}

} // namespace Libs::Graphics
