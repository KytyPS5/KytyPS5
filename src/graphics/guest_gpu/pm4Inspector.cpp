#include "graphics/guest_gpu/pm4Inspector.h"

#include "common/file.h"
#include "graphics/guest_gpu/pm4.h"

#include <algorithm>
#include <fmt/format.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <tuple>
#include <utility>

namespace Libs::Graphics::Pm4 {

namespace {

constexpr uint32_t RegisterSelectorMask = 0x70000000u;

uint32_t NormalizeRegisterOffset(RegisterSpace space, uint32_t offset, uint8_t opcode) {
	if (space == RegisterSpace::Shader) {
		return offset;
	}
	if (space == RegisterSpace::Uconfig && opcode == IT_SET_UCONFIG_REG_INDEX) {
		return offset & 0x0fffffffu;
	}
	return offset & ~RegisterSelectorMask;
}

const char* QueueKindName(SubmissionQueueKind kind) {
	return kind == SubmissionQueueKind::Graphics ? "graphics" : "async_compute";
}

uint64_t QueueStateKey(SubmissionQueueKind kind, uint32_t queue) {
	return (static_cast<uint64_t>(kind) << 32u) | queue;
}

const char* RegisterSpaceName(RegisterSpace space) {
	switch (space) {
		case RegisterSpace::Context: return "context";
		case RegisterSpace::Shader: return "shader";
		case RegisterSpace::Uconfig: return "uconfig";
	}
	return "unknown";
}

const char* ReferenceKindName(MemoryReferenceKind kind) {
	switch (kind) {
		case MemoryReferenceKind::CommandBuffer: return "command_buffer";
		case MemoryReferenceKind::RegisterPairs: return "register_pairs";
		case MemoryReferenceKind::CompareValue: return "compare_value";
	}
	return "unknown";
}

std::string AddressText(uint64_t address) {
	return fmt::format("0x{:016x}", address);
}

std::string WordText(uint32_t value) {
	return fmt::format("0x{:08x}", value);
}

void HashWord(uint64_t* hash, uint32_t value) {
	constexpr uint64_t FnvPrime = 1099511628211ull;
	for (uint32_t shift = 0; shift < 32; shift += 8) {
		*hash ^= (value >> shift) & 0xffu;
		*hash *= FnvPrime;
	}
}

class Parser final {
public:
	Parser(SubmissionInspection* result, QueueRegisterState* state, const MemoryReader& reader,
	       const InspectionLimits& limits)
	    : m_result(result), m_state(state), m_reader(reader), m_limits(limits) {}

	void AddRoot(const char* role, uint64_t address, std::span<const uint32_t> words) {
		AddBuffer(role, address, words, 0);
	}

private:
	size_t AddBuffer(const char* role, uint64_t address, std::span<const uint32_t> words,
	                 uint32_t depth) {
		BufferInspection buffer;
		buffer.id               = m_result->buffers.size();
		buffer.role             = role;
		buffer.address          = address;
		buffer.declared_size_dw = static_cast<uint32_t>(words.size());
		buffer.depth            = depth;
		buffer.words.assign(words.begin(), words.end());
		const size_t id = buffer.id;
		m_result->buffers.push_back(std::move(buffer));
		m_buffer_ids.emplace(std::make_pair(address, static_cast<uint32_t>(words.size())), id);
		ParseBuffer(id);
		return id;
	}

	void Count(InspectionStatus status) { m_result->status_counts[static_cast<size_t>(status)]++; }

	bool ReadMemory(uint64_t address, uint32_t size_dw, std::vector<uint32_t>* words,
	                std::string* note) {
		if (size_dw == 0) {
			words->clear();
			return true;
		}
		if (!m_reader) {
			*note = "no memory reader was available";
			return false;
		}
		if (size_dw > m_limits.max_single_snapshot_dw ||
		    size_dw > m_limits.max_total_snapshot_dw - m_snapshot_dw) {
			*note = "snapshot limit exceeded";
			return false;
		}
		if (!m_reader(address, size_dw, words) || words->size() != size_dw) {
			words->clear();
			*note = "memory was not readable at capture time";
			return false;
		}
		m_snapshot_dw += size_dw;
		return true;
	}

	MemoryReference SnapshotWords(MemoryReferenceKind kind, uint64_t address, uint32_t size_dw) {
		MemoryReference reference;
		reference.kind    = kind;
		reference.address = address;
		reference.size_dw = size_dw;
		if (address == 0 && size_dw != 0) {
			reference.note = "null address";
			return reference;
		}
		if (ReadMemory(address, size_dw, &reference.snapshot_words, &reference.note)) {
			reference.snapshot_status = InspectionStatus::Known;
		}
		return reference;
	}

	MemoryReference SnapshotCommandBuffer(uint64_t address, uint32_t size_dw, uint32_t depth) {
		MemoryReference reference;
		reference.kind    = MemoryReferenceKind::CommandBuffer;
		reference.address = address;
		reference.size_dw = size_dw;
		if (size_dw == 0) {
			reference.snapshot_status = InspectionStatus::Known;
			reference.note            = "empty command buffer";
			return reference;
		}
		if (address == 0) {
			reference.note = "null address";
			return reference;
		}
		const auto key = std::make_pair(address, size_dw);
		if (const auto existing = m_buffer_ids.find(key); existing != m_buffer_ids.end()) {
			reference.snapshot_status  = InspectionStatus::Known;
			reference.nested_buffer_id = existing->second;
			reference.note             = "already captured";
			return reference;
		}
		if (depth > m_limits.max_depth) {
			reference.note = "maximum indirect-buffer depth exceeded";
			return reference;
		}
		std::vector<uint32_t> words;
		if (!ReadMemory(address, size_dw, &words, &reference.note)) {
			return reference;
		}
		reference.snapshot_status  = InspectionStatus::Known;
		reference.nested_buffer_id = AddBuffer("indirect_command_buffer", address, words, depth);
		return reference;
	}

	void AddRegisterWrite(PacketInspection* packet, RegisterSpace space, uint32_t raw_offset,
	                      uint32_t value) {
		if (raw_offset == 0xffffffffu) {
			return;
		}
		const auto offset = NormalizeRegisterOffset(space, raw_offset, packet->opcode);
		packet->register_writes.push_back({space, offset, value});
		m_state->Set(space, offset, value);
	}

	void InspectDirectRegisters(PacketInspection* packet, RegisterSpace space) {
		if (packet->payload.size() < 2) {
			packet->status = InspectionStatus::UnreadableAtCaptureTime;
			return;
		}
		const auto first = packet->payload[0];
		for (size_t index = 1; index < packet->payload.size(); index++) {
			AddRegisterWrite(packet, space, first + static_cast<uint32_t>(index - 1),
			                 packet->payload[index]);
		}
		packet->status = InspectionStatus::Known;
	}

	void InspectIndirectRegisters(PacketInspection* packet, RegisterSpace space) {
		if (packet->payload.size() != 4) {
			packet->status = InspectionStatus::Partial;
			return;
		}
		const uint64_t address    = (static_cast<uint64_t>(packet->payload[0]) & 0xfffffffcu) |
		                            (static_cast<uint64_t>(packet->payload[1]) << 32u);
		const uint32_t pair_count = packet->payload[3] & 0x3fffu;
		if (pair_count > std::numeric_limits<uint32_t>::max() / 2u) {
			packet->status = InspectionStatus::UnreadableAtCaptureTime;
			return;
		}
		auto reference =
		    SnapshotWords(MemoryReferenceKind::RegisterPairs, address, pair_count * 2u);
		if (reference.snapshot_status == InspectionStatus::Known) {
			for (uint32_t index = 0; index < pair_count; index++) {
				AddRegisterWrite(packet, space, reference.snapshot_words[index * 2u],
				                 reference.snapshot_words[index * 2u + 1u]);
			}
			packet->status = InspectionStatus::Known;
		} else {
			packet->status = InspectionStatus::Partial;
		}
		packet->references.push_back(std::move(reference));
	}

	void InspectIndirectBuffer(PacketInspection* packet, uint32_t depth) {
		if (packet->payload.size() == 3) {
			const uint64_t address   = static_cast<uint64_t>(packet->payload[0]) |
			                           (static_cast<uint64_t>(packet->payload[1]) << 32u);
			const uint32_t size_dw   = packet->payload[2] & 0xfffffu;
			auto           reference = SnapshotCommandBuffer(address, size_dw, depth + 1u);
			packet->status           = reference.snapshot_status == InspectionStatus::Known
			                               ? InspectionStatus::Known
			                               : InspectionStatus::Partial;
			packet->references.push_back(std::move(reference));
			return;
		}
		if (packet->payload.size() == 13) {
			const uint64_t compare_address =
			    (static_cast<uint64_t>(packet->payload[1]) & 0xfffffff8u) |
			    (static_cast<uint64_t>(packet->payload[2]) << 32u);
			packet->references.push_back(
			    SnapshotWords(MemoryReferenceKind::CompareValue, compare_address, 2));

			const uint64_t then_address =
			    (static_cast<uint64_t>(packet->payload[7]) & 0xfffffffcu) |
			    (static_cast<uint64_t>(packet->payload[8]) << 32u);
			const uint32_t then_size = packet->payload[9] & 0xfffffu;
			packet->references.push_back(
			    SnapshotCommandBuffer(then_address, then_size, depth + 1u));

			const uint64_t else_address =
			    (static_cast<uint64_t>(packet->payload[10]) & 0xfffffffcu) |
			    (static_cast<uint64_t>(packet->payload[11]) << 32u);
			const uint32_t else_size = packet->payload[12] & 0xfffffu;
			if (else_size != 0) {
				packet->references.push_back(
				    SnapshotCommandBuffer(else_address, else_size, depth + 1u));
			}
			packet->status =
			    std::all_of(packet->references.begin(), packet->references.end(),
			                [](const auto& reference) {
				                return reference.snapshot_status == InspectionStatus::Known;
			                })
			        ? InspectionStatus::Known
			        : InspectionStatus::Partial;
			return;
		}
		packet->status = InspectionStatus::Partial;
	}

	void InspectType3(PacketInspection* packet, uint32_t depth) {
		packet->name = GetOpcodeName(packet->opcode);
		if (packet->opcode == IT_NOP) {
			packet->custom_opcode = KYTY_PM4_R(packet->header);
			if (IsCustomOpcodeNamed(packet->custom_opcode)) {
				packet->name += fmt::format(":{}", GetCustomOpcodeName(packet->custom_opcode));
			}
			packet->status = packet->custom_opcode == R_ZERO ? InspectionStatus::Known
			                                                 : InspectionStatus::Inferred;
			return;
		}
		switch (packet->opcode) {
			case IT_SET_CONTEXT_REG: InspectDirectRegisters(packet, RegisterSpace::Context); return;
			case IT_SET_SH_REG: InspectDirectRegisters(packet, RegisterSpace::Shader); return;
			case IT_SET_UCONFIG_REG:
			case IT_SET_UCONFIG_REG_INDEX:
				InspectDirectRegisters(packet, RegisterSpace::Uconfig);
				return;
			case IT_SET_CONTEXT_REG_INDIRECT:
				InspectIndirectRegisters(packet, RegisterSpace::Context);
				return;
			case IT_SET_SH_REG_INDIRECT:
				InspectIndirectRegisters(packet, RegisterSpace::Shader);
				return;
			case IT_SET_UCONFIG_REG_INDIRECT:
				InspectIndirectRegisters(packet, RegisterSpace::Uconfig);
				return;
			case IT_INDIRECT_BUFFER: InspectIndirectBuffer(packet, depth); return;
			default:
				packet->status = IsOpcodeNamed(packet->opcode) ? InspectionStatus::Inferred
				                                               : InspectionStatus::Unknown;
				return;
		}
	}

	void ParseBuffer(size_t buffer_id) {
		const auto                    words = m_result->buffers[buffer_id].words;
		const auto                    depth = m_result->buffers[buffer_id].depth;
		std::vector<PacketInspection> packets;
		for (uint32_t offset = 0; offset < words.size();) {
			PacketInspection packet;
			packet.offset_dw = offset;
			packet.header    = words[offset];
			packet.type      = packet.header >> 30u;
			packet.predicate = (packet.header & 1u) != 0;

			uint32_t packet_dw = 0;
			switch (packet.type) {
				case 0: packet_dw = ((packet.header >> 16u) & 0x3fffu) + 2u; break;
				case 1: packet_dw = 3; break;
				case 2: packet_dw = 1; break;
				case 3: packet_dw = KYTY_PM4_LEN(packet.header); break;
				default: break;
			}
			const auto remaining = static_cast<uint32_t>(words.size()) - offset;
			if (packet_dw == 0 || packet_dw > remaining) {
				packet.size_dw = remaining;
				packet.status  = InspectionStatus::UnreadableAtCaptureTime;
				packet.name    = "truncated_or_invalid_packet";
				if (remaining > 1) {
					packet.payload.assign(words.begin() + offset + 1, words.end());
				}
				Count(packet.status);
				packets.push_back(std::move(packet));
				break;
			}

			packet.size_dw = packet_dw;
			if (packet_dw > 1) {
				packet.payload.assign(words.begin() + offset + 1,
				                      words.begin() + offset + packet_dw);
			}
			if (packet.type == 3) {
				packet.opcode = static_cast<uint8_t>((packet.header >> 8u) & 0xffu);
				InspectType3(&packet, depth);
			} else if (packet.type == 2) {
				packet.name   = "TYPE2_PADDING";
				packet.status = InspectionStatus::Known;
			} else {
				packet.name   = fmt::format("TYPE{}_PACKET", packet.type);
				packet.status = InspectionStatus::Unsupported;
			}
			Count(packet.status);
			packets.push_back(std::move(packet));
			offset += packet_dw;
		}
		m_result->buffers[buffer_id].packets = std::move(packets);
	}

	SubmissionInspection*                           m_result;
	QueueRegisterState*                             m_state;
	const MemoryReader&                             m_reader;
	InspectionLimits                                m_limits;
	uint32_t                                        m_snapshot_dw = 0;
	std::map<std::pair<uint64_t, uint32_t>, size_t> m_buffer_ids;
};

nlohmann::json StateToJson(const StateView& state) {
	return {{"context_registers", state.context_registers},
	        {"shader_registers", state.shader_registers},
	        {"uconfig_registers", state.uconfig_registers},
	        {"hash", AddressText(state.hash)}};
}

} // namespace

const char* InspectionStatusName(InspectionStatus status) noexcept {
	switch (status) {
		case InspectionStatus::Known: return "Known";
		case InspectionStatus::Partial: return "Partial";
		case InspectionStatus::Inferred: return "Inferred";
		case InspectionStatus::Unknown: return "Unknown";
		case InspectionStatus::Unsupported: return "Unsupported";
		case InspectionStatus::UnreadableAtCaptureTime: return "UnreadableAtCaptureTime";
		case InspectionStatus::Count: break;
	}
	return "Unknown";
}

void QueueRegisterState::Reset() {
	m_context.clear();
	m_shader.clear();
	m_uconfig.clear();
}

std::map<uint32_t, uint32_t>& QueueRegisterState::Select(RegisterSpace space) {
	switch (space) {
		case RegisterSpace::Context: return m_context;
		case RegisterSpace::Shader: return m_shader;
		case RegisterSpace::Uconfig: return m_uconfig;
	}
	return m_context;
}

const std::map<uint32_t, uint32_t>& QueueRegisterState::Select(RegisterSpace space) const {
	switch (space) {
		case RegisterSpace::Context: return m_context;
		case RegisterSpace::Shader: return m_shader;
		case RegisterSpace::Uconfig: return m_uconfig;
	}
	return m_context;
}

void QueueRegisterState::Set(RegisterSpace space, uint32_t offset, uint32_t value) {
	Select(space)[offset] = value;
}

std::optional<uint32_t> QueueRegisterState::Get(RegisterSpace space, uint32_t offset) const {
	const auto& registers = Select(space);
	const auto  value     = registers.find(offset);
	return value == registers.end() ? std::nullopt : std::optional<uint32_t>(value->second);
}

StateView QueueRegisterState::View() const {
	StateView view;
	view.context_registers = static_cast<uint32_t>(m_context.size());
	view.shader_registers  = static_cast<uint32_t>(m_shader.size());
	view.uconfig_registers = static_cast<uint32_t>(m_uconfig.size());
	uint64_t   hash        = 14695981039346656037ull;
	const auto hash_map    = [&hash](uint32_t tag, const auto& values) {
		HashWord(&hash, tag);
		for (const auto& [offset, value]: values) {
			HashWord(&hash, offset);
			HashWord(&hash, value);
		}
	};
	hash_map(0, m_context);
	hash_map(1, m_shader);
	hash_map(2, m_uconfig);
	view.hash = hash;
	return view;
}

SubmissionInspection InspectSubmission(const SubmissionMetadata& metadata, const char* root_role,
                                       uint64_t root_address, std::span<const uint32_t> root_words,
                                       QueueRegisterState* state, const MemoryReader& reader,
                                       const InspectionLimits& limits) {
	SubmissionInspection result;
	result.metadata           = metadata;
	result.state_before_reset = state->View();
	if (metadata.reset_state) {
		state->Reset();
	}
	result.state_at_submit = state->View();
	Parser parser(&result, state, reader, limits);
	parser.AddRoot(root_role, root_address, root_words);
	result.state_after = state->View();
	return result;
}

std::string SerializeSubmissionInspection(const SubmissionInspection& inspection) {
	nlohmann::json root;
	root["schema"]                 = "kyty.pm4.submission.v1";
	root["capture"]                = {{"id", inspection.metadata.capture_id},
	                                  {"frame", inspection.metadata.frame},
	                                  {"queue", inspection.metadata.queue},
	                                  {"queue_kind", QueueKindName(inspection.metadata.queue_kind)},
	                                  {"reset_tracked_state", inspection.metadata.reset_state}};
	root["tracked_register_state"] = {{"before_reset", StateToJson(inspection.state_before_reset)},
	                                  {"at_submit", StateToJson(inspection.state_at_submit)},
	                                  {"after", StateToJson(inspection.state_after)}};
	root["capture_scope"]          = {
	    {"command_buffers", "root and readable nested indirect buffers"},
	    {"indirect_registers", "readable register-pair arrays"},
	    {"resource_registration_map", "not available"},
	    {"descriptor_memory", "not yet captured unless encoded as an inspected reference"},
	    {"classification", "packet structure, not a claim of complete runtime support"}};

	nlohmann::json summary = nlohmann::json::object();
	for (size_t index = 0; index < static_cast<size_t>(InspectionStatus::Count); index++) {
		summary[InspectionStatusName(static_cast<InspectionStatus>(index))] =
		    inspection.status_counts[index];
	}
	root["summary"] = std::move(summary);

	root["buffers"] = nlohmann::json::array();
	for (const auto& buffer: inspection.buffers) {
		nlohmann::json buffer_json = {{"id", buffer.id},
		                              {"role", buffer.role},
		                              {"address", AddressText(buffer.address)},
		                              {"declared_size_dw", buffer.declared_size_dw},
		                              {"depth", buffer.depth},
		                              {"words", buffer.words},
		                              {"packets", nlohmann::json::array()}};
		for (const auto& packet: buffer.packets) {
			nlohmann::json packet_json = {{"offset_dw", packet.offset_dw},
			                              {"size_dw", packet.size_dw},
			                              {"header", WordText(packet.header)},
			                              {"type", packet.type},
			                              {"predicate", packet.predicate},
			                              {"status", InspectionStatusName(packet.status)},
			                              {"name", packet.name},
			                              {"payload", packet.payload},
			                              {"register_writes", nlohmann::json::array()},
			                              {"references", nlohmann::json::array()}};
			if (packet.type == 3) {
				packet_json["opcode"]     = packet.opcode;
				packet_json["opcode_hex"] = fmt::format("0x{:02x}", packet.opcode);
				if (packet.opcode == IT_NOP) {
					packet_json["custom_opcode"] = packet.custom_opcode;
				}
			}
			for (const auto& write: packet.register_writes) {
				packet_json["register_writes"].push_back({{"space", RegisterSpaceName(write.space)},
				                                          {"offset", WordText(write.offset)},
				                                          {"value", WordText(write.value)}});
			}
			for (const auto& reference: packet.references) {
				nlohmann::json reference_json = {
				    {"kind", ReferenceKindName(reference.kind)},
				    {"address", AddressText(reference.address)},
				    {"size_dw", reference.size_dw},
				    {"snapshot_status", InspectionStatusName(reference.snapshot_status)},
				    {"note", reference.note}};
				if (reference.nested_buffer_id.has_value()) {
					reference_json["nested_buffer_id"] = *reference.nested_buffer_id;
				}
				if (!reference.snapshot_words.empty()) {
					reference_json["snapshot_words"] = reference.snapshot_words;
				}
				packet_json["references"].push_back(std::move(reference_json));
			}
			buffer_json["packets"].push_back(std::move(packet_json));
		}
		root["buffers"].push_back(std::move(buffer_json));
	}
	return root.dump(2);
}

SubmissionInspector::SubmissionInspector(std::filesystem::path folder, MemoryReader reader,
                                         InspectionLimits limits)
    : m_folder(std::move(folder)), m_reader(std::move(reader)), m_limits(limits) {}

bool SubmissionInspector::CaptureGraphics(uint32_t frame, bool reset_state, uint64_t dcb_address,
                                          std::span<const uint32_t> dcb_words) {
	return Capture(SubmissionQueueKind::Graphics, frame, 0, reset_state, "dcb", dcb_address,
	               dcb_words);
}

bool SubmissionInspector::CaptureAsyncCompute(uint32_t frame, uint32_t queue, uint64_t acb_address,
                                              std::span<const uint32_t> acb_words) {
	return Capture(SubmissionQueueKind::AsyncCompute, frame, queue, false, "acb", acb_address,
	               acb_words);
}

bool SubmissionInspector::Capture(SubmissionQueueKind queue_kind, uint32_t frame, uint32_t queue,
                                  bool reset_state, const char* role, uint64_t address,
                                  std::span<const uint32_t> words) {
	std::lock_guard    lock(m_mutex);
	SubmissionMetadata metadata;
	metadata.capture_id  = m_next_capture_id++;
	metadata.frame       = frame;
	metadata.queue       = queue;
	metadata.queue_kind  = queue_kind;
	metadata.reset_state = reset_state;
	auto inspection =
	    InspectSubmission(metadata, role, address, words,
	                      &m_queue_states[QueueStateKey(queue_kind, queue)], m_reader, m_limits);
	auto json            = SerializeSubmissionInspection(inspection);
	auto path = m_folder / fmt::format("{:06d}_f{:05d}_q{:02x}_{}.json", metadata.capture_id, frame,
	                                   queue, role);
	if (!Common::File::CreateDirectories(path.parent_path())) {
		return false;
	}
	Common::File file;
	file.Create(path);
	if (file.IsInvalid() || json.size() > std::numeric_limits<uint32_t>::max()) {
		return false;
	}
	uint32_t bytes_written = 0;
	file.Write(json.data(), static_cast<uint32_t>(json.size()), &bytes_written);
	file.Close();
	return bytes_written == json.size();
}

} // namespace Libs::Graphics::Pm4
