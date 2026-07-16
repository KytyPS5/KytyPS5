#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_AGC_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_AGC_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/subsystems.h"
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

KYTY_SUBSYSTEM_DEFINE(Graphics);

void GraphicsDbgDumpDcb(const char* type, uint32_t num_dw, uint32_t* cmd_buffer);

namespace Gen5 {

struct CommandBuffer;
struct Label;

int KYTY_SYSV_ABI   GraphicsInit(uint32_t* state, uint32_t ver);
void* KYTY_SYSV_ABI GraphicsGetRegisterDefaults2(uint32_t ver);
void* KYTY_SYSV_ABI GraphicsGetRegisterDefaults2Internal(uint32_t ver);
int KYTY_SYSV_ABI   GraphicsCreateShader(Shader** dst, void* header, const volatile void* code);
int KYTY_SYSV_ABI   GraphicsUnknownGetFusedShaderSize(SizeAlign* dst, const Shader* front,
                                                      const Shader* back);
int KYTY_SYSV_ABI   GraphicsUnknownFuseShaderHalves(Shader* fused_result, const Shader* front,
                                                    const Shader* back, void* scratch_mem);
int KYTY_SYSV_ABI   GraphicsSetCxRegIndirectPatchSetAddress(uint32_t*                      cmd,
                                                            const volatile ShaderRegister* regs);
int KYTY_SYSV_ABI   GraphicsSetShRegIndirectPatchSetAddress(uint32_t*                      cmd,
                                                            const volatile ShaderRegister* regs);
int KYTY_SYSV_ABI   GraphicsSetUcRegIndirectPatchSetAddress(uint32_t*                      cmd,
                                                            const volatile ShaderRegister* regs);
int KYTY_SYSV_ABI   GraphicsSetCxRegIndirectPatchSetNumRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   GraphicsSetShRegIndirectPatchSetNumRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   GraphicsSetUcRegIndirectPatchSetNumRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   GraphicsSetCxRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   GraphicsSetShRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   GraphicsSetUcRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs);
int KYTY_SYSV_ABI   GraphicsCreatePrimState(ShaderRegister* cx_regs, ShaderRegister* uc_regs,
                                            const Shader* hs, const Shader* gs, uint32_t prim_type);
int KYTY_SYSV_ABI   GraphicsUpdatePrimState(ShaderRegister* cx_regs, ShaderRegister* uc_regs,
                                            uint32_t prim_type);
int KYTY_SYSV_ABI   GraphicsCreateInterpolantMapping(ShaderRegister* regs, const Shader* gs,
                                                     const Shader* ps);
int KYTY_SYSV_ABI   GraphicsGetDataPacketPayloadAddress(uint32_t** addr, uint32_t* cmd, int type);
int KYTY_SYSV_ABI   GraphicsGetDataPacketPayloadRange(MemoryRange* range, uint32_t* cmd, int type);
int KYTY_SYSV_ABI   GraphicsWriteDataPatchSetAddressOrOffset(uint32_t* cmd,
                                                             uint64_t  address_or_offset);
int KYTY_SYSV_ABI GraphicsUnknownJumpPatchSetTarget(uint32_t* cmd, const volatile uint32_t* target,
                                                    uint32_t size_in_dwords);
int KYTY_SYSV_ABI GraphicsJumpPatchSetTarget(uint32_t* cmd, const volatile uint32_t* target,
                                             uint32_t size_in_dwords);
int KYTY_SYSV_ABI GraphicsSuspendPoint();
uint32_t* KYTY_SYSV_ABI GraphicsUnknownQj7QZpgr9Uw(CommandBuffer* buf, uint32_t mode,
                                                   uint32_t value);
uint64_t KYTY_SYSV_ABI  GraphicsGetIsTrinityMode();

uint32_t KYTY_SYSV_ABI GraphicsDriverGetDefaultOwner();
uint32_t KYTY_SYSV_ABI GraphicsDriverGetResourceRegistrationMaxNameLength();
uint32_t KYTY_SYSV_ABI GraphicsDriverInitResourceRegistration();
uint32_t KYTY_SYSV_ABI
GraphicsDriverQueryResourceRegistrationUserMemoryRequirements(uint64_t* size_in_bytes);
int KYTY_SYSV_ABI GraphicsDriverRegisterOwner();
int KYTY_SYSV_ABI GraphicsDriverRegisterResource();
int KYTY_SYSV_ABI GraphicsDriverUnregisterResource();
int KYTY_SYSV_ABI GraphicsDriverRegisterWorkloadStream(uint32_t stream_id, const void* stream);

uint32_t* KYTY_SYSV_ABI GraphicsCbNop(CommandBuffer* buf, uint32_t size_in_dwords);
uint32_t KYTY_SYSV_ABI  GraphicsCbNopGetSize(uint32_t size_in_dwords);
uint32_t* KYTY_SYSV_ABI GraphicsCbDispatch(CommandBuffer* buf, uint32_t thread_group_x,
                                           uint32_t thread_group_y, uint32_t thread_group_z,
                                           uint32_t modifier);
uint32_t KYTY_SYSV_ABI  GraphicsCbDispatchGetSize();
uint32_t* KYTY_SYSV_ABI GraphicsCbBranch(CommandBuffer* buf, uint8_t mode, uint8_t compare_function,
                                         const volatile uint64_t* compare_addr, uint64_t mask,
                                         uint64_t reference, uint8_t cache_policy1,
                                         const volatile uint32_t* buffer1, uint32_t size_in_dwords1,
                                         uint8_t cache_policy2, const volatile uint32_t* buffer2,
                                         uint32_t size_in_dwords2);
uint32_t* KYTY_SYSV_ABI GraphicsCbSetShRegisterRangeDirect(CommandBuffer* buf, uint32_t offset,
                                                           const uint32_t* values,
                                                           uint32_t        num_values);
uint32_t KYTY_SYSV_ABI  GraphicsCbSetShRegisterRangeDirectGetSize(uint32_t num_values);
uint32_t* KYTY_SYSV_ABI GraphicsCbSetShRegistersDirect(CommandBuffer*                 buf,
                                                       const volatile ShaderRegister* regs,
                                                       uint32_t                       num_regs);
uint32_t* KYTY_SYSV_ABI GraphicsCbReleaseMem(CommandBuffer* buf, uint8_t action, uint16_t gcr_cntl,
                                             uint8_t dst, uint8_t cache_policy,
                                             const volatile Label* address, uint8_t data_sel,
                                             uint64_t data, uint16_t gds_offset, uint16_t gds_size,
                                             uint8_t interrupt, uint32_t interrupt_ctx_id);
uint32_t KYTY_SYSV_ABI  GraphicsCbQueueEndOfPipeActionGetSize();
int KYTY_SYSV_ABI       GraphicsDebugRaiseException(uint32_t exception_id);
uint32_t* KYTY_SYSV_ABI GraphicsAcbResetQueue(CommandBuffer* buf, uint32_t op);
uint32_t* KYTY_SYSV_ABI GraphicsDcbResetQueue(CommandBuffer* buf, uint32_t op, uint32_t state);
uint32_t* KYTY_SYSV_ABI GraphicsDcbWaitUntilSafeForRendering(CommandBuffer* buf,
                                                             uint32_t       video_out_handle,
                                                             uint32_t       display_buffer_index);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetWorkloadsActive(CommandBuffer* buf, uint32_t stream_id,
                                                      const uint32_t* workload_ids,
                                                      uint32_t        workload_count);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetWorkloadComplete(CommandBuffer* buf, uint32_t stream_id,
                                                       uint32_t workload_id);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetShRegisterDirect(CommandBuffer* buf, ShaderRegister reg);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetCxRegisterDirect(CommandBuffer* buf, ShaderRegister reg);
uint32_t KYTY_SYSV_ABI  GraphicsDcbSetCxRegisterDirectGetSize();
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetUcRegisterDirect(CommandBuffer* buf, ShaderRegister reg);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetCxRegistersIndirect(CommandBuffer*                 buf,
                                                          const volatile ShaderRegister* regs,
                                                          uint32_t                       num_regs);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetShRegistersIndirect(CommandBuffer*                 buf,
                                                          const volatile ShaderRegister* regs,
                                                          uint32_t                       num_regs);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetUcRegistersIndirect(CommandBuffer*                 buf,
                                                          const volatile ShaderRegister* regs,
                                                          uint32_t                       num_regs);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetIndexSize(CommandBuffer* buf, uint8_t index_size,
                                                uint8_t cache_policy);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetIndexBuffer(CommandBuffer* buf, uint64_t index_addr);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetIndexCount(CommandBuffer* buf, uint32_t index_count);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetNumInstances(CommandBuffer* buf, uint32_t num_instances);
uint32_t KYTY_SYSV_ABI  GraphicsDcbSetNumInstancesGetSize();
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndex(CommandBuffer* buf, uint32_t index_count,
                                             const volatile void* index_addr, uint64_t modifier);
uint32_t KYTY_SYSV_ABI  GraphicsDcbDrawIndexGetSize();

uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexMultiInstanced(CommandBuffer* buf, uint32_t index_count,
                                                           const volatile void* index_addr,
                                                           const volatile void* object_ids,
                                                           uint32_t             instance_count,
                                                           uint64_t             modifier);
uint32_t KYTY_SYSV_ABI  GraphicsDcbDrawIndexMultiInstancedGetSize();
int KYTY_SYSV_ABI GraphicsUnknownIkfdtRIqCE(uint32_t* cmd, uint64_t arg1,
                                            const volatile uint32_t* target,
                                            uint32_t size_in_dwords, uint64_t arg4, uint64_t arg5);
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexAuto(CommandBuffer* buf, uint32_t index_count,
                                                 uint64_t modifier);
uint32_t KYTY_SYSV_ABI  GraphicsDcbDrawIndexAutoGetSize();
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexOffset(CommandBuffer* buf, uint32_t index_offset,
                                                   uint32_t index_count, uint64_t modifier);
uint32_t KYTY_SYSV_ABI  GraphicsDcbDrawIndexOffsetGetSize();
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetBaseIndirectArgs(CommandBuffer* buf, uint32_t shader_type,
                                                       const volatile void* indirect_base_addr);
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndirect(CommandBuffer* buf, uint32_t data_offset_in_bytes,
                                                uint64_t modifier);
uint32_t KYTY_SYSV_ABI  GraphicsDcbDrawIndirectGetSize();
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexIndirect(CommandBuffer* buf,
                                                     uint32_t       data_offset_in_bytes,
                                                     uint64_t       modifier);
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexIndirectMulti(
    CommandBuffer* buf, uint32_t data_offset_in_bytes, uint32_t count_indirect,
    uint32_t max_count_or_count, const volatile void* count_addr, uint32_t stride_in_bytes,
    uint64_t modifier);
uint32_t* KYTY_SYSV_ABI GraphicsDcbDispatchIndirect(CommandBuffer* buf,
                                                    uint32_t data_offset_in_bytes, uint32_t flags);
uint32_t KYTY_SYSV_ABI  GraphicsDcbDispatchIndirectGetSize();
uint32_t* KYTY_SYSV_ABI GraphicsDcbEventWrite(CommandBuffer* buf, uint8_t event_type,
                                              const volatile void* address);
uint32_t* KYTY_SYSV_ABI GraphicsDcbAcquireMem(CommandBuffer* buf, uint8_t engine, uint32_t cb_db_op,
                                              uint32_t gcr_cntl, const volatile void* base,
                                              uint64_t size_bytes, uint32_t poll_cycles);
uint32_t KYTY_SYSV_ABI  GraphicsDcbAcquireMemGetSize();
uint32_t* KYTY_SYSV_ABI GraphicsAcbEventWrite(CommandBuffer* buf, uint8_t event_type,
                                              const volatile void* address);
uint32_t* KYTY_SYSV_ABI GraphicsAcbAcquireMem(CommandBuffer* buf, uint32_t gcr_cntl,
                                              const volatile void* base, uint64_t size_bytes,
                                              uint32_t poll_cycles);
uint32_t KYTY_SYSV_ABI  GraphicsAcbAcquireMemGetSize();
uint32_t* KYTY_SYSV_ABI GraphicsAcbCondExec(CommandBuffer* buf, const volatile uint32_t* address,
                                            uint32_t num_dwords);
uint32_t KYTY_SYSV_ABI  GraphicsAcbCondExecGetSize();
uint32_t* KYTY_SYSV_ABI GraphicsAcbWaitRegMem(CommandBuffer* buf, uint8_t size,
                                              uint8_t compare_function, uint8_t cache_policy,
                                              const volatile void* address, uint64_t reference,
                                              uint64_t mask, uint32_t poll_cycles);
uint32_t* KYTY_SYSV_ABI GraphicsAcbDmaData(CommandBuffer* buf, uint8_t engine, uint8_t dst,
                                           uint8_t dst_cache_policy, uint64_t dst_address_or_offset,
                                           uint8_t src, uint8_t src_cache_policy,
                                           uint64_t src_address_or_offset_or_immediate,
                                           uint32_t num_bytes, uint8_t wait_for_previous,
                                           uint8_t write_confirm, uint8_t block_engine);
uint32_t* KYTY_SYSV_ABI GraphicsAcbCopyData(CommandBuffer* buf, uint8_t dst,
                                            uint8_t dst_cache_policy, uint64_t dst_address,
                                            uint8_t src, uint8_t src_cache_policy,
                                            uint64_t src_address_or_immediate, uint8_t item_size,
                                            uint8_t write_confirm);
uint32_t* KYTY_SYSV_ABI GraphicsAcbDispatchIndirect(CommandBuffer*       buf,
                                                    const volatile void* indirect_args,
                                                    uint32_t             modifier);
uint32_t* KYTY_SYSV_ABI GraphicsAcbWriteData(CommandBuffer* buf, uint8_t dst, uint8_t cache_policy,
                                             uint64_t address_or_offset, const void* data,
                                             uint32_t num_dwords, uint8_t increment,
                                             uint8_t write_confirm);
uint32_t* KYTY_SYSV_ABI GraphicsAcbSetMarker(CommandBuffer* buf, const char* str, uint32_t color);
uint32_t* KYTY_SYSV_ABI GraphicsAcbPushMarker(CommandBuffer* buf, const char* str, uint32_t color);
uint32_t* KYTY_SYSV_ABI GraphicsAcbPopMarker(CommandBuffer* buf);
uint32_t* KYTY_SYSV_ABI GraphicsDcbStallCommandBufferParser(CommandBuffer* buf);
uint32_t* KYTY_SYSV_ABI GraphicsDcbCopyData(CommandBuffer* buf, uint8_t dst,
                                            uint8_t dst_cache_policy, uint64_t dst_address,
                                            uint8_t src, uint8_t src_cache_policy,
                                            uint64_t src_address_or_immediate, uint8_t item_size,
                                            uint8_t write_confirm);
uint32_t* KYTY_SYSV_ABI GraphicsDcbDmaData(CommandBuffer* buf, uint8_t engine, uint8_t dst,
                                           uint8_t dst_cache_policy, uint64_t dst_address_or_offset,
                                           uint8_t src, uint8_t src_cache_policy,
                                           uint64_t src_address_or_offset_or_immediate,
                                           uint32_t num_bytes, uint8_t wait_for_previous,
                                           uint8_t write_confirm, uint8_t block_engine);
uint32_t* KYTY_SYSV_ABI GraphicsDcbJump(CommandBuffer* buf, uint8_t mode, uint8_t cache_policy,
                                        const uint32_t* target, uint32_t size_in_dwords);
uint32_t KYTY_SYSV_ABI  GraphicsDcbJumpGetSize();
uint32_t KYTY_SYSV_ABI  GraphicsDcbRewindGetSize();
uint32_t* KYTY_SYSV_ABI GraphicsDcbRewind(CommandBuffer* buf, uint32_t initial_state);
uint32_t* KYTY_SYSV_ABI GraphicsDcbCondExec(CommandBuffer* buf, const volatile uint32_t* address,
                                            uint32_t num_dwords);
uint32_t KYTY_SYSV_ABI  GraphicsDcbCondExecGetSize();
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetPredication(CommandBuffer* buf, uint8_t condition, uint8_t op,
                                                  uint8_t wait_op, const volatile void* address,
                                                  uint32_t count_in_dwords);
uint32_t* KYTY_SYSV_ABI GraphicsUnknownKRzWekV120(CommandBuffer* buf, uint32_t arg1, uint32_t arg2,
                                                  uint32_t arg3);
int KYTY_SYSV_ABI       GraphicsDmaDataPatchSetDstAddressOrOffset(uint32_t* cmd,
                                                                  uint64_t  dst_address_or_offset);
int KYTY_SYSV_ABI       GraphicsDmaDataPatchSetSrcAddressOrOffsetOrImmediate(
    uint32_t* cmd, uint64_t src_address_or_offset_or_immediate);
uint32_t KYTY_SYSV_ABI  GraphicsGetPacketSize(uint32_t* packet);
int KYTY_SYSV_ABI       GraphicsSetPacketPredication(uint32_t* packet, uint32_t predication);
int KYTY_SYSV_ABI       GraphicsSetRangePredication(uint32_t* start, const volatile uint32_t* end,
                                                    uint32_t predication);
int KYTY_SYSV_ABI       GraphicsCondExecPatchSetEnd(uint32_t* cmd, const volatile uint32_t* buffer);
int KYTY_SYSV_ABI       GraphicsCondExecPatchSetCommandAddress(uint32_t*                cmd,
                                                               const volatile uint32_t* command);
uint32_t* KYTY_SYSV_ABI GraphicsDcbWriteData(CommandBuffer* buf, uint8_t dst, uint8_t cache_policy,
                                             uint64_t address_or_offset, const void* data,
                                             uint32_t num_dwords, uint8_t increment,
                                             uint8_t write_confirm);
uint32_t KYTY_SYSV_ABI  GraphicsDcbWriteDataGetSize(uint32_t num_dwords);
uint32_t* KYTY_SYSV_ABI GraphicsDcbGetLodStats(CommandBuffer* buf, uint8_t cache_policy,
                                               const volatile void* buffer,
                                               uint32_t buffer_size_in_bytes, uint32_t reset_count,
                                               uint8_t force_reset, uint8_t report_and_reset,
                                               uint32_t reporting_interval_in_100k_clocks);
uint32_t* KYTY_SYSV_ABI GraphicsDcbWaitRegMem(CommandBuffer* buf, uint8_t size,
                                              uint8_t compare_function, uint8_t op,
                                              uint8_t cache_policy, const volatile void* address,
                                              uint64_t reference, uint64_t mask,
                                              uint32_t poll_cycles);
uint32_t KYTY_SYSV_ABI  GraphicsDcbWaitOnAddressGetSize(uint32_t size);
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetMarker(CommandBuffer* buf, const char* str, uint32_t color);
uint32_t* KYTY_SYSV_ABI GraphicsDcbPushMarker(CommandBuffer* buf, const char* str, uint32_t color);
uint32_t* KYTY_SYSV_ABI GraphicsDcbPopMarker(CommandBuffer* buf);

int KYTY_SYSV_ABI GraphicsWaitRegMemPatchAddress(uint32_t* cmd, const volatile void* address);
int KYTY_SYSV_ABI GraphicsWaitRegMemPatchReference(uint32_t* cmd, uint64_t reference);
int KYTY_SYSV_ABI GraphicsQueueEndOfPipeActionPatchAddress(uint32_t*             cmd,
                                                           const volatile Label* address);
int KYTY_SYSV_ABI GraphicsQueueEndOfPipeActionPatchData(uint32_t* cmd, uint32_t context_id,
                                                        uint32_t data_sel, uint64_t data);

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetFlip(CommandBuffer* buf, uint32_t video_out_handle,
                                           int32_t display_buffer_index, uint32_t flip_mode,
                                           int64_t flip_arg);

} // namespace Gen5

namespace Gen5Driver {

struct Packet;

int KYTY_SYSV_ABI GraphicsDriverSubmitDcb(const Packet* packet);
int KYTY_SYSV_ABI GraphicsDriverSubmitMultiDcbs(uint32_t* const* dcb_gpu_addrs,
                                                const uint32_t*  dcb_sizes_in_dwords,
                                                uint32_t         count);
int KYTY_SYSV_ABI GraphicsDriverSubmitCommandBuffer(uint32_t queue, uint32_t* dcb,
                                                    uint32_t size_in_dwords);
int KYTY_SYSV_ABI GraphicsDriverSubmitMultiCommandBuffers(uint32_t queue, uint32_t* const* dcbs,
                                                          const uint32_t* sizes_in_dwords,
                                                          uint32_t        count);
int KYTY_SYSV_ABI GraphicsDriverSubmitAcb(uint32_t queue, const Packet* packet);
int KYTY_SYSV_ABI GraphicsDriverSubmitMultiAcbs(uint32_t queue, uint32_t* const* acbs,
                                                const uint32_t* sizes_in_dwords, uint32_t count);
int KYTY_SYSV_ABI GraphicsDriverAddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id,
                                           void* udata);
int KYTY_SYSV_ABI GraphicsDriverDeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id);
int KYTY_SYSV_ABI GraphicsDriverGetEqEventType(const LibKernel::EventQueue::KernelEvent* ev);
uint32_t KYTY_SYSV_ABI GraphicsDriverGetEqContextId(const LibKernel::EventQueue::KernelEvent* ev);
int KYTY_SYSV_ABI      GraphicsDriverSetTFRing(const volatile void* base, uint32_t size);
int KYTY_SYSV_ABI      GraphicsDriverSetHsOffchipParam(uint64_t value0, uint64_t value1,
                                                       uint64_t value2);
bool KYTY_SYSV_ABI     GraphicsDriverIsCaptureInProgress();
int KYTY_SYSV_ABI      GraphicsDriverUnknownU9ueyEhSkF4();

} // namespace Gen5Driver

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_AGC_H_ */
