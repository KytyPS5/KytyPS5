#include "graphics/shader/rectListShader.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <array>
#include <bit>
#include <cstdint>
#include <utility>

namespace Libs::Graphics {

namespace {

using ShaderRecompiler::Spirv::Builder;

constexpr uint32_t SpirvVersion15 = 0x00010500u;

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
		if ((vertex_info.stage.program->param_export_mask & (1u << input_location)) != 0) {
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
		builder.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);

		void_type       = Type(spv::OpTypeVoid);
		uint_type       = Type(spv::OpTypeInt, 32u, 0u);
		int_type        = Type(spv::OpTypeInt, 32u, 1u);
		float_type      = Type(spv::OpTypeFloat, 32u);
		vec4_float_type = Type(spv::OpTypeVector, float_type, 4u);
		function_type   = Type(spv::OpTypeFunction, void_type);

		per_vertex_type = builder.DecoratedType(
		    spv::OpTypeStruct,
		    {{spv::OpMemberDecorate, {0u, spv::DecorationBuiltIn, spv::BuiltInPosition}},
		     {spv::OpDecorate, {spv::DecorationBlock}}},
		    vec4_float_type);

		ptr_input_vec4_float  = Pointer(spv::StorageClassInput, vec4_float_type);
		ptr_output_vec4_float = Pointer(spv::StorageClassOutput, vec4_float_type);
		if (model == spv::ExecutionModelTessellationControl) {
			bool_type        = Type(spv::OpTypeBool);
			vec2_bool_type   = Type(spv::OpTypeVector, bool_type, 2u);
			vec2_float_type  = Type(spv::OpTypeVector, float_type, 2u);
			ptr_output_float = Pointer(spv::StorageClassOutput, float_type);
		} else {
			vec3_float_type = Type(spv::OpTypeVector, float_type, 3u);
			ptr_input_float = Pointer(spv::StorageClassInput, float_type);
		}
	}

	std::vector<uint32_t> EmitControl() {
		DefineEntry(spv::ExecutionModelTessellationControl);
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
			const auto left =
			    Result(spv::OpVectorShuffle, vec2_float_type, positions[i], positions[i], 0u, 1u);
			const auto right = Result(spv::OpVectorShuffle, vec2_float_type,
			                          positions[(i + 1u) % 3u], positions[(i + 1u) % 3u], 0u, 1u);
			coordinate_equal[i] = Result(spv::OpFOrdEqual, vec2_bool_type, left, right);
		}

		std::array<uint32_t, 3> barycentric {};
		std::array<uint32_t, 3> edge_vertex {};
		const auto float_minus_one = Constant(float_type, std::bit_cast<uint32_t>(-1.0f));
		for (uint32_t i = 0; i < edge_vertex.size(); i++) {
			const auto previous = (i + 2u) % 3u;
			const auto xy =
			    Result(spv::OpLogicalAnd, bool_type,
			           Result(spv::OpCompositeExtract, bool_type, coordinate_equal[i], 0u),
			           Result(spv::OpCompositeExtract, bool_type, coordinate_equal[previous], 1u));
			const auto yx =
			    Result(spv::OpLogicalAnd, bool_type,
			           Result(spv::OpCompositeExtract, bool_type, coordinate_equal[i], 1u),
			           Result(spv::OpCompositeExtract, bool_type, coordinate_equal[previous], 0u));
			edge_vertex[i] = Result(spv::OpLogicalOr, bool_type, xy, yx);
			barycentric[i] =
			    Result(spv::OpSelect, float_type, edge_vertex[i], float_minus_one, float_one);
		}

		auto vertex_index = Result(spv::OpSelect, int_type, edge_vertex[2], Int(2), Int(0));
		vertex_index      = Result(spv::OpSelect, int_type, edge_vertex[1], Int(1), vertex_index);
		const auto invocation = Load(int_type, invocation_id);
		const auto is_fourth  = Result(spv::OpIEqual, bool_type, invocation, Int(3));
		const auto index = Result(spv::OpSMod, int_type,
		                          Result(spv::OpIAdd, int_type, vertex_index, invocation), Int(3));

		const auto position3 = Interpolate(positions[0], positions[1], positions[2], barycentric);
		const auto position =
		    Result(spv::OpSelect, vec4_float_type, is_fourth, position3,
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
			    Result(spv::OpSelect, vec4_float_type, is_fourth, input3,
			           Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], index)));
			Store(Access(ptr_output_vec4_float, outputs[i], invocation), value);
		}

		Emit(spv::OpReturn);
		Emit(spv::OpFunctionEnd);
		return builder.Build();
	}

	std::vector<uint32_t> EmitEvaluation() {
		DefineEntry(spv::ExecutionModelTessellationEvaluation);

		const auto x     = Load(float_type, Access(ptr_input_float, tess_coord, Int(0)));
		const auto y     = Load(float_type, Access(ptr_input_float, tess_coord, Int(1)));
		const auto index =
		    Result(spv::OpIAdd, int_type,
		           Result(spv::OpIMul, int_type, Result(spv::OpConvertFToS, int_type, y), Int(2)),
		           Result(spv::OpConvertFToS, int_type, x));

		const auto position =
		    Load(vec4_float_type, Access(ptr_input_vec4_float, gl_in, index, Int(0)));
		Store(Access(ptr_output_vec4_float, gl_out, Int(0)), position);
		for (uint32_t i = 0; i < parameters.size(); i++) {
			Store(outputs[i],
			      Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], index)));
		}

		Emit(spv::OpReturn);
		Emit(spv::OpFunctionEnd);
		return builder.Build();
	}

private:
	template <typename... Args>
	uint32_t Type(spv::Op opcode, Args... operands) {
		return builder.Type(opcode, operands...);
	}

	uint32_t Constant(uint32_t type, uint32_t value) {
		return builder.Constant(spv::OpConstant, type, value);
	}

	uint32_t Pointer(spv::StorageClass storage, uint32_t type) {
		return Type(spv::OpTypePointer, storage, type);
	}

	uint32_t Array(uint32_t type, uint32_t size) {
		return Type(spv::OpTypeArray, type, Uint(size));
	}

	template <typename... Args>
	uint32_t Result(spv::Op opcode, uint32_t type, Args... operands) {
		const auto id = builder.AllocateId();
		builder.AddFunction(opcode, type, id, operands...);
		return id;
	}

	template <typename... Args>
	uint32_t ResultWithoutType(spv::Op opcode, Args... operands) {
		const auto id = builder.AllocateId();
		builder.AddFunction(opcode, id, operands...);
		return id;
	}

	template <typename... Args>
	void Emit(spv::Op opcode, Args... operands) {
		builder.AddFunction(opcode, operands...);
	}

	template <typename... Args>
	uint32_t Access(uint32_t pointer_type, uint32_t base, Args... indices) {
		return Result(spv::OpAccessChain, pointer_type, base, indices...);
	}

	uint32_t Load(uint32_t type, uint32_t pointer) { return Result(spv::OpLoad, type, pointer); }

	void Store(uint32_t pointer, uint32_t value) { Emit(spv::OpStore, pointer, value); }

	uint32_t Int(uint32_t value) { return Constant(int_type, value); }

	uint32_t Uint(uint32_t value) { return Constant(uint_type, value); }

	uint32_t AddInterface(spv::StorageClass storage, uint32_t type) {
		const auto variable = builder.DefineGlobalVariable(Pointer(storage, type), storage);
		interfaces.push_back(variable);
		return variable;
	}

	void Decorate(uint32_t target, spv::Decoration decoration, uint32_t value) {
		builder.AddAnnotation(spv::OpDecorate, target, decoration, value);
	}

	void DefineEntry(spv::ExecutionModel model) {
		builder.RequireCapability(spv::CapabilityShader);
		builder.RequireCapability(spv::CapabilityTessellation);
		main = Result(spv::OpFunction, void_type, spv::FunctionControlMaskNone, function_type);
		if (model == spv::ExecutionModelTessellationControl) {
			builder.AddExecutionMode(main, spv::ExecutionModeOutputVertices, 4u);
		} else {
			builder.AddExecutionMode(main, spv::ExecutionModeQuads);
			builder.AddExecutionMode(main, spv::ExecutionModeSpacingEqual);
			builder.AddExecutionMode(main, spv::ExecutionModeVertexOrderCw);
		}
		DefineInputs(model);
		DefineOutputs(model);
		builder.AddEntryPoint(model, main, "main", interfaces);
		ResultWithoutType(spv::OpLabel);
	}

	void DefineInputs(spv::ExecutionModel model) {
		const auto tess_control = model == spv::ExecutionModelTessellationControl;
		if (tess_control) {
			invocation_id = AddInterface(spv::StorageClassInput, int_type);
			Decorate(invocation_id, spv::DecorationBuiltIn, spv::BuiltInInvocationId);
		} else {
			tess_coord = AddInterface(spv::StorageClassInput, vec3_float_type);
			Decorate(tess_coord, spv::DecorationBuiltIn, spv::BuiltInTessCoord);
		}
		gl_in =
		    AddInterface(spv::StorageClassInput, Array(per_vertex_type, tess_control ? 3u : 4u));

		inputs.resize(parameters.size());
		std::array<uint32_t, ShaderVertexInputInfo::RES_MAX> locations {};
		for (uint32_t i = 0; i < parameters.size(); i++) {
			const auto location =
			    tess_control ? parameters[i].input_location : parameters[i].output_location;
			if (tess_control && locations[location] != 0) {
				inputs[i] = locations[location];
				continue;
			}
			inputs[i] = AddInterface(spv::StorageClassInput,
			                         Array(vec4_float_type, tess_control ? 3u : 4u));
			Decorate(inputs[i], spv::DecorationLocation, location);
			locations[location] = inputs[i];
		}
	}

	void DefineOutputs(spv::ExecutionModel model) {
		const auto tess_control = model == spv::ExecutionModelTessellationControl;
		if (tess_control) {
			gl_out     = AddInterface(spv::StorageClassOutput, Array(per_vertex_type, 4u));
			tess_inner = AddInterface(spv::StorageClassOutput, Array(float_type, 2u));
			Decorate(tess_inner, spv::DecorationBuiltIn, spv::BuiltInTessLevelInner);
			builder.AddAnnotation(spv::OpDecorate, tess_inner, spv::DecorationPatch);
			tess_outer = AddInterface(spv::StorageClassOutput, Array(float_type, 4u));
			Decorate(tess_outer, spv::DecorationBuiltIn, spv::BuiltInTessLevelOuter);
			builder.AddAnnotation(spv::OpDecorate, tess_outer, spv::DecorationPatch);
		} else {
			gl_out = AddInterface(spv::StorageClassOutput, per_vertex_type);
		}

		outputs.resize(parameters.size());
		for (uint32_t i = 0; i < parameters.size(); i++) {
			outputs[i] = AddInterface(spv::StorageClassOutput,
			                          tess_control ? Array(vec4_float_type, 4u) : vec4_float_type);
			Decorate(outputs[i], spv::DecorationLocation, parameters[i].output_location);
		}
	}

	uint32_t Interpolate(uint32_t v0, uint32_t v1, uint32_t v2,
	                     const std::array<uint32_t, 3>& barycentric) {
		const auto p0 = Result(spv::OpVectorTimesScalar, vec4_float_type, v0, barycentric[0]);
		const auto p1 = Result(spv::OpVectorTimesScalar, vec4_float_type, v1, barycentric[1]);
		const auto p2 = Result(spv::OpVectorTimesScalar, vec4_float_type, v2, barycentric[2]);
		return Result(spv::OpFAdd, vec4_float_type, p0,
		              Result(spv::OpFAdd, vec4_float_type, p1, p2));
	}

	Builder                       builder {SpirvVersion15};
	const std::vector<Parameter>& parameters;
	std::vector<uint32_t>         interfaces;
	std::vector<uint32_t>         inputs;
	std::vector<uint32_t>         outputs;
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
	RectListEmitter control(parameters, spv::ExecutionModelTessellationControl);
	RectListEmitter evaluation(parameters, spv::ExecutionModelTessellationEvaluation);
	return {control.EmitControl(), evaluation.EmitEvaluation()};
}

} // namespace Libs::Graphics
