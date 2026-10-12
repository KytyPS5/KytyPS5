#include "graphics/shader/triangleVertexValueShader.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <array>
#include <bit>
#include <cstdint>
#include <utility>

namespace Libs::Graphics {

namespace {

using ShaderRecompiler::Spirv::Builder;
namespace IR = ShaderRecompiler::IR;

constexpr uint32_t SpirvVersion15 = 0x00010500u;

// A vertex shader output the pixel shader reads.
struct Parameter {
	uint32_t input_location  = 0; // of the vertex shader output
	uint32_t output_location = 0; // of the pixel shader input
	bool     flat            = false;
};

// A vertex shader output whose value at each vertex the pixel shader reads.
struct VertexValue {
	uint32_t                input_location = 0;
	bool                    exported       = false; // false: the vertex shader does not write it
	std::array<uint32_t, 3> output_locations {};
};

struct Interface {
	std::vector<Parameter>   parameters;
	std::vector<VertexValue> vertex_values;
	uint32_t                 clip_distances = 0;
	bool                     provoking_last = false;
};

Interface GetInterface(const ShaderVertexInputInfo& vertex_info,
                       const ShaderPixelInputInfo& pixel_info, bool provoking_vertex_last) {
	EXIT_IF(pixel_info.input_num > ShaderVertexInputInfo::RES_MAX);
	EXIT_IF(pixel_info.stage.program == nullptr || vertex_info.stage.program == nullptr);
	const auto& vertex_program = *vertex_info.stage.program;

	for (const auto& output: vertex_program.info.outputs) {
		// These would have to pass through both shaders too. No draw has needed it yet.
		EXIT_NOT_IMPLEMENTED(output.kind == IR::StageOutputKind::PointSize);
		EXIT_NOT_IMPLEMENTED(output.kind == IR::StageOutputKind::CullDistance);
		EXIT_NOT_IMPLEMENTED(output.kind == IR::StageOutputKind::Layer);
		EXIT_NOT_IMPLEMENTED(output.kind == IR::StageOutputKind::ViewportIndex);
	}

	std::vector<uint32_t> active_inputs;
	for (const auto& input: pixel_info.stage.program->info.inputs) {
		if (input.kind == IR::StageInputKind::Parameter) {
			active_inputs.push_back(input.location);
		}
	}

	Interface interface;
	interface.clip_distances =
	    ShaderRecompiler::Spirv::VertexClipDistanceCount(vertex_program.stage, vertex_program.info);
	interface.provoking_last = provoking_vertex_last;
	uint32_t used            = 0;
	uint32_t per_vertex      = 0;
	for (const auto& input: pixel_info.stage.program->info.inputs) {
		if (input.kind != IR::StageInputKind::Parameter) {
			continue;
		}
		const auto input_location = ShaderPixelParameterMappedLocation(pixel_info, input.location);
		const auto output_location =
		    ShaderPixelParameterLocation(pixel_info, active_inputs, input.location);
		const bool exported = (vertex_program.param_export_mask & (1u << input_location)) != 0;
		const auto bit      = 1u << output_location;
		if (exported && (used & bit) == 0u) {
			interface.parameters.push_back(
			    {input_location, output_location,
			     ShaderPixelParameterIsFlat(pixel_info, input.location)});
		}
		if (input.per_vertex && (per_vertex & bit) == 0u) {
			interface.vertex_values.push_back({input_location, exported, {output_location}});
		}
		used |= bit;
		per_vertex |= input.per_vertex ? bit : 0u;
	}
	for (auto& value: interface.vertex_values) {
		value.output_locations =
		    ShaderPixelVertexValueLocations(used, per_vertex, value.output_locations[0]);
	}
	return interface;
}

class Emitter {
public:
	Emitter(const Interface& interface_, spv::ExecutionModel model_)
	    : interface(interface_), control(model_ == spv::ExecutionModelTessellationControl) {
		builder.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);

		void_type       = builder.Type(spv::OpTypeVoid);
		bool_type       = builder.Type(spv::OpTypeBool);
		uint_type       = builder.Type(spv::OpTypeInt, 32u, 0u);
		int_type        = builder.Type(spv::OpTypeInt, 32u, 1u);
		float_type      = builder.Type(spv::OpTypeFloat, 32u);
		vec3_float_type = builder.Type(spv::OpTypeVector, float_type, 3u);
		vec4_float_type = builder.Type(spv::OpTypeVector, float_type, 4u);
		function_type   = builder.Type(spv::OpTypeFunction, void_type);

		if (interface.clip_distances != 0) {
			clip_type       = Array(float_type, interface.clip_distances);
			per_vertex_type = builder.DecoratedType(
			    spv::OpTypeStruct,
			    {{spv::OpMemberDecorate, {0u, spv::DecorationBuiltIn, spv::BuiltInPosition}},
			     {spv::OpMemberDecorate, {1u, spv::DecorationBuiltIn, spv::BuiltInClipDistance}},
			     {spv::OpDecorate, {spv::DecorationBlock}}},
			    vec4_float_type, clip_type);
		} else {
			per_vertex_type = builder.DecoratedType(
			    spv::OpTypeStruct,
			    {{spv::OpMemberDecorate, {0u, spv::DecorationBuiltIn, spv::BuiltInPosition}},
			     {spv::OpDecorate, {spv::DecorationBlock}}},
			    vec4_float_type);
		}
	}

	std::vector<uint32_t> EmitControl() {
		DefineEntry();
		const auto float_one  = Constant(float_type, std::bit_cast<uint32_t>(1.0f));
		const auto float_zero = Constant(float_type, std::bit_cast<uint32_t>(0.0f));
		const auto zero = builder.Constant(spv::OpConstantComposite, vec4_float_type, float_zero,
		                                   float_zero, float_zero, float_zero);
		// Level 1 on every edge and inside: the patch stays one triangle.
		for (uint32_t i = 0; i < 4; i++) {
			Store(Access(Pointer(spv::StorageClassOutput, float_type), tess_outer, Int(i)),
			      float_one);
		}
		for (uint32_t i = 0; i < 2; i++) {
			Store(Access(Pointer(spv::StorageClassOutput, float_type), tess_inner, Int(i)),
			      float_one);
		}

		const auto invocation = Load(int_type, invocation_id);
		CopyBuiltins(invocation, gl_out, invocation);
		const auto provoking = Int(interface.provoking_last ? 2u : 0u);
		for (uint32_t i = 0; i < interface.parameters.size(); i++) {
			// A flat input has the value of the provoking vertex at every vertex, so that the
			// order in which the tessellator emits the vertices does not matter.
			const auto vertex = interface.parameters[i].flat ? provoking : invocation;
			Store(Access(OutputVec4(), outputs[i], invocation),
			      Load(vec4_float_type, Access(InputVec4(), inputs[i], vertex)));
		}
		for (uint32_t i = 0; i < interface.vertex_values.size(); i++) {
			const auto value =
			    interface.vertex_values[i].exported
			        ? Load(vec4_float_type, Access(InputVec4(), value_inputs[i], invocation))
			        : zero;
			Store(Access(OutputVec4(), value_outputs[i][0], invocation), value);
		}

		builder.AddFunction(spv::OpReturn);
		builder.AddFunction(spv::OpFunctionEnd);
		return builder.Build();
	}

	std::vector<uint32_t> EmitEvaluation() {
		DefineEntry();
		// At level 1 the three vertices are the corners of the patch: (1,0,0), (0,1,0), (0,0,1).
		const auto half = Constant(float_type, std::bit_cast<uint32_t>(0.5f));
		const auto is   = [&](uint32_t component) {
			const auto value = Load(float_type, Access(Pointer(spv::StorageClassInput, float_type),
			                                           tess_coord, Int(component)));
			return Result(spv::OpFOrdGreaterThan, bool_type, value, half);
		};
		const auto index = Result(spv::OpSelect, int_type, is(1), Int(1),
		                          Result(spv::OpSelect, int_type, is(2), Int(2), Int(0)));

		CopyBuiltins(index, gl_out, 0);
		for (uint32_t i = 0; i < interface.parameters.size(); i++) {
			Store(outputs[i], Load(vec4_float_type, Access(InputVec4(), inputs[i], index)));
		}
		for (uint32_t i = 0; i < interface.vertex_values.size(); i++) {
			for (uint32_t vertex = 0; vertex < 3u; vertex++) {
				Store(value_outputs[i][vertex],
				      Load(vec4_float_type, Access(InputVec4(), value_inputs[i], Int(vertex))));
			}
		}

		builder.AddFunction(spv::OpReturn);
		builder.AddFunction(spv::OpFunctionEnd);
		return builder.Build();
	}

private:
	uint32_t Constant(uint32_t type, uint32_t value) {
		return builder.Constant(spv::OpConstant, type, value);
	}

	uint32_t Pointer(spv::StorageClass storage, uint32_t type) {
		return builder.Type(spv::OpTypePointer, storage, type);
	}

	uint32_t InputVec4() { return Pointer(spv::StorageClassInput, vec4_float_type); }

	uint32_t OutputVec4() { return Pointer(spv::StorageClassOutput, vec4_float_type); }

	uint32_t Array(uint32_t type, uint32_t size) {
		return builder.Type(spv::OpTypeArray, type, Constant(uint_type, size));
	}

	template <typename... Args>
	uint32_t Result(spv::Op opcode, uint32_t type, Args... operands) {
		const auto id = builder.AllocateId();
		builder.AddFunction(opcode, type, id, operands...);
		return id;
	}

	template <typename... Args>
	uint32_t Access(uint32_t pointer_type, uint32_t base, Args... indices) {
		return Result(spv::OpAccessChain, pointer_type, base, indices...);
	}

	uint32_t Load(uint32_t type, uint32_t pointer) { return Result(spv::OpLoad, type, pointer); }

	void Store(uint32_t pointer, uint32_t value) {
		builder.AddFunction(spv::OpStore, pointer, value);
	}

	uint32_t Int(uint32_t value) { return Constant(int_type, value); }

	uint32_t AddInterface(spv::StorageClass storage, uint32_t type) {
		const auto variable = builder.DefineGlobalVariable(Pointer(storage, type), storage);
		interfaces.push_back(variable);
		return variable;
	}

	uint32_t AddLocation(spv::StorageClass storage, uint32_t type, uint32_t location) {
		const auto variable = AddInterface(storage, type);
		builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationLocation, location);
		return variable;
	}

	// Position and clip distances of input vertex `from`, to the output vertex. `to_index` is the
	// invocation in the control shader and unused in the evaluation shader.
	void CopyBuiltins(uint32_t from, uint32_t to, uint32_t to_index) {
		const auto Target = [&](uint32_t type, uint32_t member) {
			return control
			           ? Access(Pointer(spv::StorageClassOutput, type), to, to_index, Int(member))
			           : Access(Pointer(spv::StorageClassOutput, type), to, Int(member));
		};
		Store(Target(vec4_float_type, 0),
		      Load(vec4_float_type, Access(InputVec4(), gl_in, from, Int(0))));
		if (interface.clip_distances != 0) {
			Store(Target(clip_type, 1),
			      Load(clip_type,
			           Access(Pointer(spv::StorageClassInput, clip_type), gl_in, from, Int(1))));
		}
	}

	void DefineEntry() {
		builder.RequireCapability(spv::CapabilityShader);
		builder.RequireCapability(spv::CapabilityTessellation);
		if (interface.clip_distances != 0) {
			builder.RequireCapability(spv::CapabilityClipDistance);
		}
		const auto model = control ? spv::ExecutionModelTessellationControl
		                           : spv::ExecutionModelTessellationEvaluation;
		main = Result(spv::OpFunction, void_type, spv::FunctionControlMaskNone, function_type);
		if (control) {
			builder.AddExecutionMode(main, spv::ExecutionModeOutputVertices, 3u);
		} else {
			builder.AddExecutionMode(main, spv::ExecutionModeTriangles);
			builder.AddExecutionMode(main, spv::ExecutionModeSpacingEqual);
			// With the upper-left tessellation domain origin of Vulkan, this keeps the winding
			// of the three control points.
			builder.AddExecutionMode(main, spv::ExecutionModeVertexOrderCw);
		}
		DefineInputs();
		DefineOutputs();
		builder.AddEntryPoint(model, main, "main", interfaces);
		const auto label = builder.AllocateId();
		builder.AddFunction(spv::OpLabel, label);
	}

	void DefineInputs() {
		if (control) {
			invocation_id = AddInterface(spv::StorageClassInput, int_type);
			builder.AddAnnotation(spv::OpDecorate, invocation_id, spv::DecorationBuiltIn,
			                      spv::BuiltInInvocationId);
		} else {
			tess_coord = AddInterface(spv::StorageClassInput, vec3_float_type);
			builder.AddAnnotation(spv::OpDecorate, tess_coord, spv::DecorationBuiltIn,
			                      spv::BuiltInTessCoord);
		}
		// The evaluation shader may declare up to gl_MaxPatchVertices inputs; three are written.
		gl_in = AddInterface(spv::StorageClassInput, Array(per_vertex_type, control ? 3u : 32u));

		// The control shader reads the vertex shader's outputs, and two pixel shader inputs may
		// read the same one. The evaluation shader reads what the control shader wrote.
		const auto                                           array = Array(vec4_float_type, 3u);
		std::array<uint32_t, ShaderVertexInputInfo::RES_MAX> by_location {};
		const auto                                           Input = [&](uint32_t location) {
			if (by_location[location] == 0) {
				by_location[location] = AddLocation(spv::StorageClassInput, array, location);
			}
			return by_location[location];
		};
		for (const auto& parameter: interface.parameters) {
			inputs.push_back(Input(control ? parameter.input_location : parameter.output_location));
		}
		for (const auto& value: interface.vertex_values) {
			value_inputs.push_back(!control         ? Input(value.output_locations[0])
			                       : value.exported ? Input(value.input_location)
			                                        : 0u);
		}
	}

	void DefineOutputs() {
		if (control) {
			gl_out     = AddInterface(spv::StorageClassOutput, Array(per_vertex_type, 3u));
			tess_inner = AddInterface(spv::StorageClassOutput, Array(float_type, 2u));
			builder.AddAnnotation(spv::OpDecorate, tess_inner, spv::DecorationBuiltIn,
			                      spv::BuiltInTessLevelInner);
			builder.AddAnnotation(spv::OpDecorate, tess_inner, spv::DecorationPatch);
			tess_outer = AddInterface(spv::StorageClassOutput, Array(float_type, 4u));
			builder.AddAnnotation(spv::OpDecorate, tess_outer, spv::DecorationBuiltIn,
			                      spv::BuiltInTessLevelOuter);
			builder.AddAnnotation(spv::OpDecorate, tess_outer, spv::DecorationPatch);
		} else {
			gl_out = AddInterface(spv::StorageClassOutput, per_vertex_type);
		}
		const auto type = control ? Array(vec4_float_type, 3u) : vec4_float_type;
		for (const auto& parameter: interface.parameters) {
			outputs.push_back(
			    AddLocation(spv::StorageClassOutput, type, parameter.output_location));
		}
		for (const auto& value: interface.vertex_values) {
			// The control shader passes each vertex's value on at the first of the three
			// locations. The evaluation shader writes all three.
			std::array<uint32_t, 3> variables {};
			for (uint32_t vertex = 0; vertex < (control ? 1u : 3u); vertex++) {
				variables[vertex] =
				    AddLocation(spv::StorageClassOutput, type, value.output_locations[vertex]);
			}
			value_outputs.push_back(variables);
		}
	}

	Builder                              builder {SpirvVersion15};
	const Interface&                     interface;
	bool                                 control = false;
	std::vector<uint32_t>                interfaces;
	std::vector<uint32_t>                inputs;
	std::vector<uint32_t>                outputs;
	std::vector<uint32_t>                value_inputs;
	std::vector<std::array<uint32_t, 3>> value_outputs;
	uint32_t                             main            = 0;
	uint32_t                             void_type       = 0;
	uint32_t                             bool_type       = 0;
	uint32_t                             uint_type       = 0;
	uint32_t                             int_type        = 0;
	uint32_t                             float_type      = 0;
	uint32_t                             vec3_float_type = 0;
	uint32_t                             vec4_float_type = 0;
	uint32_t                             function_type   = 0;
	uint32_t                             clip_type       = 0;
	uint32_t                             per_vertex_type = 0;
	uint32_t                             gl_in           = 0;
	uint32_t                             gl_out          = 0;
	uint32_t                             tess_inner      = 0;
	uint32_t                             tess_outer      = 0;
	uint32_t                             tess_coord      = 0;
	uint32_t                             invocation_id   = 0;
};

} // namespace

bool ShaderPixelReadsVertexValues(const ShaderPixelInputInfo& pixel_info) {
	EXIT_IF(pixel_info.stage.program == nullptr);
	for (const auto& input: pixel_info.stage.program->info.inputs) {
		if (input.kind == IR::StageInputKind::Parameter && input.per_vertex) {
			return true;
		}
	}
	return false;
}

TriangleVertexValueShaders BuildTriangleVertexValueShaders(const ShaderVertexInputInfo& vertex_info,
                                                           const ShaderPixelInputInfo&  pixel_info,
                                                           bool provoking_vertex_last) {
	const auto interface = GetInterface(vertex_info, pixel_info, provoking_vertex_last);
	Emitter    control(interface, spv::ExecutionModelTessellationControl);
	Emitter    evaluation(interface, spv::ExecutionModelTessellationEvaluation);
	return {control.EmitControl(), evaluation.EmitEvaluation()};
}

} // namespace Libs::Graphics
