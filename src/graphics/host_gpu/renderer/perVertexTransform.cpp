#include "graphics/host_gpu/renderer/perVertexTransform.h"

#include "common/file.h"
#include "common/threads.h"
#include "kytyGitVersion.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fmt/format.h>
#include <map>
#include <regex>
#include <set>
#include <spirv/unified1/spirv.hpp>
#include <sstream>
#include <string>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

// Older MSVC STL always matches ^/$ at line boundaries, but has no multiline
// flag. The macro arrived with the implementation of the standard option.
#if defined(_MSVC_STL_VERSION) && !defined(_REGEX_LEGACY_MULTILINE_MODE)
static constexpr auto kMultilineRegex = std::regex_constants::ECMAScript;
#else
static constexpr auto kMultilineRegex =
    std::regex_constants::ECMAScript | std::regex_constants::multiline;
#endif

// The transformed modules are assembled as SPIR-V 1.5. Since 1.4, the entry
// interface must include all used globals, including resources and Private I/O.
// Source shaders can be 1.3, whose interfaces only list Input/Output variables.
static std::string CompleteEntryPointInterface(const std::string& source) {
	std::istringstream lines(source);
	std::string        line, interfaces;
	while (std::getline(lines, line)) {
		std::istringstream tokens(line);
		std::string        id, equals, opcode;
		tokens >> id >> equals >> opcode;
		if (opcode == "OpFunction") break;
		if (equals == "=" && opcode == "OpVariable") interfaces += " " + id;
	}
	const std::regex entry_re(R"((OpEntryPoint\s+\S+\s+%\S+\s+"[^"]*")[^\r\n]*)");
	return std::regex_replace(source, entry_re, "$1" + interfaces);
}

bool DerivePerVertexLayout(const std::string& vs_source, const std::string& ps_source,
                           PerVertexLayout&                 layout,
                           std::map<uint32_t, std::string>& vs_param_vars) {
	std::regex var_re(R"(^\s*(%\S+)\s*=\s*OpVariable\s+%\S+\s+Output\s*$)", kMultilineRegex);
	std::set<std::string> vs_outputs;
	for (auto it = std::sregex_iterator(vs_source.begin(), vs_source.end(), var_re);
	     it != std::sregex_iterator(); ++it) {
		vs_outputs.insert((*it)[1]);
	}
	if (vs_outputs.empty()) return false;

	bool has_clip = vs_outputs.find("%gl_ClipDistance") != vs_outputs.end();

	vs_param_vars.clear();
	for (const auto& var: vs_outputs) {
		if (var == "%outPerVertex" || var == "%gl_ClipDistance" || var == "%gl_PointSize") continue;
		std::regex  loc_re(R"(^\s*OpDecorate\s+)" + var + R"(\s+Location\s+(\d+)\s*$)",
		                   kMultilineRegex);
		std::smatch m;
		if (std::regex_search(vs_source, m, loc_re)) {
			uint32_t loc       = static_cast<uint32_t>(std::stoul(m[1]));
			vs_param_vars[loc] = var;
		}
	}

	layout.has_clip   = has_clip;
	layout.num_params = static_cast<uint32_t>(vs_param_vars.size());
	layout.param_locations_and_slots.clear();
	layout.location_to_slot.clear();

	uint32_t current_slot = 1;
	for (const auto& [loc, var_name]: vs_param_vars) {
		layout.location_to_slot[loc] = current_slot;
		layout.param_locations_and_slots.push_back({loc, current_slot});
		current_slot++;
	}

	if (has_clip) {
		layout.clip_slot          = current_slot;
		layout.record_stride_vec4 = current_slot + 1;
	} else {
		layout.clip_slot          = UINT32_MAX;
		layout.record_stride_vec4 = current_slot;
	}
	if (layout.location_to_slot.find(31) != layout.location_to_slot.end()) {
		return false;
	}

	std::regex pv_re(R"(^\s*OpDecorate\s+(%\S+)\s+PerVertexKHR\s*$)", kMultilineRegex);
	std::vector<std::string> pv_vars;
	for (auto it = std::sregex_iterator(ps_source.begin(), ps_source.end(), pv_re);
	     it != std::sregex_iterator(); ++it) {
		pv_vars.push_back((*it)[1]);
	}
	if (pv_vars.empty()) return false;

	for (const auto& pv_var: pv_vars) {
		std::regex  loc_re(R"(^\s*OpDecorate\s+)" + pv_var + R"(\s+Location\s+(\d+)\s*$)",
		                   kMultilineRegex);
		std::smatch m;
		if (!std::regex_search(ps_source, m, loc_re)) {
			return false;
		}
		uint32_t loc = static_cast<uint32_t>(std::stoul(m[1]));
		auto     it  = layout.location_to_slot.find(loc);
		if (it == layout.location_to_slot.end()) {
			return false;
		}
		uint32_t slot = it->second;
		if (slot >= layout.clip_slot) {
			return false;
		}
		if (slot >= layout.record_stride_vec4) {
			return false;
		}
	}
	return true;
}

std::string GenerateReplayVertexSpvasm(const PerVertexLayout& layout) {
	std::vector<std::string> entry_interfaces = {"%gl_VertexIndex", "%_", "%capture_output"};
	for (const auto& [loc, _]: layout.param_locations_and_slots) {
		entry_interfaces.push_back("%out_param_" + std::to_string(loc));
	}
	entry_interfaces.push_back("%out_primitive_id");

	std::string entry_iface_str;
	for (size_t i = 0; i < entry_interfaces.size(); ++i) {
		if (i > 0) entry_iface_str += " ";
		entry_iface_str += entry_interfaces[i];
	}

	std::ostringstream ss;
	ss << "OpCapability Shader\n";
	if (layout.has_clip) {
		ss << "OpCapability ClipDistance\n";
	}
	ss << "OpMemoryModel Logical GLSL450\n";
	ss << "OpEntryPoint Vertex %main \"main\" " << entry_iface_str << "\n";
	ss << "OpDecorate %gl_VertexIndex BuiltIn VertexIndex\n";
	ss << "OpDecorate %gl_PerVertex Block\n";
	ss << "OpMemberDecorate %gl_PerVertex 0 BuiltIn Position\n";
	ss << "OpMemberDecorate %gl_PerVertex 1 BuiltIn PointSize\n";
	if (layout.has_clip) {
		ss << "OpMemberDecorate %gl_PerVertex 2 BuiltIn ClipDistance\n";
	}
	ss << "OpDecorate %_runtimearr_v4float ArrayStride 16\n";
	ss << "OpDecorate %CaptureOutput Block\n";
	ss << "OpMemberDecorate %CaptureOutput 0 NonWritable\n";
	ss << "OpMemberDecorate %CaptureOutput 0 Offset 0\n";
	ss << "OpDecorate %capture_output NonWritable\n";
	ss << "OpDecorate %capture_output Binding 1\n";
	ss << "OpDecorate %capture_output DescriptorSet 1\n";
	for (const auto& [loc, _]: layout.param_locations_and_slots) {
		ss << "OpDecorate %out_param_" << loc << " Location " << loc << "\n";
	}
	ss << "OpDecorate %out_primitive_id Flat\n";
	ss << "OpDecorate %out_primitive_id Location 31\n";
	ss << "%void = OpTypeVoid\n";
	ss << "%fn = OpTypeFunction %void\n";
	ss << "%uint = OpTypeInt 32 0\n";
	ss << "%int = OpTypeInt 32 1\n";
	ss << "%float = OpTypeFloat 32\n";
	ss << "%v4float = OpTypeVector %float 4\n";
	ss << "%uint_0 = OpConstant %uint 0\n";
	ss << "%uint_1 = OpConstant %uint 1\n";
	ss << "%uint_3 = OpConstant %uint 3\n";
	ss << "%int_0 = OpConstant %int 0\n";
	ss << "%int_2 = OpConstant %int 2\n";
	ss << "%stride_const = OpConstant %uint " << layout.record_stride_vec4 << "\n";
	for (const auto& [loc, slot]: layout.param_locations_and_slots) {
		ss << "%slot_const_" << slot << " = OpConstant %uint " << slot << "\n";
	}
	if (layout.has_clip) {
		ss << "%clip_slot_const = OpConstant %uint " << layout.clip_slot << "\n";
		ss << "%_arr_float_1 = OpTypeArray %float %uint_1\n";
		ss << "%_ptr_StorageBuffer_float = OpTypePointer StorageBuffer %float\n";
		ss << "%_ptr_Output_float = OpTypePointer Output %float\n";
		ss << "%gl_PerVertex = OpTypeStruct %v4float %float %_arr_float_1\n";
	} else {
		ss << "%gl_PerVertex = OpTypeStruct %v4float %float\n";
	}
	ss << "%_ptr_Output_gl_PerVertex = OpTypePointer Output %gl_PerVertex\n";
	ss << "%_ = OpVariable %_ptr_Output_gl_PerVertex Output\n";
	ss << "%_ptr_Input_int = OpTypePointer Input %int\n";
	ss << "%gl_VertexIndex = OpVariable %_ptr_Input_int Input\n";
	ss << "%_runtimearr_v4float = OpTypeRuntimeArray %v4float\n";
	ss << "%CaptureOutput = OpTypeStruct %_runtimearr_v4float\n";
	ss << "%_ptr_StorageBuffer_CaptureOutput = OpTypePointer StorageBuffer %CaptureOutput\n";
	ss << "%capture_output = OpVariable %_ptr_StorageBuffer_CaptureOutput StorageBuffer\n";
	ss << "%_ptr_StorageBuffer_v4float = OpTypePointer StorageBuffer %v4float\n";
	ss << "%_ptr_Output_v4float = OpTypePointer Output %v4float\n";
	ss << "%_ptr_Output_uint = OpTypePointer Output %uint\n";
	ss << "%out_primitive_id = OpVariable %_ptr_Output_uint Output\n";
	for (const auto& [loc, _]: layout.param_locations_and_slots) {
		ss << "%out_param_" << loc << " = OpVariable %_ptr_Output_v4float Output\n";
	}

	ss << "%main = OpFunction %void None %fn\n";
	ss << "%label = OpLabel\n";
	ss << "%vidx_i = OpLoad %int %gl_VertexIndex\n";
	ss << "%vidx = OpBitcast %uint %vidx_i\n";
	ss << "%base = OpIMul %uint %vidx %stride_const\n";
	ss << "%pos_ptr = OpAccessChain %_ptr_StorageBuffer_v4float %capture_output %int_0 %base\n";
	ss << "%pos_val = OpLoad %v4float %pos_ptr\n";
	ss << "%out_pos_ptr = OpAccessChain %_ptr_Output_v4float %_ %int_0\n";
	ss << "OpStore %out_pos_ptr %pos_val\n";
	for (const auto& [loc, slot]: layout.param_locations_and_slots) {
		ss << "%param_idx_" << slot << " = OpIAdd %uint %base %slot_const_" << slot << "\n";
		ss << "%param_ptr_" << slot
		   << " = OpAccessChain %_ptr_StorageBuffer_v4float %capture_output %int_0 %param_idx_"
		   << slot << "\n";
		ss << "%param_val_" << slot << " = OpLoad %v4float %param_ptr_" << slot << "\n";
		ss << "OpStore %out_param_" << loc << " %param_val_" << slot << "\n";
	}
	if (layout.has_clip) {
		ss << "%clip_idx = OpIAdd %uint %base %clip_slot_const\n";
		ss << "%clip_src_ptr = OpAccessChain %_ptr_StorageBuffer_float %capture_output %int_0 "
		      "%clip_idx %uint_0\n";
		ss << "%clip_val = OpLoad %float %clip_src_ptr\n";
		ss << "%clip_dst_ptr = OpAccessChain %_ptr_Output_float %_ %int_2 %uint_0\n";
		ss << "OpStore %clip_dst_ptr %clip_val\n";
	}
	ss << "%primid = OpUDiv %uint %vidx %uint_3\n";
	ss << "OpStore %out_primitive_id %primid\n";
	ss << "OpReturn\n";
	ss << "OpFunctionEnd\n";
	return ss.str();
}

std::string LowerVertexToCompute(const std::string& source, const PerVertexLayout& layout,
                                 const std::map<uint32_t, std::string>& vs_param_vars) {
	std::regex            var_re(R"(^\s*(%\S+)\s*=\s*OpVariable\s+%\S+\s+(Input|Output)\s*$)",
	                             kMultilineRegex);
	std::set<std::string> inputs, outputs, attr_inputs;
	for (auto it = std::sregex_iterator(source.begin(), source.end(), var_re);
	     it != std::sregex_iterator(); ++it) {
		std::string name    = (*it)[1];
		std::string storage = (*it)[2];
		if (storage == "Input") {
			inputs.insert(name);
			if (name.rfind("%in_attr_", 0) == 0) attr_inputs.insert(name);
		} else {
			outputs.insert(name);
		}
	}
	std::vector<int> attr_indices;
	for (const auto& a: attr_inputs) {
		attr_indices.push_back(std::stoi(a.substr(9)));
	}
	std::sort(attr_indices.begin(), attr_indices.end());
	uint32_t num_attrs     = static_cast<uint32_t>(attr_indices.size());
	uint32_t output_stride = layout.record_stride_vec4;
	bool     has_clip      = layout.has_clip;

	std::vector<uint32_t>    attr_widths(num_attrs, 1);
	std::vector<std::string> attr_types(num_attrs), attr_scalars(num_attrs);
	for (uint32_t i = 0; i < num_attrs; ++i) {
		std::regex  var_decl(R"(^\s*%in_attr_)" + std::to_string(i) +
		                         R"(\s*=\s*OpVariable\s+(%\S+)\s+Input\s*$)",
		                     kMultilineRegex);
		std::smatch m;
		if (!std::regex_search(source, m, var_decl)) return {};
		const std::string ptr_type = m[1];
		std::regex ptr_re(R"(^\s*)" + ptr_type + R"(\s*=\s*OpTypePointer\s+Input\s+(%\S+)\s*$)",
		                  kMultilineRegex);
		if (!std::regex_search(source, m, ptr_re)) return {};
		attr_types[i]   = m[1];
		attr_scalars[i] = m[1];
		std::regex vec_re(R"(^\s*)" + attr_types[i] +
		                      R"(\s*=\s*OpTypeVector\s+(%\S+)\s+([2-4])\s*$)",
		                  kMultilineRegex);
		if (std::regex_search(source, m, vec_re)) {
			attr_scalars[i] = m[1];
			attr_widths[i]  = std::stoi(m[2]);
		}
		if (attr_scalars[i] != "%float" && attr_scalars[i] != "%uint" && attr_scalars[i] != "%int")
			return {};
	}

	std::regex  main_re(R"(^\s*OpEntryPoint\s+Vertex\s+(%\S+)\s+"main".*$)", kMultilineRegex);
	std::smatch m_main;
	if (!std::regex_search(source, m_main, main_re)) return {};
	std::string main_name = m_main[1];

	std::string modified = std::regex_replace(source, main_re,
	                                          "OpEntryPoint GLCompute " + main_name +
	                                              " \"main\" %capture_global_id %capture_input "
	                                              "%capture_output %capture_ids\nOpExecutionMode " +
	                                              main_name + " LocalSize 1 1 1");

	std::regex ptr_replace(R"((OpTypePointer\s+)(Input|Output)(\s+))");
	modified = std::regex_replace(modified, ptr_replace, "$1Private$3");

	std::regex var_replace(R"((OpVariable\s+%\S+\s+)(Input|Output)(\s*$))", kMultilineRegex);
	modified = std::regex_replace(modified, var_replace, "$1Private$3");

	std::istringstream       iss(modified);
	std::string              line;
	std::vector<std::string> lines;
	std::set<std::string>    all_interface = inputs;
	all_interface.insert(outputs.begin(), outputs.end());

	while (std::getline(iss, line)) {
		bool skip = false;
		if (line.find("OpDecorate ") != std::string::npos) {
			for (const auto& var: all_interface) {
				if (line.find("OpDecorate " + var) != std::string::npos) {
					skip = true;
					break;
				}
			}
		}
		if (line.find("OpMemberDecorate %gl_PerVertex") != std::string::npos) skip = true;
		if (line.find("OpDecorate %gl_PerVertex Block") != std::string::npos) skip = true;
		if (line.find("OpCapability ClipDistance") != std::string::npos) skip = true;
		if (!skip) lines.push_back(line);
	}

	std::vector<std::string> annotations = {
	    "OpDecorate %capture_global_id BuiltIn GlobalInvocationId",
	    "OpDecorate %capture_array ArrayStride 16",
	    "OpMemberDecorate %capture_block 0 Offset 0",
	    "OpDecorate %capture_block Block",
	    "OpDecorate %capture_input DescriptorSet 1",
	    "OpDecorate %capture_input Binding 0",
	    "OpDecorate %capture_input NonWritable",
	    "OpDecorate %capture_output DescriptorSet 1",
	    "OpDecorate %capture_output Binding 1",
	    "OpDecorate %capture_ids_array ArrayStride 8",
	    "OpMemberDecorate %capture_ids_block 0 Offset 0",
	    "OpDecorate %capture_ids_block Block",
	    "OpDecorate %capture_ids DescriptorSet 1",
	    "OpDecorate %capture_ids Binding 2",
	    "OpDecorate %capture_ids NonWritable"};

	const char* named_constants[] = {"zero", "one", "two", "three", "four", "five", "six"};
	auto        get_const         = [&](int k) -> std::string {
		if (k < 7) return std::string("%capture_") + named_constants[k];
		return "%capture_const_" + std::to_string(k);
	};

	std::vector<std::string> declarations = {
	    "%capture_uvec3 = OpTypeVector %uint 3",
	    "%capture_id_pointer = OpTypePointer Input %capture_uvec3",
	    "%capture_global_id = OpVariable %capture_id_pointer Input",
	    "%capture_array = OpTypeRuntimeArray %v4float",
	    "%capture_block = OpTypeStruct %capture_array",
	    "%capture_buffer_pointer = OpTypePointer StorageBuffer %capture_block",
	    "%capture_input = OpVariable %capture_buffer_pointer StorageBuffer",
	    "%capture_output = OpVariable %capture_buffer_pointer StorageBuffer",
	    "%capture_uvec2 = OpTypeVector %uint 2",
	    "%capture_ids_array = OpTypeRuntimeArray %capture_uvec2",
	    "%capture_ids_block = OpTypeStruct %capture_ids_array",
	    "%capture_ids_pointer = OpTypePointer StorageBuffer %capture_ids_block",
	    "%capture_ids = OpVariable %capture_ids_pointer StorageBuffer",
	    "%capture_ids_element_pointer = OpTypePointer StorageBuffer %capture_uvec2",
	    "%capture_element_pointer = OpTypePointer StorageBuffer %v4float",
	    "%capture_private_element_pointer = OpTypePointer Private %v4float"};

	int max_const = std::max({6u, num_attrs, output_stride + 1u});
	for (int k = 0; k <= max_const; ++k) {
		declarations.push_back(get_const(k) + " = OpConstant %uint " + std::to_string(k));
	}

	std::string clip_pointer = "%capture_clip_pointer";
	if (has_clip) {
		std::regex  ptr_priv(R"(^\s*(%\S+)\s*=\s*OpTypePointer\s+Private\s+%float\s*$)",
		                     kMultilineRegex);
		std::smatch m_priv;
		if (std::regex_search(modified, m_priv, ptr_priv)) {
			clip_pointer = m_priv[1];
		} else {
			declarations.push_back(clip_pointer + " = OpTypePointer Private %float");
		}
	}
	declarations.push_back("%capture_zero_float = OpConstant %float 0");

	std::vector<std::string> start = {
	    "%capture_id = OpLoad %capture_uvec3 %capture_global_id",
	    "%capture_invocation = OpCompositeExtract %uint %capture_id 0",
	    "%capture_original_ids_pointer = OpAccessChain %capture_ids_element_pointer %capture_ids "
	    "%capture_zero %capture_invocation",
	    "%capture_original_ids = OpLoad %capture_uvec2 %capture_original_ids_pointer",
	    "%capture_vertex = OpCompositeExtract %uint %capture_original_ids 0",
	    "%capture_instance = OpCompositeExtract %uint %capture_original_ids 1",
	    "%capture_vertex_signed = OpBitcast %int %capture_vertex",
	    "%capture_instance_signed = OpBitcast %int %capture_instance",
	    "OpStore %gl_VertexIndex %capture_vertex_signed",
	    "OpStore %gl_InstanceIndex %capture_instance_signed",
	    "%capture_input_base = OpIMul %uint %capture_invocation " + get_const(num_attrs),
	    "%capture_output_base = OpIMul %uint %capture_invocation " + get_const(output_stride)};

	std::map<std::string, std::string> wide_types {{"%float", "%v4float"}};
	for (uint32_t i = 0; i < num_attrs; ++i) {
		start.push_back("%capture_input_index_" + std::to_string(i) +
		                " = OpIAdd %uint %capture_input_base " + get_const(i));
		start.push_back("%capture_input_ptr_" + std::to_string(i) +
		                " = OpAccessChain %capture_element_pointer %capture_input %capture_zero "
		                "%capture_input_index_" +
		                std::to_string(i));
		start.push_back("%capture_attribute_" + std::to_string(i) +
		                " = OpLoad %v4float %capture_input_ptr_" + std::to_string(i));
		std::string value = "%capture_attribute_" + std::to_string(i);
		if (attr_scalars[i] != "%float") {
			auto [type, inserted] =
			    wide_types.try_emplace(attr_scalars[i], "%capture_v4" + attr_scalars[i].substr(1));
			if (inserted) {
				std::regex  wide_re(R"(^\s*(%\S+)\s*=\s*OpTypeVector\s+)" + attr_scalars[i] +
				                        R"(\s+4\s*$)",
				                    kMultilineRegex);
				std::smatch match;
				if (std::regex_search(source, match, wide_re))
					type->second = match[1];
				else
					declarations.push_back(type->second + " = OpTypeVector " + attr_scalars[i] +
					                       " 4");
			}
			const std::string typed = "%capture_typed_" + std::to_string(i);
			start.push_back(typed + " = OpBitcast " + type->second + " " + value);
			value = typed;
		}
		uint32_t w = attr_widths[i];
		if (w == 1) {
			start.push_back("%capture_decoded_" + std::to_string(i) + " = OpCompositeExtract " +
			                attr_types[i] + " " + value + " 0");
			start.push_back("OpStore %in_attr_" + std::to_string(i) + " %capture_decoded_" +
			                std::to_string(i));
		} else if (w < 4) {
			std::string comps;
			for (uint32_t c = 0; c < w; ++c)
				comps += " " + std::to_string(c);
			start.push_back("%capture_decoded_" + std::to_string(i) + " = OpVectorShuffle " +
			                attr_types[i] + " " + value + " " + value + comps);
			start.push_back("OpStore %in_attr_" + std::to_string(i) + " %capture_decoded_" +
			                std::to_string(i));
		} else {
			start.push_back("OpStore %in_attr_" + std::to_string(i) + " " + value);
		}
	}

	std::vector<std::string> end = {
	    "%capture_position_ptr = OpAccessChain %capture_private_element_pointer %outPerVertex "
	    "%capture_zero",
	    "%capture_position = OpLoad %v4float %capture_position_ptr",
	    "%capture_output_ptr_0 = OpAccessChain %capture_element_pointer %capture_output "
	    "%capture_zero %capture_output_base",
	    "OpStore %capture_output_ptr_0 %capture_position"};

	for (const auto& [loc, slot]: layout.param_locations_and_slots) {
		const auto& var_name = vs_param_vars.at(loc);
		end.push_back("%capture_parameter_" + std::to_string(slot) + " = OpLoad %v4float " +
		              var_name);
		end.push_back("%capture_output_index_" + std::to_string(slot) +
		              " = OpIAdd %uint %capture_output_base " + get_const(slot));
		end.push_back("%capture_output_ptr_" + std::to_string(slot) +
		              " = OpAccessChain %capture_element_pointer %capture_output %capture_zero "
		              "%capture_output_index_" +
		              std::to_string(slot));
		end.push_back("OpStore %capture_output_ptr_" + std::to_string(slot) +
		              " %capture_parameter_" + std::to_string(slot));
	}

	if (has_clip) {
		uint32_t clip_slot = layout.clip_slot;
		end.push_back("%capture_clip_element = OpAccessChain " + clip_pointer +
		              " %gl_ClipDistance %capture_zero");
		end.push_back("%capture_clip_distance = OpLoad %float %capture_clip_element");
		end.push_back("%capture_clip_vector = OpCompositeConstruct %v4float %capture_clip_distance "
		              "%capture_zero_float %capture_zero_float %capture_zero_float");
		end.push_back("%capture_output_index_" + std::to_string(clip_slot) +
		              " = OpIAdd %uint %capture_output_base " + get_const(clip_slot));
		end.push_back("%capture_output_ptr_" + std::to_string(clip_slot) +
		              " = OpAccessChain %capture_element_pointer %capture_output %capture_zero "
		              "%capture_output_index_" +
		              std::to_string(clip_slot));
		end.push_back("OpStore %capture_output_ptr_" + std::to_string(clip_slot) +
		              " %capture_clip_vector");
	}

	std::vector<std::string> out_lines;
	bool annotations_done = false, declarations_done = false, entry_label = false,
	     start_done = false;
	for (const auto& l: lines) {
		if (!annotations_done && (l.find("= OpType") != std::string::npos ||
		                          l.find("= OpConstant") != std::string::npos)) {
			out_lines.insert(out_lines.end(), annotations.begin(), annotations.end());
			annotations_done = true;
		}
		if (!declarations_done && l.find("= OpFunction ") != std::string::npos) {
			out_lines.insert(out_lines.end(), declarations.begin(), declarations.end());
			declarations_done = true;
		}
		if (entry_label && !start_done && l.find("= OpVariable ") == std::string::npos) {
			out_lines.insert(out_lines.end(), start.begin(), start.end());
			start_done = true;
		}
		if (declarations_done && l.find("= OpLabel") != std::string::npos) {
			entry_label = true;
		}
		if (l == "OpReturn" || l == "               OpReturn") {
			out_lines.insert(out_lines.end(), end.begin(), end.end());
		}
		out_lines.push_back(l);
	}

	std::ostringstream oss;
	for (const auto& l: out_lines)
		oss << l << "\n";
	std::string res = oss.str();

	for (int width: {2, 3}) {
		std::regex  vec_re(R"(^\s*(%\S+)\s*=\s*OpTypeVector\s+%uint\s+)" + std::to_string(width) +
		                       R"(\s*$)",
		                   kMultilineRegex);
		std::smatch m;
		if (std::regex_search(source, m, vec_re)) {
			std::string orig_id = m[1];
			std::regex  decl_re(R"(%capture_uvec)" + std::to_string(width) +
			                    R"(\s*=\s*OpTypeVector\s+%uint\s+)" + std::to_string(width) +
			                    R"(\n)");
			res = std::regex_replace(res, decl_re, "");
			std::regex use_re(R"(%capture_uvec)" + std::to_string(width));
			res = std::regex_replace(res, use_re, orig_id);
		}
	}
	return CompleteEntryPointInterface(res);
}

std::string LowerFragmentToBufferReplay(const std::string& source, const PerVertexLayout& layout) {
	std::regex pv_re(R"(^\s*OpDecorate\s+(%\S+)\s+PerVertexKHR\s*$)", kMultilineRegex);
	std::vector<std::string> pv_vars;
	for (auto it = std::sregex_iterator(source.begin(), source.end(), pv_re);
	     it != std::sregex_iterator(); ++it) {
		pv_vars.push_back((*it)[1]);
	}
	if (pv_vars.empty()) return {};

	struct VarInfo {
		int         location;
		int         slot;
		std::string uint_type;
		std::string vector_type;
		std::string scalar_type;
		std::string size_const;
	};
	std::map<std::string, VarInfo> var_map;

	for (const auto& var: pv_vars) {
		std::regex  loc_re(R"(^\s*OpDecorate\s+)" + var + R"(\s+Location\s+(\d+)\s*$)",
		                   kMultilineRegex);
		std::smatch m;
		if (!std::regex_search(source, m, loc_re)) return {};
		int  loc     = std::stoi(m[1]);
		auto slot_it = layout.location_to_slot.find(static_cast<uint32_t>(loc));
		if (slot_it == layout.location_to_slot.end()) return {};
		int slot = static_cast<int>(slot_it->second);
		if (static_cast<uint32_t>(slot) >= layout.clip_slot ||
		    static_cast<uint32_t>(slot) >= layout.record_stride_vec4)
			return {};

		std::regex var_re(R"(^\s*)" + var + R"(\s*=\s*OpVariable\s+(%\S+)\s+Input\s*$)",
		                  kMultilineRegex);
		if (!std::regex_search(source, m, var_re)) return {};
		std::string ptr_type = m[1];

		std::regex ptr_re(R"(^\s*)" + ptr_type + R"(\s*=\s*OpTypePointer\s+Input\s+(%\S+)\s*$)",
		                  kMultilineRegex);
		if (!std::regex_search(source, m, ptr_re)) return {};
		std::string arr_type = m[1];

		std::regex arr_re(R"(^\s*)" + arr_type + R"(\s*=\s*OpTypeArray\s+(%\S+)\s+(%\S+)\s*$)",
		                  kMultilineRegex);
		if (!std::regex_search(source, m, arr_re)) return {};
		std::string vec_type   = m[1];
		std::string size_const = m[2];

		std::regex vec_detail(R"(^\s*)" + vec_type + R"(\s*=\s*OpTypeVector\s+(%\S+)\s+4\s*$)",
		                      kMultilineRegex);
		if (!std::regex_search(source, m, vec_detail)) return {};
		std::string scalar_type = m[1];

		std::regex size_detail(R"(^\s*)" + size_const + R"(\s*=\s*OpConstant\s+(%\S+)\s+3\s*$)",
		                       kMultilineRegex);
		if (!std::regex_search(source, m, size_detail)) return {};
		std::string uint_type = m[1];

		var_map[var] = {loc, slot, uint_type, vec_type, scalar_type, size_const};
	}

	const auto& first          = var_map.begin()->second;
	uint32_t    vertex_bytes   = layout.record_stride_vec4 * 16;
	uint32_t    triangle_bytes = 3 * vertex_bytes;

	std::vector<std::string> annotations = {
	    "OpDecorate %capture_vertex ArrayStride 16",
	    "OpDecorate %capture_triangle ArrayStride " + std::to_string(vertex_bytes),
	    "OpDecorate %capture_array ArrayStride " + std::to_string(triangle_bytes),
	    "OpMemberDecorate %capture_block 0 Offset 0",
	    "OpDecorate %capture_block Block",
	    "OpDecorate %capture_buffer DescriptorSet 1",
	    "OpDecorate %capture_buffer Binding 1",
	    "OpDecorate %capture_buffer NonWritable",
	    "OpDecorate %capture_primitive Flat",
	    "OpDecorate %capture_primitive Location 31"};

	std::vector<std::string> declarations = {
	    "%capture_vertex_count = OpConstant " + first.uint_type + " " +
	        std::to_string(layout.record_stride_vec4),
	    "%capture_vertex = OpTypeArray " + first.vector_type + " %capture_vertex_count"};
	std::set<int> distinct_slots;
	for (const auto& [_, v]: var_map)
		distinct_slots.insert(v.slot);
	for (int s: distinct_slots) {
		declarations.push_back("%capture_parameter_slot_" + std::to_string(s) + " = OpConstant " +
		                       first.uint_type + " " + std::to_string(s));
	}
	if (distinct_slots.size() == 1) {
		declarations.push_back("%capture_parameter_slot = OpConstant " + first.uint_type + " " +
		                       std::to_string(*distinct_slots.begin()));
	}
	declarations.insert(
	    declarations.end(),
	    {"%capture_zero = OpConstant " + first.uint_type + " 0",
	     "%capture_triangle = OpTypeArray %capture_vertex " + first.size_const,
	     "%capture_array = OpTypeRuntimeArray %capture_triangle",
	     "%capture_block = OpTypeStruct %capture_array",
	     "%capture_buffer_pointer = OpTypePointer StorageBuffer %capture_block",
	     "%capture_buffer = OpVariable %capture_buffer_pointer StorageBuffer",
	     "%capture_float_pointer = OpTypePointer StorageBuffer " + first.scalar_type,
	     "%capture_id_pointer = OpTypePointer Input " + first.uint_type,
	     "%capture_primitive = OpVariable %capture_id_pointer Input"});

	std::istringstream       iss(source);
	std::string              line;
	std::vector<std::string> out_lines;
	bool                     decorated = false, declared = false;
	int                      accesses = 0;

	std::regex entry_re(R"(^\s*OpEntryPoint\s+Fragment\s+%\S+\s+"main"(.*)$)");

	while (std::getline(iss, line)) {
		if (!decorated && line.find("OpDecorate ") != std::string::npos) {
			out_lines.insert(out_lines.end(), annotations.begin(), annotations.end());
			decorated = true;
		}
		if (!declared && line.find("= OpFunction ") != std::string::npos) {
			out_lines.insert(out_lines.end(), declarations.begin(), declarations.end());
			declared = true;
		}
		std::smatch m_entry;
		if (std::regex_match(line, m_entry, entry_re)) {
			std::string iface = line;
			for (const auto& [v, _]: var_map) {
				size_t pos = iface.find(" " + v);
				if (pos != std::string::npos) {
					iface.erase(pos, v.size() + 1);
				}
			}
			iface += " %capture_primitive %capture_buffer";
			out_lines.push_back(iface);
			continue;
		}
		std::string matched_var = "";
		for (const auto& [v, _]: var_map) {
			std::regex v_re(R"((^|\s))" + v + R"((\s|$))");
			if (std::regex_search(line, v_re)) {
				matched_var = v;
				break;
			}
		}
		if (!matched_var.empty()) {
			if (line.find("OpDecorate " + matched_var) != std::string::npos ||
			    line.find(matched_var + " = OpVariable") != std::string::npos ||
			    line.find("OpName " + matched_var) != std::string::npos) {
				continue;
			}
			std::regex  acc_re(R"(^\s*(%\S+)\s*=\s*OpAccessChain\s+%\S+\s+)" + matched_var +
			                   R"(\s+(%\S+)\s+(%\S+)\s*$)");
			std::smatch m_acc;
			if (std::regex_match(line, m_acc, acc_re)) {
				std::string result_id = m_acc[1];
				std::string corner    = m_acc[2];
				std::string component = m_acc[3];
				std::string prim_id   = "%capture_id_" + std::to_string(accesses++);
				out_lines.push_back(prim_id + " = OpLoad " + first.uint_type +
				                    " %capture_primitive");
				int         slot       = var_map[matched_var].slot;
				std::string slot_const = "%capture_parameter_slot_" + std::to_string(slot);
				out_lines.push_back(
				    result_id +
				    " = OpAccessChain %capture_float_pointer %capture_buffer %capture_zero " +
				    prim_id + " " + corner + " " + slot_const + " " + component);
				continue;
			}
		}
		out_lines.push_back(line);
	}

	std::ostringstream oss;
	for (const auto& l: out_lines)
		oss << l << "\n";
	return CompleteEntryPointInterface(oss.str());
}

static std::atomic<uint64_t> s_pv_temp_counter = 0;

std::string GetPerVertexTransformSignature() {
#ifndef KYTY_PER_VERTEX_TRANSFORM_SIGNATURE
#define KYTY_PER_VERTEX_TRANSFORM_SIGNATURE "none"
#endif
#ifndef KYTY_GIT_REVISION
#define KYTY_GIT_REVISION "unknown"
#endif
	return fmt::format("v2:{}:{}:env=vk1.2:num_ids", KYTY_GIT_REVISION,
	                   KYTY_PER_VERTEX_TRANSFORM_SIGNATURE);
}

bool SaveTransformedShadersToDisk(const std::filesystem::path& cache_dir, uint64_t vs_hash,
                                  uint64_t ps_hash, const PerVertexLayout& layout,
                                  std::span<const uint32_t> cap_words,
                                  std::span<const uint32_t> frag_words,
                                  std::span<const uint32_t> replay_words) {
	if (cache_dir.empty()) return false;

	if (!Common::File::CreateDirectories(cache_dir)) {
		return false;
	}

	const auto filename   = fmt::format("pv_{:016x}_{:016x}.bin", vs_hash, ps_hash);
	const auto final_path = cache_dir / filename;
	const auto temp_path =
	    cache_dir /
	    fmt::format("{}.tmp.{}.{}", filename, Common::Thread::GetProcessId(), s_pv_temp_counter++);

	std::vector<uint8_t> buffer;
	auto                 write_bytes = [&](const void* src, size_t size) {
		const auto* ptr = static_cast<const uint8_t*>(src);
		buffer.insert(buffer.end(), ptr, ptr + size);
	};

	// 1. Magic (KYTYPV02)
	write_bytes(kPerVertexCacheMagic, 8);

	// 2. Format version & transformation signature
	const uint32_t version = kPerVertexTransformVersion;
	write_bytes(&version, sizeof(version));
	const std::string sig     = GetPerVertexTransformSignature();
	const uint32_t    sig_len = static_cast<uint32_t>(sig.size());
	write_bytes(&sig_len, sizeof(sig_len));
	write_bytes(sig.data(), sig.size());

	// 3. Payload Checksum placeholder (will cover all bytes following this 8-byte checksum field)
	const size_t   checksum_offset = buffer.size();
	const uint64_t zero_checksum   = 0;
	write_bytes(&zero_checksum, sizeof(zero_checksum));

	const size_t protected_start_offset = buffer.size();

	// 4. Layout scalar fields
	write_bytes(&layout.num_params, sizeof(layout.num_params));
	const uint8_t has_clip_byte = layout.has_clip ? 1 : 0;
	write_bytes(&has_clip_byte, sizeof(has_clip_byte));
	write_bytes(&layout.clip_slot, sizeof(layout.clip_slot));
	write_bytes(&layout.record_stride_vec4, sizeof(layout.record_stride_vec4));

	// 5. Layout dynamic mappings
	const uint32_t param_count = static_cast<uint32_t>(layout.param_locations_and_slots.size());
	write_bytes(&param_count, sizeof(param_count));
	for (const auto& [loc, slot]: layout.param_locations_and_slots) {
		write_bytes(&loc, sizeof(loc));
		write_bytes(&slot, sizeof(slot));
	}

	// 6. Shader word counts
	const uint32_t cap_count    = static_cast<uint32_t>(cap_words.size());
	const uint32_t frag_count   = static_cast<uint32_t>(frag_words.size());
	const uint32_t replay_count = static_cast<uint32_t>(replay_words.size());
	write_bytes(&cap_count, sizeof(cap_count));
	write_bytes(&frag_count, sizeof(frag_count));
	write_bytes(&replay_count, sizeof(replay_count));

	// 7. Shader bytecodes
	write_bytes(cap_words.data(), cap_words.size_bytes());
	write_bytes(frag_words.data(), frag_words.size_bytes());
	write_bytes(replay_words.data(), replay_words.size_bytes());

	// Compute checksum on all bytes from protected_start_offset to buffer end
	const uint64_t full_checksum =
	    XXH3_64bits(buffer.data() + protected_start_offset, buffer.size() - protected_start_offset);
	std::memcpy(buffer.data() + checksum_offset, &full_checksum, sizeof(full_checksum));

	// Write atomically
	Common::File file;
	uint32_t     bytes_written = 0;
	if (file.Create(temp_path)) {
		file.Write(buffer.data(), static_cast<uint32_t>(buffer.size()), &bytes_written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();

	if (bytes_written != buffer.size() || !flushed ||
	    !Common::File::RenameFile(temp_path, final_path)) {
		if (Common::File::IsFileExisting(temp_path)) {
			std::error_code remove_error;
			std::filesystem::remove(temp_path, remove_error);
		}
		return false;
	}

	return true;
}

bool TryLoadTransformedShadersFromDisk(const std::filesystem::path& cache_dir, uint64_t vs_hash,
                                       uint64_t ps_hash, PerVertexLayout& layout,
                                       std::vector<uint32_t>& cap_words,
                                       std::vector<uint32_t>& frag_words,
                                       std::vector<uint32_t>& replay_words) {
	if (cache_dir.empty()) return false;

	const auto filename = fmt::format("pv_{:016x}_{:016x}.bin", vs_hash, ps_hash);
	const auto path     = cache_dir / filename;

	if (!Common::File::IsFileExisting(path)) return false;

	Common::File file(path, Common::File::Mode::Read);
	if (file.IsInvalid()) return false;
	const auto file_size = file.Size();
	if (file_size < 48 || file_size > 32 * 1024 * 1024) {
		file.Close();
		return false;
	}

	std::vector<uint8_t> buffer(file_size);
	uint32_t             bytes_read = 0;
	file.Read(buffer.data(), static_cast<uint32_t>(file_size), &bytes_read);
	file.Close();
	if (bytes_read != file_size) return false;

	size_t offset     = 0;
	auto   read_bytes = [&](void* dst, size_t size) -> bool {
		if (offset + size > buffer.size()) return false;
		std::memcpy(dst, buffer.data() + offset, size);
		offset += size;
		return true;
	};

	// 1. Magic
	char magic[8];
	if (!read_bytes(magic, 8) || std::memcmp(magic, kPerVertexCacheMagic, 8) != 0) {
		return false;
	}

	// 2. Version
	uint32_t version = 0;
	if (!read_bytes(&version, sizeof(version)) || version != kPerVertexTransformVersion) {
		return false;
	}

	// 3. Transformation signature
	uint32_t sig_len = 0;
	if (!read_bytes(&sig_len, sizeof(sig_len)) || sig_len > 1024) return false;
	std::string file_sig(sig_len, '\0');
	if (!read_bytes(file_sig.data(), sig_len)) return false;
	const std::string expected_sig = GetPerVertexTransformSignature();
	if (file_sig != expected_sig) {
		return false;
	}

	// 4. Full Checksum verification (protects all layout metadata + dynamic mappings + counts +
	// bytecode payload)
	uint64_t expected_checksum = 0;
	if (!read_bytes(&expected_checksum, sizeof(expected_checksum))) return false;

	const size_t   protected_bytes = buffer.size() - offset;
	const uint64_t actual_checksum = XXH3_64bits(buffer.data() + offset, protected_bytes);
	if (actual_checksum != expected_checksum) {
		return false;
	}

	// 5. Layout scalar fields
	PerVertexLayout loaded_layout {};
	if (!read_bytes(&loaded_layout.num_params, sizeof(loaded_layout.num_params))) return false;
	uint8_t has_clip_byte = 0;
	if (!read_bytes(&has_clip_byte, sizeof(has_clip_byte))) return false;
	loaded_layout.has_clip = (has_clip_byte != 0);
	if (!read_bytes(&loaded_layout.clip_slot, sizeof(loaded_layout.clip_slot))) return false;
	if (!read_bytes(&loaded_layout.record_stride_vec4, sizeof(loaded_layout.record_stride_vec4)))
		return false;

	// Sémantique stricte du layout pour interdire toute incohérence (ex: stride altéré produisant
	// un buffer trop petit)
	if (loaded_layout.num_params > 32) return false;
	const uint32_t expected_stride =
	    1 + loaded_layout.num_params + (loaded_layout.has_clip ? 1 : 0);
	if (loaded_layout.record_stride_vec4 != expected_stride) {
		return false;
	}
	const uint32_t expected_clip_slot =
	    loaded_layout.has_clip ? (1 + loaded_layout.num_params) : UINT32_MAX;
	if (loaded_layout.clip_slot != expected_clip_slot) {
		return false;
	}

	// 6. Layout dynamic mappings
	uint32_t param_count = 0;
	if (!read_bytes(&param_count, sizeof(param_count)) || param_count != loaded_layout.num_params)
		return false;
	loaded_layout.param_locations_and_slots.reserve(param_count);
	std::vector<bool> slot_used(loaded_layout.record_stride_vec4, false);
	slot_used[0] = true; // slot 0 reserved for gl_Position
	if (loaded_layout.has_clip) {
		slot_used[loaded_layout.clip_slot] = true;
	}

	for (uint32_t i = 0; i < param_count; ++i) {
		uint32_t loc = 0, slot = 0;
		if (!read_bytes(&loc, sizeof(loc)) || !read_bytes(&slot, sizeof(slot))) return false;
		if (loc == 31) return false;
		if (slot == 0 || slot >= loaded_layout.record_stride_vec4 || slot_used[slot]) {
			return false; // slot hors bornes ou collision
		}
		if (loaded_layout.location_to_slot.find(loc) != loaded_layout.location_to_slot.end()) {
			return false; // location dupliquée
		}
		slot_used[slot] = true;
		loaded_layout.param_locations_and_slots.emplace_back(loc, slot);
		loaded_layout.location_to_slot[loc] = slot;
	}

	// 7. Shader word counts
	uint32_t cap_count = 0, frag_count = 0, replay_count = 0;
	if (!read_bytes(&cap_count, sizeof(cap_count)) || cap_count == 0 || cap_count > 1024 * 1024)
		return false;
	if (!read_bytes(&frag_count, sizeof(frag_count)) || frag_count == 0 || frag_count > 1024 * 1024)
		return false;
	if (!read_bytes(&replay_count, sizeof(replay_count)) || replay_count == 0 ||
	    replay_count > 1024 * 1024)
		return false;

	const size_t remaining_bytes = buffer.size() - offset;
	const size_t expected_bytecode_bytes =
	    (static_cast<size_t>(cap_count) + frag_count + replay_count) * sizeof(uint32_t);
	if (remaining_bytes != expected_bytecode_bytes) {
		return false;
	}

	cap_words.resize(cap_count);
	if (!read_bytes(cap_words.data(), cap_count * sizeof(uint32_t))) return false;

	frag_words.resize(frag_count);
	if (!read_bytes(frag_words.data(), frag_count * sizeof(uint32_t))) return false;

	replay_words.resize(replay_count);
	if (!read_bytes(replay_words.data(), replay_count * sizeof(uint32_t))) return false;

	layout = std::move(loaded_layout);
	return true;
}

bool TryLoadTransformedShadersFromDisk(const std::string& title_id, uint64_t vs_hash,
                                       uint64_t ps_hash, PerVertexLayout& layout,
                                       std::vector<uint32_t>& cap_words,
                                       std::vector<uint32_t>& frag_words,
                                       std::vector<uint32_t>& replay_words) {
	if (title_id.empty()) return false;
	return TryLoadTransformedShadersFromDisk(std::filesystem::path("_ShaderCache") / title_id,
	                                         vs_hash, ps_hash, layout, cap_words, frag_words,
	                                         replay_words);
}

bool SaveTransformedShadersToDisk(const std::string& title_id, uint64_t vs_hash, uint64_t ps_hash,
                                  const PerVertexLayout&    layout,
                                  std::span<const uint32_t> cap_words,
                                  std::span<const uint32_t> frag_words,
                                  std::span<const uint32_t> replay_words) {
	if (title_id.empty()) return false;
	return SaveTransformedShadersToDisk(std::filesystem::path("_ShaderCache") / title_id, vs_hash,
	                                    ps_hash, layout, cap_words, frag_words, replay_words);
}

} // namespace Libs::Graphics
