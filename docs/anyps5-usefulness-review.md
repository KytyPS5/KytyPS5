# AnyPS5 shader and bring-up review (2026-10-09)

Source: [boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5/tree/d70b89989473ba1f6ae13e44e079e67f1f8a44b0),
revision `d70b89989473ba1f6ae13e44e079e67f1f8a44b0`.
Target baseline: `daca7b2315d6e64aa8304b5669a0f9f0733a0018`,
`yotei-windows-bringup`; existing renderer diagnostics and local documentation
were preserved. Both repository roots contain GPL version 2 licenses.

AnyPS5 relinks executables to native Windows/Linux programs and supplies PRX
libraries. KytyPS5 owns loading, CPU runtime and its renderer itself. Their
RDNA-to-IR-to-SPIR-V compilers use different types, resource metadata and host
execution contracts. Copying the compiler or host GLSL files does not connect
those contracts. This review selects instruction semantics that fit existing
Kyty IR rather than replacing the backend.

## Selected scalar instruction transfer

Adapted from AnyPS5 `core/shader/recompiler/Translation/src/ScalarInstructions.cpp`
and decoder `RdnaDecoder/src/RdnaScalarOpDecoder.cpp`; source attribution is
retained in the Kyty translator. AMD RDNA2 ISA scalar arithmetic/bit operations,
SOP1 encodings, are the independent contract (local primary reference:
`_Build/analysis/rdna2-isa-budget.txt`, sections 5.4/5.8 and SOP1 instruction list).

| Instruction | Encoding | Shared behavior | SCC |
| --- | --- | --- | --- |
| S_BREV_B64 | 0x0c | Reverse all 64 bits, swapping reversed DWORD halves | Preserve |
| S_BCNT0_I32_B32 | 0x0d | Count zero bits in 32-bit input | Result != 0 |
| S_BCNT0_I32_B64 | 0x0e | Count zero bits in both DWORD halves | Result != 0 |
| S_FF0_I32_B32 | 0x11 | First zero from LSB, or 0xffffffff | Preserve |
| S_FF0_I32_B64 | 0x12 | First zero across bit31/32, or 0xffffffff | Preserve |
| S_SEXT_I32_I8 | 0x19 | Sign extend input bits7:0 | Preserve |
| S_SEXT_I32_I16 | 0x1a | Sign extend input bits15:0 | Preserve |

These were absent from Kyty's decoder and translator. They reuse existing
BitReverse32, BitCount32, FindILsb32 and BitFieldSExtract, the existing 64-bit
first-bit helper and normal register-write semantics. Existing opcode ordinals
are preserved; unknown encodings still reject. No title/hash/address dispatch,
zero resources, synchronization bypass or device-limit increase was added.

Regression: `--scalar-zero-sign-cpu-only` and `--scalar-zero-sign-gpu-only` in
`tests/ShaderRecompilerComputeTests.cpp`. Decoder checks use seven synthetic encodings;
GPU fixtures use runtime buffer inputs and an independent bit-by-bit oracle.
They check zeros/all ones/alternating/asymmetric halves, first zero at31/32/63,
byte and short sign boundaries, in-place source/destination alias, both SCC states,
untouched source data and two guard DWORDs. Guest wave32/wave64 are explicit.
Existing scalar bitfield, BREV32/SCC and 64-bit operations are numeric neighbors.

Native baseline RED: `_Build/checks/20261009-124105-0716504-shader_recompiler_compute_tests`
(build0, intended missing-SOP1 decoder failure). Same unchanged CPU GREEN:
`_Build/checks/20261009-124320-2749990-shader_recompiler_compute_tests`.
Initial124001 fixture build used an incorrect workgroup member name; it was
corrected before RED and is not semantic reproduction evidence. Native GPUAV GREEN: `_Build/checks/20261009-124535-5851930-shader_recompiler_compute_tests`,
seven numeric cases including four explicit wave/alias variants and three
existing scalar neighbors; no validation errors. This is synthetic cross-case
coverage, not a second-game compatibility claim. No proprietary shaders were
added to source. Full shader_cfg still has the separately recorded baseline
scalar-descriptor/EXEC failure; focused tests do not establish full-suite GREEN. Final registered CTest
`_Build/checks/20261009-124727-9774579-shader_recompiler_compute_tests`: CPU0.54s,
GPUAV65.27s, PASS2/2. Core/tests commit `f20f2bd8` includes CTest registration
and default corpus registration. Native emulator build: `_Build/checks/20261009-125056-0716455-kyty_emulator`,
PASS at core `f20f2bd8e6196f184b2abf4b72e28ea9c481dd6a`. Installed SHA-256
`335749799ad684bffe587eb7eb1753b8326df50581ddeaa875a2c7a3ff367d4a`; previous
EXE87a9 and985153519B driver cache preserved in the analysis directory.

## Other useful candidates and overlap

| Area | Finding | Next requirement |
| --- | --- | --- |
| ShaderDiskCache / request serializer / source version | AnyPS5 persists compiled variants and result metadata, with exact keys, rejection of incompatible entries and asynchronous writes. Kyty currently persists Vulkan driver cache and reuses SPIR-V permutations in memory. | Separate regression for serializing the complete Kyty resource/binding contract, identical restored results, corruption/key mismatch, source/device invalidation and lifecycle flush; measure cold/warm title runs. Driver pipeline compilation cost may remain. |
| SPIR-V optimizer | AnyPS5 avoids exhaustive helper inlining by default. Kyty already has bounded local/cooperative recipes, also avoiding full inlining/unrolling. | No blind recipe transfer; measure instruction/helper growth and preserve numeric/barrier behavior. Last Yotei driver pipeline still took109978/112657ms, despite optimizer5626/6486ms. |
| DS_PERMUTE_B32 | AnyPS5 has push-style lane routing, distinct from BPERMUTE. | Collision/disabled-lane/wave64 numerical regression and shared IR/backend support, including host subgroup limitations. |
| Buffer/flat/image atomics | AnyPS5 has additional decode/translation paths; some merely select an operation and still depend on backend capability checks. | Review each memory scope, return/GLC behavior, bounds and operand width before transfer; atomic opcode names are not execution proof. |
| Image BY2/BY4 | Their TechnicalDebt records narrow hardware-oracle cases with format/mask/dimension restrictions; stores still throw at translation. | Independent format/dmask/alignment/OOB oracle and resource ownership before importing a supported subset. |
| MeshArguments / TextureDetile / ColorTransfer / SampleCounter GLSL | Bound to AnyPS5 AGC descriptors and push constants. Kyty already has tiled texture, color/depth transform and presentation helper shaders. | Capture a missing command/format contract before integrating another GPU pass. |
| Static scalar shader calls | AnyPS5 recognizes bounded nested static link-pair call regions, rejecting recursion/shared links and escaping link values. | CFG-level call/return regression if a title actually reaches an unsupported call. Kyty already treats NULL SWAPPC as a plain jump. |
| Ray tracing | AnyPS5 has a BVH operation and optional strict/miss modes. | Do not import forced-miss mode: it fabricates misses. Real traversal needs descriptor layout and triangle/box numerical proof. |
| PRX and executable relinker | Native shared-library loader and library ABI differ from Kyty. Some library code cites Kyty as its own source. | Port only an independently reproduced guest ABI gap; avoid circular or cosmetic replacement. |

Textual comparison found1076 AnyPS5 translation case labels versus609 Kyty labels,
with467 unmatched normalized names. This is a search inventory, not a count of
working missing instructions: aliases, generic routes and explicit throws inflate
it. Full inventory is local `_Build/analysis/anyps5-study-20261009/textual-inventory.json`.
AnyPS5's compatibility table currently lists Dreaming Sarah; it supplies no
verified Yotei menu/gameplay evidence.

## Current Yotei runtime boundary

Latest pre-transfer native run:
`_Build/runs/game-20261009-034619-runtime-stride-warm-capture/run.json`,
source daca7b23 plus preserved diagnostics, installed SHA-256
`87a9a607f79d232755510530031711f449c6082e08e08e1313ead31738bf5ddc`.
Started03:46:19UTC, finished04:46:35UTC,3600s deadline/graceful exit0,
no memory guard; final pipeline1215, cache985153519B saved. Streams end in
pipeline creation/cache save, with no captured fatal. No active native process
was found before this work. No visually verified menu or game entry, and no
issue108 publication milestone. New scalar support has no measured effect on
this title yet; shader validation alone must not be presented as a boot fix.

Post-transfer bounded smoke: `_Build/runs/game-20261009-125238-anyps5-scalar-smoke`,
original eboot5178 unchanged/rehash verified;1280x720, normal async Performance,
120s budget, graceful exit0 at12:54:54UTC, no memory guard or forced cleanup.
Loaded985153519B cache, checkpointed985689265B; stderr empty. Owned-window
PrintWindow capture `smoke-window.png` is black and is not GPU swapchain readback.
No visual menu or game entry. No issue108 comment; new instructions have no
measured Yotei boot benefit. Original diagnostic diffs compare byte-identically.

Final remote read-back (2026-10-09 12:59UTC): `git ls-remote origin
refs/heads/yotei-windows-bringup` returns the tested core
`f20f2bd8e6196f184b2abf4b72e28ea9c481dd6a`; tracking reflog says "update by push".
This session performed no push. The publisher was not identified; its CI outcome was not checked here. The review
commit is local. No publication to game issue108 occurred.

## Follow-up: badge meaning and complete integer64 predicates

The public progress page reports1166/1166 (100%). Inspecting
`tools/progress.py::collect_shaders` shows that this is derived from enum names,
aliases and FLAT/GLOBAL/SCRATCH name expansion. It does not run decoder fixtures,
IR/SPIR-V validation or numeric GPU tests. The HTML describes this as recognition
by the decoder. Examples still rejected by AnyPS5: GWS synchronization/semaphores,
ordered GDS counts and unimplemented image stores BY2/BY4/PCK. Its native runtime
memory ownership and host execution model cannot simply replace Kyty's guards.

Local reference scanner independently reproduced1166/1166; artefacts
`_Build/analysis/anyps5-continuation-20261009T134838Z/` contain badge-count output,
source gap inventory and direct opcode table pairs. Table/name counts remain
search aids: Kyty's generic MIMG_SAMPLE/MIMG_GATHER flags already represent many
separate AnyPS5 opcode names. They are not missing runtime support merely because
there is no matching enum string.

Core/tests `6e401f7c618e92e18a0ddf0f007f8672dbc7c9bf` completes all32 I64/U64
V_CMP/V_CMPX predicates by adding25 absent operations. VOPC and VOP3 forms,
strict signed/unsigned order, masked predicates, unchanged VCC on CMPX and SCC
preservation use the existing shared compare IR/backend. DPP/DPP8 remain rejected.
Native intended decoder RED140128 -> CPU GREEN140748; GPUAV141131 PASS7 cases,
including four explicit wave/form fixtures with dynamic per-lane input, signed
extremes/high-word differences and full/partial/zero EXEC. Final registered native
CTest141600 PASS2/2 (CPU0.18s/GPUAV3.19s). Initial classification omission was caught
and corrected. The first GPU fixture used loadX3 encoding0x0f instead of loadX4
0x0e; its input was corrected without changing the oracle. That mismatch is not
production semantic failure evidence.

Original native f20/EXE335 timing-only retry134839 ran600s, completed570 pipelines,
closed gracefully0 at13:58:48UTC, no memory guard. Cache grew988710222B. Optimizer
sum185876ms/692 calls and compute creation sum79667ms are operation sums, not
wall-time partitions. No verified menu/game entry. This motivates independent
proof of earlier reuse of already-invariant raw DWORD stride variants; that work
is separate from the completed25-op transfer.

## Follow-up: early reuse of runtime raw DWORD stride variants

Two independent native regressions establish the behavior gap: actual cache
RED142849 (8->48 produced another compiled program object despite equal SPV)
and actual renderer RED143036 (cached8/current descriptor48 publishes old8).
CPU cache GREEN143351 and metadata GREEN144025 pass without changing those
assertions. A compiler-owned binding annotation proves every live access to an
eligible slot is ordinary raw DWORD with runtime stride. Bounded tables, scalar,
typed/formatted, byte/short, atomic and image-alias slots cannot receive that proof.
The cache still requires exact SRT/table/origin/image/sampler state and layout,
positive strides, unchanged swizzle/ADD_TID and the rest of each buffer signature.
Only proven stride differences skip decode/IR/SPV/optimization; descriptor
materialization and ownership/bounds preflight still run for every request.
Renderer metadata now comes from the current resource snapshot, not the retained
compiled buffer's stride. No arbitrary static-state canonicalization was added.

Native actual renderer GPUAV145227 passes8->48->8/zero->8 read/store and complete
256-DWORD backing/sentinel checks in actual guestwave32/wave64 (one invocation).
The first dispatch fixture used wave64 mode0x41 against initialwave32; valid
wave-state specialization correctly rejected that reuse. Correct dispatch mode
0x8041 forwave32 and0x41 forwave64 aligns the fixture; the oracle is unchanged.
Final registered GPUAV145614 PASS4/4, including six real-cache negatives (mixed
byte, typed, atomic, scalar, swizzle and ADD_TID) and zero-stride transitions.
Affected GPUAV145854 PASS5/5 (six raw runtime stride cases, compact bounded
metadata, vertex byte, pixel formatted and bounded zero-stride). The direct
renderer checks do not claim full-wave occupancy; existing raw-stride harness
covers full guestwave64 independently. Initial fixture142441 had two wrong API
calls corrected before RED; it is not reproduction evidence.

The prior dirty timing/source diagnostics are kept outside the staged cache fix.
Native CPU150125 PASS4/4 (raw module identity, bounded shared access, cache
identity and permutation reuse). Core `8c5beaa1` is committed separately; native
build/install and measured Yotei benefit remain pending. The changes are a local adaptation of runtime-versus-static resource
separation; AnyPS5's disk serializer itself has not been copied into Kyty.


## GPU-selected formatted X reads and runtime descriptor faults

Actual original-game run `game-20261009-150855-anyps5-reuse-native` on
`8c5beaa1` / installed EXE SHA8a9407 naturally exited321 at15:31:20UTC,
without deadline or memory guard. It completed1158 pipelines and1349 optimizer
calls. Owned-window capture `window-1518.png` visibly shows a white loading arc;
no menu or game entry. Registered source for CSaf9e060c8c9207ba has
BUFFER_LOAD_FORMAT_X at PC0x230, not a confirmed raw subword load. The capture
contains no runtime resource values, so its actual format remains unproved.

The AnyPS5 runtime descriptor dispatch informed a shared Kyty adaptation:
read-only formatted X / U32 output through BDA with GPU-selected descriptors.
Known format conversion uses Kyty's existing format information/converters,
with runtime component selection, integer/float constant selectors, null/EXEC
handling, address stride/swizzle/SOFFSET, ADD_TID, OOB_SELECT0..3 and mapped-address
checks. One shared DontInline SPIR-V helper avoids multiplying the format switch
at each access. Wide formatted reads, D16/typed access, writes and atomics retain
their previous admission restrictions. No game/hash/PC/address gates.

Unsupported live format, reserved dst_sel and invalid type set a dedicated
trailing fault DWORD. The real fault parser moves these bits into the existing
download header's upper DWORD, leaving ordinary page faults in its lower count
and page list. The actual renderer consumes the bits and exits with a specific
error; a dummy shader return is never accepted as successful emulation. Lazy
initialization clears the device-local fault buffer before its first use.

Independent resource admission RED155757 -> same GREEN164341; compute admission
RED163638; actual generated parser RED161846 -> GREEN162713 GPUAV. Actual renderer
error handling scoped-reversal RED170719 -> same GREEN171049: all three child
workers reach READY, fail with the intended diagnostics and never return success.
Earlier170233/170503 fixture initialization failures are not semantic RED proof.
Initial165220 GPUAV pipeline compilation exceeded180s; DontInline resolves the
helper expansion issue. Ordinary165826 PASS and final expanded171515 GPUAV PASS
cover28 valid format/address/EXEC rows per explicitwave32/64, four invalid states,
exact fault flags and the entire4096-DWORD backing. ADD_TID checks here use lane0,
not full-wave occupancy. No GPU validation errors. Registered/neighbor172407
GPUAV PASS10/10 includes parser mixed page+descriptor faults/65-word rounded
workgroup boundary, all host fault paths, old raw DWORD/ushort and static/bounded
formatted paths. Resource admission172520 PASS. The new formatted operation is
synthetically proved; native emulator/game retry is pending at this checkpoint.
Other-game GPU corpus is unavailable; full shader_cfg retains recorded baseline
debt. No all-shaders or playable-game claim, push or issue108 update.


Core `4d5a2376b7a99ea3dc065b55d20b47692b2df344` contains the reusable formatted-X
fix/tests/CTest registrations separately from prior dirty renderer diagnostics.
Seven affected indirect/BDA GPUAV cases172550 PASS. Native emulator172716 PASS;
installed SHA142a67f7a8c7f0bc6bdd5491fbcd273a62e9aa2b379c362151c786aff35a2392,
old EXE8a9407 and1002959326B cache preserved. Original native retry172811 is
active with1800s/28GiB budget. No menu/game entry result yet at17:28UTC.


## Next native frontier: formatted XYZ

Actual4d5a/EXE142a original5178 retry172811 naturally exited32117:45:22UTC,
1172 complete pipelines/no deadline or memory guard. The previousaf9e failure
is removed through resource planning/SPIR-V and actual pipeline1319 creation
(51434ms). Window1735 shows loading arc, window1745 black; no menu/entry. New
CS9d4c341d4e05f879 PC704 private registered source confirms MUBUFopcode2
BUFFER_LOAD_FORMAT_XYZ IDXEN. Actual format is still not captured.

New independent resource wide-load RED174837 -> unchanged GREEN175338;
compute XY/XYZ/XYZW RED175209 -> CPU GREEN175420. Initial175110 test name
assignment failed to build and is not semantic RED. The shared conversion helper
now accepts output index and transfer width; it computes the set of required
source components and ANDs their bounds before reading any component. Each
output retains its own dst_sel/default/conversion and strict fault reporting.
This preserves whole formatted-transfer OOB rather than enabling independent
partial component reads. Address operands are snapshotted before destination
writes. Native numeric GPUAV proof and original-game retry are pending.
Fixtures include partial records, repeated components, constant selectors,
packed/unpacked integer/float/normalized formats, odd short addresses, null,
EXEC, full backing and reserved selectors only among transferred outputs.


Expanded six-case GPUAV175543 PASS XY/XYZ/XYZW32/64, full backing/12 rows each,
exact runtimefault flags including ignored untransferred selectors. Code size
121094..136409 words, so the initially conservative250000 test-only budget is
tightened to180000 for final registration; no device/resource limit changes.
All numeric expectations are unchanged from RED. No full-wave occupancy or
other-game compatibility claim. Original-game retry remains pending.


Final registeredGPUAV180052 PASS10/10 includes expanded formats and the original
unchanged numeric X oracle. Complete resource_tracking initially exposed three
old assumptions introduced before formatted-X GPU support: unproved table
witness/count guards were expected to fail rather than execute through DMA;
a typed-Phi negative fixture had formatted=true without typed=true. Table/proof
positives stay unchanged. Negatives now explicitly assert no bounded host SRT
snapshot/table, one live IndirectBuffer formatted read and DMA; the typed-Phi
fixture actually uses typed metadata and still rejects transactionally. Full
native resource_tracking plus both admission registrations181300 PASS3/3.
No production guard was removed for those CPU fixture updates. Initial180020
registration used a capturing lambda for a function-pointer API; fixed before
final180052; that build error is not semantic proof. Emulator retry pending.


Core7afac3cd committed separately; nativeemulator181435 PASS, installed
SHAa2b62ef9fa273ddb903541b72c7b894f1f866ffaeb143766dc100ed1b9c1866c.
Actual original5178 run181524 passes both priorformatted failures, including
9d4c pipeline1340 creation165446ms,1178 total pipelines. It ends exit0 at
18:37:03UTC without deadline/memory guard following a chat interruption; closure
cause is unproved and this is not successful-boot evidence. Cache1012582765B
saved. Last1824ownedwindow shows loading arc, no menu/entry. Warm retry pending.
