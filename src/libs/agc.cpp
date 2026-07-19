#include "libs/agc.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/virtualMemory.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "graphics/presentation/renderDoc.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/shader/shader.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace Libs::Graphics {

KYTY_SUBSYSTEM_INIT(Graphics) {
	// Some games lock up if this is not called first
	if (Config::RenderDocEnabled()) {
		RenderDocInit();
	}

	auto width  = Config::GetScreenWidth();
	auto height = Config::GetScreenHeight();

	WindowInit(width, height);
	VideoOut::VideoOutInit(width, height);
	GraphicsRenderInit();
	GraphicsRunInit();
	LabelInit();
	TileInit();
	ShaderInit();
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Graphics) {}

KYTY_SUBSYSTEM_DESTROY(Graphics) {}

void GraphicsDbgDumpDcb(const char* type, uint32_t num_dw, uint32_t* cmd_buffer) {
	EXIT_IF(type == nullptr);

	static std::atomic_int id = 0;

	if (Config::CommandBufferDumpEnabled() && num_dw > 0 && cmd_buffer != nullptr) {
		Common::File f;
		auto         file_name =
		    Config::GetCommandBufferDumpFolder() /
		    fmt::format("{:04d}_{:04d}_buffer_{}.log", GraphicsRunGetFrameNum(), id++, type);
		Common::File::CreateDirectories(file_name.parent_path());
		f.Create(file_name);
		if (f.IsInvalid()) {
			auto file_name_text = Common::PathToString(file_name);
			LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", file_name_text.c_str());
			return;
		}
		Pm4::DumpPm4PacketStream(&f, cmd_buffer, 0, num_dw);
		f.Close();
	}
}

namespace Gen5 {

LIB_NAME("Graphics5", "Graphics5");

struct RegisterDefaults {
	ShaderRegister** tbl0                = nullptr;
	ShaderRegister** tbl1                = nullptr;
	ShaderRegister** tbl2                = nullptr;
	ShaderRegister** tbl3                = nullptr;
	uint32_t         tbl0_register_count = 0;
	uint32_t         tbl1_register_count = 0;
	uint32_t         tbl2_register_count = 0;
	uint32_t         tbl3_register_count = 0;
	uint32_t*        types               = nullptr;
	uint32_t         count               = 0;
};

static_assert(offsetof(RegisterDefaults, count) == 0x38);

struct CompactRegisterDefaults {
	ShaderRegister* tbl0_regs            = nullptr;
	uint32_t        tbl0_register_count  = 0;
	const uint16_t* tbl0_pointer_offsets = nullptr;
	uint32_t        tbl0_pointer_count   = 0;
	ShaderRegister* tbl1_regs            = nullptr;
	uint32_t        tbl1_register_count  = 0;
	const uint16_t* tbl1_pointer_offsets = nullptr;
	uint32_t        tbl1_pointer_count   = 0;
	ShaderRegister* tbl2_regs            = nullptr;
	uint32_t        tbl2_register_count  = 0;
	const uint16_t* tbl2_pointer_offsets = nullptr;
	uint32_t        tbl2_pointer_count   = 0;
	ShaderRegister* tbl3_regs            = nullptr;
	uint32_t        tbl3_register_count  = 0;
	const uint16_t* tbl3_pointer_offsets = nullptr;
	uint32_t        tbl3_pointer_count   = 0;
	uint32_t*       types                = nullptr;
	uint32_t        count                = 0;
};

struct RegisterDefaultsStorage {
	RegisterDefaults             defaults;
	std::vector<ShaderRegister*> tbl0;
	std::vector<ShaderRegister*> tbl1;
	std::vector<ShaderRegister*> tbl2;
	std::vector<ShaderRegister*> tbl3;
	bool                         initialized = false;
};

#include "libs/agcRegisterDefaults.inc"

#include <fmt/format.h>

static constexpr uint32_t GRAPHICS_REGISTER_DEFAULTS_MAX_VERSION      = 12;
static constexpr uint32_t GRAPHICS_REGISTER_DEFAULTS_FALLBACK_VERSION = 11;
static constexpr uint32_t GRAPHICS_INIT_NO_FEATURE_STATE              = 0;

static std::mutex g_register_defaults_mutex;

static ShaderRegister** init_register_default_pointer_table(std::vector<ShaderRegister*>* table,
                                                            ShaderRegister*               regs,
                                                            uint32_t        register_count,
                                                            const uint16_t* offsets,
                                                            uint32_t        pointer_count) {
	EXIT_IF(table == nullptr);

	table->clear();
	if (pointer_count == 0) {
		return nullptr;
	}

	EXIT_IF(regs == nullptr);
	EXIT_IF(offsets == nullptr);

	table->resize(pointer_count);
	for (uint32_t i = 0; i < pointer_count; i++) {
		EXIT_IF(offsets[i] >= register_count);
		(*table)[i] = regs + offsets[i];
	}

	return table->data();
}

static RegisterDefaults* get_register_defaults(CompactRegisterDefaults* compact,
                                               RegisterDefaultsStorage* storage) {
	EXIT_IF(compact == nullptr);
	EXIT_IF(storage == nullptr);

	std::lock_guard lock(g_register_defaults_mutex);
	if (!storage->initialized) {
		storage->defaults.tbl0 = init_register_default_pointer_table(
		    &storage->tbl0, compact->tbl0_regs, compact->tbl0_register_count,
		    compact->tbl0_pointer_offsets, compact->tbl0_pointer_count);
		storage->defaults.tbl1 = init_register_default_pointer_table(
		    &storage->tbl1, compact->tbl1_regs, compact->tbl1_register_count,
		    compact->tbl1_pointer_offsets, compact->tbl1_pointer_count);
		storage->defaults.tbl2 = init_register_default_pointer_table(
		    &storage->tbl2, compact->tbl2_regs, compact->tbl2_register_count,
		    compact->tbl2_pointer_offsets, compact->tbl2_pointer_count);
		storage->defaults.tbl3 = init_register_default_pointer_table(
		    &storage->tbl3, compact->tbl3_regs, compact->tbl3_register_count,
		    compact->tbl3_pointer_offsets, compact->tbl3_pointer_count);
		storage->defaults.tbl0_register_count = compact->tbl0_register_count;
		storage->defaults.tbl1_register_count = compact->tbl1_register_count;
		storage->defaults.tbl2_register_count = compact->tbl2_register_count;
		storage->defaults.tbl3_register_count = compact->tbl3_register_count;
		storage->defaults.types               = compact->types;
		storage->defaults.count               = compact->count;
		storage->initialized                  = true;
	}

	return &storage->defaults;
}

static uint32_t normalize_register_defaults_version(uint32_t ver) {
	return ver <= GRAPHICS_REGISTER_DEFAULTS_MAX_VERSION
	           ? ver
	           : GRAPHICS_REGISTER_DEFAULTS_FALLBACK_VERSION;
}

static RegisterDefaults* get_public_register_defaults(uint32_t ver) {
	static RegisterDefaultsStorage storage[GRAPHICS_REGISTER_DEFAULTS_MAX_VERSION + 1] = {};
	const auto                     index = normalize_register_defaults_version(ver);
	return get_register_defaults(g_agc_public_reg_defaults_by_version[index], &storage[index]);
}

static RegisterDefaults* get_internal_register_defaults(uint32_t ver) {
	static RegisterDefaultsStorage storage[GRAPHICS_REGISTER_DEFAULTS_MAX_VERSION + 1] = {};
	const auto                     index = normalize_register_defaults_version(ver);
	return get_register_defaults(g_agc_internal_reg_defaults_by_version[index], &storage[index]);
}

struct PendingGraphicsSegment {
	uint32_t* start     = nullptr;
	uint32_t* end       = nullptr;
	uint32_t* range_end = nullptr;
};

static std::mutex             g_pending_graphics_segment_mutex;
static PendingGraphicsSegment g_pending_graphics_segment;

static void track_pending_graphics_segment_after_submit(uint32_t* dcb, uint32_t size_in_dwords) {
	if (dcb == nullptr || size_in_dwords == 0) {
		return;
	}

	auto* segment_start = dcb + size_in_dwords;
	auto* range_end     = segment_start + 0xfffffu;

	std::lock_guard lock(g_pending_graphics_segment_mutex);
	g_pending_graphics_segment.start     = segment_start;
	g_pending_graphics_segment.end       = segment_start;
	g_pending_graphics_segment.range_end = range_end;
}

static void track_pending_graphics_allocation(uint32_t* cmd, uint32_t size_dw) {
	if (cmd == nullptr || size_dw == 0) {
		return;
	}

	std::lock_guard lock(g_pending_graphics_segment_mutex);
	auto*           range_start = g_pending_graphics_segment.start;
	auto*           range_end   = g_pending_graphics_segment.range_end;
	if (range_start == nullptr || range_end == nullptr || cmd < range_start || cmd >= range_end) {
		return;
	}

	auto* cmd_end = cmd + size_dw;
	if (cmd > g_pending_graphics_segment.end) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 64) {
			LOGF("\t pending graphics segment: ignoring non-contiguous allocation cmd = "
			     "0x%016" PRIx64 ", tracked_end = 0x%016" PRIx64 "\n",
			     reinterpret_cast<uint64_t>(cmd),
			     reinterpret_cast<uint64_t>(g_pending_graphics_segment.end));
		}
		return;
	}
	if (cmd_end > g_pending_graphics_segment.end && cmd_end <= range_end) {
		g_pending_graphics_segment.end = cmd_end;
	}
}

struct CommandBuffer {
	using Callback = KYTY_SYSV_ABI bool (*)(CommandBuffer*, uint32_t, void*);

	uint32_t* bottom;
	uint32_t* top;
	uint32_t* cursor_up;
	uint32_t* cursor_down;
	Callback  callback;
	void*     user_data;
	uint32_t  reserved_dw;

	void DbgDump() const {
		if (!Config::GraphicsDebugDumpEnabled() ||
		    Config::GetPrintfDirection() == Config::OutputDirection::Silent) {
			return;
		}
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) >= 64) {
			return;
		}

		LOGF("\t bottom      = 0x%016" PRIx64 "\n"
		     "\t top         = 0x%016" PRIx64 "\n"
		     "\t cursor_up   = 0x%016" PRIx64 "\n"
		     "\t cursor_down = 0x%016" PRIx64 "\n"
		     "\t callback    = 0x%016" PRIx64 "\n"
		     "\t user_data   = 0x%016" PRIx64 "\n"
		     "\t reserved_dw = %" PRIu32 "\n",
		     reinterpret_cast<uint64_t>(bottom), reinterpret_cast<uint64_t>(top),
		     reinterpret_cast<uint64_t>(cursor_up), reinterpret_cast<uint64_t>(cursor_down),
		     reinterpret_cast<uint64_t>(callback), reinterpret_cast<uint64_t>(user_data),
		     reserved_dw);
	}

	[[nodiscard]] KYTY_SYSV_ABI uint32_t GetAvailableSizeDW() const {
		if (cursor_up == nullptr || cursor_down == nullptr || cursor_down <= cursor_up) {
			return 0;
		}

		auto available = static_cast<uint64_t>(cursor_down - cursor_up);
		if (available <= reserved_dw) {
			return 0;
		}
		if (available - reserved_dw > UINT32_MAX) {
			LOGF_COLOR(
			    Log::Color::Red,
			    "\t command buffer has suspiciously large free space: cursor_up = 0x%016" PRIx64
			    ", cursor_down = 0x%016" PRIx64 ", reserved_dw = %" PRIu32 "\n",
			    reinterpret_cast<uint64_t>(cursor_up), reinterpret_cast<uint64_t>(cursor_down),
			    reserved_dw);
			return UINT32_MAX;
		}
		return static_cast<uint32_t>(available - reserved_dw);
	}

	KYTY_SYSV_ABI bool ReserveDW(uint32_t num_dw) {
		uint32_t remaining = GetAvailableSizeDW();
		if (num_dw > remaining) {
			if (callback == nullptr) {
				LOGF_COLOR(
				    Log::Color::Red,
				    "\t command buffer exhausted and has no grow callback: requested = %" PRIu32
				    ", remaining = %" PRIu32 ", reserved_dw = %" PRIu32 "\n",
				    num_dw, remaining, reserved_dw);
				DbgDump();
				return false;
			}

			bool result = callback(this, num_dw + reserved_dw, user_data);
			if (!result) {
				LOGF_COLOR(Log::Color::Red,
				           "\t command buffer grow callback failed: requested = %" PRIu32
				           ", remaining = %" PRIu32 ", reserved_dw = %" PRIu32 "\n",
				           num_dw, remaining, reserved_dw);
				DbgDump();
				return false;
			}
			if (GetAvailableSizeDW() < num_dw) {
				LOGF_COLOR(Log::Color::Red,
				           "\t command buffer grow callback did not provide enough space: "
				           "requested = %" PRIu32 ", remaining = %" PRIu32
				           ", reserved_dw = %" PRIu32 "\n",
				           num_dw, GetAvailableSizeDW(), reserved_dw);
				DbgDump();
				return false;
			}
		}
		return true;
	}

	KYTY_SYSV_ABI uint32_t* AllocateDW(uint32_t size_dw) {
		if (size_dw == 0 || !ReserveDW(size_dw)) {
			LOGF_COLOR(Log::Color::Red,
			           "\t command buffer AllocateDW failed: size_dw = %" PRIu32 "\n", size_dw);
			return nullptr;
		}
		auto* ret_ptr = cursor_up;
		cursor_up += size_dw;
		track_pending_graphics_allocation(ret_ptr, size_dw);
		return ret_ptr;
	}
};

struct Label {
	volatile uint64_t m_value;
	uint64_t          m_reserved[3];
};

int KYTY_SYSV_ABI GraphicsInit(uint32_t* state, uint32_t ver) {
	PRINT_NAME();

	LOGF("\t state = 0x%016" PRIx64 "\n"
	     "\t ver   = %u\n",
	     reinterpret_cast<uint64_t>(state), ver);

	if (state == nullptr) {
		LOGF_COLOR(Log::Color::Red, "\t state is null\n");
		return OK;
	}

	if (ver > GRAPHICS_REGISTER_DEFAULTS_MAX_VERSION) {
		LOGF_COLOR(Log::Color::Red, "\t unsupported version %u\n", ver);
	}

	printf("version = %u\n", ver);

	state[0] = ver;
	state[1] = GRAPHICS_INIT_NO_FEATURE_STATE;

	return OK;
}

void* KYTY_SYSV_ABI GraphicsGetRegisterDefaults2(uint32_t ver) {
	PRINT_NAME();

	if (ver > GRAPHICS_REGISTER_DEFAULTS_MAX_VERSION) {
		LOGF_COLOR(Log::Color::Red, "\t unsupported version %u\n", ver);
	}
	return get_public_register_defaults(ver);
}

void* KYTY_SYSV_ABI GraphicsGetRegisterDefaults2Internal(uint32_t ver) {
	PRINT_NAME();

	if (ver > GRAPHICS_REGISTER_DEFAULTS_MAX_VERSION) {
		LOGF_COLOR(Log::Color::Red, "\t unsupported version %u\n", ver);
	}
	return get_internal_register_defaults(ver);
}

static void dbg_dump_shader(const Shader* h) {
	LOGF("\t file_header  = 0x%08" PRIx32 "\n"
	     "\t version      = 0x%08" PRIx32 "\n"
	     "\t user_data    = 0x%016" PRIx64 "\n",
	     h->file_header, h->version, reinterpret_cast<uint64_t>(h->user_data));
	if (h->user_data != nullptr) {
		LOGF("\t\t direct_resource_count    = 0x%04" PRIx16 "\n"
		     "\t\t direct_resource_offset   = 0x%016" PRIx64 "\n",
		     h->user_data->direct_resource_count,
		     reinterpret_cast<uint64_t>(h->user_data->direct_resource_offset));
		for (int i = 0; i < static_cast<int>(h->user_data->direct_resource_count); i++) {
			LOGF("\t\t\t offset[%02d] = %04" PRIx16 "\n", i,
			     h->user_data->direct_resource_offset[i]);
		}
		for (int imm = 0; imm < 4; imm++) {
			LOGF("\t\t sharp_resource_count  [%d] = 0x%04" PRIx16 "\n", imm,
			     h->user_data->sharp_resource_count[imm]);
			LOGF("\t\t sharp_resource_offset [%d] = 0x%016" PRIx64 "\n", imm,
			     reinterpret_cast<uint64_t>(h->user_data->sharp_resource_offset[imm]));
			for (int i = 0; i < static_cast<int>(h->user_data->sharp_resource_count[imm]); i++) {
				LOGF("\t\t\t offset_dw[%d] = %04" PRIx16 ", size = %" PRIu16 "\n", i,
				     static_cast<uint16_t>(h->user_data->sharp_resource_offset[imm][i].offset_dw),
				     static_cast<uint16_t>(h->user_data->sharp_resource_offset[imm][i].size));
			}
		}
		LOGF("\t\t eud_size_dw    = 0x%04" PRIx16 "\n"
		     "\t\t srt_size_dw    = 0x%04" PRIx16 "\n",
		     h->user_data->eud_size_dw, h->user_data->srt_size_dw);
	}
	LOGF("\t code             = 0x%016" PRIx64 "\n"
	     "\t num_cx_registers = 0x%02" PRIx8 "\n"
	     "\t cx_registers     = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(h->code), h->num_cx_registers,
	     reinterpret_cast<uint64_t>(h->cx_registers));
	for (int i = 0; i < static_cast<int>(h->num_cx_registers); i++) {
		LOGF("\t\t cx[%d]: offset = %08" PRIx32 ", value = %08" PRIx32 "\n", i,
		     h->cx_registers[i].offset, h->cx_registers[i].value);
	}
	LOGF("\t num_sh_registers = 0x%02" PRIx8 "\n"
	     "\t sh_registers     = 0x%016" PRIx64 "\n",
	     h->num_sh_registers, reinterpret_cast<uint64_t>(h->sh_registers));
	for (int i = 0; i < static_cast<int>(h->num_sh_registers); i++) {
		LOGF("\t\t sh[%d]: offset = %08" PRIx32 ", value = %08" PRIx32 "\n", i,
		     h->sh_registers[i].offset, h->sh_registers[i].value);
	}
	LOGF("\t specials                          = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(h->specials));
	LOGF("\t\t ge_cntl:              offset = %08" PRIx32 ", value = %08" PRIx32 "\n",
	     h->specials->ge_cntl.offset, h->specials->ge_cntl.value);
	LOGF("\t\t vgt_shader_stages_en: offset = %08" PRIx32 ", value = %08" PRIx32 "\n",
	     h->specials->vgt_shader_stages_en.offset, h->specials->vgt_shader_stages_en.value);
	LOGF("\t\t vgt_gs_out_prim_type: offset = %08" PRIx32 ", value = %08" PRIx32 "\n",
	     h->specials->vgt_gs_out_prim_type.offset, h->specials->vgt_gs_out_prim_type.value);
	LOGF("\t\t ge_user_vgpr_en:      offset = %08" PRIx32 ", value = %08" PRIx32 "\n",
	     h->specials->ge_user_vgpr_en.offset, h->specials->ge_user_vgpr_en.value);
	LOGF("\t\t dispatch_modifier = %08" PRIx32 "\n", h->specials->dispatch_modifier);
	LOGF("\t\t user_data_range: start = %08" PRIx32 ", end = %08" PRIx32 "\n",
	     h->specials->user_data_range.start, h->specials->user_data_range.end);
	LOGF("\t\t draw_modifier: enbl_start_vertex_offset   = %08" PRIx32 "\n"
	     "\t\t draw_modifier: enbl_start_index_offset    = %08" PRIx32 "\n"
	     "\t\t draw_modifier: enbl_start_instance_offset = %08" PRIx32 "\n"
	     "\t\t draw_modifier: enbl_draw_index            = %08" PRIx32 "\n"
	     "\t\t draw_modifier: enbl_user_vgprs            = %08" PRIx32 "\n"
	     "\t\t draw_modifier: render_target_slice_offset = %08" PRIx32 "\n"
	     "\t\t draw_modifier: fuse_draws                 = %08" PRIx32 "\n"
	     "\t\t draw_modifier: compiler_flags             = %08" PRIx32 "\n"
	     "\t\t draw_modifier: is_default                 = %08" PRIx32 "\n"
	     "\t\t draw_modifier: reserved                   = %08" PRIx32 "\n"
	     "\t num_input_semantics               = 0x%08" PRIx32 "\n"
	     "\t input_semantics                   = 0x%016" PRIx64 "\n",
	     static_cast<uint32_t>(h->specials->draw_modifier.enbl_start_vertex_offset),
	     static_cast<uint32_t>(h->specials->draw_modifier.enbl_start_index_offset),
	     static_cast<uint32_t>(h->specials->draw_modifier.enbl_start_instance_offset),
	     static_cast<uint32_t>(h->specials->draw_modifier.enbl_draw_index),
	     static_cast<uint32_t>(h->specials->draw_modifier.enbl_user_vgprs),
	     static_cast<uint32_t>(h->specials->draw_modifier.render_target_slice_offset),
	     static_cast<uint32_t>(h->specials->draw_modifier.fuse_draws),
	     static_cast<uint32_t>(h->specials->draw_modifier.compiler_flags),
	     static_cast<uint32_t>(h->specials->draw_modifier.is_default),
	     static_cast<uint32_t>(h->specials->draw_modifier.reserved), h->num_input_semantics,
	     reinterpret_cast<uint64_t>(h->input_semantics));
	for (int i = 0; i < static_cast<int>(h->num_input_semantics); i++) {
		LOGF("\t\t input_semantics[%d]: semantic         = %08" PRIx32 "\n"
		     "\t\t input_semantics[%d]: hardware_mapping = %08" PRIx32 "\n"
		     "\t\t input_semantics[%d]: size_in_elements = %08" PRIx32 "\n"
		     "\t\t input_semantics[%d]: is_f16           = %08" PRIx32 "\n"
		     "\t\t input_semantics[%d]: is_flat_shaded   = %08" PRIx32 "\n"
		     "\t\t input_semantics[%d]: is_linear        = %08" PRIx32 "\n"
		     "\t\t input_semantics[%d]: is_custom        = %08" PRIx32 "\n"
		     "\t\t input_semantics[%d]: static_vb_index  = %08" PRIx32 "\n"
		     "\t\t input_semantics[%d]: static_attribute = %08" PRIx32 "\n"
		     "\t\t input_semantics[%d]: reserved         = %08" PRIx32 "\n"
		     "\t\t input_semantics[%d]: default_value    = %08" PRIx32 "\n"
		     "\t\t input_semantics[%d]: default_value_hi = %08" PRIx32 "\n",
		     i, static_cast<uint32_t>(h->input_semantics[i].semantic), i,
		     static_cast<uint32_t>(h->input_semantics[i].hardware_mapping), i,
		     static_cast<uint32_t>(h->input_semantics[i].size_in_elements), i,
		     static_cast<uint32_t>(h->input_semantics[i].is_f16), i,
		     static_cast<uint32_t>(h->input_semantics[i].is_flat_shaded), i,
		     static_cast<uint32_t>(h->input_semantics[i].is_linear), i,
		     static_cast<uint32_t>(h->input_semantics[i].is_custom), i,
		     static_cast<uint32_t>(h->input_semantics[i].static_vb_index), i,
		     static_cast<uint32_t>(h->input_semantics[i].static_attribute), i,
		     static_cast<uint32_t>(h->input_semantics[i].reserved), i,
		     static_cast<uint32_t>(h->input_semantics[i].default_value), i,
		     static_cast<uint32_t>(h->input_semantics[i].default_value_hi));
	}
	LOGF("\t num_output_semantics              = 0x%04" PRIx16 "\n"
	     "\t output_semantics                  = 0x%016" PRIx64 "\n",
	     h->num_output_semantics, reinterpret_cast<uint64_t>(h->output_semantics));
	for (int i = 0; i < static_cast<int>(h->num_output_semantics); i++) {
		LOGF("\t\t output_semantics[%d]: semantic         = %08" PRIx32 "\n"
		     "\t\t output_semantics[%d]: hardware_mapping = %08" PRIx32 "\n"
		     "\t\t output_semantics[%d]: size_in_elements = %08" PRIx32 "\n"
		     "\t\t output_semantics[%d]: is_f16           = %08" PRIx32 "\n"
		     "\t\t output_semantics[%d]: is_flat_shaded   = %08" PRIx32 "\n"
		     "\t\t output_semantics[%d]: is_linear        = %08" PRIx32 "\n"
		     "\t\t output_semantics[%d]: is_custom        = %08" PRIx32 "\n"
		     "\t\t output_semantics[%d]: static_vb_index  = %08" PRIx32 "\n"
		     "\t\t output_semantics[%d]: static_attribute = %08" PRIx32 "\n"
		     "\t\t output_semantics[%d]: reserved         = %08" PRIx32 "\n"
		     "\t\t output_semantics[%d]: default_value    = %08" PRIx32 "\n"
		     "\t\t output_semantics[%d]: default_value_hi = %08" PRIx32 "\n",
		     i, static_cast<uint32_t>(h->output_semantics[i].semantic), i,
		     static_cast<uint32_t>(h->output_semantics[i].hardware_mapping), i,
		     static_cast<uint32_t>(h->output_semantics[i].size_in_elements), i,
		     static_cast<uint32_t>(h->output_semantics[i].is_f16), i,
		     static_cast<uint32_t>(h->output_semantics[i].is_flat_shaded), i,
		     static_cast<uint32_t>(h->output_semantics[i].is_linear), i,
		     static_cast<uint32_t>(h->output_semantics[i].is_custom), i,
		     static_cast<uint32_t>(h->output_semantics[i].static_vb_index), i,
		     static_cast<uint32_t>(h->output_semantics[i].static_attribute), i,
		     static_cast<uint32_t>(h->output_semantics[i].reserved), i,
		     static_cast<uint32_t>(h->output_semantics[i].default_value), i,
		     static_cast<uint32_t>(h->output_semantics[i].default_value_hi));
	}
	LOGF("\t header_size                       = 0x%08" PRIx32 "\n"
	     "\t shader_size                       = 0x%08" PRIx32 "\n"
	     "\t embedded_constant_buffer_size_dqw = 0x%08" PRIx32 "\n"
	     "\t target                            = 0x%08" PRIx32 "\n"
	     "\t scratch_size_dw_per_thread        = 0x%04" PRIx16 "\n"
	     "\t special_sizes_bytes               = 0x%04" PRIx16 "\n"
	     "\t type                              = 0x%02" PRIx8 "\n",
	     h->header_size, h->shader_size, h->embedded_constant_buffer_size_dqw, h->target,
	     h->scratch_size_dw_per_thread, h->special_sizes_bytes, h->type);
}

static constexpr int GRAPHICS5_ERROR_INVALID_SHADER_PROGRAM = static_cast<int>(0x8a6c0005u);
static constexpr int GRAPHICS5_ERROR_INVALID_SHADER_HALVES  = static_cast<int>(0x8a6c0008u);

static bool get_shader_program_address_register(uint8_t type, uint32_t* lo_offset) {
	if (lo_offset == nullptr) {
		return false;
	}

	switch (static_cast<Prospero::ShaderBinaryType>(type)) {
		case Prospero::ShaderBinaryType::kCs: *lo_offset = Pm4::COMPUTE_PGM_LO; return true;
		case Prospero::ShaderBinaryType::kPs: *lo_offset = Pm4::SPI_SHADER_PGM_LO_PS; return true;
		case Prospero::ShaderBinaryType::kGs: *lo_offset = Pm4::SPI_SHADER_PGM_LO_ES; return true;
		case Prospero::ShaderBinaryType::kHs: *lo_offset = Pm4::SPI_SHADER_PGM_LO_LS; return true;
		case Prospero::ShaderBinaryType::kGsBack:
			*lo_offset = Pm4::SPI_SHADER_PGM_LO_GS;
			return true;
		case Prospero::ShaderBinaryType::kHsBack:
			*lo_offset = Pm4::SPI_SHADER_PGM_LO_HS;
			return true;
		case Prospero::ShaderBinaryType::kGsFront:
		case Prospero::ShaderBinaryType::kHsFront:
		case Prospero::ShaderBinaryType::kFs: return false;
	}

	return false;
}

static int patch_program_address_register(ShaderRegister* regs, uint32_t num_regs, uint8_t type,
                                          uint64_t base) {
	uint32_t lo_offset = 0;
	if (!get_shader_program_address_register(type, &lo_offset)) {
		return OK;
	}

	if (regs == nullptr || num_regs == 0) {
		return GRAPHICS5_ERROR_INVALID_SHADER_PROGRAM;
	}

	for (auto lo_index = 0u; lo_index < num_regs; lo_index++) {
		if (regs[lo_index].offset != lo_offset) {
			continue;
		}

		const auto hi_index  = lo_index + 1u;
		const auto hi_offset = lo_offset + 1u;
		if (hi_index >= num_regs || regs[hi_index].offset != hi_offset) {
			return GRAPHICS5_ERROR_INVALID_SHADER_PROGRAM;
		}

		const auto shader_offset = (static_cast<uint64_t>(regs[lo_index].value) << 8u) |
		                           ((static_cast<uint64_t>(regs[hi_index].value) & 0xffu) << 40u);
		const auto addr          = base + shader_offset;

		regs[lo_index].value = static_cast<uint32_t>((addr >> 8u) & 0xffffffffu);
		regs[hi_index].value &= 0xffffff00u;
		regs[hi_index].value |= static_cast<uint32_t>((addr >> 40u) & 0xffu);
		return OK;
	}

	return GRAPHICS5_ERROR_INVALID_SHADER_PROGRAM;
}

static ShaderRegister* find_shader_register(ShaderRegister* regs, uint32_t num_regs,
                                            uint32_t offset, uint32_t occurrence = 0) {
	if (regs == nullptr) {
		return nullptr;
	}

	for (uint32_t i = 0; i < num_regs; i++) {
		if (regs[i].offset != offset) {
			continue;
		}
		if (occurrence == 0) {
			return regs + i;
		}
		occurrence--;
	}

	return nullptr;
}

static void patch_shader_register_address(ShaderRegister* regs, uint32_t num_regs,
                                          uint32_t lo_offset, uint64_t address) {
	auto* lo = find_shader_register(regs, num_regs, lo_offset);
	if (lo == nullptr) {
		return;
	}

	auto* hi = (lo + 1 < regs + num_regs && (lo + 1)->offset == lo_offset + 1u ? lo + 1 : nullptr);
	if (hi == nullptr) {
		return;
	}

	lo->value = static_cast<uint32_t>((address >> 8u) & 0xffffffffu);
	hi->value &= 0xffffff00u;
	hi->value |= static_cast<uint32_t>((address >> 40u) & 0xffu);
}

int KYTY_SYSV_ABI GraphicsCreateShader(Shader** dst, void* header, const volatile void* code) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(dst == nullptr);
	EXIT_NOT_IMPLEMENTED(header == nullptr);
	EXIT_NOT_IMPLEMENTED(code == nullptr);

	LOGF("\t header = 0x%016" PRIx64 "\n"
	     "\t code   = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(header), reinterpret_cast<uint64_t>(code));

	auto* h = static_cast<Shader*>(header);

	auto update_addr = [](auto& m) {
		if (m != nullptr) {
			m = reinterpret_cast<typename std::remove_reference<decltype(m)>::type>(
			    reinterpret_cast<uintptr_t>(m) + reinterpret_cast<uintptr_t>(&m));
		}
	};

	update_addr(h->cx_registers);
	update_addr(h->sh_registers);
	update_addr(h->user_data);
	update_addr(h->specials);
	update_addr(h->input_semantics);
	update_addr(h->output_semantics);
	if (h->user_data != nullptr) {
		update_addr(h->user_data->direct_resource_offset);
		update_addr(h->user_data->sharp_resource_offset[0]);
		update_addr(h->user_data->sharp_resource_offset[1]);
		update_addr(h->user_data->sharp_resource_offset[2]);
		update_addr(h->user_data->sharp_resource_offset[3]);
	}

	h->code = code;

	EXIT_NOT_IMPLEMENTED(h->file_header != 0x34333231);
	EXIT_NOT_IMPLEMENTED(h->version != 0x00000018);

	auto base = reinterpret_cast<uint64_t>(code);

	LOGF("\t base   = 0x%016" PRIx64 "\n", base);

	ShaderMappedData map;
	map.user_data           = h->user_data;
	map.input_semantics     = h->input_semantics;
	map.num_input_semantics = h->num_input_semantics;
	map.code_size_bytes     = h->shader_size;

	ShaderMapUserData(base, map);

	EXIT_NOT_IMPLEMENTED((base & 0xFFFF0000000000FFull) != 0);

	auto patch_result =
	    patch_program_address_register(h->sh_registers, h->num_sh_registers, h->type, base);
	if (patch_result != OK) {
		return patch_result;
	}

	*dst = h;

	dbg_dump_shader(h);

	return OK;
}

int KYTY_SYSV_ABI GraphicsUnknownGetFusedShaderSize(SizeAlign* dst, const Shader* front,
                                                    const Shader* back) {
	PRINT_NAME();

	LOGF("\t dst   = 0x%016" PRIx64 "\n"
	     "\t front = 0x%016" PRIx64 "\n"
	     "\t back  = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(front),
	     reinterpret_cast<uint64_t>(back));

	EXIT_NOT_IMPLEMENTED(dst == nullptr);
	EXIT_NOT_IMPLEMENTED(front == nullptr);
	EXIT_NOT_IMPLEMENTED(back == nullptr);

	const auto front_type = static_cast<Prospero::ShaderBinaryType>(front->type);
	const auto back_type  = static_cast<Prospero::ShaderBinaryType>(back->type);
	if (!((front_type == Prospero::ShaderBinaryType::kGsFront &&
	       back_type == Prospero::ShaderBinaryType::kGsBack) ||
	      (front_type == Prospero::ShaderBinaryType::kHsFront &&
	       back_type == Prospero::ShaderBinaryType::kHsBack))) {
		return GRAPHICS5_ERROR_INVALID_SHADER_HALVES;
	}

	dst->m_size  = static_cast<uint64_t>(back->num_sh_registers) * sizeof(ShaderRegister);
	dst->m_align = 4;

	return OK;
}

int KYTY_SYSV_ABI GraphicsUnknownFuseShaderHalves(Shader* fused_result, const Shader* front,
                                                  const Shader* back, void* scratch_mem) {
	PRINT_NAME();

	LOGF("\t fused_result = 0x%016" PRIx64 "\n"
	     "\t front        = 0x%016" PRIx64 "\n"
	     "\t back         = 0x%016" PRIx64 "\n"
	     "\t scratch_mem  = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(fused_result), reinterpret_cast<uint64_t>(front),
	     reinterpret_cast<uint64_t>(back), reinterpret_cast<uint64_t>(scratch_mem));

	EXIT_NOT_IMPLEMENTED(fused_result == nullptr);
	EXIT_NOT_IMPLEMENTED(front == nullptr);
	EXIT_NOT_IMPLEMENTED(back == nullptr);

	const auto front_type = static_cast<Prospero::ShaderBinaryType>(front->type);
	const auto back_type  = static_cast<Prospero::ShaderBinaryType>(back->type);
	if (!((front_type == Prospero::ShaderBinaryType::kGsFront &&
	       back_type == Prospero::ShaderBinaryType::kGsBack) ||
	      (front_type == Prospero::ShaderBinaryType::kHsFront &&
	       back_type == Prospero::ShaderBinaryType::kHsBack))) {
		return GRAPHICS5_ERROR_INVALID_SHADER_HALVES;
	}

	*fused_result      = *back;
	fused_result->type = static_cast<uint8_t>(front_type == Prospero::ShaderBinaryType::kGsFront
	                                              ? Prospero::ShaderBinaryType::kGs
	                                              : Prospero::ShaderBinaryType::kHs);

	if (front->specials != nullptr && back->specials != nullptr) {
		const auto front_stages = front->specials->vgt_shader_stages_en.value;
		const auto back_stages  = back->specials->vgt_shader_stages_en.value;
		const auto mismatch_bit =
		    (front_type == Prospero::ShaderBinaryType::kGsFront ? (1u << 22u) : (1u << 21u));
		if (((front_stages ^ back_stages) & mismatch_bit) != 0) {
			return GRAPHICS5_ERROR_INVALID_SHADER_HALVES;
		}
	}

	if (scratch_mem != nullptr && back->sh_registers != nullptr && back->num_sh_registers != 0) {
		auto* sh_registers = static_cast<ShaderRegister*>(scratch_mem);
		memcpy(sh_registers, back->sh_registers,
		       static_cast<size_t>(back->num_sh_registers) * sizeof(ShaderRegister));
		fused_result->sh_registers = sh_registers;
	}

	auto*      fused_regs      = fused_result->sh_registers;
	const auto fused_reg_count = static_cast<uint32_t>(fused_result->num_sh_registers);
	const auto front_reg_count = static_cast<uint32_t>(front->num_sh_registers);

	if (front_type == Prospero::ShaderBinaryType::kGsFront) {
		for (uint32_t occurrence = 0; occurrence < 2; occurrence++) {
			auto*       dst = find_shader_register(fused_regs, fused_reg_count,
			                                       Pm4::SPI_SHADER_PGM_CHKSUM_GS, occurrence);
			const auto* src = find_shader_register(front->sh_registers, front_reg_count,
			                                       Pm4::SPI_SHADER_PGM_CHKSUM_GS, occurrence);
			if (dst != nullptr && src != nullptr) {
				dst->value = src->value;
			}
		}
		patch_shader_register_address(fused_regs, fused_reg_count, Pm4::SPI_SHADER_PGM_LO_ES,
		                              reinterpret_cast<uint64_t>(front->code));
	} else {
		patch_shader_register_address(fused_regs, fused_reg_count, Pm4::SPI_SHADER_PGM_LO_LS,
		                              reinterpret_cast<uint64_t>(front->code));
	}

	fused_result->user_data = nullptr;

	return OK;
}

static constexpr int GRAPHICS5_ERROR_INVALID_PACKET = static_cast<int>(0x8a6c000cu);

enum class RegIndirectPacket : uint32_t {
	Cx,
	Sh,
	Uc,
};

static uint32_t reg_indirect_native_op(RegIndirectPacket type) {
	switch (type) {
		case RegIndirectPacket::Cx: return Pm4::IT_SET_CONTEXT_REG_INDIRECT;
		case RegIndirectPacket::Sh: return Pm4::IT_SET_SH_REG_INDIRECT;
		case RegIndirectPacket::Uc: return Pm4::IT_SET_UCONFIG_REG_INDIRECT;
	}
	return 0;
}

static bool is_native_reg_indirect_packet(const uint32_t* cmd, RegIndirectPacket type) {
	return ((cmd[0] >> 8u) & 0xffu) == reg_indirect_native_op(type);
}

static uint32_t reg_indirect_pm4_r(RegIndirectPacket type) {
	return (type == RegIndirectPacket::Uc && GraphicsGetIsTrinityMode() != 0 ? 1u : 0u);
}

static void reg_indirect_write_packet(uint32_t* cmd, uint64_t vaddr, uint32_t num_regs,
                                      RegIndirectPacket type) {
	cmd[0] = KYTY_PM4(5, reg_indirect_native_op(type), reg_indirect_pm4_r(type));
	cmd[1] = static_cast<uint32_t>(vaddr) & 0xfffffffcu;
	cmd[2] = static_cast<uint32_t>(vaddr >> 32u);
	cmd[3] = 0x80000000u;
	cmd[4] = num_regs & 0x3fffu;
}

static int reg_indirect_patch_set_address(uint32_t* cmd, uint64_t vaddr, RegIndirectPacket type) {
	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	if (is_native_reg_indirect_packet(cmd, type)) {
		cmd[1] = (cmd[1] & 0x3u) | (static_cast<uint32_t>(vaddr) & 0xfffffffcu);
		cmd[2] = static_cast<uint32_t>(vaddr >> 32u);
		return OK;
	}

	return GRAPHICS5_ERROR_INVALID_PACKET;
}

static int reg_indirect_patch_set_num_registers(uint32_t* cmd, uint32_t num_regs,
                                                RegIndirectPacket type) {
	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	if (is_native_reg_indirect_packet(cmd, type)) {
		cmd[4] = (cmd[4] & ~0x3fffu) | (num_regs & 0x3fffu);
		return OK;
	}

	return GRAPHICS5_ERROR_INVALID_PACKET;
}

static int reg_indirect_patch_add_registers(uint32_t* cmd, uint32_t num_regs,
                                            RegIndirectPacket type) {
	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	if (is_native_reg_indirect_packet(cmd, type)) {
		auto new_num_regs = ((cmd[4] & 0x3fffu) + num_regs) & 0x3fffu;
		cmd[4]            = (cmd[4] & ~0x3fffu) | new_num_regs;
		return OK;
	}

	return GRAPHICS5_ERROR_INVALID_PACKET;
}

int KYTY_SYSV_ABI GraphicsSetCxRegIndirectPatchSetAddress(uint32_t*                      cmd,
                                                          const volatile ShaderRegister* regs) {
	PRINT_NAME();

	LOGF("\t cmd  = 0x%016" PRIx64 "\n"
	     "\t regs = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(cmd), reinterpret_cast<uint64_t>(regs));

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);
	EXIT_NOT_IMPLEMENTED(regs == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(regs);

	return reg_indirect_patch_set_address(cmd, vaddr, RegIndirectPacket::Cx);
}

int KYTY_SYSV_ABI GraphicsSetShRegIndirectPatchSetAddress(uint32_t*                      cmd,
                                                          const volatile ShaderRegister* regs) {
	PRINT_NAME();

	LOGF("\t cmd  = 0x%016" PRIx64 "\n"
	     "\t regs = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(cmd), reinterpret_cast<uint64_t>(regs));

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);
	EXIT_NOT_IMPLEMENTED(regs == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(regs);

	return reg_indirect_patch_set_address(cmd, vaddr, RegIndirectPacket::Sh);
}

int KYTY_SYSV_ABI GraphicsSetUcRegIndirectPatchSetAddress(uint32_t*                      cmd,
                                                          const volatile ShaderRegister* regs) {
	PRINT_NAME();

	LOGF("\t cmd  = 0x%016" PRIx64 "\n"
	     "\t regs = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(cmd), reinterpret_cast<uint64_t>(regs));

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);
	EXIT_NOT_IMPLEMENTED(regs == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(regs);

	return reg_indirect_patch_set_address(cmd, vaddr, RegIndirectPacket::Uc);
}

int KYTY_SYSV_ABI GraphicsSetCxRegIndirectPatchSetNumRegisters(uint32_t* cmd, uint32_t num_regs) {
	PRINT_NAME();

	LOGF("\t cmd      = 0x%016" PRIx64 "\n"
	     "\t num_regs = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(cmd), num_regs);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	return reg_indirect_patch_set_num_registers(cmd, num_regs, RegIndirectPacket::Cx);
}

int KYTY_SYSV_ABI GraphicsSetShRegIndirectPatchSetNumRegisters(uint32_t* cmd, uint32_t num_regs) {
	PRINT_NAME();

	LOGF("\t cmd      = 0x%016" PRIx64 "\n"
	     "\t num_regs = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(cmd), num_regs);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	return reg_indirect_patch_set_num_registers(cmd, num_regs, RegIndirectPacket::Sh);
}

int KYTY_SYSV_ABI GraphicsSetUcRegIndirectPatchSetNumRegisters(uint32_t* cmd, uint32_t num_regs) {
	PRINT_NAME();

	LOGF("\t cmd      = 0x%016" PRIx64 "\n"
	     "\t num_regs = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(cmd), num_regs);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	return reg_indirect_patch_set_num_registers(cmd, num_regs, RegIndirectPacket::Uc);
}

int KYTY_SYSV_ABI GraphicsSetCxRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs) {
	PRINT_NAME();

	LOGF("\t cmd      = 0x%016" PRIx64 "\n"
	     "\t num_regs = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(cmd), num_regs);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	return reg_indirect_patch_add_registers(cmd, num_regs, RegIndirectPacket::Cx);
}

int KYTY_SYSV_ABI GraphicsSetShRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs) {
	PRINT_NAME();

	LOGF("\t cmd      = 0x%016" PRIx64 "\n"
	     "\t num_regs = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(cmd), num_regs);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	return reg_indirect_patch_add_registers(cmd, num_regs, RegIndirectPacket::Sh);
}

int KYTY_SYSV_ABI GraphicsSetUcRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs) {
	PRINT_NAME();

	LOGF("\t cmd      = 0x%016" PRIx64 "\n"
	     "\t num_regs = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(cmd), num_regs);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	return reg_indirect_patch_add_registers(cmd, num_regs, RegIndirectPacket::Uc);
}

static uint32_t GraphicsPrimitiveTypeToGsOut(uint32_t prim_type) {
	switch (static_cast<Prospero::PrimitiveType>(prim_type)) {
		case Prospero::PrimitiveType::kPointList:
			return Prospero::GpuEnumValue(Prospero::GsOutputPrimitiveType::kPoints);
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kLineListAdjacency:
		case Prospero::PrimitiveType::kLineStripAdjacency:
		case Prospero::PrimitiveType::kLineLoop:
			return Prospero::GpuEnumValue(Prospero::GsOutputPrimitiveType::kLines);
		case Prospero::PrimitiveType::kRectList:
			return Prospero::GpuEnumValue(Prospero::GsOutputPrimitiveType::k2dRectangle);
		case Prospero::PrimitiveType::kRectListLegacy:
			return Prospero::GpuEnumValue(Prospero::GsOutputPrimitiveType::kRectList);
		default: return Prospero::GpuEnumValue(Prospero::GsOutputPrimitiveType::kTriangles);
	}
}

int KYTY_SYSV_ABI GraphicsCreatePrimState(ShaderRegister* cx_regs, ShaderRegister* uc_regs,
                                          const Shader* hs, const Shader* gs, uint32_t prim_type) {
	PRINT_NAME();

	LOGF("\t cx_regs   = 0x%016" PRIx64 "\n"
	     "\t uc_regs   = 0x%016" PRIx64 "\n"
	     "\t hs        = 0x%016" PRIx64 "\n"
	     "\t gs        = 0x%016" PRIx64 "\n"
	     "\t prim_type = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(cx_regs), reinterpret_cast<uint64_t>(uc_regs),
	     reinterpret_cast<uint64_t>(hs), reinterpret_cast<uint64_t>(gs), prim_type);

	if (cx_regs == nullptr && uc_regs == nullptr) {
		return OK;
	}

	EXIT_NOT_IMPLEMENTED(gs == nullptr);

	EXIT_NOT_IMPLEMENTED(hs != nullptr && static_cast<Prospero::ShaderBinaryType>(hs->type) !=
	                                          Prospero::ShaderBinaryType::kHs);
	EXIT_NOT_IMPLEMENTED(static_cast<Prospero::ShaderBinaryType>(gs->type) !=
	                     Prospero::ShaderBinaryType::kGs);

	if (cx_regs != nullptr) {
		EXIT_NOT_IMPLEMENTED(hs != nullptr && hs->specials->vgt_shader_stages_en.offset !=
		                                          Pm4::VGT_SHADER_STAGES_EN);
		EXIT_NOT_IMPLEMENTED(hs != nullptr && hs->specials->vgt_gs_out_prim_type.offset !=
		                                          Pm4::VGT_GS_OUT_PRIM_TYPE);
		EXIT_NOT_IMPLEMENTED(gs->specials->vgt_shader_stages_en.offset !=
		                     Pm4::VGT_SHADER_STAGES_EN);
		EXIT_NOT_IMPLEMENTED(gs->specials->vgt_gs_out_prim_type.offset !=
		                     Pm4::VGT_GS_OUT_PRIM_TYPE);

		cx_regs[0] = gs->specials->vgt_shader_stages_en;
		if ((cx_regs[0].value & 0x20u) != 0) {
			cx_regs[1] = gs->specials->vgt_gs_out_prim_type;
		} else {
			cx_regs[1].offset = Pm4::VGT_GS_OUT_PRIM_TYPE;
			cx_regs[1].value  = GraphicsPrimitiveTypeToGsOut(prim_type);
		}

		if (hs != nullptr) {
			cx_regs[0].value |= hs->specials->vgt_shader_stages_en.value;
			if ((cx_regs[0].value & 0x20u) == 0) {
				cx_regs[1] = hs->specials->vgt_gs_out_prim_type;
			}
		}
	}

	if (uc_regs != nullptr) {
		EXIT_NOT_IMPLEMENTED(gs->specials->ge_cntl.offset != Pm4::GE_CNTL);
		EXIT_NOT_IMPLEMENTED(gs->specials->ge_user_vgpr_en.offset != Pm4::GE_USER_VGPR_EN);
		EXIT_NOT_IMPLEMENTED(hs != nullptr &&
		                     hs->specials->ge_user_vgpr_en.offset != Pm4::GE_USER_VGPR_EN);

		uc_regs[0]        = gs->specials->ge_cntl;
		uc_regs[1]        = gs->specials->ge_user_vgpr_en;
		uc_regs[2].offset = Pm4::VGT_PRIMITIVE_TYPE;
		uc_regs[2].value  = prim_type;

		if (hs != nullptr) {
			uc_regs[1] = hs->specials->ge_user_vgpr_en;
		}
	}

	return OK;
}

int KYTY_SYSV_ABI GraphicsUpdatePrimState(ShaderRegister* cx_regs, ShaderRegister* uc_regs,
                                          uint32_t prim_type) {
	PRINT_NAME();

	LOGF("\t cx_regs   = 0x%016" PRIx64 "\n"
	     "\t uc_regs   = 0x%016" PRIx64 "\n"
	     "\t prim_type = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(cx_regs), reinterpret_cast<uint64_t>(uc_regs), prim_type);

	if (cx_regs != nullptr && (cx_regs[0].value & 0x24u) == 0) {
		cx_regs[1].value &= ~0x7u;
		cx_regs[1].value |= GraphicsPrimitiveTypeToGsOut(prim_type);
	}

	if (uc_regs != nullptr) {
		uc_regs[2].value &= ~0x1fu;
		uc_regs[2].value |= prim_type;
	}

	return OK;
}

static uint32_t shader_semantic_word(const ShaderSemantic& semantic) {
	return ((semantic.semantic & 0xffu) << 0u) | ((semantic.hardware_mapping & 0xffu) << 8u) |
	       ((semantic.size_in_elements & 0xfu) << 16u) | ((semantic.is_f16 & 0x3u) << 20u) |
	       ((semantic.is_flat_shaded & 0x1u) << 22u) | ((semantic.is_linear & 0x1u) << 23u) |
	       ((semantic.is_custom & 0x1u) << 24u) | ((semantic.static_vb_index & 0x1u) << 25u) |
	       ((semantic.static_attribute & 0x1u) << 26u) | ((semantic.reserved & 0x1u) << 27u) |
	       ((semantic.default_value & 0x3u) << 28u) | ((semantic.default_value_hi & 0x3u) << 30u);
}

static uint32_t apply_interpolant_default_value(uint32_t value, uint32_t ps_word) {
	value &= ~0x00000300u;
	value |= ((ps_word >> 28u) & 0x3u) << 8u;
	return value;
}

static uint32_t apply_interpolant_default_value_hi(uint32_t value, uint32_t ps_word) {
	value &= ~0x00600000u;
	value |= ((ps_word >> 30u) & 0x3u) << 21u;
	return value;
}

static uint32_t create_interpolant_mapping_value(uint32_t value, uint32_t ps_word,
                                                 uint32_t gs_word) {
	const uint32_t flat_shade =
	    ((ps_word & 0x00400000u) != 0 || (ps_word & 0x01000000u) != 0 ? 0x00000400u : 0u);

	value &= ~0x0000001fu;
	value |= (gs_word >> 8u) & 0x1fu;
	value &= ~0x00000400u;
	value |= flat_shade;

	return apply_interpolant_default_value(value, ps_word);
}

static uint32_t create_interpolant_default_value(uint32_t value, uint32_t ps_word) {
	value &= ~0x0000001fu;
	value &= ~0x00000400u;

	return apply_interpolant_default_value(value, ps_word);
}

static uint32_t create_interpolant_f16_value(uint32_t ps_word, const ShaderSemantic* gs_semantic) {
	uint32_t value = (ps_word << 4u) & 0x03000000u;

	if (gs_semantic == nullptr) {
		value |= 0x00180020u;
	} else {
		const uint32_t common_word = ps_word & shader_semantic_word(*gs_semantic);

		value &= 0xfff7ffdfu;
		value |= (common_word >> 15u) & 0x20u;
		value ^= 0x00080020u;
		value &= ~0x00100000u;
		value |= (~common_word >> 1u) & 0x00100000u;
	}

	return apply_interpolant_default_value_hi(value, ps_word);
}

static uint32_t create_interpolant_non_f16_value(uint32_t              ps_word,
                                                 const ShaderSemantic* gs_semantic) {
	uint32_t value = 0;
	if ((ps_word & 0x01000000u) != 0 || gs_semantic == nullptr) {
		value |= 0x20u;
	}

	return value;
}

static const ShaderSemantic* find_interpolant_output_semantic(const Shader* gs, uint32_t semantic) {
	if (gs == nullptr || gs->output_semantics == nullptr) {
		return nullptr;
	}

	for (uint16_t i = 0; i < gs->num_output_semantics; i++) {
		if (gs->output_semantics[i].semantic == semantic) {
			return &gs->output_semantics[i];
		}
	}

	return nullptr;
}

static void set_interpolant_register(ShaderRegister* regs, uint32_t index, uint32_t value) {
	// The native implementation writes a descriptor offset (0x10000000 + index). Kyty stores
	// decoded PM4 register offsets here because direct/custom indirect paths consume them.
	regs[index].offset = Pm4::SPI_PS_INPUT_CNTL_0 + index;
	regs[index].value  = value;
}

static void set_identity_interpolant_registers(ShaderRegister* regs, uint32_t first_index) {
	for (uint32_t i = first_index; i < 32u; i++) {
		set_interpolant_register(regs, i, i);
	}
}

int KYTY_SYSV_ABI GraphicsCreateInterpolantMapping(ShaderRegister* regs, const Shader* gs,
                                                   const Shader* ps) {
	PRINT_NAME();

	LOGF("\t regs = 0x%016" PRIx64 "\n"
	     "\t gs   = 0x%016" PRIx64 "\n"
	     "\t ps   = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(regs), reinterpret_cast<uint64_t>(gs),
	     reinterpret_cast<uint64_t>(ps));

	EXIT_NOT_IMPLEMENTED(regs == nullptr);
	EXIT_NOT_IMPLEMENTED(ps != nullptr && ps->num_input_semantics != 0 &&
	                     ps->input_semantics == nullptr);

	if (ps == nullptr || ps->num_input_semantics == 0) {
		set_identity_interpolant_registers(regs, 0);
		return OK;
	}

	EXIT_NOT_IMPLEMENTED(gs == nullptr);
	EXIT_NOT_IMPLEMENTED(gs->num_output_semantics != 0 && gs->output_semantics == nullptr);

	for (uint32_t ps_index = 0; ps_index < ps->num_input_semantics; ps_index++) {
		const auto& ps_semantic = ps->input_semantics[ps_index];
		const auto* gs_semantic = find_interpolant_output_semantic(gs, ps_semantic.semantic);
		const auto  ps_word     = shader_semantic_word(ps_semantic);

		auto value =
		    ((ps_word & 0x00300000u) != 0 ? create_interpolant_f16_value(ps_word, gs_semantic)
		                                  : create_interpolant_non_f16_value(ps_word, gs_semantic));
		value = (gs_semantic == nullptr ? create_interpolant_default_value(value, ps_word)
		                                : create_interpolant_mapping_value(
		                                      value, ps_word, shader_semantic_word(*gs_semantic)));

		set_interpolant_register(regs, ps_index, value);
	}

	set_identity_interpolant_registers(regs, ps->num_input_semantics);

	return OK;
}

int KYTY_SYSV_ABI GraphicsGetDataPacketPayloadAddress(uint32_t** addr, uint32_t* cmd, int type) {
	PRINT_NAME();

	static std::atomic<uint32_t> log_count {0};
	if (log_count.fetch_add(1) < 64) {
		LOGF("\t addr = 0x%016" PRIx64 "\n"
		     "\t cmd  = 0x%016" PRIx64 "\n"
		     "\t type = %d\n",
		     reinterpret_cast<uint64_t>(addr), reinterpret_cast<uint64_t>(cmd), type);
	}

	EXIT_NOT_IMPLEMENTED(addr == nullptr);

	if (type != 0) {
		*addr = cmd + 2;
		return OK;
	}

	auto cmd_id = cmd[0];
	*addr       = ((~cmd_id & 0x3fff0000u) != 0 ? cmd + 1 : nullptr);

	return OK;
}

int KYTY_SYSV_ABI GraphicsGetDataPacketPayloadRange(MemoryRange* range, uint32_t* cmd, int type) {
	PRINT_NAME();

	static std::atomic<uint32_t> log_count {0};
	const bool                   should_log = (log_count.fetch_add(1) < 64);
	if (should_log) {
		LOGF("\t range = 0x%016" PRIx64 "\n"
		     "\t cmd   = 0x%016" PRIx64 "\n"
		     "\t type  = %d\n",
		     reinterpret_cast<uint64_t>(range), reinterpret_cast<uint64_t>(cmd), type);
	}

	EXIT_NOT_IMPLEMENTED(range == nullptr);
	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	const auto cmd_id = cmd[0];
	const auto len    = KYTY_PM4_LEN(cmd_id);

	if (type != 0) {
		range->m_base = cmd + 2;
		range->m_size = static_cast<uint64_t>((cmd_id >> 14u) & 0xfffcu);
	} else {
		if ((~cmd_id & 0x3fff0000u) == 0) {
			range->m_base = nullptr;
			range->m_size = 0;
		} else {
			range->m_base = cmd + 1;
			range->m_size = static_cast<uint64_t>((cmd_id >> 14u) & 0xfffcu) + sizeof(uint32_t);
		}
	}

	if (should_log) {
		LOGF("\t cmd_id = 0x%08" PRIx32 ", len = %" PRIu32 "\n"
		     "\t base   = 0x%016" PRIx64 "\n"
		     "\t size   = 0x%016" PRIx64 "\n",
		     cmd_id, len, reinterpret_cast<uint64_t>(range->m_base), range->m_size);
	}

	return OK;
}

int KYTY_SYSV_ABI GraphicsWriteDataPatchSetAddressOrOffset(uint32_t* cmd,
                                                           uint64_t  address_or_offset) {
	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto op = (cmd[0] >> 8u) & 0xffu;
	if (op == Pm4::IT_WRITE_DATA) {
		cmd[2] = static_cast<uint32_t>(address_or_offset & 0xffffffffu);
		cmd[3] = static_cast<uint32_t>((address_or_offset >> 32u) & 0xffffffffu);
		return OK;
	}

	return static_cast<int>(0x8a6c000cu);
}

int KYTY_SYSV_ABI GraphicsUnknownJumpPatchSetTarget(uint32_t* cmd, const volatile uint32_t* target,
                                                    uint32_t size_in_dwords) {
	PRINT_NAME();

	LOGF("Calling unknown stub");

	return GraphicsJumpPatchSetTarget(cmd, target, size_in_dwords);
}

int KYTY_SYSV_ABI GraphicsJumpPatchSetTarget(uint32_t* cmd, const volatile uint32_t* target,
                                             uint32_t size_in_dwords) {
	PRINT_NAME();

	LOGF("\t cmd            = 0x%016" PRIx64 "\n"
	     "\t target         = 0x%016" PRIx64 "\n"
	     "\t size_in_dwords = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(cmd), reinterpret_cast<uint64_t>(target), size_in_dwords);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto cmd_id = cmd[0];
	auto op     = (cmd_id >> 8u) & 0xffu;

	LOGF("\t cmd_id         = 0x%08" PRIx32 ", op = 0x%02" PRIx32 ", len = %" PRIu32 "\n", cmd_id,
	     op, KYTY_PM4_LEN(cmd_id));

	if (op != Pm4::IT_INDIRECT_BUFFER) {
		return GRAPHICS5_ERROR_INVALID_PACKET;
	}

	auto vaddr = reinterpret_cast<uint64_t>(target);

	cmd[1] = static_cast<uint32_t>(vaddr & 0xffffffffu);
	cmd[2] = (cmd[2] & 0xffff0000u) | static_cast<uint32_t>((vaddr >> 32u) & 0xffffu);
	cmd[3] = (cmd[3] & 0xfff00000u) | (size_in_dwords & 0xfffffu);

	return OK;
}

int KYTY_SYSV_ABI GraphicsSuspendPoint() {
	PRINT_NAME();

	GraphicsRunDone();

	return OK;
}

uint32_t* KYTY_SYSV_ABI GraphicsUnknownQj7QZpgr9Uw(CommandBuffer* buf, uint32_t mode,
                                                   uint32_t value) {
	PRINT_NAME();
	// ResetContextState?
	LOGF("\t mode  = 0x%08" PRIx32 "\n"
	     "\t value = 0x%08" PRIx32 "\n",
	     mode, value);

	if (buf == nullptr) {
		return nullptr;
	}

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(1);

	if (cmd == nullptr) {
		return nullptr;
	}

	cmd[0] = 0x80000000u;

	return cmd;
}

uint64_t KYTY_SYSV_ABI GraphicsGetIsTrinityMode() {
	// PS5 Pro?
	return 0;
}

static constexpr int      GRAPHICS5_DRIVER_ERROR_INVALID_VALUE    = static_cast<int>(0x8a6c0033u);
static constexpr int      GRAPHICS5_DRIVER_ERROR_INVALID_ARGUMENT = static_cast<int>(0x8a6c0035u);
static constexpr uint32_t WORKLOAD_STREAM_RECORD_SIZE             = 32;
static constexpr uint32_t WORKLOAD_ACTIVE_PACKET_SIZE_DW          = 18;
static constexpr uint32_t WORKLOAD_COMPLETE_PACKET_SIZE_DW        = 12;
static constexpr uint32_t WORKLOAD_STREAM_MIN_ID                  = 1;
static constexpr uint32_t WORKLOAD_STREAM_MAX_ID                  = 31;
static constexpr uint32_t WORKLOAD_ID_MAX                         = 63;
static constexpr uint32_t WORKLOAD_ACTIVE_COUNT_MAX               = 63;

static std::mutex g_workload_stream_mutex;
static uint32_t   g_workload_stream_mask = 0;
static uint8_t    g_workload_streams[WORKLOAD_STREAM_MAX_ID + 1][WORKLOAD_STREAM_RECORD_SIZE] {};

uint32_t KYTY_SYSV_ABI GraphicsDriverGetDefaultOwner() {
	PRINT_NAME();

	return 0x8a6c9018u;
}

uint32_t KYTY_SYSV_ABI GraphicsDriverGetResourceRegistrationMaxNameLength() {
	PRINT_NAME();

	return 0x8a6c9018u;
}

uint32_t KYTY_SYSV_ABI GraphicsDriverInitResourceRegistration() {
	PRINT_NAME();

	return 0x8a6c9018u;
}

uint32_t KYTY_SYSV_ABI
GraphicsDriverQueryResourceRegistrationUserMemoryRequirements(uint64_t* size_in_bytes) {
	PRINT_NAME();

	if (size_in_bytes != nullptr) {
		*size_in_bytes = 0;
	}

	return 0x8a6c9018u;
}

int KYTY_SYSV_ABI GraphicsDriverRegisterOwner() {
	PRINT_NAME();

	return static_cast<int>(0x8a6c9018u);
}

int KYTY_SYSV_ABI GraphicsDriverRegisterResource() {
	PRINT_NAME();

	return static_cast<int>(0x8a6c9018u);
}

int KYTY_SYSV_ABI GraphicsDriverUnregisterResource() {
	PRINT_NAME();

	return static_cast<int>(0x8a6c9018u);
}

int KYTY_SYSV_ABI GraphicsDriverRegisterWorkloadStream(uint32_t stream_id, const void* stream) {
	PRINT_NAME();

	LOGF("\t stream_id = %" PRIu32 "\n"
	     "\t stream    = 0x%016" PRIx64 "\n",
	     stream_id, reinterpret_cast<uint64_t>(stream));

	if (stream_id < WORKLOAD_STREAM_MIN_ID || stream_id > WORKLOAD_STREAM_MAX_ID) {
		return GRAPHICS5_DRIVER_ERROR_INVALID_VALUE;
	}

	if (stream == nullptr) {
		return GRAPHICS5_DRIVER_ERROR_INVALID_ARGUMENT;
	}

	std::lock_guard lock(g_workload_stream_mutex);

	const uint32_t stream_bit = 1u << stream_id;
	if ((g_workload_stream_mask & stream_bit) != 0) {
		return GRAPHICS5_DRIVER_ERROR_INVALID_VALUE;
	}

	std::memset(g_workload_streams[stream_id], 0, WORKLOAD_STREAM_RECORD_SIZE);
	std::memcpy(g_workload_streams[stream_id], stream, WORKLOAD_STREAM_RECORD_SIZE);
	g_workload_stream_mask |= stream_bit;

	return OK;
}

uint32_t* KYTY_SYSV_ABI GraphicsCbNop(CommandBuffer* buf, uint32_t size_in_dwords) {
	if (buf == nullptr || size_in_dwords < 2) {
		return nullptr;
	}

	auto* cmd = buf->AllocateDW(size_in_dwords);

	if (cmd == nullptr) {
		return nullptr;
	}

	cmd[0] = KYTY_PM4(size_in_dwords, Pm4::IT_NOP, Pm4::R_ZERO);
	if (size_in_dwords > 1) {
		memset(cmd + 1, 0, static_cast<size_t>(size_in_dwords - 1) * 4);
	}

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsCbNopGetSize(uint32_t size_in_dwords) {
	return 4u * size_in_dwords;
}

uint32_t* KYTY_SYSV_ABI GraphicsCbDispatch(CommandBuffer* buf, uint32_t thread_group_x,
                                           uint32_t thread_group_y, uint32_t thread_group_z,
                                           uint32_t modifier) {
	PRINT_NAME();

	static std::atomic<uint32_t> log_count {0};
	if (log_count.fetch_add(1) < 64) {
		LOGF("\t thread_group_x = %" PRIu32 "\n"
		     "\t thread_group_y = %" PRIu32 "\n"
		     "\t thread_group_z = %" PRIu32 "\n"
		     "\t modifier       = 0x%08" PRIx32 "\n",
		     thread_group_x, thread_group_y, thread_group_z, modifier);
	}

	if (buf == nullptr) {
		return nullptr;
	}

	auto* cmd = buf->AllocateDW(5);

	if (cmd == nullptr) {
		return nullptr;
	}

	cmd[0] = KYTY_PM4(5, Pm4::IT_DISPATCH_DIRECT, 0u);
	cmd[1] = thread_group_x;
	cmd[2] = thread_group_y;
	cmd[3] = thread_group_z;
	cmd[4] = (modifier & 0xa038u) | 0x41u;

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsCbDispatchGetSize() {
	PRINT_NAME();

	return 20;
}

uint32_t* KYTY_SYSV_ABI GraphicsCbBranch(CommandBuffer* buf, uint8_t mode, uint8_t compare_function,
                                         const volatile uint64_t* compare_addr, uint64_t mask,
                                         uint64_t reference, uint8_t cache_policy1,
                                         const volatile uint32_t* buffer1, uint32_t size_in_dwords1,
                                         uint8_t cache_policy2, const volatile uint32_t* buffer2,
                                         uint32_t size_in_dwords2) {
	PRINT_NAME();

	LOGF("\t mode             = 0x%02" PRIx8 "\n"
	     "\t compare_function = 0x%02" PRIx8 "\n"
	     "\t compare_addr     = 0x%016" PRIx64 "\n"
	     "\t mask             = 0x%016" PRIx64 "\n"
	     "\t reference        = 0x%016" PRIx64 "\n"
	     "\t cache_policy1    = 0x%02" PRIx8 "\n"
	     "\t buffer1          = 0x%016" PRIx64 "\n"
	     "\t size_in_dwords1  = %" PRIu32 "\n"
	     "\t cache_policy2    = 0x%02" PRIx8 "\n"
	     "\t buffer2          = 0x%016" PRIx64 "\n"
	     "\t size_in_dwords2  = %" PRIu32 "\n",
	     mode, compare_function, reinterpret_cast<uint64_t>(compare_addr), mask, reference,
	     cache_policy1, reinterpret_cast<uint64_t>(buffer1), size_in_dwords1, cache_policy2,
	     reinterpret_cast<uint64_t>(buffer2), size_in_dwords2);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	auto* cmd = buf->AllocateDW(14);

	if (cmd == nullptr) {
		LOGF_COLOR(Log::Color::Red, "\t failed to allocate branch packet\n");
		return nullptr;
	}

	const auto compare_vaddr = reinterpret_cast<uint64_t>(compare_addr);
	const auto then_vaddr    = reinterpret_cast<uint64_t>(buffer1);
	const auto else_vaddr    = reinterpret_cast<uint64_t>(buffer2);

	cmd[0]  = KYTY_PM4(14, Pm4::IT_INDIRECT_BUFFER, 0u);
	cmd[1]  = (mode & 0x3u) | ((static_cast<uint32_t>(compare_function) & 0x7u) << 8u);
	cmd[2]  = static_cast<uint32_t>(compare_vaddr & 0xfffffff8u);
	cmd[3]  = static_cast<uint32_t>((compare_vaddr >> 32u) & 0xffffffffu);
	cmd[4]  = static_cast<uint32_t>(mask & 0xffffffffu);
	cmd[5]  = static_cast<uint32_t>((mask >> 32u) & 0xffffffffu);
	cmd[6]  = static_cast<uint32_t>(reference & 0xffffffffu);
	cmd[7]  = static_cast<uint32_t>((reference >> 32u) & 0xffffffffu);
	cmd[8]  = static_cast<uint32_t>(then_vaddr & 0xfffffffcu);
	cmd[9]  = static_cast<uint32_t>((then_vaddr >> 32u) & 0xffffffffu);
	cmd[10] = (size_in_dwords1 & 0xfffffu) | ((static_cast<uint32_t>(cache_policy1) & 0x3u) << 28u);
	cmd[11] = static_cast<uint32_t>(else_vaddr & 0xfffffffcu);
	cmd[12] = static_cast<uint32_t>((else_vaddr >> 32u) & 0xffffffffu);
	cmd[13] = (size_in_dwords2 & 0xfffffu) | ((static_cast<uint32_t>(cache_policy2) & 0x3u) << 28u);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsCbSetShRegisterRangeDirect(CommandBuffer* buf, uint32_t offset,
                                                           const uint32_t* values,
                                                           uint32_t        num_values) {
	PRINT_NAME();

	static std::atomic<uint32_t> log_count {0};
	if (log_count.fetch_add(1) < 64) {
		LOGF("\t buf        = 0x%016" PRIx64 "\n"
		     "\t offset     = %" PRIx32 "\n"
		     "\t values     = 0x%016" PRIx64 "\n"
		     "\t num_values = %" PRIu32 "\n",
		     reinterpret_cast<uint64_t>(buf), offset, reinterpret_cast<uint64_t>(values),
		     num_values);
	}

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(num_values + 2);

	if (cmd == nullptr) {
		LOGF_COLOR(Log::Color::Red, "\t failed to allocate set-register-range command\n");
		return nullptr;
	}

	cmd[0] = KYTY_PM4(num_values + 2, Pm4::IT_SET_SH_REG, 0u);
	cmd[1] = offset & 0xffffu;

	if (values != nullptr) {
		memcpy(cmd + 2, values, static_cast<size_t>(num_values) * 4);
	}

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsCbSetShRegisterRangeDirectGetSize(uint32_t num_values) {
	PRINT_NAME();

	LOGF("\t num_values = %" PRIu32 "\n", num_values);

	return 4u * num_values + 8u;
}

uint32_t* KYTY_SYSV_ABI GraphicsCbSetShRegistersDirect(CommandBuffer*                 buf,
                                                       const volatile ShaderRegister* regs,
                                                       uint32_t                       num_regs) {
	PRINT_NAME();

	LOGF("\t regs     = 0x%016" PRIx64 "\n"
	     "\t num_regs = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(regs), num_regs);

	if (num_regs == 0) {
		return nullptr;
	}

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED(regs == nullptr);

	buf->DbgDump();

	std::vector<ShaderRegister> local_regs(num_regs);
	for (uint32_t i = 0; i < num_regs; i++) {
		local_regs[i].offset = regs[i].offset;
		local_regs[i].value  = regs[i].value;
	}

	uint32_t* first_cmd        = nullptr;
	uint32_t  run_start_index  = 0;
	uint32_t  run_start_offset = local_regs[0].offset;
	uint32_t  prev_offset      = run_start_offset;

	for (uint32_t i = 1; i <= num_regs; i++) {
		const bool flush = (i == num_regs || local_regs[i].offset != prev_offset + 1u);

		if (flush) {
			const auto run_count = i - run_start_index;
			auto*      cmd       = buf->AllocateDW(run_count + 2u);

			if (cmd == nullptr) {
				LOGF_COLOR(Log::Color::Red,
				           "\t failed to allocate set-sh-registers-direct command\n");
				return first_cmd;
			}

			if (first_cmd == nullptr) {
				first_cmd = cmd;
			}

			cmd[0] = KYTY_PM4(run_count + 2u, Pm4::IT_SET_SH_REG, 0u);
			cmd[1] = run_start_offset & 0xffffu;

			for (uint32_t j = 0; j < run_count; j++) {
				cmd[j + 2u] = local_regs[run_start_index + j].value;
			}

			if (i < num_regs) {
				run_start_index  = i;
				run_start_offset = local_regs[i].offset;
			}
		}

		if (i < num_regs) {
			prev_offset = local_regs[i].offset;
		}
	}

	return first_cmd;
}

int KYTY_SYSV_ABI GraphicsDebugRaiseException(uint32_t exception_id) {
	PRINT_NAME();

	LOGF("\t exception_id = 0x%08" PRIx32 "\n", exception_id);

	return OK;
}

uint32_t* KYTY_SYSV_ABI GraphicsCbReleaseMem(CommandBuffer* buf, uint8_t action, uint16_t gcr_cntl,
                                             uint8_t dst, uint8_t cache_policy,
                                             const volatile Label* address, uint8_t data_sel,
                                             uint64_t data, uint16_t gds_offset, uint16_t gds_size,
                                             uint8_t interrupt, uint32_t interrupt_ctx_id) {
	PRINT_NAME();

	static std::atomic<uint32_t> log_count {0};
	if (log_count.fetch_add(1) < 64) {
		LOGF("\t action           = 0x%02" PRIx8 "\n"
		     "\t gcr_cntl         = 0x%04" PRIx16 "\n"
		     "\t dst              = %" PRIu8 "\n"
		     "\t cache_policy     = 0x%02" PRIx8 "\n"
		     "\t address          = 0x%016" PRIx64 "\n"
		     "\t data_sel         = 0x%02" PRIx8 "\n"
		     "\t data             = 0x%016" PRIx64 "\n"
		     "\t gds_offset       = %" PRIu16 "\n"
		     "\t gds_size         = %" PRIu16 "\n"
		     "\t interrupt        = 0x%02" PRIx8 "\n"
		     "\t interrupt_ctx_id = %" PRIu32 "\n",
		     action, gcr_cntl, dst, cache_policy, reinterpret_cast<uint64_t>(address), data_sel,
		     data, gds_offset, gds_size, interrupt, interrupt_ctx_id);
	}

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED(dst > 1);
	EXIT_NOT_IMPLEMENTED(data_sel != 0 && data_sel != 1 && data_sel != 2 && data_sel != 3 &&
	                     data_sel != 5);
	EXIT_NOT_IMPLEMENTED(interrupt > 4);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(8);

	if (cmd == nullptr) {
		LOGF_COLOR(Log::Color::Red, "\t failed to allocate release_mem packet\n");
		return nullptr;
	}

	auto packet_gcr_cntl = static_cast<uint32_t>(gcr_cntl);
	if ((packet_gcr_cntl & 0x300u) == 0x100u) {
		packet_gcr_cntl |= 0x200u;
	}

	auto address_value = reinterpret_cast<uint64_t>(address);
	auto packet_data   = data;
	if ((interrupt & 0x7u) == 4u) {
		address_value = 0;
		packet_data   = 0;
	} else if ((data_sel & 0x7u) == 5u) {
		packet_data = static_cast<uint64_t>(gds_offset) | (static_cast<uint64_t>(gds_size) << 16u);
	}

	const uint32_t packet_action      = static_cast<uint32_t>(action) & 0x3fu;
	const uint32_t packet_event_index = (action >= 0x2fu ? 6u : 5u);

	cmd[0] = KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_RELEASE_MEM);
	cmd[1] = packet_action | (packet_event_index << 8u) | ((packet_gcr_cntl & 0xfffu) << 12u) |
	         ((static_cast<uint32_t>(cache_policy) & 0x3u) << 25u);
	cmd[2] = ((static_cast<uint32_t>(dst) & 0x3u) << 16u) |
	         ((static_cast<uint32_t>(interrupt) & 0x7u) << 24u) |
	         ((static_cast<uint32_t>(data_sel) & 0x7u) << 29u);
	cmd[3] = static_cast<uint32_t>(address_value & 0xfffffffcu);
	cmd[4] = static_cast<uint32_t>((address_value >> 32u) & 0xffffffffu);
	cmd[5] = static_cast<uint32_t>(packet_data & 0xffffffffu);
	cmd[6] = static_cast<uint32_t>((packet_data >> 32u) & 0xffffffffu);
	cmd[7] = interrupt_ctx_id & 0x07ffffffu;

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsCbQueueEndOfPipeActionGetSize() {
	PRINT_NAME();

	return 32;
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbResetQueue(CommandBuffer* buf, uint32_t op) {
	PRINT_NAME();

	LOGF("\t op    = 0x%08" PRIx32 "\n", op);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED((op & ~0x1c2u) != 0);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(2);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_DISPATCH_RESET);
	cmd[1] = 0;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbResetQueue(CommandBuffer* buf, uint32_t op, uint32_t state) {
	PRINT_NAME();

	LOGF("\t op    = 0x%08" PRIx32 "\n"
	     "\t state = 0x%08" PRIx32 "\n",
	     op, state);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED((op & ~0xfffu) != 0);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(2);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(2, Pm4::IT_CLEAR_STATE, 0u);
	cmd[1] = state & 0xfu;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbWaitUntilSafeForRendering(CommandBuffer* buf,
                                                             uint32_t       video_out_handle,
                                                             uint32_t       display_buffer_index) {
	PRINT_NAME();

	LOGF("\t video_out_handle     = %" PRIu32 "\n"
	     "\t display_buffer_index = %" PRIu32 "\n",
	     video_out_handle, display_buffer_index);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(7);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_WAIT_FLIP_DONE);
	cmd[1] = video_out_handle;
	cmd[2] = display_buffer_index;
	cmd[3] = 0;
	cmd[4] = 0;
	cmd[5] = 0;
	cmd[6] = 0;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetWorkloadsActive(CommandBuffer* buf, uint32_t stream_id,
                                                      const uint32_t* workload_ids,
                                                      uint32_t        workload_count) {
	PRINT_NAME();

	LOGF("\t stream_id      = %" PRIu32 "\n"
	     "\t workload_ids   = 0x%016" PRIx64 "\n"
	     "\t workload_count = %" PRIu32 "\n",
	     stream_id, reinterpret_cast<uint64_t>(workload_ids), workload_count);

	if (buf == nullptr || workload_ids == nullptr || workload_count == 0 ||
	    workload_count > WORKLOAD_ACTIVE_COUNT_MAX || stream_id < WORKLOAD_STREAM_MIN_ID ||
	    stream_id > WORKLOAD_STREAM_MAX_ID) {
		return nullptr;
	}

	uint64_t workload_mask = 0;
	for (uint32_t i = 0; i < workload_count; i++) {
		const auto workload_id = workload_ids[i];
		if (workload_id > WORKLOAD_ID_MAX) {
			return nullptr;
		}

		const uint64_t workload_bit = 1ull << workload_id;
		if ((workload_mask & workload_bit) != 0) {
			return nullptr;
		}
		workload_mask |= workload_bit;
	}

	{
		std::lock_guard lock(g_workload_stream_mutex);
		if ((g_workload_stream_mask & (1u << stream_id)) == 0) {
			return nullptr;
		}
	}

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(WORKLOAD_ACTIVE_PACKET_SIZE_DW);
	if (cmd == nullptr) {
		return nullptr;
	}

	cmd[0] = KYTY_PM4(WORKLOAD_ACTIVE_PACKET_SIZE_DW, Pm4::IT_NOP, Pm4::R_ZERO);
	std::memset(cmd + 1, 0,
	            static_cast<size_t>(WORKLOAD_ACTIVE_PACKET_SIZE_DW - 1) * sizeof(uint32_t));
	cmd[1] = stream_id;
	cmd[2] = static_cast<uint32_t>(workload_mask & 0xffffffffu);
	cmd[3] = static_cast<uint32_t>((workload_mask >> 32u) & 0xffffffffu);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetWorkloadComplete(CommandBuffer* buf, uint32_t stream_id,
                                                       uint32_t workload_id) {
	PRINT_NAME();

	LOGF("\t stream_id   = %" PRIu32 "\n"
	     "\t workload_id = %" PRIu32 "\n",
	     stream_id, workload_id);

	if (buf == nullptr || stream_id < WORKLOAD_STREAM_MIN_ID ||
	    stream_id > WORKLOAD_STREAM_MAX_ID || workload_id > WORKLOAD_ID_MAX) {
		return nullptr;
	}

	{
		std::lock_guard lock(g_workload_stream_mutex);
		if ((g_workload_stream_mask & (1u << stream_id)) == 0) {
			return nullptr;
		}
	}

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(WORKLOAD_COMPLETE_PACKET_SIZE_DW);
	if (cmd == nullptr) {
		return nullptr;
	}

	const uint64_t workload_clear_mask = ~(1ull << workload_id);

	cmd[0] = KYTY_PM4(WORKLOAD_COMPLETE_PACKET_SIZE_DW, Pm4::IT_NOP, Pm4::R_ZERO);
	std::memset(cmd + 1, 0,
	            static_cast<size_t>(WORKLOAD_COMPLETE_PACKET_SIZE_DW - 1) * sizeof(uint32_t));
	cmd[1] = stream_id;
	cmd[2] = workload_id;
	cmd[3] = static_cast<uint32_t>(workload_clear_mask & 0xffffffffu);
	cmd[4] = static_cast<uint32_t>((workload_clear_mask >> 32u) & 0xffffffffu);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetShRegisterDirect(CommandBuffer* buf, ShaderRegister reg) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(3);

	if (cmd == nullptr) {
		LOGF_COLOR(Log::Color::Red, "\t failed to allocate set-sh-register-direct command\n");
		return nullptr;
	}

	cmd[0] = KYTY_PM4(3, Pm4::IT_SET_SH_REG, 0u);
	cmd[1] = reg.offset & 0xffffu;
	cmd[2] = reg.value;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetCxRegisterDirect(CommandBuffer* buf, ShaderRegister reg) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(3);

	if (cmd == nullptr) {
		LOGF_COLOR(Log::Color::Red, "\t failed to allocate set-cx-register-direct command\n");
		return nullptr;
	}

	cmd[0] = KYTY_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0u);
	cmd[1] = reg.offset & 0xffffu;
	cmd[2] = reg.value;

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsDcbSetCxRegisterDirectGetSize() {
	PRINT_NAME();

	return 12;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetUcRegisterDirect(CommandBuffer* buf, ShaderRegister reg) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(3);

	if (cmd == nullptr) {
		LOGF_COLOR(Log::Color::Red, "\t failed to allocate set-uc-register-direct command\n");
		return nullptr;
	}

	cmd[0] = KYTY_PM4(3, Pm4::IT_SET_UCONFIG_REG, 0u);
	cmd[1] = reg.offset & 0xffffu;
	cmd[2] = reg.value;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetCxRegistersIndirect(CommandBuffer*                 buf,
                                                          const volatile ShaderRegister* regs,
                                                          uint32_t                       num_regs) {
	PRINT_NAME();

	LOGF("\t regs     = 0x%016" PRIx64 "\n"
	     "\t num_regs = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(regs), num_regs);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(5);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(regs);

	reg_indirect_write_packet(cmd, vaddr, num_regs, RegIndirectPacket::Cx);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetShRegistersIndirect(CommandBuffer*                 buf,
                                                          const volatile ShaderRegister* regs,
                                                          uint32_t                       num_regs) {
	PRINT_NAME();

	LOGF("\t regs     = 0x%016" PRIx64 "\n"
	     "\t num_regs = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(regs), num_regs);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(5);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(regs);

	reg_indirect_write_packet(cmd, vaddr, num_regs, RegIndirectPacket::Sh);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetUcRegistersIndirect(CommandBuffer*                 buf,
                                                          const volatile ShaderRegister* regs,
                                                          uint32_t                       num_regs) {
	PRINT_NAME();

	LOGF("\t regs     = 0x%016" PRIx64 "\n"
	     "\t num_regs = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(regs), num_regs);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(5);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(regs);

	reg_indirect_write_packet(cmd, vaddr, num_regs, RegIndirectPacket::Uc);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetIndexSize(CommandBuffer* buf, uint8_t index_size,
                                                uint8_t cache_policy) {
	PRINT_NAME();

	LOGF("\t index_size   = 0x%" PRIx8 "\n"
	     "\t cache_policy = 0x%" PRIx8 "\n",
	     index_size, cache_policy);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	buf->DbgDump();

	auto* cmd = buf->AllocateDW(3);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(3, Pm4::IT_SET_UCONFIG_REG_INDEX, 0u);
	cmd[1] = 0x20000000u | Pm4::VGT_INDEX_TYPE;
	cmd[2] = 0x400u | (index_size & 0x3u) | ((cache_policy & 0x3u) << 6u);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetIndexBuffer(CommandBuffer* buf, uint64_t index_addr) {
	PRINT_NAME();

	LOGF("\t index_addr = 0x%016" PRIx64 "\n", index_addr);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED((index_addr & 1u) != 0);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(3);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(3, Pm4::IT_INDEX_BASE, 0u);
	cmd[1] = static_cast<uint32_t>(index_addr & 0xffffffffu);
	cmd[2] = static_cast<uint32_t>((index_addr >> 32u) & 0xffffffffu);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetIndexCount(CommandBuffer* buf, uint32_t index_count) {
	PRINT_NAME();

	LOGF("\t index_count = 0x%" PRIx32 "\n", index_count);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(2);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(2, Pm4::IT_INDEX_BUFFER_SIZE, 0u);
	cmd[1] = index_count;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetNumInstances(CommandBuffer* buf, uint32_t num_instances) {
	PRINT_NAME();

	LOGF("\t num_instances = 0x%" PRIx32 "\n", num_instances);

	if (buf == nullptr) {
		return nullptr;
	}

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(2);

	if (cmd == nullptr) {
		return nullptr;
	}

	cmd[0] = KYTY_PM4(2, Pm4::IT_NUM_INSTANCES, 0u);
	cmd[1] = num_instances;

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsDcbSetNumInstancesGetSize() {
	PRINT_NAME();

	return 8;
}

static uint32_t extract_modifier_bits(uint32_t modifier, uint32_t start, uint32_t count) {
	return (modifier >> start) & ((1u << count) - 1u);
}

static uint32_t indirect_modifier_sgpr_base(uint32_t modifier) {
	const auto stage = modifier >> 29u;
	return ((stage == 3u || stage == 5u) ? 0x80u : 0u) + 0x8cu;
}

static uint64_t decode_indirect_modifier_patch_offsets(uint64_t modifier, bool indexed) {
	const auto low       = static_cast<uint32_t>(modifier);
	const auto sgpr_base = indirect_modifier_sgpr_base(low);

	uint64_t base_vtx_loc = 0x280u;
	if ((low & 0x1u) != 0) {
		base_vtx_loc = sgpr_base + extract_modifier_bits(low, 9u, 5u);
	}

	uint64_t start_inst_loc = 0x280u;
	if ((low & 0x4u) != 0) {
		start_inst_loc = sgpr_base + extract_modifier_bits(low, 19u, 5u);
	}

	if (indexed && (low & 0x2u) != 0) {
		base_vtx_loc |= static_cast<uint64_t>(sgpr_base + extract_modifier_bits(low, 14u, 5u))
		                << 16u;
		base_vtx_loc |= 1ull << 59u;
	}

	return base_vtx_loc | (start_inst_loc << 32u);
}

static uint32_t decode_indirect_draw_initiator(uint64_t modifier) {
	const auto low       = static_cast<uint32_t>(modifier);
	uint32_t   initiator = 2u;

	if ((modifier & (1ull << 32u)) == 0) {
		initiator = ((low >> 3u) & 0x20u) | 2u;
	}

	return initiator;
}

static uint32_t decode_draw_index_initiator(uint64_t modifier) {
	if ((modifier & (1ull << 32u)) != 0) {
		return 0;
	}

	return (static_cast<uint32_t>(modifier) >> 3u) & 0x20u;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndex(CommandBuffer* buf, uint32_t index_count,
                                             const volatile void* index_addr, uint64_t modifier) {
	PRINT_NAME();

	LOGF("\t index_count = 0x%" PRIx32 "\n"
	     "\t index_addr  = 0x%016" PRIx64 "\n"
	     "\t modifier    = 0x%016" PRIx64 "\n",
	     index_count, reinterpret_cast<uint64_t>(index_addr), modifier);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED(index_addr == nullptr);
	EXIT_NOT_IMPLEMENTED((reinterpret_cast<uint64_t>(index_addr) & 1u) != 0);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(6);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(index_addr);

	cmd[0] = KYTY_PM4(6, Pm4::IT_DRAW_INDEX_2, 0u);
	cmd[1] = (index_count == 0 ? 1u : index_count);
	cmd[2] = static_cast<uint32_t>(vaddr & 0xffffffffu);
	cmd[3] = static_cast<uint32_t>((vaddr >> 32u) & 0xffffffffu);
	cmd[4] = index_count;
	cmd[5] = decode_draw_index_initiator(modifier);

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndexGetSize() {
	PRINT_NAME();

	return 6u * 4u;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexMultiInstanced(CommandBuffer* buf, uint32_t index_count,
                                                           const volatile void* index_addr,
                                                           const volatile void* object_ids,
                                                           uint32_t             instance_count,
                                                           uint64_t             modifier) {
	PRINT_NAME();

	LOGF("\t index_count    = 0x%" PRIx32 "\n"
	     "\t index_addr     = 0x%016" PRIx64 "\n"
	     "\t instance_count = 0x%" PRIx32 "\n"
	     "\t object_ids     = 0x%016" PRIx64 "\n"
	     "\t modifier       = 0x%016" PRIx64 "\n",
	     index_count, reinterpret_cast<uint64_t>(index_addr), instance_count,
	     reinterpret_cast<uint64_t>(object_ids), modifier);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED(index_addr == nullptr);
	EXIT_NOT_IMPLEMENTED(object_ids == nullptr);
	EXIT_NOT_IMPLEMENTED((reinterpret_cast<uint64_t>(index_addr) & 1u) != 0);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(9);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto index_vaddr  = reinterpret_cast<uint64_t>(index_addr);
	auto object_vaddr = reinterpret_cast<uint64_t>(object_ids);

	cmd[0] = KYTY_PM4(9, Pm4::IT_DISPATCH_DRAW_PREAMBLE, 0u);
	cmd[1] = index_count;
	cmd[2] = static_cast<uint32_t>(index_vaddr & 0xffffffffu);
	cmd[3] = static_cast<uint32_t>((index_vaddr >> 32u) & 0xffffffffu);
	cmd[4] = (instance_count == 0 ? 1u : instance_count);
	cmd[5] = static_cast<uint32_t>(object_vaddr & 0xffffffffu);
	cmd[6] = static_cast<uint32_t>((object_vaddr >> 32u) & 0xffffffffu);
	cmd[7] = instance_count;
	cmd[8] = decode_draw_index_initiator(modifier) | 0x80u;

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndexMultiInstancedGetSize() {
	PRINT_NAME();

	return 9u * 4u;
}

int KYTY_SYSV_ABI GraphicsUnknownIkfdtRIqCE(uint32_t* cmd, uint64_t arg1,
                                            const volatile uint32_t* target,
                                            uint32_t size_in_dwords, uint64_t arg4, uint64_t arg5) {
	PRINT_NAME();

	LOGF("\t cmd            = 0x%016" PRIx64 "\n"
	     "\t arg1           = 0x%016" PRIx64 "\n"
	     "\t target         = 0x%016" PRIx64 "\n"
	     "\t size_in_dwords = %" PRIu32 "\n"
	     "\t arg4           = 0x%016" PRIx64 "\n"
	     "\t arg5           = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(cmd), arg1, reinterpret_cast<uint64_t>(target), size_in_dwords,
	     arg4, arg5);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto op = (cmd[0] >> 8u) & 0xffu;
	if (op != Pm4::IT_INDIRECT_BUFFER) {
		return 0x8a6c000c;
	}

	auto vaddr        = reinterpret_cast<uint64_t>(target);
	auto cache_policy = static_cast<uint32_t>(arg1) & 0x3u;

	cmd[1] = (cmd[1] & 0x3u) | (static_cast<uint32_t>(vaddr) & 0xfffffffcu);
	cmd[2] = static_cast<uint32_t>(vaddr >> 32u);
	cmd[3] = (cmd[3] & 0xcff00000u) | (cache_policy << 28u) | (size_in_dwords & 0xfffffu);

	return OK;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexAuto(CommandBuffer* buf, uint32_t index_count,
                                                 uint64_t modifier) {
	PRINT_NAME();

	LOGF("\t index_count = 0x%" PRIx32 "\n"
	     "\t modifier    = 0x%016" PRIx64 "\n",
	     index_count, modifier);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(3);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(3, Pm4::IT_DRAW_INDEX_AUTO, 0u);
	cmd[1] = index_count;
	cmd[2] = decode_draw_index_initiator(modifier) | 0x2u;

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndexAutoGetSize() {
	PRINT_NAME();

	return 3u * 4u;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexOffset(CommandBuffer* buf, uint32_t index_offset,
                                                   uint32_t index_count, uint64_t modifier) {
	PRINT_NAME();

	LOGF("\t index_offset = 0x%" PRIx32 "\n"
	     "\t index_count  = 0x%" PRIx32 "\n"
	     "\t modifier     = 0x%016" PRIx64 "\n",
	     index_offset, index_count, modifier);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(5);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(5, Pm4::IT_DRAW_INDEX_OFFSET_2, 0u);
	cmd[1] = (index_count == 0 ? 1u : index_count);
	cmd[2] = index_offset;
	cmd[3] = index_count;
	cmd[4] = decode_draw_index_initiator(modifier);

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndexOffsetGetSize() {
	PRINT_NAME();

	return 5u * 4u;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetBaseIndirectArgs(CommandBuffer* buf, uint32_t shader_type,
                                                       const volatile void* indirect_base_addr) {
	PRINT_NAME();

	LOGF("\t shader_type        = %" PRIu32 "\n"
	     "\t indirect_base_addr = 0x%016" PRIx64 "\n",
	     shader_type, reinterpret_cast<uint64_t>(indirect_base_addr));

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(4);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	const auto addr = reinterpret_cast<uint64_t>(indirect_base_addr);

	cmd[0] = KYTY_PM4(4, Pm4::IT_SET_BASE, 0u) | (shader_type << 1u);
	cmd[1] = 1u;
	cmd[2] = static_cast<uint32_t>(addr) & ~0x7u;
	cmd[3] = static_cast<uint32_t>(addr >> 32u);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexIndirect(CommandBuffer* buf,
                                                     uint32_t       data_offset_in_bytes,
                                                     uint64_t       modifier) {
	PRINT_NAME();

	LOGF("\t data_offset = 0x%" PRIx32 "\n"
	     "\t modifier    = 0x%016" PRIx64 "\n",
	     data_offset_in_bytes, modifier);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(5);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	const auto patch_offsets = decode_indirect_modifier_patch_offsets(modifier, true);

	cmd[0] = KYTY_PM4(5, Pm4::IT_DRAW_INDEX_INDIRECT, 0u);
	cmd[1] = data_offset_in_bytes;
	cmd[2] = static_cast<uint32_t>(patch_offsets);
	cmd[3] = static_cast<uint32_t>(patch_offsets >> 32u);
	cmd[4] = decode_indirect_draw_initiator(modifier);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndirect(CommandBuffer* buf, uint32_t data_offset_in_bytes,
                                                uint64_t modifier) {
	PRINT_NAME();

	LOGF("\t data_offset = 0x%" PRIx32 "\n"
	     "\t modifier    = 0x%016" PRIx64 "\n",
	     data_offset_in_bytes, modifier);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(5);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	const auto patch_offsets = decode_indirect_modifier_patch_offsets(modifier, false);

	cmd[0] = KYTY_PM4(5, Pm4::IT_DRAW_INDIRECT, 0u);
	cmd[1] = data_offset_in_bytes;
	cmd[2] = static_cast<uint32_t>(patch_offsets);
	cmd[3] = static_cast<uint32_t>(patch_offsets >> 32u);
	cmd[4] = decode_indirect_draw_initiator(modifier);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexIndirectMulti(
    CommandBuffer* buf, uint32_t data_offset_in_bytes, uint32_t count_indirect,
    uint32_t max_count_or_count, const volatile void* count_addr, uint32_t stride_in_bytes,
    uint64_t modifier) {
	PRINT_NAME();

	LOGF("\t data_offset        = 0x%" PRIx32 "\n"
	     "\t count_indirect     = 0x%" PRIx32 "\n"
	     "\t max_count_or_count = 0x%" PRIx32 "\n"
	     "\t count_addr         = 0x%016" PRIx64 "\n"
	     "\t stride_in_bytes    = 0x%" PRIx32 "\n"
	     "\t modifier           = 0x%016" PRIx64 "\n",
	     data_offset_in_bytes, count_indirect, max_count_or_count,
	     reinterpret_cast<uint64_t>(count_addr), stride_in_bytes, modifier);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED((count_indirect & ~1u) != 0);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(10);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	const auto patch_offsets = decode_indirect_modifier_patch_offsets(modifier, true);
	const auto count_vaddr   = reinterpret_cast<uint64_t>(count_addr);

	cmd[0] = KYTY_PM4(10, Pm4::IT_DRAW_INDEX_INDIRECT_MULTI, 0u);
	cmd[1] = data_offset_in_bytes;
	cmd[2] = static_cast<uint32_t>(patch_offsets);
	cmd[3] = static_cast<uint32_t>(patch_offsets >> 32u);
	cmd[4] = (count_indirect & 1u) << 30u;
	cmd[5] = max_count_or_count;
	cmd[6] = static_cast<uint32_t>(count_vaddr) & ~0x3u;
	cmd[7] = static_cast<uint32_t>(count_vaddr >> 32u);
	cmd[8] = stride_in_bytes;
	cmd[9] = decode_indirect_draw_initiator(modifier);

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsDcbDrawIndirectGetSize() {
	PRINT_NAME();

	return 5u * 4u;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbDispatchIndirect(CommandBuffer* buf,
                                                    uint32_t data_offset_in_bytes, uint32_t flags) {
	PRINT_NAME();

	LOGF("\t data_offset = 0x%" PRIx32 "\n"
	     "\t flags       = 0x%08" PRIx32 "\n",
	     data_offset_in_bytes, flags);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(3);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(3, Pm4::IT_DISPATCH_INDIRECT, 0u);
	cmd[1] = data_offset_in_bytes;
	cmd[2] = (flags & 0xa038u) | 0x41u;

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsDcbDispatchIndirectGetSize() {
	PRINT_NAME();

	return 3u * 4u;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbEventWrite(CommandBuffer* buf, uint8_t event_type,
                                              const volatile void* address) {
	PRINT_NAME();

	static std::atomic<uint32_t> log_count {0};
	if (log_count.fetch_add(1) < 64) {
		LOGF("\t event_type = 0x%02" PRIx8 "\n"
		     "\t address    = 0x%016" PRIx64 "\n",
		     event_type, reinterpret_cast<uint64_t>(address));
	}

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED(event_type > 0x3fu);

	buf->DbgDump();

	const bool addressed_event = ((event_type & 0xfeu) == 0x38u);
	auto*      cmd             = buf->AllocateDW(addressed_event ? 4u : 2u);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(addressed_event ? 4u : 2u, Pm4::IT_EVENT_WRITE, 0u);

	if (event_type == 7u || event_type == 15u || event_type == 16u) {
		cmd[1] = 0x400u | event_type;
	} else if (addressed_event) {
		auto addr = reinterpret_cast<uint64_t>(address);

		cmd[1] = 0x100u | event_type;
		cmd[2] = static_cast<uint32_t>(addr) & 0xfffffff8u;
		cmd[3] = static_cast<uint32_t>(addr >> 32u);
	} else {
		cmd[1] = event_type & 0x3fu;
	}

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbEventWrite(CommandBuffer* buf, uint8_t event_type,
                                              const volatile void* address) {
	return GraphicsDcbEventWrite(buf, event_type, address);
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbAcquireMem(CommandBuffer* buf, uint8_t engine, uint32_t cb_db_op,
                                              uint32_t gcr_cntl, const volatile void* base,
                                              uint64_t size_bytes, uint32_t poll_cycles) {
	PRINT_NAME();

	static std::atomic<uint32_t> log_count {0};
	if (log_count.fetch_add(1) < 64) {
		LOGF("\t engine      = 0x%02" PRIx8 "\n"
		     "\t cb_db_op    = 0x%08" PRIx32 "\n"
		     "\t gcr_cntl    = 0x%08" PRIx32 "\n"
		     "\t base        = 0x%016" PRIx64 "\n"
		     "\t size_bytes  = 0x%016" PRIx64 "\n"
		     "\t poll_cycles = %" PRIu32 "\n",
		     engine, cb_db_op, gcr_cntl, reinterpret_cast<uint64_t>(base), size_bytes, poll_cycles);
	}

	bool no_size = (static_cast<int64_t>(size_bytes) == -1);
	auto vaddr   = reinterpret_cast<uint64_t>(base);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	static std::atomic<uint32_t> warning_log_count {0};
	const bool                   log_warning = (warning_log_count.fetch_add(1) < 64);
	if (!no_size && (size_bytes & 0xffu) != 0) {
		if (log_warning) {
			LOGF_COLOR(Log::Color::Red, "\t warning: size_bytes is not 256-byte aligned\n");
		}
	}
	if (!no_size && (size_bytes >> 40u) != 0) {
		if (log_warning) {
			LOGF_COLOR(Log::Color::Red, "\t warning: size_bytes is too large\n");
		}
	}
	if ((vaddr & 0xffu) != 0) {
		if (log_warning) {
			LOGF_COLOR(Log::Color::Red, "\t warning: base is not 256-byte aligned\n");
		}
	}
	if ((vaddr >> 40u) != 0) {
		if (log_warning) {
			LOGF_COLOR(Log::Color::Red, "\t warning: base is too high\n");
		}
	}
	if (engine > 1) {
		if (log_warning) {
			LOGF_COLOR(Log::Color::Red, "\t warning: unsupported engine 0x%02" PRIx8 "\n", engine);
		}
	}

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(8);

	if (cmd == nullptr) {
		LOGF_COLOR(Log::Color::Red, "\t failed to allocate acquire_mem packet\n");
		return nullptr;
	}

	cmd[0] = KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_ACQUIRE_MEM);
	cmd[1] = (static_cast<uint32_t>(engine & 1u) << 31u) | cb_db_op;
	cmd[2] = (no_size ? 0 : static_cast<uint32_t>(size_bytes >> 8u));
	cmd[3] = 0;
	cmd[4] = static_cast<uint32_t>(vaddr >> 8u);
	cmd[5] = 0;
	cmd[6] = poll_cycles / 40;
	cmd[7] = gcr_cntl;

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsDcbAcquireMemGetSize() {
	PRINT_NAME();

	return 8u * 4u;
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbAcquireMem(CommandBuffer* buf, uint32_t gcr_cntl,
                                              const volatile void* base, uint64_t size_bytes,
                                              uint32_t poll_cycles) {
	return GraphicsDcbAcquireMem(buf, 1, 0, gcr_cntl, base, size_bytes, poll_cycles);
}

uint32_t KYTY_SYSV_ABI GraphicsAcbAcquireMemGetSize() {
	PRINT_NAME();

	return GraphicsDcbAcquireMemGetSize();
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbCondExec(CommandBuffer* buf, const volatile uint32_t* address,
                                            uint32_t num_dwords) {
	PRINT_NAME();

	LOGF("\t address    = 0x%016" PRIx64 "\n"
	     "\t num_dwords = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(address), num_dwords);

	if (buf == nullptr || address == nullptr) {
		LOGF_COLOR(Log::Color::Red, "\t invalid arguments\n");
		return nullptr;
	}
	if ((reinterpret_cast<uintptr_t>(address) & 0x3u) != 0) {
		LOGF_COLOR(Log::Color::Red, "\t condExec command address is not 4-byte aligned\n");
		return nullptr;
	}
	if (num_dwords > 0x3fffu) {
		LOGF_COLOR(Log::Color::Red, "\t condExec range is too large: %" PRIu32 "\n", num_dwords);
		return nullptr;
	}

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(5);

	if (cmd == nullptr) {
		LOGF_COLOR(Log::Color::Red, "\t failed to allocate condExec packet\n");
		return nullptr;
	}

	auto addr = reinterpret_cast<uint64_t>(address);

	cmd[0] = KYTY_PM4(5, Pm4::IT_COND_EXEC, 0u);
	cmd[1] = static_cast<uint32_t>(addr) & 0xfffffffcu;
	cmd[2] = static_cast<uint32_t>(addr >> 32u);
	cmd[3] = 0;
	cmd[4] = num_dwords & 0x3fffu;

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsDcbCondExecGetSize() {
	PRINT_NAME();

	return 20;
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbCondExec(CommandBuffer* buf, const volatile uint32_t* address,
                                            uint32_t num_dwords) {
	return GraphicsDcbCondExec(buf, address, num_dwords);
}

uint32_t KYTY_SYSV_ABI GraphicsAcbCondExecGetSize() {
	PRINT_NAME();

	return GraphicsDcbCondExecGetSize();
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbWaitRegMem(CommandBuffer* buf, uint8_t size,
                                              uint8_t compare_function, uint8_t cache_policy,
                                              const volatile void* address, uint64_t reference,
                                              uint64_t mask, uint32_t poll_cycles) {
	return GraphicsDcbWaitRegMem(buf, size, compare_function, 0, cache_policy, address, reference,
	                             mask, poll_cycles);
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbDmaData(CommandBuffer* buf, uint8_t engine, uint8_t dst,
                                           uint8_t dst_cache_policy, uint64_t dst_address_or_offset,
                                           uint8_t src, uint8_t src_cache_policy,
                                           uint64_t src_address_or_offset_or_immediate,
                                           uint32_t num_bytes, uint8_t wait_for_previous,
                                           uint8_t write_confirm, uint8_t block_engine) {
	return GraphicsDcbDmaData(buf, engine, dst, dst_cache_policy, dst_address_or_offset, src,
	                          src_cache_policy, src_address_or_offset_or_immediate, num_bytes,
	                          wait_for_previous, write_confirm, block_engine);
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbCopyData(CommandBuffer* buf, uint8_t dst,
                                            uint8_t dst_cache_policy, uint64_t dst_address,
                                            uint8_t src, uint8_t src_cache_policy,
                                            uint64_t src_address_or_immediate, uint8_t item_size,
                                            uint8_t write_confirm) {
	const auto dcb_src = (src == 5 ? static_cast<uint8_t>(5u << 1u) : src);
	return GraphicsDcbCopyData(buf, dst, dst_cache_policy, dst_address, dcb_src, src_cache_policy,
	                           src_address_or_immediate, item_size, write_confirm);
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbDispatchIndirect(CommandBuffer*       buf,
                                                    const volatile void* indirect_args,
                                                    uint32_t             modifier) {
	PRINT_NAME();

	LOGF("\t indirect_args = 0x%016" PRIx64 "\n"
	     "\t modifier      = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(indirect_args), modifier);

	if (buf == nullptr) {
		return nullptr;
	}

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(4);

	if (cmd == nullptr) {
		return nullptr;
	}

	const auto args_addr = reinterpret_cast<uint64_t>(indirect_args);

	cmd[0] = KYTY_PM4(4, Pm4::IT_DISPATCH_INDIRECT, 0u);
	cmd[1] = static_cast<uint32_t>(args_addr & 0xffffffffu);
	cmd[2] = static_cast<uint32_t>((args_addr >> 32u) & 0xffffffffu);
	cmd[3] = (modifier & 0xa038u) | 0x41u;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbWriteData(CommandBuffer* buf, uint8_t dst, uint8_t cache_policy,
                                             uint64_t address_or_offset, const void* data,
                                             uint32_t num_dwords, uint8_t increment,
                                             uint8_t write_confirm) {
	return GraphicsDcbWriteData(buf, dst, cache_policy, address_or_offset, data, num_dwords,
	                            increment, write_confirm);
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbStallCommandBufferParser(CommandBuffer* buf) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(2);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(2, Pm4::IT_PFP_SYNC_ME, 0u);
	cmd[1] = 0;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbCopyData(CommandBuffer* buf, uint8_t dst,
                                            uint8_t dst_cache_policy, uint64_t dst_address,
                                            uint8_t src, uint8_t src_cache_policy,
                                            uint64_t src_address_or_immediate, uint8_t item_size,
                                            uint8_t write_confirm) {
	PRINT_NAME();

	LOGF("\t dst                      = 0x%02" PRIx8 "\n"
	     "\t dst_cache_policy         = 0x%02" PRIx8 "\n"
	     "\t dst_address              = 0x%016" PRIx64 "\n"
	     "\t src                      = 0x%02" PRIx8 "\n"
	     "\t src_cache_policy         = 0x%02" PRIx8 "\n"
	     "\t src_address_or_immediate = 0x%016" PRIx64 "\n"
	     "\t item_size                = 0x%02" PRIx8 "\n"
	     "\t write_confirm            = 0x%02" PRIx8 "\n",
	     dst, dst_cache_policy, dst_address, src, src_cache_policy, src_address_or_immediate,
	     item_size, write_confirm);

	if (buf == nullptr) {
		return nullptr;
	}

	auto* cmd = buf->AllocateDW(6);

	if (cmd == nullptr) {
		return nullptr;
	}

	cmd[0] = KYTY_PM4(6, Pm4::IT_COPY_DATA, 0u);
	cmd[1] = ((static_cast<uint32_t>(src) >> 1u) & 0xfu) |
	         (((static_cast<uint32_t>(dst) >> 1u) & 0xfu) << 8u) |
	         ((static_cast<uint32_t>(src_cache_policy) & 0x3u) << 13u) |
	         ((static_cast<uint32_t>(item_size) & 0x1u) << 16u) |
	         ((static_cast<uint32_t>(write_confirm) & 0x1u) << 20u) |
	         ((static_cast<uint32_t>(dst_cache_policy) & 0x3u) << 25u) |
	         ((static_cast<uint32_t>(src) & 0x1u) << 30u);
	cmd[2] = static_cast<uint32_t>(src_address_or_immediate & 0xffffffffu);
	cmd[3] = static_cast<uint32_t>((src_address_or_immediate >> 32u) & 0xffffffffu);
	cmd[4] = static_cast<uint32_t>(dst_address & 0xffffffffu);
	cmd[5] = static_cast<uint32_t>((dst_address >> 32u) & 0xffffffffu);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbDmaData(CommandBuffer* buf, uint8_t engine, uint8_t dst,
                                           uint8_t dst_cache_policy, uint64_t dst_address_or_offset,
                                           uint8_t src, uint8_t src_cache_policy,
                                           uint64_t src_address_or_offset_or_immediate,
                                           uint32_t num_bytes, uint8_t wait_for_previous,
                                           uint8_t write_confirm, uint8_t block_engine) {
	if (buf == nullptr) {
		return nullptr;
	}

	auto src_address = src_address_or_offset_or_immediate;
	switch (src) {
		case 0x14: src_address = (engine == 1u ? 0x30148u : 0x30174u); break;
		case 0x24: src_address = (engine == 1u ? 0x30150u : 0x3017cu); break;
		case 0x64: src_address = (engine == 1u ? 0x30158u : 0x30184u); break;
		default: break;
	}

	auto* cmd = buf->AllocateDW(7);

	if (cmd == nullptr) {
		return nullptr;
	}

	cmd[0] = KYTY_PM4(7, Pm4::IT_DMA_DATA, 0u);
	cmd[1] = (static_cast<uint32_t>(engine) & 0x1u) |
	         ((static_cast<uint32_t>(src_cache_policy) & 0x3u) << 13u) |
	         ((static_cast<uint32_t>(dst) & 0x3u) << 20u) |
	         ((static_cast<uint32_t>(dst_cache_policy) & 0x3u) << 25u) |
	         ((static_cast<uint32_t>(src) & 0x3u) << 29u) |
	         ((static_cast<uint32_t>(block_engine) & 0x1u) << 31u);
	cmd[2] = static_cast<uint32_t>(src_address & 0xffffffffu);
	cmd[3] = static_cast<uint32_t>((src_address >> 32u) & 0xffffffffu);
	cmd[4] = static_cast<uint32_t>(dst_address_or_offset & 0xffffffffu);
	cmd[5] = static_cast<uint32_t>((dst_address_or_offset >> 32u) & 0xffffffffu);
	cmd[6] = (num_bytes & 0x03ffffffu) | ((static_cast<uint32_t>(src) & 0x4u) << 24u) |
	         ((static_cast<uint32_t>(dst) & 0x4u) << 25u) |
	         ((static_cast<uint32_t>(src) & 0x8u) << 25u) |
	         ((static_cast<uint32_t>(dst) & 0x8u) << 26u) |
	         ((static_cast<uint32_t>(wait_for_previous) & 0x1u) << 30u) |
	         ((static_cast<uint32_t>(write_confirm) & 0x1u) << 31u);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbJump(CommandBuffer* buf, uint8_t mode, uint8_t cache_policy,
                                        const uint32_t* target, uint32_t size_in_dwords) {
	PRINT_NAME();

	LOGF("\t mode           = 0x%02" PRIx8 "\n"
	     "\t cache_policy   = 0x%02" PRIx8 "\n"
	     "\t target         = 0x%016" PRIx64 "\n"
	     "\t size_in_dwords = %" PRIu32 "\n",
	     mode, cache_policy, reinterpret_cast<uint64_t>(target), size_in_dwords);

	if (buf == nullptr) {
		return nullptr;
	}

	auto* cmd = buf->AllocateDW(4);

	if (cmd == nullptr) {
		return nullptr;
	}

	auto vaddr = reinterpret_cast<uint64_t>(target);

	cmd[0] = KYTY_PM4(4, Pm4::IT_INDIRECT_BUFFER, 0u);
	cmd[1] = static_cast<uint32_t>(vaddr) & ~0x3u;
	cmd[2] = static_cast<uint32_t>(vaddr >> 32u);
	cmd[3] = 0x0f200000u | ((static_cast<uint32_t>(cache_policy) & 0x3u) << 28u) |
	         ((static_cast<uint32_t>(mode) & 0x1u) << 20u) | (size_in_dwords & 0xfffffu);

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsDcbJumpGetSize() {
	PRINT_NAME();

	return 16;
}

uint32_t KYTY_SYSV_ABI GraphicsDcbRewindGetSize() {
	PRINT_NAME();

	return 8;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbRewind(CommandBuffer* buf, uint32_t initial_state) {
	PRINT_NAME();

	LOGF("\t initial_state = 0x%08" PRIx32 "\n", initial_state);

	if (buf == nullptr) {
		return nullptr;
	}

	auto* cmd = buf->AllocateDW(2);

	if (cmd == nullptr) {
		return nullptr;
	}

	cmd[0] = KYTY_PM4(2, Pm4::IT_REWIND, 0u);
	cmd[1] = (initial_state & 0x1u) << 31u;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetPredication(CommandBuffer* buf, uint8_t condition, uint8_t op,
                                                  uint8_t wait_op, const volatile void* address,
                                                  uint32_t count_in_dwords) {
	PRINT_NAME();

	LOGF("\t condition       = 0x%02" PRIx8 "\n"
	     "\t op              = 0x%02" PRIx8 "\n"
	     "\t wait_op         = 0x%02" PRIx8 "\n"
	     "\t address         = 0x%016" PRIx64 "\n"
	     "\t count_in_dwords = %" PRIu32 "\n",
	     condition, op, wait_op, reinterpret_cast<uint64_t>(address), count_in_dwords);

	if (buf == nullptr) {
		return nullptr;
	}

	auto* cmd = buf->AllocateDW(4);

	if (cmd == nullptr) {
		return nullptr;
	}

	auto addr = reinterpret_cast<uint64_t>(address);

	cmd[0] = KYTY_PM4(4, Pm4::IT_SET_PREDICATION, 0u);
	cmd[1] = ((static_cast<uint32_t>(condition) & 0x1u) << 8u) |
	         ((static_cast<uint32_t>(wait_op) & 0x1u) << 12u) |
	         ((static_cast<uint32_t>(op) & 0x7u) << 16u);
	cmd[2] = static_cast<uint32_t>(addr) & ~0xfu;
	cmd[3] = static_cast<uint32_t>(addr >> 32u);

	(void)count_in_dwords;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsUnknownKRzWekV120(CommandBuffer* buf, uint32_t arg1, uint32_t arg2,
                                                  uint32_t arg3) {
	PRINT_NAME();

	LOGF("\t argc = 4\n"
	     "\t arg0 = 0x%016" PRIx64 "\n"
	     "\t arg1 = 0x%08" PRIx32 "\n"
	     "\t arg2 = 0x%08" PRIx32 "\n"
	     "\t arg3 = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(buf), arg1, arg2, arg3);

	if (buf == nullptr) {
		return nullptr;
	}

	auto* cmd = buf->AllocateDW(3);

	if (cmd == nullptr) {
		return nullptr;
	}

	cmd[0] = 0xc0017a00u;
	cmd[1] = 0x20000243u;

	uint32_t word8 = 0x400u;
	word8 |= (arg1 & 0x3u);
	word8 |= (arg2 & 0x3u) << 6u;
	word8 |= (arg3 & 0x1u) << 14u;

	cmd[2] = word8;

	return cmd;
}

int KYTY_SYSV_ABI GraphicsDmaDataPatchSetDstAddressOrOffset(uint32_t* cmd,
                                                            uint64_t  dst_address_or_offset) {
	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto op = (cmd[0] >> 8u) & 0xffu;
	if (op == Pm4::IT_DMA_DATA) {
		cmd[4] = static_cast<uint32_t>(dst_address_or_offset & 0xffffffffu);
		cmd[5] = static_cast<uint32_t>((dst_address_or_offset >> 32u) & 0xffffffffu);
		return OK;
	}

	return static_cast<int>(0x8a6c000cu);
}

int KYTY_SYSV_ABI GraphicsDmaDataPatchSetSrcAddressOrOffsetOrImmediate(
    uint32_t* cmd, uint64_t src_address_or_offset_or_immediate) {
	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto op = (cmd[0] >> 8u) & 0xffu;
	if (op == Pm4::IT_DMA_DATA) {
		cmd[2] = static_cast<uint32_t>(src_address_or_offset_or_immediate & 0xffffffffu);
		cmd[3] = static_cast<uint32_t>((src_address_or_offset_or_immediate >> 32u) & 0xffffffffu);
		return OK;
	}

	return static_cast<int>(0x8a6c000cu);
}

uint32_t KYTY_SYSV_ABI GraphicsGetPacketSize(uint32_t* packet) {
	const auto cmd_id = packet[0];
	if ((cmd_id & 0x3fffff00u) == 0x3fff1000u) {
		return 1;
	}

	return KYTY_PM4_LEN(cmd_id);
}

int KYTY_SYSV_ABI GraphicsSetPacketPredication(uint32_t* packet, uint32_t predication) {
	PRINT_NAME();

	LOGF("\t packet      = 0x%016" PRIx64 "\n"
	     "\t predication = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(packet), predication);

	LOGF("\t packet0     = 0x%08" PRIx32 ", op = 0x%02" PRIx32 ", r = 0x%02" PRIx32
	     ", len = %" PRIu32 "\n",
	     packet[0], (packet[0] >> 8u) & 0xffu, KYTY_PM4_R(packet[0]),
	     GraphicsGetPacketSize(packet));

	packet[0] = (packet[0] & ~1u) | (static_cast<uint8_t>(predication) == 1 ? 1u : 0u);

	return OK;
}

int KYTY_SYSV_ABI GraphicsSetRangePredication(uint32_t* start, const volatile uint32_t* end,
                                              uint32_t predication) {
	PRINT_NAME();

	LOGF("\t start       = 0x%016" PRIx64 "\n"
	     "\t end         = 0x%016" PRIx64 "\n"
	     "\t predication = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(start), reinterpret_cast<uint64_t>(end), predication);

	auto* packet    = start;
	auto* range_end = const_cast<uint32_t*>(reinterpret_cast<const volatile uint32_t*>(end));
	auto  packet_va = reinterpret_cast<uintptr_t>(packet);
	auto  end_va    = reinterpret_cast<uintptr_t>(range_end);

	if (packet_va >= end_va) {
		return OK;
	}

	const uint32_t predication_bit = (static_cast<uint8_t>(predication) == 1 ? 1u : 0u);
	while (packet_va < end_va) {
		const auto cmd_id = packet[0];
		packet[0]         = (cmd_id & ~1u) | predication_bit;

		auto size = KYTY_PM4_LEN(cmd_id);
		if ((cmd_id & 0x3fffff00u) == 0x3fff1000u) {
			size = 1;
		}

		packet_va += size * sizeof(uint32_t);
		packet = reinterpret_cast<uint32_t*>(packet_va);
	}

	return OK;
}

int KYTY_SYSV_ABI GraphicsCondExecPatchSetEnd(uint32_t* cmd, const volatile uint32_t* buffer) {
	PRINT_NAME();

	LOGF("\t cmd    = 0x%016" PRIx64 "\n"
	     "\t buffer = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(cmd), reinterpret_cast<uint64_t>(buffer));

	if (cmd == nullptr || buffer == nullptr) {
		return static_cast<int>(0x8a6c000cu);
	}

	auto op = (cmd[0] >> 8u) & 0xffu;
	if (op != Pm4::IT_COND_EXEC) {
		return static_cast<int>(0x8a6c000cu);
	}

	auto* packet_end = cmd + 5;
	auto* range_end  = const_cast<uint32_t*>(reinterpret_cast<const volatile uint32_t*>(buffer));
	if (range_end < packet_end) {
		return static_cast<int>(0x8a6c000cu);
	}

	auto num_dwords = static_cast<uint64_t>(range_end - packet_end);
	if (num_dwords > 0x3fffu) {
		return static_cast<int>(0x8a6c000cu);
	}

	cmd[4] = (cmd[4] & ~0x3fffu) | static_cast<uint32_t>(num_dwords);
	return OK;
}

int KYTY_SYSV_ABI GraphicsCondExecPatchSetCommandAddress(uint32_t*                cmd,
                                                         const volatile uint32_t* command) {
	PRINT_NAME();

	LOGF("\t cmd     = 0x%016" PRIx64 "\n"
	     "\t command = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(cmd), reinterpret_cast<uint64_t>(command));

	if (cmd == nullptr || command == nullptr) {
		return static_cast<int>(0x8a6c000cu);
	}

	auto op = (cmd[0] >> 8u) & 0xffu;
	if (op != Pm4::IT_COND_EXEC || (reinterpret_cast<uintptr_t>(command) & 0x3u) != 0) {
		return static_cast<int>(0x8a6c000cu);
	}

	auto addr = reinterpret_cast<uint64_t>(command);
	cmd[1]    = (cmd[1] & 0x3u) | (static_cast<uint32_t>(addr) & 0xfffffffcu);
	cmd[2]    = static_cast<uint32_t>(addr >> 32u);

	return OK;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbWriteData(CommandBuffer* buf, uint8_t dst, uint8_t cache_policy,
                                             uint64_t address_or_offset, const void* data,
                                             uint32_t num_dwords, uint8_t increment,
                                             uint8_t write_confirm) {
	PRINT_NAME();

	LOGF("\t dst               = 0x%02" PRIx8 "\n"
	     "\t cache_policy      = 0x%02" PRIx8 "\n"
	     "\t address_or_offset = 0x%016" PRIx64 "\n"
	     "\t data              = 0x%016" PRIx64 "\n"
	     "\t num_dwords        = %" PRIu32 "\n"
	     "\t increment         = 0x%02" PRIx8 "\n"
	     "\t write_confirm     = 0x%02" PRIx8 "\n",
	     dst, cache_policy, address_or_offset, reinterpret_cast<uint64_t>(data), num_dwords,
	     increment, write_confirm);

	if (buf == nullptr || data == nullptr) {
		LOGF_COLOR(Log::Color::Red, "\t invalid arguments\n");
		return nullptr;
	}
	if ((4 + num_dwords - 2u) > 0x3fffu) {
		LOGF_COLOR(Log::Color::Red, "\t packet is too large\n");
		return nullptr;
	}

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(4 + num_dwords);

	if (cmd == nullptr) {
		LOGF_COLOR(Log::Color::Red, "\t failed to allocate command\n");
		return nullptr;
	}

	const auto dst_value = static_cast<uint32_t>(dst);
	const auto write_confirm_value =
	    (dst == 0u ? 0u : (static_cast<uint32_t>(write_confirm) & 0x1u));

	cmd[0] = KYTY_PM4(4 + num_dwords, Pm4::IT_WRITE_DATA, 0u);
	cmd[1] = ((dst_value & 0x1u) << 30u) | ((dst_value & 0x1eu) << 7u) |
	         ((static_cast<uint32_t>(increment) & 0x1u) << 16u) | (write_confirm_value << 20u) |
	         ((static_cast<uint32_t>(cache_policy) & 0x3u) << 25u);
	cmd[2] = static_cast<uint32_t>(address_or_offset) & ~0x3u;
	cmd[3] = static_cast<uint32_t>(address_or_offset >> 32u);

	memcpy(cmd + 4, data, static_cast<size_t>(num_dwords) * 4);

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsDcbWriteDataGetSize(uint32_t num_dwords) {
	PRINT_NAME();

	LOGF("\t num_dwords = %" PRIu32 "\n", num_dwords);

	return 4u * num_dwords + 16u;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbGetLodStats(CommandBuffer* buf, uint8_t cache_policy,
                                               const volatile void* buffer,
                                               uint32_t buffer_size_in_bytes, uint32_t reset_count,
                                               uint8_t force_reset, uint8_t report_and_reset,
                                               uint32_t reporting_interval_in_100k_clocks) {
	PRINT_NAME();

	LOGF("\t cache_policy                      = 0x%02" PRIx8 "\n"
	     "\t buffer                            = 0x%016" PRIx64 "\n"
	     "\t buffer_size_in_bytes              = %" PRIu32 "\n"
	     "\t reset_count                       = %" PRIu32 "\n"
	     "\t force_reset                       = 0x%02" PRIx8 "\n"
	     "\t report_and_reset                  = 0x%02" PRIx8 "\n"
	     "\t reporting_interval_in_100k_clocks = %" PRIu32 "\n",
	     cache_policy, reinterpret_cast<uint64_t>(buffer), buffer_size_in_bytes, reset_count,
	     force_reset, report_and_reset, reporting_interval_in_100k_clocks);

	if (buf == nullptr) {
		return nullptr;
	}

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(5);

	if (cmd == nullptr) {
		return nullptr;
	}

	const auto buffer_addr = reinterpret_cast<uint64_t>(buffer);

	cmd[0] = KYTY_PM4(5, Pm4::IT_GET_LOD_STATS, 0u);
	cmd[1] = buffer_size_in_bytes;
	cmd[2] = static_cast<uint32_t>(buffer_addr & 0xffffffc0u);
	cmd[3] = static_cast<uint32_t>((buffer_addr >> 32u) & 0xffffffffu);
	cmd[4] = ((static_cast<uint32_t>(cache_policy) & 0x3u) << 28u) |
	         ((static_cast<uint32_t>(report_and_reset) & 0x1u) << 19u) |
	         ((static_cast<uint32_t>(force_reset) & 0x1u) << 18u) | ((reset_count & 0xffu) << 10u) |
	         ((reporting_interval_in_100k_clocks & 0xffu) << 2u);

	return cmd;
}

int KYTY_SYSV_ABI GraphicsWaitRegMemPatchAddress(uint32_t* cmd, const volatile void* address) {
	PRINT_NAME();

	LOGF("\t cmd     = 0x%016" PRIx64 "\n"
	     "\t address = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(cmd), reinterpret_cast<uint64_t>(address));

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(address);
	auto op    = (cmd[0] >> 8u) & 0xffu;

	if (op == Pm4::IT_NOP && KYTY_PM4_R(cmd[0]) == Pm4::R_WAIT_MEM_32) {
		cmd[1] = static_cast<uint32_t>(vaddr) & ~0x3u;
		cmd[2] = static_cast<uint32_t>(vaddr >> 32u) & 0x3ffffu;
	} else if (op == Pm4::IT_NOP && KYTY_PM4_R(cmd[0]) == Pm4::R_WAIT_MEM_64) {
		cmd[1] = static_cast<uint32_t>(vaddr) & ~0x7u;
		cmd[2] = static_cast<uint32_t>(vaddr >> 32u) & 0x3ffffu;
	} else {
		EXIT("unsupported waitOnAddress packet for address patch: 0x%08" PRIx32 "\n", cmd[0]);
	}

	return OK;
}

int KYTY_SYSV_ABI GraphicsWaitRegMemPatchReference(uint32_t* cmd, uint64_t reference) {
	PRINT_NAME();

	LOGF("\t cmd       = 0x%016" PRIx64 "\n"
	     "\t reference = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(cmd), reference);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto op = (cmd[0] >> 8u) & 0xffu;

	if (op == Pm4::IT_NOP && KYTY_PM4_R(cmd[0]) == Pm4::R_WAIT_MEM_32) {
		cmd[4] = static_cast<uint32_t>(reference & 0xffffffffu);
	} else if (op == Pm4::IT_NOP && KYTY_PM4_R(cmd[0]) == Pm4::R_WAIT_MEM_64) {
		cmd[5] = static_cast<uint32_t>(reference & 0xffffffffu);
		cmd[6] = static_cast<uint32_t>((reference >> 32u) & 0xffffffffu);
	} else {
		EXIT("unsupported waitOnAddress packet for reference patch: 0x%08" PRIx32 "\n", cmd[0]);
	}

	return OK;
}

int KYTY_SYSV_ABI GraphicsQueueEndOfPipeActionPatchAddress(uint32_t*             cmd,
                                                           const volatile Label* address) {
	PRINT_NAME();

	// Not sure

	LOGF("\t cmd     = 0x%016" PRIx64 "\n"
	     "\t address = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(cmd), reinterpret_cast<uint64_t>(address));

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(address);
	auto op    = (cmd[0] >> 8u) & 0xffu;

	if ((op == Pm4::IT_NOP && KYTY_PM4_R(cmd[0]) == Pm4::R_RELEASE_MEM) ||
	    op == Pm4::IT_RELEASE_MEM) {
		cmd[3] = static_cast<uint32_t>(vaddr & 0xffffffffu);
		cmd[4] = static_cast<uint32_t>((vaddr >> 32u) & 0xffffffffu);
	} else if (op == Pm4::IT_EVENT_WRITE_EOP) {
		cmd[2] = static_cast<uint32_t>(vaddr & 0xffffffffu);
		cmd[3] = (cmd[3] & 0xffff0000u) | static_cast<uint32_t>((vaddr >> 32u) & 0xffffu);
	} else {
		EXIT("unsupported queueEndOfPipeAction packet for address patch: 0x%08" PRIx32 "\n",
		     cmd[0]);
	}

	return OK;
}

int KYTY_SYSV_ABI GraphicsQueueEndOfPipeActionPatchData(uint32_t* cmd, uint32_t context_id,
                                                        uint32_t data_sel, uint64_t data) {
	PRINT_NAME();

	LOGF("\t cmd        = 0x%016" PRIx64 "\n"
	     "\t context_id = 0x%08" PRIx32 "\n"
	     "\t data_sel   = 0x%08" PRIx32 "\n"
	     "\t data       = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(cmd), context_id, data_sel, data);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto op = (cmd[0] >> 8u) & 0xffu;

	if ((op == Pm4::IT_NOP && KYTY_PM4_R(cmd[0]) == Pm4::R_RELEASE_MEM) ||
	    op == Pm4::IT_RELEASE_MEM) {
		uint64_t packet_data = data;
		if (op == Pm4::IT_NOP && KYTY_PM4_R(cmd[0]) == Pm4::R_RELEASE_MEM && context_id > 1 &&
		    data_sel == 1) {
			// Agc Core ring-buffer release packets pack the segment generation
			// into bits 24..31 and wrap that byte every 256 submissions. The
			// patch context carries the monotonic generation, so expand the
			// packed value before it reaches the command processor.
			packet_data = (static_cast<uint64_t>(context_id - 2u) << 24u) | (data & 0x00ffffffull);
		}

		cmd[5] = static_cast<uint32_t>(packet_data & 0xffffffffu);
		cmd[6] = static_cast<uint32_t>((packet_data >> 32u) & 0xffffffffu);
	} else if (op == Pm4::IT_EVENT_WRITE_EOP) {
		cmd[4] = static_cast<uint32_t>(data & 0xffffffffu);
		cmd[5] = static_cast<uint32_t>((data >> 32u) & 0xffffffffu);
	} else {
		EXIT("unsupported queueEndOfPipeAction packet for data patch: 0x%08" PRIx32 "\n", cmd[0]);
	}

	return OK;
}

static uint32_t wait_reg_mem_poll_cycles_to_packet(uint32_t poll_cycles) {
	return std::min(poll_cycles >> 4u, 0xffffu);
}

static uint32_t wait_reg_mem32_control(uint8_t compare_function, uint8_t op, uint8_t cache_policy) {
	return 0x10u | (static_cast<uint32_t>(compare_function) & 0x7u) |
	       ((static_cast<uint32_t>(op) & 0x3u) << 8u) | ((static_cast<uint32_t>(op) & 0xcu) << 4u) |
	       ((static_cast<uint32_t>(cache_policy) & 0x3u) << 25u);
}

static uint32_t wait_reg_mem64_control(uint8_t compare_function, uint8_t op, uint8_t cache_policy) {
	return 0x10u | (static_cast<uint32_t>(compare_function) & 0x7u) |
	       ((static_cast<uint32_t>(op) & 0x1u) << 8u) | ((static_cast<uint32_t>(op) & 0x6u) << 5u) |
	       ((static_cast<uint32_t>(cache_policy) & 0x3u) << 25u);
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbWaitRegMem(CommandBuffer* buf, uint8_t size,
                                              uint8_t compare_function, uint8_t op,
                                              uint8_t cache_policy, const volatile void* address,
                                              uint64_t reference, uint64_t mask,
                                              uint32_t poll_cycles) {
	PRINT_NAME();

	LOGF("\t size             = 0x%02" PRIx8 "\n"
	     "\t compare_function = 0x%02" PRIx8 "\n"
	     "\t op               = 0x%02" PRIx8 "\n"
	     "\t cache_policy     = 0x%02" PRIx8 "\n"
	     "\t address          = 0x%016" PRIx64 "\n"
	     "\t reference        = 0x%016" PRIx64 "\n"
	     "\t mask             = 0x%016" PRIx64 "\n"
	     "\t poll_cycles      = %" PRIu32 "\n",
	     size, compare_function, op, cache_policy, reinterpret_cast<uint64_t>(address), reference,
	     mask, poll_cycles);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	if (size != 0 && size != 1) {
		LOGF_COLOR(Log::Color::Red,
		           "\t warning: unsupported size 0x%02" PRIx8 ", expected 0 (32b) or 1 (64b)\n",
		           size);
		return nullptr;
	}
	if (cache_policy > 3) {
		LOGF_COLOR(Log::Color::Red,
		           "\t warning: unsupported cache_policy 0x%02" PRIx8 ", expected 0..3\n",
		           cache_policy);
	}

	buf->DbgDump();

	auto address_value = reinterpret_cast<uint64_t>(address);
	bool wait32        = (size == 0);
	auto poll          = wait_reg_mem_poll_cycles_to_packet(poll_cycles);

	auto* cmd = buf->AllocateDW(wait32 ? 7 : 9);

	if (cmd == nullptr) {
		return nullptr;
	}

	if (wait32) {
		cmd[0] = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_WAIT_MEM_32);
		cmd[1] = static_cast<uint32_t>(address_value) & ~0x3u;
		cmd[2] = static_cast<uint32_t>(address_value >> 32u) & 0x3ffffu;
		cmd[3] = static_cast<uint32_t>(mask & 0xffffffffu);
		cmd[4] = static_cast<uint32_t>(reference & 0xffffffffu);
		cmd[5] = wait_reg_mem32_control(compare_function, op, cache_policy);
		cmd[6] = poll;

		return cmd;
	}

	cmd[0] = KYTY_PM4(9, Pm4::IT_NOP, Pm4::R_WAIT_MEM_64);
	cmd[1] = static_cast<uint32_t>(address_value) & ~0x7u;
	cmd[2] = static_cast<uint32_t>(address_value >> 32u) & 0x3ffffu;
	cmd[3] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(mask) & 0xffffffffu);
	cmd[4] = static_cast<uint32_t>((reinterpret_cast<uint64_t>(mask) >> 32u) & 0xffffffffu);
	cmd[5] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(reference) & 0xffffffffu);
	cmd[6] = static_cast<uint32_t>((reinterpret_cast<uint64_t>(reference) >> 32u) & 0xffffffffu);
	cmd[7] = wait_reg_mem64_control(compare_function, op, cache_policy);
	cmd[8] = poll;

	return cmd;
}

uint32_t KYTY_SYSV_ABI GraphicsDcbWaitOnAddressGetSize(uint32_t size) {
	PRINT_NAME();

	switch (size) {
		case 0: return 14u * 4u;
		case 1: return 16u * 4u;
		default: return 0;
	}
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbPushMarker(CommandBuffer* buf, const char* str,
                                              uint32_t /*color*/) {
	if (buf == nullptr) {
		return nullptr;
	}

	if (str == nullptr) {
		str = "";
	}

	const auto len            = strlen(str) + 1;
	const auto payload_dwords = static_cast<uint32_t>((len + 3) / 4);
	const auto size           = 1u + (payload_dwords == 0 ? 1u : payload_dwords);
	auto*      cmd            = buf->AllocateDW(size);

	if (cmd == nullptr) {
		return nullptr;
	}

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PUSH_MARKER);
	memset(cmd + 1, 0, static_cast<size_t>(size - 1) * sizeof(uint32_t));
	memcpy(cmd + 1, str, len);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetMarker(CommandBuffer* buf, const char* str, uint32_t color) {
	return GraphicsDcbPushMarker(buf, str, color);
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbSetMarker(CommandBuffer* buf, const char* str, uint32_t color) {
	return GraphicsDcbSetMarker(buf, str, color);
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbPushMarker(CommandBuffer* buf, const char* str, uint32_t color) {
	return GraphicsDcbPushMarker(buf, str, color);
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbPopMarker(CommandBuffer* buf) {
	if (buf == nullptr) {
		return nullptr;
	}

	auto* cmd = buf->AllocateDW(2);

	if (cmd == nullptr) {
		return nullptr;
	}

	cmd[0] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_POP_MARKER);
	cmd[1] = 0;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbPopMarker(CommandBuffer* buf) {
	return GraphicsDcbPopMarker(buf);
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetFlip(CommandBuffer* buf, uint32_t video_out_handle,
                                           int32_t display_buffer_index, uint32_t flip_mode,
                                           int64_t flip_arg) {
	PRINT_NAME();

	LOGF("\t video_out_handle     = %" PRIu32 "\n"
	     "\t display_buffer_index = %" PRId32 "\n"
	     "\t flip_mode            = %" PRIu32 "\n"
	     "\t flip_arg             = %" PRId64 "\n",
	     video_out_handle, display_buffer_index, flip_mode, flip_arg);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(6);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(6, Pm4::IT_NOP, Pm4::R_FLIP);
	cmd[1] = video_out_handle;
	cmd[2] = display_buffer_index;
	cmd[3] = flip_mode;
	cmd[4] = static_cast<uint32_t>(static_cast<uint64_t>(flip_arg) & 0xffffffffu);
	cmd[5] = static_cast<uint32_t>((static_cast<uint64_t>(flip_arg) >> 32u) & 0xffffffffu);

	return cmd;
}

} // namespace Gen5

namespace Gen5Driver {

LIB_NAME("Graphics5Driver", "Graphics5Driver");

struct TessellationDriverState {
	uint64_t tf_ring_base      = 0;
	uint32_t tf_ring_size      = 0;
	uint64_t hs_offchip_value0 = 0;
	uint64_t hs_offchip_value1 = 0;
	uint64_t hs_offchip_value2 = 0;
};

static TessellationDriverState g_tessellation_driver_state {};

struct Packet {
	uint32_t* addr;
	uint32_t  dw_num;
	uint8_t   pad[4];
};

static bool dcb_has_queued_interrupt(const uint32_t* dcb, uint32_t size_in_dwords) {
	if (dcb == nullptr) {
		return false;
	}

	for (uint32_t offset = 0; offset < size_in_dwords;) {
		auto cmd_id = dcb[offset];
		auto len    = KYTY_PM4_LEN(cmd_id);
		if (len == 0 || len > size_in_dwords - offset) {
			return false;
		}

		auto op = (cmd_id >> 8u) & 0xffu;
		if (op == Pm4::IT_NOP && KYTY_PM4_R(cmd_id) == Pm4::R_RELEASE_MEM && len >= 6) {
			auto interrupt = (dcb[offset + 2] >> 24u) & 0x7u;
			if (interrupt == 1 || interrupt == 2 || interrupt >= 4) {
				return true;
			}
		}

		offset += len;
	}

	return false;
}

static void submit_dcb(uint32_t* dcb, uint32_t size_in_dwords) {
	GraphicsDbgDumpDcb("d", size_in_dwords, dcb);
	GraphicsRunSubmit(dcb, size_in_dwords, nullptr, 0,
	                  !dcb_has_queued_interrupt(dcb, size_in_dwords));
	Gen5::track_pending_graphics_segment_after_submit(dcb, size_in_dwords);
}

static std::vector<uint64_t> collect_acb_wait_addresses(const uint32_t* acb,
                                                        uint32_t        size_in_dwords) {
	std::vector<uint64_t> addresses;

	for (uint32_t offset = 0; offset < size_in_dwords;) {
		auto cmd_id = acb[offset];
		auto len    = KYTY_PM4_LEN(cmd_id);
		if (len == 0 || len > size_in_dwords - offset) {
			return addresses;
		}

		auto op = (cmd_id >> 8u) & 0xffu;
		if (op == Pm4::IT_NOP && KYTY_PM4_R(cmd_id) == Pm4::R_WAIT_MEM_32 && len >= 7) {
			auto address = static_cast<uint64_t>(acb[offset + 1]) |
			               (static_cast<uint64_t>(acb[offset + 2]) << 32u);
			if (address != 0) {
				addresses.push_back(address);
			}
		} else if (op == Pm4::IT_NOP && KYTY_PM4_R(cmd_id) == Pm4::R_WAIT_MEM_64 && len >= 9) {
			auto address = static_cast<uint64_t>(acb[offset + 1]) |
			               (static_cast<uint64_t>(acb[offset + 2]) << 32u);
			if (address != 0) {
				addresses.push_back(address);
			}
		}

		offset += len;
	}

	return addresses;
}

static bool acb_waits_for_address(const std::vector<uint64_t>& wait_addresses,
                                  uint64_t                     release_address) {
	for (auto address: wait_addresses) {
		if (address == release_address) {
			return true;
		}
	}

	return false;
}

static void flush_pending_graphics_segment_before_acb(const uint32_t* acb,
                                                      uint32_t        acb_size_in_dwords) {
	uint32_t* dcb            = nullptr;
	uint32_t  size_in_dwords = 0;
	auto      wait_addresses = collect_acb_wait_addresses(acb, acb_size_in_dwords);

	{
		std::lock_guard lock(Gen5::g_pending_graphics_segment_mutex);
		if (!wait_addresses.empty() && Gen5::g_pending_graphics_segment.start != nullptr) {
			auto* scan        = Gen5::g_pending_graphics_segment.start;
			auto* matched_end = Gen5::g_pending_graphics_segment.start;
			while (scan < Gen5::g_pending_graphics_segment.end) {
				auto cmd_id = *scan;
				if (cmd_id == 0x80000000u) {
					scan++;
					continue;
				}
				if ((cmd_id & 0xC0000000u) != 0xC0000000u) {
					break;
				}

				auto len = KYTY_PM4_LEN(cmd_id);
				if (len == 0 ||
				    len > static_cast<uint32_t>(Gen5::g_pending_graphics_segment.end - scan)) {
					break;
				}

				if (((cmd_id >> 8u) & 0xffu) == Pm4::IT_NOP &&
				    KYTY_PM4_R(cmd_id) == Pm4::R_RELEASE_MEM && len >= 7) {
					auto release_addr =
					    static_cast<uint64_t>(scan[3]) | (static_cast<uint64_t>(scan[4]) << 32u);
					if (acb_waits_for_address(wait_addresses, release_addr)) {
						matched_end = scan + len;
					}
				}

				scan += len;
			}

			if (matched_end > Gen5::g_pending_graphics_segment.start) {
				Gen5::g_pending_graphics_segment.end = matched_end;
			}
		}

		if (Gen5::g_pending_graphics_segment.start != nullptr &&
		    Gen5::g_pending_graphics_segment.end > Gen5::g_pending_graphics_segment.start) {
			auto* scan      = Gen5::g_pending_graphics_segment.start;
			auto* valid_end = Gen5::g_pending_graphics_segment.start;
			while (scan < Gen5::g_pending_graphics_segment.end) {
				auto cmd_id = *scan;
				if (cmd_id == 0x80000000u) {
					scan++;
					valid_end = scan;
					continue;
				}
				if ((cmd_id & 0xC0000000u) != 0xC0000000u) {
					break;
				}

				auto len = KYTY_PM4_LEN(cmd_id);
				if (len == 0 ||
				    len > static_cast<uint32_t>(Gen5::g_pending_graphics_segment.end - scan)) {
					break;
				}

				scan += len;
				valid_end = scan;
			}

			if (valid_end < Gen5::g_pending_graphics_segment.end) {
				static std::atomic<uint32_t> log_count {0};
				if (log_count.fetch_add(1) < 64) {
					LOGF("\t trimming pending graphics segment: addr = 0x%016" PRIx64
					     ", old_dw = 0x%08" PRIx32 ", new_dw = 0x%08" PRIx32 "\n",
					     reinterpret_cast<uint64_t>(Gen5::g_pending_graphics_segment.start),
					     static_cast<uint32_t>(Gen5::g_pending_graphics_segment.end -
					                           Gen5::g_pending_graphics_segment.start),
					     static_cast<uint32_t>(valid_end - Gen5::g_pending_graphics_segment.start));
				}
				Gen5::g_pending_graphics_segment.end = valid_end;
			}
		}

		if (Gen5::g_pending_graphics_segment.start == nullptr ||
		    Gen5::g_pending_graphics_segment.end <= Gen5::g_pending_graphics_segment.start) {
			return;
		}

		dcb            = Gen5::g_pending_graphics_segment.start;
		size_in_dwords = static_cast<uint32_t>(Gen5::g_pending_graphics_segment.end -
		                                       Gen5::g_pending_graphics_segment.start);
	}

	LOGF("\t flushing pending graphics segment before ACB: addr = 0x%016" PRIx64
	     ", dw_num = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(dcb), size_in_dwords);

	submit_dcb(dcb, size_in_dwords);
}

int KYTY_SYSV_ABI GraphicsDriverSubmitDcb(const Packet* packet) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(packet == nullptr);
	EXIT_NOT_IMPLEMENTED(packet->pad[0] != 0);

	LOGF("\t addr   = 0x%016" PRIx64 "\n"
	     "\t dw_num = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(packet->addr), packet->dw_num);

	submit_dcb(packet->addr, packet->dw_num);

	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverSubmitMultiDcbs(uint32_t* const* dcb_gpu_addrs,
                                                const uint32_t*  dcb_sizes_in_dwords,
                                                uint32_t         count) {
	PRINT_NAME();

	LOGF("\t count = %" PRIu32 "\n", count);

	if (count == 0) {
		return OK;
	}
	if (dcb_gpu_addrs == nullptr || dcb_sizes_in_dwords == nullptr) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	for (uint32_t i = 0; i < count; i++) {
		auto*    dcb            = dcb_gpu_addrs[i];
		uint32_t size_in_dwords = dcb_sizes_in_dwords[i];

		LOGF("\t dcb[%" PRIu32 "]  = 0x%016" PRIx64 "\n"
		     "\t size[%" PRIu32 "] = 0x%08" PRIx32 "\n",
		     i, reinterpret_cast<uint64_t>(dcb), i, size_in_dwords);

		if (dcb == nullptr) {
			continue;
		}
		submit_dcb(dcb, size_in_dwords);
	}

	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverSubmitCommandBuffer(uint32_t queue, uint32_t* dcb,
                                                    uint32_t size_in_dwords) {
	PRINT_NAME();

	LOGF("\t queue = 0x%08" PRIx32 "\n"
	     "\t dcb   = 0x%016" PRIx64 "\n"
	     "\t size  = 0x%08" PRIx32 "\n",
	     queue, reinterpret_cast<uint64_t>(dcb), size_in_dwords);

	if (dcb == nullptr || size_in_dwords == 0) {
		return OK;
	}

	submit_dcb(dcb, size_in_dwords);

	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverSubmitMultiCommandBuffers(uint32_t queue, uint32_t* const* dcbs,
                                                          const uint32_t* sizes_in_dwords,
                                                          uint32_t        count) {
	PRINT_NAME();

	LOGF("\t queue = 0x%08" PRIx32 "\n"
	     "\t count = %" PRIu32 "\n",
	     queue, count);

	if (count == 0) {
		return OK;
	}
	if (dcbs == nullptr || sizes_in_dwords == nullptr) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	for (uint32_t i = 0; i < count; i++) {
		GraphicsDriverSubmitCommandBuffer(queue, dcbs[i], sizes_in_dwords[i]);
	}

	return OK;
}

static void submit_acb(uint32_t queue, uint32_t* acb, uint32_t size_in_dwords) {
	if (acb != nullptr && size_in_dwords >= 5) {
		auto descriptor_addr =
		    static_cast<uint64_t>(acb[0]) | (static_cast<uint64_t>(acb[1]) << 32u);
		auto descriptor_size  = acb[2];
		auto descriptor_flags = acb[3];
		auto descriptor_magic = acb[4];
		if (descriptor_addr != 0 && descriptor_size != 0 && descriptor_flags == 0 &&
		    descriptor_magic == 0x5533ccaau) {
			LOGF("\t descriptor addr = 0x%016" PRIx64 "\n"
			     "\t descriptor size = 0x%08" PRIx32 "\n"
			     "\t descriptor magic = 0x%08" PRIx32 "\n",
			     descriptor_addr, descriptor_size, descriptor_magic);
			acb            = reinterpret_cast<uint32_t*>(descriptor_addr);
			size_in_dwords = descriptor_size;
		}
	}

	if (acb == nullptr || size_in_dwords == 0) {
		return;
	}

	for (uint32_t i = 0; i < std::min<uint32_t>(size_in_dwords, 8); i++) {
		LOGF("\t acb[%u] = 0x%08" PRIx32 "\n", i, acb[i]);
	}

	flush_pending_graphics_segment_before_acb(acb, size_in_dwords);

	GraphicsDbgDumpDcb("a", size_in_dwords, acb);

	const bool trigger_interrupt_on_done = !dcb_has_queued_interrupt(acb, size_in_dwords);
	GraphicsRunSubmitCompute(queue, acb, size_in_dwords, trigger_interrupt_on_done);
}

int KYTY_SYSV_ABI GraphicsDriverSubmitAcb(uint32_t queue, const Packet* packet) {
	PRINT_NAME();

	LOGF("\t queue = 0x%08" PRIx32 "\n"
	     "\t packet = 0x%016" PRIx64 "\n",
	     queue, reinterpret_cast<uint64_t>(packet));

	if (packet == nullptr) {
		return OK;
	}
	LOGF("\t acb   = 0x%016" PRIx64 "\n"
	     "\t size  = 0x%08" PRIx32 "\n"
	     "\t flags = 0x%02" PRIx8 "\n",
	     reinterpret_cast<uint64_t>(packet->addr), packet->dw_num, packet->pad[0]);

	submit_acb(queue, packet->addr, packet->dw_num);

	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverSubmitMultiAcbs(uint32_t queue, uint32_t* const* acbs,
                                                const uint32_t* sizes_in_dwords, uint32_t count) {
	PRINT_NAME();

	LOGF("\t queue = 0x%08" PRIx32 "\n"
	     "\t count = %" PRIu32 "\n",
	     queue, count);

	if (count == 0) {
		return OK;
	}
	if (acbs == nullptr || sizes_in_dwords == nullptr) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	for (uint32_t i = 0; i < count; i++) {
		submit_acb(queue, acbs[i], sizes_in_dwords[i]);
	}

	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverAddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id,
                                           void* udata) {
	PRINT_NAME();

	if (eq == nullptr) {
		return LibKernel::KERNEL_ERROR_EBADF;
	}

	return Sync::AddEqEvent(eq, id, udata);
}

int KYTY_SYSV_ABI GraphicsDriverDeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id) {
	PRINT_NAME();

	if (eq == nullptr) {
		return LibKernel::KERNEL_ERROR_EBADF;
	}

	return Sync::DeleteEqEvent(eq, id);
}

int KYTY_SYSV_ABI GraphicsDriverGetEqEventType(const LibKernel::EventQueue::KernelEvent* ev) {
	PRINT_NAME();

	if (ev == nullptr) {
		return 0;
	}

	if (ev->filter == LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS) {
		return static_cast<int>(ev->ident);
	}

	return static_cast<int>(ev->data);
}

uint32_t KYTY_SYSV_ABI GraphicsDriverGetEqContextId(const LibKernel::EventQueue::KernelEvent* ev) {
	PRINT_NAME();

	if (ev == nullptr) {
		return 0;
	}

	if (ev->filter == LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS) {
		return static_cast<uint32_t>(ev->data);
	}

	return static_cast<uint32_t>(ev->ident);
}

int KYTY_SYSV_ABI GraphicsDriverSetTFRing(const volatile void* base, uint32_t size) {
	PRINT_NAME();

	g_tessellation_driver_state.tf_ring_base = reinterpret_cast<uint64_t>(base);
	g_tessellation_driver_state.tf_ring_size = size;

	LOGF("\t base = 0x%016" PRIx64 "\n"
	     "\t size = 0x%08" PRIx32 "\n",
	     g_tessellation_driver_state.tf_ring_base, g_tessellation_driver_state.tf_ring_size);

	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverSetHsOffchipParam(uint64_t value0, uint64_t value1,
                                                  uint64_t value2) {
	PRINT_NAME();

	g_tessellation_driver_state.hs_offchip_value0 = value0;
	g_tessellation_driver_state.hs_offchip_value1 = value1;
	g_tessellation_driver_state.hs_offchip_value2 = value2;

	LOGF("\t value0 = 0x%016" PRIx64 "\n"
	     "\t value1 = 0x%016" PRIx64 "\n"
	     "\t value2 = 0x%016" PRIx64 "\n",
	     value0, value1, value2);

	return OK;
}

bool KYTY_SYSV_ABI GraphicsDriverIsCaptureInProgress() {
	PRINT_NAME();

	return false;
}

int KYTY_SYSV_ABI GraphicsDriverUnknownU9ueyEhSkF4() {
	PRINT_NAME();

	return static_cast<int>(0x8a6c9018u);
}

} // namespace Gen5Driver

} // namespace Libs::Graphics
