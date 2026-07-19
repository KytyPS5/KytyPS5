#include "graphics/guest_gpu/pm4.h"

#include "common/assert.h"
#include "common/file.h"

#include <array>

namespace Libs::Graphics::Pm4 {

namespace {

enum class PacketType : uint32_t { Type0, Type1, Type2, Type3 };

constexpr auto MakeRegisterNames() {
	std::array<const char*, R_NUM> names {};
	for (auto& name: names) {
		name = "<unknown>";
	}

	names[R_ZERO]           = "R_ZERO";
	names[R_DRAW_RESET]     = "R_DRAW_RESET";
	names[R_WAIT_FLIP_DONE] = "R_WAIT_FLIP_DONE";
	names[R_DISPATCH_RESET] = "R_DISPATCH_RESET";
	names[R_WAIT_MEM_32]    = "R_WAIT_MEM_32";
	names[R_PUSH_MARKER]    = "R_PUSH_MARKER";
	names[R_POP_MARKER]     = "R_POP_MARKER";
	names[R_ACQUIRE_MEM]    = "R_ACQUIRE_MEM";
	names[R_WRITE_DATA]     = "R_WRITE_DATA";
	names[R_WAIT_MEM_64]    = "R_WAIT_MEM_64";
	names[R_FLIP]           = "R_FLIP";
	names[R_RELEASE_MEM]    = "R_RELEASE_MEM";
	names[R_DMA_DATA]       = "R_DMA_DATA";
	return names;
}

constexpr auto MakeOpcodeNames() {
	std::array<const char*, 256> names {};
	for (auto& name: names) {
		name = "<unknown>";
	}

	names[IT_NOP]                       = "IT_NOP";
	names[IT_SET_BASE]                  = "IT_SET_BASE";
	names[IT_CLEAR_STATE]               = "IT_CLEAR_STATE";
	names[IT_INDEX_BUFFER_SIZE]         = "IT_INDEX_BUFFER_SIZE";
	names[IT_DISPATCH_DIRECT]           = "IT_DISPATCH_DIRECT";
	names[IT_DISPATCH_INDIRECT]         = "IT_DISPATCH_INDIRECT";
	names[IT_SET_PREDICATION]           = "IT_SET_PREDICATION";
	names[IT_COND_EXEC]                 = "IT_COND_EXEC";
	names[IT_DRAW_INDIRECT]             = "IT_DRAW_INDIRECT";
	names[IT_DRAW_INDEX_INDIRECT]       = "IT_DRAW_INDEX_INDIRECT";
	names[IT_INDEX_BASE]                = "IT_INDEX_BASE";
	names[IT_DRAW_INDEX_2]              = "IT_DRAW_INDEX_2";
	names[IT_CONTEXT_CONTROL]           = "IT_CONTEXT_CONTROL";
	names[IT_INDEX_TYPE]                = "IT_INDEX_TYPE";
	names[IT_DRAW_INDIRECT_MULTI]       = "IT_DRAW_INDIRECT_MULTI";
	names[IT_DRAW_INDEX_AUTO]           = "IT_DRAW_INDEX_AUTO";
	names[IT_NUM_INSTANCES]             = "IT_NUM_INSTANCES";
	names[IT_INDIRECT_BUFFER_CNST]      = "IT_INDIRECT_BUFFER_CNST";
	names[IT_DRAW_INDEX_OFFSET_2]       = "IT_DRAW_INDEX_OFFSET_2";
	names[IT_WRITE_DATA]                = "IT_WRITE_DATA";
	names[IT_MEM_SEMAPHORE]             = "IT_MEM_SEMAPHORE";
	names[IT_DRAW_INDEX_INDIRECT_MULTI] = "IT_DRAW_INDEX_INDIRECT_MULTI";
	names[IT_INDIRECT_BUFFER]           = "IT_INDIRECT_BUFFER";
	names[IT_COPY_DATA]                 = "IT_COPY_DATA";
	names[IT_CP_DMA]                    = "IT_CP_DMA";
	names[IT_PFP_SYNC_ME]               = "IT_PFP_SYNC_ME";
	names[IT_SURFACE_SYNC]              = "IT_SURFACE_SYNC";
	names[IT_EVENT_WRITE]               = "IT_EVENT_WRITE";
	names[IT_EVENT_WRITE_EOP]           = "IT_EVENT_WRITE_EOP";
	names[IT_EVENT_WRITE_EOS]           = "IT_EVENT_WRITE_EOS";
	names[IT_RELEASE_MEM]               = "IT_RELEASE_MEM";
	names[IT_DMA_DATA]                  = "IT_DMA_DATA";
	names[IT_ACQUIRE_MEM]               = "IT_ACQUIRE_MEM";
	names[IT_REWIND]                    = "IT_REWIND";
	names[IT_SET_SH_REG_INDIRECT]       = "IT_SET_SH_REG_INDIRECT";
	names[IT_SET_UCONFIG_REG_INDIRECT]  = "IT_SET_UCONFIG_REG_INDIRECT";
	names[IT_SET_CONFIG_REG]            = "IT_SET_CONFIG_REG";
	names[IT_SET_CONTEXT_REG]           = "IT_SET_CONTEXT_REG";
	names[IT_SET_SH_REG]                = "IT_SET_SH_REG";
	names[IT_SET_QUEUE_REG]             = "IT_SET_QUEUE_REG";
	names[IT_SET_UCONFIG_REG]           = "IT_SET_UCONFIG_REG";
	names[IT_SET_UCONFIG_REG_INDEX]     = "IT_SET_UCONFIG_REG_INDEX";
	names[IT_WRITE_CONST_RAM]           = "IT_WRITE_CONST_RAM";
	names[IT_DUMP_CONST_RAM]            = "IT_DUMP_CONST_RAM";
	names[IT_INCREMENT_CE_COUNTER]      = "IT_INCREMENT_CE_COUNTER";
	names[IT_INCREMENT_DE_COUNTER]      = "IT_INCREMENT_DE_COUNTER";
	names[IT_WAIT_ON_CE_COUNTER]        = "IT_WAIT_ON_CE_COUNTER";
	names[IT_WAIT_ON_DE_COUNTER_DIFF]   = "IT_WAIT_ON_DE_COUNTER_DIFF";
	names[IT_DISPATCH_DRAW_PREAMBLE]    = "IT_DISPATCH_DRAW_PREAMBLE";
	names[IT_DISPATCH_DRAW]             = "IT_DISPATCH_DRAW";
	names[IT_GET_LOD_STATS]             = "IT_GET_LOD_STATS";
	names[IT_SET_CONTEXT_REG_INDIRECT]  = "IT_SET_CONTEXT_REG_INDIRECT";
	return names;
}

constexpr auto g_register_names = MakeRegisterNames();
constexpr auto g_opcode_names   = MakeOpcodeNames();

} // namespace

void DumpPm4PacketStream(Common::File* file, uint32_t* cmd_buffer, uint32_t start_dw,
                         uint32_t num_dw) {
	// db_dump();

	file->Printf("----- Buffer --- dwords: 0x%05" PRIx32 ", offset : %u, addr: %016" PRIx64
	             " ----- \n",
	             num_dw, start_dw, reinterpret_cast<uint64_t>(cmd_buffer));

	auto* cmd = cmd_buffer + start_dw;
	auto  dw  = num_dw;
	while (dw != 0) {
		EXIT_NOT_IMPLEMENTED(dw < 2);
		EXIT_NOT_IMPLEMENTED(dw > num_dw);

		auto cmd_id = *cmd++;

		file->Printf("%05" PRIx32 " | 0x%08" PRIx32 " | ", start_dw, cmd_id);

		uint32_t len = 0;

		const auto packet_type = static_cast<PacketType>(cmd_id >> 30u);
		switch (packet_type) {
			case PacketType::Type3: {
				const bool    sh_gx = (cmd_id & 0x2u) == 0;
				const uint8_t op    = ((cmd_id >> 8u) & 0xffu);
				const auto    r     = KYTY_PM4_R(cmd_id);
				len                 = ((cmd_id >> 16u) & 0x3fffu) + 1;

				EXIT_NOT_IMPLEMENTED(len >= dw);

				file->Printf("%s %s(OP:0x%02" PRIx8 ") SH:%s CNT:%u\n", g_opcode_names[op],
				             (op == IT_NOP ? g_register_names[r] : ""), op, sh_gx ? "GX" : "CX",
				             len);

				for (uint32_t i = 0; i < len; i++) {
					file->Printf("      | 0x%08" PRIx32 " | \n", cmd[i]);
				}

				if ((op == IT_SET_CONTEXT_REG_INDIRECT || op == IT_SET_SH_REG_INDIRECT ||
				     op == IT_SET_UCONFIG_REG_INDIRECT) &&
				    len == 4) {
					auto* indirect_buffer =
					    reinterpret_cast<uint32_t*>((static_cast<uint64_t>(cmd[0]) & 0xfffffffcu) |
					                                (static_cast<uint64_t>(cmd[1]) << 32u));
					const uint32_t indirect_num_regs = cmd[3] & 0x3fffu;

					for (uint32_t i = 0; indirect_buffer != nullptr && i < indirect_num_regs;
					     i++, indirect_buffer += 2) {
						file->Printf("      |            | offset = 0x%08" PRIx32
						             ", value = 0x%08" PRIx32 "\n",
						             indirect_buffer[0], indirect_buffer[1]);
					}
				}
				break;
			}
			case PacketType::Type0:
			case PacketType::Type1:
			case PacketType::Type2:
				file->Printf("<unsupported TYPE%u packet>\n", static_cast<uint32_t>(packet_type));
				break;
		}

		cmd += len;
		dw -= len + 1;
		start_dw += len + 1;
	}
}

} // namespace Libs::Graphics::Pm4
