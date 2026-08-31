#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_AGC_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_AGC_H_

#include "common/abi.h"
#include "common/common.h"
#include "kernel/eventQueue.h"

namespace Libs::Graphics {

struct Shader;
struct ShaderRegister;

struct SizeAlign {
	uint64_t m_size;
	size_t   m_align;
};

struct MemoryRange {
	void*    m_base;
	uint64_t m_size;
};

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr const char* name       = "Graphics";
	static constexpr auto        initialize = Libs::Graphics::Initialize;
	static constexpr auto        shutdown   = Libs::Graphics::Shutdown;
};

void GraphicsDbgDumpDcb(const char* type, uint32_t num_dw, const uint32_t* cmd_buffer);

namespace Gen5 {

struct CommandBuffer;
struct Label;

int KYTY_SYSV_ABI   AgcInit(uint32_t* state, uint32_t ver);
void* KYTY_SYSV_ABI AgcGetRegisterDefaults2(uint32_t ver);
void* KYTY_SYSV_ABI AgcGetRegisterDefaults2Internal(uint32_t ver);
int KYTY_SYSV_ABI   AgcCreateShader(Shader** dst, void* header, const volatile void* code);
int KYTY_SYSV_ABI   AgcUnknownGetFusedShaderSize(SizeAlign* dst, const Shader* front,
                                                 const Shader* back);
int KYTY_SYSV_ABI   AgcUnknownFuseShaderHalves(Shader* fused_result, const Shader* front,
                                               const Shader* back, void* scratch_mem);
int KYTY_SYSV_ABI   AgcUnknownNApJjpKNBl4(Shader* fused_result, const Shader* front,
                                          const Shader* back, void* scratch_mem);
int KYTY_SYSV_ABI   AgcSetCxRegIndirectPatchSetAddress(uint32_t*                      cmd,
                                                       const volatile ShaderRegister* regs);
int KYTY_SYSV_ABI   AgcSetShRegIndirectPatchSetAddress(uint32_t*                      cmd,
                                                       const volatile ShaderRegister* regs);
int KYTY_SYSV_ABI   AgcSetUcRegIndirectPatchSetAddress(uint32_t*                      cmd,
                                                       const volatile ShaderRegister* regs);
int KYTY_SYSV_ABI   AgcSetCxRegIndirectPatchSetNumRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   AgcSetShRegIndirectPatchSetNumRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   AgcSetUcRegIndirectPatchSetNumRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   AgcSetCxRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   AgcSetShRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   AgcSetUcRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   AgcCreatePrimState(ShaderRegister* cx_regs, ShaderRegister* uc_regs,
                                       const Shader* hs, const Shader* gs, uint32_t prim_type);
int KYTY_SYSV_ABI   AgcUpdatePrimState(ShaderRegister* cx_regs, ShaderRegister* uc_regs,
                                       uint32_t prim_type);
int KYTY_SYSV_ABI AgcGetGsOversubscription(ShaderRegister* regs, const Shader* gs, uint32_t budget,
                                           float factor);
int KYTY_SYSV_ABI AgcCreateInterpolantMapping(ShaderRegister* regs, const Shader* gs,
                                              const Shader* ps);
int KYTY_SYSV_ABI AgcCreateInterpolantMapping2(ShaderRegister* regs, const Shader* gs,
                                               const Shader* ps);
int KYTY_SYSV_ABI AgcGetDataPacketPayloadAddress(uint32_t** addr, uint32_t* cmd, int type);
int KYTY_SYSV_ABI AgcGetDataPacketPayloadRange(MemoryRange* range, uint32_t* cmd, int type);
int KYTY_SYSV_ABI AgcWriteDataPatchSetAddressOrOffset(uint32_t* cmd, uint64_t address_or_offset);
int KYTY_SYSV_ABI AgcUnknownJumpPatchSetTarget(uint32_t* cmd, const volatile uint32_t* target,
                                               uint32_t size_in_dwords);
int KYTY_SYSV_ABI AgcJumpPatchSetTarget(uint32_t* cmd, const volatile uint32_t* target,
                                        uint32_t size_in_dwords);
int KYTY_SYSV_ABI AgcSuspendPoint();
uint32_t* KYTY_SYSV_ABI AgcDcbContextStateOp(CommandBuffer* buf, uint32_t operation);
uint64_t KYTY_SYSV_ABI  AgcDcbContextStateOpGetSize(uint32_t operation);
void KYTY_SYSV_ABI      AgcGetIsTrinityMode(uint8_t* result);

uint32_t KYTY_SYSV_ABI AgcDriverGetDefaultOwner();
uint32_t KYTY_SYSV_ABI AgcDriverGetResourceRegistrationMaxNameLength();
uint32_t KYTY_SYSV_ABI AgcDriverInitResourceRegistration();
uint32_t KYTY_SYSV_ABI
AgcDriverQueryResourceRegistrationUserMemoryRequirements(uint64_t* size_in_bytes);
int KYTY_SYSV_ABI AgcDriverRegisterOwner();
int KYTY_SYSV_ABI AgcDriverRegisterResource();
int KYTY_SYSV_ABI AgcDriverUnregisterOwnerAndResources(uint32_t owner_handle);
int KYTY_SYSV_ABI AgcDriverUnregisterResource();
int KYTY_SYSV_ABI AgcDriverRegisterWorkloadStream(uint32_t stream_id, const void* stream);

uint32_t* KYTY_SYSV_ABI AgcCbNop(CommandBuffer* buf, uint32_t size_in_dwords);
uint32_t KYTY_SYSV_ABI  AgcCbNopGetSize(uint32_t size_in_dwords);
uint32_t* KYTY_SYSV_ABI AgcCbDispatch(CommandBuffer* buf, uint32_t thread_group_x,
                                      uint32_t thread_group_y, uint32_t thread_group_z,
                                      uint32_t modifier);
uint32_t KYTY_SYSV_ABI  AgcCbDispatchGetSize();
uint32_t* KYTY_SYSV_ABI AgcCbBranch(CommandBuffer* buf, uint8_t mode, uint8_t compare_function,
                                    const volatile uint64_t* compare_addr, uint64_t mask,
                                    uint64_t reference, uint8_t cache_policy1,
                                    const volatile uint32_t* buffer1, uint32_t size_in_dwords1,
                                    uint8_t cache_policy2, const volatile uint32_t* buffer2,
                                    uint32_t size_in_dwords2);
uint32_t* KYTY_SYSV_ABI AgcCbSetShRegisterRangeDirect(CommandBuffer* buf, uint32_t offset,
                                                      const uint32_t* values, uint32_t num_values);
uint32_t KYTY_SYSV_ABI  AgcCbSetShRegisterRangeDirectGetSize(uint32_t num_values);
uint32_t* KYTY_SYSV_ABI AgcCbSetShRegistersDirect(CommandBuffer*                 buf,
                                                  const volatile ShaderRegister* regs,
                                                  uint32_t                       num_regs);
uint32_t* KYTY_SYSV_ABI AgcCbSetUcRegistersDirect(CommandBuffer*                 buf,
                                                  const volatile ShaderRegister* regs,
                                                  uint32_t                       num_regs);
uint32_t* KYTY_SYSV_ABI AgcCbReleaseMem(CommandBuffer* buf, uint8_t action, uint16_t gcr_cntl,
                                        uint8_t dst, uint8_t cache_policy,
                                        const volatile Label* address, uint8_t data_sel,
                                        uint64_t data, uint16_t gds_offset, uint16_t gds_size,
                                        uint8_t interrupt, uint32_t interrupt_ctx_id);
uint32_t KYTY_SYSV_ABI  AgcCbQueueEndOfPipeActionGetSize();
int KYTY_SYSV_ABI       AgcDebugRaiseException(uint32_t exception_id);
uint32_t* KYTY_SYSV_ABI AgcAcbResetQueue(CommandBuffer* buf, uint32_t op);
uint32_t* KYTY_SYSV_ABI AgcDcbResetQueue(CommandBuffer* buf, uint32_t op, uint32_t state);
uint32_t* KYTY_SYSV_ABI AgcDcbWaitUntilSafeForRendering(CommandBuffer* buf,
                                                        uint32_t       video_out_handle,
                                                        uint32_t       display_buffer_index);
uint32_t* KYTY_SYSV_ABI AgcDcbSetWorkloadsActive(CommandBuffer* buf, uint32_t stream_id,
                                                 const uint32_t* workload_ids,
                                                 uint32_t        workload_count);
uint32_t* KYTY_SYSV_ABI AgcDcbSetWorkloadComplete(CommandBuffer* buf, uint32_t stream_id,
                                                  uint32_t workload_id);
uint32_t* KYTY_SYSV_ABI AgcDcbSetShRegisterDirect(CommandBuffer* buf, ShaderRegister reg);
uint32_t* KYTY_SYSV_ABI AgcDcbSetCxRegisterDirect(CommandBuffer* buf, ShaderRegister reg);
uint32_t KYTY_SYSV_ABI  AgcDcbSetCxRegisterDirectGetSize();
uint32_t* KYTY_SYSV_ABI AgcDcbSetUcRegisterDirect(CommandBuffer* buf, ShaderRegister reg);
uint32_t* KYTY_SYSV_ABI AgcDcbSetCxRegistersIndirect(CommandBuffer*                 buf,
                                                     const volatile ShaderRegister* regs,
                                                     uint32_t                       num_regs);
uint32_t* KYTY_SYSV_ABI AgcDcbSetShRegistersIndirect(CommandBuffer*                 buf,
                                                     const volatile ShaderRegister* regs,
                                                     uint32_t                       num_regs);
uint32_t* KYTY_SYSV_ABI AgcDcbSetUcRegistersIndirect(CommandBuffer*                 buf,
                                                     const volatile ShaderRegister* regs,
                                                     uint32_t                       num_regs);
uint32_t* KYTY_SYSV_ABI AgcDcbSetIndexSize(CommandBuffer* buf, uint8_t index_size,
                                           uint8_t cache_policy);
uint32_t* KYTY_SYSV_ABI AgcDcbSetIndexBuffer(CommandBuffer* buf, uint64_t index_addr);
uint32_t* KYTY_SYSV_ABI AgcDcbSetIndexCount(CommandBuffer* buf, uint32_t index_count);
uint32_t* KYTY_SYSV_ABI AgcDcbSetNumInstances(CommandBuffer* buf, uint32_t num_instances);
uint32_t KYTY_SYSV_ABI  AgcDcbSetNumInstancesGetSize();
uint32_t* KYTY_SYSV_ABI AgcDcbDrawIndex(CommandBuffer* buf, uint32_t index_count,
                                        const volatile void* index_addr, uint64_t modifier);
uint32_t KYTY_SYSV_ABI  AgcDcbDrawIndexGetSize();

uint32_t* KYTY_SYSV_ABI AgcDcbDrawIndexMultiInstanced(CommandBuffer* buf, uint32_t index_count,
                                                      const volatile void* index_addr,
                                                      const volatile void* object_ids,
                                                      uint32_t instance_count, uint64_t modifier);
uint32_t KYTY_SYSV_ABI  AgcDcbDrawIndexMultiInstancedGetSize();
int KYTY_SYSV_ABI AgcUnknownIkfdtRIqCE(uint32_t* cmd, uint64_t arg1,
                                       const volatile uint32_t* target, uint32_t size_in_dwords,
                                       uint64_t arg4, uint64_t arg5);
uint32_t* KYTY_SYSV_ABI AgcDcbDrawIndexAuto(CommandBuffer* buf, uint32_t index_count,
                                            uint64_t modifier);
uint32_t KYTY_SYSV_ABI  AgcDcbDrawIndexAutoGetSize();
uint32_t* KYTY_SYSV_ABI AgcDcbDrawIndexOffset(CommandBuffer* buf, uint32_t index_offset,
                                              uint32_t index_count, uint64_t modifier);
uint32_t KYTY_SYSV_ABI  AgcDcbDrawIndexOffsetGetSize();
uint32_t* KYTY_SYSV_ABI AgcDcbSetBaseIndirectArgs(CommandBuffer* buf, uint32_t shader_type,
                                                  const volatile void* indirect_base_addr);
uint32_t* KYTY_SYSV_ABI AgcDcbDrawIndirect(CommandBuffer* buf, uint32_t data_offset_in_bytes,
                                           uint64_t modifier);
uint32_t KYTY_SYSV_ABI  AgcDcbDrawIndirectGetSize();
uint32_t* KYTY_SYSV_ABI AgcDcbDrawIndirectMulti(CommandBuffer*       buf,
                                                uint32_t             data_offset_in_bytes,
                                                uint32_t             count_indirect,
                                                uint32_t             max_count_or_count,
                                                const volatile void* count_addr,
                                                uint32_t stride_in_bytes, uint64_t modifier);
uint32_t* KYTY_SYSV_ABI AgcDcbDrawIndexIndirect(CommandBuffer* buf, uint32_t data_offset_in_bytes,
                                                uint64_t modifier);
uint32_t* KYTY_SYSV_ABI AgcDcbDrawIndexIndirectMulti(CommandBuffer*       buf,
                                                     uint32_t             data_offset_in_bytes,
                                                     uint32_t             count_indirect,
                                                     uint32_t             max_count_or_count,
                                                     const volatile void* count_addr,
                                                     uint32_t stride_in_bytes, uint64_t modifier);
uint32_t* KYTY_SYSV_ABI AgcDcbDispatchIndirect(CommandBuffer* buf, uint32_t data_offset_in_bytes,
                                               uint32_t flags);
uint32_t KYTY_SYSV_ABI  AgcDcbDispatchIndirectGetSize();
uint32_t* KYTY_SYSV_ABI AgcDcbEventWrite(CommandBuffer* buf, uint8_t event_type,
                                         const volatile void* address);
uint64_t KYTY_SYSV_ABI AgcDcbEventWriteGetSize(uint8_t event_type);
uint32_t* KYTY_SYSV_ABI AgcDcbAcquireMem(CommandBuffer* buf, uint8_t engine, uint32_t cb_db_op,
                                         uint32_t gcr_cntl, const volatile void* base,
                                         uint64_t size_bytes, uint32_t poll_cycles);
uint32_t KYTY_SYSV_ABI  AgcDcbAcquireMemGetSize();
uint32_t* KYTY_SYSV_ABI AgcAcbEventWrite(CommandBuffer* buf, uint8_t event_type,
                                         const volatile void* address);
uint32_t* KYTY_SYSV_ABI AgcAcbAcquireMem(CommandBuffer* buf, uint32_t gcr_cntl,
                                         const volatile void* base, uint64_t size_bytes,
                                         uint32_t poll_cycles);
uint32_t KYTY_SYSV_ABI  AgcAcbAcquireMemGetSize();
uint32_t* KYTY_SYSV_ABI AgcAcbCondExec(CommandBuffer* buf, const volatile uint32_t* address,
                                       uint32_t num_dwords);
uint32_t KYTY_SYSV_ABI  AgcAcbCondExecGetSize();
uint32_t* KYTY_SYSV_ABI AgcAcbJump(CommandBuffer* buf, uint8_t cache_policy,
                                   const uint32_t* target, uint32_t size_in_dwords);
uint32_t KYTY_SYSV_ABI  AgcAcbJumpGetSize();
uint32_t* KYTY_SYSV_ABI AgcAcbWaitRegMem(CommandBuffer* buf, uint8_t size, uint8_t compare_function,
                                         uint8_t cache_policy, const volatile void* address,
                                         uint64_t reference, uint64_t mask, uint32_t poll_cycles);
uint64_t KYTY_SYSV_ABI  AgcAcbWaitOnAddressGetSize(uint8_t size);
uint32_t* KYTY_SYSV_ABI AgcAcbDmaData(CommandBuffer* buf, uint8_t dst, uint8_t dst_cache_policy,
                                      uint64_t dst_address_or_offset, uint8_t src,
                                      uint8_t  src_cache_policy,
                                      uint64_t src_address_or_offset_or_immediate,
                                      uint32_t num_bytes, uint8_t wait_for_previous,
                                      uint8_t write_confirm);
uint32_t* KYTY_SYSV_ABI AgcAcbCopyData(CommandBuffer* buf, uint8_t dst, uint8_t dst_cache_policy,
                                       uint64_t dst_address, uint8_t src, uint8_t src_cache_policy,
                                       uint64_t src_address_or_immediate, uint8_t item_size,
                                       uint8_t write_confirm);
uint64_t KYTY_SYSV_ABI  AgcAcbCopyDataGetSize();
uint32_t* KYTY_SYSV_ABI AgcAcbDispatchIndirect(CommandBuffer*       buf,
                                               const volatile void* indirect_args,
                                               uint32_t             modifier);
uint32_t* KYTY_SYSV_ABI AgcAcbWriteData(CommandBuffer* buf, uint8_t dst, uint8_t cache_policy,
                                        uint64_t address_or_offset, const void* data,
                                        uint32_t num_dwords, uint8_t increment,
                                        uint8_t write_confirm);
uint32_t* KYTY_SYSV_ABI AgcAcbSetMarker(CommandBuffer* buf, const char* str, uint32_t color);
uint32_t* KYTY_SYSV_ABI AgcAcbPushMarker(CommandBuffer* buf, const char* str, uint32_t color);
uint32_t* KYTY_SYSV_ABI AgcAcbPopMarker(CommandBuffer* buf);
uint32_t* KYTY_SYSV_ABI AgcDcbStallCommandBufferParser(CommandBuffer* buf);
uint32_t* KYTY_SYSV_ABI AgcDcbCopyData(CommandBuffer* buf, uint8_t dst, uint8_t dst_cache_policy,
                                       uint64_t dst_address, uint8_t src, uint8_t src_cache_policy,
                                       uint64_t src_address_or_immediate, uint8_t item_size,
                                       uint8_t write_confirm);
uint64_t KYTY_SYSV_ABI  AgcDcbCopyDataGetSize();
uint32_t* KYTY_SYSV_ABI AgcDcbDmaData(CommandBuffer* buf, uint8_t engine, uint8_t dst,
                                      uint8_t dst_cache_policy, uint64_t dst_address_or_offset,
                                      uint8_t src, uint8_t src_cache_policy,
                                      uint64_t src_address_or_offset_or_immediate,
                                      uint32_t num_bytes, uint8_t wait_for_previous,
                                      uint8_t write_confirm, uint8_t block_engine);
uint32_t* KYTY_SYSV_ABI AgcDcbJump(CommandBuffer* buf, uint8_t mode, uint8_t cache_policy,
                                   const uint32_t* target, uint32_t size_in_dwords);
uint32_t KYTY_SYSV_ABI  AgcDcbJumpGetSize();
uint32_t KYTY_SYSV_ABI  AgcDcbRewindGetSize();
uint32_t* KYTY_SYSV_ABI AgcDcbRewind(CommandBuffer* buf, uint32_t initial_state);
uint32_t* KYTY_SYSV_ABI AgcDcbCondExec(CommandBuffer* buf, const volatile uint32_t* address,
                                       uint32_t num_dwords);
uint32_t KYTY_SYSV_ABI  AgcDcbCondExecGetSize();
uint32_t* KYTY_SYSV_ABI AgcDcbSetPredication(CommandBuffer* buf, uint8_t condition, uint8_t op,
                                             uint8_t wait_op, const volatile void* address,
                                             uint32_t count_in_dwords);
uint32_t* KYTY_SYSV_ABI AgcUnknownKRzWekV120(CommandBuffer* buf, uint32_t arg1, uint32_t arg2,
                                             uint32_t arg3);
int KYTY_SYSV_ABI       AgcDmaDataPatchSetDstAddressOrOffset(uint32_t* cmd,
                                                             uint64_t  dst_address_or_offset);
int KYTY_SYSV_ABI       AgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate(
    uint32_t* cmd, uint64_t src_address_or_offset_or_immediate);
uint32_t KYTY_SYSV_ABI  AgcGetPacketSize(uint32_t* packet);
int KYTY_SYSV_ABI       AgcSetPacketPredication(uint32_t* packet, uint32_t predication);
int KYTY_SYSV_ABI       AgcSetRangePredication(uint32_t* start, const volatile uint32_t* end,
                                               uint32_t predication);
int KYTY_SYSV_ABI       AgcRewindPatchSetRewindState(uint32_t* cmd, uint8_t state);
int KYTY_SYSV_ABI       AgcCondExecPatchSetEnd(uint32_t* cmd, const volatile uint32_t* buffer);
int KYTY_SYSV_ABI       AgcCondExecPatchSetCommandAddress(uint32_t*                cmd,
                                                          const volatile uint32_t* command);
uint32_t* KYTY_SYSV_ABI AgcDcbWriteData(CommandBuffer* buf, uint8_t dst, uint8_t cache_policy,
                                        uint64_t address_or_offset, const void* data,
                                        uint32_t num_dwords, uint8_t increment,
                                        uint8_t write_confirm);
uint32_t KYTY_SYSV_ABI  AgcDcbWriteDataGetSize(uint32_t num_dwords);
uint32_t* KYTY_SYSV_ABI AgcDcbGetLodStats(CommandBuffer* buf, uint8_t cache_policy,
                                          const volatile void* buffer,
                                          uint32_t buffer_size_in_bytes, uint32_t reset_count,
                                          uint8_t force_reset, uint8_t report_and_reset,
                                          uint32_t reporting_interval_in_100k_clocks);
uint32_t* KYTY_SYSV_ABI AgcDcbWaitRegMem(CommandBuffer* buf, uint8_t size, uint8_t compare_function,
                                         uint8_t op, uint8_t cache_policy,
                                         const volatile void* address, uint64_t reference,
                                         uint64_t mask, uint32_t poll_cycles);
uint64_t KYTY_SYSV_ABI  AgcDcbWaitOnAddressGetSize(uint32_t size);
bool                    AgcIsInternalDataPacket(uint32_t cmd_id, const uint32_t* payload);
uint32_t* KYTY_SYSV_ABI AgcDcbSetMarker(CommandBuffer* buf, const char* str, uint32_t color);
uint32_t* KYTY_SYSV_ABI AgcDcbPushMarker(CommandBuffer* buf, const char* str, uint32_t color);
uint32_t* KYTY_SYSV_ABI AgcDcbPopMarker(CommandBuffer* buf);

int KYTY_SYSV_ABI AgcWaitRegMemPatchAddress(uint32_t* cmd, const volatile void* address);
int KYTY_SYSV_ABI AgcWaitRegMemPatchReference(uint32_t* cmd, uint64_t reference);
int KYTY_SYSV_ABI AgcQueueEndOfPipeActionPatchAddress(uint32_t* cmd, const volatile Label* address);
int KYTY_SYSV_ABI AgcQueueEndOfPipeActionPatchData(uint32_t* cmd, uint64_t data);

uint32_t* KYTY_SYSV_ABI AgcDcbSetFlip(CommandBuffer* buf, uint32_t video_out_handle,
                                      int32_t display_buffer_index, uint32_t flip_mode,
                                      int64_t flip_arg);

} // namespace Gen5

namespace Gen5Driver {

struct Packet {
	uint32_t* addr;
	uint32_t  dw_num;
	uint8_t   flags;
	uint8_t   reserved[3];
};

static_assert(sizeof(Packet) == 0x10);
static_assert(offsetof(Packet, addr) == 0x0);
static_assert(offsetof(Packet, dw_num) == 0x8);
static_assert(offsetof(Packet, flags) == 0xc);

int KYTY_SYSV_ABI AgcDriverSubmitDcb(const Packet* packet);
int KYTY_SYSV_ABI AgcDriverSubmitMultiDcbs(uint32_t* const* dcb_gpu_addrs,
                                           const uint32_t* dcb_sizes_in_dwords, uint32_t count);
int KYTY_SYSV_ABI AgcDriverSubmitCommandBuffer(void* queue_context, const Packet* packet);
int KYTY_SYSV_ABI AgcDriverSubmitMultiCommandBuffers(void*            queue_context,
                                                     uint32_t* const* command_buffers,
                                                     const uint32_t*  sizes_in_dwords,
                                                     uint32_t         count);
int KYTY_SYSV_ABI AgcDriverSubmitAcb(uint32_t queue, const Packet* packet);
int KYTY_SYSV_ABI AgcDriverSubmitMultiAcbs(uint32_t queue, uint32_t* const* acbs,
                                           const uint32_t* sizes_in_dwords, uint32_t count);
int KYTY_SYSV_ABI AgcDriverAddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, void* udata);
int KYTY_SYSV_ABI AgcDriverDeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id);
int KYTY_SYSV_ABI AgcDriverGetEqEventType(const LibKernel::EventQueue::KernelEvent* ev);
uint32_t KYTY_SYSV_ABI AgcDriverGetEqContextId(const LibKernel::EventQueue::KernelEvent* ev);
int KYTY_SYSV_ABI      AgcDriverSetTFRing(const volatile void* base, uint32_t size);
int KYTY_SYSV_ABI  AgcDriverSetHsOffchipParam(uint64_t value0, uint64_t value1, uint64_t value2);
bool KYTY_SYSV_ABI AgcDriverIsCaptureInProgress();
int KYTY_SYSV_ABI  AgcDriverUnknownU9ueyEhSkF4();

} // namespace Gen5Driver

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_AGC_H_ */
