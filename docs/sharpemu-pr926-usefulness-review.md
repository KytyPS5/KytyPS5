# SharpEmu PR926 bring-up review

Reference: [PR926](https://github.com/sharpemu/sharpemu/pull/926), pinned head
`848fceefbd409444e2236d7438571641d603dafb` (404 commits,369 changed files).
The PR author reports Yotei menu/gameplay on RTX5070. This is the author's test
scope, not a independently verified Kyty result. The implementation is C# with
its own scheduler, resource metadata and Vulkan interfaces; ports require shared
Kyty mechanisms and independent regressions rather than copying the branch.
GPL-2.0-or-later source headers match the intended attribution route.

## Selected candidates and existing overlap

| Area | Source finding | Kyty boundary / next proof |
| --- | --- | --- |
| Draws that only write memory | `RenderExecutor.Targets.cs` explicitly retains vertex/geometry or pixel storage writes without framebuffer exports. | `renderDraw.cpp::PrepareDrawRenderState` currently skips no-color/no-depth/!ps_active draws regardless of vertex writes, while PS activation only checks color/depth flags. Candidate regression: actual DrawAuto single-vertex buffer store/full backing without framebuffer; then fragment-only stores, untouched stale PS, zero-work and depth/color neighbors. Preserve shader write ownership and barriers. |
| Runtime descriptors / BDA | `Gen5SpirvTranslator.Resources.cs`, BdaFaultProcessor and resource graph preserve GPU-selected descriptors. | Kyty now has raw loads and formatted X/XY/XYZ/XYZW, strict runtime descriptor faults and mapped-address checks. Do not replace with another duplicate backend. Next actual workload determines remaining shape. |
| DCC clears and attachment preservation | `GuestImageCache.Metadata.cs`, per-slice metadata and deferred clear before sampling/rendering. | Kyty already has `MaterializeColorClear`, DCC/CMASK per-slice backing checks and color clear conversion. Compare uncovered retirement/reuse cases, not a blind replacement. Depth passes must not be clipped by stale unexported color targets. |
| Fault lifetime and prefetch | `GuestBufferCache.DeviceAddressFaultSpan`, mapping-clipped windows and fault-span retention avoid repeatedly dropping pointed buffers. | Candidate only after actual fault/eviction evidence. Keep mapped-range/overflow/lifetime checks and existing cache budget; invalid-pointer suppression and arbitrary allocation enlargement are not a semantic fix. |
| Tessellation | Combined LS/HS cooperative wave64, TF ring, offchip slots and bridge TCS/domain inputs. | Large host/guest interface adaptation. Needs synthetic ring/patch/domain/wave synchronization and numerical geometry proof when an actual missing stage is reached. |
| Linear surface pitch | TextureTransferLayout/CachedImage transfers preserve guest row pitch. | Compare existing image transfer helpers and non-default pitch readback tests; require a failing independent span/copy regression before changing allocation or stride. |
| Merged BDA/vertex ranges | Resource acquisition merges overlapping ranges before bindings. | Kyty already merges vertex ranges in `PrepareVertexBuffers`; compare missing shared BDA cases rather than double acquiring/replacing existing correct resources. |
| Wave64/shared phases | LDS exchange for half masks, shared-memory phase synchronization and guest LDS size. | Kyty has explicit split/cooperative paths and wave64 regressions; compare individual contracts and real guest workgroup size. |
| Opcode and arithmetic fixes | Extended multiply high words, flags as uint, literals/packed arithmetic, bitreplicate. | Many names already decoded/translated in Kyty; validate actual opcode forms and independent integer/float/mask oracle. Enum/name presence alone does not establish execution. |
| Driver cache >2GiB | Streamed cache save/load avoids signed-int lengths in C#. | Kyty reads/writes uint32 byte counts and currently validates a4GiB ceiling; the C#2GiB overflow is not the same current defect. Preserve compatibility signature/checksum/atomic rename. |
| Audio/system helpers | ATRAC9, AudioOut2, PlayGo and ABI fixes. | The PR explicitly notes unproved title-dependent AudioOut2 offsets and ATRAC9 licensing uncertainty; no title-specific layout branch or silent decoder fallback transfer. Require a guest ABI/codec contract and independent failing test for any candidate. |

Primary host contract for the storage-only draw candidate:
[Vulkan rasterization](https://docs.vulkan.org/spec/latest/chapters/primsrast.html)
discards primitives after the last pre-raster shader, not by skipping that shader.
A lack of framebuffer exports cannot erase observable shader storage writes.
Kyty's existing `IR::HasShaderMemoryWrites` and draw write barriers can inform the
shared correction; do not enable a disabled stale pixel program indiscriminately.
The current rasterization regression deliberately has a stale PS whose inputs
would fail if it were compiled after color exports were disabled.

## Current local state

User authorized committing all current files. Checkpoint `bc4b4d4d` preserves
four diagnostic source files and four progress documents; worktree was clean
immediately afterwards. Previous completed shared fixes remain separate commits.
Native Windows emulator build200525 PASS; installed SHA
`f2683c3d99e97cffab8d536ebbbb5abd4d0c485d533f02bc6f5d8003d4b857b9`.
Original eboot5178 remains untouched. No push/merge or issue108 comment here.

Previous7af/a2b run181524 ends exit0 at18:37:03UTC after chat interruption,
no deadline/memory guard,1178 pipelines, both former formatted frontiers now
pass actual pipeline creation (9d4c1340 takes165446ms). Exit cause is unproved;
loading arc is the last recorded image, no menu/entry proof.

Current original warm run200619 is active under an independent bounded native
supervisor, PID47764/supervisor31612,1800s/28GiB/240s close grace. Source frozen;
no simultaneous native build/test. Reference source read-only while waiting.
No production transfer from PR926 yet. Any candidate fix starts with recorded
synthetic RED, same GREEN, affected native/GPU tests and original workload retry.
Public reference/archive and API file inventory are outside tracked source in
`_Build/analysis/sharpemu-pr926-20261009/`; no proprietary captures copied into tests.


Final native checkpoint: warm original200619 naturally exits32120:28:24UTC,
1237 complete pipelines, both prior formatted frontiers passed. New pixel
PS07b3d5ecac4aad8a PC6d0 rejects independent inline image and sampler material
buffer/selector. This makes PR926's runtime resource/waterfall/dense image and
bindless heap work directly relevant as a reference; Kyty's own planning,
separate sources, budget/ownership and GPU selection still require regression
proof. No new production port at checkpoint. No menu/entry; native processes
ended. User requests transfer context and this single Codex session to Windows,
then continue there; no push/merge/issue post here.
