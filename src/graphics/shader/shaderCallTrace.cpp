#include "graphics/shader/shaderCallTrace.h"

#include "common/logging/log.h"
#include "common/magicEnum.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fmt/format.h>
#include <optional>
#include <string>
#include <vector>

namespace Libs::Graphics {

namespace {

namespace Decoder = ShaderRecompiler::Decoder;
using Decoder::Opcode;
using Decoder::OperandKind;

constexpr uint32_t MaxDepth = 12;

constexpr uint32_t Sop1Prefix     = 0x17Du;
constexpr uint32_t Sop1SwappcB64  = 0x21u;
constexpr uint32_t Sop1SetpcB64  = 0x20u;
constexpr uint32_t SCodeEnd      = 0xbf9f0000u;
constexpr uint32_t SBranch       = 0xbf820000u;
constexpr uint32_t SEndpgm       = 0xbf810000u;

struct SwappcSite {
	uint32_t pc   = 0;
	uint32_t sdst = 0;
	uint32_t ssrc = 0;
};

std::vector<SwappcSite> FindSwappcSites(std::span<const uint32_t> code) {
	std::vector<SwappcSite> sites;
	for (uint32_t index = 0; index < code.size(); index++) {
		const auto word = code[index];
		if ((word >> 23u) != Sop1Prefix || ((word >> 8u) & 0xffu) != Sop1SwappcB64) {
			continue;
		}
		sites.push_back({index * 4u, (word >> 16u) & 0x7fu, word & 0xffu});
	}
	return sites;
}

bool ScalarCode(const Decoder::Operand& operand, uint32_t& code) {
	if (operand.kind != OperandKind::Sgpr) {
		return false;
	}
	code = operand.reg;
	return true;
}

bool WritesScalar(const Decoder::Instruction& inst, uint32_t reg, uint32_t& base,
                  uint32_t& dwords) {
	uint32_t dst = 0;
	if (!ScalarCode(inst.dst, dst)) {
		return false;
	}
	uint32_t width = 1;
	switch (inst.opcode) {
		case Opcode::S_LOAD_DWORD:
		case Opcode::S_BUFFER_LOAD_DWORD: width = 1; break;
		case Opcode::S_LOAD_DWORDX2:
		case Opcode::S_BUFFER_LOAD_DWORDX2: width = 2; break;
		case Opcode::S_LOAD_DWORDX4:
		case Opcode::S_BUFFER_LOAD_DWORDX4: width = 4; break;
		case Opcode::S_LOAD_DWORDX8:
		case Opcode::S_BUFFER_LOAD_DWORDX8: width = 8; break;
		case Opcode::S_LOAD_DWORDX16:
		case Opcode::S_BUFFER_LOAD_DWORDX16: width = 16; break;
		case Opcode::S_MOV_B32: width = 1; break;
		case Opcode::S_MOV_B64: width = 2; break;
		default: width = 1; break;
	}
	if (reg < dst || reg >= dst + width) {
		return false;
	}
	base   = dst;
	dwords = width;
	return true;
}

bool IsLoad(Opcode opcode) {
	switch (opcode) {
		case Opcode::S_LOAD_DWORD:
		case Opcode::S_LOAD_DWORDX2:
		case Opcode::S_LOAD_DWORDX4:
		case Opcode::S_LOAD_DWORDX8:
		case Opcode::S_LOAD_DWORDX16:
		case Opcode::S_BUFFER_LOAD_DWORD:
		case Opcode::S_BUFFER_LOAD_DWORDX2:
		case Opcode::S_BUFFER_LOAD_DWORDX4:
		case Opcode::S_BUFFER_LOAD_DWORDX8:
		case Opcode::S_BUFFER_LOAD_DWORDX16: return true;
		default: return false;
	}
}

bool IsBufferLoad(Opcode opcode) {
	switch (opcode) {
		case Opcode::S_BUFFER_LOAD_DWORD:
		case Opcode::S_BUFFER_LOAD_DWORDX2:
		case Opcode::S_BUFFER_LOAD_DWORDX4:
		case Opcode::S_BUFFER_LOAD_DWORDX8:
		case Opcode::S_BUFFER_LOAD_DWORDX16: return true;
		default: return false;
	}
}

class ChainWalker {
public:
	ChainWalker(const Decoder::Program& program, std::span<const uint32_t> user_data)
	    : m_program(program), m_user_data(user_data) {}

	std::optional<uint32_t> Scalar(uint32_t reg, uint32_t before_pc, uint32_t depth) {
		if (depth > MaxDepth) {
			Note(depth, fmt::format("s{} - giving up, chain deeper than {}", reg, MaxDepth));
			return std::nullopt;
		}
		const auto* writer = FindWriter(reg, before_pc);
		if (writer == nullptr) {
			if (reg < m_user_data.size()) {
				return m_user_data[reg];
			}
			Note(depth, fmt::format("s{} - no writer and outside the {} user-data words", reg,
			                        m_user_data.size()));
			return std::nullopt;
		}
		uint32_t base   = 0;
		uint32_t dwords = 0;
		WritesScalar(*writer, reg, base, dwords);

		if (writer->opcode == Opcode::S_MOV_B32 || writer->opcode == Opcode::S_MOV_B64) {
			uint32_t src = 0;
			if (ScalarCode(writer->src0, src)) {
				return Scalar(src + (reg - base), writer->pc, depth + 1);
			}
			if (writer->src0.kind == OperandKind::LiteralConstant ||
			    writer->src0.kind == OperandKind::IntegerInlineConstant) {
				return writer->src0.value;
			}
			Note(depth, fmt::format("s{} <- s_mov from an operand kind this walker does not read",
			                        reg));
			return std::nullopt;
		}

		if (!IsLoad(writer->opcode)) {
			Note(depth, fmt::format("s{} <- {} at pc 0x{:08x}, not a load or move", reg,
			                        magic_enum::enum_name(writer->opcode), writer->pc));
			return std::nullopt;
		}

		uint32_t sbase = 0;
		if (!ScalarCode(writer->src0, sbase)) {
			Note(depth, fmt::format("s{} <- load whose base is not an SGPR", reg));
			return std::nullopt;
		}

		uint64_t address = 0;
		if (IsBufferLoad(writer->opcode)) {
			const auto d0 = Scalar(sbase, writer->pc, depth + 1);
			const auto d1 = Scalar(sbase + 1u, writer->pc, depth + 1);
			if (!d0 || !d1) {
				return std::nullopt;
			}
			address = static_cast<uint64_t>(*d0) |
			          (static_cast<uint64_t>(*d1 & 0xffffu) << 32u);
			Note(depth, fmt::format("s{} <- s_buffer_load [V# s{} base=0x{:012x}] + 0x{:x}", reg,
			                        sbase, address, writer->offset));
			if (std::find(m_bases.begin(), m_bases.end(), address) == m_bases.end()) {
				m_bases.push_back(address);
			}
		} else {
			const auto lo = Scalar(sbase, writer->pc, depth + 1);
			const auto hi = Scalar(sbase + 1u, writer->pc, depth + 1);
			if (!lo || !hi) {
				return std::nullopt;
			}
			address = static_cast<uint64_t>(*lo) | (static_cast<uint64_t>(*hi) << 32u);
			Note(depth, fmt::format("s{} <- s_load [s[{}:{}] = 0x{:012x}] + 0x{:x}", reg, sbase,
			                        sbase + 1u, address, writer->offset));
		}

		address += writer->offset;
		address += static_cast<uint64_t>(reg - base) * 4u;

		uint32_t value = 0;
		if (!Libs::LibKernel::Memory::TryReadBacking(address, &value, sizeof(value))) {
			Note(depth, fmt::format("s{} - guest read at 0x{:012x} failed (unmapped)", reg,
			                        address));
			return std::nullopt;
		}
		return value;
	}

	[[nodiscard]] const std::vector<std::string>& Notes() const { return m_notes; }
	[[nodiscard]] const std::vector<uint64_t>&    Bases() const { return m_bases; }

private:
	const Decoder::Instruction* FindWriter(uint32_t reg, uint32_t before_pc) const {
		const Decoder::Instruction* found = nullptr;
		for (const auto& inst: m_program.instructions) {
			if (inst.pc >= before_pc) {
				break;
			}
			uint32_t base   = 0;
			uint32_t dwords = 0;
			if (WritesScalar(inst, reg, base, dwords)) {
				found = &inst;
			}
		}
		return found;
	}

	void Note(uint32_t depth, std::string text) {
		m_notes.push_back(std::string(depth * 2u, ' ') + std::move(text));
	}

	std::vector<uint64_t>     m_bases;
	const Decoder::Program&   m_program;
	std::span<const uint32_t> m_user_data;
	std::vector<std::string>  m_notes;
};

} // namespace

std::vector<IndirectCallSite> ResolveIndirectCalls(std::span<const uint32_t> code,
                                                   std::span<const uint32_t> user_data,
                                                   uint64_t                  shader_addr) {
	std::vector<IndirectCallSite> resolved;
	const auto                    sites = FindSwappcSites(code);
	if (sites.empty()) {
		return resolved;
	}

	Decoder::Program program {};
	Decoder::DecodeProgram(code, program);
	if (program.instructions.empty()) {
		return resolved;
	}

	for (const auto& site: sites) {
		IndirectCallSite out;
		out.pc          = site.pc;
		out.target_sgpr = site.ssrc;

		ChainWalker walker(program, user_data);
		const auto  lo = walker.Scalar(site.ssrc, site.pc, 0);
		const auto  hi = walker.Scalar(site.ssrc + 1u, site.pc, 0);
		if (lo && hi) {
			out.handler = static_cast<uint64_t>(*lo) | (static_cast<uint64_t>(*hi) << 32u);
		}

		resolved.push_back(out);
	}
	return resolved;
}

std::span<const uint32_t> TrimToCode(std::span<const uint32_t> code, uint32_t return_sgpr) {
	for (size_t word = 0; word < code.size(); word++) {
		if (code[word] == SCodeEnd) {
			return code.first(word);
		}
	}
	for (size_t word = code.size(); word-- > 0;) {
		if (code[word] == SEndpgm) {
			return code.first(word + 1u);
		}
	}
	if (return_sgpr != UINT32_MAX) {
		for (size_t word = code.size(); word-- > 0;) {
			const auto candidate = code[word];
			if ((candidate >> 23u) == Sop1Prefix && ((candidate >> 8u) & 0xffu) == Sop1SetpcB64 &&
			    (candidate & 0xffu) == return_sgpr) {
				return code.first(word + 1u);
			}
		}
	}
	return code;
}

bool EndsWithReturn(std::span<const uint32_t> code, uint32_t target_sgpr) {
	if (code.empty()) {
		return false;
	}
	const auto last = code.back();
	return (last >> 23u) == Sop1Prefix && ((last >> 8u) & 0xffu) == Sop1SetpcB64 &&
	       (last & 0xffu) == target_sgpr;
}

bool IsBranchOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::S_BRANCH:
		case Opcode::S_CBRANCH_SCC0:
		case Opcode::S_CBRANCH_SCC1:
		case Opcode::S_CBRANCH_VCCZ:
		case Opcode::S_CBRANCH_VCCNZ:
		case Opcode::S_CBRANCH_EXECZ:
		case Opcode::S_CBRANCH_EXECNZ: return true;
		default: return false;
	}
}

std::vector<uint32_t> SpliceIndirectCalls(std::span<const uint32_t>                  code,
                                          std::span<const IndirectCallSite>          sites,
                                          std::span<const std::span<const uint32_t>> handlers) {
	if (sites.size() != handlers.size() || sites.empty()) {
		return {};
	}

	const auto base = TrimToCode(code);

	Decoder::Program program {};
	Decoder::DecodeProgram(base, program);
	if (program.instructions.empty()) {
		return {};
	}

	struct Insertion {
		uint32_t                  word = 0; // dword index of the S_SWAPPC_B64 being replaced
		std::span<const uint32_t> body;     // callee code without its trailing return
	};
	std::vector<Insertion> insertions;
	for (size_t index = 0; index < sites.size(); index++) {
		const auto& site    = sites[index];
		const auto  handler = handlers[index];
		if (site.handler == 0 || handler.empty() || !EndsWithReturn(handler, site.target_sgpr)) {
			return {};
		}
		const auto word = site.pc / 4u;
		if (word >= base.size()) {
			return {};
		}
		insertions.push_back({word, handler.first(handler.size() - 1u)});
	}
	std::sort(insertions.begin(), insertions.end(),
	          [](const Insertion& lhs, const Insertion& rhs) { return lhs.word < rhs.word; });

	std::vector<uint32_t> remap(base.size(), 0);
	std::vector<uint32_t> out;
	out.reserve(base.size() + 512u);
	size_t next = 0;
	for (const auto& insertion: insertions) {
		for (; next < insertion.word; next++) {
			remap[next] = static_cast<uint32_t>(out.size());
			out.push_back(base[next]);
		}
		remap[insertion.word] = static_cast<uint32_t>(out.size());
		out.insert(out.end(), insertion.body.begin(), insertion.body.end());
		next = insertion.word + 1u; // the call itself is gone
	}
	for (; next < base.size(); next++) {
		remap[next] = static_cast<uint32_t>(out.size());
		out.push_back(base[next]);
	}

	for (const auto& inst: program.instructions) {
		if (!IsBranchOpcode(inst.opcode)) {
			continue;
		}
		const auto from = inst.pc / 4u;
		const auto to   = inst.branch_target / 4u;
		if (from >= base.size() || to >= base.size()) {
			return {};
		}
		const auto new_from = remap[from];
		const auto new_to   = remap[to];
		const auto delta = static_cast<int64_t>(new_to) - static_cast<int64_t>(new_from) - 1;
		if (delta < INT16_MIN || delta > INT16_MAX || new_from >= out.size()) {
			return {};
		}
		out[new_from] = (out[new_from] & 0xffff0000u) |
		                static_cast<uint32_t>(static_cast<uint16_t>(delta));
	}
	return out;
}

} // namespace Libs::Graphics
