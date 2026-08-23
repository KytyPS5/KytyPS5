#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERDECODER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERDECODER_H_

#include "common/common.h"
#include "common/magicEnum.h"

#include <span>
#include <string_view>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Decoder {

enum class Family {
	Unknown,
	SOP1,
	SOP2,
	SOPK,
	SOPC,
	SOPP,
	VOP1,
	VOP2,
	VOP3,
	VOP3P,
	VOPC,
	VINTRP,
	SMEM,
	MUBUF,
	MTBUF,
	FLAT,
	DS,
	MIMG,
	EXP
};

enum class Opcode {
	UNKNOWN,
	UNSUPPORTED,

	S_MOV_B32,
	S_MOV_B64,
	S_MOVK_I32,
	S_ABS_I32,
	S_BREV_B32,
	S_BCNT1_I32_B32,
	S_BCNT1_I32_B64,
	S_FF1_I32_B32,
	S_FF1_I32_B64,
	S_FLBIT_I32_B64,
	S_BITREPLICATE_B64_B32,
	S_QUADMASK_B64,
	S_GETPC_B64,
	S_SETPC_B64,
	S_AND_SAVEEXEC_B32,
	S_ANDN1_SAVEEXEC_B32,
	S_AND_SAVEEXEC_B64,
	S_ORN2_SAVEEXEC_B64,
	S_ANDN1_SAVEEXEC_B64,
	S_NOT_B32,
	S_NOT_B64,
	S_WQM_B64,
	S_ADD_U32,
	S_ADDC_U32,
	S_SUBB_U32,
	S_SUB_U32,
	S_ADD_I32,
	S_SUB_I32,
	S_BITCMP0_B32,
	S_BITCMP1_B32,
	S_BITSET0_B32,
	S_BITSET1_B32,
	S_MIN_I32,
	S_MAX_I32,
	S_MIN_U32,
	S_MAX_U32,
	S_AND_B32,
	S_AND_B64,
	S_ANDN2_B32,
	S_ANDN2_B64,
	S_OR_B32,
	S_OR_B64,
	S_ORN2_B32,
	S_ORN2_B64,
	S_XOR_B32,
	S_XOR_B64,
	S_NAND_B32,
	S_NAND_B64,
	S_NOR_B32,
	S_NOR_B64,
	S_XNOR_B32,
	S_XNOR_B64,
	S_LSHL_B32,
	S_LSHL_B64,
	S_LSHL1_ADD_U32,
	S_LSHL2_ADD_U32,
	S_LSHL3_ADD_U32,
	S_LSHL4_ADD_U32,
	S_LSHR_B32,
	S_LSHR_B64,
	S_ASHR_I32,
	S_MUL_I32,
	S_MUL_HI_U32,
	S_MULK_I32,
	S_BFE_U32,
	S_BFE_I32,
	S_BFE_U64,
	S_BFM_B32,
	S_BFM_B64,
	S_CSELECT_B32,
	S_CSELECT_B64,
	S_PACK_LL_B32_B16,
	S_PACK_LH_B32_B16,
	S_PACK_HH_B32_B16,
	S_CMP_EQ_I32,
	S_CMP_LG_I32,
	S_CMP_GT_I32,
	S_CMP_GE_I32,
	S_CMP_LT_I32,
	S_CMP_LE_I32,
	S_CMP_EQ_U32,
	S_CMP_LG_U32,
	S_CMP_GT_U32,
	S_CMP_GE_U32,
	S_CMP_LT_U32,
	S_CMP_LE_U32,
	S_CMP_EQ_U64,
	S_CMP_LG_U64,

	V_NOP,
	V_MOV_B32,
	V_MOVRELD_B32,
	V_MOVRELS_B32,
	V_READFIRSTLANE_B32,
	V_READLANE_B32,
	V_WRITELANE_B32,
	V_PERMLANE16_B32,
	V_PERMLANEX16_B32,
	V_CUBEID_F32,
	V_CUBESC_F32,
	V_CUBETC_F32,
	V_CUBEMA_F32,
	V_CNDMASK_B32,
	V_DOT2C_F32_F16,
	V_CVT_F32_I32,
	V_CVT_F32_U32,
	V_CVT_U32_F32,
	V_CVT_I32_F32,
	V_CVT_F16_F32,
	V_CVT_F32_F16,
	V_CVT_F16_U16,
	V_CVT_U16_F16,
	V_CVT_F16_I16,
	V_CVT_I16_F16,
	V_CVT_RPI_I32_F32,
	V_CVT_FLR_I32_F32,
	V_FREXP_EXP_I32_F32,
	V_FREXP_MANT_F32,
	V_CVT_OFF_F32_I4,
	V_CVT_F32_UBYTE0,
	V_CVT_F32_UBYTE1,
	V_CVT_F32_UBYTE2,
	V_CVT_F32_UBYTE3,
	V_RCP_F32,
	V_RCP_IFLAG_F32,
	V_FRACT_F32,
	V_TRUNC_F32,
	V_CEIL_F32,
	V_RNDNE_F32,
	V_FLOOR_F32,
	V_EXP_F32,
	V_LOG_F32,
	V_RSQ_F32,
	V_SQRT_F32,
	V_RCP_F16,
	V_SQRT_F16,
	V_RSQ_F16,
	V_LOG_F16,
	V_EXP_F16,
	V_FLOOR_F16,
	V_CEIL_F16,
	V_TRUNC_F16,
	V_RNDNE_F16,
	V_SIN_F32,
	V_COS_F32,
	V_NOT_B32,
	V_BFREV_B32,
	V_FFBH_U32,
	V_FFBL_B32,
	V_ADD_F32,
	V_SUB_F32,
	V_SUBREV_F32,
	V_MUL_F32,
	V_MAC_F32,
	V_MADMK_F32,
	V_MADAK_F32,
	V_MIN_F32,
	V_MAX_F32,
	V_ADD_F16,
	V_SUB_F16,
	V_SUBREV_F16,
	V_MUL_F16,
	V_FMAC_F16,
	V_FMAMK_F16,
	V_FMAAK_F16,
	V_MIN_I32,
	V_MAX_I32,
	V_MUL_I32_I24,
	V_MUL_U32_U24,
	V_MUL_LO_U32,
	V_MUL_HI_U32,
	V_MUL_LO_I32,
	V_MUL_HI_I32,
	V_CVT_PKRTZ_F16_F32,
	V_CVT_PK_U8_F32,
	V_MAD_F32,
	V_MAD_I32_I24,
	V_MAD_U32_U24,
	V_MAD_U64_U32,
	V_FMA_F32,
	V_FMA_F16,
	V_PACK_B32_F16,
	V_BFE_U32,
	V_BFE_I32,
	V_BFI_B32,
	V_ALIGNBIT_B32,
	V_ALIGNBYTE_B32,
	V_MIN3_F32,
	V_MIN3_I32,
	V_MIN3_U32,
	V_MIN3_F16,
	V_MAX3_F32,
	V_MAX3_I32,
	V_MAX3_U32,
	V_MAX3_F16,
	V_MED3_F32,
	V_MED3_I32,
	V_MED3_U32,
	V_MED3_F16,
	V_MED3_I16,
	V_SAD_U32,
	V_ADD3_U32,
	V_LSHL_ADD_U32,
	V_ADD_LSHL_U32,
	V_XAD_U32,
	V_LSHL_OR_B32,
	V_AND_OR_B32,
	V_OR3_B32,
	V_XOR3_B32,
	V_ADD_I32,
	V_SUB_I32,
	V_SUBREV_I32,
	V_BFM_B32,
	V_LDEXP_F32,
	V_CVT_PKNORM_I16_F32,
	V_CVT_PKNORM_U16_F32,
	V_CVT_PK_U16_U32,
	V_CVT_PK_I16_I32,
	V_PK_MAD_I16,
	V_PK_MUL_LO_U16,
	V_PK_ADD_I16,
	V_PK_SUB_I16,
	V_PK_LSHLREV_B16,
	V_PK_LSHRREV_B16,
	V_PK_ASHRREV_I16,
	V_PK_MAX_I16,
	V_PK_MIN_I16,
	V_PK_MAD_U16,
	V_PK_ADD_F16,
	V_PK_MUL_F16,
	V_PK_MIN_F16,
	V_PK_MAX_F16,
	V_PK_FMA_F16,
	V_PK_FMAC_F16,
	V_PK_ADD_U16,
	V_PK_SUB_U16,
	V_PK_MAX_U16,
	V_PK_MIN_U16,
	V_MAD_MIXLO_F16,
	V_MAD_MIXHI_F16,
	V_ADD_NC_U32,
	V_ADDC_U32,
	V_SUBREV_CO_CI_U32,
	V_SUB_NC_U32,
	V_SUBREV_NC_U32,
	V_ADD_NC_U16,
	V_SUB_NC_U16,
	V_MAX_U16,
	V_MAX_I16,
	V_MIN_U16,
	V_MIN_I16,
	V_ADD_NC_I16,
	V_SUB_NC_I16,
	V_AND_B32,
	V_OR_B32,
	V_XOR_B32,
	V_XNOR_B32,
	V_LSHL_B32,
	V_LSHLREV_B32,
	V_LSHR_B32,
	V_LSHRREV_B32,
	V_ASHR_I32,
	V_ASHRREV_I32,
	V_LSHLREV_B64,
	V_LSHRREV_B64,
	V_LSHLREV_B16,
	V_LSHRREV_B16,
	V_ASHRREV_I16,
	V_BCNT_U32_B32,
	V_MBCNT_LO_U32_B32,
	V_MBCNT_HI_U32_B32,
	V_MIN_U32,
	V_MAX_U32,
	V_MAX_F16,
	V_MIN_F16,
	V_CMP_F_F32,
	V_CMP_LT_F32,
	V_CMP_EQ_F32,
	V_CMP_LE_F32,
	V_CMP_GT_F32,
	V_CMP_LG_F32,
	V_CMP_GE_F32,
	V_CMP_O_F32,
	V_CMP_U_F32,
	V_CMP_NGE_F32,
	V_CMP_NLG_F32,
	V_CMP_NGT_F32,
	V_CMP_NLE_F32,
	V_CMP_NEQ_F32,
	V_CMP_NLT_F32,
	V_CMP_TRU_F32,
	V_CMPX_LT_F32,
	V_CMPX_EQ_F32,
	V_CMPX_LE_F32,
	V_CMPX_GT_F32,
	V_CMPX_LG_F32,
	V_CMPX_GE_F32,
	V_CMPX_NGE_F32,
	V_CMPX_NLG_F32,
	V_CMPX_NGT_F32,
	V_CMPX_NLE_F32,
	V_CMPX_NEQ_F32,
	V_CMPX_NLT_F32,
	V_CMP_F_I32,
	V_CMP_LT_I32,
	V_CMP_EQ_I32,
	V_CMP_LE_I32,
	V_CMP_GT_I32,
	V_CMP_NE_I32,
	V_CMP_GE_I32,
	V_CMP_T_I32,
	V_CMP_CLASS_F32,
	V_CMP_LT_I16,
	V_CMP_EQ_I16,
	V_CMP_LE_I16,
	V_CMP_GT_I16,
	V_CMP_NE_I16,
	V_CMP_GE_I16,
	V_CMP_LT_F16,
	V_CMP_EQ_F16,
	V_CMP_LE_F16,
	V_CMP_GT_F16,
	V_CMP_LG_F16,
	V_CMP_GE_F16,
	V_CMP_NEQ_F16,
	V_CMPX_LT_F16,
	V_CMPX_EQ_F16,
	V_CMPX_LE_F16,
	V_CMPX_GT_F16,
	V_CMPX_GE_F16,
	V_CMPX_NEQ_F16,
	V_CMPX_NLT_F16,
	V_CMPX_LT_I32,
	V_CMPX_EQ_I32,
	V_CMPX_LE_I32,
	V_CMPX_GT_I32,
	V_CMPX_NE_I32,
	V_CMPX_GE_I32,
	V_CMP_LT_U16,
	V_CMP_EQ_U16,
	V_CMP_LE_U16,
	V_CMP_GT_U16,
	V_CMP_NE_U16,
	V_CMP_GE_U16,
	V_CMP_F_U32,
	V_CMP_LT_U32,
	V_CMP_EQ_U32,
	V_CMP_LE_U32,
	V_CMP_GT_U32,
	V_CMP_NE_U32,
	V_CMP_GE_U32,
	V_CMP_T_U32,
	V_CMP_EQ_I64,
	V_CMP_GT_U64,
	V_CMP_NE_U64,
	V_CMPX_NE_I64,
	V_CMPX_NE_U64,
	V_CMPX_LT_U32,
	V_CMPX_EQ_U32,
	V_CMPX_LE_U32,
	V_CMPX_GT_U32,
	V_CMPX_NE_U32,
	V_CMPX_GE_U32,

	S_LOAD_DWORD,
	S_LOAD_DWORDX2,
	S_LOAD_DWORDX4,
	S_LOAD_DWORDX8,
	S_LOAD_DWORDX16,
	S_BUFFER_LOAD_DWORD,
	S_BUFFER_LOAD_DWORDX2,
	S_BUFFER_LOAD_DWORDX4,
	S_BUFFER_LOAD_DWORDX8,
	S_BUFFER_LOAD_DWORDX16,
	BUFFER_LOAD_FORMAT_X,
	BUFFER_LOAD_FORMAT_XY,
	BUFFER_LOAD_FORMAT_XYZ,
	BUFFER_LOAD_FORMAT_XYZW,
	BUFFER_STORE_FORMAT_X,
	BUFFER_STORE_FORMAT_XY,
	BUFFER_STORE_FORMAT_XYZ,
	BUFFER_STORE_FORMAT_XYZW,
	BUFFER_LOAD_UBYTE,
	BUFFER_LOAD_SBYTE,
	BUFFER_LOAD_USHORT,
	BUFFER_LOAD_SSHORT,
	BUFFER_LOAD_DWORD,
	BUFFER_LOAD_DWORDX2,
	BUFFER_LOAD_DWORDX3,
	BUFFER_LOAD_DWORDX4,
	BUFFER_STORE_BYTE,
	BUFFER_STORE_SHORT,
	BUFFER_STORE_DWORD,
	BUFFER_STORE_DWORDX2,
	BUFFER_STORE_DWORDX3,
	BUFFER_STORE_DWORDX4,
	TBUFFER_LOAD_FORMAT_X,
	TBUFFER_LOAD_FORMAT_XY,
	TBUFFER_LOAD_FORMAT_XYZ,
	TBUFFER_LOAD_FORMAT_XYZW,
	TBUFFER_STORE_FORMAT_X,
	TBUFFER_STORE_FORMAT_XY,
	TBUFFER_STORE_FORMAT_XYZ,
	TBUFFER_STORE_FORMAT_XYZW,
	BUFFER_ATOMIC_SWAP,
	BUFFER_ATOMIC_ADD,
	BUFFER_ATOMIC_SUB,
	BUFFER_ATOMIC_SMIN,
	BUFFER_ATOMIC_UMIN,
	BUFFER_ATOMIC_SMAX,
	BUFFER_ATOMIC_UMAX,
	BUFFER_ATOMIC_AND,
	BUFFER_ATOMIC_OR,
	BUFFER_ATOMIC_XOR,
	BUFFER_ATOMIC_FMIN,
	BUFFER_ATOMIC_FMAX,
	FLAT_LOAD_UBYTE,
	FLAT_LOAD_SBYTE,
	FLAT_LOAD_USHORT,
	FLAT_LOAD_SSHORT,
	FLAT_LOAD_DWORD,
	FLAT_LOAD_DWORDX2,
	FLAT_LOAD_DWORDX3,
	FLAT_LOAD_DWORDX4,
	FLAT_STORE_BYTE,
	FLAT_STORE_SHORT,
	FLAT_STORE_DWORD,
	FLAT_STORE_DWORDX2,
	FLAT_STORE_DWORDX3,
	FLAT_STORE_DWORDX4,
	DS_ADD_U32,
	DS_ADD_RTN_U32,
	DS_SUB_U32,
	DS_SUB_RTN_U32,
	DS_MIN_I32,
	DS_MIN_RTN_I32,
	DS_MAX_I32,
	DS_MAX_RTN_I32,
	DS_MIN_U32,
	DS_MIN_RTN_U32,
	DS_MAX_U32,
	DS_MAX_RTN_U32,
	DS_AND_B32,
	DS_AND_RTN_B32,
	DS_OR_B32,
	DS_OR_RTN_B32,
	DS_XOR_B32,
	DS_XOR_RTN_B32,
	DS_WRXCHG_RTN_B32,
	DS_MIN_F32,
	DS_MAX_F32,
	DS_SWIZZLE_B32,
	DS_CONSUME,
	DS_APPEND,
	DS_READ_I8,
	DS_READ_U8,
	DS_READ_I16,
	DS_READ_U16,
	DS_READ_U16_D16,
	DS_READ2_B32,
	DS_READ2ST64_B32,
	DS_READ_B32,
	DS_READ_B64,
	DS_READ2_B64,
	DS_READ2ST64_B64,
	DS_READ_B96,
	DS_READ_B128,
	DS_WRITE_B8,
	DS_WRITE_B16,
	DS_WRITE2_B32,
	DS_WRITE2ST64_B32,
	DS_WRITE2_B64,
	DS_WRITE2ST64_B64,
	DS_WRITE_B32,
	DS_WRITE_B64,
	DS_WRITE_B96,
	DS_WRITE_B128,
	DS_WRITE_ADDTID_B32,
	DS_READ_ADDTID_B32,
	IMAGE_GET_RESINFO,
	IMAGE_GET_LOD,
	IMAGE_LOAD,
	IMAGE_LOAD_MIP,
	IMAGE_STORE,
	IMAGE_STORE_MIP,
	IMAGE_ATOMIC_ADD,
	IMAGE_ATOMIC_UMIN,
	IMAGE_ATOMIC_UMAX,
	IMAGE_ATOMIC_AND,
	IMAGE_ATOMIC_OR,
	IMAGE_ATOMIC_XOR,
	IMAGE_SAMPLE,
	IMAGE_GATHER4_LZ,
	IMAGE_GATHER4_C,
	IMAGE_GATHER4_C_LZ,
	IMAGE_GATHER4_LZ_O,
	IMAGE_GATHER4_C_O,
	IMAGE_GATHER4_C_LZ_O,
	IMAGE_GATHER4H,
	V_INTERP_P1_F32,
	V_INTERP_P2_F32,
	V_INTERP_MOV_F32,

	S_NOP,
	S_WAITCNT,
	S_WAITCNT_DEPCTR,
	S_BARRIER,
	S_BRANCH,
	S_CBRANCH_SCC0,
	S_CBRANCH_SCC1,
	S_CBRANCH_VCCZ,
	S_CBRANCH_VCCNZ,
	S_CBRANCH_EXECZ,
	S_CBRANCH_EXECNZ,
	S_SENDMSG,
	S_SETREG_B32,
	S_SLEEP,
	S_TRAP,
	S_TTRACEDATA,
	S_INST_PREFETCH,
	S_ENDPGM,
	EXP,
	COUNT
};

enum class OperandKind {
	Unknown,
	LiteralConstant,
	IntegerInlineConstant,
	FloatInlineConstant,
	Sgpr,
	VccLo,
	VccHi,
	VccZ,
	ExecLo,
	ExecHi,
	ExecZ,
	Scc,
	M0,
	PopsExitingWaveId,
	Null,
	Vgpr,
};

enum ImageSampleFlag : uint32_t {
	ImageSampleFlagLod              = 1u << 0u,
	ImageSampleFlagBias             = 1u << 1u,
	ImageSampleFlagDerivative       = 1u << 2u,
	ImageSampleFlagCompare          = 1u << 3u,
	ImageSampleFlagOffset           = 1u << 4u,
	ImageSampleFlagLevelZero        = 1u << 5u,
	ImageSampleFlagLodClamp         = 1u << 6u,
	ImageSampleFlagA16              = 1u << 7u,
	ImageSampleFlagCd               = 1u << 8u,
	ImageSampleFlagGatherHorizontal = 1u << 9u,
	ImageSampleFlagAdjust           = 1u << 10u,
};

enum class ImageDimension : uint32_t {
	Unknown,
	Dim1D,
	Dim1DArray,
	Dim2D,
	Dim3D,
	Dim2DArray,
	Dim2DMsaa,
	Dim2DMsaaArray,
};

constexpr uint32_t MaxInstructionRawWords       = 5u;
constexpr uint32_t MaxImageNsaAddressComponents = 12u;

struct Operand {
	OperandKind kind       = OperandKind::Unknown;
	uint32_t    value      = 0;
	int32_t     signed_val = 0;
	float       float_val  = 0.0f;
	uint32_t    reg        = 0;
	uint32_t    sdwa_sel   = 6;
	// Native 16-bit destinations use the same selector fields internally but preserve the
	// untouched half. Real SDWA encodings overwrite this with their decoded DST_U value.
	uint32_t sdwa_dst_unused    = 2;
	uint32_t omod               = 0;
	uint32_t dpp_ctrl           = 0;
	uint32_t dpp_row_mask       = 0xf;
	uint32_t dpp_bank_mask      = 0xf;
	bool     sdwa_sext          = false;
	bool     dpp_fetch_inactive = false;
	bool     dpp_bound_ctrl     = false;
	bool     op_sel             = false;
	bool     op_sel_hi          = false;
	bool     negate             = false;
	bool     negate_hi          = false;
	bool     absolute           = false;
	bool     clamp              = false;
	bool     dpp                = false;
};

struct Instruction {
	uint32_t       pc                          = 0;
	uint32_t       word                        = 0;
	uint32_t       word_count                  = 1;
	uint32_t       raw[MaxInstructionRawWords] = {};
	uint32_t       raw_count                   = 1;
	Family         family                      = Family::Unknown;
	uint32_t       opcode_id                   = 0;
	Opcode         opcode                      = Opcode::UNKNOWN;
	Operand        dst;
	Operand        dst2;
	Operand        src0;
	Operand        src1;
	Operand        src2;
	Operand        src3;
	uint32_t       src_count                                    = 0;
	uint32_t       offset                                       = 0;
	uint32_t       secondary_offset                             = 0;
	uint32_t       dmask                                        = 0;
	uint32_t       data_components                              = 0;
	uint32_t       data_dwords                                  = 1;
	uint32_t       data_bits                                    = 32;
	uint32_t       data_format                                  = 0;
	uint32_t       number_format                                = 0;
	uint32_t       image_sample_flags                           = 0;
	ImageDimension image_dimension                              = ImageDimension::Unknown;
	uint32_t       image_address_components                     = 0;
	uint32_t       image_nsa_dwords                             = 0;
	uint32_t       image_nsa_addr[MaxImageNsaAddressComponents] = {};
	uint32_t       memory_segment                               = 0;
	bool           data_signed                                  = false;
	bool           typed                                        = false;
	bool           formatted                                    = false;
	bool           gds                                          = false;
	bool           glc                                          = false;
	bool           slc                                          = false;
	bool           idxen                                        = false;
	bool           offen                                        = false;
	bool           image_r128                                   = false;
	int32_t        branch_offset                                = 0;
	uint32_t       branch_target                                = 0;
	struct {
		uint32_t target = 0;
		uint32_t en     = 0;
		bool     done   = false;
		bool     compr  = false;
		bool     vm     = false;
	} exp;
	std::string_view unsupported_reason;
};

struct Program {
	std::span<const uint32_t> code;
	std::vector<Instruction>  instructions;
};

// Code spans are trusted to contain complete instructions, valid branch targets, and 32-bit PCs.
Family GetInstructionFamily(uint32_t word);
// The output object must be freshly initialized.
bool DecodeInstruction(std::span<const uint32_t> code, uint32_t word_index, Instruction& inst,
                       std::string* error);
bool DecodeProgram(std::span<const uint32_t> code, Program& program, std::string* error);

bool DecodeScalarSource(uint32_t code, uint32_t pc, Operand& operand, std::string* error);
bool DecodeScalarDestination(uint32_t code, uint32_t pc, Operand& operand, std::string* error);
void DecodeVectorGpr(uint32_t reg, Operand& operand);
void ReadLiteralOperands(std::span<const uint32_t> code, uint32_t word_index, Instruction& inst);
void SetRawWords(Instruction& inst, std::span<const uint32_t> code, uint32_t word_index,
                 uint32_t word_count);
void SetUnsupported(Instruction& inst, Family family, uint32_t opcode_id, const char* reason);
std::string OperandToString(const Operand& operand);
const char* ImageDimensionToString(ImageDimension dimension);
std::string InstructionToString(const Instruction& inst);
std::string ProgramToString(const Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

template <>
struct magic_enum::customize::enum_range<Libs::Graphics::ShaderRecompiler::Decoder::Opcode> {
	static constexpr int min =
	    static_cast<int>(Libs::Graphics::ShaderRecompiler::Decoder::Opcode::UNKNOWN);
	static constexpr int max =
	    static_cast<int>(Libs::Graphics::ShaderRecompiler::Decoder::Opcode::COUNT) - 1;
};

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERDECODER_H_ */
