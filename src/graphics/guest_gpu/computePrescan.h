#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_COMPUTEPRESCAN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_COMPUTEPRESCAN_H_

#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"

#include <cstdint>
#include <span>
#include <vector>

// Run-ahead over a command list: the compute dispatches it will make, with the compute shader
// registers each one sees, so their shaders can be translated before the GPU thread reaches them.
// A wrong guess only costs work on a worker thread: nothing here changes what the GPU thread does.
namespace Libs::Graphics {

// Writes one compute shader register (an SH register from COMPUTE_START_X to COMPUTE_PACE_ID).
// False for a register outside that set.
[[nodiscard]] inline bool ApplyComputeShaderRegister(HW::CsStageRegisters& cs_regs, uint32_t offset,
                                                     uint32_t value) {
	switch (offset) {
		case Pm4::COMPUTE_PGM_LO:
			cs_regs.data_addr &= 0xFFFFFF00000000FFull;
			cs_regs.data_addr |= static_cast<uint64_t>(value) << 8u;
			break;
		case Pm4::COMPUTE_PGM_HI:
			cs_regs.data_addr &= 0xFFFF00FFFFFFFFFFull;
			cs_regs.data_addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			break;
		case Pm4::COMPUTE_PGM_RSRC1:
			cs_regs.vgprs =
			    (value >> Pm4::COMPUTE_PGM_RSRC1_VGPRS_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_VGPRS_MASK;
			cs_regs.priority      = (value >> Pm4::COMPUTE_PGM_RSRC1_PRIORITY_SHIFT) &
			                        Pm4::COMPUTE_PGM_RSRC1_PRIORITY_MASK;
			cs_regs.float_mode    = (value >> Pm4::COMPUTE_PGM_RSRC1_FLOAT_MODE_SHIFT) &
			                        Pm4::COMPUTE_PGM_RSRC1_FLOAT_MODE_MASK;
			cs_regs.dx10_clamp    = ((value >> Pm4::COMPUTE_PGM_RSRC1_DX10_CLAMP_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC1_DX10_CLAMP_MASK) != 0u;
			cs_regs.debug_mode    = ((value >> Pm4::COMPUTE_PGM_RSRC1_DEBUG_MODE_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC1_DEBUG_MODE_MASK) != 0u;
			cs_regs.ieee_mode     = ((value >> Pm4::COMPUTE_PGM_RSRC1_IEEE_MODE_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC1_IEEE_MODE_MASK) != 0u;
			cs_regs.fp16_overflow = ((value >> Pm4::COMPUTE_PGM_RSRC1_FP16_OVFL_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC1_FP16_OVFL_MASK) != 0u;
			cs_regs.threadgroup_configuration = ((value >> Pm4::COMPUTE_PGM_RSRC1_WGP_MODE_SHIFT) &
			                                     Pm4::COMPUTE_PGM_RSRC1_WGP_MODE_MASK) != 0u;
			cs_regs.require_forward_progress =
			    ((value >> Pm4::COMPUTE_PGM_RSRC1_FWD_PROGRESS_SHIFT) &
			     Pm4::COMPUTE_PGM_RSRC1_FWD_PROGRESS_MASK) != 0u;
			break;
		case Pm4::COMPUTE_PGM_RSRC2:
			cs_regs.scratch_en     = ((value >> Pm4::COMPUTE_PGM_RSRC2_SCRATCH_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_SCRATCH_EN_MASK) != 0u;
			cs_regs.user_sgpr      = (value >> Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_MASK;
			cs_regs.tgid_x_en      = ((value >> Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_MASK) != 0u;
			cs_regs.tgid_y_en      = ((value >> Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_MASK) != 0u;
			cs_regs.tgid_z_en      = ((value >> Pm4::COMPUTE_PGM_RSRC2_TGID_Z_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_TGID_Z_EN_MASK) != 0u;
			cs_regs.tg_size_en     = ((value >> Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_SHIFT) &
			                          Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_MASK) != 0u;
			cs_regs.tidig_comp_cnt = (value >> Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_MASK;
			cs_regs.lds_size       = (value >> Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_SHIFT) &
			                         Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_MASK;
			break;
		case Pm4::COMPUTE_PGM_RSRC3:
			cs_regs.shared_vgprs = (value >> Pm4::COMPUTE_PGM_RSRC3_SHARED_VGPRS_SHIFT) &
			                       Pm4::COMPUTE_PGM_RSRC3_SHARED_VGPRS_MASK;
			break;
		case Pm4::COMPUTE_NUM_THREAD_X: cs_regs.num_thread_x = value; break;
		case Pm4::COMPUTE_NUM_THREAD_Y: cs_regs.num_thread_y = value; break;
		case Pm4::COMPUTE_NUM_THREAD_Z: cs_regs.num_thread_z = value; break;
		case Pm4::COMPUTE_START_X:
		case Pm4::COMPUTE_START_Y:
		case Pm4::COMPUTE_START_Z:
		case Pm4::COMPUTE_RESOURCE_LIMITS:
		case Pm4::COMPUTE_TMPRING_SIZE:
		case Pm4::COMPUTE_PACE_ID: break;
		default: return false;
	}
	return true;
}

// A dispatch the command list makes, with the compute shader registers it sees.
struct PrescannedDispatch {
	HW::ComputeShaderInfo regs;
	uint32_t              mode     = 0; // COMPUTE_DISPATCH_INITIATOR
	bool                  indirect = false;
};

struct PrescanLimits {
	uint32_t max_depth      = 4;         // Nested indirect buffers followed.
	uint32_t max_dwords     = 1u << 20u; // Dwords read from indirect buffers in all.
	uint32_t max_dispatches = 512;
};

namespace PrescanDetail {

// The register offset of an SH register write, without its selector bits (as the CP reads it).
constexpr uint32_t RegisterOffset(uint32_t raw) {
	return raw & ~0x70000000u;
}

inline void SetShRegister(HW::ComputeShaderInfo& state, uint32_t reg, uint32_t value) {
	if (reg >= Pm4::COMPUTE_USER_DATA_0 && reg < Pm4::COMPUTE_USER_DATA_0 + 16u) {
		state.cs_user_sgpr.value[reg - Pm4::COMPUTE_USER_DATA_0] = value;
	} else {
		(void)ApplyComputeShaderRegister(state.cs_regs, reg, value);
	}
}

template <class Read>
void Scan(std::span<const uint32_t> commands, uint32_t depth, HW::ComputeShaderInfo& state,
          Read& read, std::vector<PrescannedDispatch>& out, const PrescanLimits& limits,
          uint32_t& dwords_left) {
	size_t offset = 0;
	while (offset < commands.size() && out.size() < limits.max_dispatches) {
		const auto header = commands[offset];
		if (header == 0x80000000u || Pm4::PacketSizeDw(header) == 1u) {
			offset++;
			continue;
		}
		// Only type-3 packets are framed by their header: anything else ends the run-ahead.
		if ((header >> 30u) != 3u) {
			return;
		}
		const auto length = KYTY_PM4_LEN(header);
		if (length > commands.size() - offset) {
			return;
		}
		const auto body   = commands.subspan(offset + 1, length - 1);
		const auto opcode = (header >> 8u) & 0xffu;
		if (opcode == Pm4::IT_SET_SH_REG && !body.empty()) {
			const auto base = body[0];
			for (uint32_t i = 1; i < body.size(); i++) {
				SetShRegister(state, base + i - 1, body[i]);
			}
		} else if (opcode == Pm4::IT_SET_SH_REG_INDIRECT && length == 5u) {
			// Offset and value pairs in guest memory.
			const auto address = (body[0] & 0xfffffffcu) | (static_cast<uint64_t>(body[1]) << 32u);
			const auto pairs   = body[3] & 0x3fffu;
			if (pairs != 0u && pairs * 2u <= dwords_left) {
				dwords_left -= pairs * 2u;
				std::vector<uint32_t> words(static_cast<size_t>(pairs) * 2u);
				if (read(address, std::span<uint32_t>(words))) {
					for (uint32_t i = 0; i < pairs; i++) {
						if (words[i * 2u] == 0xffffffffu) continue;
						SetShRegister(state, RegisterOffset(words[i * 2u]), words[i * 2u + 1u]);
					}
				}
			}
		} else if (opcode == Pm4::IT_DISPATCH_DIRECT && length == 5u) {
			// As the CP does: the initiator sets the wave size the shader runs with.
			state.cs_regs.wave_size = Pm4::ComputeWaveSize(body[3]);
			out.push_back({state, body[3], false});
		} else if (opcode == Pm4::IT_DISPATCH_INDIRECT && (length == 3u || length == 4u)) {
			state.cs_regs.wave_size = Pm4::ComputeWaveSize(body[length - 2u]);
			out.push_back({state, body[length - 2u], true});
		} else if (opcode == Pm4::IT_INDIRECT_BUFFER && length == 4u && depth < limits.max_depth) {
			const auto address = body[0] | (static_cast<uint64_t>(body[1]) << 32u);
			const auto count   = body[2] & 0xfffffu;
			if (count != 0u && count <= dwords_left) {
				dwords_left -= count;
				std::vector<uint32_t> nested(count);
				if (read(address, std::span<uint32_t>(nested))) {
					Scan(std::span<const uint32_t>(nested), depth + 1, state, read, out, limits,
					     dwords_left);
				}
			}
		}
		offset += length;
	}
}

} // namespace PrescanDetail

// Appends the dispatches of `commands` to `out`, starting from the compute registers `state`
// (left as they are after the list). `read(address, words)` fills `words` from guest memory
// and returns false when the range is not readable; nested indirect buffers are read with it.
template <class Read>
void PrescanComputeDispatches(std::span<const uint32_t> commands, HW::ComputeShaderInfo& state,
                              Read&& read, std::vector<PrescannedDispatch>& out,
                              const PrescanLimits& limits = {}) {
	uint32_t dwords_left = limits.max_dwords;
	PrescanDetail::Scan(commands, 0, state, read, out, limits, dwords_left);
}

} // namespace Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_COMPUTEPRESCAN_H_
