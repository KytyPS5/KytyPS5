# Emulator regression test debt

## Runtime menu comparison and exact row-domain proof (2026-10-10)

User requests regression investigation before more gate-by-gate extensions.
Oldb4eae640/87f24 initial Medium setup is visually verified; the same binary
later failed sampler33 after saved Quality selection. Source regression is
not isolated. Compare exact copied save/cache state, None2560x1440/redzone
for baseline/current; then separate fresh first-run cases if needed. No user
SaveData reset. Investigation/seed manifests and native baseline helper are
under _Build/analysis/menu-regression-investigation-20261010; baseline checkout
has exact pinned dependencies and must use serialized native execution.

Diagnostic092136/a593/454656 naturally exits32109:39:32.199UTC/no guards,
minRAM13.39GiB. Wave table record0,row64 is not a texture (type4 reserved);
source2records/stride136, guard range255,row_stride368. Earlier rows passed.
Do not patch only LLC bits or replace this entry with null. Require minimal
native RED for a packed sparse selector set with an excluded sentinel and
unavailable/invalid rows elsewhere in the guard range, then prove shared
selector provenance/materialization bounds. An invalid actually reachable
row must still fail transactionally. Hardcoded game row counts are prohibited.

## Wave-address table descriptor validity (2026-10-10)

Original5df6ff5d/EXE4fefcb/eboot5178 run090158 naturally exits321 at
09:18:42.632UTC,1266 completed pipelines, no guards, minRAM13.25GiB.
Full actual PS617c tracking passes, then materialization rejects an
unsupported image descriptor from the wave-address table. Require bounded
diagnostic evidence for source record/row, pointer/source metadata and all
eight descriptor words. Determine whether this is unsupported guest format,
incorrect field validation/provenance, or an unreachable table row; derive
the contract independently and reproduce synthetically before a semantic fix.
Do not discard the descriptor, substitute zero or disable validity checks.

## Nonempty original mask after a preceding loop (2026-10-10)

Original game8799c502/EXEa84498/eboot5178 run084004 naturally exits321 at
08:56:54.123UTC,1265 completed pipelines, no guards, minimum RAM13.72GiB.
Tracking passed pc8b4 and rejects pcA20 (transactional group). CPU083643
identifies the individual remaining failure at pcDF4: initial mask1659
remains nonempty from an earlier positive EXEC edge, but loops42/43 and46/47
precede Phi1930 and the backward proof rejects cycles. Require synthetic RED
with a preceding control-flow loop in the bounded CPU proof harness that leaves the original mask
unchanged; an empty-mask bypass must remain rejected. Any dominance proof
must establish that the witness tests the current mask definition, including
re-evaluation boundaries, rather than reuse a previous iteration's value.

Verified extension: native admission RED085811 -> same GREEN085900,
including the zero-mask bypass negative. A dominating positive edge proves
nonemptiness through loops only when the current mask definition dominates
the witness and is reachable with that edge removed, while the incoming
block is not. Full resource090014 and GPUAV090033 contracts/ownership/numeric
readback PASS. Captured full shader CPU085935 passes tracking31images,
unknown/unsupported instruction lists empty, with default pixel metadata.
Materialization, SPIR-V/driver execution and original-game runtime remain
pending; the temporary audit mode was removed and preserved as a patch.

## Nonempty wave mask across a control-flow merge (2026-10-10)

Captured PS617c diagnostic CPU audit passes pc8b4 with the shrinking-mask
implication correction, then fails at pcA20: HasActiveLane stops at a
merge with two predecessors. Require a synthetic positive EXEC witness
before a diamond, preserving that witness on every route into the mask phi;
a bypass permitting an empty mask must remain rejected. Establish native
RED before extending the shared proof. Saved diagnostic audit081132 is
tracking only with default pixel metadata; it does not prove GPU execution.

Proof update: shrinking-mask native RED072739 -> GREEN073014; merge
RED083437 -> unchanged GREEN083519. Full resource083624 passes, including
empty-mask bypass and growing-mask rejection. GPUAV083740 passes three
existing address-table contract/ownership/readback cases. Diagnostic CPU
083643 now recognizes individual sampled images through pcD88 but fails a
later nonempty-mask proof at pcDF4; transactional inline-group rejection
still reports pcA20. Full captured shader tracking remains unproved. Next
isolate that remaining mask shape before another semantic extension.

## Real wave-address graph recognition extension (2026-10-10)

Original5178/source1ee315aa/EXE6b8e6 run060729 naturally exits321 at
06:48:02.696UTC,1265 complete pipelines, no guards, min RAM16.99GiB.
The bounded synthetic address-table case is numerically proved, but real
PS617c still rejects pc8b4 during tracking. Do not claim that full shader or
game works. Saved ps617c-early.txt contains all scalar-address/pointer/row
ancestors. Next identify the exact failed proof (row dominance, active-mask
phi/Select ancestry, paired pointer/load provenance), reproduce that extension
synthetically, then preserve the original mask/address/ownership constraints
while extending shared recognition. No rejection suppression or empty-lane
assumptions. A new game diagnostic is justified only if existing IR cannot
identify the failed proof; keep diagnostics separate from semantic fixes.

## Wave-selected scalar-address image table (2026-10-10)

Original5178/sourceed3f2e74/EXEd2bd run042530 naturally exits321 at
04:41:53.957UTC, no timeout/memory guard, min RAM17.64GiB. PS617c passes
the first scalar-buffer IMAGE_LOAD check, then fails TrackResources atpc8b4:
GetImageResource dword0 from LoadAddressU32 is not a valid runtime value.
Existing diagnostic ps617c-early.txt covers this later block: raw scalar
address loads use low/high ReadFirstLane pointer words,368-byte row offsets
and an independently selected inline sampler. Require a minimal bounded
native RED for paired pointer provenance, row selection, full-width mapping,
independent sampler selection and exact numerical results. Diagnose pointer
and row domains first; preserve full address bits, clean-memory ownership,
write protection/bounds/quotas, and explicit unsupported cases.

Bounded shared implementation proved locally: paired indexed VMEMx2 pointer
fields, matching ReadFirstLane masks/original scope, guarded row limit, full
canonical48-bit address reads, independent inline sampler and separate U32
record/row keys. Corrected-fixture admission RED052530 (scoped disable of only
new admission, restored) -> numeric GPUAV GREEN052958. Secondary-key cleanup
RED052939 proves CompileProgram must retain both GPU keys. Initial050228 used
the wrong shift encoding and is superseded. Neighbor-stage ownership
RED054831 -> GREEN055328; conservative decoder classification includes only
known read-only producer code, rejects stores/atomics/unknown/indirect control.
GPUAV055827 passes full wave32/wave64 and complete backing. CPU055635 proves
same-low/different-high addresses, distinct rows, transactional image/sampler
limits, unavailable upper words and writer-alias rejection. The060028 batch
passes8/9; atomic fixture encoding was unsupported, corrected and rerun as
CPU060226 PASS1/1. Resource tracking/admission060325 PASS3/3; shared ordinary
sampling GPUAV060437 PASS. No compiler/device/probe ceilings raised.
Null/unaligned pointers, unavailable descriptor bytes, unsafe stage writers,
unproved row domains and unsupported addressing shapes remain failures.
Native emulator rebuild and original PS617c retry pending.

## Scalar-buffer image descriptor runtime provenance (2026-10-10)

Original5178/source0b147273/EXE3ddab run033329 naturally exits321 at
03:49:08.420UTC,1262 completed pipelines, no timeout/memory guard, available
RAM minimum18.77GiB. PS07b3 passes tracking/emission/validation with350494
->324041 SPIR-V words; its asynchronous graphics pipeline has a begin only,
so driver completion remains unproved. Next PS617c fails TrackResources at
pc380: full eight-word GetImageResource dword0 is not a valid runtime value,
root=ReadConstBuffer. Require diagnosis of buffer/key/provenance before the
minimal synthetic native RED; preserve scalar-read bounds, loop predicates,
resource ownership and source lifetime. No zero substitutes/error suppression.

Diagnostic same-source run035159 naturally exits321 at04:07:43.598UTC,
no guards. Relevant decoded/normalized IR extracted (12911 lines) to
`_Build/analysis/scalar-buffer-image-provenance-20261010/diagnostic-20261010T035159Z-66f1f556/ps617c-early.txt`.
IMAGE_LOAD consumes one eight-word half of S_BUFFER_LOAD_DWORDX16,
136-byte record stride and vector-derived ReadFirstLane key. Tracking and
materialization incorrectly require a sampler for this fetch-only image view.
Synthetic CPU intended RED041137; GPU ReadFirstLane RED041524. Initial CPU
arity failure is setup only; scalar-selector GPU041236 passes old code and
is not RED. Same CPU GREEN041903; full resource/admission042210 PASS2/2,
including full upper-word payload, no sampler, exact mapping, transactional
image-budget/unavailable-byte failures. Full/compact GPU loads042018 PASS
with OOB key; registered loads plus independent sampling042244 PASS2/2;
ordinary/shared full-image sampling GPUAV042332 PASS. Native rebuild and
original-game retry pending; async PS07b3 driver completion remains unproved.

## Independent sampler indexing without Cartesian sample instructions (2026-10-10)

Original5178/sourceebf81d71/EXE452ce run025503 passes PS07b3 materialization
and SPIR-V validation (1203794 ->973501 words), then exceeds32GiB while
the driver creates its graphics pipeline. Supervisor memoryGuard=true,
timeout=false; close grace expires and owned process is killed at03:23:16.681UTC.
No menu/gameplay. Required bounded CPU RED: on an explicitly enabled sampled
descriptor nonuniform-indexing host profile, independently selected samplers
must not multiply sample instructions per image. Same numeric GPU oracles,
NonUniform validation, mixed-class descriptor mapping, OOB/default keys and
unsupported-feature fallback. Preserve all actual descriptors and quotas.
Contract: Vulkan shaderSampledImageArrayNonUniformIndexing includes separate
sampler arrays: https://docs.vulkan.org/spec/latest/chapters/interfaces.html .

Completed locally: CPU RED032438 expects three sample operations but gets11;
same GREEN032629, with disabled/unknown profiles validating the fallback.
The RED adds only a disabled profile metadata field/test seam before emitter
changes. Optional logical-device feature enabled only when advertised; shader
profile and diagnostics record actual enablement. Integer index selection,
bounded descriptor mappings and NonUniform sampler/sample annotations avoid
opaque-object phi nodes and preserve class-specific operands. GPUAV032803
passes nine numeric cases (three also force the fallback), full T# and full
wave32/64 backing. Registered GPUAV032942 PASS6/6; saved ctest-details.log
confirms sampled indexing enabled=1 and three sampling operations per image
set. Native emulator rebuild and large original-game pipeline retry pending.

## Compact independent sampled-use topology (2026-10-10)

Original5178 / source1272580a / EXEfeb412 run023009 naturally exits321
at02:49:05.403UTC, without timeout or either memory guard. PS07b3 now passes
the image-view budget but expanded sampled-use pairs exceed MaxSampledPairs.
Required synthetic RED: independently selected images/samplers retain all
native operands and class-specific sampler usage without constructing the
Cartesian pair list. Prove bounded use metadata, unchanged numeric selection,
class cloning and strict native descriptor budgets. Do not raise graph quotas
or discard actual sampler states.

Completed locally: native intended RED025026 -> unchanged GREEN025054.
Full resource suite025129 PASS2/2 includes independent float/UInt class
cloning and transactional native-sampler overflow. Six unchanged numeric
GPUAV cases and wave64 neighbor025158 PASS2/2; sampler/image-capacity and
coherent-selector GPUAV025311 PASS3/3. Complete sampler sets contribute
class usage directly; explicit sampled pairs retain representative use edges.
No descriptor or graph limits raised. Native emulator build/retry pending.

## Independent image/sampler bindings without Cartesian image duplication (2026-10-10)

Nativef8b8e2f8 / EXE9b191 / original5178 run010007 naturally exits321 at
01:36:13.035UTC,1270 complete pipelines, no timeout/process/system memory
guard. PS07b3 passes resource tracking, then independent candidate products
exceed the dense image resource limit during materialization. No menu/entry.
Required RED: two direct inline images plus null and independently selected
samplers must materialize within a three-image budget, without raising quotas
or dropping sampler state; same numeric outputs for all root/key variants,
compact/full descriptors, wave32/64/full backing. Use separate sampler slots
and key mappings, preserve per-origin classes/ownership/bounds and strict
aggregate native sampler/image/pair budgets. Same-source driver cache warmed
through six bounded runs; do not discard it for speculative changes.

Completed locally: intended native RED
`_Build/checks/20261010-014057-9245537-resource_tracking_tests`; unchanged
three-image-budget GREEN015807 and full resource suite020913 PASS2/2.
Separate image/sampler bindings pass all six unchanged numeric GPUAV cases
in022433, including full T#, wave32/64 and complete backing. Registered
neighbors021106 PASS5/5; shared/ordinary image GPUAV022037 PASS. Image,
native sampler and sampled-pair quotas remain enforced. Independently
selected manual depth compare and nested image tables remain explicit
unsupported cases. Native emulator rebuild and original-game retry pending.

## Split-wave64 scalar ConditionRef provenance (PR1338 candidate, 2026-10-09)

Current `spirvEmitterFlow.cpp::EmitConditionRef` passes a `const Inst*` to
`IR::Value`, selecting the bool constructor. PR1338 head
`0b022120dde4e5dde752e60e671183b5d9100635` fixes this reference conversion.
Before adaptation, require a bounded native regression: scalar buffer read
sets a false SCC branch, both wave64 halves write the same independently
computed value, and full backing remains unchanged outside those stores.
Use a forward branch only, with no guest loop or barrier that could hang.
Then same GREEN plus true-branch/mask and neighboring wave64 cases. Do not
run new wave64 sampled loops until this candidate is resolved.

Completed as local `c01d2892`: direct production-emitter ID-handoff
RED220232 -> unchanged GREEN/GPUAV220352. Forward false-SCC branch checks
64 lane results/full backing. Ordinary branch220023 passed on the old code
and is not reproduction evidence. Registered neighbor batch220925 PASS5/5.

## Independent inline image and sampler sources (2026-10-09, current native frontier)

Direct compact/full image support is now numerically proved in the working
patch on top of `c01d2892`. Native admission RED214439, native shader
RED215357 (scoped restoration of only the original correlation rejection),
root-lifetime RED215806 -> same GREEN215856. Native GPUAV215909 passes
separate roots/keys/both; GPUAV220509 adds full wave32/64 occupancy and whole
backing; GPUAV221416 additionally passes full eight-word image descriptors.
Resource tracking + registered admission221612 PASS2/2; shared-image and
ordinary-sampler GPUAV221724 PASS. Quotas/unavailable memory remain failures.
Nested image-table + independent sampler is explicitly rejected and has a
negative regression: OOB image keys can select valid table[0], requiring a
separate numerical regression before extending the mapping. Other-game
runtime coverage and the original Yotei retry remain pending.

Related lifetime regression (2026-10-09 21:56 UTC): native numeric fixture
passes resource tracking but fails materialization after production DCE;
independent sampler buffer source dword3 has become `Void`. A sampler handle
has only four operands and cannot retain its live key plus all four root words.
Required RED: extract the synthetic plan after DCE, then materialize independent
sources with the unchanged descriptor oracle. Retain complete planning roots
through the existing resource-source lifetime mechanism, never replace the
lost word with zero. GPU diagnostic artifacts:
`_Build/checks/20261009-215626-0671273-shader_recompiler_compute_tests/`.

Native Windows continuation 2026-10-09 21:44 UTC: source `959cce06` with
test-only additions to `ResourceTrackingTests.cpp`; unchanged production
`--independent-inline-sampled-sources-only` reproduces the exact same-material
rejection. Verified RED artifacts:
`_Build/checks/20261009-214439-9671471-resource_tracking_tests/`.
Fixture separates material roots and/or live loop selectors using synthetic IR.
This is resource admission proof only; numeric materialization, native GPU
wave32/64 results, strict boundary cases and original-game retry remain pending.

Nativecheckpointbc4/EXEf268/original5178 warmrun200619 naturally exits321 at
20:28:24UTC,1237 complete pipelines, no deadline/memoryguard. Both formatted
frontiersaf9e/9d4c now pass. New pixel shader07b3d5ecac4aad8a PC6d0 rejects
inline image and sampler not sharing material buffer/selector. No menu/entry.
Required before production: synthetic pixel image+sampler sourced from different
material roots/selectors, independent numeric texture/filter/LOD results; same
RED/GREEN, correlated-source neighbors, wave32/64 and nonuniform cases, full
resource/device budget/bounds/ownership rejection boundaries. Compare pinned
SharpEmu926848fceef resource/dense-image waterfall planning and persistent
bindless heap; do not force selectors to match, fake samplers or raise limits.
No production change started at handoff; shader/loop/ABI-only proof is not menu.

## Draws that only write storage (2026-10-09, PR926 candidate; not current fatal)

SharpEmu RenderExecutor.Targets.cs retains vertex/pixel writes without framebuffer.
Kyty PrepareDrawRenderState currently skips no-color/no-depth/!ps_active draws
regardless of vertex writes. Required independent actualDrawAuto RED: one
vertex raw buffer store with no framebuffer, full backing/sentinels; same GREEN,
pixel-only storage writes, zero-work/disabled stalePS/depth/color neighbors,
proper shader write ownership/barriers and valid empty-attachment rendering.
Use existing IR::HasShaderMemoryWrites. Preserve supported guest shader effects,
not merely BDA-read presence. No production change started before handoff.


## GPU-selected formatted XYZ (2026-10-09, new native frontier)

Core4d5a/EXE142a original5178 run172811 naturally exited32117:45:22UTC,
1172 pipelines, no deadline/memory guard. Previousaf9e formattedX pipeline1319
created51434ms; window1735 loading arc, window1745 black, no menu/entry.
NewCS9d4c341d4e05f879 PC704: private registered source confirms MUBUFopcode2
BUFFER_LOAD_FORMAT_XYZ IDXEN; actual runtimeformat remains unrecorded.
Resource RED174837 -> same GREEN175338; compute RED175209 -> CPU175420,
six XY/XYZ/XYZW32/64 GPUAV175543 PASS. Twelve rows per width/wave include
aggregate partial/OOB records, conversions/repeated sources/constants/odd
halfwords, address/destination overlap, EXEC/null, entire4096-DWORD backing,
transferred reserved selector faults and ignored untransferred selectors.
No full-wave occupancy or other-game proof. Registered GPUAV180052 PASS10/10;
full resource_tracking/admissions181300 PASS3/3 after correcting old GPU-fallback
and typed metadata expectations. Native retry pending. Initial175110 name-assignment build failure is not semantic RED.
Required independent RED: truly GPU-selected formatted3-component load. Same
GREEN plus XY/XYZ/XYZW32/64 numeric conversion/swizzle/repeatedcomponents,
constants, aggregate transferred-element bounds, null/EXEC/mapped address,
full backing and reserved transferred-selector faults. Preserve typed/D16/
store/atomic rejection; do not implement wide loads as independent partial
reads. Use the existing formatted transfer contract and shared helper.


## GPU-selected formatted buffer load X (2026-10-09, exact blocker confirmed)

Synthetic proof completed locally: resource RED155757 -> GREEN164341;
GPU fixture admission RED163638 -> numeric wave32/64/fault GPUAV171515 PASS.
Actual parser RED161846 -> GREEN162713; real host fault trap scoped RED170719
-> GREEN171049. Registered neighbor GPUAV17240710/10 and admission172520 PASS.
Full backing/28 rows/4 invalid descriptors, exact flags; ADD_TID lane0 only.
Native game retry pending. Typed/D16/wide-formatted/store/atomic expansion and
other-game corpus remain unproved; do not infer them from formatted X proof.

Captured source cs_af9e060c8c9207ba_d656bb5d885ddee5.bin from registered015949
has PC0x230 MUBUF opcode0 BUFFER_LOAD_FORMAT_X, IDXEN, descriptor s16:s19.
Registered capture has runtime_resources_captured=false; format value remains
unproved. The preceding aggregate raw-load error does NOT identify a subword
operation. Required synthetic native RED: formatted32x1 load through a truly
GPU-selected descriptor, actual indirect tracking/no fake host bindings. Then
read-only BDA format conversion/swizzle/default/OOB/numeric oracle and explicit
unsupported runtime format/selector fault propagation. Use AnyPS5 RuntimeBuffer
format dispatch as reference; preserve raw/BDA/ownership/typed/write/atomic
boundaries, full element bounds and mapped-address checks. Runtime formats must
not silently fall back to raw reads or zeros as success. New fault reporting,
if needed, requires GPU fault record and actual host error-path proof. No title/
hash/PC/address branch in production. Read-only source root diagnostics permitted.

## GPU-selected raw subword read (2026-10-09, separate potential gap)

Native8c5/EXE8a9407 original5178 run150855 naturally exited321 at15:31:20UTC,
no deadline/memory guard.1158 completed pipelines/lastCS1324; captured fatal
CSaf9e060c8c9207ba PC0x230 at resource tracking: unproved live GPU descriptor
only admitted rawDWORD or unsigned16. Loading arc observed, no menu/entry.
Required before expanding admission: independent synthetic CPU RED for U8/S8/S16
GPU-selected descriptors, untouched reference payloads; same GREEN + native
GPU/GPUAV values for high bytes/sign boundaries, odd and crossed-DWORD halfwords,
last valid/OOB/null/masked reads, stride/OOB_SELECT0..3/SOFFSET/swizzle/ADD_TID,
actualwave32/64 and whole-backing sentinels. Keep formatted/typed/writes/atomics
rejected. Compare AnyPS5 RuntimeBufferLoad/widenSubdword to AMD raw8/16 ISA,
retain guest BDA mapping checks and bounded lifetime; no guessed resource payload,
physical-pointer shortcut, title/hash/address gate or relaxed device limits.
Exact opcode/descriptor form remains to confirm from capture; aggregate error
alone is not proof all rejected shapes are supported.

## Runtime raw stride compiled-artifact reuse (2026-10-09, completed locally)

Core `8c5beaa1`: native cache RED142849 -> GREEN143351; renderer metadata
RED143036 -> GREEN144025. Actual renderer GPUAV145227 wave32/wave64 numeric
read/store/backing PASS; final registeredGPUAV145614 PASS4/4, six real-cache
negative scenarios. AffectedGPUAV145854 PASS5/5; CPU150125 PASS4/4. Initial
142441 API setup and GPU fixture wave mismatch were corrected before verified
proof; neither is claimed as a semantic RED. Native build/install/retry pending.

Measured normal600s run134839:570 completed pipelines,692 optimizer calls,
optimizer sum185876ms vscompute creation79667ms. Operation sums are not
wall-time partitions. Existing positive rawDWORD stride emits equal SPV8/48,
but ProgramCache compares exact ResourceSpecialization and recompiles before
post-compile module reuse; renderer publishes packed stride from compiled info.
Required independent native REDs before changing production: real cache request
8->48 returns another compiled program object; real RebindBuffers with cached
stride8/current snapshot48 publishes old8. Then same GREEN and real renderer
GPU numeric read/store/unchanged backing,8->48->8/zero/mixed-byte/typed/atomic/
swizzle/ADD_TID/topology boundaries. Reuse requires a compiler-owned proof that
all actual accesses to a slot consume dynamic rawDWORD stride; bounded-table,
scalar, typed/formatted, byte/short and atomic paths stay exact-specialized.
Never ignore unrelated specialization/layout/descriptor ownership changes.

## AnyPS5 complete integer64 comparison family (2026-10-09, completed locally)

Core `6e401f7c`: intended native decoderRED140128 -> CPU GREEN140748;
GPUAV141131 PASS7; final registeredCPU/GPUAV141600 PASS2/2. Corrected input
loadX3->loadX4 before numeric GREEN, unchanged oracle; DPP/DPP8 rejects.

AnyPS5d70b8998 contains all V_CMP/V_CMPX I64/U64 predicates, while Kyty
recognizes only7 of32. AMD RDNA2 VOPC161..167/176..183/224..231/240..247 is
the contract. Required unchanged decoder RED for32 synthetic encodings in both
VOPC/VOP3 forms, then same GREEN. Numeric GPU oracle must use dynamic per-lane
64-bit pairs (signed min/max/-1/0, unsigned high-word/low-word edges), all eight
predicates, full/partial/zero EXEC, preserve VCC on CMPX, preserve SCC, both guest
wave sizes, and existing compare neighbors. Keep64-bit DPP/DPP8 rejection.
No public-list enum badge is execution proof. Original f20/native335 normal
600s retry134839 endeddeadline/graceful0 at13:58:48UTC; no memory guard/fatal,
no verified menu. Source inputs remained frozen through the completed run.

## AnyPS5 scalar SOP1 support gap (2026-10-09, completed locally)

Core/tests `f20f2bd8`: BREV_B64, BCNT0_B32/B64, FF0_B32/B64, SEXT_I8/I16.
AMD RDNA2 scalar arithmetic/bit ISA is the contract; AnyPS5d70b8998 supplies
adaptation reference. Native intended decoder RED124105 -> same CPU GREEN124320;
GPUAV124535 PASS7 numeric cases; final registered CTest124727 PASS2/2.
Independent dynamic-input oracle checks32/64 widths, SCC set/preserve,
first-zero31/32/63/-1, sign boundaries, in-place alias and untouched backing/guards.
Actual guestwave32/wave64, no coerced wave size. Unsupported0x23 still rejects.
Native emulator125056 PASS; EXE33574979 installed. Bounded original eboot5178
smoke125238 ended120s/close/graceful0 at12:54:54UTC, no memory guard/forced cleanup;
black window capture only, no menu/game entry or measured boot benefit. Full
shader_cfg scalar-descriptor EXEC baseline below remains separate/unresolved.
Evidence `_Build/analysis/anyps5-study-20261009/`, `_Build/checks/20261009-124*/`
and `_Build/runs/game-20261009-125238-anyps5-scalar-smoke/`.

## Full shader_cfg scalar descriptor execution-mask baseline (2026-10-09)

Full native020813 fails "scalar descriptor planning retained an unrelated native
execution mask" after SMOV fixture repaired with valid s[48:51] descriptor.
Native scoped production-only reversal021044 reproduces SAME failure with stride
fix absent, preserving original unrelated diagnostics. Production patch restored
and exact file hashes checked. This is separate baseline debt; do not claim full
shader_cfg GREEN from focused checks. Required before a separate correction:
inspect scalar planning/EXEC ownership contract, isolated unchanged minimal RED,
then same GREEN and numeric/descriptor/zero-trip/conditional provenance neighbors.
Raw runtime-stride patch does not suppress the failure.

## Ordinary raw DWORD runtime stride (2026-10-09, regression in progress)

Native8df/EXE98cd warm234326 completed past oldVS1135/PSc428, then reached
CS1244 before3600s deadline. Graceful0/no memory guard/fatal, original5178 restored
00:43:45UTC, compatible846963327B cache persisted. Loading/black only, no menu.
Heavy cooperative family variants repeatedly cost116..140s. Read-only comparisons
show additional true SRT mapping-offset differences; general ID canonicalization
cost32s/module and did NOT establish equivalence. Do not reuse unequal modules.
Existing stride8/48 source lead is independent: ordinary BufferByteAddress embeds
packed_stride while native byte offsets/limits already use current shader data.
Required CPU RED: unchanged synthetic raw DWORD read/store emits one module for
stride8 and48, with dynamic index and exact native binding metadata. Test added to
shaderCfgTests --raw-buffer-stride-module-only, intended native RED012655 -> unchanged GREEN013216. Final CPU021239 PASS5/5.
GPUAV015627 PASS6/6 stride8/48, guest64, ADD_TID/OOB/sentinels; final neighbors021354 PASS6/6
and real renderer021848 PASS2/2. Coredaca7b23/native022643/EXE87a9 installed; actual
normal retry022803 ACTIVE PID33436. No menu/game-entry proof. Then current stride publication/unchanged numeric
GPU/GPUAV read+write, OOB/offset/zero/add-TID/swizzle/typed/atomic boundaries and
native renderer/binding neighbors; preserve all guards and actual resource data.
Other mapping-offset variants remain separate unresolved performance work.

## First-scene pipeline preparation cost (2026-10-08, measured lead)

Actual native8df/EXE98cd retry231226 passed oldVS1135 andPSc428, then completed
988pipelines before1800s launcher deadline. Graceful exit0/no memory guard/fatal;
lastCS1134, window only loading arc/black. Newbed1121/cooperative64 module472189
words/6095labels/386barriers/3functions/249-case switches, pipeline116920ms.
Metadata threads64x1x1/LDS128dwords, no guessed guest payload. Final compatible
806986099B driver cache saved2541ms; intermediate783725618B write110200ms with
G: disk queue. These are measured first-load costs, not a new semantic RED.
Same-source native warm retry234326/3600s+240sclose-grace is next to distinguish
cached load from continuing unsupported runtime work. Resource limits unchanged.
Before any shared performance correction, require independent synthetic cooperative
wave64/helper/LDS dispatcher fixture, linear-size compiler invariants and unchanged
numeric GPU/GPUAV oracle with neighbor/full-wave/zero-work/branch/barrier coverage.
Do not skip shader work, force reuse between different static states, fake resources
or modify OS/GPU timeout policy. Ordinary stride8/48 debt below remains separate.

## Vertex bounded scalar descriptor byte read (2026-10-08)

Completed locallya0e548bc+wave64 test8df60ac2. Native CPU RED102631->GREEN103900;
renderer RED103328 -> final native11020920rejected/13allowed. CPU1119403/3,
GPUAV1050598numeric cases/count/unsigned199/OOB and explicitguest64 test112218
PASS; affectedGPUAV1112135/5 +legacy13-bufferindexed753111536 PASS. Workgroup
axis and unsupported typed/byte-DMA gates remain closed. Static-key diagnostic
103203 reproduces rootPhi0/+1*196/ReadConst32..35, scratch0 vsbadmanifest;104148
tracking29bounded reads, no payload/materialization/GPU proof. Diagnostic removed.
Harness arena supervisor fixed after unrelated105454 commit-pressure failure; no
emulator/guest limits changed. Actual native8df/EXE98cd retry231226 passed old VS1135:127528words,
graphicsVS965/PS9647364ms. Same run oldPSc428297659words, VS937/PS93624ms
and VS937/PS94120ms. Gate passage is proved; runtime still active, white loading
arc/window2319 only, no menu or game entry.


37/EXEd103 normalPerformance100400 natural32110:20:55.431Z, no guards;
original5178 restored10:20:55.658Z. RendererPSa3 gate passed/pipeline4065ms;
oldPSc428 passed/recompiled297659words and graphics7383ms, later work followed.
New fatal vertex1135e3d2715c8ba2 PC1738, private e0202020/80010040 decodes
BUFFER_LOAD_UBYTE(op8) imm32. Four descriptor DWORDs ReadConstBuffer(offset=IMul32)
rooted in GetBufferResource(ReadConst x4); bounded=no. Capture metadata_complete
false and bogus scratch628885250 must not be trusted as compiler inputs.
Required independent native CPU RED: vertex scalar bounded descriptor loop, raw
unsigned-byte use, exact coherent table footprint/count/zero/DMA/writer negatives;
same ISA loop/table proof as compute/pixel, no workgroup-axis admission. Then actual
vertex GPU numeric output/readback/renderer binding and neighboring graphics stage
cross-alias guards. Keep GPU-selected unbounded raw-byte BDA gate closed; no title/
hash/address exception or fabricated capture payload. Diagnose index proof before
stage expansion, and native game retry afterwards. No menu or game entry.


## Pixel immutable SRT renderer binding gate (2026-10-08)

Completed locally37bdd695: intended native RED093304 -> same GREEN093822;
exact upload footprint095104 PASS; final native matrix09525116 rejected/10 allowed
and scalar-selector/write neighbors095550 PASS. Cross-stage preflight checks
buffer/image writers before bindings; compute-own ordered exception cannot skip
another owner's snapshot, pixel ordered overlap remains rejected. Vertex-owned
snapshots/DMA writes/range/count/atomic aliases stay closed. GPUAV094519 allocation
failure was unrelated, not proof; final native tests uninstrumented. Native emulator
and actual game retry pending. Render-target/depth attachment vs snapshot overlap
is separate unproved graphics debt; do not claim exhaustive attachment alias coverage.
Last actual PSa3 compiled; oldc428 passage not proved by this renderer fatal.


Warm a4/EXEfe0d normalPerformance090720 naturally exited321 at09:28:56.055Z,
no lifetime/memory guards; original5178 restored09:28:56.293Z. Compiler passed
prior pixel bounded-loop planning failure and reached renderer descriptors.cpp899:
immutable SRT snapshot requires compute without DMA writes, stage2/dma_write0.
Required synthetic native RED: real renderer PrepareBindings on independent pixel
snapshot, exact immutable range and flattened payload; no acquisition-side failure
accepted as RED. Preserve vertex-owned snapshot rejection and DMA-write guard.
Prove graphics companion vertex/pixel declared writers are checked against every
stage's snapshot BEFORE descriptor/cache acquisition; overlap/disjoint/atomic/
ordered-own-write and resource count/range negatives. Then same GREEN and native
build/original retry. No title/hash/address special case or snapshot bypass.


## Raw ordinary-buffer stride permutation cost (2026-10-08, diagnostic lead)

Same a4/EXEfe0d Performance warm090720 reuses four5490 pipelines in76..83ms, but
new fc6 variant recompiles134496ms. Cold0537 and warm0601 have identical671595
word counts, differ by12 OpIMul operands referencing constants8 vs48. These are
REAL semantic variants, not proven equivalent; do not force cache reuse or ignore
stride. Warm newly encountered5490 variants287/288/290 cost141179/142514/142114ms.
Required before any shared correction: bounded independent raw buffer stride8/48
address/read/write and exact bounds regression; zero stride/OOB modes/add-TID/
alignment/mixed typed/raw/atomic restrictions and renderer metadata publication.
If making stride runtime data, prove unchanged GPU/GPUAV output and valid module
reuse with correct current metadata; preserve guest ABI, host limits and typed
format distinctions. This is performance debt, no production change made here.
Current native game still active/frozen; no menu/entry or issue108 milestone.


## Bounded buffer metadata control-flow expansion (2026-10-08)

Runtime correction08:31: actual heavy5490 module is unchanged681k after2c. Its
860-case switch is cooperative PC dispatcher of outlined helpers, NOT metadata.
Do not cite metadata grouping as fixing that runtime delay. Independent metadata
CPU/GPU proofs remain valid. Performance existing bounded recipe CPU082247 PASS;
actual saved cooperative module082344681618->661988 in1.2s. GPUAV0829136 numeric
cases including cooperative=1 multiwave LDS reduction pass. Next bounded normal
Performance game retry; no performance/menu/game-entry claim from source audit.


Completed locally2c746bb0: metadata classes share labels; actual native offsets and
limits remain dynamically selected. Singleton constants and default unreachable
selectors preserved. Native intended CPU RED074209 -> GREEN074410; final registered
GPUAV074750 PASS7/7. Large515 native numerical neighbors075850 PASS3/3 WITHOUT GPUAV;
combined instrumented075438 timed out120s during compile, not semantic evidence.
Native emulator/runtime retry pending; no game/menu proof and no limit increase.


Normal e498/EXE60629 runs065629 and warm071239 both timed out900s before pixelc428.
Warm5490 compute variants681342/681618/681594words take134397/141030/136994ms;
860-way switch becomes thousands of blocks through cooperative lowering. Cache
checkpoint can take105615ms. No memory guard; originals restored after owned cleanup.
Required independent native CPU RED: four bounded64-entry tables, three stride
classes, different byte bounds/offsets, typed fallback and OOB; bound metadata CFG
by actual semantic classes rather than rows. GPU/GPUAV read+store numeric oracle,
wave32/cooperative64, sparse/native remap/zero-stride/fallback neighbors. Preserve
all valid-selector checks, host descriptor limits, byte limits, volatile/atomic
semantics; no title/hash exceptions. Native intended RED074209 (labels399/words14865) -> unchanged GREEN074410
(labels99/words11690); GPUAV raw/formatted numeric074510 PASS. Final registered
074750 GPUAV PASS7/7 (includes existing full/sparse wave64 count-guard neighbors).
Experimental compact fixture wave64+partial4+barrier was rejected by existing
resource planning at PC38/3c in074931/075139, before metadata emission; keep its
root/EXEC proof as separate unproved debt, not an optimization regression. Its
numeric oracle was never executed. Existing515-row stride/add-TID/formatted
neighbors and original retry are next. No production admission was relaxed.


## Pixel-stage bounded scalar formatted descriptor loop (2026-10-08)

Completed locally e4985763: intended native CPU RED030356 -> unchanged GREEN030943;
readonly SSBO native RED043228/051528 -> GREEN044102/052427. Final numeric pixel
GPUAV060544 count0/1/2/3 sums0/1/3/6 no VUIDs; registered CPU065412 PASS2/2 and
pixel+compute neighbors GPUAV065335 PASS5/5, including writable formatted buffers.
Zero-trip reads none; DMA writer failure leaves snapshot/specialization unchanged.
Compute workgroup-only proof, budgets, alias restrictions and typed GPU-selected
rejections retained. Temporary captured-key/operand diagnostics removed before commit.
Native emulator/runtime retry next; no menu/game entry proof. Original private
capture033203 source translation passed, separate from synthetic regression/runtime.


Normal72be9e34/EXE7af247b1 run015949 naturally ended321 at02:10:19, no guards;
originaleboot5178 restored. Pixelc428 now passes rawDWORD PC3280 and fails PC37f0
BUFFER_LOAD_FORMAT_X. Bounded CPU-only key reconstruction round-trips all captured
static inputs (no runtime payload fabrication/GPU access), then reproduces same
tracking failure in023420. Descriptor DWORDs are ReadConstBuffer(offset=Phi*196)
with a0/+1 induction and SRT-backed four-word table root, not a known format
literal. ProveBoundedSrtRead/PlanBoundedReads/MaterializeBoundedReads only admit
compute. Keep formatted GPU-selected BDA gate closed; first prove an independently
bounded pixel scalar loop can retain/materialize its real typed table and exact
coherent read footprint, with count/zero/OOB/unsafe-root/writer negatives and native
formatted pixel readback. Reuse established dominance/budget/snapshot machinery;
workgroup-axis proof stays compute-only. No unbounded enum guesses or fake resources.
Temporary diagnostics: analysis/pixel-formatted-probe-20261008-023237-8ad128,
ShaderBatchAudit.cpp key replay + ResourceTracking.cpp bounded operand graph logger;
not a fix/semantic RED. Preserve originals and remove or isolate before committing.

## GPU-selected raw DWORD descriptor load in pixel stage (2026-10-08)

Completed locally72be9e34: original GPUAV RED014248 -> unchanged GREEN014532;
registered DWORD/ushort + admission guards014931 PASS2/2; vectors and related
address/snapshot/stride/format neighbors015237 PASS. Existing BDA emitter unchanged.
Native emulator015646 PASS, install/retry next. No new push/game-entry proof.

Same native numerical fixture RED014248 -> unchanged GPUAV GREEN014532. Existing
one-component BDA emitter is unchanged; shared raw32 admission now includesDWORDx1.
All19 rows (four OOB modes, stride/offset/soffset/unaligned crossing/last payload/
invalid descriptor/inactive lane/swizzle and sentinels) pass. Formatted/typed/
signed16/write/atomic gates remain closed; registered guard+ushort/vector neighbors
and original runtime retry are next. Core fix remains local/unpublished.

Nativeec26997d+diagnostics/EXEa0b99068 normal480x270 run012532 naturally ended321
at01:36:39.530581 UTC, no900s/memory guard; originaleboot5178 restored. Old packedCB
PSe82 now compiled to39346 words and graphics VS918/PS917 completed1510ms. This
closes that admission/pipeline blocker on the smaller profile; both visual captures
remain black, no menu/game entry proof. Prior CS8457 id294/220047words completed
367727ms. Next fatal: pixelc428451cf6d96d04 PC3280 GPU-selected buffer descriptor.
Private raw0xe0302024/0x80000502 decodes MUBUF BUFFER_LOAD_DWORD(op0xc), immediate36.
Metadata+37056B capture under runs/game-20261008-012532-packed-cb-capture/shaders/dispatched/.

Required independent unchanged native GPU/GPUAV raw single-DWORD fixture: GPU
selected row through load/readfirstlane/descriptor fetch, stride/index/offset/soffset,
OOB modes0..3, complete last DWORD/OOB tail, unaligned cross-DWORD bytes, swizzle,
invalid descriptor, inactive lane, output/guest sentinels. Existing scalar indirect
BDA emitter already accepts one component; admission only allows unsigned16 and
DWORDx2/x3/x4. First intended RED before extending the raw32 classifier; preserve
formatted/typed/signed16/write/atomic restrictions and existing ushort/vector cases.
Then native build and bounded normal game retry; no title/hash/address behavior.

## Packed normalized color export (2026-10-08)

Completed locally ec26997d: GPUAV same positive cases GREEN010603; expanded
producer/unsupported-state and neighboring float/alpha/load/store011934 PASS5/5.
Expected partial-export refusal011250; CPU admission011712 PASS after replacing
old unproved-CB footprint0 expectation with the now-tested four-byte footprint.
Native emulator012319 PASS; install/normal runtime retry next. Blend, truncation,
partial-component exports/write masks, multisample and non-FP32 source remain
explicit errors; fully masked targets require no conversion. No game/menu proof.

Corrected independent channel-order oracle uses declared Agbr mapping3/1/0;
final intended native admission RED005850 -> unchanged numeric GPUAV GREEN010603.
Both widths/four orders/41 single-target samples+mixedMRT2/cache identity and real
shared CB/texture backing/download/sentinel pass. Exact positive FP32 half-LSB
packing uses a two-U32 significand product, avoiding intermediate FP32 rounding;
buffer/image RNE remains default. Producer/unsupported-state neighbors and original
normal game retry remain pending. No title/hash/address behavior, push or menu proof.
First005252 fixture build error and010313 API-overload build error are not REDs.

Normal6f5c15bf+diagnostics/EXE991acf8a run002103 naturally ended321 at00:32:38,
no900s/memory guard; originaleboot5178 restored. CS8457 creation completed383090ms
and expensive work was checkpointed; final runtime blocker is unchanged packedCB
layout6/type0/order0, slot2, PSe82, full decoded MRT mask7f, mode9 FP32, unblended
single-sample480x270 surface. This supersedes the unknown-phase stall lead: no
DCE causality or speed claim. Captures/logs under runs/game-20261008-002103-cache-budget-capture.

Required independent synthetic CPU admission + native GPU color-export/readback
RED/GREEN, both packed widths, four orders, zero/max/unequal/half-LSB/clamp/NaN/Inf,
mixed float MRT0 + packed MRT2, distinct shader permutation and real shared
render-target/texture raw backing/upload/download/adjacent guest sentinel.
Keep blending/partial-component exports and unsupported rounding explicit.
CB ROUND_BY_HALF differs from buffer/image RNE; do not reuse their packer without
an explicit rounding policy. Preserve actual UNorm numeric interpretation; no
float substitution. AMD primary GCN3 data/MRT table and CB conversion definitions:
https://gpuopen.com/download/AMD_GCN3_Instruction_Set_Architecture_rev1.1.pdf
https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/programmer-references/R6xx_3D_Registers.pdf
New input metadata fields are initially unused test seams, not production support.

## Slow driver-cache checkpoint I/O on the GPU command lane (2026-10-08)

Native unchanged policy fixture RED001418 -> GREEN001548. Cheap1000-creation
burst now retains pending work after one save; eventual cooldown/long compilation/
initial/cheap/no-work/uint64 boundaries pass. Source correction measures actual
checkpoint cost, budgets intermediate I/O, saturates pending count and retains
pending work on save failure. Original SaveDriverCacheLocked and unconditional
shutdown Save remain unchanged. Cache identity/validation-mode/SPIR-V-reuse/checkpoint neighbors001730 PASS4/4.
Local fix6f5c15bf committed separately; native build001939 PASS. Normal bounded
retry with independent pipeline/cache timing is next; no push or game/menu proof.

Native1a795a82 + preserved diagnostics + temporary cache timing logger,
EXEd8d972d2, normal asynchronous480x270 run000848 confirms a633936331B
checkpoint took26500ms: hash173ms, write26414ms, flush26458ms, rename26500ms.
An earlier write took2046ms; unchanged probes172..183ms. Optional host persistence
currently stalls the command lane every16 creations or after an expensive creation.
The180s probe reached shown190/pstg3, then owned guard/forcedcleanup; no menu proof.
Original eboot5178 restored. Artifacts: runs/game-20261008-000848-cache-io-capture
and analysis/yotei-cache-io-20261008-000824-127810. This is a measured contributor,
not proof it explains the entire previous900s runtime stall.

Required independent native CPU RED/GREEN for checkpoint cadence using measured
save cost and elapsed time: a simulated burst of cheap pipeline creations must
not pay repeated26s writes, deferred work must eventually checkpoint, initial/
cheap checkpoints and valuable long compilation must remain eligible, no work
must remain false, and uint64 time boundaries must not wrap. Preserve unchanged
unconditional shutdown Save, write/flush/rename/hash/driver/validation identity and the16/5s
eligibility triggers. Apply cost budget only to intermediate persistence, then
cache identity/validation neighbors, native build/install and bounded normal retry.
Keep pipeline/driver/GPU phase diagnosis separate; no DCE rollback or guest workaround.

## Native indirect offset ownership (latest main420bb51d, 2026-10-07)

Candidate integration deferred: independent native GPUAV RED230803 at auto draw;
the unmodified upstream fetch_embedded predicate candidate still failed230923.
Do not publish it as a fix. Branch SGPR/fetch contracts need separate diagnosis;
restore old production/test pair, retain generic fixture patch outside tracked
source in pr497-conflicts analysis. No failing test removed from the branch's
existing harness; the new upstream-dependent fixture is not yet integrated.


Main advanced during PR497 validation; native indirect fetch programs consume
base-vertex/instance offsets through their guest SGPRs. The branch still supplies
the same offsets to Vulkan, potentially applying them twice. Import main420's
independent synthetic indexed/auto native raster fixture first; capture intended
RED before adopting its decoded fetch_embedded/offset_source-based correction.
Preserve rewritten-fetch/Vulkan offset and ordinary draw-state neighbors, with
GPU pixel readback unchanged. No title/hash/address gate. AudioOut2/AMPR changes
from the same new base require their existing targeted native checks.


## Bounded stall after DCE integration (2026-10-07 22:50 UTC)

Native33a115c9+preserved diagnostics/EXE555d game223527 exceeded900s at480x270,
owned cleanup/native-1; no28GiB guard. Last observed shown190, black capture,
cache630040201B, originaleboot5178 restored, no process remains. Exact pending
shader/pipeline/phase is unknown; no diagnostic hash or fatal context captured.
Earlier7730 retries passed storage admission and reached packedCB layout6/type0
at480x270 and3840x2160. Latest advancement regressed, cause not isolated:
source/SPIR-V/cache differ, so do not claim DCE causality or speed improvement.

Required first read-only/bounded capture of phase/hash/IR/resource shape, then
independent CPU compiler/IR/resource-plan or scheduling regression matching that
cause. Preserve expected numerical/readback behavior and existing liveness roots;
no speculative DCE rollback, guessed timeouts/limits or GPU reset loops. A host
compiler-progress invariant may be needed before any further potentially hanging
GPU reproduction. Menu/game-entry/gameplay remain unproved.


## Paired DCE integration after PR497 CI read-back (2026-10-07 22:16 UTC)

Completed: native same-fixture RED221644 -> GREEN222227; affected resources/CFG/
GPUAV222605/222626/222639/222726 pass, native build223317. Public commits1511939a/
e4c255df and exact-head CI37696776012 PASS all three platforms. Main branch IR
layout/F64 retained. Existing sampled-depth disassembly expectation222832 remains
old debt, corroborated September27 evidence; it never reached numerical execution.


Publishedd585 Windows/Linux CI failed the old scalar-buffer OOB planning oracle;
production+the October2 clean-scalar regression already require known OOB zero
without a memory probe. Update this stale expectation and retain an unreadable
in-bounds negative/control; no production scalar-read change. macOS compile
failed mixed uintptr_t/uint64_t initializer-list deduction in the depth fixture;
use an explicit uint64_t array without changing addresses or numerical oracles.

After the OOB fixture repair, unchanged main-provided TestDeadPhiCyclesAndPlanningRoots
is intended native CPU RED221644: old branch DCE retains a dead Phi/increment SCC.
Port upstream95c89229's liveness traversal and its small live marker while preserving
branch Value/F64/storage abstractions. Same unchanged GREEN must retain direct
Identity edges, live recurrence and external planning roots, remove dead cycles,
and clear old marks on a second pass. Then affected scalar/resource/CFG/GPU checks,
native build and bounded game retry. Keep the core patch separate from test-only
portability/oracle changes. No removal of failing tests or broad architecture replacement.


## Direct packed UNorm raw32 storage writes (2026-10-07 21:29 UTC)

Native d585b0c9 + preserved diagnostics + packed-use error detail, EXE4a7c792f,
480x270 bounded game212925 ended naturally/native321 after17.27s, no guards.
Exact use: image/root24, source31, PC0x4328, format30, storage=true, atomic=false,
compare=false, root_sampled=false, width mismatch0, indirect=false. The preserved
same-hash capture decodes IMAGE_STORE/Dim2D/dmask7/raw32 at that PC. Original
eboot5178 restored; no owned native process remains.

Completed locally in7730fd11: shared formatted-component packing for raw32
normalized storage writes, fixed4-byte native backing. Same numerical GPUAV
RED213801 -> unchanged GREEN214014; expanded stores/loads/packed-float/scaled/
selector214532 PASS5/5. Real storage binding/upload/download and sentinels pass.
CPU resource tracking/admission214403 PASS; atomic/compare/sample/D16 transactional
rejections retained. Original game retry and original-resolution confirmation
remain pending; CB support is unchanged. No push of this follow-up fix yet.

Intended RED confirmed on unchanged production admission at
`_Build/checks/20261007-213801-4632464-shader_recompiler_compute_tests/`: genuine
format30/IMAGE_STORE rejected by raw-load-only gate, not a build/input failure.
The numerical fixture and real-storage-backing checks remain unchanged for GREEN.

Required independent unchanged native GPU RED/GREEN: both11/11/10 and10/11/11
layouts, exact packed bytes, zero/max/quarter/half/three-quarter, clamping, NaN/Inf,
one-LSB, inverse swizzle, sparse DMASK and nonwritten texel sentinels. Real renderer
storage binding/upload/download must retain4-byte backing and adjacent guest bytes.
Reuse the established formatted-component packer; keep sample/atomic/compare/D16/CB
admission closed. RDNA2 ISA section8.2.4 specifies entire-element writes: omitted
components become zero, rather than retaining old fields. This corrects the older
prospective partial-component-preservation requirement; use that primary contract
in the new sparse-mask oracle. RDNA2 reference retained in
`_Build/analysis/rdna2-isa-budget.pdf`; normalized conversion contract:
https://github.khronos.org/Vulkan-Site/spec/latest/chapters/fundamentals.html


## CPU-only unmap must not wait for unrelated GPU work (2026-10-07)

The merge candidate retained unconditional scheduler Finish in RenderContext::UnmapMemory.
Independent native GPUAV `gpu_command_lane` RED at
`_Build/checks/20261007-211522-8046356-shader_recompiler_compute_tests/`
failed specifically at `CPU-only unmap with pending host retirement` after the
fixture released its bounded timeline gate and drained owned threads. No driver reset.
The unregistered CPU range must retire without submitting/waiting for unrelated
GPU commands. Buffer/image ownership or pending priority callbacks still requires
the scheduler drain. Preserve the test unchanged; admit upstream's cache-ownership
query with the branch's image model, then rerun this same regression and neighbors.


Same unchanged native GPUAV `gpu_command_lane` GREEN passed at
`_Build/checks/20261007-211806-3027197-shader_recompiler_compute_tests/`.
The cache-ownership query and pending-priority guard are upstream's shared
mechanism; the branch image model is retained. Adjacent scheduler/ring/draw-offset
checks previously passed; no title or address-specific behavior.



## Reachable packed UNorm use in compute e519 (2026-10-06 21:26 UTC)

Native59c26be4+preserved diagnostics/EXEd65295f6 bounded480x270 rungame211642
ended naturally21:16:57.504546 UTC/native321, no guards/forcedcleanup. Original
ebootSHA5178 restored and checked, no owned process remains. It stops earlier
than the old CB error: compute e519fa9713f7b8e3 fails the new raw32-only UNorm
gate. A descriptor previously became null due to missing known-format metadata.
Runtime advancement is regressed; do not claim improved boot. Exact gate reason
(sample,storage,compare,atomic,D16), format/root/usePC require read-only diagnosis.
Existing223637 dispatched compute_e519fa9713f7b8e3_7b111caaa5beb65a has18352B code,
but lacks needs_lds_barriers; direct CPU audit is input_error, not semantic RED.

Required independent next regression before extension: actual reachable operation,
exact packed bits/normalized values and guest/output sentinels, zero/maxima/one-LSB,
distinct RGB/swizzle and operation boundaries. For storage prove rounding/clamp,
partial-component preservation and NaN handling; for sampling prove requested
filtering/border/LOD behavior rather than force nearest. Keep D16 and CB distinct.
Consider useful portions of PR1115 only after semantic RED/GREEN; no title/hash/
address behavior or fake descriptors. Then bounded lowres retry and original
resolution when closing a corrected runtime blocker. Existing CB remains pending.

## Eliminated image instruction remapping, upstream PR #1112 (2026-10-06)

Candidate head9c324c6c, semantic commit31fb7208 (format-only f0cb8162 excluded).
Imported its independent compute unused-load and pixel live-sample/dead-load
fixtures before behavior changes. Native shader_cfg_tests intended RED204919
hits ImageRemap index>=source_count on a removed instruction's stale metadata;
source d982a98c plus test/previous preserved diagnostics, no GPU execution.
Required same GREEN, retained live image binding/valid SPIR-V and nearby affected
resource tracking/packed-image tests. Preserve guards for actual live instructions.
After resolving the cherry-pick conflicts, the unchanged PR fixtures pass native
CPU205633. Merge preserves our copy/first buffer remap and gather tests. A second
independent IR regression checks live normalized raw32 plus removed D16 metadata:
our width gate must use actual live image accesses, not stale memory_info entries.
Completed local e7ba687f: intended RED210004 -> same GREEN210247; final full
resource_tracking/admission210953 PASS. PR1112 integrated31e54af9, final registered
unused-image CPU211052 PASS. Fallback without liveness metadata remains conservative.
Local integration authorized; no push, no upstream merge or issue108 publication.

## Packed normalized image load prerequisite (2026-10-06)

Before enabling a packed normalized render target, prove its storage interpretation
independently. New synthetic native IMAGE_LOAD fixtures for both 11/11/10 and
10/11/11 UNorm widths: zero/maxima/one-LSB/unequal fields, actual f32 register
bits, RGB/default alpha and explicit swizzle constants; same-width raw R32Uint
host surface. Fixed-point oracle is field/(2^width-1), not packed-float decoding.
Required intended RED is existing materialization's unsupported format30 rejection,
then unchanged numerical GREEN and existing packed integer/float neighbors.
Diagnosis refined before production edits: the native fixture's valid format30
descriptor is instead erased by ValidImageDescriptor/IsKnownFormat and becomes
a null descriptor (format0). Intended CPU RED202110 fails the unchanged descriptor
preservation invariant expected30/actual0. Earlier201803 bounded readback shows
raw packed words, but that run did not retain a valid typed descriptor and is not
used as numeric semantics proof. First201723 build was aborted after noticing a
wrong selector; only task-owned Ninja/clang were stopped. No reproduction claim.
Keep filtering, storage writes and CB admission unsupported until separately proved;
no forced nearest filtering for normalized sampling. No game/title/address fixture.
R6xx/R7xx primary NUMBER_TYPE-ignore rule is generation-specific and cannot alone
justify changing PS5 CB semantics. Completed local d982a98c: scoped metadata RED202738 -> unchanged numerical
GPU/GPUAV GREEN. Final211156 covers both widths/all four fixtures, real raw
renderer backing/download/sentinel and packed float/scaled filtering neighbors.
CPU210953 checks D16/sample/store/dead-metadata boundaries. CB stays closed;
runtime now exposes the earlier e519 use above, no boot improvement claimed.

## Render-target packed 11/11/10 format with numeric selector zero (2026-10-06)

20:01 UTC update: bounded diagnostic 480x270 retry game-20261006-195253
reproduces the same layout6/type0/order0 error in196.132s, native321/no guards.
EXEBED81987, original game SHA5178 restored and independently checked after
termination. Actual failing surface480x270, single sample/fragment, blend off,
clamp on, bypass off. First eight indirect source observations are identical
CPU/backing/clean-read0x8018; no stale backing evidence within this sample.
No normalized-to-float substitution, partial-attachment drop or production fix.
Native exact pixel capture audit195721 PASS through decoder/CFG only (971
instructions,44 structured blocks); all41 pre-structured blocks reachable.
Block39 includes MRT2 EXP atPC0x15f8/en0xf along with MRT0..6; it cannot be
excluded merely by absent static export. Runtime branch execution and legal
packed-UNorm raster semantics still unproved; semantic RED remains pending.
Artifacts `_Build/analysis/low-resolution-probe-20261006-195227-e351f8/`
and `_Build/checks/20261006-195721-8277234-shader_cfg_tests/`.
Use the authorized smaller profile for quick retries, then original resolution
for closing a corrected runtime blocker; different cache warmth is not a speed
benchmark. Buffer pack/unpack support alone does not prove CB numeric semantics.

Resume evidence 18:55 UTC: last normal run game-20261006-052933 ended naturally
05:39:19.182665 UTC/exit321, no guard. Installed EXE3e3513c2 matches run.json;
no active native game/build/harness found. Diagnostic raw indirect register
CB_COLOR2_INFO=0x00008018 at0x33a; PS e82bdf234f518f8c/6400B has decoded MRT
export union0x7f, including slot2. Existing registered manifest/binary is
`_Build/runs/yotei-integrated-20261005-223637-vertex-access-capture-noval/shaders/registered/ps_e82bdf234f518f8c_556489d9735846dc.json`.
Thus absent-export fallback is not justified by current evidence. Union of
decoded EXP instructions does not establish actual conditional execution.
Next distinguish legal numeric format from wrong command/state provenance;
the existing semantic RED requirement below stays pending. No core behavior
change, build, test or repeated game launch performed during this diagnosis.

Native08dfdade/b361 normal original rungame041213 ended04:29:10.472UTC/native321,
no guards/forcedcleanup, owned32080/supervisor40776 absent and streams finalized.
It passes earlier sampler/image/FMASK blockers, then TextureGetRenderTargetFormat
rejects layout6/type0/order0. Current enum layout k11_11_10, type kUNorm; guest
ResolveRenderTargetFormat yields k11_11_10UNorm (30), host format and size table
support only packedUInt (34) and packedFloat (36). Corecache615354867B preserved.
Spinner visuallyseen04:23:51/shown256, no currentmenu/gameentry/playability proof.

Read-only diagnosis first: actual CB operation and numeric-format contract,
active/render versus metadata/clear carrier, raw register parsing and channel
ordering. Both direct/indirect PM4 parser currently decode NUMBER_TYPE8..10.
AMD primary register documents say packed11/11/10 CB formats are float-only, but
this does not yet prove that numeric selector0 may be ignored; avoid guessing
float conversion, fabricated normalized values or dropping required attachments.

Required independent native CPU intended RED before behavior correction: legal
register/API encoding with independently specified exact format/bytes/channel
mapping and pixel packing/interpretation, same GREEN. Preserve unsupportedformat
and numeric/order cases. NativeGPU render-export/readback and sampled/storage
roundtrip with distinguishable components, orders and boundary values plus
nearest existing packed target cases, then original bounded normal game retry.
If actual use is inactive or metadata-only, reproduce that shared resource-
planning contract rather than admit unsupported raster numeric semantics.
Current production unchanged for this blocker; no runtime/Q/game claim.

## Coherent scalar selector domains for inline resources (2026-10-06)

Diagnostic-only native896ca780+delta/EXE4e2890 normalrun032325 ended03:33:15.107UTC/
native321, no guards/forcedcleanup, own12336/supervisor41476 absent. Exact failure:
compute34be6ffcc212383c image250/root8/pc0648, storagefalse/comparefalse,
root_sampledtrue, FMASKformat159, firstkey0x113a8=70568. With stride872/offset204,
this points to row81+140 (sampler-word bytes), not a normal record-start image.
CapturedIR confirms selector raw U32 scalar-buffer word at an offset shifted by2;
no proven value bound. Do not simply admit indirectFMASKload or sampledFMASK.

Required independent CPU RED before behavior edits: synthetic readable readonly
index buffer with only ordinary record IDs, inline material table includes valid
ordinary records plus a valid but unreachable sampled-FMASK descriptor at a
wrapped interior offset. Materialization must use a proved coherent selector
snapshot to preserve exact reachable ordinary descriptors, and retain full
wrapped-U32 behavior when no such proof/callback exists. Readonly/index-domain
capture must track immutable ranges and enforce all existing buffer and actual
padded-image writer-alias guards; raw address writes or unavailable/dirty data
cannot be treated as trusted. Never drop a reachable unsupported FMASK descriptor.

Neighbors: input includes huge U32 value that really wraps to FMASK key32 -> same
unsupported error; missing selector data -> conservative fallback/reject, no
partial commit; duplicate IDs/out-of-range/partial word/zero default; aliased
shader buffer writers and actual storage-image writers rejected, no bound or
coherence suppression. The native renderer currently also skips alias checks when
older bounded coefficient reads precede writes; new selector snapshots must not
inherit that unrelated proof. Required native host admission RED with selector
marker plus coefficient-before-write flag and an actual padded image/buffer overlap;
unchanged GREEN after guarding that bypass, legacy ordered/disjoint neighbors.
Reuse existing64MiB snapshot work budget and finite probe limits, avoid unbounded scans or whole-device allocations. Mechanism now completed
in local08dfdadeaa6b3d90b9e9fda0b93e649d23d8464f: optional compute coherent selector
snapshots, exact U32-wrapped keys, protected source ranges, conservative fallback.
CPU intendedRED034102 -> unchangedGREEN035130; expanded035655 input/alias/partial/
work-bound/real-wrapped-FMASK casesPASS. Finalresource_tracking0407324.17sPASS.
NativeGPUAV numeric/readback035828PASS; native actual paddedimage/buffer writer
admission intendedRED040225 -> unchangedGREEN040520, legacyordered/disjointPASS.
Finalregistered selectors040847 .50+1.49sPASS. _Build/checks/ artifacts. Native
build/install/current game retry still needed; actual input extent/coherence and
runtimepass remain unproved, so the runtimeblocker staysopen. No playableclaim.

## Indirect FMASK candidate semantics (2026-10-06; new normal-run blocker)

Native896ca780/SHAffc81f normal original-resolution rungame-025652 ended
03:06:59.227UTC/native321, no timeout/memory guard/forcedcleanup; own36488 and
supervisor35268 absent, streams finalized. It passes prior sampler33 and combined
image513 checks, then compute34be6ffcc212383c rejects "FMASK requires a direct image
load". Spinner captured03:00:28/shown233, no menu/gameentry. Cachepayload613123768B
preserved. Normal Release/originalgameSHA5178/redzone/no diagnostics unchanged.

Read-only diagnosis required first: determine whether the FMASK candidate belongs
to a legitimate IMAGE_LOAD root or an over-approximated IMAGE_SAMPLE table. Existing
check rejects storage, compare, indirect and sampled use together; current sparse
error does not distinguish them. Do not assume indirect alone or silence illegal
sampling. Identify root/use PC and format/selector provenance via existing local
capture or bounded diagnostic error detail if needed, preserve proprietary data.

Required independent semantic CPU RED before behavior changes: bounded table with
FMASK identity sample-to-fragment mapping and ordinary image candidates, observable
selector and EXEC result, supported raw32 IMAGE_LOAD only. Keep sampling/storage/
compare/unsupported widths rejected. Derive guest/hardware FMASK load behavior and
host sample layout contract from primary documentation; do not invent zero data
or flatten nonidentity sample storage. If actual failure is sampled/over-approximated
FMASK instead, reproduce that shared domain-planning problem independently first.
Then unchanged GREEN, nearest direct FMASK/heterogeneous/indirect load cases and
bounded native GPU exact readback before rebuilding and retrying original game.
Current FMASK production behavior unchanged. Playable outcome still pending.

## Combined native image candidates beyond logical capacity (2026-10-06)

Native07bea78d/SHA8626 normal original-resolution retry022841 exited321 at
02:39:29.981UTC, no timeout/28GiB guard/forcedcleanup; owned49428/supervisor51068
absent and both streams finalized. It passes the earlier sampler33 error and
compute34be6ffcc212383c instead rejects combined image513 against compiler512:
size71504/stride872/probes8921/current table123 candidates. Durable core payload
610628405B retained. Loading spinner visually confirmed atshown255; no gameentry.

Required independent native CPU intended RED before mechanism edits: multiple
bounded synthetic tables each within512, combined native descriptors beyond512,
exact original image values and per-root selector mapping. Keep logical guest
image admission and per-table work bounds separate from combined native stage
image operands. Check lower host budget rejection is transactional, fmask compaction
and ordinary/sampled/storage/mip accounting remain correct. If using host-derived
aggregate image ceiling, derive it from sampled+storage typed stage/layout limits
and stage resources; final layout still accounts other resource classes, shared
stages and storage mips. Avoid a global constant increase, fabricated descriptors,
rewritten addresses, pair collapse without semantic proof or stage skipping.

Synthetic mechanism proof completed in local896ca780cefa39ef668f48f6ac35e2dc374e2961:
native CPU intended RED024406 -> unchanged GREEN024522; budgets513/512/1/0
transactional, all514 descriptor values and root mappings, native binding count.
First test build024339 failed from a fixture enum typo and is not RED evidence.
Final resource_tracking025128 PASS4.13s; descriptor_budget025255 PASS with typed
limits, storage mips, auxiliary descriptors, zero and overflow checks. Native
GPUAV combined514 test025015 PASS8.39s: independent first/last float values,
explicit out-of-range selector leaves sentinels and neighboring guard intact.
Initial GPU fixture024704 failed at preserved per-table work bound before GPU
execution; bounded256 selector fixture corrected, numeric oracle unchanged.
Shared inline GPU025326 and FMASK/integer load GPU025414 PASS. Artifacts under
_Build/checks/. Core keeps logical512/per-table512/probe budgets, derives aggregate
native stage image ceiling from actual device limits, and uses sized ImageRemap;
final native layout still checks typed families, mips and other descriptors.
Native build/install/original normal game retry pending; runtime blocker remains
open until it passes there. No other-game GPU corpus or playable runtime proof.

## Inline sampled-pair native sampler capacity (2026-10-06; current original-resolution blocker)

Stable background normal rungame-20261006-011540/nativeB4/SHA87f24 ended
01:53:34.472UTC/native321, bothstreamfiles finalized. No timeout/memoryguard or
forcedcleanup; sourceoriginal5178/--redzone/noDebug unchanged. No repeat guest
DIVexception. Compute34be6ffcc212383c pc05e8 materialization rejects sampler33
(size71504, stride872, probes8921, pairs172) against ShaderInfo::MaxSamplers32.
Corecache lastcheckpoint609930772B preserved. Actualgameentry still unproved.

Required minimal independent native CPU RED BEFORE core edits: reusable inline
image+sampler table with33 distinct genuine sampler descriptors (observable root
and candidate mapping), hostbudget allowing33 versus32. Preserve conservative
logical guestlimit32 separately from materialized native descriptor expansion.
Current BuildSamplerPlan also has fixed32 mapping/binding/usage arrays and may
clone samplerclasses, so raising only the first guard is insufficient. Derive
per-stage/set sampler descriptor budget and totalresource/accounting from actual
Vulkan limits, include samplerclass clones, and retain transactional reject when
budget32 or finaltype expansion exceeds budget. Do not increaseglobalguest limits
blindly or collapse different filtering/depth/coordinate semantics to pass.

After intended CPU RED/unchanged GREEN, nearest existing inline sampler/pair/class
neighbors and available synthetic native GPU sampled image readback across first,
32nd/33rd slots, then build/install and retry originalnormalgame. No game/hash/
address code, fake/nullsampler substitutions, stage skipping or bound suppression.
Exact shader capture is absent in the no-debug run. Generic synthetic proof now
completed in local fix 07bea78d: native CPU intended RED021845 -> unchanged
GREEN021949, expanded transactional/class neighbors022203, full resource_tracking
4.11s022418 and descriptor_budget .03s022513 PASS. Native GPUAV33 sampler readback,
null fallback and ordinary neighbor022238 PASS; final registered CTest022548
.71s PASS; integer/packed/float sampling022618 PASS. Actual device native sampler
ceiling1048576, logical guest cap32 preserved. Artifacts under _Build/checks/.
Normal original game retry remains required before closing the runtime blocker.
No other-game GPU corpus or gameplay proof. No title/hash/address bypass.

## Original-resolution no-debug integer divide exception (2026-10-06)

Native retry234419/localb4eae640/EXE87f24 ended00:01:29.156UTC with
0xC0000094 (integer divide-by-zero), no guard/timeout/forcedkill. Originalgame
SHA5178 restored; sharp Medium menu visuallyconfirmed atshown296-313,
WhiteCrosshint shown308. Owned60s PostMessageCross23:59:05-00:00:05 released,
helper0;00:00:35 stillMedium, no Standard/Quality/gameentry confirmation.
ForegroundSendInput attempt afterexit withheld before any input. Both streams
finalized, owned44484 absent. Corecache loaded542376395B, checkpoint547858142B.
No game/shader dump/debugger/profiler or validation in this normalgame launch.

Next read task-specific Windows crash event/report to locate host versus guest
fault. Required independent regression before production changes: identify exact
shared operation and legal zero/empty boundary from ISA/API/ABI, then minimal
CPU fixture that produces the same meaningful divide error/result on current
production. Keep unsupported errors and avoid generic exception masking/fakezero
results. Native build/semantic RED-GREEN only after path identified. Do not replay
identical crashed game repeatedly or attribute it to the recent reserve fix/input
without evidence. Missing crash context must be an explicit limit.
Windows event1000/PID44484 at00:01:16.802 and existing WER dump locate the
fault at guestmain0x90113378e (base0x900000000): unsignedDIV32 [rsp-0x74],
EDX0/EAXffff087d, remainder indexes a pointertable. Thus zero divisor is implied
(no unsigned quotient overflow withEDX0), but stackDWORD/its producer is not
captured. This is guestcode fault, not a symbolized hostcapacity function. No
red-zone/empty-hash/API/coherence cause yet; require source/ABI evidence before
fixing. Ignored _Build/analysis/workgroup-capacity-20261006/crash-context.json;
OS dump remains local, no live attach or replicated full memory in shared notes.

Read-only performance lead: bounded workgroup snapshots call clean-memory reader
perDWORD; native coherent callback does texture/buffer ownership queries and
backing map locks perword. GPU dirty readback already widens to512KiB, so repeated
GPU downloads are NOT proved. A future batching change needs exact same source
words/coherence/alias/unmapped/wrapped/transactional guarantees and independent
callback/work-budget regression; actual contribution to current slowframes is
unmeasured. No speculative batching code introduced in this retry.

Read-only follow-up00:19: original guestfunction explicitly tests initial divisor
forzero after storing it atRSP-0x74, then performs faultable external accesses with
that local live; leafuses full128-byte redzone. Prior normal launcher omitted
--redzone (defaultfalse), bypassing existing Windows protection. Native existing
--red-zone-patcher-only PASS00:17:12/EXE11C2D122/.17s; fixture proves disabled
corruption/protected exactsentinel against modeled Windows fault. No production
edit. Next bounded original-resolution run001807 enables --redzone with debugger,
validation, shaderdump/profiler/readback off; owned44940 active. This is testing
an existing correctness setting, not confirmed gamecrashfix. Keep causal link
pending actual previousstage and use foreground keyboardinput only after prompt.

## Original-resolution workgroup coefficient snapshot capacity (2026-10-05)

Native no-debug original-resolution retry233030/sourcea025/EXEa1e03 exited321
at23:30:41.706UTC after loading542344503B driver cache. No timeout/memoryguard
or forced kill; streams complete, owned19352 absent. Restored original game
SHA5178cf80, diagnostic480x270 preserved separately. New failure is shared
MaterializeBoundedReads: workgroup-axis count130560 exceeds stable reserve65536
for compute5be616. No high-resolution rendered frame/Quality/game entry proved.

Required independent native CPU regression BEFORE production edits: synthetic
WorkgroupId-indexed scalar coefficient table with >65536 launched x groups,
exact first/last words and unused-tail sentinels; multiple columns/shared reads,
zero dispatch, unreadable source, axis/count validity and transactional rejection
beyond existing64MiB snapshot budget. WorkgroupId is32-bit with dispatch domain,
not a16-bit descriptor selector (Vulkan interfaces WorkgroupId contract).
Use size-tiered stable reserve derived from actual launch and existing storage
budget, preserving old small-grid layout and within-tier pipeline identity;
keep selector count65536 and write-alias/clean-reader/address guards separate.
Do not increase global descriptor budgets or suppress errors. Existing32769
workgroup-column rejection expectation is the former software ceiling, not ISA;
update it only with independent semantic evidence and retain true budget reject.
After intended RED/unchanged GREEN, numeric nativeGPU snapshot readback above
old bound, focused existing workgroup/coefficient neighbors, then build/install
and retry original resolution without debug. No concurrent execution slots.

Native CPU intendedRED23:34:30/EXE1240BE0E; identical original fixture
GREEN23:35:21/EXEF2650E2F. Expanded CPU casesPASS23:37:35/EXE4131DDE3;
actual per-index data, tier boundaries65537/100000/131072/131073,3-axis columns,
shared-source memoization/zero padding, missinglastword/transaction, zero grid
withUINT_MAX axis and over64MiB pre-read rejection. No16-bit selector cap change.
Existing32769-axis rejection changed to supported tier transition because the
old limit was software reservation, with true8388609*2-column budget rejection
retained. No expectations based on actual game output.
Native GPUAV numeric65537-group GREEN23:40:38/EXEEFE42359: actual immutable
coefficient snapshot load and exact output/input/neighbor sentinels, noVUID.
First GPU fixture expected the unrelated ShaderData storage fallback, while
bounded coefficients use dedicated flattened_srt. Corrected ONLY that placement
check; numericoracle unchanged. TypedIR requires ReadBoundedSrtU32 and SPIR-V
flattened_srt/no physical-pointer fallback. Existing cooperativeBDA coefficient
neighbor GPUAVPASS23:41; full resource_tracking CPU PASS3.99s/23:41.
Artifacts _Build/checks/20261005-{233417,233512,233717,233946,234059,234127}*/.
Source correction validated and committed separately as local b4eae640. Native high-res
original retry only after emulator build/install/hash; no gameentry proof yet.

## GPU-selected vertex raw buffer load width (2026-10-05; native runtime blocker)

Original retry212706/source locald2bafa93/EXE402ceb50 passed all first-launch
settings and real12ch ATRAC9 initialization, then ended22:02:34.846UTC/native321.
No watchdog/memory guard, forced kill or WindowsAppHang observed. New vertex
1135e3d2715c8ba2/pc1398 fails resource tracking: GPU-selected descriptor fallback
requires raw DWORDx2/x3/x4 load. Peak25.29GiB; shown532 is still the old dark scene,
not game entry. Both streams finalized; emulator51156/supervisor41980 absent.

Diagnostic223637 captured registered gs_1135e3d2715c8ba2_5c0764798df35917.bin
(14272B) before execution; exactpc1398 wordsE0283000/80010100 decode MUBUF
opcode10 BUFFER_LOAD_USHORT: one16-bit unsigned value zero-extended to VGPR.
Shared correction now admits only unsigned16-bit raw scalar load in addition to
existingDWORDx2/x3/x4; formatted/typed/signed/8-bit/scalarDWORD/store paths stay
unsupported. Backend uses real16-bit physical load, two-byte mode3 payload bounds,
zero-extension and unchanged descriptor addressing/other OOB modes/EXEC guards.
Native CPU intendedRED22:44:34/9E22B304 then unchangedGREEN22:47:01/2F190D0C.
Numerical20case GPUAVGREEN23:00:48/12BE30DF after correcting ONLY fixture's
EXEC restoration from all32 lanes to original1 lane. Oracles unchanged. Earlier
inactive mismatch was a test output race (IR proved initial mask invocation<1),
not a production defect; no masking code/expectation weakened. Active cases had
already matched. Real first/last halfword, DWORD crossing, allOOBmodes, SOFFSET,
swizzle/null/EXECinactive and exactneighbor sentinels PASS. Seven existingindirect
load/address/format neighbors GPUAVPASS23:03; complete native resource_tracking
suite PASS23:04:52 in3.89s. Local a025b5a7 commits the correction; emulator build
active. Broad metadata baseline remains recorded, no unrelated full-suite rerun. Artifacts
_Build/checks/20261005-{224415,224645,230000,230254}*/ and ignoredcaptureanalysis.

Source not in static4.3MBall_shaders corpus (runtime-generated variant). Own
40512 gracefullyclosed/native0 at22:39:25 aftercapture, no guard/forcedkill;
streams finalized/cache preserved. Native CPU regression now extends the existing
GPU-selected raw admission fixture with that UInt16 case and retains vector,
rawDWORDx1, formatted/typed/8-bit/signed/store rejection neighbors.
Primary AMD RDNA2 ISA tableBufferInstructions opcode10 and section8.1 bounds;
local official rdna2-isa-budget.txt. Independent LLVM per-byte strictbuffer OOB
contract: https://llvm.org/docs/AMDGPUUsage.html . No originalgame-specificbytes
or hashes in tests/production; actual capture stays under ignored_Build.

Required minimal regression: determine actual decoded load opcode/count/type,
then synthetic guest/IR GPU-produced four-word descriptor feeding that access.
Use lightweight resource_tracking_tests for CPU intended RED; retain neighboring
supportedx2/x3/x4, invalid descriptor/bounds and unsupported formatted/atomic/store
rejections. Do not assume the captured access is rawx1 before inspecting it.
After shared tracking/backend correction, run the same bounded native numerical
GPU oracle for first/last element and exact neighboring sentinels, then closest
indirect-load neighbors. One focused cycle and final affected validation; reuse
current baseline metadata failure instead of rebuilding its absence again.
Preserve535725251-byte driver-cache payload; original game retry only after fix.
No shader/title/address exception, zero-resource fallback or guard suppression.

## DCC/HTile metadata reuse retirement (2026-10-05; current native baseline RED)

Independent current native GPUAV broad cache-flow runs fail at
`reused metadata owner retirement` on both d2 fixed GC and scoped17529 GC bytes:
retiring an old DCC image loses expected live HTile slice state. Logs
texture-gc-cache-flow-{baseline-check,unfixed-current-baseline}-clean-gpuav-
20261005.log{,.stderr,.run.json}; this is a baseline correctness lead, not a
proved cause of Yotei dark pixels or AppHang and not a GC-fix regression.

Required next minimal native RED: isolate only shared metadata allocation reused
from color DCC to two-slice depth HTile. Establish independently observed slice0
written/uncleared and slice1 cleared; retire old color owner, bind depth again,
and verify live metadata identity and per-slice state. Also retire final depth
owner and require metadata removal. Record values before and after retirement
and after rebind to identify the actual transition; do not infer which API
caused it from a compound assertion. Keep CPU-write coherence/imported-clear
behavior and real Vulkan depth readback, no fabricated clear flags. Test alone
on exact current unfixed bytes, then unchanged GREEN for the owning cache/meta
mechanism and its existing alias/HTile neighbors. Game link remains pending.
Do not edit core/tests or build this regression during active212706 game.

## Texture GC protected-prefix starvation (2026-10-05; native regression pending)

Read-only follow-up to measured memory pressure: PR977/8dd843e6 identifies
protected textures consuming the LRU candidate budget. Current shared native
TextureCache has that same scan-before-filter mechanism. This is an independent
cache correctness/performance defect lead; its causal link to Yotei AppHang or
working-set guard remains unproved. Do not port the unrelated accounting hunk
without a separate regression.

Required independent fixture: cold linear1x1 images, 0/9/10/12 GPU-owned safe
protected entries before two clean reclaimable entries. Nonpressured GC must
preserve protected GPU content and reach both eligible entries within the ten
candidate budget. Under pressure, retire/download those protected entries and
verify exact GPU words and neighboring guest sentinels. Keep existing recursive
depth/stencil ten-candidate budget regression unchanged. Native GPUAV RED against
17529f82, unchanged GREEN after shared preselection only, then cache neighbors.
Artifacts _Build/logs/texture-gc-protected-prefix-*. Production/game effect pending.

Executable standalone regression now registered as texture_cache_protected_gc /
--texture-gc-protected-prefix-only. RED test SHA7238c0271b900227c2473ad4daf64d4e
7857a710468c7f8a61b3819f71413c1d, production SHA b3d229761282fcbccba18af900ed250f
6cb09a0eb3a5ae62d5a8d6761860f412. Native clean GPUAV RED21:15:37UTC / EXE352AFFEB
fails the intended protected_count9 clean-tail oracle, not timeout or VUID.
Only owned seven-line production patch was absent; exact baseline bytes checked.
The initial broad cache test is already baseline RED on video-out metadata, so
this fixture is independent of it. Test integration compile errors are not RED.
Initial GREEN passed all four numerical cases but teardown caught a stale Epic
implicit-overlay manifest in the host loader. Process-only VK_IMPLICIT_LAYER_PATH
uses an empty test folder per Khronos loader contract, while explicit Khronos
validation/GPUAV stays enabled; no validation error suppression or registry edit.
Clean initial GREEN exit0 / EXE22414E14, then clean independent RED above with
identical test/source oracle. Fixed source restored byte-for-byte for final GREEN.
https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderApplicationInterface.md

Final unchanged-oracle clean GPUAV GREEN21:16:40UTC/EXEC242D794 PASS exit0.
Added neighboring independent clean12-image ten-candidate and six recursive
D32/stencil-pair budget oracles PASS with prefix/pressure/readback/sentinels
21:19:29UTC/EXE7613A30E; image-view and dirty buffer-GC GPUAV neighbors also PASS.
Broader UnifiedTextureCacheFlow remains RED at reused metadata owner retirement.
Scoped absence of ONLY owned seven-line fix with exact17529 production bytes
repeats IDENTICAL phase/error21:21:10UTC/EXEC1BFBD6D; fixed run failed there
21:19:43UTC. This is current baseline evidence, no broad suite GREEN claim.
Core fixed bytes restored; locald2bafa93 committed. Native build/install and
committed scoped GPUAV CTest3/3 PASS. Original212706 replay active with exact
EXE402ceb50, original limits and memory/own-window responsiveness diagnostics;
current screenshots still loading. No game-effect claim.
No other-game corpus; GC fix does not prove Yotei memory/Windows-hang resolution.

## Post-settings Windows AppHang (2026-10-05; diagnosis pending)

Finalized run133643/source17529f82/EXE3bc052af ended14:21:26.270UTC with
native0xCFFFFFFF. Supervisor recorded no memory/progress/total guard or cleanup
request; Windows Application event1002 and WER event1001 report AppHangB1 at the
same time. This identifies Windows hang termination, not its cause or initiator.
Latest in-flight operation is cooperative54904 compute pipeline creation after
661 successful creations; shown507 stayed unchanged after settings. Peak sampled
working set28275372032B/private16684023808B; final sample26858307584/16156315648B.
Preserved cache512077120B; both streams retained, stderr empty, no game entry.

Diagnostic next: exact last681588-word module SHA1845865845088138c5b052ffa18ebad6
56f1dcd2b6e69ccd0304be4d1200bbd0, validated descriptor layout0[27],51..54[1].
Standalone bounded native create/destroy with three same-device cached creations,
phase-specific working/private samples, no dispatch, under8GiB/240s guards.
Artifacts _Build/analysis/post-settings-lifecycle-20261005/. This is diagnosis,
not a synthetic RED or production fix. If retained lifecycle ownership is proved
incorrect, add a reusable independent lifecycle regression before core changes.
If it is not, instrument bounded own-window responsiveness on the next original
retry; distinguish compiler progress, UI event-pump stalls and memory retention.
Do not infer GPU hang/TDR, leak or gameplay from this exit code alone.

Native exact-module spirv-val PASS21:04:25UTC. Standalone lifecycle diagnostic
PASS21:06:54UTC (three successful creates; no dispatch; both streams retained,
stderr empty; timeout/memoryguard false). First creation147828ms; cached repeats
125/94ms. Peak working3216777216B. Phase-specific samples: ready79646720B;
after first destroy2976915456B; cache/module/device/instance destruction still
~2986MB working/~3237MB private immediately. No delayed post-destroy samples,
allocator live-allocation invariant or synthetic leak RED; retained process
memory does not prove an emulator lifetime leak. Probe PID45240 cleaned up.
Generated standalone diagnostic target built through windows-local.cmd; original
build.ninja restored byte-for-byte (SHAf639edd264a308ea403ae80144caecb1f5435fac
271537067d72de32eac8d568) before any regular native build. Core unchanged.

## Post-settings native memory bound (2026-10-05; diagnosis pending)

Source17529f82 / EXE3bc052af, run130216: normal UI selects Performance and
accepts settings; no game entry. Original28GiB working-set guard triggered after
675 compute completions / shown756, native graceful close succeeded, exit0 at
13:25:34.608UTC. Not a crash, timeout or independently reproduced memory leak.
Both streams retained (stderr empty); cache preserved492622718B on disk.
Latest completed cooperative `da4ff122` variant took117.133s; snapshot actual
`1971_new_shader_cs_da4ff1222c09f131.spv` is retained in
`_Build/analysis/performance-memory-bound-pipeline-20261005/` with hash/bindings.

Before another long retry, validate this exact module and measure a bounded
pipeline-only creation/destruction outside the game with its admitted descriptor
layout and native device features. Sample working/private memory before, during
and after creation; distinguish cold compilation peak from retained pipelines,
shader modules and renderer resources. No dispatch needed for this diagnosis.
No speculative production patch or memory-limit increase. If a lifetime leak is
identified, first prove it with reusable synthetic native lifecycle/ownership
fixtures and an independent invariant, then unchanged GREEN and affected cache/
resource-neighbor cases. A watchdog event alone is not semantic RED evidence.

Diagnostic completed: native CPU spirv-val PASS 13:30:22 on exact hash
45c929c8309578c24fbb6f7883cf93e0be9eec583e9b236750a1df9c4ec97ef2.
Descriptor layout verified: storage buffers binding0[27],51–54[1]. Existing
native probe SHA DB7FF1C2D3B8B6B903EC76CF8BA12137D87E96F9F8A1D513BB0F2E60BB5AE596,
no pipeline cache, disable optimization matching native cooperative flags:
117.716s creation/cleanup PASS, exit0, stderr empty, timeout/memoryguard false.
Peak sampled working set3281620992B, private3448729600B; final live sample
1517056000B working set. Sampling cannot assign the late drop to a precise
create/destroy phase; no after-destroy lifetime invariant or leak RED proved.
No dispatch/readback, therefore no numerical or gameplay proof. Same-source
bounded replay133643 now includes memory sampling, preserves28GiB guard and
uses492622718-byte cache. No speculative production memory fix.

## RDNA2 BVH intersections (2026-10-05; observed missing capability, regression pending)

Final stdout of native `120227` / source `17529f82` / installed `3bc052af…`
reports an existing skip of BVH compute shader `4f07b07b3d8c8406`, pc1738,
raw MIMG opcodee6. Settings are visibly rendered and accepted; background remains
very dark. This warning is a concrete color investigation lead, not proof that
RT caused the darkness. No RT implementation or color fix is claimed.

Current Decoder::DecodeProgram stops at e6/e7 and marks `has_bvh`; compute
ShaderRecompiler returns `skip_dispatch`. Its comment describes a temporary
workaround until the player selects a guest mode without RT. Do not extend this
skip or fabricate hit/miss outputs. Unchanged-source warm replay `130216` is
active; inspect normal Graphics Mode / Performance before interpreting its effect.

Required synthetic regressions before a core implementation:

- CPU decoder/IR tests for BVH32/BVH64, contiguous and NSA operands, A16 off/on,
  full 4-dword output, descriptor and encoding restrictions. Keep the instructions
  before AND after the BVH operation; no successful truncated/skipped program.
- Independent finite synthetic four-child box intersections, hit/miss and distance
  sorting, then triangle intersection in both return modes. Bound every allocation,
  node fetch and shader loop; exact sentinels/EXEC-inactive and neighboring-memory
  guards, high address words, first/last valid nodes and invalid descriptor bounds.
- Native Windows intended RED on unchanged behavior, same unchanged oracle GREEN
  only after shared decoder/typed IR/resource/backend semantics. Native GPUAV
  readback after CPU proof; no host driver reset. Do not run builds/GPU tests while
  current game is active. Other-game corpus unavailable; no compatibility claim.

Primary contract: AMD RDNA2 ISA section8.2.10/tables48–50, local official PDF
`_Build/analysis/rdna2-isa-budget.pdf` and extracted text. Ray VGPRs number11/8
(BVH32 A16off/on) or12/9 (BVH64); 128bit BVH SRD is distinct from ordinary image
SRD, with address, bounds, box growth/sort and triangle return mode. Preserve
DMASKf/D16zero/R128one/UNRMone/DIMzero/LWEzero/TFEzero/SSAMPzero restrictions.
https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna2-shader-instruction-set-architecture.pdf
Current web PDF fetch returned401; local earlier official copy supplies the text.
AMD GPURT official hardware-node definitions are a separate primary layout lead:
https://github.com/GPUOpen-Drivers/gpurt/blob/7b226d48b46b7e92fec3b9ecc5712e5bf2bf3dd9/src/shadersClean/common/gfx10/BoxNode1_0.hlsli
Do not assume RDNA3/4 or desktop GPURT layout is console-compatible without
checking actual descriptor/node metadata and the applicable RDNA2 contract.
No synthetic test is yet executed for this capability; tests remain pending.

## ATRAC9 mono substream multiplexing (2026-10-05; native RED/GREEN proved, original game initialization verified)

Completed local commit29d32a5c9d64acaa0281d8a4d5975923d5466827 implements
shared AJM configuration geometry and real independent mono decoder histories,
frame-major channel interleaving and per-channel padding on the last frame.
Ordinary FE configurations/haptics and NGS2 numeric-word initialization remain
unchanged. Native Windows original retry104233/source29d32a5c/exactF41301AD visually
passes Medium/Standard/Quality; Quality input released11:03:47UTC. Actual target
creator11:03:38.077/R8=12/R10=3072 followed by AJM ATRAC9 initialized48000Hz,
12ch/frame_samples256/superframe3072bytes/4frames (_kyty.txt line23492595).
Old guest C0000094 not observed; later large shader compilation progresses.
Intentional scoped close11:26:28 completes gracefully11:28:33/native0, manifest
finalized11:28:36/native0. Both streams read/owned processes absent; warmcache
451288980B preserved. No timeout/forced kill/driver reset, no main-title menu.
This proves actual extended initialization past the former fault, not heard PCM,
movie playback, main/title menu or gameplay. Those remain unverified.

Required regression is executable in tests/ShaderRecompilerComputeTests.cpp,
CheckAjmAt9Multistream/--ajm-at9-multistream-only, CTest ajm_at9_multistream.
It covers metadata2/6/12/36channels and rate/byte-count/exponent variations;
nonzero Float/S16/S32 synthetic mono spectral frames with independent per-channel
LibAtrac9 references, two superframes and exact interleaved PCM; final per-channel
padding, single-frame continuation/reset, gapless257-sample skip/137-sample limit,
short input/output retries and sentinels. Invalid signature/reserved fields and
38/64channel counts leave metadata unchanged. Twelve-channel AJM batch control/
run checks real decoder output and sideband counters. No proprietary packet
fixture is versioned. Other-game audio corpus unavailable; no cross-game claim.

- Native intended RED10:30:59UTC, unchanged productionebe7a6f0/testEXE
  332627c4af75de5de89dacc7ccf099bcd5fa9582cb9c7d4cea58902e833080ef:
  all5 valid metadata and all5 decoder initialization cases rejected, after
  independent nonzero mono references succeed. Expected harnessFail C0000409;
  no build/dependency failure or timeout. Both streams/manifest retained in
  _Build/logs/ajm-multistream-red-20261005.log{,.stderr,.run.json}.
- Same unchanged oracle GREEN10:35:38UTC/testEXE
  16723b32c5bdfe78e79bb1db785ce289ec9e9b1c347b443d3f1ab66295dac42d,
  all Float cases/metadata/padding/continuation/reset/gapless/bounds PASS, exit0.
  Added S16/S32 and12channel batch neighbors PASS10:37:56UTC/testEXE
  c6cd3e6bf621e2d91f4e07878627da4e83a548035d74d3abebcf62a178d523b7.
- Native existing audio targets rebuilt; bounded CTest4/4 PASS10:40UTC:
  ajm_at9_multistream, ajm_at9_configuration, ngs2_sampler, audio_out2_port;
  both streams read/exit0/no timeout, ajm-multistream-final-ctest-20261005.log.
  Full suite still has prior baseline failures; scoped tests do not prove menu.

Contract lead: primary independent interoperability implementation
https://github.com/iStark/PS5PCEM/blob/baa718235a37d310d91ac001f4d92cdc8099a69c/src/hle/ajm_codec.zig .
Extended config has sync30, byte2 low7bits40, sample-rate index in byte1 high
nibble, channel pairs in byte1 low nibble/byte2 high bit, frame bytes per mono
stream=(byte3>>2)+1 and superframe exponent=byte3&3. This is independent
implementation evidence, not public Sony SDK proof of all extension/layouts.
Actual raw30 72 c0 fe gives12ch/48k/64bytes per mono frame/4frames/256samples
per frame, ordinary mono configFE7007F0 and3072-byte superframe. Geometry agrees
with captured3072-byte input and nearby12channel metadata. Isolated Linux-only
actual packet probe decodes48mono frames/nonzeroPCM and consumes exactly3072B;
it validates the lead, not native Windows emulator/audio/menu operation.

Run100823 sourceebe7a6f0/B6C525F3 after viewedMedium/Standard/Quality ended
nativeC0000094 on guestDIV9003502d2/ESI0. Early metadata watch directly captured
bytes30/72/C0/FE written at guest90030df25/29/31/39 at10:25:41.649UTC;
R8=12,R10=3072,R15=metadata100952beb0. The captured instruction sequence reverses
a numerical stack config into raw bytes; ordinary RIFF helper was not the
creator. Source/root lead now directly observed; an endian-only normalization,
NGS2 change or silence/haptics fallback is unjustified. Earlier unproved word-
candidate patch stays unapplied. Manifest finalized10:25:50/nativeC0000094,
both streams read/owned processes absent/hash unchanged/warmcache preserved.
Native RED began only after runtime/recorder cleanup. Preparatory Linux-only
synthetic36variant probe is not native RED/GREEN. Original retry finished gracefully; native DS regression began after verified cleanup.

## SNORM formatted buffer store endpoints (2026-10-05; upstream candidate, native RED/GREEN and scoped neighbors proved)

Candidate de9c15fa changes shared formatted stores for SNORM/scaled types.
Read-only current core converts SNORM16 but leaves SNORM8 raw float bits before
packing. Required independent minimal oracle: signed normalized -1,0,+1 and
finite clamping -2/+2 in8bit scalar/pair/RGBA stores, MUBUF descriptor and MTBUF
explicit formats. Exact127/-127 endpoints, preserve neighboring bytes/words,
and16bit currently supported neighbor. Do not copy upstream fraction/tie/NaN
oracles without AMD contract proof. Primary normalized representation/conversion:
https://docs.vulkan.org/spec/latest/chapters/fundamentals.html#fundamentals-fixedfpconv
requires exact endpoints; host contract alone does not resolve all guest format
rounding/special cases. Executable finite endpoint regression is now in the native harness and CTest;
its independent RED/GREEN evidence is recorded below.
Additional packed2bit signed-format exception and scaled truncation/NaN need
primary guest ISA proof before full upstream port. Current actual Yotei dark
scene link unproved; do not claim this candidate fixes game colors. Keep sources/
tests frozen during run091337, no nativebuild or GPUtests while runtime active.

Native GPUAV RED09:53:16UTC, unfixed productionb3667d29 plus independent
8case test; native EXE SHA2562270c7e6ef927216a8dd620c5a658f36f3257db1017e93baa7bcfd139b711847.
DescriptorSnorm8EndpointsAndClamp exact readback expected7f7f0081, actual00000000;
neighbor deadbeef/cafef00d preserved, intended conversion failure. Test exit
c0000409 through harness fail-fast; no timeout/VUID. Both streams saved
snorm8-endpoints-red-gpuav-20261005.log{,.stderr,.run.json}. Earlier harness
integration compile errors corrected before this result, not counted as RED.
Selected correction scope only SNORM8, reusing shared clamp/normalized rounding;
existing SNORM16/Unorm/float/scaled/packed2bit behavior unchanged. Do not import
unproved upstream scaled/NaN/packed2bit changes or claim actual game colors fixed.

Unchanged8case native GPUAV GREEN09:54:57UTC/native EXE
4dbad1b3007b0449f8c3a74f93fb975f4f201f5bbe650f481d972df14e283b86,
all exact readbacks, exit0/no VUID or timeout. Core correction10lines shared
EncodeFormattedStoreComponent only finite SNORM8 clamp[-1,+1]*127/signed
conversion, preserving prior normalized rounding policy and byte merge/bounds.
Existing12formatted store cases GREEN09:55UTC, D16load/store2cases GREEN09:57;
CTest scoped5/5 GREEN09:58 (new endpoints, image rebind, packedfloat, scaled
filtering, FP32 MAD). Logs snorm8-{endpoints-green-gpuav,endpoints-neighbor-
formatted-stores-gpuav,endpoints-neighbor-d16-gpuav,final-ctest-gpuav}-20261005.
Native CPU-only cooperative-admission corpus2/2 PASS10:02/resource_tracking;
report snorm8-cooperative-admission-cpu-20261005. Not backend emission/GPU or
cross-game proof; no other independent game's corpus available. Actual native retries after this fix visually passed Medium/Standard/Quality;
scene remains dark, calibrated colors/main-title menu unproved. Fullsuite notgreen.

## Audio code-origin diagnostic own-trap handling (2026-10-05; workflow, synthetic native checks)

Run090311 sourceb3667d29/exact7ce9d837 ends09:06:53UTC exit80000004.
Diagnostic v1 assumed source/output pointers in heap0x1000000000..0x2000000000;
on its own verified execution breakpoint it threw before recording context, so
single-step was incorrectly forwarded. This is a diagnostic-induced interruption,
NOT emulator/audio defect RED or a menu result. Both streams preserved, stderr0;
prior fixed-address diagnostic files unchanged. No metadata origin inference.
Ignored v2 handles only independently identified ownDR0 event even on capture
failure, sets RF for one resumed instruction, restores own watches and records
mapped source20B/caller before inference. No guest code/data/operands modified.
Independent native synthetic probe has same4byte instruction signature, source
outside former heap assumption, then one with only17mapped bytes while its single
byte read is valid. Valid target09:11UTC captures source20B; partial09:12UTC logs
expected own-capture error and restores watches. Both preserve all12 readbacks
and targetexit0/recorderexit0; no bounded timeout. Artifacts ignored under
_Build/analysis/audio-source-probe-v2-20261005/{valid,partial}/. Workflow evidence
only; no artificial production emulator test or audio contract GREEN. Use only
watch-task-audio-metadata-source-instruction-v2-20261005.ps1 for next game.

## LDS float min/max operands (2026-10-05; independent native RED/GREEN, neighbors and corpus baseline proved)

Completed local commit17529f8241624b2ab79002e384a8b2daeb086702 corrects
decoder explicit ADDR+DATA0, typed IR
address/DATA0/EXEC, existing EmitFloatAtomicReplacement applied to native/packed
LDS and GDS atomics. Per-lane EXEC, address bounds and real atomic retry remain.
No title/hash/address exceptions or new NaN/denormal/tie policy. Extra special-
value contract remains debt; actual darkscene/menu effect unproved.

Primary independent contract: updated AMD RDNA2 machine-readable XML says DS_MIN/
MAX_F32 select the minimum/maximum of LDS and DATA0, no DATA1 input.
https://gpuopen.com/machine-readable-isa/ and official archive
https://gpuopen.com/download/machine-readable-isa/latest/ ; XML SHA256
 d671ecbc36543674ab59e9b2feffd56718fd2137e7ad913c314f2e2508b9f4f7.
Older PDF DATA2 pseudo-code conflicts with this newer explicit operand table;
upstream e85279ea supplied the lead, not the oracle. Saved actual54904fb4 shader
has six min/max at3b74-3b9c with distinctDATA0 and unusedDATA1=v0. Game image
causation still unproved; no calibrated colors/main-menu claim.

Executable regressions in ShaderRecompilerComputeTests.cpp cover separate
CPU source-count oracle and six independent finite positive/negative pairs,
DATA1 variations, offset cells, GDS neighboring guard and inactive EXEC. Explicit
S_BARRIER publishes LDS and orders ordinary reads after atomics; first preliminary
RED also had a GPUAV race and is preserved/excluded from clean-validation proof.

- Native intended CPU RED11:32:56/test31390092: wrong source-count, C0000409.
- Clean native GPUAV RED11:35:53/test4DDE1D66 on unchanged29d32a5c:
  expected[2,4,-9,-1,-2,1], actual[4,3,-2,-1,1,1], exactfloat bits saved.
  No VUID/race/validationerror/timeout; intended harnessC0000409.
  _Build/logs/ds-float-finite-contract-synchronized-red-gpuav-20261005.log{,.stderr,.run.json}.
- Same unchanged synchronized oracle GREEN11:39:53/test94CA8596, four GPUAV
  cases exactPASS, CPU decoderGREEN11:39:52; no validationerror/timeout.
- Five additional GPUAV neighbors PASS11:42:58/testBDEB2EAF: min/max with256
  guest lanes/two groups and unusedDATA1, old cooperative min, GDS/subdword,
  real buffer compare-and-swap. Old dormant compare-operand oracle9/1 corrected
  explicitly from primary min(4,9)=4/max(4,1)=4, synchronized and PASS11:46:27.
- Existing dormant shadercfg translation fixture now runnable: actual IR has
  address/DATA0/EXEC with exactDATA0 literals and native SPIR-V validates;
  PASS11:46:26/shadercfg6B9EDF80. Native scoped CTest6/6 PASS, fourDS entries
  plus prior SNORM8 and ATRAC9. Full suite still has known baseline failures.

Corpus: first raw encoding selection used wrong shift17, not affected coverage;
corrected bits18..25 selects16 header-profile manifests (raw literals can still
be false positives). Native fixed audit sevenPASS throughresource_tracking,
eightdecodeUnsupported(MIMG e5/e6,DS e1,SOPP19 groups overlap across8shaders),
one30s CPU timeout(cs_00012684). Runtime metadata incomplete, not backend/GPU
or other-game compatibility proof. Other-game corpus unavailable.

Exact baseline: preserve fixed five ownedDS corefiles+SHA256, temporarily restore
ONLY them to29d32a5c, serial native shadercfg6240EB58 and identical16manifest
30s audit. All16 status/phase/error match fixed report, including same timeout;
no newly introduced corpus refusal observed. Both reports and comparison saved:
_Build/shader-audits/ds-minmax-cpu-{v2,baseline29d}-20261005,
_Build/analysis/ds-minmax-corpus-baseline-comparison-20261005.json.
All five exact fixed bytes restored/SHA256verified11:53; restored native shadercfg
build PASS/shadercfgD1601FA1; final restored native CTest6/6 PASS11:56.
Unrelated fixes/tests preserved; no reset or driver recovery. Native emulator
build/install for `17529f82` passed; installed SHA256
`3bc052af7203667ea7e63308d6f22eaa83ffb759880d62ced30f27be9b9f7f2e`.
Original retry `120227-ds-finite-warm-noval` is active. At 12:17 UTC it is still
loading new large shader variants; no new-run settings/input/color result yet.
One actual current SPIR-V module passed bounded native CPU Vulkan 1.3 structural
validation at 12:12. User clarified the already verified first-launch settings
count as the menu milestone; gameplay remains pending. See the launch checkpoint.

## Overlapping image dimensions and view acquisition (2026-10-05; native RED/GREEN and scoped neighbors proved)

Candidate upstream a2f851788f3f4edc7649c9b31f27215b05febb6d corrects shared
RebindImages view acquisition and depth overlap dimensional recreation. Current
source7ace6ff8 still resolves all aliases before creating their views. Required
synthetic regression: one mapped allocation exposed through 3D then 2D descriptors
and reverse order, R8 uploaded byte0x7f and R16 GPU-produced D16 depth1.0/rawffff.
Verify each final native image dimension/live view and exact GPU-transfer readback
of both aliases after replacement. Independent Vulkan image/view compatibility:
https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewCreateInfo.html .
Native GPUAV RED08:16UTC on unchanged production7ace6ff8/testEXE
454d698beb7a4f90931017fa7d95f3e635875721d6b78a070bbabe961e85049b:
--image-rebind-dimensions-only and independent --image-rebind-depth-only both
exit321 with texture requires rediscovery before final acquisition at
textureCache.cpp1785. Both streams inspected: no VUID/timeout, intended alias
retirement error. Logs image-rebind-{dimensions,depth}-red-gpuav-20261005.log.
Test inserted from ignored draft, both orders/oracles unchanged for GREEN.
Selective shared RebindImages view acquisition applied after RED. Unchanged
four cases GREEN08:18UTC/testEXE186c279cfa7d73246a86fc7135d5ee33bcfeeb7e7efff370900b2e096701f311,
exact native GPU contents/live dimensions and zero VUIDs. Additional upstream
ResolveDepthOverlap IsVolume condition NOT imported: local existing depth/color
recreation already passes these R16 cases; no independent RED for that condition.
No unrelated read-only buffer fixture change. Layered/cube and HTile subset
neighbors GREEN08:20UTC/zero VUIDs. Existing stencil/mip fixture FAIL at R32 uint
depth-backed view, expecting same D32 backing. Scoped removal of ONLY saved
view-acquisition patch reproduces identical failure08:23UTC/testEXEf657c41cc50101b54bd3be7a0e11bbb24d8236364c769241328f27ea2583d945,
no VUID/timeout; therefore preexisting, not introduced by this fix. Both logs
image-rebind-neighbor-stencil-mips-gpuav and image-rebind-stencil-baseline-gpuav-
20261005 retain exact failure. This old fixture does not validate later cases;
update its depth/color oracle only from independent contract/readback evidence.
Restored exact owned patch; final native testEXE5a8ee05c1abf867b1d3cf1ece8c748900330e3cc53196ea67f5d513c2a031ede.
Native GPUAV CTest3/3 PASS08:25UTC: graphics_image_rebind_dimensions,
shader_scaled_texture_filtering, graphics_packed_float_roundtrip. Log
image-rebind-final-ctest-gpuav-20261005.log (UTF16). Full suite remains RED;
actual scene colors/menu/game retry not proved by these synthetic checks. Native bounded GPUAV RED on unfixed source
must precede a core port; preserve local read-only unaligned-buffer fixture (do
not copy unrelated upstream change to read/write). Neighbor stencil/depth alias,
mip-view and layered image tests required after any fix. Relation to Yotei dark
scene unproved; no menu/color compatibility claim from this candidate.

## Native run finalization race (2026-10-05; workflow diagnosis)

Cold run070232 source7ace6ff8 reaches counter205 by30min deadline; native
wrapper Kill reports access denied and skips original run.json finalization.
Later CIM confirms process42480/descendants absent, logs EOF/cache flush,
but emulator exit code is unverified. Preserve separate observation JSON.
Prospective ignored warm-origin helper checks original handle after Kill error
and records/drains/disposes in finally; parser syntax PASS. Workflow-only, not
an emulator defect RED or a passing menu result. Deadline capture withheld
on failed foreground acquisition; no settings/input/metadata writer verified.

## Legacy MAD code-size and native pipeline cost (2026-10-05; native RED/GREEN and GPUAV neighbors proved)

Native source1fd40efc retry061159 remains at black shown157 while creating
compute pipeline54904fb419d79e49; emitted965060 SPIR-V words (later fc6f8c56eb7e168f:975694). Legacy MAD
now explicitly flushes five values and rounds product/add; numerical contract
tests pass, but inline expansion increases code and cold native compiler cost.
This is not a proved GPU hang or a reason to remove denorm/rounding semantics.
Required synthetic regression: bounded, dependent MAD chain with independent
zero cancellation oracle and explicit module-size budget, fail on current
emitter before optimizing shared emission. Retain all FTZ/FMA numeric cases,
validate ordinary and cooperative native GPU execution after any outlining.
Pipeline creation does complete:54904fb4 variants107-114seconds each.
Previous run used warmed cache; elapsed-time comparison is not a controlled
before/after cold performance proof. Cache preserved, new SPIR-V requires new keys.
Native GPUAV RED06:43:53-06:44:20UTC on production1fd40efc/testEXE
1886a57583aa57aaeefe3be6895785d369f1487eff2480b31a6222de7b3497e2:
128-dependent-MAD module47717words exceeds8192; cooperative128lanes+barrier
module44153words exceeds32768. SPIR-V validated before budget check; no VUID
or timeout; C0000409 is intended test Fail, not unrelated crash/GPU hang.
Numerical cancellation oracle0 and module budgets are unchanged for GREEN.
Logs mad-code-size-{normal,cooperative}-red-gpuav-20261005.log/.stderr/.run.json.
Shared MAD outlining applied only after RED. First attempt8485words still
fails8192 budget (mad-code-size-green-gpuav log); budget/oracle unchanged.
Block-local immutable bitcast reuse clears at every EmitLabel, preventing
cross-branch dominance errors. Final GREEN normal6453/cooperative2889words,
both numeric0 GPU readbacks, plus independent add/multiply arms and join GPU
readbacks/validation PASS. Final testEXEb68a9c24250b5fb57b3c6256e7d962cd7360d0805e7973fd3ae1186c8362c770,
06:55UTC mad-code-size-final-green-gpuav-20261005.log; no VUID/timeout.
Legacy MAD rounding/FTZ and fused FMA cases PASS, affected FP32 compare5,
FP64 arithmetic7/conversion10, cooperative BDA1 cases PASS under GPUAV.
Final targeted CTest5/5 PASS06:56UTC mad-outlined-final-ctest-gpuav-20261005.log.
A missing test helper argument caused final-build failure, corrected to nullptr
in test harness; not counted as a regression. Full suite still not green.
Actual new source7ace6ff8 retry070232: E80 module105989->51437,6cc64dee
498313->313130, cooperative54904fb4 965060->680914words. First big native
cooperative pipeline still136641ms versus older107-114s variants. Module-size
correction is proved; cold-time improvement/scene colors/menu are not. Current
runtime continues within original30min/480s/28GiB bounds, audio origin pending.
Khronos SPIR-V Function Control DontInline is only a performance hint:
https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#_function_control .
FTZ and product/add NoContraction remain in the function body; no global mode
or title/hash/address handling change.
Run061159 ended by1800s deadline/graceful close06:42:15UTC, exit0/shown200,\nonly spinner verified. Recorder detached/restored watches; no metadata-origin\nwrite captured, no input or menu. Color comparison/audio root remain pending.

## Audio metadata source directly proved (2026-10-05; contract RED pending)

Run053914 on7f2a1c71/EXE1c5b8337 reaches Quality570 then C0000094, no main
menu. Correct task-owned hardware watch records source write3a4e29 at05:53:40:
R15 original header=0, R12 metadata nonnull. Metadata+0xc contains3072c0fe,
copied unchanged into descriptor+0x18; other metadata fields are0/skip256/0.
Unlike prior list-node mistake this proves the actual metadata route. Data watches
restored after capture, unrelated/fatal guest exceptions forwarded; all own runs
ended and native CIM clean. Raw proprietary snapshots remain ignored under run
053914/audio-config-producer. No production audio fix or independent defect RED.
Required next evidence: producer of metadata and explicit applicable AJM/API byte
versus numeric-word contract, then synthetic variation/bounds/real PCM regression.
Ordinary byte-contract baseline passes; do not reinterpret or auto-swap unknown
configurations merely to bypass this one fault.

## Audio write-watch boundary (2026-10-05; diagnostic correction)

Native run051911 on7f2a1c71 ends after Quality/C0000094, no main menu. The
first data-watch helper used owner+0x28 (list node), not owner+0x40 (actual
configuration, descriptor+0x18); its three list-write events are not configuration
producer evidence. Corrected prospective ignored helper watch-task-packed-audio-
config-20261005.ps1, retain logs and do not present the offset mistake as a fix.
Bounded heap capture verifies actual source parent/sourcekind4 and captures input
plus surroundings/config candidates, but original header still needs direct proof.
4096B stack read crossed mapping boundary; prospective probe512B restored.
No production audio change or valid audio defect RED yet. Need actual source
header and responsible shared contract before a synthetic failure/fix.

## FP32 legacy MAD versus fused FMA (2026-10-05; native RED/GREEN and neighbors proved)

Selective upstreamd8be245c correction is now backed by primary LLVM instruction
tables: GFX10 legacy0x1f/20/21 versus fused0x2b/2c/2d. LLVM SIISelLowering
isFMADLegal requires signed denorm flushing; FMAD represents separately rounded
multiply/add (SelectionDAG ISDOpcodes contract), unlike FMA.
References: https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AMDGPU/VOP2Instructions.td
and https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AMDGPU/SIISelLowering.cpp .
Independent synthetic cancellation: (1+2^-23)*(1-2^-23)-1 =0 after rounded
multiply then add, fused result-2^-46 bitsa8800000. Native GPUAV RED on source
7f2a1c71 before production edits: first five legacy forms wronglya8800000,
next five fused forms correctlya8800000, no timeout or validation error.
Log fp32-mad-red-gpuav-20261005.log/.stderr/.run.json,05:59:56-06:00:00UTC,
testEXE105e8e744fab0feb293c6d0ac8c35c433320b6abb9d37d79ef9436a8639dd149,
exitC0000409 is test Fail after numeric mismatch, not a valid game failure claim.
Shared decoder/IR/emitter now distinguish MAD/FMA with intermediate NoContraction
rounding and explicit signed denorm flushing; local scheduling pure-op classification
includes the new MAD operation. GREEN unchanged numeric oracle PASS06:03:29UTC, testEXEe7b0e355e4e4c5509a530a0cd672d40e309ddbcb19974fc09d759b52324bf1b7.
Signed denorm boundaries/mode0xf0, fused negation and ordinary arithmetic PASS.
FP32 compare/modeC0/E0, seven FP64 arithmetic/narrowing and cooperative BDA
coefficient neighbors PASS under GPUAV; affected CTest4/4 PASS, no VUID. Final
coverage metadata corrected; final native MAD and CTest4/4 PASS06:07UTC, testEXE
dc4d5b62c071da526a89f7ff637ec7bac6b18805f3bbcc7bdef233b1997f1a36. Full suite not green;
no actual dark-scene/menu/audio fix claimed yet.

## Packed-float sampling harness boundary (2026-10-05)

Expanded neighbors pass numeric values but GPUAV rejects the fixture's sampled
image descriptor left in TRANSFER_SRC_OPTIMAL by the earlier host byte probe
(VUID-VkWriteDescriptorSet-descriptorType-04149). This is not a production
color defect RED: caller must transition the image to a sampled-read layout.
Fixture sampled-read transition corrected without changing numeric/component/alias
oracles. Expanded regression and packed/BGRA16/tiler/image-transition neighbors
now PASS under GPUAV (final-neighbor logs); no validation errors.

## Audio descriptor provenance correction (2026-10-05; diagnosis only)

Read-only guest producer/callback analysis proves source descriptor+0/+8 are
linked-list fields, not an original stream-header pointer. Earlier NULL-header
statements were an interpretation error; no original header was captured. The
source configuration remains inline in the descriptor and is copied unchanged
to AJM control initialization. Original source metadata/header must be captured
independently before asserting an endian/ABI/audio fix. Guest RIFF callback copies
its four ATRAC9 configuration bytes individually; ordinary AJM initializations
continue to pass. Producer analysis artifacts stay ignored under _Build/analysis.

## Packed-float render-target transfer (2026-10-05; native RED/GREEN proved)

Selective upstream5940e623 shared correction preserves unequal guest 10/11/11
unsigned-float component widths through linear and RenderTarget64KB transfers.
Native --packed-float-rt-only uses independent numeric values and exact downloaded
bytes, real RT register discovery/cache transfers, and four different texels.
Baseline567663de GPUAV RED: unsupported RT layout7/type7/order2, exit321,
no timeout/VUID; testEXE37dd5b5aeff3f8506d3e87f8ab3b2921fd3825ae911d0c72197bd8f03c905c2b,
`logs/packed-float-rt-red-gpuav-20261005.log` (04:49:41UTC).
Same linear/tiled numeric/exact-byte oracle GREEN, testEXE8076e671be29cd6b76d1ea21845674416f3413eb5d5ff3bed692771d544984d7,
`logs/packed-float-rt-green-gpuav-20261005.log`.
Expanded sampled component/constant and incompatible same-native-format alias
neighbors PASS; final packed-float, packed textures, BGRA16, image transitions,
tiled sampled-format and tiler selectors PASS; affected CTest5/5 PASS. Extra
BGRA16 upload and SNorm/1555 physical-width probes PASS05:11UTC, testEXE
22ca9b32e663bf8182a1045159f75bb77c6c2d0d11ab2d9ba7e76816607bdd7d,
`logs/packed-float-final-bgra-neighbor-gpuav-20261005.log`.
Per-tick scratch lifetime, stream-wrap ordering and local HTile/depth behavior
preserved. No production audio change, full-suite success, other-game compatibility
or actual dark-scene correction claimed. Native emulator install/game comparison
still pending; useful upstream port is supported by synthetic numeric GPU evidence.

Run042814 source567663de ended04:43:23UTC/C0000094 after Quality; Medium/
Standard/Quality visually proved, scene still dark, main menu/game entry pending.
Expanded fault has same invalid audio config/control result, no new root proof.
Game/recorder ended and own native processes cleaned before test build.

## Filterable scaled 8-bit textures (2026-10-05; reproduction pending)

Upstream96c067d0 is a useful candidate, not yet a verified local fix. Before any
production change, add synthetic R8/RG8 UScaled bilinear samples at fractional
texel coordinates plus gathers of a component and a swizzle constant. Independent
oracle: interpolate byte values in the scaled domain, retain fractional results,
and apply selector constants after conversion. Check real runtime sampler mapping,
nearest-filter integer neighbors and packed-integer extraction. Native RED/GREEN
and GPUAV required; no build/GPU regression during active game033835/PID7628.
Use existing explicit readback intervals for fractional floating tolerance only;
zero/one swizzle constants remain exact. Current installed source2b8e95b0 unchanged.

Initial scaled fixture stops at an existing shader-swizzle internal assertion,
`scaled-texture-red-gpuav-20261005.log`, EXEa1c2168e6e7140b88334f79e170e32de515577b78fdf0317b0f148c8b931ddce.
Refine fixture to use its independent numerical swizzle oracle without requiring
which implementation layer performs the mapping. No production behavior changed;
rebuild and intended numerical RED still pending. Existing tests keep the assertion.

The initial numerical zero is NOT a valid defect RED: the adapted fixture used
ssamp0 (texture words) while its real sampler descriptor was in s[8:11]. This
also explains zero after the candidate production port. Preserve those logs as
fixture diagnosis, not proof (`scaled-texture-numerical-red-gpuav-20261005.log`,
EXE35d33de7db85; `scaled-texture-point-admission-green-gpuav-20261005.log`,
EXEeef94cd9a2b9). Expected colors unchanged. All three instructions now use
ssamp2, matching the explicit bilinear runtime descriptor. Scoped reversal of
only four owned production files restores HEAD2b8e95b0; candidate preserved at
`_Build/analysis/scaled-texture-local-production-20261005.patch`. Corrected native
RED confirmed on production2b8e95b0: runtime bilinear sample is ~0.2500038
(0x3e800080), expected63.75±0.01; this now exposes missing scaled conversion.
`scaled-texture-correct-sampler-red-gpuav-20261005.log`, native EXE
710c0e88ff1fd97e400be92d1d4a0fd7af1061cc8710e7aec316a69d0e07889a,
04:20:53–04:21:01UTC, C0000409, no timeout/VUID. Reapplied preserved generic
candidate, including local sampler point-admission adjustments. Native unchanged numerical
GREEN R8/RG8 PASS, `scaled-texture-correct-sampler-green-gpuav-20261005.log`,
EXE08f6c7979449fa63b69361fda37aec36d6f7cfda2d6202586b3f783b317b69b6.
First integer neighbor exposes a harness gap: runtime sampler creation ignored
specialized force_point_filtering (production NativeSampler applies it), causing
VUID04553 for R8_UINT. Align harness descriptor handling with NativeSampler;
keep all integer and scaled expected outputs unchanged. Final native GPUAV
integer neighbor and packed texture neighbor PASS; affected CTest4/4 PASS
(`scaled-texture-color-neighbors-final-ctest-gpuav-20261005.log`). Final test EXE
fed6305dfe1fb9086a9775365059623e51422bab12fb571c4edf6677675e7d19.
No second game installed; full suite remains not green. Clamp-to-border/custom
colors and additional scaled formats are not numerically verified by these fixtures;
this validates R8/RG8 interior bilinear filtering and component/constant gathers.
Windows emulator installation and game color comparison still pending.

## Color pipeline comparison with upstream (2026-10-05; regressions pending)

User requests comparison against andsouzam/main and useful integration, plus
investigation of very dark Yōtei colors. Fetched exact fork719e0257 and upstream
af3011cd; fork is upstream ancestor, 27 newer upstream commits, no fork-only
commits. Local branch diverges at8e61798b; do not infer absent behavior solely
from missing commit IDs. No merge/reset/push performed.

Candidates: upstream5940e623 packed10/11/11 floating color upload/download,
96c067d0 filterable8-bit UScaled textures, and logical-alpha blend support
73615c31 +0bef3fc0 +94e7d239. Some require older shared-mechanism dependencies
and manual adaptation to preserve local fixes. Required before production port:
synthetic native RED on existing implementation and unchanged GREEN for affected
color contracts, including independent channel values, linear/tiled roundtrip,
swizzle constants and alpha equations. Keep unsupported equations/aliases guarded.

Additional required neighbor before final port: native MIN/MAX on all four mappings
with deliberately different/unused color and alpha factors. These operations ignore
factors per Vulkan basic blend contract; imported classification must not reject
a previously valid MIN/MAX draw. Record intended guard RED then numerical GREEN.

Synthetic numerical test added to existing native raster harness with selector
`--logical-alpha-only`: four RGBA component mappings and three alpha source
factors, independent source-over oracle for every native pixel/channel, nonzero
destination clear. Production unchanged; native build/RED NOT RUN while bounded
GPU game diagnostic is active. Do not claim tested integration yet.

The captured setup UI is legible but scene very dark at diagnostic480x270 guest
resolution. Runtime source is10-bitUNORM, actual guest colorimetry remains to be
captured. HDR warning code exists but no PQ warning found in the failed run;
do not assume HDR or change gamma/brightness to conceal an unproved defect.
Read-only comparison and bounded fault capture continue; main menu/game entry
pending and issue108 unchanged under user's milestone rule.

Selective upstream logical-alpha integration verified (installed game still old):
- Production baseline f708edc1, native RED `logical-alpha-capability-red-gpuav-20261005.log`,
  EXE b0ade25b9548c403507907b5e7399dae857cd9006c2e0fc8de2f36a4fbccc3b2:
  mapping27/SrcAlpha expected0.4 actual0.46, no timeout/VUID. Initial harness
  depthClamp/dualSrcBlend omissions corrected before this RED. IndependentBlend
  omission exposed only by later existing MRT neighbor and corrected in harness.
- Adapted upstream73615c31/0bef3fc0/94e7d239 to local FP/resource/parameter linking;
  broadcast logical alpha before output mapping, distinct static shader keys,
  remapped shared blend factors. Unsupported cases explicitly fail; upstream
  disabling blending/warning fallback was NOT imported.
- Additional synthetic MIN/MAX RED `logical-alpha-minmax-red-gpuav-20261005.log`,
  EXE f6057477ca78eee9eb02a2ecd289155708c588977906359734bb7f7e0b1fc3b8,
  exit321 at newly imported classification for a valid ignored-factor MIN draw,
  no VUID. Classifier now preserves identical MIN/MAX color/alpha equations.
- Unchanged numerical oracle covers12 alpha variants plus8 MIN/MAX mapping cases,
  every pixel/channel. Final GPUAV CTest3/3 PASS, EXE
  c13ddb6350fca25d1360933285ddbfb053edf02d84488848f606bae36bd2b51b:
  `logical-alpha-minmax-dynamic-state-final-ctest-gpuav-20261005.log`. Previous
  extended run exposed missing vkCmdSetBlendConstants only in the harness;
  production renderDraw already sets it. Corrected fixture state, same oracle. (new blending, existing
  rasterization including packed vertex color/wave32/wave64/cache/masked MRT,
  draw offsets). Full suite remains unproved/not green; no other game installed.
- Contract: https://docs.vulkan.org/spec/latest/chapters/framebuffer.html basic
  blend factors/operations. Native emulator build/install/game colors comparison
  still pending; don't claim Yōtei darkness resolved from synthetic readback.

## ATRAC9 configuration diagnosis and baseline coverage (2026-10-05)

A global packed-word conversion candidate was disproved by existing runtime evidence:
run023451 has9807 successful ordinary AJM ATRAC9 initializations using the same
control API, not NGS2. Global word-to-byte conversion would break those streams.
Unproved audio candidate was removed before installation/commit; production audio
restored byte-for-byte to HEAD2e3637ea. The initial packed-word test had an unproved
ABI oracle; its failure is NOT a demonstrated emulator defect. Do not call this a
proved RED/GREEN audio fix. No auto-detection/header swapping or fabricated metadata.

New baseline test uses independently constructed MSB-first configurations (mono,
stereo,96kHz,vibration), metadata/null/error/sentinel boundaries, and real AJM control
initialization plus synthetic nonzero native PCM against a canonical backend byte
reference. Explicitly rejects reversed byte order. Native baseline PASS on
unchanged audio production, test EXE58e5a1e53d5f45c087c35e26b96859af1bca38b0d947f4f6d7a8537fa0d44bdb,
`ajm-at9-byte-contract-baseline-20261005.log`. Neighbor CTest2/2 PASS
`ajm-at9-byte-contract-neighbors-ctest-20261005.log` (new AJM plus NGS2 sampler).
Initial draft build/reference-frame errors were not valid reproductions; corrected
synthetic superframe flags before this baseline proof. This adds
coverage without changing behavior; original guest configuration root remains open.
Required next capture: source audio descriptor/header plus upstream read/API inputs
at the integer exception, distinguish unsupported format from corrupted metadata.
Successful control initializations do not establish the separate metadata-entry
ABI. Its opaque pointer declaration and baseline byte-codec tests are not primary
guest API documentation. Do not infer either a universal byte or universal word
metadata contract from those successes. Captured guest code submits the same
object configuration to control and metadata without an intervening byte swap;
identify the source descriptor/header that produced the invalid byte sequence.

## Integer divide exception during initial setup (2026-10-05; diagnosis pending)

Native production/tests source `f708edc1`, installed EXE SHA
`c73cc32e4bd75cd75c6ab9784989b1c8c96fe7f03f3a2f0a84ae719b08d8e29c`.
Run `yotei-integrated-20261005-015124-imported-htile-saved-setup-continuation-noval`
ends 02:19:33UTC with exit -1073741676 / 0xC0000094 (integer divide by zero),
not the 1800s deadline. Windows Application event1000 records guest-mapped fault
address `0x9003502d2`, unknown module. stderr empty; no DeviceLost/watchdog.
Task-owned PID45544 ended and runner streams drained/disposed.

Brightness, Medium difficulty, Standard experience and Quality setup screens are
visually verified. Normal native SendInput J/Cross confirms Quality at02:19:04UTC;
no post-confirm screen captured before the crash. Correlation does not identify
its cause. Main menu/game entry remain pending; no issue108 update.

Further diagnostic run023451/PID17812 repeats C0000094 after Quality at02:55:49,
ends02:56:02UTC; read-only recorder saved first/second-chance registers/object.
Codec ATRAC9, object block bytes0; configuration order is a lead requiring its own
synthetic metadata/decoder contract regression. Own processes gone and recorder
closed; local proprietary bytes remain ignored, not fixtures/public evidence.

Required regression BEFORE any behavior fix: determine the faulting instruction,
operand/register state and upstream guest API/ABI contract through crash evidence
or bounded diagnostic-only capture. Reproduce the responsible emulator behavior
with synthetic inputs and an independent observable oracle, then demonstrate the
intended native RED and unchanged GREEN plus neighboring boundary cases. If this
is unsupported guest signal delivery, establish that contract rather than ignoring
the exception. Do not skip the divide, alter guest registers, fabricate a nonzero
result, or select behavior by the recorded address/title. Existing WER temp dump
was already removed; archived Report.wer retains exception/address evidence.

## Sampled HTile owner rediscovery (2026-10-05; regression proved, runtime guard passed)

Native source06bfff66/SHA
`05f4705d791c21484307ce74b9c3cf94c16f580fcc00864694b4887ce4879412`,
run `yotei-integrated-20261005-000502-htile-layer-tracking-noval`,
00:05:02–00:10:00UTC, exits321 with
"sampled HTile import requires its metadata-aware lookup path"
at textureCache.cpp:1624. VS73/PS91/CS478;460 completed compute pipelines.
No timeout/DeviceLost; task-owned PID43356 gone and runner drained/disposed.
Readback150–202 and verified task window black; menu/game entry PENDING.
Corecache loaded118508765bytes, retained124091529bytes after this run.

Before changing behavior, diagnose the exact imported owner/request mismatch
through bounded, diagnostic-only logging, then reproduce the decoded semantics
with a synthetic clear1 HTile owner and deliberately different raw-depth bytes.
Check retained/native pixel or coherent backing results rather than merely passing
lookup. Cover legal attachment/subview/layer reinterpretations only as justified
by image/metadata contracts; retain mismatched metadata, unsupported compression,
bounds and conflicting-owner admission errors. Current tests prove only exact
single-layer promotion and single-layer extent expansion; a different input needs
its own intended native RED and unchanged GREEN plus neighboring numerical tests.
Record required fixture here once actual mismatch fields are captured. Do not
remove the sampled-import guard or invent zero resources just to advance this run.

Diagnostic-only run `yotei-integrated-20261005-001505-htile-import-owner-diagnostic-noval`
00:15:05–00:17:47UTC exits at the same guard, shader counts74/92/497,
478 pipeline completions, shown201 black. Exact diagnostic EXE SHA
`0bc61e11dfd61e78e98ecb6f97be8620b32ea601a4be990f692931fd537a9a22`.
Both descriptions are clean D32Sfloat/k32Float,128x128, pitch128, single-level,
sample1, depth tile, same data and HTile bases and equal per-layer footprints.
Owner is256 layers (depth16MiB/HTile8MiB); incoming depth target describes87-layer
prefix (87*64KiB /87*32KiB), selecting only layer86. Current exact-owner promotion
rejects the legitimate native array subview. No proprietary bytes needed for RED.
Required fixture: import a synthetic65-layer clear1 array with contradictory raw
0.25 depth; resolve hardware subviews0/31/32/63/64, retain the actual65-layer
owner/allocation metadata, verify every selected and neighboring native pixel,
then sample the full65-layer descriptor again. Cover multi-layer view and exact
promotion; preserve mismatched metadata/stride/format/dirty-owner rejection.
Do not shrink allocation metadata to the register-described prefix; that would
break subsequent full-array sampling/whole-owner fills. Temporary diagnostic-only
production logging removed after capture, source restored exactly to06bfff66.

Synthetic original RED confirmed on06bfff66 before behavior changes:
`imported-htile-depth-subviews-red-20261005.log`, test SHA
`b524fa2b9cb5e47fe4e946775b10f7e6be911426d4fa5ed00509f89516641f29`,
exit321/no timeout at the exact sampled-import owner guard, after a real65-layer
native import and READY for hardware layer32. Shared allocation-prefix matching
now compares complete depth/HTile layer strides plus geometry and preserves the
full allocation metadata during final depth acquisition. Native normalized texture
mip descriptions can hold linear mip-tail size/padded height while hardware depth
uses physical allocation size/logical height; those scalar layouts cannot be
compared as identical owners of this single depth mip. Smaller64x64 synthetic
geometry independently exposed that difference; the oracle was not weakened.
Unchanged original GREEN under GPUAV:
`imported-htile-depth-subviews-physical-stride-green-gpuav-20261005.log`, SHA
`0a0b8bfd58b95139de96d5e3f2a868f8244927399b180a223cb47ef12c7a10c7`,
exit0/no timeout, all65 layers/all64x64 pixels across five hardware subviews,
including full-array rediscovery after each selected clear. Additional clear0,
128x128/no-tail and incompatible metadata/format/layer-bound admission controls
are being validated before native emulator build/install/game retry.

Imported-array correction validated (2026-10-05 00:44UTC): extended native
GPUAV executable SHA
`52acd1aa14e92acf8b090461dd4868fc3b8f8e605a9c1b989384ab2c4b5777f8`,
`imported-htile-depth-subviews-final-green-gpuav-20261005.log` and
`imported-htile-admission-final-green-gpuav-20261005.log`, exit0/no timeout.
Original five-subview oracle retained, additional clear0 and128x128/no-tail cases
pass. Every pixel in every65-layer native image checked after selected clears;
full-array identity/range and repeated full descriptor sampling retained. Three
bounded sequential children preserve specific guard rejection for mismatched
metadata base, unsupported format reinterpretation and layers outside the owner.
The two new CTests plus affected HTile-layer/native-subset/sample-array cases pass
8/8 under GPUAV (7.57s). Existing single-layer promotion and extent expansion also
pass (`imported-htile-neighbor-*-20261005.log`, same executable).
No diagnostic logging remains in production; installed emulator is still the
older diagnostic-only0bc61e11 image until the new native build/install. No other
game tested; no full-suite/menu claim. Next commit this shared fix separately,
serialize native build/install/hash verification, preserve pipeline cache and
retry the actual game. Issue108 only actual menu/game entry; no push.


Native runtime verification after local commit `f708edc1`: Windows build/install
PASS, exact EXE SHA
`c73cc32e4bd75cd75c6ab9784989b1c8c96fe7f03f3a2f0a84ae719b08d8e29c`.
Run `yotei-integrated-20261005-005102-imported-htile-depth-subviews-noval`
00:51:02–01:03:12UTC passes the earlier imported-owner guard, VS112/PS97/CS536,
514 successful compute creations. First nonzero readback frame202; actual task
window remains very dark with no legible menu. Total720s deadline, graceful close
exit0, no fatal/DeviceLost; PID43004 gone, streams drained/disposed. Active native
compiler observed and one cold creation151142ms; module1437 validates Vulkan1.3.
Cache138390578bytes retained. Next same-EXE bounded warm-cache continuation;
nonzero pixels are separate from menu/game entry, both still PENDING. Older fill,
compressed-alias and CFG baseline failures are not resolved by this correction.

## HTile clear state beyond32 layers (2026-10-05; new runtime frontier)

Native source30488f8d / installed SHA
`3d3321949886d702d655706aea996847a4fa0be0189728648a647895951bf423`
passes the previous executable-page fault frontier. Actual run
`yotei-integrated-20261004-232320-gpu-executable-protection-noval`,
23:23:20–23:30:56UTC, completes470 compute creations, reaches VS75/PS93/CS493,
then exits321 at `depthRenderTarget.cpp` with stderr
"HTile clear tracking supports at most32 slices". No timeout/DeviceLost;
process27568 gone. Cache loaded99239905bytes and checkpointed118508765bytes.
Readback150–205 and verified window remain black; menu/game entry pending.
Sampled code page retains execution; thread profiles show real NVIDIA compilation
and guest polling. Captured larger CS8457 module1250 validates Vulkan1.3.

Required regression before production changes: independently register a real
33/65-layer depth/HTile owner, prove clear/touch/query state at31/32/63/64 is
independent, and reject indices outside the actual owner. Resolve a high-layer
hardware depth view through the native renderer (not only direct cache APIs).
Test actual selected-layer clear/readback, neighbor preservation and partial
selected ranges across32/64; clearing one view must not clear unrelated layers.
Full metadata fills remain whole-owner operations. Count storage must follow
actual validated image layers and lifecycle, not another fixed integer mask.
Cover same-base reuse/expansion/retirement and native host array-layer admission;
keep mapping/format/unsupported metadata checks and bounded test lifetimes.
Vulkan contracts: resources image creation maxArrayLayers and clear subresource
ranges (`docs.vulkan.org/spec/latest/chapters/{resources,clears}.html`). Guest
DB_DEPTH_VIEW already decodes low and high slice bits;32 is an emulator tracker
representation limit, not the encoded view limit. No blind cap increase or skipped
clear is acceptable. Synthetic RED and unchanged GREEN/GPUAV required, then game.

Native synthetic RED (before production changes, source30488f8d + tests):
`htile-layer-state-serial-red-20261005.log(.stderr/.run.json)`, selector
`--htile-layer-state-only`, SHA
`ba76a3065a6f934955be4fbe7c8129ab3a11cba1c872b58bb1161e8090e9649a`,
valid 33-layer native D32/HTile owner registered before the intended assertion:
whole-owner fill loses layer32. `htile-low-layer-views-serial-red-20261005.log`
with the same executable independently fails after explicit two-layer view clear:
neighboring layers retain incorrectly global clear state. Both assertions are
bounded harness failures, no timeout or DeviceLost. Initial overlapping launches
were repeated serially; the unrelated memory-reservation error is not RED evidence.
A separate hardware-view admission assertion is being captured next. Fixtures use
reusable synthetic33/65/33-layer allocations (same HTile base reused after owner
retirement), independent per-layer bit/value checks, and GPU readback of every
64x64 depth texel for explicit, mixed-deferred and already-consumed view clears.
AMD PAL programs DB_DEPTH_VIEW from baseArraySlice / arraySize and sets depth
clear enable on the selected view:
https://github.com/GPUOpen-Drivers/pal/blob/dev/src/core/hw/gfxip/gfx9/gfx9DepthStencilView.cpp

HTile correction completed locally (2026-10-05 00:01UTC; source30488f8d plus fix):
final original RED executable SHA
`8158ff9d924056069fc7961a56b684a63b8ef0e7febef2d944f77255550fe35b`,
`htile-layer-state-final-red-20261005.log`,
`htile-low-layer-views-final-red-20261005.log`, and
`htile-hardware-admission-red-20261005.log` (specific old32-slice fatal).
The same original cases pass after the correction, SHA
`f514a6b0c245e53a4c21c37aef2ec8ec17b5299d7924cffd07474af55e5e612d`,
`htile-{layer-state,layer-views,low-layer-views}-green-gpuav-20261005.log`.
Shared tracker now stores per-native-owner layer state, bounds queries/touches to
actual native layers, preserves fill value and consumes selected views only.
Mixed deferred views materialize only pending depth layers before attachment LOAD;
whole-view deferred/explicit clears consume every selected layer; full metadata
fills still affect the entire owner. Host image/format/framebuffer array limits
and count-overflow checks replace the emulator32-bit representation cap.

Additional neighboring cases keep the original selected-pixel oracle and cover
pending clears outside the view, uniform selected-view clears and reverse mixed
state. Every 64x64 D32 texel in every layer is compared across five passes and
33/65/33 same-base owner retirement/reuse. Native GPUAV final executable SHA
`4623b0a1e9282cd5afc01987a23ed339798f1192b23ac14331b506429e7d865b`:
`htile-{layer-state,layer-views,low-layer-views,host-admission}-final-green-gpuav-20261005.log`
all exit0/no timeout; three host-limit/count-overflow children reject specifically
before image allocation. Existing native subset, sampled array import,
metadata-aware promotion, expanded alias, depth footprint and feedback controls
pass (logs `htile-neighbor-*-20261005.log`, feedback final-green log).
CTest new four selectors + native_subset/sample_array passes6/6 under GPUAV.
No other installed game was exercised; no cross-game runtime/menu claim.

Two older compute-fill selectors remain RED: sampled HTile discovery at
"complete dword-pattern HTile fill was not recognized", and compute-meta clear
at "bounded read" (snapshot-dependent dispatch incorrectly consumed).
Removing ONLY this HTile correction and restoring the legacy friend fixture
representation reproduces both exact assertions, baseline test SHA
`d3d2fc48b5c9471a6c53c0c11c1d6131e14f7721b85c0256ad8034def39f5ae5`,
logs `htile-old-fill-baseline-{sampled-htile-clear,compute-meta-clear}-20261005.log`.
The sampled fixture does not populate the current uniform_fill proof; the runtime
meta path also permits an unproved coarse clear. These require separate synthetic
contract work; do not weaken their oracles or call the full suite green. All four
fixed production files restored byte-for-byte from local backup; no scoped
baseline reversal remains. Native emulator build/install and game retry pending.
No issue108 update until visually verified menu/game entry; no push.

## Executable guest pages under GPU cache protection (2026-10-05; diagnosis)

New frontier after same-EXE cache continuation: native run
`yotei-integrated-20261004-225751-function-outline-cache-continuation-noval`
completes404 compute pipelines, reaches CS424, then neither pipelines nor
shown203 advances for480s. Exit0/graceful close23:09:18UTC. CPU thread samples
hit guest code and `BufferCache::DownloadBufferMemory`; window capture remains
black. External read-only VirtualQueryEx at a sampled guest PC reports host
protection0x2 (PAGE_READONLY), allocation protection0x40 (PAGE_EXECUTE_READWRITE).
`thread-pcs{,-second}.json` and `window-after-cs424.png` are local artifacts.
This suggests lost execute permission under tracker protection; it is not yet
proof of the full runtime cause.

Native synthetic RED confirmed on the existing implementation:
`gpu-exec-protection-red-20261005.log(.stderr/.run.json)`, selector
`--gpu-executable-protection-only`, SHA
`ebb2807f3cd4a4a4dab8335071fabe158269d9d9a9427c067c1e192df8d0567b`,
exit1/no timeout; actual PAGE_EXECUTE_READWRITE allocation becomes PAGE_READONLY
under temporary read tracking. This is the intended assertion, not a crash.
Shared `Memory::ProtectGuestHostMemory` now preserves execution from current
semantic range permissions, processing mixed spans independently. NoAccess
remains NoAccess; guest permissions are not rewritten; sparse no-ops preserved.
Unchanged native GREEN: `gpu-exec-protection-green-20261005.log`, SHA
`c5058191d18c1e57263872d56fa24cecb30ddd2294995bda404ee9e379710832`,
exit0/no timeout. It checks actual Windows protections, executes a tiny bounded
function returning42, covers NoAccess/release, mixed executable/data pages and
permanent partial revocation. Full native virtual_memory_allocation_tests also
passes (`gpu-exec-protection-memory-neighbors-20261005.log`, sameSHA,8.44s).
CTest guest_gpu_executable_protection+memory_tracker+page_manager passes3/3.

GPUAV `--buffer-cache-range-only` remains RED at the older compressed video-out
metadata alias assertion recorded2026-09-28 below. Current test SHA
`54e71d80e496319356a6189b48d4b9b58434e06937746bfac4b188f2e0173c80`,
log `gpu-exec-protection-gpuav-buffer-cache-range-only-20261005.log.stderr`.
Removing ONLY this memory fix reproduces the identical assertion, baseline SHA
`aec32086729c72e52a0aa95fb234eb2f6c2b5ef1519dcfd64fd16a06d89d6d62`,
`gpu-exec-protection-cache-baseline-20261005.log.stderr`. Fix restored exactly;
full GPU cache suite is not GREEN. Independent `--buffer-cache-gc-only`
passes numerical dirty-buffer publication/readback, CPU fault, ring, partial-unmap
and GC controls under GPUAV, log
`gpu-exec-protection-gpuav-buffer-cache-gc-only-20261005.log`, compute SHA
`c5e755b7a0f6a8ae4e859d7d6849a8024f6665a53961891fc1f7048dbe2f9cc4`,
exit0/no timeout. Native game retry still pending. No issue108 update until actual menu/game entry.

Synthetic contract exercised: allocate executable
and nonexecutable program memory without any game addresses. Temporary host
read tracking must preserve executable permission while blocking writes;
NoAccess must still block access, and releasing it must restore the executable
baseline. Cover mixed spans and a permanent partial permission change so the
tracker neither grants execution to data nor restores revoked execution. Check
actual native Windows page permissions and execute a bounded tiny synthetic
function only after permission assertions. Preserve semantic guest queries,
sparse placeholder no-ops and existing memory allocation tests. No GPU hang
needed for RED; future game retry only after native unchanged GREEN.

## Large CS pipeline before the shared-table frontier (2026-10-04; bounded runtime diagnosis)

Interface-boundary coverage completed (2026-10-05, native Windows CPU):
`TestCooperativeOutlineDerivedPointers` tests Function-array element pointers
at indices0/1 without VariablePointers. `TestCooperativeOutlineArgumentLimit`
imports254/255 independent loaded scalar values plus the original spill object
(255/256 total arguments). Each original module validates before transformation;
loads, arithmetic and stores are preserved. The255-argument cases validate and
outline;256 retains the exact original module, as do the derived-pointer cases.

Scoped removal of ONLY the two existing admission guards from committed00c7df8e
produces intended RED: `outline-boundary-pointer-red-20261005.log.stderr`
rejects an OpFunctionCall pointer operand that is not a memory object declaration;
`outline-boundary-arguments-red-20261005.log.stderr` rejects a256-argument
OpTypeFunction. Both use test SHA
`fce3ddf23864280a08872dae4089436fb41764d73f7977d0e1c14bcbbab4a477`,
valid original inputs and separate pointer/argument selectors. No GPU dispatch.
Production source restored byte-for-byte; `git diff --exit-code` confirms no
production change. Unchanged final GREEN:
`_Build/run-native-regression.ps1 -ExecutableName shader_cfg_tests.exe
-Arguments --cooperative-outline-boundaries-only
-LogName outline-boundaries-final-green-20261005.log -TimeoutSeconds60`, SHA
`86b415cd31c1a46bce0d6a4a4d77b19b257d1313c81df420d3e0f8b077edc685`,
exit0/no timeout. Native CTest isolation+outline_boundaries passes2/2.
Native builds use `windows-local.cmd` through bounded build wrapper; logs
`outline-boundaries-{initial,red,final-green}-build-20261005.log`.
The emulator was not rebuilt/reinstalled during these tests; its accepted driver
cache and executable SHAa5f6ba5c remain intact. Older unrelated failing selectors
remain debt below. Issue108 is not updated: user limits further publication to
visually confirmed menu and then entry into the game.

Native game retry on committed `00c7df8e` / installed SHA
`a5f6ba5c4c74f22333098693cd0e728a3259c1fac3f217c0b06172034dbe6a9b`
now completes all4 CS54904 specializations (98.010–103.827s) and the next
cooperative CSfc6f8c56eb7e168f (97.004s). Run
`_Build/runs/yotei-integrated-20261004-221817-function-outline-noval`,
22:18:17–22:30:20UTC, stops at720s with graceful close/exit0, shown207,
readback150–206 black. No unmatched compute creation remains at stop; native
process read-back is empty. Existing driver-cache checkpoints preserve the
completed work. No menu, GPUAV game validation or cross-game runtime proof.
Command: `_Build/run-yotei-function-outline-20261005.ps1 -ExpectedHash <above>
-TimeoutSeconds 720 -FrameWatchdogSeconds 480 -ReadbackStart 150
-ReadbackLimit 1000 -MaxWorkingSetGiB 28 -NoVulkanValidation -ContinueAfterColored`
CTest focused
isolation1/1 passes; four synthetic fixtures are run by that entry.

Current shared correction preserves Function variables/initializers in main and
passes the original objects as typed Function-pointer arguments. No Private
reclassification remains. The entry arithmetic invariant has valid native RED
(67/529 entry adds, `cooperative-segment-valid-red-20261005.log`,
SHA `d928d185964d31fe5e3eed5a0bfd500e807d79d39c4049d6995b05decafff609`)
and unchanged GREEN (1/1). Function-lifetime RED against the Private prototype:
`cooperative-outline-lifetime-red-20261005.stderr`, SHA
`8af9a8e23df6814931ecdd2239fc825fc754088d1f61c872cdce741c9c74b00f`; GREEN preserves the
initializer/interface in SPIR-V1.3/1.4 and passes the original object to both
helpers. Merge-Phi RED validates the source then rejects the transformed
predecessors (`cooperative-outline-merge-red-20261005.log.stderr`, SHA
`91e9a48fb45fcb85b2dc31a4723d9895143d691251f09c593a05bec32497ad20`).
The ABI now retains arms whose SSA values/labels escape to other entry blocks;
merge-Phi GREEN uses the unchanged source. Atomic RMW arms, imported derived
pointers and interfaces exceeding the universal255-argument limit retain the
original dispatcher; no operations or resources are dropped.

Final focused CPU GREEN through `cooperative-outline-merge-green-20261005.log`
(test SHA `30e71c0a922647c958f9eabb6132fa6c5621cd7d17702fe8344e60e974da73d0`).
Neighbor guard/phase/collective/admission/buffer-cycle/autopromotion/scalar-branch/
shared-bounds/optimizer selectors pass on the Function-pointer variant.
`--cooperative-spill-reuse-only` still fails its older duplicate-guard assertion;
scoped removal of only this outlining effect also fails identically
(`cooperative-outline-spill-baseline-20261005.log.stderr`, SHA
`e19b0896a838dad3dc47edef6c0192c5f217d52bd007f5e748f30d37bd87325d`).
Do not report a fully green CFG suite.

Final native numerical GPUAV Function-pointer variant (compute SHA
`24433adb518a5a5eea5ee67e3b5e8852950d08af45e6981da5a1940fce0a3384`)
passes all9 `--wave64-multiwave-lds-only` controls, including the previously
DeviceLost atomic reduction: `cooperative-outline-final-gpuav-wave64-multiwave-lds-only-20261005.log`.
Cyclic guest barriers, cyclic scalar/physical addresses, BDA coefficients and
cooperative SSBO producer/consumer also pass with independent backing/readback
oracles (`cooperative-outline-final-gpuav-*-20261005.log`).
Unaligned scalar load still returns the old1024/1032 wrong DWORDs
(0xdf579bdf instead of0x13579bdf), matching the separate2026-09-28 debt below;
this neighboring case is not GREEN.

Exact CS54904 diagnostic Function-pointer rewrite validates, outlines856 arms
and retains4 atomic arms. The same exact-layout pipeline probe completes in
106s at about2.3GiB sampled working set, flags disable-optimization:
`cs54904-function-pointers-pipeline-20261005.log/.run.json`.
This is pipeline-only evidence on a diagnostic rewrite. Actual native emitter
pipeline completions are recorded above; large CS8457 and menu remain pending.

2026-10-05 continuation: exact current module captured at
`_Build/runs/yotei-integrated-20261004-210755-cs54904-capture-noval/shaders/0001_new_shader_cs_54904fb419d79e49.spv`.
It validates for Vulkan1.3, has860 dispatcher arms and352 Function variables;
buffers binding0 has27 descriptors, auxiliary bindings51–54 have one each.
Exact-layout native probe times out60s with both driver optimization modes;
moving Function spills to invocation-private globals alone also times out60s.
Diagnostic outlining of all860 complete arms, with typed SSA imports, private
spills and unchanged barriers/branches/operations, validates and creates its
pipeline in88.3s with disable-optimization (`cs54904-isolated-outlined-disableopt-20261005.log`).
This is compiler evidence, not numerical execution or a menu.

GPUAV on the first outline prototype passes LDS exchange128/256, then returns
DeviceLost during `Wave64MultiWaveLdsAtomicReduction`
(`cooperative-outline-gpuav-wave64-multiwave-lds-only-20261005.log`, test SHA
`b5aba09949ed0b884e8830a26ccd32149c40f24ebb8a209676f35b0d7b0d511e`).
Do not repeat the unbounded failing path to obtain another driver reset.
Required CPU boundary regression before refinement: a synthetic two-arm
dispatcher contains a workgroup atomic read-modify-write in one arm and ordinary
arithmetic in the other. Preserve both operations and valid SPIR-V, but keep
the RMW in the original uniform entry dispatcher rather than crossing a new
non-inlined function call boundary. Then unchanged GREEN and original numerical
GPUAV controls must pass. This is a conservative outlining boundary; it is not
proof that Vulkan forbids atomic operations in functions or a diagnosed driver
root cause. Original supported guest atomics must remain intact.

Keeping RMW segments in the entry dispatcher alone still returns DeviceLost in
the same numerical fixture (SHA `16c4f7a598a470e81ff12108e043768668841cc4b68f7d59b8153f02d30b43cd`).
Stop GPU retries of this prototype. Next CPU lifetime RED: the original Function
variable and initializer must retain their storage/lifetime, and each outlined
use must refer to that original object through an explicit Function-pointer
parameter. The Private reclassification prototype must fail this contract.
SPIR-V1.3/1.4 validation must cover the retained initializer and interface, and
derived pointer arguments must retain the original path unless their required
capability is proved. This changes the earlier implementation-specific Private
expectation to a source-lifetime invariant; the guest arithmetic oracle remains
unchanged. Do not call either failed Private prototype a completed fix.

Required synthetic RED before implementing: compare cooperative dispatchers
with2 and16 synthetic guest blocks, each containing the same observable
arithmetic chain and retaining guest barriers/transitions. The entry function's
arithmetic body must stay independent of the number of dispatched segments,
while the module retains all arithmetic work. Require valid SPIR-V before
the intended isolation assertion fails. Then unchanged GREEN plus numerical
cyclic LDS, scalar/buffer-address, EXEC/collective, Phi and multi-workgroup
cases must prove invocation-private state and typed function imports. Preserve
initializer lifetime and SPIR-V1.4+ entry interfaces. Contract:
[SPIR-V storage and functions](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html).

Local fix `2bfde86d` is built/installed with native Windows SHA
`801a4195145dab7466efc9aeb81bf7fe27a67401df861ff79e05db2a0aeb8707`.
Run `_Build/runs/yotei-integrated-20261004-205642-sharedbuffer-noval`
used1280x720/Fifo/optimizationNone without GPUAV, Vulkan or shader validation.
It reached shown155, with black source readbacks150–154, then exceeded its
28GiB working-set guard inside CS54904 `vkCreateComputePipelines`:
568075 emitted words, cooperative wave64, flags0x1. PID38544 was stopped;
no task-owned native processes remain. This also blocked the older no-validation
run before the new change (566635 words); the small word increase does not
identify the underlying expensive operation. No compiler completion, menu or
performance improvement for this specialization is proved.

Required next diagnostic: capture this exact emitted module and admitted layout,
validate it, and run a bounded pipeline-only probe before another long game run.
Classify repeated resource accesses, execution/control flow and other expensive
operations using the module, rather than its hash. If production behavior is to
change, first add a reusable synthetic RED that preserves every candidate,
address/bound and observable value; use unchanged numerical GPU/GPUAV GREEN
and affected wave32/wave64 neighbors. Do not raise the memory limit, truncate
candidates, suppress guest work or specialize by game/hash/address. A compiler
timeout alone is diagnostic, not a semantic synthetic regression.

## Formatted access byte bounds with host backing offsets (2026-10-04; native RED/GREEN)

New shared-table formatted numerical fixture passes plain readback but GPUAV
reports a CAS DWORD beyond candidate514's 4256-byte descriptor range, lane2.
The existing formatted bound compares unrebased address against a rebased host
limit; raw DWORD/byte paths instead include the offset in element indexing.
The minimized unchanged three-candidate fixture with sharing disabled only in
its test host profile reproduces GPUAV OOB at bound160/access163 and175 on
lanes2/3 (`formatted-bounds-baseline-red-20261004.log.stderr`, native test SHA
`293433c302241085396d81faef4861985b4dc3fcffa9762d6d2cd6ff0e46fa15`).
This proves the old specialized path's defect independently of nonuniform access.
The common helper now checks the actual DWORD pointer's host-base adjustment
and both additions for overflow. Fractional host-offset reconstruction is not
implemented by this change; retain the older unaligned-access debt below.
Same fixture GPUAV GREEN, whole-backing oracle unchanged:
`shared-buffer-final-gpuav-SpecializedBoundedBufferFormattedByteAccess-20261004.log`,
SHA `03310d19ef4a18ef952c55414f04a99628251f6ea2040176190fb41cfbdc6811`.
Independent emitted-SSA bounds oracle covers first/last byte, widths1/2/4,
zero bounds, base-add overflow and end-add overflow in the focused CFG test.

## Dense buffer capacity supplied by the host (2026-10-04; native CPU/GPU proved, game path pending)

Diagnostic native4481+trace run163905 naturally exits321 at16:45:20.549UTC,
shown219/first RGB spinner scale; no menu/DeviceLost. Installed diagnostic SHA
b174a05d307500c539f06e4c554666533ed2ae2bc484902f84df6b1618e230ef.
CS8457 complete snapshot88533unique source words. Trace shows root1 six,
root2 373 and root3 133 candidates at failure (one direct buffer); no non-null
cross-root payload matches among those roots and no non-buffer descriptor types.
Thus neither compatible sharing nor early non-buffer null normalization resolves
this actual capacity frontier. Trace/analyzer are ignored diagnostic artifacts.

Required native RED: one synthetic finite table contains513 genuinely distinct
valid buffer descriptors with an explicit host admission capacity513. Check
all raw backing descriptors, logical row mappings, origin metadata and immutable
source footprint. Current materializer incorrectly applies unrelated fixed512.
Keep default offline/logical-root policy512, and reject exact host-capacity+1
transactionally, including zero/low capacities. Replace dense backend arrays
with admitted-shape storage; no guessed larger global constant. Native renderer
must supply actual min(stage/set storage-buffer limits); final DescriptorBudget
still includes auxiliary descriptors and all resource classes/stages. Numerical
GPUAV should read distinct rows beyond512, write through a final candidate,
verify all outputs and full backing, and pass actual device descriptor checks.

Native CPU RED `dense-buffer-host-cpu-red-20261004.log.stderr`, SHA
f755cdcb20e8decec404f3e72d0b033373c1a10cee22c9eb3b6d9e94756aa06a,
rejects513 valid unique candidates despite explicit host capacity513.
Unchanged focused and FULL ResourceTracking GREEN5.135s, SHA
523f98bbf7e0d375b4a8d39b7d4fc4c4823ab250b4a3a567ad8fa64590fc971e:
513/1024/512/1 exact values/maps/footprints/live binding storage; host plus-one
and zero capacity preserve caller outputs. Native DescriptorBudget PASS SHA
aac7fedca7c041590467b8345270fb67fb48ef89a8be39ae6f2260f272725d4d:
stage/set/all-resource ceilings, dense513+two auxiliary exact/overflow checks.

Final numerical GPU RED `dense-buffer-host-gpu-final-red-retry-20261004.log.stderr`,
SHA209b53556ed1001bc22f5e321641a7d92a9f4ff903fab80f20b1923cdd789f7c:
with ONLY core admission guard reverted, rejects513/512 before dispatch.
Fixture has515 distinct legal strides at shared base128; keys0/512/514/515,
last-candidate write and whole backing oracle. Earlier large-address fixture
GREEN attempt stopped at the harness eight-bit adjustment constraint (not a
production failure/valid GREEN); revised oracle was rerun RED unchanged.
Initial final RED attempt failed guest reserve13.5GiB due Windows commit
pressure, not a semantic RED. Task log cache eviction and one WSL clean-file
cache drop restored headroom; no application stopped or guest reserve reduced.
Scoped core reversal restored from ignored fixed backup after final RED.
Native device query reports RTX5060Ti stage/set SSBO1048576 and resources
4294967295. Bounded vulkaninfo full format dump timed out30s/killed/drained;
acquired properties are evidence, not an exhaustive query PASS. GPU fixture
also obtains actual properties and checks the full native descriptor budget.

Unchanged final numerical GPU GREEN without GPUAV shader instrumentation:
`dense-buffer-host-gpu-final-plain-green-20261004.log{,.run.json}`,
SHA7b849109b85fe32be8f35e78920457442d69a843f915048a190ae68c679fb06d,
66.2s, exit0. The fixture checks keys0/512/514/515, last-candidate write,
full backing and actual device descriptor budget. Neighboring
FiniteScalarBufferDescriptorExtent, FiniteScalarBufferWrappedAliases and
FiniteScalarBufferFullSnapshotDomain numerical GPU cases passed on the same
test SHA (`dense-buffer-host-neighbor-{extent,wrapped,full-domain}-20261004.log`).
GPUAV-instrumented attempt
`dense-buffer-host-gpu-final-gpuav-green-20261004.log{,.run.json}` timed out
at120s inside `vkCreateComputePipelines` after thousands of descriptor/OOB
instrumentation passes; it is neither a numerical failure nor GPUAV GREEN.
Final native full ResourceTracking run also passed on rebuilt SHA
5bd6017cafbc78b272ee4494ebf49d1a4fb4b0482865549dab76515ca0445391
(`dense-buffer-host-final-full-cpu-20261004.log`). Native emulator
build/install passed; installed SHA
045ceb1b29e313ce236335a65008debdfc69c26902c02fbd651ada77f25dcbc6.
The 1280x720 game attempts below stopped before CS8457 materialization, so
the real 513/512 frontier remains unverified after this correction.

## Wave64 pipeline compiler and GPUAV instrumentation (2026-10-04; isolated, mechanism pending)

On installed SHA045ceb1b29e313ce236335a65008debdfc69c26902c02fbd651ada77f25dcbc6,
1280x720 run `yotei-integrated-20261004-173254-presentfix-gpuav` with GPUAV
shader instrumentation off crashed in `nvgpucomp64.dll` at
`vkCreateComputePipelines` for CS b90e2024732c6111 (Windows exception
0x80000003, module offset0x589eb2); shown0. With instrumentation and descriptor
checks on, `yotei-integrated-20261004-173407-presentfix-gpuav` reached shown255,
but readback150-254 was black and `VkLayer_khronos_validation.dll` access
violation stopped CS753c552fae650ec4 pipeline creation. A second bounded
capture run `yotei-integrated-20261004-174018-presentfix-gpuav` reproduced the
same CS753c failure, shown128, and saved exact8358-word SPIR-V at
`shaders/0000_new_shader_cs_753c552fae650ec4.spv` (SHA256
99a7bd0ab9e87f79c42b4145737d9fdeb26fa8ddd2af42791fa52c5150d8dfc0).
The previous 2560x1440 run163905 had created this pipeline successfully;
that does not prove the 1280x720 path or descriptor specialization.

Bounded isolated probe on the exact module with a valid push-descriptor layout
(bindings3 sampled image,41 storage image,49 sampler,51-53 storage buffers),
128-byte push constants, and disable-optimization flag: without GPUAV passes;
with game-equivalent GPUAV descriptor checks crashes in
`VkLayer_khronos_validation.dll` at offset0x890cb4, matching the game's
exception PC offset; changing the optimization flag does not help. Switching
only GPUAV descriptor checks off makes the probe pass. An earlier probe without
the required push-descriptor extension also crashed in validation and is not
valid evidence. Game run `yotei-integrated-20261004-174908-presentfix-gpuav`
with descriptor checks off returned to the b90e NVIDIA crash, shown0. No mode
currently proves both pipelines on this 1280x720 path.

Required regression before a production change: minimize the saved modules
through isolated bounded probes, identify the SPIR-V operation/interaction
that provokes each compiler, then encode it as reusable synthetic wave64,
barrier/LDS/image cases with a meaningful pre-fix RED and unchanged post-fix
GREEN. Preserve descriptor and execution semantics, test affected wave32 and
multi-wave neighbors, and verify core Vulkan/GPUAV modes separately. Do not
special-case either hash or suppress required GPU work. A driver/layer crash
alone is diagnostic; avoid repeated full-game crashes or GPU resets while the
smaller probe can answer the question.

## Cross-root dense buffer admission (2026-10-04; diagnosis, regression pending)

Native4481e348 run161735 naturally exits321 at16:24:48UTC on
CS8457901d80b91921: logical buffer3,65536rows/stride592,133 candidates
at dense binding513/512. Shown224/firstRGB210; no menu/DeviceLost.
The preceding bounded snapshot succeeds with88533distinct source words.
Static header-only CPU audit `cs8457-buffer-plan-audit-20261004.log` PASS:
logical1/2 read-only; failing logical3 is read+written, not atomic. The163905 trace disproves useful non-null overlap among roots1/2/3. Diagnostics may
capture full four-word candidates and semantic metadata before admission.

Required synthetic regression once the mechanism is confirmed: independent
finite descriptor roots can reference identical backing with different row
orders. Verify each root's key-to-dense mapping and exact values. Reuse only
semantically compatible descriptors, retaining source coherence, all rows,
null defaults, metadata distinctions and the actual512 unique binding limit.
If sharing writable candidates, prove union of per-root selection/write
footprints: an unselected alias in one root must not zero a resource selected
by another; failed selector proof keeps conservative writes. Preserve protected
source overlap rejection and transactional failure. Native numerical GPUAV
must check both roots' outputs/backing, with compatibility and cap controls.
No production correction before an intended native RED. No blind cap increase.

## Bounded snapshot source domain versus storage capacity (2026-10-04; native CPU/GPU proved, actual source quota cleared)

Native8d04ad39 retry154518 naturally exits321 at15:54:58UTC onCS8457:
read59 reaches65537 DISTINCT source words. Alias correction is independently
proved but does not solve this actual variant. Shown221/firstRGB206,
window1550 visibly shows loading spinner, no menu/DeviceLost; PID42744 exited.

Required synthetic RED: two independent finite scalar columns have32769 keys
and65538 distinct source DWORDs, all values/layout/immutable ranges checked.
Then65536 keys ×2 disjoint columns (131072 words); both comfortably below
existing64MiB snapshot capacity. Numerical GPU RED/GREEN: full16-bit selector
reads four genuinely distinct columns from65536 valid repeated descriptor rows
(262144 source DWORDs,1MiB), keys0/1/65535 all return real backing values.
Descriptors themselves deduplicate normally; do not alter descriptor limits.
Preserve per-column key65536, workgroup reservations, failed callback/rollback,
SRD OOB/U32 wrap, and exact64MiB/plus-one storage controls. Source-work bound
must derive from already admitted dense rows: each distinct source DWORD
corresponds to at least one stored row, therefore unique_words <= stored_rows
<= MaxBoundedSnapshotWords. Do not guess a new larger probe constant.

Contract checks: FlattenedSrt is one SSBO; LoadBoundedFlatWord uses U32 count/
offset. NativeUpload copies data.size_bytes() into the64MiB stream buffer;
StreamBuffer::Map rejects requests larger than backing. Vulkan's
[maxStorageBufferRange](https://docs.vulkan.org/spec/latest/chapters/limits.html)
minimum is128MiB, so64MiB policy remains below that hardware contract. The
old independent65536 global probe quota is a software policy inherited from
single selector domains, not a U16 flat index or a descriptor/API capacity.
Worst-case coherent snapshot/cache work remains bounded by admitted rows;
full worst-case all-distinct64MiB cache memory/performance is not yet measured.
No production change before valid native RED.

Native CPU RED `bounded-snapshot-domain-cpu-red-20261004.log.stderr`, SHA
9d8fb1b502d258ad4f1fb4fa0220ec8f64e07a4750b71990b54ddeeaf6b9633b:
rejects65537th unique word in65538-word/256KiB fixture. Native GPU RED
`bounded-snapshot-domain-gpu-red-20261004.log.stderr`, SHA
c945a54e0605cd7950462570c1cc36f9bb90a1b5dd0177fcf699585db1ac7f5c:
rejects65537th word before dispatch of1MiB/full-selector fixture. Both REDs
are on production8d04ad39, before the shared budget correction.

Shared guard now derives from MaxBoundedSnapshotWords (existing64MiB/4),
using the invariant unique bounded source words <= admitted logical rows.
No guessed larger quota, no descriptor/per-column/workgroup/storage cap changes,
no skipped row/read. Old global65536 quota oracles are superseded by positive
cross-boundary values and an actual failed-source callback/rollback test.
Full native ResourceTracking GREEN `bounded-snapshot-domain-full-cpu-green-20261004.log`
(4.825s), nativeSHA b84355f8e70b1ef4a6d291f0d7ad8cfed05fa6e174eddcc1e57570c122c2ddb1:
unchanged new65538/131072-value oracle, per-column65537 rejection before reads,
exact64MiB/plus-one storage, immutable ranges and affected existing cases pass.

Unchanged numerical native GPUAV GREEN `bounded-snapshot-domain-gpuav-green-20261004.log`
(0.70s), SHA ec5866ca46b6259c9d7d7d40a4904e7ca3f5eb99baae938dd51f5dc3a54af855:
full65536-row/four-column SRD has262144 genuinely distinct source DWORDs;
keys0/1/65535 all return100+lane. All12outputs and entire1MiB backing checked;
dense descriptor identity remains1. Same exe passes wrapped aliases/SRD extent
GPUAV neighbors. Fixture wave32; actual4481 retry161735 retains81columns/5308444words/
88533unique words, then fails independently at dense buffer513/512. Full
actual shader specialization, other-game runtime and worst-case all-distinct
64MiB cache memory/performance remain unproved.

## Bounded SRT snapshot aliases (2026-10-04; native CPU/GPU proved, actual retry pending)

Native df835e1e retry151821 exits321 at15:27:21UTC on CS8457901d80b91921:
read59 charges65580 logical in-bounds references against65536 probes.
Shown210, firstRGB201/max214colored pixels; offscreenwindow1523black, no menu.
No DeviceLost; PID37208 exited. CS0102 passes3buffers/SPIR-V3283words/pipeline66ms.

Required synthetic RED: two finite65536-row columns alias two coherent source
DWORDs; preserve every logical value/offset while reading each source once.
Also test overlapping columns, U32 wrap and SRD OOB zeros, raw addresses,
exact65536 distinct source words and plus-one rejection, failed source reads,
transactionality/immutable ranges and the existing64MiB storage bound.
Keep the65536 unique-word budget and per-column selector/workgroup reserves.
The existing SnapshotReader caches coherent DWORDs by address; alias references
are dense output work already bounded by storage, not additional memory probes.
Do not change quota constants or discard rows. CPU RED/GREEN and native GPU
numerical readback are required before retry; actual CS8457 alias count remains
to verify. Earlier repeated-address quota tests must be replaced by genuinely
distinct-word quota controls, with the reason recorded.

Native CPU RED on productiondf835e1e: `bounded-unique-words-red-20261004.log.stderr`
rejects131072 logical references despite2 source words; nativeSHA
c8f57d7e5619381b6bface2597b3d02468cf4728ac577a0e62a2cc3b4002aa8b.
Unchanged primary oracle GREEN and full ResourceTracking pass
`bounded-unique-words-final-resource-suite-20261004.log` (5.75s, nativeSHA
9c5b5e65b32ac78e396147ff6c813b6cfed6ff12a2ef8267834b9c5aa217e3bb).
Exact65536 DISTINCT raw source words/plus-one, failed callback/rollback,
overlapping/wrapped/zero rows, immutable ranges and existing storage/workgroup
reserves pass. Previous repeated-address quota expectation is superseded:
SnapshotReader already returns one coherent cached value per address; distinct
resolved words consume memory work, dense aliases consume bounded output storage.
The constants remain65536 unique words,65536 per-column keys and64MiB storage.
Mapped/foreign clamped addresses conservatively occupy a unique slot too.

Native GPU RED with ONLY this core correction absent:
`bounded-unique-words-gpu-red-20261004.log.stderr`, SHA
 d43da004b340227c2b5dd15c3b16792dc98a536469ce03b03dd0d72c22e45fd7,
rejects third column98304references before dispatch. Unchanged numerical
GPUAV GREEN `bounded-unique-words-gpuav-green-20261004.log`, nativeSHA
c24756300940223520f7689dd3cd80e8cb584d73e9631ad5768c08fac72b3071.
Finite16-bit keys0/1/2/65534/65535 shifted31bits wrap even keys to the same
four descriptor words, odd keys are SRD OOB. All20 outputs and full backing
are checked; this independent GPU fixture uses wave32. Existing descriptor
extent, sparse three-row scalar-loop and4 zero-stride candidate GPUAV neighbors
pass on the same exe.
Original CS8457 unique-word count/pipeline, native emulator retry and other-game
runtime remain pending; no menu/gameplay claim.

## Cyclic single-wave buffer atomic returns (2026-10-04; synthetic CPU/GPU and actual CS0102 pipeline proved)

Native a1ae8a8d run143716 naturally exits321 at14:45:08UTC on
CS01025cd5c3102c4c: wave64 splitting does not support live BufferAtomicIAdd32
return values. PriorCS596 passes258images/752pairs and pipeline8784ms.
Shown197, source readback through196black, no colored frame/menu/gameplay;
no DeviceLost. PID19912 exited.

Required bounded synthetic RED: one complete guest wave64/native subgroup32,
a fixed finite loop with a predicated buffer atomic old value used by stores
and ReadLane-derived uniform control. Keep one64-invocation host workgroup,
per-lane old-value ownership and post-instruction publication. Numerical
native GPU cases must have an independent iteration cap, both physical halves,
sparse/inactive predicates, multiple counters/alias feedback, and real bounds.
Reject unproved partitioned/cooperative atomic communication, raw varying
branches and existing LDS/GDS unsupported loops. Use current shared atomic
emitter and split-wave publication mechanism; no dropped return or zero stub.
Atomic RMW is indivisible; the guest GLC return is the value before the update.
Record native planner RED before admission change, unchanged GREEN/SPIR-V and
numerical GPUAV neighbors, then retry the game.

Native final synthetic planner RED on unchanged productiona1ae8a8d:
`cyclic-buffer-atomic-final-red-20261004.log.stderr`, native exe SHA256
5dae65a4899733cae664718fe390c557de298dd8d842fe1880e0c788fbcb06b6:
exact rejection is live BufferAtomicIAdd32 return. The correction admits cyclic
DWORD buffer returns only in one complete direct guest wave and requests the
existing post-instruction AcquireRelease/UniformMemory workgroup rendezvous.
Convergence checks still reject raw varying branches; cyclic wide/shared/GDS
and partitioned/cooperative buffer communication remain guarded. Native
unchanged GREEN and numerical GPUAV are pending.

Unchanged planner GREEN `cyclic-buffer-atomic-green-20261004.log` and four
native numerical GPUAV cases pass in `cyclic-buffer-atomic-gpuav-20261004.log`
(lower/upper ReadLane, sparseEXEC, contended shared counter; native SHA256
cd01efa0ee6ca15fc212541d6be2e9d817e9ebbc639ff1eb0e8aeb384f639ed2).
An expanded legacy convergence suite exposed an obsolete barrier expectation:
scenario8 expected128-thread uniform guest barrier rejection but current
cooperative planner accepts it. Reversing ONLY the new core patch reproduces
that same failure (`cyclic-buffer-atomic-baseline-neighbors-20261004.log.stderr`,
SHA956521a8b22da8f4c5277d4d968805a1cbd5021b81bf68d8b74dac5642d6a029).
The old expectation is preserved as pending debt, outside the focused neighbor
gate. A second legacy feedback assertion (scenario4/PollBallot,128invocations)
expects rejection despite existing cooperative promotion; reversing ONLY the
new core patch reproduces it too (`cyclic-buffer-atomic-feedback-baseline-20261004.log.stderr`,
native SHAaefa7c42c850e123b42db98fc347535eacca6ae95ef130e7fd13de9a3a572086).
Neither legacy oracle is weakened in this correction. SavedCS0102 CPU IR audit
(`cs0102-resource-ir-audit-20261004.log`) finds a predicated atomic returned
through ReadFirstLane, with sparse EXEC and per-lane prefix offsets; add lower/
upper-active ReadFirstLane numerical controls before original retry.

Final native planner/regression and focused existing GDS/LDS/cooperative/image
publication neighbors pass (`cyclic-buffer-atomic-final-{planner,focused-neighbors}-20261004.log`,
SHA c1c790f633a60ab421696799e9196a5466cb2f0582b969ac1f5e499c20f0c94c).
Six numerical native GPUAV cases pass (`cyclic-buffer-atomic-final-gpuav-20261004.log`):
per-lane old values AND direct ReadLane/ReadFirstLane broadcasts through each
loop visit, sparse/inactive lanes, upper-only EXEC, contended counter unique
slot allocation. An independent three-iteration bound prevents GPU hangs.
Same native exe SHA578c5186a19b122de66274e31a9ae59ffa7b72ab836e27197264028accc6881a
passes9 cooperative LDS/barrier/image-atomic neighbors and buffer integer
atomic family/GLC0 tests in `cyclic-buffer-atomic-{cooperative-gpu-neighbors,family-gpu-neighbor,glc0-gpu-neighbor}-20261004.log`.
No core emitter changes, no work/return dropped, no guest-specific branches.
Native df835e1e retry151821 confirms actualCS0102 passes3buffers, SPIR-V3283words
and pipeline66ms before the later CS8457 snapshot blocker. NonzeroRGB201/
shown210/window1523black; menu/gameplay and other-game runtime remain pending.

## Sampled pair graph versus separate operand descriptors (2026-10-04; synthetic and actual CS596 pipeline proved; menu pending)

Native source8a496a73 run `yotei-integrated-20261004-140111-presentfix-gpuav`
naturally exits321 on CS59630740d07d5a1c: specialized sampled pairs exceed512.
CSb629 now passes261 images/504 pairs and pipeline creation9688ms. RGB197 /
shown199 and the black offscreen window do not prove a menu.

Required synthetic RED:257 genuinely distinct sampled images with two samplers
produce514 distinct logical pair edges, but only257 image and2 sampler bindings.
Also cover inline specialization expansion beyond512 pairs, root-local selection
and defaults, both samplers, exact operand boundaries and transactional rejection.
Verify native numerical GPUAV readback of a bounded table with more than512
pairs and actual device descriptor-budget rejection; retain MaxImages512 and
MaxSamplers32 and all source/type/coherence/probe checks. The maximum unique
pair graph is derived from its validated operand domains (images × samplers),
not an independently guessed descriptor capacity. BindingLayout uses separate
images and samplers; OpSampledImage creates a local SSA value. Vulkan distinguishes
[separate and combined descriptor types](https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorType.html).
No production correction until the native synthetic failure is recorded.

Native REDs on unmodified production8a496a73: tracking
`sampled-pair-domain-red-20261004.log.stderr` rejects required513/limit512;
specialization `sampled-pair-materialization-red-20261004.log.stderr` rejects
the independently asserted262 images /2 samplers /522 usage edges.
The preceding intentional failed-read neighbor also logs its expected error.
The shared bound now derives from MaxImages × MaxSamplers; actual descriptor
capacity remains unchanged. Old tests expecting512 usage edges to be an
independent descriptor limit are corrected to operand counts and plus-one
image rejection; no existing pixel oracle changes. Final tracking fixture uses valid full descriptors and normal materialization;
scoped reversal of only the bound reproduces the513rd-edge failure again
(`sampled-pair-domain-final-red-20261004.log.stderr`, native SHA256
58c6622cae97084ef32804c84f0599807d4f6324fe3b4f7f97023f409d4c748d).
Unchanged final GREEN and full ResourceTracking pass (exact512×32/invalid513th
image transactional boundary included). Native GPUAV passes520 numerical values
with262 images/2 samplers/522 edges, rotated key order and clamp/repeat; small
shared-root and compact dynamic-sampler neighbors pass. DescriptorBudget full
suite passes real exact/over-budget operand checks. Logs are
`sampled-pair-domain-{final-green,final-resource-suite,gpuav,shared-neighbor,dynamic-neighbor,budget-green}-20261004.log`.
Native GPU exe SHA25605c56fb2b414fc66ad6b24d78ee1b58452c2eee480c420d1a994e68c5c004c58.
Original-game native a1ae8a8d retry143716 FINISHED exit321. ActualCS596 admitted
8buffers/258images/752pairs, SPIR-V354059words, pipeline8784ms and immediate
427958103-byte cache checkpoint; subsequent shaders pass untilCS0102 live cyclic buffer atomic return rejection. No menu/gameplay
or completed actual dispatch readback yet. Other-game runtime remains pending.

## Inline sampled-image selector domain and aggregate admission (2026-10-04; pending)

Source `79695c15`, native installed SHA256
`af0e73da0b42541006d955ec101e74c0136f6b8e380bfa0f4ed054d44c4fc91b`,
completed run `yotei-integrated-20261004-132158-presentfix-gpuav` exits321
at CS `b629e5773956d33c`: dense image resource limit exceeded
(size13640 stride440 probes1677 pairs245 accumulated images753).
The preceding CS4d6 pipeline passed (329910 words, 26247ms); first nonzero
RGB195/shown199 do not prove a menu. No DeviceLost is recorded.

Required next proof: inspect the live selector's normalized IR and its
unavoidable guards. If they establish a finite unsigned domain, reproduce
that domain with independent synthetic inline sampled-image tables, including
multiple roots and wrapped U32 keys. Reject missing/opposite/bypassed guards,
per-lane counts and incompatible descriptor/sampler domains. Preserve wrapped
interior aliases for unbounded selectors and existing aggregate limits.
Read-only IR finds three full-width inline sampled roots with the same buffer
and stride440, offsets0/32/224, and no finite selector guard. These domains
overlap in their wrapped gcd8 offsets. Required admission RED is two synthetic
roots with 260 distinct full-width images in different key orders: the union
and separate null defaults fit512; duplicate per-root expansion exceeds512.
Keep each root mapping/default and ordinary sampler uses, upper descriptor words,
validation, source coherence, probe budgets and transactionality. Only identical
descriptors with equal sampled-image semantics may share a dense slot; dynamic
sampler origins and distinct type/format/swizzle metadata remain separate. Native CPU RED must precede
a production fix, followed by unchanged GREEN and numerical native GPUAV
selection/alias/extent neighbors. No blind MaxImages increase or deduplication
across distinct typed roots. Existing sampled-pair admission debt below remains.

Synthetic native RED on source79695c15 (test exe SHA256
`1c5f40a96c81d83ca63445b4b6019502bdf98c6e15e5126eb7eb6d3282cd608e`):
`shared-inline-images-red-20261004.log.stderr`, size114400/stride440,
260 probes/261 candidates per root, aggregate522. The unchanged positive
oracle passes after sharing exact non-null descriptors of compatible ordinary-
sampler inline sampled roots. Each root retains its own ordinal-to-dense vector,
null/default and wrapped key mapping. Typed metadata, dynamic sampler origins,
all source reads, clean snapshots and all budgets are retained. Admission now
charges genuinely allocated dense images; MaxImages and MaxSampledPairs stay512.

`shared-inline-images-final-neighbors-20261004.log` and full ResourceTracking
`shared-inline-images-final-resource-suite-20261004.log` pass (native exe SHA256
`c6381bda9556a39a928d794aa3899af11e1a61bf89483ca7b9ebb62eabb979c3`).
Controls cover changed upper words, incompatible root numeric proof, separate
ordinary samplers and pair capacity, exact512/plus-one513 distinct admission,
failed reads of already-duplicated descriptors and transactional snapshots.
The existing unbounded full-width case now shares two full descriptors across
both roots while retaining wrapped gcd8 aliases and partial final-dword bounds.
An initial GPU fixture invocation failed a harness distinct-origin count check
before dispatch: dense candidates are not distinct SRT sources. Its dense count
check was corrected without changing expected pixels; this is not GPU RED/GREEN.
Final native GPUAV selection passes two different key orders and independent
clamp/repeat samplers (`shared-inline-images-final-gpuav-20261004.log`, native
exe SHA256 `006291d1f063d213eac8007da293d177d2b8f74536a110db22838e859ca3846a`).
Dynamic compact sampler, dependent image-table and actual wave64 read-loop
numerical neighbors pass on the same executable; wide explicit topology SPIR-V
passes. Final capacity-resource full suite also passes. Original retry pending.

## Wave-selected scalar table row under an equality bucket (2026-10-04; synthetic proof complete, actual pipeline passed)

Native source `8ad1ff36`, installed exe SHA-256
`78e857bc52e9c01add76e2a3e1b0e9507d1fb24371774098940bd2bd9b34ffc2`,
run `yotei-integrated-20261004-124226-presentfix-gpuav` passes the old
CS4d6 PC0x284 frontier and exits321 at PC0x41c. All four descriptor words
are scalar buffer reads with a ReadLane-selected row; their direct roots
are user data. Nonzero RGB starts at frame198, shown207; offscreen
`window-1252.png` shows only a loading spinner. No menu is proved.

Required synthetic RED: a wave-uniform ReadLane key equals a per-lane
source in an unavoidable nonempty EXEC/VCC bucket, and that same lane's
source is unsigned below a host-evaluable count. The key must stay live on
GPU while two different formatted descriptor rows materialize. Include an
invariant predicate Phi and the active-and-not-outside boolean recipe.
Reject OR, signed bounds, unrelated equality sources, changing predicates,
empty-edge reads, bypasses and per-lane counts. Then native GPUAV readback
must cover sparse masks and both halves of actual wave64 without weakening
descriptor limits. The existence of a matching lane, rather than a guess
about which lane ReadLane selected, establishes the unsigned key bound.

Native RED on the unfixed production source is
`wave-table-witness-red-20261004.log.stderr` (test exe SHA-256
`57376576b2910f2f1e5736d07b1eea7c5ee8b646abb54dffd9e918badc26a3cf`).
The unchanged positive oracle passes after the shared correction; all six
predicate/polarity variants and eight rejection controls pass. Full
ResourceTracking is GREEN in `wave-table-witness-final-resource-suite-20261004.log`
(exe SHA-256 `c80d8b5043c282fb49ddd2472ca233a87da96025b6ef3fca52e56cbd0ac003ce`).
Eleven native GPUAV readbacks pass (`wave-table-witness-final-gpuav-20261004.log`,
exe SHA-256 `1c582d5f84ebe449f2cacca1af6503c85f474ba70117fab5c6f70193a3c6f825`),
including sparse wave64 even/odd masks, inactive leader lanes, N0/N1/N2,
distinct float descriptors, OOB zero and full VCC copies in wave32/wave64.
Nine neighboring induction GPUAV cases pass on the same executable.
Saved CS4d6 now passes the CPU resource audit with both barrier profiles;
actual game retry132158 now passes materialization and pipeline creation;
completed GPU execution is not independently proved.

**Separate unproved representation:** initial GPU fixture attempts numerically
reconstructed a Boolean mask from ballot words, including the wave32 VALU
VCC-low write. Tracking rejects that shape before dispatch; the new proof
does not infer local facts through it. Diagnostics are
`wave-table-witness-{ir-diagnostic,vcc-ir-diagnostic}-20261004.log.stderr`.
The final guest fixtures use direct CMPX predicates and full VCC copies,
with the same numeric oracle. Before extending ballot support, add an
independent RED for the exact current-lane bit extraction, both ballot
halves and lane indices; reject other-lane bits, mismatched ballots and
shift-wrapping recipes. Temporary diagnostic source changes are removed.

## Ordinary compute compiler breakpoint after heavy pipelines (2026-10-04; pending)

The saved d0c module validates under Vulkan 1.3 and reproduces the NVIDIA
pipeline-creation breakpoint in the bounded `_Build/windows/vk_spv_d0c_probe.exe`
without starting the game. A 22008-byte reduced module still crashes; deleting
its single `OpGroupNonUniformBitwiseOr` makes that reduced module compile, but
replacing all 13 subgroup OR operations in the full module does **not** fix the
full-module crash. Thus the subgroup opcode is not a sufficient production
target. In the full module, replacing the initial FP64 `OpFDiv` of the
integer-derived reciprocal with FP32 conversion, FP32 division, and widening
back to FP64 lets the unchanged FP64 FMA correction and RTE64 mode compile.
This diagnostic binary mutation is not yet a numerical proof or production fix;
see `_Build/analysis/d0c-diagnostic-f32-reciprocal-estimate-20261004.spv`.

Before changing the shared reciprocal lowering, add a synthetic regression
covering a proven nonzero integer-derived FP64 reciprocal in a wave64 compute
shader. The emitted Vulkan module must retain `RoundingModeRTE 64` and fused
FP64 corrections while seeding the reciprocal with FP32 work, without a native
FP64 `OpFDiv`; the current backend is expected to fail this compiler-portability
invariant. Then run the existing independent GPU readback intervals for odd
integers and the reciprocal/FMA/narrowing chain, plus new signed/boundary
inputs if admitted. Check zero/unknown denominators remain rejected and
neighboring FP64 multiply/FMA/narrowing cases remain unchanged. Finally probe
the saved d0c module emitted by the fixed backend and retry the bounded game.
Do not infer numerical correctness or a menu from pipeline creation alone.

Result on 2026-10-04: the synthetic split-wave64/SPIR-V seed test reached the
existing emitter and failed at its FP64-division portability assertion (RED
log `_Build/logs/split-wave64-reciprocal-seed-red2-20261004.log`), then passed
unchanged after the shared integer-domain seed used FP32 division and retained
two FP64 FMA corrections and RTE64 (GREEN log
`_Build/logs/f32-seed-final-synthetic-20261004.log`). Native GPU readback
passed the existing positive reciprocal and FMA chain oracles, the new signed
negative reciprocal intervals, and neighboring FP64 arithmetic/conversion
tests (`_Build/logs/f32-seed-signed-gpu-arithmetic-20261004.log`,
`_Build/logs/f32-seed-final-conversion-20261004.log`). The installed native
binary SHA is
`503303e14a8293180af9579e17856f67d5cea69a0e36f7a442b6bcc6a2ebee19`.
The two 1280x720 retries reached only shown377 and shown205 respectively,
both stopped during long CS8457 pipeline creation before d0c was encountered;
runtime d0c completion and a menu are **not** proved.

## Large bounded buffer-table lowering and native compiler time (2026-10-04; synthetic proved, large runtime specialization pending)

Native synthetic RED on parent `cc264198`, with only test/profile scaffolding:
`shader_cfg_tests.exe --bounded-buffer-shared-access-only` emits a valid
64-candidate raw volatile byte load/store module, then fails the independent
one-CAS-body invariant. Log `_Build/logs/shared-buffer-cpu-red2-20261004.log.stderr`,
test SHA `87f5f5bfafc69f047527a5a1d8d4a5100ff75ef0ecaa7d78cb9a3c6361da0f30`.
The earlier scaffold topology error is not RED evidence. Candidates have
different legal strides; unknown logical-device profile must retain the
specialized path.

Shared correction: on a known logical device with enabled
`shaderStorageBufferArrayNonUniformIndexing`, select native index, packed stride,
host offset/limit and compatibility using scalar SSA, then emit the compatible
access body once. Formatted candidates with different formats/swizzles or
zero-stride defaults retain specialized accesses. Raw zero-stride OOB uses
the proved zero bound. Every final load/store/CAS pointer carries NonUniform;
unknown/disabled feature keeps the previous path. Device creation and test
harness enable only advertised support; capture metadata records the enabled
feature. No table truncation, game/hash test or larger guessed descriptor cap.
Contract: [Vulkan descriptor indexing](https://docs.vulkan.org/samples/latest/samples/extensions/descriptor_indexing/README.html).

Unchanged RED invariant plus unknown-device/ADD_TID/swizzle/formatted controls
and independent byte-bound/overflow oracle PASS on native CFG SHA
`d97aa134b65863b98546bb3487917a5cf4bbc9fb3d8cf8342e974122d639fa27`
(`shared-buffer-final-cpu-bounds-green-20261004.log`). Raw515 and formatted515
numerical GPUAV tests preserve first/high/last/null selectors, different
strides/host offsets/exact descriptor limits, volatile reads and full backing;
raw emits one CAS, formatted emits shared plus incompatible CAS (two total).
Logs `shared-buffer-final-gpuav-SharedBoundedBuffer{ByteAccess,FormattedByteAccess}-20261004.log`,
native SHA `03310d19ef4a18ef952c55414f04a99628251f6ea2040176190fb41cfbdc6811`.
Same SHA passes host-capacity515, zero-stride raw/formatted/D16 and all nine
formatted EXEC/count/VCC guards including both wave64 halves
(`shared-buffer-formatted-guards-gpuav-20261004.log`). Earlier tests failed
because the fixture did not request descriptor ranges; later GPUAV exposed
the independent formatted-bound defect above. Neither is recorded as GREEN.
Actual large-specialization compilation time, menu and other-game runtime remain pending.
Default CFG still fails the existing literal-word serialization assertion
(`shared-buffer-default-cfg-20261004.log.stderr`); focused storage bounds/access
and split-wave reciprocal selectors pass. No suite-wide GREEN claim.
Additional numerical GPUAV mixed ADD_TID515 and formatted host-offset overflow
neighbors PASS on SHA `b2409ff1f6daa62dcba8de3dde91b49a26351d085eda839e398561a3dda68531`
(`shared-buffer-final-gpuav-SharedBoundedBufferByteAddTidAccess-20261004.log` and
`shared-buffer-final-gpuav-BufferFormatUint8HostOffsetOverflowStaysOutOfBounds-20261004.log`).
GPU fixtures retain their numerical oracle on devices without the optional
indexing feature; only the shared-body shape assertion is feature-dependent.

Committed source `2bfde86d` passed native emulator build/install and the bounded
game retry described above. Twelve early CS8457 captures have55505–56265 words
and one CAS each, with largest switches2–9 arms. The new1019-arm specialization
was not reached because CS54904 hit the memory guard first; its size, complete
pipeline creation and numerical game result remain unverified. No benchmark
claim follows from cache-dependent early pipeline completion.

The exact 581327-word CS8457 SPIR-V saved at
`_Build/runs/yotei-integrated-20261004-191348-presentfix-gpuav/shaders/0024_new_shader_cs_8457901d80b91921.spv`
validates for Vulkan 1.3. Its native RTX 5060 Ti pipeline-only probe reached
`vkCreateComputePipelines` but did not return within 60 s
(`_Build/logs/cs8457-large-isolated-probe-20261004.log`). This is a bounded
compiler-time reproduction, not a proven driver crash. The task-owned process
was stopped. The earlier 120679-word variant of the same guest shader compiled
in about 12 s with its layout. Its two largest resource switches had 106 arms;
the new specialization has 1019 arms each, with 1018 atomic compare-exchange
loops and 1020 memory barriers across the whole module. Aggressive dead-code
elimination does not shrink the original module. Diagnostic-only arm truncation
plus dead-branch elimination yields valid SPIR-V: 16-arm copies compile in
2.64 s (453308 bytes), 128-arm copies in 25.36 s (818044 bytes), and a 512-arm
copy (1711980 bytes) exceeds a 120 s probe bound. The 512-arm probe used about
4.15 GB working set before its task-owned stop. These copies change selector
semantics and are only a cost experiment, never game-path substitutions.
A second diagnostic copy preserves all selector arms, values and access bodies
while nesting switches into groups of at most 64 cases. It validates for
Vulkan 1.3 but still exceeds a 120 s native pipeline-probe bound
(`_Build/logs/cs8457-sharded64-equivalent-probe-20261004.log`). Do not adopt
switch sharding alone as a claimed compiler fix.

The synthetic RED/GREEN requirement above has been completed for a bounded buffer
table with many live candidate resources and an observable byte read and
subword atomic write. Its invariant checks that the emitted module does not replicate the
full access/CAS body per candidate while retaining all candidate choices and
exact bounds. Include neighboring cases with different packed strides, byte
offsets/limits, a volatile read, a selection outside the table, and a table
whose candidates are semantically incompatible with any shared access path.
An optimization may share only operations whose metadata and semantics it
proves compatible; otherwise keep the existing specialized path. Check any
needed Vulkan dynamic-indexing feature before use and keep a valid fallback.
Focused unchanged RED/GREEN, SPIRV-Tools validation and numerical first/high/last/
invalid-selector GPU tests passed as recorded above. Remaining work: emit and
probe the actual new large specialization with its full admitted layout, then
retry the bounded1280x720 game. Do not substitute the old pre-fix binary or
truncate real candidate tables to claim a fixed compiler frontier.

The default `shader_cfg_tests.exe` run at
`_Build/logs/f32-seed-final-shader-cfg-20261004.log` fails at the older
`TestTypedSpirvSerialization` literal-word assertion before reaching the new
reciprocal test. The independent reversal documented in the existing CFG
literal-word section below reproduces that failure; do not count the full
suite as GREEN or attribute it to the reciprocal change.

Committed/pushed source `f80f87b0` with installed binary SHA
045ceb1b29e313ce236335a65008debdfc69c26902c02fbd651ada77f25dcbc6
reproduced this breakpoint in bounded 1280x720 selective-GPUAV run
`yotei-integrated-20261004-175633-presentfix-gpuav`: first nonzero RGB at
frame209, shown230, then `nvgpucomp64.dll` 0x80000003/offset0x589eb2 while
creating the same CS d0c5556e1c26cb1c. CS8457 created11 prior variants
(17–232 dense buffers), but the 513th-binding variant was not reached. The
previous diagnostic RTE64 removal does not preserve the FP64 rounding
contract; minimize before a shared production correction. No menu proof.

Run `yotei-integrated-20261004-110636-presentfix-gpuav` completed the fourth
CS549 variant in 294965 ms and CSfc6f in 327456 ms, with immediate driver-cache
checkpoints after both. It reached shown356 and nonzero RGB from frame239;
`window-1108.png` visibly shows a loading spinner. It then exited
`0x80000003` at `vkCreateComputePipelines` for ordinary CS `d0c5556e1c26cb1c`
(40239 words, cooperative=false, flags0). Windows Application event1000
identifies `nvgpucomp64.dll` 32.0.16.1714, offset0x589eb2; the log does not
record DeviceLost. This is a new compiler reproduction lead, not yet an
emulator semantic defect or a valid synthetic RED.

Next: capture the pre-driver SPIR-V without dispatching it, validate it, derive
the actual descriptor/push layout, and use one bounded pipeline-only probe.
Then isolate its trigger in a reusable synthetic ordinary compute fixture
(the guest CFG has 3 loops / 36 blocks). Preserve BDA/resource bounds and
errors. No production behavior change before a meaningful synthetic RED.

## Persist expensive driver pipeline compilation (2026-10-04; host policy proved)

The warm run `yotei-integrated-20261004-104455-presentfix-gpuav` completed
CS549 variants in 154 ms (cached), 284842 ms and 291983 ms, then began a
fourth. The 16-pipeline checkpoint interval saved the second variant but did
not checkpoint the third before the task-owned stop. Required synthetic
regression: the actual checkpoint policy must preserve fast-pipeline batching
and request a checkpoint immediately after an expensive successful creation,
including when it is the first pending pipeline. Cover the interval and cost
boundaries independently; retain cache identity checks and atomic file writes.
`--pipeline-cache-checkpoint-only` got the intended native RED on the original
16-creation policy, extracted without changing its behavior
(`driver-cache-checkpoint-red-20261004.log.stderr`: completed expensive pipeline
waits for 16 creations). The unchanged test is GREEN after measuring successful
graphics/compute creation and checkpointing at 5000 ms; ordinary creations still
batch every 16. Identity, validation-mode, instrumentation and revision selectors
also passed (`_Build/logs/driver-cache-*-green-20261004.log`). Native test exe
SHA-256 `0ee283548160a3b22d6f86989960ef2481afaffee10ff94bf20cd29fa0ef5af9`.
The first build had a missing test forward declaration; that build failure is
not RED evidence. Original workload integration is now proved on source
`3eb16e4b`: CS549's fourth variant completed in 294965 ms, immediately followed
by a 356345341-byte checkpoint; CSfc6f completed in 327456 ms and immediately
checkpointed 358485977 bytes in run `...-110636-presentfix-gpuav`. Subsequent
read-back on a warm launch remains pending. This improves repeat-run persistence,
not shader semantics or menu rendering.

## Cooperative pipeline scale after local SSA reuse (2026-10-04; pending)

Next bounded synthetic regression: `shader_recompiler_compute_tests
--cooperative-wide-indirect-image-size-only` must keep a 64-operation
same-Guard arithmetic chain from emitting per-value Function stores, while
retaining spills for cross-Guard uses. Before the store correction, the native
test failed on source `1bc0d04c` with `OpStore 10 -> 74` versus its allowed
increment of eight (`_Build/logs/cooperative-local-store-red-20261004.log`).
The shader includes a guest barrier and 251-way indirect image switch; SPIR-V
validation and the existing same-Guard load check passed in that RED run.
The unchanged oracle passed after `7afddcd8` with `OpStore 9 -> 9` and
`OpLoad 530 -> 530`; multiwave LDS, cyclic barrier and cyclic scalar-address
GPUAV readback selectors also passed. The game module dropped to 566635 words
and 18629 OpStore, but the second specialization of the same shader still
blocked pipeline creation at `shown=161`.

The bounded synthetic Vulkan switch probe did not reproduce this driver
cost: 860 cases with 4 nested diamonds each, 40 Function spill/load pairs
per case and one enclosing loop produced 558338 words and created a pipeline
in 1124 ms (`_Build/logs/synthetic-switch-loop-860-20261004.log.stderr`).
The diagnostic patch is saved outside tracked source at
`_Build/analysis/synthetic-switch-pipeline-probe-20261004.patch`.
The actual CS has 32 guest loops, 844 IR blocks, 12174 SPIR-V labels and
three frequently called helpers; neither its full control flow nor the
driver-time pathology is covered by the probe. Do not use a word-count-only
threshold as evidence of menu progress.

The first 566635-word CS pipeline completed after 279999 ms in
`_Build/runs/yotei-integrated-20261004-102947-presentfix-gpuav`. The next
specialization had different bounded-SRT/table occupancy and emitted 566871
words; its pipeline creation did not complete before the 450-second frame
watchdog. Before changing specialization caching or descriptor layout, add
two synthetic materializations of one guest shader with varying active table
lengths. Verify which generated code and bounds genuinely depend on those
lengths, numerical results for valid indices, and errors for invalid indices.
Only then decide whether a shared pipeline variant can preserve the contract.

Commit `6dbee0c8` removes redundant Function loads when a cooperative
instruction is consumed inside the same Guard. The synthetic RED/GREEN and
GPUAV neighboring cases are recorded in the 09:54 UTC launch-plan checkpoint.
On the same game CS `54904…`, SPIR-V fell from 773030 to 639478 words and
OpLoad from 62605 to 29217. The bounded selective-GPUAV retry at
`_Build/runs/yotei-integrated-20261004-094551-presentfix-gpuav` still spent
300 seconds at `shown=155` inside `vkCreateComputePipelines` with no completion.
This is a remaining driver pipeline-creation blocker, not a failed SPIR-V
emission test or evidence of menu rendering.

Before another production change, create a synthetic cooperative wave64
program with many direct CFG segments and a bounded pipeline-creation probe.
The captured module has an 860-case scheduler `OpSwitch`, 87 two-case switches,
12174 labels and 886 calls. Use several segment counts to locate any steep
driver-time/memory growth without copying proprietary shader bytes or repeatedly
stressing the GPU. Keep unsupported semantics and required guest work intact.

## Large compute shader and diagnostic runtime stalls (2026-10-03; pending)

The bounded diagnostic game run
`_Build/runs/yotei-integrated-20261003-194531-presentfix-gpuav` used a
temporary four-iteration graphics-loop cap to pass the clean build's earlier
Vertex DeviceLost frontier. It reached `shown=373`, then the shown-frame
watchdog closed the task-owned process after 900 seconds without a new shown
frame. The last complete phase marker in captured stdout was `IR Normalize` for
compute shader `54904fb419d79e49` (36,869 instructions); stdout ended
midstream near `IR TrackResources`. The process was responsive and consumed up
to about 30 GB while CPU time grew. No menu was visible: a Windows desktop
screenshot in the run folder shows a loading spinner. This diagnostic result
does not establish a clean-build runtime advance or an emulator semantic RED.

The saved `54904…` code passes an offline CPU audit in under one second
(`_Build/logs/cs549-offline-audit-20261003.log`), but the registered capture
lacks runtime descriptor values and skips materialization. A second bounded
diagnostic game run with `KYTY_RESOURCE_TRACKING_TRACE=1`
(`_Build/runs/yotei-integrated-20261003-201438-presentfix-gpuav`) showed all
seven ResourceTracking phases for `54904…` finishing in milliseconds, repeatedly.
The previous partial stdout did **not** locate the stall in ResourceTracking.
This repeat instead exited 321 with `ErrorDeviceLost` at `shown=202`, despite
the same temporary cap. The cap is not a reliable workaround, and no menu was
seen. Next isolate the clean large indexed Vertex draw and the long GPUAV
pipeline/specialization path separately with bounded synthetic inputs. The
temporary cap was removed and the native installed exe restored after both
diagnostic runs.

## Divergent graphics wave64 loop with indirect images (2026-10-03; pending)

The dispatched wave64 vertex shader captured on 2026-10-04 derives EXEC from
the low byte of SGPR3 (`ESVertCount`) before copying EXEC into its scalar loop
mask. Current vertex translation initializes that byte to the fixed guest wave
size, including for a native 32-lane graphics subgroup with fewer active
invocations. AMD's RDNA2 ISA defines the byte as ESVertCount and specifies
that combined ES+GS shaders calculate real EXEC from initialized SGPRs;
Vulkan permits partially populated graphics subgroups. Add a synthetic vertex
entry-state regression for wave32/native32, wave64/native32 and wave64/native64:
the low byte must follow actual launched lanes, while unrelated SGPR3 bits
remain intact. Prove the old fixed value fails before changing translation.
Then validate a partial indexed draw with GPUAV readback. Vulkan does not
guarantee that a graphics subgroup stays within one draw; this remains a
compatibility limit of the existing wave64 partition model and must not be
presented as full guest wave64 emulation.

The synthetic entry-state regression is `shader_cfg_tests
--ngg-launched-lanes-only`. On `5a46caf4` with only host-size metadata
plumbed through, it failed as intended with `vertex ESVertCount is fixed
instead of following launched lanes` (`_Build/logs/ngg-launched-lanes-red-20261004.log`).
The shared vertex entry now uses the entry ballot's population count for
SGPR3's low byte, counting one physical half for guest wave64/native32.
The unchanged failure oracle passes after the fix. Extended checks substitute
synthetic 16/32/48/64-lane ballots, verify SGPR3's high bits, validate emitted
SPIR-V, and pass the older NGG entry fixture. Logs:
`_Build/logs/ngg-launched-lanes-spv-20261004.log` and
`_Build/logs/ngg-vertex-entry-neighbor-20261004.log`. The 48-index GPUAV
active-ballot control still passes, but its standalone test executable predates
this translation fix and is only a downstream control.

Installed emulator SHA-256 `27ab5e00a5720cd39adfcfd93045dfc3c52ea72522dc84fc1040dbac3653f9a4`.
The GPUAV-instrumented game retry
`_Build/runs/yotei-integrated-20261004-013304-presentfix-gpuav` stopped at
`shown=169` after its 300-second frame watchdog. The last shader pipeline
trace is `vkCreateComputePipelines begin` for CS `fc6f8c56eb7e168f`, whose
emitted SPIR-V has 775890 words; no `done` trace or readback followed.
The GPUAV run without shader instrumentation
`_Build/runs/yotei-integrated-20261004-014304-presentfix-gpuav` exited
`-2147483645` at `shown=0` during `vkCreateComputePipelines` for the prior
CS `b90e2024732c6111`. Neither retry reached the changed Vertex draw, so
the game's device-loss cause, menu and gameplay remain unverified. The next
bounded check must first get past those compute pipeline creation blockers;
do not count the CPU regression as game progress.

Selective instrumentation of the early `b90e…` CS allowed pipeline creation,
but the silent selective run
`_Build/runs/yotei-integrated-20261004-015524-presentfix-gpuav` remained at
`shown=154` for its full 600-second frame watchdog while creating cooperative
CS `54904fb419d79e49`. A bounded follow-up captured its pre-driver module
at `_Build/runs/yotei-integrated-20261004-020758-presentfix-gpuav/shaders/0000_new_shader_cs_54904fb419d79e49.spv`.
It has 773030 words and 197953 instructions, including 62605 `OpLoad` and
42894 `OpStore` instructions. Existing cooperative and general SPIR-V
optimizer recipes reduce it to 755593 and 750947 words respectively
(`_Build/logs/cs54904-opt-{coop,general}-20261004.log`); neither is a
measured driver-compile solution. Before changing shared emission, construct
a bounded synthetic cooperative wave64 program with an indirect image switch
and enough nested control flow to reproduce the module growth; measure its
SPIR-V size and isolated pipeline creation time on current source. Keep the
captured proprietary module outside tracked tests and source.

New bounded lead: the identified indexed draw has 119856 indices, leaving
16 after division by the native 32-lane subgroup size. In saved SPIR-V
`_Build/analysis/vs-e312-20261003.spvasm`, the loop mask starts as
`0xffffffff` in both words, then subtracts a native ballot from both words.
If a final subgroup contains only 16 executing invocations, ballot cannot
clear bits 16..31 and that loop may continue selecting absent lanes. Vulkan
does not guarantee full graphics subgroups; the actual invocation grouping
and the guest shader's mask provenance remain to be verified. Add a synthetic
Vertex mask-draining loop with a strict 32-iteration guard, subgroup ballot
and ReadLane, and distinct output when the mask remains after the guard. Run
small full and partial indexed draws with readback and GPUAV before changing
production behavior. A RED here would establish the mechanism in the host
model; it would not alone prove this is the game's device-loss cause.

The bounded synthetic probe is now in `ShaderRecompilerComputeTests.cpp`.
Native test exe SHA-256 `2e05fe92170365eda43d0aa44e5e7bf983651166155beecb87ff73d5ca291b90`:
`--wide-vertex-wave64-mask-drain-indexed-48-only` returned exit 9, with
readback red `0` instead of expected `1` after 32 iterations
(`_Build/logs/mask-drain-allones48-gpuav-20261003{,.stderr,.run.json}`).
The 96-index variant also left the mask nonzero on the prior test build,
so draw count divisible by 32 does not ensure full graphics subgroups.
The same 48-index case initialized from `Ballot(true)` instead of all ones
and passed with GPUAV (`mask-drain-active48-gpuav-20261003*`); the preexisting
96-index cross-lane/long-loop control passed on the all-ones test build
(`mask-drain-neighbor96-gpuav-20261003*`). No device loss occurred in these
bounded probes. This is a host graphics subgroup mismatch diagnostic, not a
verified guest emulation fix: the guest shader's initial mask provenance and
hardware execution of padded vertices are still unknown. In particular,
Vulkan `ReadLane` of an inactive target cannot be treated as an oracle.
Do not mask arbitrary guest scalar values or silently cap the loop. Verify
the guest ISA path before changing shared lowering.

The clean Yōtei retry at `_Build/runs/yotei-integrated-20261003-181855-presentfix-gpuav`
lost the device at `shown=200` after nonzero prepared frames 198 and 199.
An earlier SyncDiag capture points to a large indexed draw using vertex shader
`e3125617f3efc38f`, but the clean retry did not independently identify the
draw. This vertex shader has a ballot/shuffle inside a loop and a 250-way
indirect-image switch. Current synthetic wave64 vertex cases execute the same
two loop iterations in every lane, so they do not cover a changing active
mask between iterations.

Add a bounded synthetic vertex case with one or two iterations chosen per
triangle, subgroup operations inside the loop, distinct image contents and
numerical readback. Run it on the current native executable with GPU assisted
validation. A pass is a control, not the required RED for the device loss;
if it passes, narrow the captured failing draw further before any production
change. Do not infer a wave64 semantic fix from a runtime timeout or GPU reset.

The one/two-iteration variant passed numerical readback and GPUAV at 96 and
120000 unique indices on the current source. Logs:
`_Build/logs/wide-vertex-divergent-{96,120000}-gpuav-20261003.log`;
native test exe hashes `c4d3c7c5...` and `ce3e07bd...` respectively.
These are GREEN controls. The saved game SPIR-V updates two mask words after
each ballot and can make many passes through its loop. Next bounded control
uses 1–32 iterations per triangle, first at 96 indices and only then at the
119856-index draw scale if the small case completes normally. A reset or
timeout alone still cannot justify changing guest semantics.

## Formatted buffer descriptor selected at runtime (2026-10-03; pending)

**2026-10-04 verified shared correction (game retry pending):** EXEC and VCC
synthetic RED/GREEN, full ResourceTracking, nine native GPUAV formatted
readbacks and four neighboring zero-stride cases pass. The two-stage fix
proves uniform induction bounds through a nonempty mask conjunction and
places the dedicated continue bridge after a linear guard chain's unique
backedge body. Primary artifacts: `formatted-mask-count-final-{resource-suite,gpuav}-20261004.log`
and `formatted-mask-{nested-latch,shared-merge,graphics-loop,graphics-loop-phi}-20261004.log`
in `_Build/logs`. GPU cases include full/sparse wave64 across both halves,
OOB zero and distinct numeric descriptor choices. Runtime descriptor sizes
are bound exactly in the test harness. No arbitrary unbounded formatted
fallback was enabled, and temporary instrumentation is removed.
Saved-game CPU audit advances PC0x284 to **PC0x41c** with synthetic user data
(`formatted-mask-cs4d-final-audit-20261004.log`). Required next reproduction:
trace the new descriptor and its guard with real runtime inputs, then add a
synthetic fixture for that provenance; do not infer it from zero user data.


**2026-10-04 adjacent emitter RED:** the independent native formatted
EXEC-count fixture now passes tracking, but SPIR-V validation rejects its
two-exit-guard while loop: continue target is not structurally post-dominated
by the generated backedge label. No GPU dispatch occurred. Evidence:
`_Build/logs/formatted-exec-count-gpuav-scalar-20261004.log.stderr` and its
run JSON (test exe SHA256 C7E4FBBDF1AA477DDEE066B2FE14D9A41638A5B8BC1C863B418C62B7F0134E53).
Extend the existing dedicated-continue bridge only for a proven single-entry
linear chain of guards whose failure edges leave to the same loop merge and
whose unique body returns to the header. Retain conservative handling of
early-continue, nested selection and multiple-entry graphs. Same fixture
must validate and read back correctly after the emitter correction.


**2026-10-04 mask refinement:** phase diagnostics of the saved shader
confirm uniform zero/+1 induction, but the required count branch is VCCZero,
not EXECZero. Add the same independent table/materialization and unsafe
boundary controls for VCCZero/VCCNonZero before extending the common mask
conjunction proof. Diagnostic logs are
`_Build/logs/formatted-exec-count-condition-diagnostic-20261004.log.stderr`;
temporary instrumentation was removed.

**2026-10-04 runtime refinement:** clean installed source `3eb16e4b` now
reaches this rejection without a vertex/work cap. Runs `113840` and `115058`
exit 321 at CS `4d6df08d2d54e0ff`, PC `0x284`; this exit is ResourceTracking,
not DeviceLost. Runtime phase trace in
`_Build/runs/yotei-integrated-20261004-115058-presentfix-gpuav/stderr.txt`
shows all four words as unproved `ReadConstBuffer`, an `IMul32` offset, and
four `GetUserData` roots. Final readback first nonzero is frame195, shown200;
no menu. The prior simple/nested controls put the count comparison in the
first scalar loop guard. Actual normalized CFG first tests an EXEC mask,
then tests `Any((i < count) && active)` before reading descriptor rows.
Required RED: an independent canonical scalar induction, a preliminary mask
guard, and an EXEC-reduced conjunction containing the uniform count bound.
Check two distinct descriptor rows and guard-bypass/OR/nonuniform-count
rejections. Preserve arbitrary formatted descriptor and store rejection.


Offline audit of the saved shader code at
`_Build/runs/yotei-immediate-20260926-011827/shaders/registered/cs_4d6df08d2d54e0ff_dccdf4ea82ba8496.bin`
reproduced the PC `0x284` rejection without launching the game or GPU
(`_Build/logs/cs4d-offline-audit-20261003.log`). A temporary diagnostic build
printed the pre-tracking IR to `_Build/logs/cs4d-offline-ir-20261003.log`;
the diagnostic source change was then removed. The scalar row index is guarded
by a count check in outer block `$6`, while the descriptor words are read in
block `$7` and used by formatted loads in block `$11`, inside an additional
loop whose active predicate passes through `Phi` in block `$8`. The simple
formatted-table control has one loop but no inner loop. Add a synthetic nested
loop with the same outer bounded row index, four consecutive scalar descriptor
words, and two distinct materialized rows. This tests the suspected CFG
dimension before changing the shared proof. Preserve rejection when the outer
count guard is bypassed.

The nested synthetic variant passed unchanged production code
(`_Build/logs/formatted-scalar-nested-red-20261003.log`), so nesting alone is
not the RED. Diagnostic audit of the saved shader showed its descriptor words
unproved and reported a cyclic dependency or invalid flat slot
(`_Build/logs/cs4d-proof-diagnostic-20261003.log.stderr`). That capture says
`runtime_resources_captured=false`; its translated user data was synthetic
zeroes. The same PC does not establish the same *reason* as the game run.
Repeat the bounded diagnostic game launch with runtime user data and trace the
bounded proof rejection before changing ResourceTracking. The temporary trace
patch has been removed from source.

The bounded GPUAV retry on the SRGB metadata correction,
`_Build/runs/yotei-integrated-20261003-180731-presentfix-gpuav`, passed the
previous `k8Srgb` render-target rejection and exited 321 after `shown=200`.
Compute shader `0x4d6df08d2d54e0ff` fails resource tracking at PC `0x284`:
`BUFFER_LOAD_FORMAT_X`, one formatted DWORD, with descriptor in `s20` and
vector index enabled. The existing GPU-selected buffer fallback permits only
raw DWORD x2/x3/x4 reads, so this formatted access remains rejected. No
prepared-frame readback or menu was captured; the run spent 340 seconds total,
including new GPUAV instrumented pipeline compilation.

Before changing production behavior, identify the four descriptor words and
their provenance at this PC, including any bounded table, scalar loop or
wave-uniform source. Add a synthetic formatted-load regression with distinct
numeric results for at least two descriptor choices and explicit bounds/OOB
controls. Preserve the rejection for arbitrary unbounded GPU-selected
formatted descriptors, typed stores and invalid format combinations. Run
the test RED against current source before extending the shared resource
tracking/materialization or GPU descriptor path; then retry the same game.

A first independent native control, `resource_tracking_tests
--formatted-scalar-table-only`, is GREEN on current source
(`_Build/logs/formatted-scalar-table-baseline-20261003.log`). It uses a signed
scalar loop, a 488-byte row stride, four consecutive descriptor words at
offset 200, a formatted load, and two distinct synthetic descriptor bases.
It also rejects a path that bypasses the count guard. This proves the simple
bounded-table mechanism already handles the observed geometry. It is **not**
the required RED: the game has additional EXEC/VCC masks and nested control
flow. Capture the failed descriptor's source/rejection with the existing
`KYTY_SHADER_PHASE_TRACE` before selecting the next synthetic variant.
The clean exe without the temporary Vertex cap instead lost the GPU at
`shown=200` after prepared frames 198 and 199 had nonzero RGB pixels
(`_Build/runs/yotei-integrated-20261003-181855-presentfix-gpuav`). Thus this
formatted-descriptor blocker is currently reached only in a diagnostic mode;
the clean GPU loss below is the earlier runtime frontier.

## GPUAV feature parity for synthetic vertex image selection (2026-10-03)

The first three-index synthetic vertex draw passed native GPU readback but
GPU assisted validation reported missing `shaderClipDistance` and
`vertexPipelineStoresAndAtomics` at shader-module/pipeline creation. The
production Vulkan device enables both features; the standalone test harness
did not. Before treating this as an emitter regression, enable the same
supported device features in the harness and rerun the **unchanged** bounded
three-index case with GPUAV. Then increase index counts only if the small
case has no validation errors or device loss. The captured validation failure
is a harness configuration failure, not evidence that the game's DeviceLost
has been reproduced.

The harness now checks support and enables both features, matching the
production device. The unchanged three-index GPUAV case passes on native
test exe SHA-256
`63c56550880df1cfdaa7878027439733aec815450d34bf88ceb0dc966345c2d1`;
300, 3000, 12000 and 120000 unique-index variants also pass with GPUAV
(`_Build/logs/wide-vertex-feature-*-gpuav-20261003*`). These first variants
used one texture for every descriptor, so they did not numerically prove
which table entry the vertex shader selected.

The strengthened test exe SHA-256
`a67459e310dc7bab49fc22fdd1b3fe1ce5007dcbdc12294d219f80062deae1d9`
uses 251 distinct 4×4 image contents and one unique vertex per index. The
vertex shader chooses `(VertexIndex / 3) & 255`; the final triangle's color
proves selected keys 0 (3 indices), 250 (753), 231 (3000), and 63
(120000). Key 251 (756 indices) correctly falls back to root image 0.
Native readback passes for all these cases, and GPUAV passes for 753 and
120000 without a validation error or timeout. Artifacts:
`_Build/logs/wide-vertex-distinct-{small,753,756,3000,120000}-20261003*`,
`_Build/logs/wide-vertex-distinct-{753,120000}-gpuav-20261003*`;
native build `_Build/logs/wide-vertex-distinct-build-20261003.log`.
The neighboring `--wide-indirect-image-spirv-only` case passes. This is a
GREEN control, not the game's DeviceLost RED: the synthetic shader has no
loop, uses no other guest buffers, and samples constant coordinates. Next
bounded fixture should combine a loop and additional read-only buffer
descriptors with the image lookup, then increase one dimension at a time.

The follow-up two-iteration loop and thirteen live storage-buffer reads also
pass native readback and GPUAV at 753 and 120000 unique indices (test exe
SHA-256 `e3a9e3de3f37c40b9fa154dcd1202487b55ca5f12c30d4bdd2b4c1f90963000d`,
logs `_Build/logs/wide-vertex-{loop,buffers}-*-20261003*`). The buffer
contents XOR to zero only when all thirteen loads contribute, preserving the
selected image oracle. The captured game SPIR-V has one loop and one 250-case
switch, but also `OpGroupNonUniformBallot`, `OpGroupNonUniformShuffle`, and
an explicit split at subgroup lane 32; the synthetic fixture used guest
wave32. Next compare bounded wave64 lowering on the same fixture before
adding subgroup operations or more complex buffer addressing. Do not infer
the game's loop bounds or final GPU command from this static inspection.

Test-only commit `cc3fa4e4` adds bounded wave64 vertex variants with the
same resource bindings. Native 753/120000 index cases pass numerical readback
and GPUAV with subgroup ballot plus self-lane shuffle; the prior wave64
loop/buffer control also passes. The final test exe SHA-256 is
`033b786da86c98d784a1b071aa0942e310ddd1df237737c1abd120b08a3a4336`;
logs `_Build/logs/wide-vertex-{wave64,subgroup}-*-20261003*` and
`_Build/logs/wide-vertex-cross-active-*-20261003*`. A first fixed-lane-31
fixture returned image 0 instead of image 31 at 96 vertices, but its target
lane was not guaranteed active; this was an invalid oracle, not a shader RED.
The corrected variant derives a target from the actual ballot mask, shuffles
that active lane's `LaneId`, and passes 96/120000 native readback plus large
GPUAV. Neighboring 753-image, loop/buffer and wide SPIR-V cases pass on the
final test exe. No production GPU change or game retry followed these GREEN
controls. Menu and gameplay remain unverified.

The captured game VS SPIR-V has one loop, 250 `OpSampledImage` instructions,
13 buffers, and a shuffle target first masked to 0–63 and then to 0–31 by
the existing graphics wave64 partition helper. That helper is documented
below as a limited native32 approximation, not full cross-subgroup wave64
equivalence. Static SPIR-V cannot prove which target lanes or loop iterations
the game executed. A semantic fix needs an independent valid guest-wave64
regression and a feasible host execution model; truncating targets or forcing
a loop budget does not establish correctness.

## Windows SysV host-entry stack alignment (2026-10-03)

Upstream PR #990 head `617e728c9309337fda2da0aba9f31bed0207930f`
reports that a guest-to-host SysV call on Windows can enter with RSP at 0
mod 16, then make an MS ABI call with invalid alignment. This is a generic
Windows ABI bug with a synthetic upstream test; the newest Yōtei blocker is
GPU DeviceLost, so do not attribute it to this fault. Before any local ABI
change, port only the trampoline test into `virtual_memory_allocation_tests`,
build natively, and prove the unmodified macro fails on the expected
`ms_entry_rsp` assertion. Then update the shared ABI macro and rerun the
unchanged test, neighboring red-zone/fiber/memory cases and the native build.
Keep Windows x64 and other platforms distinct. A host ABI test cannot prove
menu or gameplay.

Local proof on branch `yotei-windows-bringup`: ported only the upstream
trampoline test, built with `_Build/windows-local.cmd build-target
virtual_memory_allocation_tests`, then ran `--sysv-align-only` on the old
macro. It failed as intended with exit 1 and
`SysV host entry propagated a misaligned stack into an MS ABI call`
(`_Build/logs/pr990-sysv-align-red-20261003.log*`, test exe SHA-256
`1ddcb1f991984d4e3d11286621c10bf39ef8c08c1ebe4e83eb271325a02062e4`).
Commit `79db93d6` adds `force_align_arg_pointer` only for Windows
x64. The unchanged test passes with `entry_mod16=0` and `ms_mod16=8`; focused
fiber, rsqrt and red-zone tests and the complete
`virtual_memory_allocation_tests` executable all passed (logs
`_Build/logs/pr990-*-green-20261003.log*`). Native emulator build/install
passed; installed exe SHA-256
`5251cfc12f986dda2e9b3c8c761e0e80b5fe0545493dcc210cad803c2da406da`.
No game retry on this exe yet; the known repeated GPU DeviceLost is unrelated
to the synthetic ABI failure.

## DeviceLost on large indirect-image vertex draw (2026-10-03)

Clean-source retry on commit `22211827`, installed exe SHA-256
`8c33160e9bd0d89d67f4d9d9357a7ae2b11b02bc4437d55118fcbf448a703d32`:
`_Build/runs/yotei-integrated-20261003-181855-presentfix-gpuav` finalized
`run.json` with exit 321, `shown=200`, 18m22s elapsed. Prepared readback
was RGB black through frame 197, then `colored=10` at frame 198 and
`colored=62` at frame 199. `vkWaitSemaphores` returned `ErrorDeviceLost` at
wait tick 41333, known GPU tick 41331. No validation error preceded it.
This corroborates the old frontier but did not use per-draw SyncDiag, so
the exact submitted operation in this run remains unproved. The clean
retry is not evidence that the diagnostic formatted-descriptor failure
has been fixed or reached. Avoid repeating the driver reset until a
bounded synthetic Vertex regression isolates the cause.

Native indexed-draw control added in test-only commit `0201fa00` after the
full compute switch passed. It extends the existing Vulkan graphics harness
with an optional explicit uint32 index buffer, draws a repeated public
triangle and compares the same one-pixel output as the neighboring ordinary
`GraphicsPositionWExport` case. Counts 3 and 120000 both pass; the large
case also passes bundled-layer GPU assisted validation without a validation
error. Test exe SHA-256
`b42fd19cdd79c1dc874433123c2961cddad311257a1f6401b78730756ca844bd`;
native build `_Build/logs/indexed-control-final-build-20261003.log`, runs
`_Build/logs/indexed-{neighbor,small,large,large-gpuav}-final-20261003.log*`
all exit 0 without timeout. 120000 is a round synthetic bound above the
observed 119856 game indices. The control uses a simple passthrough VS,
so it does **not** execute the game's image table, vertex inputs, pipeline
bindings or descriptor state. It narrows the next test to their combination;
it is not a RED and does not justify a production workaround.

The same native exe SHA-256 `4b93dd3d2dd2c7964dbbb30446bcdad21506eba1e792a56405bfd479bae9ebf6`
gave two separate GPUAV+shader-instrumentation runs with nonzero loading
spinner pixels, then `vkWaitSemaphores` `ErrorDeviceLost`: shown 203 / tick
41560 in `_Build/runs/yotei-integrated-20261002-212649-presentprobe-gpuav`,
and shown 381 / tick 54581 in
`_Build/runs/yotei-integrated-20261003-075610-presentprobe-gpuav`. The
second run exited 321 by itself; its `run.json` is finalized. Windows System
log has `nvlddmkm` event 153 at the second loss. No validation error or
stderr preceded the loss. Its last CPU shader log is VS
`e3125617f3efc38f` with 251 image resources / 250 sampled pairs. Earlier
SyncDiag evidence (see section below) put the GPU wait on the first indexed
draw with this VS, 119856 indices, and a specific PS. That is a strong lead,
but the new runs did not enable per-draw SyncDiag, so their last submitted GPU
operation is not independently identified.

Required synthetic regression before an emitter/descriptor fix: construct a
public vertex fixture with an indirect image table large enough to exercise
the same mechanism, without copying a game shader. Prove SPIR-V validation
and selected image values for first, middle, last and missing keys with
distinct texture contents, including sparse table entries. Bound a native
indexed draw at increasing table and index counts, capture GPU output and
synchronization result, and find the smallest failing combination without
repeated driver resets. Compare shader instrumentation on/off only after a
safe isolated probe; verify descriptor-indexing/device capability requirements
and preserve heterogeneous-image rejection. A pass of CPU validation or a
pipeline build is not a draw result. Do not change the 250-way selection to
an opaque `OpSampledImage` Phi; that prior variant was invalid. Until the
failing mechanism is reproduced, no production workaround is justified.

Synthetic progress at test commit `12baacd3`, native test exe SHA-256
`92ba731d43692969b360ec60cc4bc850ec3e694048191ceeeb853439aecf866d`:
`--wide-indirect-image-spirv-only` builds an independent IR image table with
251 typed choices (root plus 250 cases); SPIR-V validates and retains one
`OpSwitch` and 251 sample operations. `--wide-indirect-image-table-compile-only`
and `--wide-indirect-image-table-gpu-only` use a guest compute fixture with
250 distinct 4×4 textures; CPU-visible indices 0, 125, 249 are correctly
pruned to three pairs, and a bounded GPU invocation returns their distinct
expected values. All three exit 0; neighboring `--indirect-image-only`,
`--indirect-image-numeric-only` and
`--homogeneous-indirect-image-validation-only` also exit 0. Logs are
`_Build/logs/wide-indirect-final-*-20261003.log*` and
`_Build/logs/wide-indirect-table-*-20261003.log*`. An earlier material-stride
variant was discarded: it scanned 27233 speculative positions and hit the
separate pair limit 513 before shader execution, so it was not a valid RED.

These passing checks narrow the problem: a legal 250-case switch and three
actual texture selections work independently. They do not execute a
250-candidate switch on the GPU or a large vertex/indexed draw. Next use a
GPU-owned selector to keep the full table live, establish a small numerical
GPU control, then increase only within a safe bound to find a failing draw
without repeated driver resets. Missing/sparse keys remain uncovered.

Follow-up test-only commit `38b4fa6c` extends the same coherent guest fixture:
the index list can contain 16 or all 250 distinct keys, so materialization
retains 16 or 250 live sampled pairs and SPIR-V has the matching number of
sample operations. The full case executes one bounded compute dispatch and
compares **all 250** independently assigned floating-point texture values.
Native test exe SHA-256
`cb502a5715ca0048d7c792f77c973c01cdfbd66d32fa21775c221012270df6a0`.
`_Build/logs/wide-indirect-full-compile-20261003.log*`,
`wide-indirect-16-gpu-20261003.log*`,
`wide-indirect-full-gpu-20261003.log*`,
`wide-indirect-16-proper-gpuav-20261003.log*`, and
`wide-indirect-full-proper-gpuav-20261003.log*` all have `exitCode=0`,
`timedOut=false`. The previous three-choice GPU case also passes unchanged
(`wide-indirect-three-recheck-20261003.log*`). The GPUAV run used
`_Build/run-regression-gpuav.ps1` and its bundled validation layer; a first
attempt with `KYTY_TEST_GPU_ASSISTED_VALIDATION=1` but no `VK_LAYER_PATH`
failed at `vkCreateInstance ErrorLayerNotPresent` and is not a shader RED
(`wide-indirect-full-gpuav-20261003.log*`). No driver reset occurred.

This covers the full 250-way **compute** switch on the current GPU, including
first, middle, and last keys, but not a vertex shader or indexed draw. The
GPU selector is read from a coherent CPU-visible guest buffer; GPU-owned
selector semantics and missing/sparse keys remain unproved. The next required
synthetic regression is a bounded native vertex fixture with explicit image
descriptors, a small indexed draw control and progressive index/resource
counts, GPU readback, and GPUAV. Keep the first actual failure as the RED;
do not infer that this passing compute test fixes the game DeviceLost.

A diagnostic attempt to hide only the CPU index words from the clean
specialization reader did **not** produce such a control. Materialization
correctly rejected `bounded SRT read 0 index 0 cannot read coherent source at
0x40` (`_Build/logs/wide-indirect-unknown-compile-20261003.log.stderr`).
The attempted harness change was removed; it was not a RED for the draw and
no GPU run was made. The next fixture must either provide a genuinely
coherent producer/consumer contract for that guest scalar read, or execute
the already validated synthetic IR module with a GPU-generated key and
explicit Vulkan descriptors. Preserve the refusal for unreadable planning
inputs.

## Wave-wide zero branches with inactive lanes (2026-10-02)

Upstream PR #985 head `139a260e4975dd612cfc4e84d84c002bfa9c2b39`
describes a shared shader-emitter defect: a `*Zero` scalar branch compares
the ballot of the condition with all ones, although inactive lanes do not
vote. A partial final wave can therefore remain in a loop. This is a lead
for the current Yōtei stall near shown 156–163, not a proven attribution.
Required synthetic RED: compile a small `s_cbranch_execz` loop for a guest
wave64 split into host wave32 and verify its SPIR-V tests that no *active*
lane fails; compare `s_cbranch_execnz` and existing neighboring branch tests.
Then make the shared emitter correction, run the unchanged test GREEN and
neighboring branch/ballot cases, and retry the bounded native game. A loop
cap as in PR #986 is a separate safety bound and cannot replace correct
branch semantics or prove a visible frame.

The initial opcode-count test passed before the fix and was too weak. The
strengthened test identified `OpIEqual` against a `0xffffffff` constant in
the emitted ExecZero loop and failed with exit 9
(`_Build/logs/zero-branch-structural-red-20261002.*`). The corrected emitter
ballots the inverted predicate and tests for zero; the unchanged test passed
(`_Build/logs/zero-branch-green-20261002.*`). Targeted
`--wave64-condition-ref-only`, graphics collective routing and scalar-mask
branch checks passed. `--single-wave64-ballot-spirv-only` still fails on a
separate native subgroup ballot expectation; its ordinary ballot path was
unchanged by this correction, so the full CFG suite is not yet green.
The final test source was rebuilt with `_Build/windows-local.cmd build-target
shader_cfg_tests`; `--zero-branch-ballot-only` and those three neighboring
cases passed again in `_Build/logs/zero-branch-final-*-20261003.log.run.json`
using test exe SHA-256 `295e981a8e1a91dc5c00103f1e0c3473d984f85ee63a22dfe4e598f3fb05b40c`.
The code and test are committed as `37b35115`.

Later inspection of the Yōtei run narrowed the visible stall more directly:
the last logged call before shown stopped at 156 is
`vkCreateComputePipelines` for CS `54904fb419d79e49`, after an emitted
771986-word SPIR-V (`_Build/runs/yotei-integrated-20261002-205909-presentprobe-gpuav`,
`_kyty.txt:7609520-7609525`). No completion line follows before the 240 s
watchdog. This establishes a pipeline-creation stall, not a proven runtime
loop in that shader. Assess the large binary and the driver's compiler
separately; do not attribute the stall to #985 without a native retry.

The first bounded native retry with this correction, exe SHA-256
`4b93dd3d2dd2c7964dbbb30446bcdad21506eba1e792a56405bfd479bae9ebf6`,
eventually compiled that 773030-word shader (`vkCreateComputePipelines`
`elapsed_ms=287593`) and other huge variants. Source readback in
`_Build/runs/yotei-integrated-20261002-212649-presentprobe-gpuav` proved
the first current-branch RGB pixels at frame 201 (`colored=10`) and reached
frame 203 (`colored=92`); then the device returned `ErrorDeviceLost` at GPU
tick 41560. The launching monitor ended before it could finalize `run.json`,
so use the log/readback rather than its stale `stopReason`/`maxShown` fields.
This proves that the earlier 240 s watchdog was too short for cold GPUAV
pipeline creation; it does not prove #985 caused the color progress or that a
menu was rendered. The separately bounded warm retry's final result is
recorded in the DeviceLost section above.

## Driver compiler crash without GPUAV (2026-10-02)

Next synthetic regression (2026-10-03): construct the same
`vk::ComputePipelineCreateInfo` that `CreatePipelineInternal` submits, using
`CompiledShaderInfo::compute_cooperative_wave64=true`, and assert that its
`flags` contains `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT`. Check the
ordinary compute case remains unflagged. The existing policy-only helper
test passes even though the submitted create info currently has zero flags;
record a native RED before changing production behavior. This is a pipeline
configuration regression, not a standalone reproduction of the NVIDIA crash.

Native RED/GREEN proof: `_Build/windows-local.cmd build-target shader_cfg_tests`
and `--cooperative-pipeline-flags-only` on test exe SHA-256
`07d1185f42c3e9dabb881abf9a208409429b012b0e6a04b6b28d6496aca9a0d5`
failed the intended flag assertion (`_Build/logs/compute-pipeline-flags-red-cfg-20261003.txt*`).
The same test on exe SHA-256
`63db5e9003d73e30b05a1fea4077aaa519ffb59dc161fb6f223e20442e24026f`
passes after routing `CompiledShaderInfo` metadata through the actual
`vk::ComputePipelineCreateInfo` builder. Neighboring
`--spirv-optimization-only` and `--cooperative-wave64-admission-only` pass.
Native emulator build/install passed; installed exe SHA-256
`be1dbd195b9b9a0ca6454ac2ed77835cd7a08442c27903cb90549a9559cb3623`.
Bounded game retry `_Build/runs/yotei-integrated-20261003-103053-presentfix-gpuav`
with GPUAV shader instrumentation disabled stopped again at `shown=0`,
`exitCode=-2147483645` during the same `b90e2024732c6111`
`vkCreateComputePipelines`. New trace proves this shader has
`compute_cooperative_wave64=0` and `flags=0x0`, so the validated pipeline
flag correction does not address the game compiler crash. Do not repeat this
mode without a separate compiler/layout reproducer or a relevant shader fix.

Isolated compiler reproduction on 2026-10-03: the saved, validator-clean
39173-word module from `_Build/runs/yotei-integrated-20261003-090128-presentprobe-gpuav`
also exits `0x80000003` in a bounded native Vulkan probe using the module's
seven descriptor bindings (including array counts), one push-descriptor set,
and 128-byte compute push-constant range. The disposable probe source and
binary are `_Build/analysis/vk_spv_probe-layout-20261003.cpp` and
`_Build/windows/vk_spv_probe_layout.exe` (SHA-256
`36563888bd5b96eddd335c37d0dee8532523c2c0bdfb03a10ccde92b6b685c85`).
Probe build used a temporary CMake target and `_Build/windows-local.cmd`;
the temporary CMake change was removed. Logs:
`_Build/logs/vk-spv-b90e-layout-probe-20261003.txt*` and
`vk-spv-b90e-layout-noopt-20261003.txt*`. Disabling driver pipeline
optimization still crashes. The probe does not model all production device
state, but it reproduces the same pipeline creation boundary without game
execution, command submission or a GPU watchdog.

Diagnostic copies under `_Build/analysis/b90e-layout-20261003` all pass
`spirv-val --target-env vulkan1.3`. Replacing all storage-buffer loads, or
just the first load from `bda_pagetable` in `get_bda_pointer`, with `OpUndef`
allows pipeline creation. That is **not** a valid fix: it removes guest
address translation. Replacing all calls to that helper and eliminating the
unused helper still crashes; therefore the individual load is not an
established unique cause. `Volatile`, explicit alignment, `OpAtomicLoad`,
two 32-bit components, constant page index, select instead of return Phi,
SPIR-V `-O`, and exhaustive inlining leave the compiler crash intact.
Artifacts are `vk-spv-b90e-*-20261003.txt*`; no transformed SPIR-V was
installed in the emulator. Before a production change, build a public
synthetic module with the relevant storage-buffer/BDA and control-flow
shape that reproduces the compiler fault or an independently wrong guest
result, then prove RED/GREEN without zeroing memory or suppressing the fault.

New bounded retry on 2026-10-03 with GPUAV enabled but shader
instrumentation disabled used installed emulator exe SHA-256
`5251cfc12f986dda2e9b3c8c761e0e80b5fe0545493dcc210cad803c2da406da`.
`_Build/runs/yotei-integrated-20261003-090128-presentprobe-gpuav/run.json`
finalized with `process-exit`, `exitCode=-2147483645` (`0x80000003`),
`maxShown=0`, `coloredProven=false`. `_kyty.txt` ends at
`vkCreateComputePipelines begin` for CS `b90e2024732c6111`; Windows
Application Error 1000 again names NVIDIA `nvgpucomp64.dll` version
`32.0.16.1714`, offset `0x589eb2`. The two new captured SPIR-V copies are
identical (SHA-256
`53dbb68a1f5886a99b1009df507931f4134dab806ef0c8f4843ea4572690092c`),
but differ from the 2026-10-02 module (SHA-256
`0900d7d8fc9da9acf8b9ba47c1b25b2bbedc9704a9fc45bfd57a44e3c4ec99c9`).
Their instruction histograms differ by 11 each of SPIR-V opcodes 168, 197
and 199; the compiler fault persists. This run is not a vertex-draw reproduction or
evidence that the Windows SysV ABI fix changed GPU behavior. Before another
uninstrumented game retry, isolate a failing generic compiler pattern in a
bounded standalone probe with a layout matching the production pipeline;
the older probe's layout did not reproduce game behavior reliably.

The bounded current-branch run
`_Build/runs/yotei-integrated-20261002-204937-presentprobe-noval`
used 2560x1440, no Vulkan core validation and no GPUAV. It crashed before
`shown=1` with Windows exception `0x80000003` in NVIDIA
`nvgpucomp64.dll` version `32.0.16.1714` while creating the compute pipeline
for shader `b90e2024732c6111`; WER Application Error event 1000 records
offset `0x589eb2`. The two identical emitted SPIR-V copies are in that run's
`shaders/0070_...spv` and `0071_...spv`. This is a driver compiler boundary,
not evidence of a visible frame or menu. Avoid repeated uninstrumented game
launches until a bounded synthetic SPIR-V or emitter regression isolates the
instruction pattern that triggers the crash. Keep shader validation and GPUAV
controls to distinguish invalid SPIR-V from a driver defect.

## Prepared-frame readback layout after a guest flip (2026-10-03)

The bounded full-GPUAV retry
`_Build/runs/yotei-integrated-20261003-163915-presentfix-gpuav` reached
`shown=150` and then failed Vulkan validation on the first requested
prepared-frame readback: `copyImageToBuffer` required
`VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`, but the image remained in
`VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL`. `Presenter::Frame::CopyFrom` leaves
the prepared image in transfer-destination state, while
`CapturePreparedFrame` copies from it without a transition.

Required synthetic regression: create a small native prepared-format image,
clear it in transfer-destination layout, call the same readback recorder used
by presentation, and require both transfer-source state and the numerical
clear color in the downloaded buffer. Run RED against the existing recorder,
then GREEN after adding the source transition. Retain the actual game retry
as a separate runtime check.

The native `--prepared-frame-readback-only` fixture used a 2×2 RGBA8 image
cleared to `(0.25, 0.5, 1, 1)`. With the old recorder it failed after a
successful build at `source layout before readback` (exit `0xC0000409`,
`_Build/logs/prepared-frame-readback-red-20261003.stderr`). After routing
the recorder through the prepared-image transition, the unchanged test
passed with numerical pixels and a second readback from an already-source
image (`_Build/logs/prepared-frame-readback-green-20261003.run.json` and
`_Build/logs/prepared-frame-readback-neighbor-20261003.run.json`). The bounded
game retry `_Build/runs/yotei-integrated-20261003-170145-presentfix-gpuav`
captured 16 prepared frames, 120–135, without the old layout error; every
captured frame had zero RGB. It later reached `shown=197` and the separate
16-bit SRGB color-target guard. These readbacks prove the layout fix and
black pixels for that interval only; they do not prove a menu.

## 8-bit SRGB render-target element size after expanded HTile alias (2026-10-03)

The bounded GPUAV run
`_Build/runs/yotei-integrated-20261003-161215-presentfix-gpuav-sync`
passed the previous HTile guard and exited 321 at `shown=192` with
`unsupported render-target format combination: layout=1 type=6 order=0`
in `textureCommon.cpp:138`. The initial interpretation as 16-bit was wrong:
the current `ChannelLayout` enum assigns `1` to `k8`; `6` is `kSrgb`.
The bounded diagnostic run
`_Build/runs/yotei-integrated-20261003-174110-presentfix-gpuav` proved
`origin=RefreshShaders valid=1 buffer=128 components=1`, i.e. the valid
guest encoding is `k8Srgb`. The host mapping is R8 UNorm, but the format
table gives its render-target element size as zero, so the tuple is
rejected. Upstream PR #1003 changes this element size from 0 to 1 and
does the corresponding 8+8 SRGB correction from 0 to 2. No readback was
requested in this particular run; prior bounded runs measured prepared
RGB black on frames 120–135 and 180–195.

Required synthetic RED before the production change: request the actual
`k8/kSrgb/standard` render-target tuple through
`TextureGetRenderTargetFormat`, require one byte and R8 UNorm backing, and
also cover `k8_8/kSrgb` with two bytes and R8G8 UNorm. Retain a negative
unsupported 16-bit SRGB encoding. Run the same test unchanged GREEN after
correcting shared format metadata, then retry the game.

Completed native RED/GREEN: `--reverse-rt-only` built successfully, then
failed with exit 321 at the exact `k8/kSrgb/standard` tuple before the
metadata change (`_Build/logs/srgb-rt-red-20261003.run.json`). The unchanged
test passed after applying the two element sizes from upstream PR #1003
(`_Build/logs/srgb-rt-green-20261003.run.json`); its 16-bit SRGB negative
encoding remains invalid. Neighboring tile tests initially exposed stale
sampled-only assumptions; the adjusted assertions preserve `k9_9_9_5Float`
as non-target and check exact BPE for both SRGB layouts. Final native
`--rt-tiled-sampled-format-only` passed and `--gpu-tiler-only` passed 330
cases / 236 format-mode pairs (`_Build/logs/srgb-rt-tiled-final-20261003.run.json`,
`_Build/logs/srgb-rt-gpu-tiler-final-20261003.run.json`). Game retry is
still required before claiming this reaches a spinner or menu.

## Generic image lookup after a sampled HTile clear import (2026-10-03)

The bounded GPUAV run
`_Build/runs/yotei-integrated-20261003-141139-presentfix-gpuav-sync`
stopped at `shown=195` in `TextureCache::FindImage`:
`sampled HTile import requires its metadata-aware lookup path`. This guard
sees an overlap with a previously imported virtual depth clear, but the log
does not identify whether the new request is a sampled texture, storage image,
color target, or depth target. The temporary Vertex loop cap was still active;
the run had no pixel readback and does not prove a visible frame.

The diagnostic retry
`_Build/runs/yotei-integrated-20261003-143757-presentfix-gpuav-sync`
identified an exact `DepthTarget` rebinding: both owner and request used
data `0x5053070000+0x1000000`, HTile `0x5054070000+0x40000`, D32 format,
and 2048×2048, one layer. It exited 321 at `shown=192`; the counter is not
pixel proof. The diagnostic patch was reverted after capture.

The independent native `--sampled-htile-depth-promotion-only` fixture uses
uniform HTile clear1 over stale raw depth, numerically samples the logical
depth, binds the exact same allocation as a native depth target, clears it to
zero, and numerically samples the new pixels. It was RED at the original
`FindImage` guard (`_Build/logs/htile-depth-promotion-red2-20261003.*`) and
GREEN with exact-owner promotion
(`_Build/logs/htile-depth-promotion-final-green-20261003.*`). A different
HTile range remains rejected with exit 321
(`_Build/logs/htile-depth-mismatch-final-20261003.*`). Neighboring array
clear and native HTile subset fixtures pass
(`_Build/logs/htile-array-neighbor-20261003.*`,
`_Build/logs/native-htile-subset-neighbor-20261003.*`). The first test design
using the older clear-discovery fixture stopped before the transition at its
unrelated pattern-fill setup and is not counted as RED. Game retry is pending;
this test does not prove a menu.

Two later bounded game retries did not verify the exact-owner path. The first
(`_Build/runs/yotei-integrated-20261003-150014-presentfix-gpuav-sync`)
closed by a 900-second shown-frame watchdog at `shown=153` while compiling
instrumented pipelines. The warm retry
(`_Build/runs/yotei-integrated-20261003-152203-presentfix-gpuav-sync`)
reached `shown=193` and hit the same guard. Detailed diagnosis in
`_Build/runs/yotei-integrated-20261003-154734-presentfix-gpuav-sync` shows
an overlapping depth target with pitch 4096 and a 64 MiB mip footprint,
versus the imported owner's pitch 2048 and 16 MiB footprint; its HTile range
also differs. This is a different allocation layout, not the exact-owner
transition proved above. Required RED before changing it: a synthetic
metadata-aware imported clear followed by a larger overlapping depth target,
with numerical proof that the target sees the correct guest memory layout and
no stale raw depth bytes. Keep the mismatch guard until such a transfer is
proven; gather the full request/owner ranges and clear/load state first.
The full-range retry
`_Build/runs/yotei-integrated-20261003-155620-presentfix-gpuav-sync`
confirmed the alias precisely: both start at `0x50540b0000`; owner data is
16 MiB at 2048×2048, target data 64 MiB at 4096×4096; both HTile ranges
start at `0x50580b0000`, but owner uses 256 KiB and target 1 MiB. Neither
source has CPU- or buffer-dirty pixels at the guard. This is a coherent
native clear that must be materialized into the overlapping guest layout
before the larger target can acquire it; blindly accepting the owner would
use the wrong pitch and extent.
The independent native `--sampled-htile-expanded-alias-only` case now
materializes a 512×512 HTile clear over stale raw bytes, then acquires a
1024×1024 depth target at the same data/metadata bases with larger footprints.
Its two numerical probes require the first target texel to retain clear1 and
the far tail to retain raw 0.25. It is RED at the existing guard, after the
first logical clear was sampled successfully
(`_Build/logs/htile-expanded-alias-red-20261003.*`). Required GREEN: coherent
download of the imported native depth to guest backing, retirement of that
owner, then ordinary larger-target upload without losing metadata ownership.
The shared texture cache now performs that transfer only for a clean,
single-layer D32 depth owner and a larger attachment at the same data and
HTile bases with compatible format/layout identity. It flushes and waits for
the guest backing write before retiring the old owner. The unchanged
numerical alias fixture is GREEN
(`_Build/logs/htile-expanded-alias-final-green-20261003.*`); a different
HTile base still fails closed with exit 321
(`_Build/logs/htile-expanded-mismatch-final-20261003.*`). The exact-owner
and array-clear neighbors pass after this change
(`_Build/logs/htile-exact-neighbor-after-expanded-20261003.*`,
`_Build/logs/htile-array-neighbor-after-expanded-20261003.*`). The bounded
game retry passed the old HTile guard and reached the 16-bit SRGB format
blocker described above. The retry did not numerically check the real
2048→4096 transfer or prove visible pixels.

## GPU-produced indirect workgroup bounds for bounded SRT (2026-10-03)

The bounded GPUAV run `_Build/runs/yotei-integrated-20261003-125433-presentfix-gpuav-sync`
exited 321 after `shown=193`: compute shader `a2dfc83fbe2e55b8` uses a
workgroup-indexed bounded SRT snapshot, while its indirect dispatch arguments
are GPU-owned. The CPU copy can be stale, and `MaterializeResources` correctly
rejects untrusted workgroup counts. The prior temporary Vertex cap was a
diagnostic and is not a fix.

The synthetic native `--gpu-owned-bounded-indirect-only` case now writes a
`2×1×1` grid through the GPU while the CPU copy remains zero, then requests a
shader whose scalar coefficient table is indexed by `WorkgroupId.x`. The
final test was RED with only the production patch reversed: exit 321 on
`bounded SRT requires coherent indirect workgroup counts`
(`_Build/logs/gpu-owned-bounded-indirect-final-red-20261003.*`). It is GREEN
with the production patch: the coherent CPU copy becomes `2×1×1` and the
materializer/pipeline accepts the shader
(`_Build/logs/gpu-owned-bounded-indirect-final-green-20261003.*`). The
ordinary empty indirect control also passes
(`_Build/logs/empty-indirect-after-coherent-grid-20261003.*`). The shared
pipeline cache drains GPU-owned argument bytes only when the shader plan
needs workgroup-indexed bounded SRT; a failed coherent read retains the
materializer's fail-closed error.

This regression proves host-side count coherence and shader admission, not
GPU execution. Its tiny storage-output shader produced no numerical output
even under a direct dispatch in diagnostic trials; those attempts are in
`_Build/logs/gpu-owned-bounded-indirect-green5-20261003.*` and later logs.
Required follow-up: an independent, numerically checked native GPU fixture
with a working direct control; zero-axis, changed-grid and unreadable
GPU-owned argument variations; then rerun the game. Do not report this
shader as a successful rendered-output test.

## Zero-sized indirect compute dispatch (2026-10-02)

The bounded GPUAV run
`_Build/runs/yotei-integrated-20261002-200407-presentprobe-gpuav`
exited 321 at shown 125 on the same immutable-SRT/writable-buffer alias.
Temporary diagnostics show the failing specialization has guest grid
`{0,1,1}`. Its earlier nonempty grids `{4096,1,1}`, `{727,1,1}`,
`{64,1,1}` and `{320,1,1}` evaluated their selectors and pruned the
unselected overlapping row. Source readback frames 60–125 remain RGB black.

Required RED: an indirect compute dispatch with a zero group count and an
unreadable shader address must be skipped before shader specialization, as
the existing direct-dispatch control does. Cover each zero axis and a
nonzero control. Before treating CPU-visible indirect args as authoritative,
check GPU-dirty ownership and pending producer writes: the recorded Vulkan
dispatch reads the argument buffer later, so a stale CPU zero cannot justify
skipping a possible GPU-generated nonzero dispatch. Preserve the alias guard
for any unproved case. Repeat the native game run after a generic fix.

The isolated native `--empty-indirect-only` test (all three zero axes,
unreadable shader address) was RED on the old path: `ShaderGetInputInfoCS()`
tried shader address 1 (`_Build/logs/empty-indirect-isolated-red-20261002.*`).
It passed unchanged after the renderer skipped only CPU-owned zero grids
(`_Build/logs/empty-indirect-isolated-green-20261002.*`). The existing
`--native-indirect-only` GPU-produced argument test still fails at its
asynchronous host-submission assertion (exit 9); its numerical controls
cannot be counted as passed. Do not weaken that assertion merely to make the
new shortcut look safe.

Required additional RED: a bounded SRT selector with an apparent small CPU
grid that would prune an overlapping writer, while the indirect argument
range is marked GPU-owned. Materialization must refuse the proof because the
GPU may execute a larger grid; the same exact CPU-owned grid remains the
positive control. This prevents the new dispatch-wide pruning from treating
stale indirect counts as authoritative.

That `--unselected-bounded-writer-only` extension was RED on the old
materializer (`_Build/logs/untrusted-grid-red-20261002.*`) and GREEN once
GPU-owned counts were marked untrusted and dispatch-wide pruning refused
them (`_Build/logs/untrusted-grid-green-20261002.*`). Full
`resource_tracking_tests` and `resource_materialization_tests` pass.
The final native GPUAV run on source `36bcf354`, exe SHA-256
`567600051cfd44a3a71ea9f126614abb52ebfc3058a2547aeb8ccab894fa8b80`,
`_Build/runs/yotei-integrated-20261002-205909-presentprobe-gpuav`, reached
shown 156 past the old fatal and then stopped by the frame watchdog; source
RGB frames 130–155 were all black. The next runtime stall remains unproved.

## Unselected bounded buffer writer (2026-10-02)

The bounded GPUAV diagnostic run
`_Build/runs/yotei-integrated-20261002-185718-presentprobe-gpuav`
reaches CS `34e090c623ad611c` at shown 142. Its 32-row buffer table has an
overlapping writable descriptor in row 17, but all 64 launched workgroups
read selector words with bits 16–20 equal to zero from a separate scalar
input buffer. The shader therefore selects row 0 in this captured dispatch;
the row-17 writer is only an enumerated candidate. This is a lead, not yet a
safe exception: the selector input must be snapshotted coherently and protected
from every possible shader write.

Required synthetic RED: a compute bounded buffer table with a dynamic
workgroup-uniform selector read from a scalar input buffer, one disjoint
selected writer and one overlapping unselected writer. The current admission
must reject the dispatch. GREEN may admit it only after enumerating every
workgroup selector, retaining its exact input read ranges, proving those
ranges cannot be written, and disabling only unreachable candidates for this
snapshot. Controls must preserve rejection when any workgroup selects the
overlapping row, when the selector cannot be evaluated or read coherently,
and when another live writer can mutate the selector input. Verify renderer
binding admission and repeat the bounded native game run; a black readback
does not prove a menu.

The first synthetic `--unselected-bounded-writer-only` reproduced the expected
CPU rejection on the old code and passed after a generic workgroup-selector
evaluation/pruning change. Its controls preserve rejection for a selected
overlap, unreadable selector input and a writer of the selector input. Full
`resource_tracking_tests` and `resource_materialization_tests` passed.
However, native exe SHA-256
`13b9913741771a10289d75183541afb68af0d46c8c96769a89dadb001cb59e65`
in `_Build/runs/yotei-integrated-20261002-192412-presentprobe-gpuav`
still exited 321 at shown 137 on the same `0x5000f37f80+120` alias
(dense resource 9). Readback frames 60–136 remained black. Later diagnostics
below identified the zero-sized indirect dispatch; the selector itself was
evaluated successfully for nonempty grids.

The bounded diagnostic rerun
`_Build/runs/yotei-integrated-20261002-193319-presentprobe-gpuav`
on a diagnostic build again exited 321 (shown 133). Earlier dispatches of
the same shader proved row 0 or 1 active; immediately before the fatal alias,
the reachability helper returned before visiting the 32-row table. The
initial hypothesis was the combined workgroup-count cap. A follow-up RED
used a synthetic selector
dependent only on WorkgroupId.x with a large independent y dimension whose
total grid exceeds 65536; the old helper rejected despite four relevant x
values. GREEN discovers every WorkgroupId axis transitively used by the
selector, including nested SRT reads, and enumerates only that Cartesian
domain. The unchanged x and y tests pass. Later game diagnostics measured
`{0,1,1}` for the failing dispatch, disproving the count-cap hypothesis as
its cause; the zero-sized indirect case above is the actual next blocker.

## Prepared-frame readback layout transition (2026-10-02)

Bounded native retry `_Build/runs/yotei-integrated-20261002-180746-presentprobe-gpuav`
on installed exe SHA-256 `4c92009ed90e9a6a41309c4fb45eff1ae6f6af98f85570d1d6a6832efd742919`
set `ReadbackStart=90` and exited 321 at shown 86, with no readback file or
pixel proof. The fatal validation message in `_kyty.txt` reports that a
`vkQueueSubmit` copy expects a prepared image in `TRANSFER_SRC_OPTIMAL` while
its current layout is `TRANSFER_DST_OPTIMAL`. This is on the optional
presentation readback path, before the later immutable-SRT alias seen in a
run with `ReadbackStart=180`.

`Presenter::Frame::CopyFrom` leaves the frame in transfer-destination layout;
`CapturePreparedFrame` submits `copyImageToBuffer` with transfer-source layout
without a transition. Required synthetic RED: exercise the prepared-frame
copy followed by optional capture with Vulkan validation and assert the
image layout/transfer access is correct at submission. Cover guest-source
capture and ordinary presentation when capture is disabled. Only then correct
the shared presentation readback transition and rerun the same test and a
bounded game readback. Do not infer visible pixels from `shown`.

The existing guest-source readback option is a diagnostic workaround for this
prepared-frame path. A copy of the bounded runner under `_Build` enabled it
without changing production code. Run
`_Build/runs/yotei-integrated-20261002-181225-presentprobe-gpuav` reached
the same live SRT alias at shown 133; `present-readback.txt` contains 74
frames 60–133, all `colored=0` with RGB min/max zero. The prepared-frame
layout issue remains pending despite this valid source readback.

## Immutable SRT source aliases a writable buffer (2026-10-02)

Native retry `_Build/runs/yotei-integrated-20261002-161240-menucheck-gpuav-sync`
on the unsigned-sampler correction exits 321 at shown 135. CS
`0x34e090c623ad611c` rejects a pre-dispatch immutable SRT snapshot
`0x201347e200+512`: a writable buffer candidate starts at
`0x201347e390`, descriptor size 1, contiguous mapped prefix 1, and lies
inside that source. Its origin is 3, first use PC `0x1d94`, zero stride,
`max_byte_extent=16`, formatted descriptor-only access. The renderer has an
explicit dispatch-wide alias guard; current resource tracking does not prove
read-before-write across invocations. Do not bypass either guard using only
local CFG order. The one-byte mapped prefix alone did not establish that a
four-byte store was impossible: the host binding rounds the range for DWORD
storage. The guest descriptor's mode-0/zero-stride bounds do establish it.

Required synthetic RED for a **live** overlap: a compute shader with a bounded
SRT read and a potentially overlapping writable candidate in distinct
invocations, plus an independent disjoint candidate and exact boundary
cases. Establish guest ordering/visibility semantics and verify numerical
GPU results, including write-first/read-first schedules, before choosing a
shared mechanism such as a coherent runtime read. Preserve rejection for
unproved cross-invocation aliasing.

Follow-up diagnosis: the captured writer descriptor has `OOB_SELECT=0` and
`STRIDE=0`. The AMD RDNA2 ISA specifies mode 0 as out of bounds when
`offset >= STRIDE` and drops out-of-bounds writes. Existing specialization
marked this exact combination `zero_stride_oob` but previously applied it only
to vector reads. Narrower regression: numerical native GPU test with a
mode-0/stride-zero store aimed inside a sentinel, plus a mode-3/stride-zero
control store that must write; CPU bounded-SRT snapshot test for an overlapping
mode-0 candidate and rejecting mode-3/nonzero-stride controls; renderer binding
admission test for the same ranges. [AMD RDNA2 ISA, vector buffer range
checking](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna2-shader-instruction-set-architecture.pdf).

The exact no-write condition is now proven separately: native numerical GPU
`--zero-stride-store-only` RED on the original emitter (sentinel overwritten)
and GREEN with the same oracle; CPU `--zero-stride-writer-alias-only` RED →
GREEN, preserving mode-3, nonzero-stride and atomic rejection; renderer
`buffer-stride-zero-overlap` RED → GREEN. Existing five zero-stride GPU cases,
four bounded zero-stride GPU candidates, full `resource_tracking_tests` and
`resource_materialization_tests` pass after correcting old test descriptors
that unintentionally used OOB mode 0 for live writer/output controls. Logs:
`_Build/logs/zero-stride-*20261002.log`. The renderer admission group test
could not pass its Windows child harness because some child processes failed
to reserve 13.8 GB before reaching the guard; direct targeted overlap and
atomic rejection children did reach and confirm their expected guard.

Native retry `_Build/runs/yotei-integrated-20261002-175224-presentprobe-gpuav`
on installed exe SHA-256 `4c92009ed90e9a6a41309c4fb45eff1ae6f6af98f85570d1d6a6832efd742919`
passes the prior mode-0/zero-stride alias candidate and creates the compute
pipeline for `34e090c623ad611c`, then exits 321 at shown 133 on a **live**
candidate: immutable source `0x5000f37f80+120`, writer the identical 120-byte
range, `STRIDE=8`, `NUM_RECORDS=15`, mode 0, origin 3, PC `0x1d94`.
This remains a cross-invocation alias problem; do not extend the OOB exception
to it. Readback starts at frame 180, so no current pixel/menu proof.
Independent source-image readback of frames 60–133 in
`_Build/runs/yotei-integrated-20261002-181225-presentprobe-gpuav` confirms
all are black, then reaches the same alias (dense resource 6 in that
specialization). There is still no nonzero frame or menu proof.

## Dynamic unsigned image sampler selection (2026-10-02)

After the clean scalar-buffer out-of-bounds and integer-sampler materialization
regressions passed, native run
`_Build/runs/yotei-integrated-20261002-160138-menucheck-gpuav-sync` still exits
321 at `vkCmdDispatchIndirect`: R8_UINT view has a linear sampler (VUID
`magFilter-04553`). Temporary binding diagnostics identify shader
`0x8968b4b53e5a246a`, image resource 48, Uint, paired to sampler 7 with
`force_point_filtering=1`. The SPIR-V emitter's
`CompatibleSamplerForImage` recognizes signed and converted images but omits
plain Uint, so its dynamic candidate can bypass that point variant.

Before changing emission, extend the synthetic indirect-image numeric-class
switch to assert the sampler descriptor index used by each sampled image:
float candidate uses the original sampler, Uint candidate uses its point
variant. Run the unchanged test RED on the current emitter, then GREEN after a
shared numeric-class correction; validate signed and float neighbors. Retry
the native game with source readback. The observed binding mismatch does not
prove a rendered frame or menu.

RED: `_Build/logs/indirect-uint-sampler-red-20261002.log`, exit 9, Uint
candidate bypassed the point sampler in emitted SPIR-V. GREEN: same
numeric-class switch with Float index 0 and Uint index 1 after the emitter
correction (`indirect-uint-sampler-green2-20261002.log`); mixed float/signed/
unsigned sampler materialization and neighboring indirect-image GPU test
also pass. A second native test build passes the same dynamic switch for
signed integer candidates (`indirect-integer-neighbors-20261002.log`). Installed
emulator SHA-256 `24c1f4ee5adc8813e375448674410f6d8943dae92508f2700de18c69445c1ebb`.
The real retry passes the previous R8_UINT Vulkan filter VUID and stops on
the independent immutable-SRT alias above before frame readback.

## Finite first-active-lane selectors through native EXEC guards (2026-09-28)

Committed 05d10203 retry `yotei-integrated-20260928-181147-menucheck-gpuav-sync`
passes d895 and reaches CS 86da5eb7b8257bb0, PC 0x530 BUFFER_STORE_DWORD.
Decoded selector uses V_READFIRSTLANE after finite per-lane choices and an EXECZ
early return. Existing finite-selector GPU fixture passes independently.
Hypothesis: nonempty-mask proof recognizes legacy low|high and Ballot equality,
but not the native ConditionRef EXEC reduction. Before production changes add
a CPU matrix for EXEC zero/nonzero, lower/upper masks and unrelated early exit;
keep missing/wrong/bypassed guards, mismatched masks, conditional read and cycles
rejected. Every snapshot root, bound and materialization limit remains required.
Do not admit arbitrary GPU-selected writes.

CPU RED confirmed on 05d10203 production: `native-exec-cpu-red-20260928.txt.stderr`,
exit 1, exact expected lost finite first-active-lane proof. Add native-CFG numerical
GPU full/upper-only fixtures with runtime unknown inactive VGPR values, matching
EXECZ guard and unchanged descriptor/output/sentinel oracles before production fix.
Both numerical native-CFG GPU fixtures RED before pipeline creation, PC 0x90
(`native-exec-gpu-red-FiniteSelectorNativeExecGuard{Full,Upper}-20260928.txt`),
exit 321. CPU legacy-profile audit of 86da independently reproduces the actual
PC 0x530 rejection (`srt-86da-cpu-audit-false-20260928.txt`); barrier=false remains
an explicit diagnostic assumption.

After guard admission, unchanged upper-only numerical fixture gives a second RED:
`native-exec-gpu-green-FiniteSelectorNativeExecGuardUpper-20260928.txt.stderr`,
272/7032 DWORD mismatches, assertion exit -1073740791. The full-mask variant passes;
the existing upper-only fixture without native guard remains GREEN. Split-wave
ConditionRef emission returns a per-lane predicate when other_half is null, even
though the admitted compute execution uses two physical native32 subgroups. Add
exact/one-byte-short mask-reduction scratch budgets to the existing planner matrix
before correcting emission and collective scheduling. Keep the numerical oracle.
Scratch-budget RED confirmed: `native-exec-ballot-budget-red-20260928.txt.stderr`,
assertion "mask ConditionRef omitted whole-wave ballot scratch" before production changes.

GREEN: unchanged CPU native-EXEC matrix (6 positives/12 negatives); full and upper-only
GPU fixtures pass all 7032 DWORDs with the original oracle. Existing finite selector
full/upper neighbors and bounded scalar store neighbor GREEN; cooperative LDS suite
9/9 and CTest 22/22 GREEN. Logs `native-exec-{cpu-green,gpu-wave-green-upper,final-*,
multiwave-green,ballot-budget-green,ctest}-20260928.*`. The 14-case planner matrix
now also enforces exact/one-byte-short mask ballot scratch budgets.
CPU translation of captured 86da passes both explicit diagnostic barrier profiles.
Fix shared native EXEC nonempty-path proof, split-compute whole-wave ConditionRef
reduction, collective rendezvous placement, requirements and scratch budgeting.
GPU-selected writes remain rejected unless the existing table/root/guard proof holds.
Native build `native-exec-wave-fix-build-20260928.log` GREEN. Committed 2e3a2a13
retry passes both descriptor-store failures and reaches the pending image boundary
at shown=101; captures of shown=0/66 remain black.

## Native image descriptor origin / split-latch post-test proof (2026-09-28)

Committed 2e3a2a13 native Windows retry reaches shown=101 and fails CS
5f3fdf61a7ca4a20, PC 0x26c: GetImageResource dword 0 rejects LoadAddressU32 root.
Run `yotei-integrated-20260928-183223-menucheck-gpuav-sync`, exit 321.
Before a production correction distinguish descriptor origin reconstruction,
finite/bounded table proof and guarded memory snapshots in a synthetic native-CFG
image regression; retain unknown/unbounded/conditional-root and writable-alias
rejections. Desktop captures remain black; no rendered image/menu claim.

Additional 2026-09-28 evidence: merge 10961866 real retry
`yotei-integrated-20260928-185252-menucheck-gpuav-sync`, maxShown=92,
black DesktopCopy shown=6/65, same image-origin exit 321. CPU-only captured-shader
audit also fails at PC 0x26c in both explicit legacy needs_lds_barriers profiles;
logs `srt-5f3f-cpu-audit-input-fixed-{false,true}-20260928`. Initial input_error
from a missing compute.needs_lds_barriers is excluded from reproduction evidence.
Next synthetic fixture must cover nested constant post-test loops with scalar image
address induction, canonical/native CFG origin, writes and invalid bounds. Keep
shader-write and runtime-value rejection until a supported proof is established.

Synthetic regression prepared before production correction: `resource_tracking_tests
--nested-posttest-image-only`. Four signed/unsigned/plain/non-nested positive cases
require dense [0,5) image table retention across an SSA split latch; six negative
cases retain nonunit step, wrong repeat edge, zero/runtime bound, entry bypass and
non-dominating update rejection. Code inspection shows the guest post-test update
and compare dominate an empty backedge block; current proof requires update.Parent
== latch even after proving update dominance. RED confirmed on unchanged 4a556c73 production: `nested-posttest-image-cpu-red-20260928.txt.stderr`,
exit 1, "split-latch nested post-test image loop lost its dense bound".

Native-CFG numerical regression prepared: `NestedPostTestImageDescriptorLoop`,
three full scalar-address image descriptors, signed outer/inner post-test loops,
two repeats per descriptor and independent six-output float oracle (2,20,80).
Wave32 fixture isolates descriptor proof; it does not claim new wave64 coverage.
All stores and table backing retained; loops have constant bounds. Before GREEN
run with only the new SrtWalker dominance correction absent to prove intended RED.

Corrected synthetic CFG still RED unchanged oracle on old proof:
`nested-posttest-image-cpu-corrected-red-20260928.txt.stderr`. Native numerical
fixture RED before pipeline creation at PC 0x38 GetImageResource root LoadAddressU32,
exit321 (`nested-posttest-image-gpu-red-20260928.txt`). Only the own SrtWalker
correction was absent; unrelated source preserved. The prior first GREEN attempt
stopped on a duplicate test edge assertion and is excluded from GREEN evidence.

GREEN on the same corrected fixtures and unchanged oracles: CPU 4/6 matrix;
native shader SPIR-V validation and numerical six outputs; captured 5f3f CPU audit
both legacy barrier profiles. Logs `nested-posttest-image-cpu-final-green`,
`nested-posttest-image-gpu-green`, `nested-posttest-capture-cpu-{false,true}`.
Shared correction replaces exact update-parent equality by proven latch dominance;
constant positive bound, unit-step phi, repeat edge, read dominance and runtime root
checks remain. Neighbor image table, wave64 image loop, bounded store3Sparse and
native EXEC upper GREEN; required CTest22/22 GREEN. Game retry pending.

Real committed c8aaaa5b retry passes 5f3f image-origin boundary, emits 150116 SPIR-V
words and creates pipeline successfully (2912ms); subsequent dispatches continue.
Run `yotei-integrated-20260928-202341-menucheck-gpuav-sync`, maxShown113,
black DesktopCopy shown37/106. No frame/menu/gameplay claim.

## Finite selector snapshot capacity corrected (2026-09-28)

Baseline c8aaaa5b committed retry failed MaterializeResources CS8457901d80b91921. stderr:
bounded SRT read1 exceeds snapshot limit, count65536 stride592 bias0,
accumulated words131072 vs word_limit65536. Guest code uses a finite 16-bit
lane-selected record key and multiple scalar-buffer descriptor/payload columns.
Hypotheses, not fixes: eager snapshot planning may cover a larger finite domain
than the descriptor extent, or ordinary payload dependencies may require a runtime
path instead. Before production change add synthetic finite selectors, descriptor
extents, OOB rows, required vs ordinary dependencies and cap endpoints. Preserve
read/store admission, zero-work semantics and snapshot/descriptor limits. Do not
claim a root cause or increase the limit without an independent regression.

### Regression required before capacity correction

`TestBoundedScalarProbeBudget` / `--bounded-scalar-probe-budget-only`:
retain 65536 logical keys in each of two scalar-buffer columns while an 8-byte
SRD permits only two coherent words; all other rows must remain zero. Repeat with
strides16/592, U32 offset wrap re-entering the extent, zero count and zero extent.
Keep exactly65536/plus-one in-bounds probe admission (including repeated addresses),
and exactly64MiB/plus-one dense storage admission. Failed materialization must
leave snapshot/specialization unchanged and never use ordinary mutable reads.
Run existing raw-address and workgroup probe limits unchanged.
Native GPU `FiniteScalarBufferDescriptorExtent` uses a GPU-loaded 16-bit key
0/1/65535, one-row scalar-buffer SRD and four descriptor columns; expected
output is the valid row then zero loads, with backing sentinels unchanged. Expected pre-fix
failure: second column rejected despite descriptor-proven zero rows. RED on unchanged production4db9964a: native CPU
`bounded-scalar-budget-red-20260928.txt.stderr` rejects read1 count65536,
stride16 bias4; native GPU `bounded-scalar-budget-gpu-corrected-red-20260928.txt.stderr`
rejects read1 at materialization before GPU execution. Both intended failures. An intervening GREEN attempt exposed a fixture-only
input address >=256, outside the harness packed offset field. Input relocated
without changing the key/data oracle; corrected fixture reran RED with only the
production patch absent. That harness failure is not GREEN evidence.
The proposed shared change charges descriptor-in-bounds probes independently of
dense storage, preserving logical rows and both existing constants. GREEN: unchanged CPU/domain oracle and corrected native GPU fixture
`bounded-scalar-budget-{final-green,gpu-final-green}-20260928.txt`; SPIR-V
validation and all backing words checked by the GPU harness. Native Windows
full build `bounded-scalar-budget-final-build-20260928.log` and required CTest22/22
`bounded-scalar-budget-ctest-20260928.log`. Scalar sparse store, upper EXEC guard,
and nested post-test image numerical neighbors GREEN. Existing raw/workgroup
exact probe boundaries pass unchanged. Original game retry remains pending.

Committed8cb79392 retry `yotei-integrated-20260928-205136-menucheck-gpuav-sync` confirms that
CS845790 passes81 bounded columns, SPIR-V54523 words and pipeline creation2041ms.
The domain/count remains65536; no quota constant changed. Source8cb79392
CI36482065792 Build/Test/Install GREEN Windows/Linux/macOS; macOS artifact upload
DNS failure prevents an overall GREEN claim. Documentation push retries CI.
New runtime boundary is below.

## Pending sampled pair image admission (2026-09-28)

2026-10-02 regression requirement before changing selector-bound lowering:
construct a synthetic inline descriptor table whose scalar selector is bounded
by a dominating unsigned CFG guard. Cover both `selector < limit` on the true
edge and `selector >= limit` on the false edge when the condition is wrapped in
`ConditionRef` (including `LogicalNot` polarity). Assert the exact finite
`selector_limit`, correct materialized image/sampler keys and pairs, and unchanged
behavior for a raw comparison. Opposite-edge, non-dominating, mismatched-selector
and EXEC/VCC-wrapped conditions must not yield an unsound finite bound. Exercise
multiple table roots whose candidate counts accumulate across the 512-image
budget, including equal/different samplers and exact/plus-one capacity. Obtain
native CPU RED on the current code before any production fix, then unchanged
GREEN; follow with a bounded native GPU selection check and game retry.
The source-level `ConditionRef` mismatch is a hypothesis until the RED/IR trace
establishes that it caused this runtime table's zero bound.
Native Windows CPU RED at source `98a46e46` with test-only fixture change:
`_Build/logs/inline-scc-guard-red-build-20261002.log` built
`resource_tracking_tests.exe`; `--inline-scc-selector-guard-only` exited 1 in
`_Build/logs/inline-scc-guard-red-20261002.log.stderr` with `buffer descriptor
is not a valid runtime value`. The control-flow guard is `selector < 3` on the
only edge reaching the table; this is the intended planner failure, not a build
failure. The relation to the game's exact current table remains to be checked.

Completed8cb79392 native Windows retry reaches PSf8927c09f4b928c7 at maxShown119,
then exit321: `inline sampled pairs exceed the dense image resource limit
(size=23184 stride=368 probes=1439 pairs=297 images=607)`. Evidence: completed
run above / stderr.txt and stdout tail. This failure is after the corrected CS
materialization and successful compute pipeline; no new frame/menu proof.

Read-only code diagnosis: BuildResourceSpecialization sums each indirect table's
candidate count minus its root before creating dense resources. Candidates carry
root-specific mapping/type metadata and an indirect sampler. It is not yet proven
whether607 represents necessary distinct typed pairs, duplicates across roots,
or an overestimated selector domain. Do not collapse images with different
samplers/types/roots blindly and do not increase MaxImages.

Before any future candidate deduplication or capacity change: capture candidate
identities/domain evidence with bounded CPU-only diagnostics; add independent
multi-root synthetic tables with overlapping
and disjoint image/sampler pairs, differing type/dimension/swizzle semantics,
invalid holes, descriptor extents and exact/plus-one admission. Prove intended
RED then unchanged GREEN. Native numerical image/sampler selection must preserve
per-root mappings and sampler differences, dirty-memory/alias rejection and
transactionality. Retry this PS in the game only after that proof.
Older diagnostic ISA capture:
`yotei-integrated-20260926-070451-presentfix-gpuav/shaders/original/0168_new_shader_ps_f8927c09f4b928c7.rdna2`;
current runtime memory values are not reconstructed from that ISA capture.

2026-10-02 GREEN: unchanged `--inline-scc-selector-guard-only` now passes after
the scalar `ConditionRef` lowering fix. New synthetic sampled image/sampler
table asserts both exact selector limits and three live pairs;
`--inline-sampled-scc-guard-only` passes. Polarity (`>=` false edge and nested
`LogicalNot`), mismatched selector, wrong edge, and EXEC/VCC rejection pass
`--inline-selector-guard-safety-only`; full `resource_tracking_tests` passes.
Logs: `_Build/logs/inline-scc-guard-green-20261002.log*`,
`inline-sampled-scc-guard-20261002.log*`,
`inline-selector-guard-safety-20261002.log*`, and
`inline-selector-full-resource-tracking-20261002.log*`. The sampled test was
added after the minimal shared buffer RED; it has no separate recorded RED.
Multi-root exact/plus-one image admission and non-dominating guard remain
synthetic coverage debt. In the native game retry
`yotei-integrated-20261001-214503-menucheck-gpuav-sync`, this PS did pass
resource specialization (386 images, 383 pairs) and emitted 295838 SPIR-V
words; a later VS SRT failure stopped the game before readback.

## Planning-only scalar-address SRT slot (2026-10-02; regression resolved)

The completed retry above reaches VS `ee4f153aa500d327` at maxShown170 and
fails in `RefreshFlatBuffer`: flat slot2 is a raw planning-only
`LoadAddressU32` (`opcode=209`, `kind=ScalarAddress`, `slot_kind=0`) whose eager
evaluation fails. Evidence: that run's `stderr.txt:1` and `_kyty.txt:8182384`.
This is distinct from the now-passed sampled-image admission. An older generic
null-root correction allowed this shader to emit SPIR-V, but the current
descriptor value and memory state have not been reconstructed. Do not turn
arbitrary unreadable planning-only memory into zero.

Before the production change, an independent CPU regression was added for a
planning-only raw scalar-address read from an optional exact-null root that is
unneeded by the active resource path, plus a used/readable root control.
Non-null unreadable, dirty, aliased or actively required roots must still fail
closed without partially committing a resource snapshot. Distinguish a slot
that is truly unused/deferred from a required descriptor word. The native
`--conditional-planning-srt-only` test gave RED on the old implementation and
GREEN unchanged after the CFG owner/reachability fix. Active/unreadable,
unknown-condition, shared-owner, shader-write and repeated-snapshot controls
are GREEN; full resource tracking/materialization suites, native build/install
and three neighboring GPU compute cases are GREEN. In the bounded game retry
VS `ee4f153aa500d327` emitted SPIR-V, while readback and a visible frame
remain unproved. Logs and run references are in the first launch-plan checkpoint.

## Pending clean scalar-buffer flat slot (2026-10-02)

The next bounded retry stops at CS `7ceb0f3417f926f9`, flat slot31:
`ReadConstBuffer`, `FlatSlotClean`, planning-only scalar buffer read, PC `0x294`,
memory index60. `RefreshFlatBuffer` returned failure before SPIR-V for that
specialization. The current error does not distinguish an invalid descriptor
extent, unreadable guest address, unavailable argument or another evaluator
failure. Older saved variants of this shader compiled, but they are not proof
that the current runtime value is valid.

Before changing production behavior, capture the evaluated descriptor base,
stride, record count, offset and exact failure reason without exposing guest
data. Add synthetic RED controls for a required readable clean slot and the
actual failing case once its semantics are known; keep descriptor bounds,
unreadable memory and failed argument evaluation closed and transactional.
Do not apply ordinary-slot reachability skipping to clean slots: bounded
wave-uniform resource planning may consume them outside the shader block.

Diagnostic retry `yotei-integrated-20261002-152819-menucheck-gpuav-sync`
(`f3f71319` plus diagnostic-only SrtWalker diff, installed exe SHA-256
`421ac459e84c220c8512ecab1884578b57ddb014175429d2b2204b09a7331779`)
reaches maxShown177 and gives the exact failure: PC `0x294` clean scalar-buffer
read, descriptor `base=0x5000000000`, `stride=8`, `records=0`, byte offset 0,
computed extent 0. No guest-memory callback was reached. The AMD RDNA2 ISA
scalar-buffer contract uses base, stride and num_records for bounds, and the
existing GPU `EmitReadConstBuffer` path returns zero for an OOB DWORD.

Before changing the CPU evaluator, add a minimal clean flat-slot regression
with a nonzero base and zero records. It must return the guest's OOB zero with
no memory probe, while a valid one-DWORD descriptor returns the real memory
word. Cover an offset at the exact end, one partially covered DWORD, stride
zero and nonzero, and an in-bounds unreadable/dirty specialization read that
still fails transactionally. Run unchanged RED with the current evaluator,
then unchanged GREEN after a shared bounds correction; do not treat invalid
descriptor shape, negative offset or arbitrary unreadable in-bounds memory as
an OOB zero. Repeat the native game with readback and pixel evidence.

Completed CPU proof: `resource_tracking_tests --clean-scalar-buffer-oob-only`
gave intended RED before the evaluator change (`clean-scalar-oob-red-20261002.log`,
exit 1), then unchanged GREEN. Cases cover zero records with a nonzero base
and both accepting/rejecting memory callbacks, exact-end and partial-DWORD
OOB, valid stride-zero/nonzero reads, and transactional in-bounds unreadable
failure. Full resource tracking/materialization suites GREEN. Native retry
`yotei-integrated-20261002-153834-menucheck-gpuav-sync` on installed SHA-256
`555231ceff7f78cd06daf678c18d305ed1385181ff371ca279ed8421d7e48e78`
emits SPIR-V for `7ceb0f3417f926f9` (`_kyty.txt:6759215`) before the
separate sampler validation error. This verifies passage of the old SRT
boundary in that run; it does not prove a visible frame.

## Pending unsigned-integer sampled-image filtering (2026-10-02)

The OOB-corrected retry `yotei-integrated-20261002-153834-menucheck-gpuav-sync`
passes the old clean SRT read (`7ceb0f3417f926f9` emits SPIR-V), then Vulkan
validation aborts at maxShown131: a `VK_FORMAT_R8_UINT` sampled view is paired
with a linear-filter VkSampler in `vkCmdDispatchIndirect` (VUID 04553).
This is an independent shader specialization/binding issue; no source readback,
visible frame or menu was proved. `BuildSamplerPlan` maps unconverted `Uint`
images to `SamplerClass::Integer`, which sets integer border color but leaves
linear filtering. Signed or converted integer images use `PointInteger`.

Before production change, run the existing `ImageSampleR8UintForcesPointSampler`
numerical fixture through a focused native CLI and prove its sampler-specialization
expectation fails on current code. Preserve its output oracle. Cover a float
sample with the same guest sampler and the packed-integer conversion case so
point filtering is selected only for integer image bindings. Check Vulkan
format-feature constraints rather than assuming every integer view supports
linear filtering. Repeat the exact game profile after unchanged GREEN.

Focused native CLI `--integer-sampler-point-only` gave intended RED before
production change: `ImageSampleR8UintForcesPointSampler` failed at sampler
specialization (`integer-sampler-red-20261002.log`). Shared correction forces
point filtering for unsigned and signed integer sampler variants while
preserving the float variant. The same R8 UINT numerical oracle, packed UINT
sample/gather, float sample/gather and mixed float/UINT/SINT one-source
binding case pass (`integer-sampler-mixed-green-20261002.log`). The mixed
case's old "UINT may remain linear" expectation was corrected under Vulkan
VUID 04553. Full resource tracking/materialization suites and native emulator
build/install also pass. The retry `yotei-integrated-20261002-155304-menucheck-gpuav-sync`
on SHA-256 `c1950a59fa01337d595dea72246cf5256f533c63f41bf8027e2eb05fbe7e7dda`
still hits VUID 04553 for `image_10[3]` / `R8_UINT`, maxShown116. Thus the
focused UINT sampler correction did not cover this runtime binding; retain it
as test-proven but runtime-unvalidated. Before further production change,
diagnose the actual binding-10 image resource class, view format and all
sampled-pair sampler variants for the failing dispatch. The shader has 64
images and 50 sampled pairs in that specialization; do not infer the bound
pair from the test's single-image fixture. Make a synthetic RED from the
identified shared mapping/format mechanism, then retry.

## Pending prepared-frame diagnostic readback layout (2026-10-02)

With readback starting at frame1, the game stops at Vulkan validation:
`copyImageToBuffer` expects `TRANSFER_SRC_OPTIMAL`, but the prepared image is
still in `TRANSFER_DST_OPTIMAL`. The no-readback retry passes that point. This
is a diagnostic-presentation blocker, separate from guest SRT and the goal of
proving actual pixels.

Before changing the capture path, add a bounded synthetic presentation
regression that prepares a frame by copy and by clear, captures it with Vulkan
validation, checks the copied bytes and the frame's later presentation, and
reuses the frame. Include guest-source capture and ensure image layout/access
transitions are recorded on the command buffer that performs the readback.
The current game run is a reproduction but is not the minimal synthetic test.

## Pending neighboring unaligned scalar-buffer read (2026-09-28)

Additional numerical test `--unaligned-scalar-buffer-load-only` fails 1024/1032
DWORDs: expected 0x13579bdf at host byte adjustment 3, actual 0xdf579bdf from offset 0.
`native-exec-unaligned-scalar-neighbor-20260928.txt.stderr` reproduces it; the
same unchanged test still fails identically with only the own wave-emitter,
cooperative scheduling, requirements and budget patch absent
(`native-exec-unaligned-baseline-red-20260928.txt.stderr`). Source read-only
comparison to 2fc423a9 shows that EmitReadConstBuffer already omitted host-view
byte adjustment before the fresh merge. This neighboring path is not GREEN.
Before a separate fix retain that RED and add host adjustments 1/2/3 plus
aligned and end-of-view cases; use shared bounds-checked byte reconstruction,
preserve guest SMEM offset semantics and avoid shifting the output resource.
The wave patch was preserved and restored via
`_Build/analysis/native-exec-wave-production-20260928.patch`; no reset/user edits.

## Bounded descriptor stores through scalar ConditionRef (2026-09-28)

Retry on 5b63741c (`yotei-integrated-20260928-174752-menucheck-gpuav-sync`)
rejects CS d8959888aafd2552 at PC 0x1b4, BUFFER_STORE_DWORD. This is a store,
not the single-DWORD read assumed during initial diagnosis. Preserve the GPU-selected
write rejection. CPU-only legacy-profile diagnostic reproduces the same boundary;
its needs_lds_barriers=false is an explicit diagnostic assumption, not a new capture.
The bounded loop proof unwraps LogicalNot but not scalar ConditionRef introduced
by upstream CFG. Before production edits, add six positive descriptor-store loop
cases (SCC zero/nonzero, unsigned/reversed/signed comparison) and eight rejection
neighbors (wrong success edge, pre-guard read, nonunit step, guarded pointer).
Keep live induction keys, host snapshot limits and runtime store semantics.
CPU RED confirmed: `bounded-condition-cpu-red-20260928.txt.stderr`, exit 1;
first SCC-wrapped descriptor store loses its bounded table and fails host validation.
The unrelated x1-read experiment is preserved outside source in
`_Build/analysis/indirect-dword-experiment-20260928.patch` and reverted from source.

GREEN: unchanged six positives/eight rejection neighbors; four additional EXEC/VCC
reduction cases remain rejected (`bounded-condition-cpu-boundaries-green-20260928.txt`).
Unwrap only SCC ConditionRef and preserve polarity from its already-decoded operand.
Numerical native-CFG BoundedBufferScalarLoopStore{1,3}{Full,Sparse} pass complete
output/SRT/sentinel comparison and SPIR-V validation. Four existing zero-stride
Raw/Formatted/D16/Scalar neighbors GREEN. Captured d895 CPU resource tracking
passes both explicit barrier diagnostic variants; materialization/GPU/game proof
is not inferred from that CPU audit. Native build3 and CTest 22/22 GREEN
(`bounded-condition-fix-build3-20260928.log`, `bounded-condition-ctest-20260928.log`).


## GPU-selected raw buffer fallback integration (2026-09-28)

Game retry on a233dfe1 `yotei-integrated-20260928-173637-menucheck-gpuav-sync`
passes the ordinary-payload SRT failure and reaches CS d8959888aafd2552,
pc 0x1b4: dynamic LoadAddressU32 buffer dword fails host validation.
Upstream already provides a GPU descriptor path limited by
SupportsIndirectBufferLoad to raw DWORD x2/x3/x4, but local GetHandle always
throws instead of returning its new false result to that guarded caller.
Before changing production run existing numerical BufferLoadsGpuSelectedDescriptors
and BufferLoadDwordx3GpuSelectedDescriptors; preserve x1/formatted/typed/store
rejections and bounded-table proof failures. Do not return a null host resource.

Existing numerical fixture RED: `indirect-buffer-red-20260928.txt`, exit 321
at PC 0x30, dynamic ReadConstBuffer descriptor rejected before pipeline creation.
Add CPU admission matrix for raw x2/x3/x4 versus x1/formatted/typed/store,
asserting supported loads use IndirectBuffer without a fake host binding.

CPU matrix RED confirmed: `indirect-buffer-cpu-red-20260928.txt`,
raw x2 fails GetBufferResource runtime validation. Return false only for a
buffer source validation failure after existing bounded proofs; the caller
still enforces the raw vector contract before activating the GPU path.

After admission, the unchanged GPU fixture reaches a second integration RED:
`indirect-buffer-gpu-green-20260928.txt` asserts in host buffer remapping at
ResourceMaterialization.cpp:2788. GPU-selected handles have no host dense index.
Extend the CPU matrix through materialization/specialization before correcting
that remap; raw GPU handles must retain their descriptor operands and kind.

CPU plan extraction RED: `indirect-buffer-specialization-red-20260928.txt`
throws invalid vector subscript in ResourceControlFlow: it also assumes an
IndirectBuffer has a host source index. Exclude only IndirectBuffer from host
source liveness and host remapping; preserve dense/table validation for other
kinds. The unchanged GPU fixture independently reproduces the later remap.

GPU-selected buffer GREEN: unchanged CPU admission/materialization matrix
and full resource suite (`indirect-buffer-specialization-green-20260928.txt`);
numerical x2/x4 and x3 fixtures pass (`indirect-buffer-final-*.txt`), including
11 descriptor variations, bounds modes, swizzle, OOB tails and unmapped rows.
Mandatory CTest 22/22 (`indirect-buffer-ctest-20260928.log`).

## Ordinary scalar payload prefetch (2026-09-28)

Retry on 76d56359 reaches CS 8968b4b53e5a246a, shown=0, flat slot 209.
Temporary bounded diagnostics (`yotei-integrated-20260928-172639-menucheck-gpuav-sync`)
show a host read of address zero at guest PC 0x39dc. The native instruction
is scalar payload under an EXECZ branch, not a descriptor dependency. The
new planner blanket-hoists constant-offset scalar loads before guest control
flow, even when only arithmetic consumes them. Add an unchanged CPU matrix
proving ordinary payload loads stay in guest IR while descriptor dependencies
still receive valid flat slots; numerical scalar-read/cooperative neighbors and
game retry must confirm the shared correction. Do not fabricate zero memory.

CPU RED confirmed: `scalar-payload-red-20260928.txt` rejects eager hoisting
of an ordinary scalar payload. Descriptor dependency is the positive neighbor
in the same unchanged fixture. Restrict host prefetch to resource recipes,
retaining ordinary scalar memory and its guest control-flow position.

Neighbor input correction: numerical BDA coefficient test RED
`scalar-payload-gpu-cooperative-bda-coefficients-20260928.txt` (2304/16516
words differ; coefficient planes pass, mailbox reads return mode-0 defaults).
The fixture replaces the default user-data array but omits word 51: its
zero-stride vector descriptor therefore explicitly requests bounds mode 0,
which correctly always returns OOB zeros. Set its raw byte-address descriptor
to bounds mode 3, matching MakeNativeUserData/MakeStructuredStorageBufferData;
retain all 16516 numerical expectations, alias separation, dispatch geometry
and real mailbox feedback. This repairs the fixture input, not production OOB
semantics. Mode-0 bounded Raw/Formatted/D16/Scalar remain GREEN unchanged.

Scalar payload GREEN: unchanged CPU matrix (`scalar-payload-green-20260928.txt`),
required CTest 22/22 (`scalar-payload-ctest-20260928.log`), bounded numerical
mode-0 Raw/Formatted/D16/Scalar 4/4 (`scalar-payload-bounded-*-final-20260928.txt`),
cyclic scalar address GPU and cooperative LDS 9/9
(`scalar-payload-multiwave-green-20260928.txt`). Corrected mode-3 BDA neighbor
passes all 16516 unchanged values (`scalar-payload-bda-fixture-green-20260928.txt`).
Temporary SRT diagnostics removed before the fix; original logs preserved.

## Wave64 ConditionRef admission (2026-09-28)

Game retry on 4fcc3575 reaches a controlled ConditionRef rejection, shown=0:
`yotei-integrated-20260928-171407-menucheck-gpuav-sync` (CS a7661ff4ea282325).
Before changing production add a synthetic CPU matrix of native mask conditions
with varying lane predicates and ScalarInstruction with uniform/varying inputs.
Mask conditions already reduce both halves in the backend; scalar instruction
conditions must retain their operand uniformity and divergent rejection.
CPU RED confirmed: `condition-ref-red-20260928.txt`, intended assertion
"native ConditionRef lost whole-wave branch reduction in cooperative planning".
Run the fixture RED/GREEN unchanged, existing scalar-mask numerical/Spir-V
cases, cooperative admission, and repeat the original game.

Neighbor fixture API updates: `condition-ref-cooperative-20260928.txt`
asserts while constructing retired four-argument DataAppend (now two args);
`condition-ref-mask-oracle-old-20260928.txt` asserts on retired integer mask
IR. Adapt fixtures to current signatures and ConditionRef kind/predicate,
retaining input matrix and rejection oracles. These are fixture compatibility
failures, not additional production RED evidence.

ConditionRef GREEN: unchanged 14-input admission matrix and six neighboring
CPU/CFG selectors (`condition-ref-neighbor-*.txt`), mandatory CTest 22/22
(`condition-ref-ctest-20260928.log`). Numerical GPU: wave64 subvector loops
and VCC branch pass; multi-wave LDS/cooperative selector 9/9 passes
(`condition-ref-multiwave-gpu-20260928.txt`). No unsupported-case guards removed.

## Fresh upstream 22ff integration regressions (2026-09-28)

Native merge build succeeds (`merge-22ff-build3-20260928.log`). Existing unchanged
resource suite is RED: `invariant indirect images`, address-backed image words at
pc 0x1220 fail runtime-source admission (`merge-22ff-ctest1-20260928.log`, 21/22).
Retain the finite scalar-buffer key enumeration when the image table has a
scalar-address root and positive table offset; the new lane/loop proof remains
required for domains without that independent finite material-buffer bound.
Do not weaken negative immediate, alignment, readability, probe or alias checks.
The preserved address-table fixture includes a shifted key followed by a constant
image-table bias; exclusive-use proofs must cover that complete offset chain
(`merge-22ff-resource2-20260928.txt`, same intended admission RED).

The bounded Raw selector fails before its own fixture is compiled: construction
of an unrelated upstream FP64 case eagerly compiles a vertex reciprocal chain
with unknown initial guest FP state. Log `merge-22ff-zero-raw1-20260928.txt`,
exit 321. Preserve this strict FP64 rejection; move the companion compile check
into the selected case execution so fixture enumeration has no compiler side
effects. Numerical bounded Raw/Formatted/D16/Scalar oracles remain unchanged.

The old shared-exit test's route-field counters describe the retired CFG
representation. Adapt it to the new expression/assignment API by comparing
executed native instruction paths for every branch-decision combination,
retaining instruction coverage and validated structured SPIR-V checks.

Numerical GPU RED after companion isolation: `merge-22ff-zero-raw2-20260928.txt`
returns the first candidate's payload for both workgroups. The dump
`_Build/analysis/merge-22ff-zero-raw-red.spv{,.ir.txt}` shows the bounded
GetBufferResource selector scrubbed to zero by upstream's descriptor cleanup.
Preserve runtime keys for bounded buffer/image/sampler and inline tables;
ordinary host-only descriptor words can still be discarded. Repeat the same
four numerical oracles unchanged and neighboring descriptor-selection cases.

Address-table oracle correction: RDNA2 ISA section 7.2.1 describes the memory
address as base + immediate + scalar offset, without truncating the resulting
sum to U32. The shader's index multiplication wraps before SMEM; adding its
positive immediate must not invent a second wrap. The old address fixture's
112-byte key is unreachable (minimum offset 368). Keep that word as a rejected
read trap and assert only OOB key zero and the real 368-byte key five are mapped.
This follows the same widened addition already covered by TestConstantBufferBounds
and upstream's split-offset matrix. Source:
https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna2-shader-instruction-set-architecture.pdf.

Legacy preplanned-IR regression RED: `merge-22ff-resource4-20260928.txt`
accepts a formatted vector record key because unconditional tracking DCE removes
the unused image query before its strict resource validation. Preserve the
planning-only API contract: run this new DCE only when tracking itself builds
the SRT plan, and rerun the unchanged resource rejection/alias fixtures.

Record-key alias regression RED: `merge-22ff-resource5-20260928.txt`
accepts a written buffer alias of an indirect record key. Include record-key
indirect images in the specialization capture policy, alongside selector masks;
retain the unchanged rejection fixture and non-alias positive cases.

Integer sampler integration RED: `merge-22ff-verified-neighbor-sampler-border-20260928.txt`
exits 321: UInt border-color image has no point variant. The new materializer
classifies UInt as Integer (integer border, original filtering), Sint/converted
as PointInteger. Match backend point routing to that existing classification;
rerun unchanged border-color numerical cases and indirect-image neighbors.

Integration GREEN: unchanged resource suite passes (`merge-22ff-resource6-20260928.txt`),
required CTest 22/22 (`merge-22ff-ctest2-20260928.log`), neighboring upstream
CPU suites 12/12 (`merge-22ff-extra-ctest-20260928.log`), shared CFG execution
and SPIR-V checks pass (`merge-22ff-shared-cfg-20260928.txt`). Four numerical
bounded-buffer cases and five neighbors pass (`merge-22ff-verified-*.txt`);
integer border fix passes `merge-22ff-sampler-border-green-20260928.txt`.
Full compute suite remains unverified: known strict FP64 and comparison-depth
boundaries are preserved, not bypassed by this integration.

## Candidate-specific zero-stride vector reads (2026-09-28)

Before changing production, add numerical native GPU cases
`BoundedBufferZeroStride{Raw,Formatted,D16}`: two dispatch workgroups
load distinct descriptors from a separate immutable SRT, one normal and one
mode-0 zero-stride. Preserve ordinary data, per-candidate float SEL_1 defaults,
D16 packed defaults, inactive EXEC sentinels and all untouched SRT/backing words.
Require table selection in validated SPIR-V. RDNA2 ISA section 8.1.5, Table 35
and range-check notes specify offset >= stride for mode 0 and SEL_1 exceptions:
https://docs.amd.com/api/khub/documents/Et~wpu9g~Ffl7d9q0QZ~Og/content.
Run unchanged oracles RED before lowering and GREEN afterwards; retain direct
zero-stride/scalar and ordinary table boundary checks. GPU work must be bounded.

RED on unfixed production d690678c: all three selectors exit 321 at the
explicit bounded zero-stride rejection, without dispatch or driver failure.
Logs: `_Build/logs/bounded-zero-{raw,format,d16}-red-20260928.txt` and
`.run.json`. The temporary CPU unsupported-case oracle can now be replaced
by retention of per-candidate OOB metadata, covered by these independent
GPU numerical fixtures. GREEN pending.

After admitting candidate-specific OOB, the unchanged Raw fixture exposes
a second compiler RED: `GetBufferResource has an empty argument`
(`bounded-zero-raw-green-20260928.txt`, exit 321). Workgroup-bounded
columns have a dispatch-axis bound and no scalar count root; the typed handle
must retain valid planning operands without inventing a descriptor count.
Preserve the numerical fixture and distinguish its dispatch bound from loop
count roots before correcting handle construction.

The first formatted emit attempt fails SPIR-V validation: vector OpSelect
requires a matching condition shape. Use the existing conditional value
merge, retaining the same sparse-EXEC oracle. Also correct the synthetic D16
encoding to set opcode bit 25 (the helper masks high bits), add a decoded
opcode assertion, and reproduce the corrected D16 case with the candidate
production patch absent. Add an independent bounded scalar read: mode-0
vector OOB must not erase scalar-buffer data.

Corrected D16 RED with all candidate production changes absent:
`bounded-zero-corrected-d16-red-20260928.txt`, exit 321 at the original
mode-0 rejection. Preserved patch: `_Build/analysis/bounded-zero-production-in-progress-20260928.patch`.
Bounded scalar numerical RED: `bounded-zero-scalar-red-20260928.txt.stderr`
returns zero for all four lanes of the second workgroup instead of 0x40000000.
Keep ScalarBuffer accesses on their existing scalar bounds path.

Native numerical GPU GREEN: all four cases, unchanged final oracles,
`bounded-zero-{raw,formatted,d16,scalar}-final-20260928.txt`, exit 0.
The corrected D16 fixture retains its decoded-opcode assertion. All emitted
modules pass Vulkan 1.2 SPIR-V validation. Neighbor selectors GREEN:
zero-stride-oob, buffer-format-store, buffer-d16, indirect-image; logs
`bounded-zero-neighbor-*-20260928.txt`. Executable SHA-256:
`536463B7358654AD1CC7E5088C352FE17D248A56AA4804BEF7789D8A31BE5619`.
Native emulator/launcher/kyty_tests build and install GREEN:
`bounded-zero-native-{build,install}-20260928.log`. Focused CTest GREEN
22/22: `bounded-zero-native-ctest-20260928.log`, including required Windows
checks and existing resource tracking/materialization boundaries. Original
game retry remains pending at this checkpoint.

Separately, exact-head CI d690678c failed Linux compilation at two test-only
zero-argument optional emplace calls (Windows/macOS succeeded). Use explicit
aggregate initialization without changing semantic expectations. Evidence:
`_Build/logs/ci-d690-linux-failure.txt`.

## Upstream integration and SRT rejection regressions (2026-09-27)

Native required checks on the merge of `e0c73250` and `421684e7` expose
unchanged CPU RED cases: `TestConstantBufferBounds` accepts an out-of-bounds
planning read, and the wrapped scalar-buffer immediate in
`TestInvariantIndirectImages` passes runtime admission. Evidence:
`_Build/logs/merge-focused-ctest-20260927.log`. Before correcting production,
preserve these rejection oracles, retain in-range reads, and reject unreadable
non-null planning roots rather than fabricating zero descriptors. Scalar-buffer
admission must agree with the evaluator's unsigned offset constraint. The
RO-upload fixture must contain a live memory access under upstream binding
pruning; its byte/padding oracle remains unchanged. GREEN and game retry pending.

After restoring those rejections, the existing `TestUniformScalarBufferImage`
also exposes RED: draw-uniform nested descriptors are captured by inline GPU
descriptor lowering instead of normal runtime materialization. Preserve its
three numerical descriptor-address oracles and the unreadable-memory rejection;
uniform descriptors must keep the ordinary specialization path.

The next existing RED is `TestSrtRawFallbackReadability`: the no-callback
SRT fallback directly dereferences null/reserved/no-access host memory, causing
native CPU AV. Evidence: `merge-resource-crash-localize-20260927.txt` and the
existing `--srt-raw-fallback-case` fixtures. Require an exact, fallible DWORD
copy through the host OS, preserve successful readable bytes and transactional
failure, and retain callback-based guest-memory reads. This is CPU memory
admission, not a GPU-driver workaround. GREEN pending.

Existing finite-selector rejection RED (`merge-resource-finite-red-20260927.txt`)
shows that a four-column descriptor with a displaced fourth column bypasses
the failed correlated-table proof via generic bounded-expression admission.
Preserve mixed-column/mixed-index, unknown/undef/cyclic/conditional-root and
stage rejection fixtures; retain positive contiguous columns and genuine
derived bounded descriptor expressions. No broader descriptor assembly is
claimed without an independent regression.

Existing finite-selector materialization RED then exposes an immutable-source
writer alias accepted by the `bounded_srt_reads_precede_writes` bypass. A local
CFG ordering observation is not a dispatch-wide ordering proof across waves
or workgroups. Preserve exact-end nonoverlap, readable candidate snapshots,
transactional failure and the last-DWORD overlap rejection before changing
alias admission. Evidence: `merge-resource-correlated-green-20260927.txt.stderr`.

Next RED: `TestWorkgroupSrtTrackingProof` rejects an otherwise proved acyclic
WorkgroupId read solely because its CFG has no cycle. Preserve the same affine
proof and coherent snapshot/alias checks for acyclic and cyclic inputs;
host-read failures must remain errors. Evidence: `merge-resource-alias-green-20260927.txt.stderr`.

Existing transactional-limit RED: `TestBoundedMaterializationLimitsAreTransactional`
accepts 65,538 logical probes across two columns after the WIP raised the
combined budget. Preserve the unchanged 65,536 success and 65,538 rejection
oracles, including repeated-address memoization and unchanged snapshots on
failure. Keep allocation capacity separate from the logical probe budget.
Evidence: `merge-resource-workgroup-green-20260927.txt.stderr`.

Conditional-buffer RED (`merge-resource-budget-green-20260927.txt.stderr`)
shows that the WIP lost upstream reachability-aware descriptor materialization.
Preserve existing untaken/taken, absent descriptor, failed predicate read, shared
use, loop and writable-alias cases. Only proved unreachable sources may skip
evaluation; unknown predicates keep every potentially executed source.

Existing image-binding ABI RED (`merge-resource-indirect-green-20260927.txt.stderr`)
accepts a depth-comparison storage image. Preserve the existing comparison,
integer, atomic and dimension rejection matrix; comparison sampling applies
only to sampled resources.

Native `resource_tracking_tests` GREEN: all unchanged rejection and positive
fixtures pass after the shared corrections, including eight raw host-memory
readability cases. Evidence: `_Build/logs/merge-resource-image-abi-green-20260927.txt`
and its `.run.json` (exit 0). The added unreadable planning-root regression
was recorded RED before its fix. Full rebuilt focused checks and GPU cases
remain pending at this checkpoint; game progress is not inferred from CPU GREEN.

SDWA MOV numerical fixture RED: the decoder rejects byte source extraction
combined with partial destination insertion, although shared translation
already treats both fields independently. Preserve the unchanged overlapping
source/destination, sign extension and inactive EXEC oracles before admitting
those combinations. Evidence: `merge-gpu-sdwa-mov-20260927.txt`.

FP64 fixtures need descriptor setup aligned with upstream zero-stride mode-0
semantics: their explicit user data overwrites default mode-3 bounds bits.
Retain every IEEE literal oracle and sentinel, set raw-buffer bounds mode 3
explicitly, and retain the separate zero-stride mode-0 rejection GPU fixtures.
Evidence: `merge-gpu-f64-arithmetic-20260927.txt.stderr` and conversion log.

Indirect-image selector RED is a host test AV (`merge-gpu-indirect-image-20260927.txt`,
Application Error fault module is the test EXE). A diagnostic native link map localizes the AV to `EmitImage`: ordinary
non-comparison sampling unconditionally reads an empty optional specialization
array for `needs_manual_depth_compare`. Preserve the unchanged indirect-image
fixtures; only comparison instructions with supplied specialization need that
lookup. Evidence: `merge-indirect-map-crash-20260927.txt` and
`_Build/analysis/indirect-diagnostic.map` (fault RVA 0x3a633e).
Separately, the fixture's SPIR-V assertion treats an `OpConstant` literal as an
SSA ID while inspecting LOD. Accept direct float constants and equivalent
bitcasts with bounds checks, preserving exact 2.0 LOD and coordinate dimensions.

GPU checkpoint: SDWA MOV GREEN (`merge-sdwa-green-20260927.txt`), all ten
FP64 conversion cases GREEN (`merge-f64-conversion-green-20260927.txt`).
FP64 arithmetic passes multiply, fused FMA, signed-zero FMA and F64-to-F32
rounding, then `RcpF64OddConvertedIntegersWithinIsaError` terminates with
`0x80000003` inside NVIDIA `nvgpucomp64.dll` 32.0.16.1714 at offset 0x589eb2.
Evidence: `merge-f64-arithmetic-green-20260927.txt` and its `.run.json`;
Windows Application Error event confirms the driver module. Do not claim the
arithmetic selector GREEN; an isolated saved-SPIR-V probe remains required.

After removing that AV, the unchanged mixed-candidate key-switch test is RED:
`merge-indirect-emitter-green-20260927.txt.stderr` reports missing shared cube
coordinate conversion. The candidate sample path calls `CoordF32` without
the selected image cube metadata. Preserve both cube-first permutations,
the two coordinate-subtraction oracle, exact LOD and three sample cases;
apply guest cube conversion for each candidate from decoded resource metadata.

The same unchanged LOD oracle then exposes RED at mixed coordinate/LOD layout
(`merge-cube-candidate-green-20260927.txt.stderr`): a 2D candidate relocates
the LOD slot of an instruction decoded as 2D-array. Guest address operands
follow decoded instruction metadata; candidate dimensions change host coordinate
width, not the instruction LOD/bias/gradient slots. Preserve LOD=2.0 for
both candidate orders before correcting the shared sample operand layout.

Indirect-image selector GREEN after the shared emitter corrections:
`merge-sample-layout-green-20260927.txt`, exit 0. Both mixed cube orders,
exact LOD assertions and the numerical cube-gradient GPU case pass. Temporary
disassembly instrumentation was removed; its RED artifact remains in `_Build/logs`.

Neighbor check `--sampled-depth-resource-only` remains RED at the existing
`ComparisonDepthTexture` assertion requiring `OpImageSampleDrefExplicitLod`:
`merge-sample-depth-neighbor-20260927.txt.stderr`. Manual depth-compare lowering
is already present in pre-merge `e0c73250`; distinguish host format capability
admission and the fixture opcode requirement from numerical comparison results
before changing either. Its GPU numerical stage has not been reached.

Game retry on `1f1f7665` reaches a binding-collector bounds error before present:
`_Build/runs/yotei-integrated-20260927-201152-menucheck-gpuav-sync`, exit 321.
A specialized bounded buffer access has a logical table and multiple dense
candidates, not one `memory.resource`. Before changing collection, add a CPU
fixture with reordered/duplicate candidates, a separate direct buffer, and
invalid table/candidate boundaries. Require only live dense resources in
bindings, preserving their ascending compact order and packed shader-data ABI.

Bounded-binding regression RED/GREEN: unchanged valid fixture fails on the
pre-fix collector (`merge-bounded-bindings-valid-red-20260927.txt`) and passes
with candidate collection (`merge-bounded-bindings-valid-green-20260927.txt`).
Full native rebuild, shader validation and original game retry remain required.

Second game retry (`yotei-integrated-20260927-201838-menucheck-gpuav-sync`,
`e47a15c7`, exit 321) passes the collector and exposes a CPU AV in
`ApplyResourceSpecialization` (RVA 0x268ce7): upstream zero-stride read rewriting
indexes a logical-table access as a direct dense resource. Before fixing it,
add empty/nonempty vector-table application tests with preserved live reads.
A vector table containing a mode-0 zero-stride candidate needs candidate-specific
zero/default-format lowering; reject that unsupported combination explicitly
until its independent numerical GPU regression exists. Do not disable the
existing direct zero-stride rewrite or treat every table candidate as zero.

Vector-table application CPU RED reproduces the native AV without Vulkan:
`merge-table-vector-red-20260927.txt`, exit 0xc0000005. Unchanged GREEN:
`merge-table-vector-green-20260927.txt`, exit 0, empty and nonempty table reads
remain explicit and mode-0 zero-stride candidates fail with the supported-case
boundary. Candidate-specific zero/default-format GPU lowering is pending.

Final checkpoint on `e55e5053`: native full target build/install passed,
focused CTest 22/22 and eight GPU selectors 8/8 passed. Evidence:
`merge-table-final-build-20260927.log`, `merge-table-final-ctest-20260927.log`,
`merge-gpu-*-table-final-20260927.txt`. Original game retry
`yotei-integrated-20260927-202436-menucheck-gpuav-sync` passes the two native
integration crashes but stops at the explicit bounded mode-0 zero-stride vector
candidate boundary (exit 321, shown=0). Next required regression is numerical
selection between normal and zero-stride candidates, formatted defaults/D16
and sparse EXEC, before shared candidate-specific lowering. Menu remains pending.

## Restored RO aligned-upload regression source (2026-09-27)

The historical RED/GREEN logs existed but the selector implementation had been
lost from the worktree. Restored `--storage-buffer-ro-aligned-upload-only` now
checks the real renderer binding and copies its upload bytes back from Vulkan.
The unchanged fixture fails with `HEAD` descriptors at guest offset 2
(`_Build/logs/ro-align-restored-red-20260927.txt`, exit 321, unsupported rebase)
and passes with the preserved WIP implementation
(`ro-align-restored-green-20260927.txt`, exit 0). Offsets 1/2/3 and zero padding
are covered; neighboring byte-offset binding and rejection tests pass. Pending:
independent GPU-dirty read-only negative fixture. A hash-only cross-ProgramKey
SPIR-V reuse experiment was removed before commit because it omitted binary
equality; the existing collision-checked sibling permutation reuse remains.

## Runtime sampler binding in synthetic GPU cases (2026-09-27)

Existing `ImageSampleR128DynamicMaterialPairs` numerical RED:
`_Build/runs/synthetic-image-20260927-routing/00000/stderr.txt` expects
`[80,8,20,2]` but receives `[80,8,80,8]`. The harness ignores its
`use_runtime_samplers` flag and binds a single ClampToEdge sampler. Before
using these fixtures as emulator evidence, bind each materialized sampler
through the runtime sampler cache and rerun the unchanged numerical oracle.
Static sampler coverage must remain green; no production sampler change is
justified by this harness failure.

GREEN: `_Build/runs/synthetic-image-20260927-runtime-samplers-green/report.json`
passes both unchanged numerical fixtures (2/2) with runtime samplers bound.

## Graphics wave64 emitter routing on subgroup32 (2026-09-27)

Before changing collective emission, prove a CPU SPIR-V regression for the
existing graphics partition contract: a Vertex wave64 module on native32 must
route ballot through the shared partition helper and normalize shuffle targets
to native lanes. Cover Pixel too and leave compute/dual-context wave64 paths
unchanged. This restores existing partition behavior; it does not establish
true cross-subgroup graphics wave64 equivalence. The captured vertex mask
clearing loop suggests this missing routing can prevent termination; that
runtime attribution requires a bounded game retry and source/menu evidence.

Confirmed CPU RED: `_Build/logs/graphics-wave64-red-20260927.txt.stderr`
reports `graphics wave64 shuffle target bypassed native32 normalization`.
The unchanged selector passes after restoring shared helper routing:
`graphics-wave64-green-20260927.txt` (native Windows, exit 0).
Neighboring partitioned graphics loop, cooperative collective functions and
split-wave64 swizzle/ballot selectors pass. Game attribution remains pending.

## Homogeneous indirect image sampling: opaque SSA validation (2026-09-27)

Required regression before correcting the uncommitted sampling optimization:
`shader_recompiler_compute_tests --homogeneous-indirect-image-validation-only`.
Two independent same-dimension float image candidates must emit valid Vulkan
SPIR-V and preserve candidate selection. `OpSampledImage` results cannot be
merged through `OpPhi`; sample in each branch and merge numerical results.
The CPU selector validates SPIR-V without initializing the GPU. Keep existing
heterogeneous dimension/numeric coverage and require GPU candidate readback
before claiming the 250-candidate game draw is fixed. Menu remains pending.

This file records regression coverage deferred during fast launch bring-up. Each item
describes a guest contract rather than a title-specific workaround. Deferred tests must
be added before the corresponding fixes are proposed upstream.

## Workgroup-axis bounded SRT pipeline identity

Status: GREEN materialization contract (26 September 2026); game still sees
remaining `SpecializationMiss` from **non-workgroup** columns.

Game evidence `…-212219-presentprobe-gpuav` (LOGF tip): CS `54904fb419d79e49`
`SpecializationMiss` ×3 with `bounded_srt[2..4]` counts `16→14→19→11` (and
matching flat-offset shifts). Columns `[0],[1]` stay `count=1`; `[5],[6]` stay
`0`. `ProgramKeySplit=0` for this hash. Workgroup-axis reserve is therefore
insufficient alone — selector/count-source layout counts still splinter
CreatePipeline identity (cold GPUAV ≈270s/variant; warm cache hides cost).

Contract (extended): count-source / selector-sourced bounded columns need a
stable layout upper bound (or non-module-affecting count) so dispatch-varying
live sizes do not create new SPIR-V, matching the workgroup-axis reserve
rule. Live words still fill only the used prefix.

GREEN (workgroup only): `resource_tracking_tests --workgroup-srt-materialization-only`
→ `KYTY_WORKGROUP_SRT_MATERIALIZATION_PASS`. Selector-stable reserve: RED pending.

## VS indirect-image expansion DeviceLost (~shown=191)

Status: failing draw identified; cause not established. The attempted opaque
SSA optimization failed CPU validation on 27 September and was removed.

SyncDiag + GPUAV8: DeviceLost on first draw using VS `0xe3125617f3efc38f`
(guest ES `0x803f946a00`) with PS `0x803fe78e00`, indexed count=119856.
SpecializationCompile: `images=251 sampled_pairs=250`,
`indirect_search_iterations=9`. Neighboring VS specialize with `images=0`.
Prior same-PS draws with ES `0x803fba0000` complete. Menu blocked here.

Read-only SPIR-V inspection on 2026-10-03: the VS loop selects a lane from
two 32-bit masks, normalizes the guest target to native 0..31, shuffles,
then uses `OpGroupNonUniformBallot` whose lower 32-bit word is duplicated
into both guest words. The duplicated words clear both loop masks. This is
the documented partial native32 graphics wave64 model; static inspection
does not prove the loop hangs in the observed draw. The existing 256-iteration
diagnostic budget applies to Pixel only, not Vertex. Before attributing
DeviceLost to this loop, add a bounded synthetic Vertex case with
independently expected high/low masks and a completion signal. A silent
vertex-loop cap would discard guest work and cannot be shipped as a fix.

Contract: preserve candidate selection and valid Vulkan SPIR-V. Opaque
`OpSampledImage` values cannot be merged by `OpPhi` or consumed across blocks.
The existing per-candidate sampling switch remains until a legal alternative
has independent validation and numerical GPU coverage. The CPU-only
`--homogeneous-indirect-image-validation-only` proves the attempted variant
invalid and passes with numerical result merging restored. Logs:
`_Build/logs/homogeneous-image-red2-20260927.txt.stderr` and
`homogeneous-image-green2-20260927.txt`. This does not prove the draw fixed.

## Current Windows regression status after branch reconciliation

Status at source revision `9a29c306` (25 September 2026): native Windows
`launcher` + `kyty_tests` build passed. The three required Windows CI tests
and three neighboring memory/kernel/resource tests passed 6/6. Logs are in
`_Build/merge-validation-20260925/final-full-build.*` and `final-tests.*`.
The full registered 55-test suite was not run; do not carry forward the old
46/51 result as a current result.

The bounded extended CPU run (`extended-cpu-tests.*`) passed 1/3:

- `scalar_provenance` now passes.
- `shader_cfg` passes the earlier wide-buffer budget check and the restored
  SAVEEXEC mask operation, then fails `nested-tail routing changed semantic
  instruction coverage`. This test and CFG implementation already existed at
  the pre-integration branch head; the newly reached failure needs its own
  unchanged regression and CFG-level correction.
- `resource_tracking` still fails because the invariant indirect-image proof
  admits a wrapped scalar immediate. This failure was recorded before the
  current integration.

Additional resource selectors `finite-selector-srt-proof-only`,
`finite-selector-srt-materialization-only` and
`workgroup-srt-materialization-only` fail. Their logs are under
`_Build/merge-validation-20260925/resource7-*`. The finite selector checks
expose missing rejection of unproved selection and writable descriptor-source
overlap; the workgroup check exposes an exceeded combined probe budget. These
results are not included in the 1/3 CTest count and need independent fixes.
The earlier `shader_recompiler_compute` and `texture_cache_image_overlap`
sampled-alias failures were not rechecked on this revision.

For history, the 9 September merge `a309653` of `upstream/main` at `0b4e78c`
had 46/51 extended CTest passes, with wide-buffer, scalar-provenance,
resource-tracking and two sampled-alias failures. Keep historical and current
results separate when judging launch readiness.

## Cooperative wave64 read-only LDS phase batching

Status: CPU regression added and Vulkan neighboring coverage complete; large-module
driver compile remains open.

Observed trigger: the cooperative emitter ended every LDS instruction with a full
workgroup rendezvous. Consecutive read-only accesses therefore created redundant phases,
guards and cross-phase spills even though no invocation could publish a conflicting LDS
write between them. The exact large compute module shrank from 702,611 to 697,920 words,
but NVIDIA `vkCreateComputePipelines` still exceeded the 60-second frame watchdog.

Covered tests:

- One and two consecutive read-only LDS accesses emit the same number of
  `OpControlBarrier` instructions.
- An intervening LDS write retains both required read/write hazard boundaries.
- Cooperative SSBO, cyclic scalar/physical address and BDA coefficient Vulkan tests
  execute successfully on the native Windows GPU path.

Remaining debt:

- Factor repeated split-wave64 ballot/readlane lowering into reusable SPIR-V functions
  without changing workgroup scratch, dynamic-uniformity or barrier semantics.
- Ratchet exact synthetic module size and validate that bounded pipeline creation no
  longer exceeds several minutes under GPUAV instrumentation before claiming menu
  progress. Game evidence 26.09 `…-081840`: CS `54904fb419d79e49` did **4×**
  specialization CreatePipeline ≈290/295/287 s under GPUAV+instr; watchdog
  shown=159 before spinner readback.

## Compressed video-out metadata on a native render-target alias

Status: regression RED confirmed on Windows 2026-09-28.
`--buffer-cache-range-only` fails at "video-out metadata on render-target alias"
with merge 8e61798b and identically with only the two upstream textureCache files
replaced by HEAD 45e09731 versions. Logs `merge-8e-image-ownership-20260928` and
`merge-8e-image-ownership-baseline-20260928` (bounded, no timeout). New upstream
ownership cases later in the fixture are not reached. Shared metadata fix pending.

Observed trigger: a guest color target is first discovered as a native render target
with DCC metadata, then the same allocation is acquired through VideoOut with an
explicit compressed-surface classification. `TextureCache::FindImage` correctly
reuses the native image, but the requested VideoOut metadata must not be lost when
the cache record was created by the render-target path.

Required tests:

- A render-target discovery followed by a compressed VideoOut lookup for the same
  backing must reuse the native image and retain the VideoOut compression/control
  metadata needed by presentation.
- An uncompressed VideoOut lookup must not erase DCC metadata already registered
  for a native render target.
- The alias must remain GPU-owned and must not trigger a guest upload or compressed
  readback while preparing the frame.

## Runtime descriptor-evaluation failure diagnostics

Status: production diagnostic and native game validation complete; automated regression deferred.

Observed trigger: a descriptor source can pass the static runtime-value validator and still
fail when evaluated against the current dispatch state. The old message identified only the
source and DWORD, hiding whether the cause was a short user-data span, a cyclic value, an
unsupported opcode, invalid memory metadata, an out-of-range scalar-buffer read or unavailable
guest memory. This made a generic correction indistinguishable from a transient dirty-resource
retry.

Required tests:

- Immediate, user-data and shader-base sources proving that successful evaluation remains silent
  and a user-data index outside the current runtime span reports both the requested and available
  ranges.
- Unsupported opcode, undefined value, malformed composite extraction and cyclic dependency
  cases proving that the deepest failing opcode/reason is retained in the source/DWORD diagnostic.
- Scalar-address and scalar-buffer reads covering invalid metadata, failed address arguments,
  negative/out-of-range offsets and unavailable guest memory, with the computed address included
  where one exists.
- `ReadFirstLane` and clean-snapshot delegation cases proving that a nested evaluator's reason is
  propagated through the outer descriptor source rather than replaced by a generic failure.
- Transactional materialization checks proving that detailed diagnostics do not mutate the
  previous `ResourceSnapshot` or `ResourceSpecialization` on failure.

## Bounded descriptor columns behind flattened SRT slots

Status: production fix and native game validation complete; automated regression deferred.

Observed trigger: `BuildSrtPlan` can replace a scalar descriptor-table load with `ReadConst`,
while the saved flat slot still resolves to a read that resource tracking later converts to
`ReadBoundedSrtU32`. Bounded buffer/image recognition looked only at the immediate handle argument,
so it missed four/eight correlated columns hidden behind those flat slots. Ordinary runtime
materialization then tried to evaluate the GPU-selected bounded value as one invariant descriptor.

Required tests:

- Four correlated buffer-descriptor columns passed directly as bounded reads and through four
  distinct `ReadConst` flat slots, checking that both forms produce the same dense candidate table.
- Eight correlated image-descriptor columns behind flattened slots, checking descriptor
  deduplication, key mapping and null/invalid descriptor normalization.
- Mixed direct/flattened columns, nested flat slots and shared flat slots proving recognition
  follows only exact `SrtRead` values, rejects a slot cycle and preserves the live GPU selector.
- Rejection cases for an invalid flat index, a non-immediate slot, unrelated bounded reads,
  mismatched count/address/stride/bias, partial descriptors and workgroup-indexed tables.
- Materialization and SPIR-V checks proving the flattened snapshot remains immutable, all column
  reads share one coherent transaction, and the generated resource-table lookup is bounds safe.

## Dependent buffer, image and sampler descriptors addressed by bounded SRT values

Status: production fix and native game validation complete; automated regression deferred.

Observed trigger: a buffer, image or sampler descriptor can be loaded through `LoadAddressU32`
whose address itself comes from one or more `ReadBoundedSrtU32` values. The bounded values are
GPU-selected, so ordinary host evaluation cannot produce one invariant descriptor; direct
four/eight-column table recognition also does not apply because the bounded read is an address
dependency rather than a descriptor DWORD.

Required tests:

- A bounded low/high pointer pair feeding four dependent `LoadAddressU32` descriptor words,
  checking that materialization evaluates every proved selector candidate and preserves the live
  selector in generated SPIR-V.
- The equivalent eight-word image descriptor, including invalid/null normalization, typed
  candidate specialization and sampled/storage binding selection.
- A four-word sampler expression paired with an image expression using the same proved selector,
  checking that every dense image candidate receives the matching sampler descriptor and depth
  comparison function.
- Repeated image descriptors with different sampler descriptors, proving candidate deduplication
  uses the image/sampler pair and does not silently collapse distinct filtering state.
- Candidate deduplication, zero-count and repeated-address cases, checking dense buffer IDs and the
  flattened selector mapping against an independent oracle.
- A bounded scalar-buffer dependency whose final candidate is outside the descriptor byte extent,
  checking that it snapshots the guest buffer-load zero result while an unavailable in-range host
  read still fails the transaction.
- Dependent descriptor evaluation through scalar-buffer and raw-address reads, checking that an
  out-of-bounds scalar-buffer word and a null-base raw read in an over-approximated bounded
  candidate produce zero, while every non-null unavailable or GPU-dirty address still fails.
- Multiple bounded dependencies with the same key/count but different source tables, and nested
  runtime-uniform address arithmetic, proving one coherent snapshot transaction covers all reads.
- Several descriptor columns sharing one 16-bit selector domain, proving the 65,536-candidate
  limit applies to each read while a separate bounded total-word budget covers the coherent
  snapshot; also check that exceeding either limit fails before any partial snapshot escapes.
- Flattened `ReadConst` slots wrapping GPU-selected address dependencies, checking that the base
  snapshot reserves those slots without eagerly evaluating them and candidate evaluation still
  follows the original saved value graph.
- Rejection cases for different selector values, counts, signedness, workgroup axes, invalid read
  IDs, dependency cycles, unavailable/dirty descriptor memory and descriptor/resource limits.
- A bounded sampler used without a correlatable bounded image, and an image referenced with two
  different bounded sampler tables, proving unsupported independent sampler selection fails closed.
- Writable-alias and transactionality cases proving no eager snapshot overlaps a writable guest
  range and no partial candidates escape after any dependent read fails.

## Wave64 uniformity through bounded resource selection

Status: production fix, offline manifest audit and native game validation complete; automated
regression deferred.

Observed trigger: a split-wave64 compute shader reaches a conditional branch after its
buffer/image/sampler resources have been specialized from a bounded selector. The execution
planner rejects the branch at guest PC 612 because the current greatest-fixed-point analysis
classifies at least one value in the condition as lane-varying without reporting the dependency
that caused the classification.

Required tests:

- A branch whose condition depends on each runtime resource-selection opcode that is emitted once
  per guest wave, proving only values with an explicit wave-uniform execution contract are accepted.
- A scalar `LoadAddressU32` branch with a uniform handle/offset and matching `ScalarAddress`
  metadata, plus varying-handle, varying-offset, missing-metadata and non-scalar-address rejection
  cases.
- The corresponding per-lane buffer/image result and lane/subgroup-derived selectors, proving a
  bounded descriptor table alone never makes the resource operation's returned data uniform.
- Nested `ReadConst`, `ReadBoundedSrtU32`, scalar-address arithmetic, comparisons, selects and Phi
  nodes, checking that uniformity propagates through pure operations and loop-carried values only
  when every reachable input and control edge is uniform.
- A diagnostic case that reports the first varying opcode/value chain and guest PC for a rejected
  branch, so corpus failures can be grouped without weakening the convergence rule.
- Split native32, native64 and cooperative multi-wave execution coverage, with a collective inside
  the conditional arm to prove accepted branches keep both native halves at matching rendezvous.

## Guard-bounded inline sampled tables

Status: production fix and native game validation complete; automated regression deferred.

Observed trigger: a material shader multiplies a dynamically reduced selector by a 368-byte record
stride, but a dominating unsigned branch admits descriptor loads only while the selector is below
255. The old materializer discarded that CFG fact and enumerated the full wrapped-U32 domain:
1,445 in-buffer offsets at 16-byte GCD spacing, exhausting the candidate limit on records the shader
cannot access. Invalid images also retained unrelated adjacent sampler words as artificial pairs.
After preserving the guard, the table has 24 candidates; combined with 115 direct images it needs
138 dense image slots. A second static-state/runtime permutation of the same shader needs 285 dense
images. The compiler image and sampled-pair ceilings are raised to 512 while sampler capacity
remains 128 and final renderer admission continues to enforce the physical Vulkan descriptor limits.

Required tests:

- Direct image and sampler tables under equivalent dominating `selector < limit` and
  `selector >= limit` guards, proving only the bounded branch records the exclusive limit.
- Nested and loop-carried guarded selectors, including a path that bypasses the guard and an
  opposite branch that can re-enter through a loop, proving the bound is accepted only when the
  guarded edge is the sole immediate region containing the descriptor handle.
- A selector without a proven guard, proving materialization retains full wrapped-U32/GCD
  enumeration and its existing probe limit.
- A wrapped selector that visits valid records and misaligned words, proving every invalid image is
  paired with the canonical null sampler while valid image/sampler combinations stay distinct.
- Multiple different sampler bit patterns beside null images, proving they map to one candidate and
  do not consume sampler origins, point-sampler clones or sampled-pair slots.
- Null and valid image candidates sharing the same selector, checking exact key-to-candidate mapping,
  emitted switch cases and a native sampled readback for both paths.
- A genuinely bounded table with 129 distinct compatible image/sampler pairs, proving the unchanged
  sampler and sampled-pair ceilings still reject real resource pressure transactionally.
- Shaders with 138 and 285 total direct and indirect images, plus a 513th-image/pair rejection,
  checking dense remapping, bounded compiler work and physical Vulkan descriptor-budget validation.
- The captured shader with selector limit 255 and stride 368, followed by Vulkan and SPIR-V
  validation and a game run that advances past its former 129th artificial pair.

## Null optional scalar-address descriptor roots

Status: production fix, native build and bounded game validation complete; automated regression
deferred.

Observed trigger: eager host materialization of a vertex shader descriptor source evaluates four
`LoadAddressU32` words from an optional SRT pointer whose exact 48-bit base is zero. Adding the
descriptor's first offset produces guest address `0x10`, which is intentionally unmapped; the
optional branch should specialize to a null descriptor without weakening failures for non-null
unreadable or GPU-dirty memory.

Required tests:

- A four-word buffer descriptor source loaded from an exact null `GetAddressResource` base with
  positive immediate offsets, proving every word materializes as zero.
- Null bases with zero, positive and negative scalar/immediate offsets, proving pointer arithmetic
  is not performed after the null root is established.
- A non-null readable base, proving ordinary descriptor words remain unchanged.
- A non-null unreadable base and a non-null GPU-dirty specialization read, proving both still fail
  closed with the exact guest address.
- A guarded runtime path where the optional descriptor is unused when null and used when present,
  checking null binding behavior and the captured `ee4f153aa500d327` vertex shader.

Validation compiled `ee4f153aa500d327` through resource tracking and emitted 15,847 SPIR-V words;
the game then advanced to renderer descriptor binding and failed at the next independent storage
buffer offset boundary. Exact null roots specialize to zero before pointer arithmetic, while
non-null unreadable and GPU-dirty roots retain the existing fail-closed path.

## Storage-buffer backing offsets beyond the packed residual ABI

Status: production fix, native build and bounded game validation complete; automated regression
deferred.

Observed trigger: after the optional null SRT shader compiles, renderer binding obtains a guest
storage-buffer subrange whose offset inside a shared Vulkan backing cannot be represented by the
current one-byte residual field or violates the access-shape admission rules. The current fatal
does not print the guest address, backing offset, device alignment, residual, range or resource
access facts, so the exact boundary must be measured before selecting a representation.

Required tests:

- Shared backings with residual offsets at 0, 1, 3, 4, 255, 256 and the device alignment minus
  one, checking the descriptor offset, bound range and shader-visible effective byte address.
- Raw, scalar, typed, formatted 8-bit and formatted 16-bit reads and writes at aligned and
  unaligned guest addresses, including accesses spanning two native DWORDs.
- A residual wider than the packed one-byte shader-data field, proving it is either rebased into
  a dedicated buffer view or represented by a wider ABI without truncation.
- Exact-end, partial-DWORD-tail, maximum Vulkan range and arithmetic-overflow cases, checking that
  out-of-range guest bytes cannot become visible through alignment expansion.
- Read/write and atomic resources backed by an existing larger allocation, checking cache
  invalidation and aliasing after any rebase or dedicated-view fallback.
- Vulkan validation/readback plus a bounded game run past the renderer failure following shader
  `ee4f153aa500d327`.

Diagnosis measured vertex slot 6 at guest address `0x80760a1a16`, size `0x240`, Vulkan alignment
16 and residual 6. The resource is descriptor-formatted-only, read-only and non-atomic. The new
shader-data layout carries an exact byte limit per buffer, so the Vulkan range may be rounded to a
complete DWORD without exposing padding to guest accesses; 16-bit cross-DWORD loads and stores
join or split their two backing words. Both native targets build. Cache-warming runs have reached
shader 131 without fatal or validation output. The final stable-SHA validation run compiled all
142 shaders, accepted the slot-6 binding and advanced to the next independent graphics-stage
interface VUID.

## Missing producer declarations for consumed graphics-stage parameters

Status: production fix, native build and bounded game validation complete; automated regression
deferred.

Observed trigger: a fragment shader declares a per-vertex `array[3] of vec4` input at SPIR-V
Location 2, while the paired vertex shader does not declare an output at that location. Both
modules validate independently, but `vkCreateGraphicsPipelines` rejects their combined interface
with VUID `RuntimeSpirv-OpEntryPoint-08743`.

Required tests:

- A pixel parameter consumed at a location exported by the vertex shader, proving no duplicate
  output declaration is added and the original value remains connected.
- A consumed pixel location absent from vertex exports, proving the producer declares a matching
  `vec4` output and the graphics pipeline passes interface validation.
- Per-vertex barycentric fragment inputs, including the three-element fragment array contract,
  matched against the preceding vertex stage's non-array output declaration.
- Sparse locations, flat and no-perspective inputs, repeated guest interpolator mappings and
  collision-remapped host locations, with deterministic producer declarations for every consumer.
- Vertex-program cache variants paired with different fragment input masks, proving the interface
  requirement participates in the static key and cannot reuse an incompatible module.
- Host `maxVertexOutputComponents` admission and a bounded validation run beyond the pipeline pair
  containing vertex shader `ee4f153aa500d327`.

The program cache now compiles the active pixel stage first and carries its exact guest-source to
host-location parameter links into the paired vertex static key. Vertex compilation keeps matching
exports, duplicates a produced value when collision remapping needs an alias, removes an
unconsumed output that occupies a required location, and declares an unwritten synthetic output
when the guest producer is absent. A native Windows build of both targets completed. Diagnostic
run `yotei-integrated-20260907-051751-146531` advanced from shader 142 to 158 without the previous
Location 2 VUID or another validation/fatal message.

## Shared descriptor dependency DAG traversal

Status: production fix and exact captured-shader performance validation complete; bounded game
validation and automated regression deferred.

Observed trigger: captured compute shader `7ceb0f3417f926f9` reaches resource tracking with 3207
normalized IR instructions, then spends more than 75 seconds in `Collect` for one
`ImageSampleRaw`. The image and sampler descriptor values contain a large Phi/Select DAG with
shared subgraphs. `CollectBoundedDependencies` remembers only the current recursion stack, so a
valid acyclic subgraph is revisited once per incoming path and traversal grows exponentially.

Required tests:

- A diamond-shaped acyclic descriptor expression with exponentially many paths but linearly many
  IR nodes, proving each completed node is traversed once for one descriptor.
- Shared subgraphs across all image and sampler descriptor DWORDs, proving the completed-node set
  is shared while every distinct bounded read is retained exactly once.
- A real dependency cycle and an invalid `ReadConst` flat slot, proving both still fail closed and
  cannot be hidden by completed-node memoization.
- Two branches reaching different bounded reads below a shared parent, proving memoization does not
  discard either dependency or merge unrelated selector/count groups.
- Native Windows audit of captured `compute_7ceb0f3417f926f9_7847dd7220f76d66.json` under a strict
  timeout, followed by a bounded game run beyond that compute shader.

Current evidence: completed-node memoization is shared across every DWORD of one descriptor while
the recursion-stack set still rejects cycles. The native Windows audit now reaches the next genuine
semantic rejection in 114 ms; the same shader previously remained inside `Collect` for more than
75 seconds before it was stopped. The next rejection is an unevaluable `ReadFirstLane` image
descriptor word and is tracked separately from this traversal-performance defect.

## Wave-uniform image descriptor candidate selection

Status: production fix, native build, exact captured-shader audit and runtime game validation
complete; automated regression deferred.

Observed trigger: compute shader `7ceb0f3417f926f9` builds eight correlated image-descriptor
DWORDs through matching `Phi` and `SelectU32` graphs, broadcasts every selected DWORD with
`ReadFirstLane`, and samples the resulting wave-uniform descriptor at guest PC `0x0000066c`.
The correlated graph has four host-readable descriptor candidates plus a null candidate, but a
single descriptor source cannot currently retain and materialize that finite candidate set.

Required tests:

- Eight `ReadFirstLane` roots with one active mask and isomorphic `Phi`/`SelectU32` topology,
  proving correlated `ReadConst` leaves become complete descriptor candidates rather than a
  Cartesian product of independent DWORDs.
- Mixed immediate and flat-SRT candidate DWORDs, repeated candidates, and the all-zero null
  candidate, proving materialization preserves exact descriptor tuples and deduplicates safely.
- Mismatched active masks, control topology, Phi predecessors, descriptor widths, invalid flat
  slots, cyclic tuples, and more candidates than the dense image limit, proving recognition fails
  closed.
- Two different descriptors with the same selected key DWORD, proving specialization rejects the
  ambiguous mapping instead of silently binding the wrong image.
- Native Windows audit of captured `compute_7ceb0f3417f926f9_7847dd7220f76d66.json`, followed by a
  bounded game run past guest PC `0x0000066c` with Vulkan validation enabled.

Current evidence: the shared recognizer accepts only eight `ReadFirstLane` roots with the same
active mask and matching `Phi`/`SelectU32` topology. It extracted four host-readable descriptor
tuples plus the null tuple from the captured shader. Both native Windows targets build, and the
exact shader now passes resource tracking and compute-execution precheck in about 0.27 seconds.
The bounded game run materialized all five candidates, emitted 103,404 SPIR-V words, compiled the
shader as number 159 and advanced through six more shaders before the separate GPU draw fault.

## GPU command completion fault isolation

Status: production diagnostics and native game validation complete; dispatch-filter and draw
eligibility regression added; end-to-end GPU fault-isolation coverage remains deferred.

Observed trigger: after `7ceb0f3417f926f9` successfully materializes, emits 103,404 SPIR-V
words, and compiles as shader 159, the game compiles six more shaders and the NVIDIA driver emits
Windows System event `nvlddmkm` 153 for `\\Device\\Video3`. The emulator then receives
`UINT64_MAX` from `vkGetSemaphoreCounterValue` at scheduler tick 16720 and exits with code 321.
That sentinel follows a real GPU driver fault; the semaphore invariant correctly prevents it from
marking every pending resource free.

Current evidence: a filtered synchronized run proved guest compute address `0x8000383f00` with
grid `128x128x1` completes in 321,278 us. The same run later reproduced `nvlddmkm` 153 and the
invalid timeline sentinel at tick 16718, so this dispatch is not the command that hangs the GPU.
Synchronizing all compute and draw calls then isolated the reset to an auto draw with four
vertices and one instance: pixel shader address `0x80003cbb00`, export/vertex shader address
`0x8000408e00`, submit 636. Its preceding `65x65x1` compute dispatch completed in 7,824 us;
the draw was recorded, but its completion wait produced the driver reset.

Required tests:

- Synchronized dispatch diagnostics around every non-empty guest dispatch, with strict optional
  minimum-workgroup and exact-grid filters. Valid filters must isolate matching commands without
  changing their execution; malformed filters must disable the diagnostic instead of silently
  synchronizing every dispatch. The trace must prove the exact shader address, grid, local size,
  and submit ID whose completion triggers the driver fault.
- Synchronized indexed and auto-draw diagnostics with vertex/pixel shader addresses, primitive
  counts, instance counts and submit IDs. Before/after completion markers must distinguish a
  failing draw from queued work that only happens to be observed at the next compute wait.
- Focused `--gpu-sync-diagnostic-only` coverage must keep non-empty draws traceable when either
  dispatch-only filter is set, while retaining minimum/exact matching for dispatches and rejecting
  empty draws or invalid configurations. This regression is now GREEN on the native Windows test
  executable.
- A large finite dispatch split into base-workgroup tiles, proving every guest workgroup executes
  exactly once and `WorkgroupId`/`GlobalInvocationId` retain the unsplit values.
- Storage/image writes shared across tiles, proving submission boundaries preserve the original
  dispatch's read/write ordering and descriptor lifetime.
- Zero dimensions, thread-dimension mode, partitioned wave64 grids, and device workgroup limits,
  proving tiling neither invents work nor exceeds Vulkan limits.
- Native Windows game run with validation plus a System-event check, proving the target dispatch
  completes without `nvlddmkm` 153 and execution advances beyond the current shader frontier.

## Graphics wave64 on native subgroup32

Status: production fix, native build, SPIR-V validation and game validation complete; automated
regression deferred.

Observed trigger: a wave64 pixel shader can be emitted for a host whose graphics stages execute
with native subgroup size 32. The captured shader `964747887d898821` contains subgroup shuffles
to guest lanes 31 and 63, and the lane-63 value controls a structured loop exit. Passing 63
directly to `OpGroupNonUniformShuffle` is outside the native subgroup and leaves the result
undefined. The corresponding four-vertex auto draw returns from recording but resets the NVIDIA
driver while the renderer waits for completion.

Exact cross-half exchange cannot be reconstructed with workgroup scratch and barriers in a
fragment stage. The safe graphics fallback therefore has to define a coherent partitioned
wave64 model: each native subgroup32 executes one guest half, guest lane selectors address the
corresponding local lane, and native ballot bits are reflected into both 32-bit guest words. This
keeps subgroup operations finite and deterministic while retaining an explicit correctness debt
for values that truly differ between the two guest halves.

Required tests:

- Pixel and vertex wave64 compilation with a native subgroup32 profile, proving every constant
  and dynamic `ReadLane`/DPP shuffle index supplied to SPIR-V is within 0...31.
- Ballot, `EXEC`/`VCC`, WQM, find-first and lane-active cases proving both guest mask words use the
  same native-half predicate and all derived bit tests remain internally consistent.
- A bounded graphics loop whose exit reads guest lanes 31 and 63, proving the emitted module
  validates and terminates on a native subgroup32 device instead of causing a watchdog reset.
- A partitioned pixel loop that stops normally on iteration 255 and one that never makes
  progress, proving the safety counter preserves the valid exit and terminates the latter
  fragment after at most 256 body iterations. Multiple and nested loops must share a documented
  per-invocation budget without creating invalid SPIR-V edges or broken Phi dominance.
- Wave32, native subgroup64 and compute cooperative-wave64 cases proving the partitioned graphics
  fallback does not alter their SPIR-V or execution plan.
- Cross-half values that intentionally differ, documenting the approximation and proving the
  emulator rejects or selects a future exact implementation before claiming pixel correctness.
- Native Windows replay of the captured pixel/vertex pair followed by a game run, draw completion
  markers and a System-event query proving the failing auto draw completes without a new
  `nvlddmkm` 153 event.

Current evidence: the emitted pixel module for `964747887d898821` passes `spirv-val` with the
loop counter inserted at the direct loop-body entry after Phi nodes. In native run
`yotei-integrated-20260907-073053-841d50`, the formerly failing four-vertex auto draw produced
`after-complete`; later draw and dispatch commands completed, no new `nvlddmkm` event appeared,
and execution advanced from shader 164 through shader 171. The next failure is an independent
CPU decoder rejection of GFX10 `MUBUF opcode 0x20` in compute shader `d7a83911714a58ee`.
This evidence proves the watchdog failure is cleared for the captured path; it does not prove
exact guest cross-half values or non-black pixel output.

## GFX10 byte-to-D16 buffer loads

Status: production fix, native build, exact shader audit and bounded game validation complete;
automated regression deferred.

Observed trigger: compute shader `d7a83911714a58ee` reaches a GFX10 MUBUF instruction with raw
words `[0xe080e000 0x80040003]` at guest PC `0x60`. The current decoder rejects opcode `0x20`.
Independent GFX1030 disassembly identifies it as `buffer_load_ubyte_d16`; the adjacent opcodes
`0x21...0x23` are the high-half unsigned form and the low/high signed-byte forms.

Required tests:

- Decode exact GFX10 MUBUF opcodes `0x20...0x23` to unsigned/signed byte-to-D16 low/high
  operations with one destination DWORD, eight memory bits, and the correct destination-half
  selector. Existing non-D16 byte and short opcodes must retain their current metadata.
- Unsigned byte values `0x00`, `0x7f`, `0x80`, and `0xff`, proving zero extension to the selected
  16-bit half while the other half of VDATA is preserved exactly.
- Signed byte values at the same boundaries, proving sign extension to 16 bits before insertion
  and preservation of the unselected half for both low and high forms.
- Indexed, offset, scalar-offset, combined-address and out-of-bounds reads, proving the new forms
  reuse the shared buffer address and bounds path and replace only their selected half.
- Decode rejection around reserved neighboring encodings plus replay of the exact captured
  shader, proving support is based on the ISA opcode family rather than a title or shader hash.
- Native Windows game run proving `d7a83911714a58ee` advances beyond CFG, translation, SPIR-V
  validation and pipeline creation without regressing the preceding graphics-wave64 draw.

Current evidence: native `shader_cfg_tests --audit-shader` decodes all 55 instructions and
passes the exact capture through CFG, IR translation and resource tracking. In game run
`yotei-integrated-20260907-075324-3c906f`, `d7a83911714a58ee` emitted 6,537 SPIR-V words,
compiled as shader 172, and its 76x1x1 dispatch completed in 63,618 us. Execution then compiled
two more shaders and stopped independently in resource tracking for `6cc64dee32dc7094`.

## Entry-prefix bounded SRT tables with dispatcher CFG fallback

Status: production proof fix implemented and validated through the next captured/runtime
resource-tracking boundary; automated regression and full materialization validation deferred.

Observed trigger: compute shader `6cc64dee32dc7094` uses a five-bit unsigned extraction to form
the byte offset `0x1030 + selector * 16`, then reads four descriptor DWORDs from the root SRT in
an unconditional entry prefix that IR represents as several basic blocks. Its CFG requires the
general dispatcher emitter because two headers share a structured merge. The bounded SRT proof
previously rejected every dispatcher program before considering that this finite table read
occurs before any control-flow split, so resource tracking later rejected descriptor DWORD 0
with root `LoadAddressU32` at PC `0x530`.

Required tests:

- A dispatcher-fallback compute program whose single-entry, unconditional block chain contains a
  five-bit selector and four correlated SRT reads at stride 16, proving exactly 32 coherent
  descriptor candidates are materialized while the live selector remains on the GPU.
- Entry-prefix ordering cases proving the selector definition and all raw descriptor reads follow
  only unconditional edges with one predecessor and precede the consuming handle; a read after a
  control-flow split, in a conditional block, or with a non-dominating address root must remain
  rejected.
- Phi, loop-carried, workgroup-indexed and non-finite selectors in a dispatcher program, proving
  the narrow admission rule cannot inherit structured-loop or cross-block assumptions.
- Address overflow, width 0/32, mismatched columns, non-consecutive offsets, unreadable memory,
  snapshot retry, candidate and byte-budget boundaries, all failing closed without partial
  descriptor publication.
- A structured version of the same table and unrelated dispatcher shaders, proving the existing
  bounded proofs and generic switch emitter retain their current behavior.
- Exact captured-shader audit followed by native game validation. The current implementation
  removes the PC `0x530` buffer-descriptor failure in both and reaches the independent image
  descriptor at PC `0x7c4`; resource tracking, materialization, SPIR-V validation and the
  4096x1x1 dispatch remain pending until that next blocker is fixed.

## Periodic Vulkan pipeline-cache checkpoints

Status: production fix and two-run native game validation complete; automated regression
deferred.

Observed trigger: bounded emulator runs often end by externally terminating the process
after the window does not close within its grace period. The Vulkan driver cache was only
serialized during normal renderer destruction, so every forced stop discarded all pipelines
compiled since startup and made the next run repeat the same expensive work.

Required tests:

- A cache-lifecycle case that creates enough new graphics and compute pipelines to cross the
  checkpoint threshold, then verifies that a complete signature, payload hash and driver blob
  are written without destroying the live `VkPipelineCache`.
- A continuation case that creates more pipelines after a checkpoint and proves the same live
  cache handle remains usable until final shutdown.
- Threshold boundary cases for zero, one less than the interval, the exact interval and several
  intervals, checking that cache hits do not count as newly created pipelines.
- A warm-cache case where newly created emulator pipeline objects leave the driver blob unchanged,
  checking that hash comparison suppresses redundant replacement of the on-disk cache.
- Disabled, empty and failed-driver-query cases proving that checkpoint attempts remain harmless
  and do not prevent later pipeline creation or final cleanup.
- File replacement and interrupted-write cases proving that a fully written temporary file is
  used and a truncated or mismatched cache cannot be accepted on the next launch.
- Concurrent graphics/compute creation coverage proving that serialization covers cache mutation
  and snapshot extraction, followed by two native Windows runs where the first is force-stopped
  and the second reports a compatible loaded cache.

## Bounded shader diagnostic dumps

Status: production fix and native game validation complete; automated regression deferred.

Observed trigger: file diagnostics serialize decoded ISA and native IR more than once for every
new shader. Large dispatcher programs can expand to tens of thousands or millions of IR
instructions; one bounded game run produced a 254 MiB log with 10,468,972 lines and reached its
timeout while formatting an early IR dump, before the next compiler phase could be observed.

Required tests:

- Small structured and dispatcher shaders below the instruction budget, checking that decoded ISA,
  pre-resource IR and final IR remain present and unchanged in diagnostic output.
- Exact-boundary and over-budget shaders, checking that expensive IR string construction is skipped
  and replaced by a compact message containing the actual instruction count and configured limit.
- A large dispatcher shader proving that normalization, resource tracking, specialization and
  SPIR-V emission still execute when verbose IR text is omitted.
- Phase-marker coverage around normalization/resource tracking so a timeout can be assigned to a
  compiler phase even when the detailed dump is suppressed.
- A silent guest-log run with `KYTY_SHADER_PHASE_TRACE=1`, checking that concise decode, CFG,
  normalization, resource-tracking and SPIR-V markers reach stdout without enabling the shared
  high-volume logger.
- Repeated permutations and mixed small/large shaders proving that each detailed dump applies the
  same bound and that concise phase tracing remains available for every program.

## Depth attachment rediscovery after resource preparation

Status: production fix and native game validation complete; automated regression deferred.

Observed trigger: draw setup discovers color/depth attachments before shader resources, vertex
buffers and descriptor bindings are prepared. That preparation can synchronously finish work and
replace an aliased cached image. Color attachments already rediscover and rebind their descriptor
at final acquisition, while an equally stale depth attachment terminated the emulator with
`depth target changed after render-state discovery`.

Required tests:

- A draw where sampled or storage resource preparation invalidates the initially discovered depth
  image, checking that final attachment acquisition finds and binds a replacement from the original
  depth descriptor.
- Registered, missing, unregistered and `needs_rebind` owner cases, including replacement with a
  different `ImageId` and reuse of a compatible existing depth image.
- Depth-only and mixed color/depth draws proving that extent, layer count, sample count, format,
  image view, pipeline rendering state and dynamic depth/stencil state use the final image.
- HTile and stencil cases proving rediscovery preserves metadata ownership, deferred clears,
  stencil association and the exact guest backing ranges.
- Negative cases for an actually incompatible replacement, invalid view or mixed sample count,
  checking that the existing precise validation remains after rediscovery.
- Native Vulkan validation with an alias transition between discovery and acquisition, followed by
  a bounded game run past the captured renderer invariant.

## Unsigned bitfield selectors for bounded resource tables

Status: production fix and corpus validation in progress; automated regression deferred.

Observed trigger: a four-word buffer descriptor is loaded from an immutable table using
an index produced by `BitFieldUExtract`. The source of the extracted bits can be
lane-varying, including a buffer lookup indexed by `WorkgroupId`, while an immediate
unsigned extraction still gives an exact dense range of `0..(2^width - 1)`.

Required tests:

- A compiler-level proof test where a 5-bit extraction drives four correlated descriptor
  reads with stride 16 and produces exactly 32 candidates.
- A materialization test that reads all correlated descriptor words coherently, deduplicates
  identical candidates, preserves the live GPU selector, and maps each key to the right buffer.
- Boundary cases for width 0, width 32, invalid `offset + width`, non-immediate offset/width,
  signed extraction, mismatched descriptor columns, and the existing resource/probe limits.
- A native Windows shader audit covering every captured manifest in the same normalized
  failure group, followed by a bounded Vulkan readback case before upstream submission.

## Uniform diamond Phi descriptor selection

Status: production fix and corpus validation in progress; automated regression deferred.

Observed trigger: four buffer descriptor words are Phi nodes at the merge of a direct
two-arm diamond. Each arm supplies host-evaluable SRT words, and the split condition is
also host-evaluable. The compact runtime resource plan does not retain the shader CFG,
so the selection must be lowered to typed `SelectU32` values before CFG disposal.

Required tests:

- A positive compiler-level case for both true/false edge orderings and four correlated
  descriptor words, checking that runtime materialization selects the expected descriptor.
- Rejection cases for divergent or undefined conditions, non-diamond control flow,
  loop-carried Phi nodes, mismatched Phi blocks, conditional raw reads, non-U32 values,
  and arms that are not valid runtime descriptor values.
- Neighboring checks showing invariant Phi handling and bounded descriptor tables remain
  unchanged, plus native Windows audits for every captured shader in this failure group.

## ReadLane-indexed descriptor tables behind scalar buffers

Status: production fix and corpus validation in progress; automated regression deferred.

Observed trigger: four correlated descriptor words are read through `ReadConstBuffer`.
Their record offset is selected with `ReadLane`; both the selected value and lane are
provably finite, but the runtime resource planner previously supported bounded tables
only when their backing source was a two-word raw address.

Required tests:

- A positive compiler case where a finite `ReadLane` key indexes four correlated
  `ReadConstBuffer` words and preserves the live GPU key in the specialized table.
- Materialization cases for zero and nonzero buffer stride, exact final-word access,
  base/offset overflow, negative immediate offsets, and coherent snapshot failures.
- Rejection cases for an unbounded source value, an out-of-wave lane selector,
  undefined lanes, malformed source descriptors, mixed table columns, and exceeded
  probe or resource limits.
- Equivalence checks against raw scalar-buffer reads and `ReadFirstLane`, followed by
  native Windows audits and a bounded Vulkan readback before upstream submission.

## Acyclic WorkgroupId coefficient reads stay on the GPU

Status: production fix validated in a bounded game run; automated regression deferred.

Observed trigger: a small acyclic compute shader reads one immutable coefficient per
`WorkgroupId` through a raw address. Eager CPU snapshotting fails when the source was
written by the GPU and is therefore unavailable to the coherent specialization reader,
although the existing BDA emitter can execute the same read directly on the GPU.

Required tests:

- An acyclic compute case where `WorkgroupId` indexes a raw scalar payload and resource
  tracking preserves `LoadAddressU32` instead of creating `ReadBoundedSrtU32`.
- GPU readback for the live BDA path across several workgroups, including wave64 split
  on a native subgroup-32 device.
- A neighboring cyclic/cooperative case proving that bounded immutable coefficient
  snapshots remain enabled where cross-wave scheduling requires them.
- Runtime rejection coverage for dirty specialization memory without converting an
  otherwise executable acyclic shader into a fatal materialization failure.

## Nested selection with an externally entered linear tail

Status: production fix and native shader validation in progress; automated regression
deferred.

Observed trigger: one arm of a nested conditional joins a straight-line tail that is also
entered by a sibling arm of the enclosing conditional. Global post-dominance assigns both
headers the same merge block. Giving the nested header an earlier merge allows a branch to
exit its selection illegally; retaining the shared merge sends the reducible shader through
the dispatcher fallback. The safe transformation clones the shared linear tail for the
nested path and gives that path a dedicated synthetic merge.

Required tests:

- A compiler case where either inner arm reaches a linear tail shared with a sibling arm of
  the enclosing selection, checking the cloned instruction range and dedicated inner merge.
- Nested and sequential variants proving that each `OpSelectionMerge` owns a distinct
  merge block and the result contains no dispatcher `OpSwitch`.
- Phi and register-state cases proving that mutually exclusive clones preserve edge values
  and execute the shared guest instructions exactly once on each original path.
- A positive case where the acyclic shared tail lies inside an enclosing loop, plus rejection
  cases for conditional or cyclic shared regions, multiple exits, and irreducible graphs;
  these require full semantic region cloning.
- Complexity-bound cases showing that large CFGs and graphs exceeding the small semantic-clone
  budget fall back promptly instead of repeatedly expanding the graph.
- Native Windows audit of captured shader `e52e19c6923301d0`, SPIR-V validation, and a
  bounded game run comparing compile time and generated word count with the dispatcher
  fallback baseline.

## Cooperative wave64 values live across scheduler phases

Status: implementation and native game validation complete; automated regression deferred.

Observed trigger: cooperative wave64 execution divides a guest CFG into software-scheduled
phases around LDS operations, subgroup collectives, guest barriers, and control-flow edges.
The previous backend assigned Function-storage slots to every typed runtime instruction, so
values used only inside one phase are repeatedly stored and loaded. In the captured
`916ea8893e5b276a` shader this produces roughly 11,561 `OpLoad` and 3,408 `OpStore`
instructions after optimization and makes a 120x68 dispatch take about 6.1 seconds.

The phase-liveness implementation reduced the optimized module from 170,999 to 146,134 words,
`OpLoad` from 11,561 to 8,993, and `OpStore` from 3,408 to 1,410. Vulkan and SPIR-V validation
passed in the native capture, but the measured dispatch remained about 6.05 seconds. The next
performance work therefore belongs to the cooperative scheduler/control-flow layer; the
regressions below still need to lock in this code-size improvement and its semantics.

Required tests:

- A compiler case with several ordinary arithmetic values used only within one cooperative
  phase, checking that they remain SSA values and receive no Function-storage spill slots.
- Cross-phase cases for guest barriers, LDS phases, subgroup collectives, CFG edges, branch
  conditions, and loop-carried Phi values, checking that every surviving value is spilled
  and reloaded before use.
- An immediate conditional-branch predicate, checking that spill analysis accepts constants
  without treating them as instruction-backed values.
- A mixed-use case where one definition has both same-phase and later-phase consumers,
  checking direct SSA use in its defining phase and a spill load in later phases.
- `ReadConstBuffer` broadcast and inactive-wave cases proving that stale private values cannot
  affect collective control flow or architectural results.
- SPIR-V validation plus GPU readback for multiwave LDS/SSBO ordering, early wave completion,
  differing loop counts, and same-address writes on a native subgroup-32 device.
- Native Windows capture of `916ea8893e5b276a`, comparing generated instruction counts and
  steady dispatch time while preserving the full output and guard readback.

## Depth comparison on non-depth color images

Status: upstream implementation selected; automated regression deferred.

Observed trigger: after reaching frame 93, a sampled color image with format 56 requested a
depth comparison. RDNA2 permits comparison sampling for this descriptor use, while Vulkan
`OpImageSampleDref*` requires a host format with native depth-comparison support. Treating the
color image as a Vulkan depth image fails specialization before the next frame.

Required tests:

- Resource-specialization cases for native depth formats and non-depth UNORM/sRGB/compressed
  formats, checking that only unsupported host formats select manual comparison.
- Single-sample, gather, offset, projected and bindless paths covering all eight comparison
  functions and the guest-defined `Dref op Dtexel` operand order.
- Sampler-splitting cases proving comparison state survives point/linear sampler cloning.
- Swizzle cases proving manual comparison reads the descriptor-selected source component.
- sRGB color formats checked through an equivalent UNORM view so depth-like values are not
  changed by hardware gamma decoding.
- Texture upload/view aspect tests for native depth images and ordinary color images.
- Cache-alias coverage where a single-sample `D32_SFLOAT_S8_UINT` depth target is later
  sampled through an equal-address, equal-footprint `R16G16_SFLOAT` color descriptor. The
  cache must replace the incompatible native image, preserve the raw 32-bit depth texels via
  the buffer-copy path, and create a color-aspect view without Vulkan validation errors.
- Neighboring depth-to-color alias cases for compatible byte widths, plus rejection or a
  diagnostic for unequal byte widths and multisample representations that cannot be copied.
- Native Vulkan readback comparing manual and hardware comparison where both are available,
  followed by a Yotei run beyond the frame-93 specialization failure.

## Acyclic buffer atomic returns in one split wave64

Status: production fix and native game validation complete; automated regression deferred.

Observed trigger: an acyclic compute shader with exactly one guest wave64 uses the old value
returned by a predicated `BufferAtomicIAdd32` as the element index for several output stores.
The native subgroup-32 backend keeps all 64 guest lanes in one host workgroup and emits the
buffer atomic once per active lane, but the execution planner rejected every live buffer
atomic result before SPIR-V emission.

Required tests:

- A planner case with one complete guest wave64 and an acyclic live buffer atomic result,
  checking that native subgroup-32 splitting retains a single 64-invocation host workgroup.
- SPIR-V and Vulkan readback cases where active lanes reserve unique output indices through
  `BufferAtomicIAdd32` and use the returned old values for scalar and vector buffer stores.
- Predicated and sparse-EXEC cases proving inactive lanes neither update the counter nor
  write output, including bounds-failure fallback values from the existing atomic emitter.
- Neighboring cases for the supported integer buffer atomic family, aliased counters,
  multiple atomics in one straight-line shader, and atomic results consumed by wave
  collectives without changing their lane ownership.
- Rejection cases for a live atomic result controlling a divergent branch, cyclic atomic
  feedback, cooperative scheduling, and partitioned multiwave workgroups until
  their separate ordering and publication contracts have executable coverage.
  Acyclic single-wave LDS and GDS integer atomic returns are covered by
  `--single-wave64-lds-atomic-return-only`,
  `--single-wave64-gds-atomic-admission-only`, and
  `--gds-atomic-add-return-wave64-only`.
- Native Windows audit and SPIR-V validation of the captured shader class, followed by a
  bounded game run beyond the original shader-admission failure.

## Bounded image descriptors in constant post-test loops

Status: production fix, native build, offline audit and bounded game validation complete;
automated regression deferred.

Observed trigger: a compute shader walks an eight-dword image descriptor table with a
loop-carried index initialized to zero. The descriptor is consumed before the latch increments
the index and repeats while the incremented signed value is less than the positive constant
bound. The bounded-read planner recognized only guards reached before the table read, and the
specialization path could turn bounded descriptor columns into buffer resources only.

Required tests:

- Planner cases for canonical post-test loops with `i = 0`, `next = i + 1`, and a backedge
  guarded by signed or unsigned `next < positive_constant`, checking the exact dense count.
- Rejection cases for zero, negative, overflowing, runtime, non-unit, decrementing, multiple
  latch and non-dominating update forms, plus a read that does not dominate the latch.
- An eight-column image table with consecutive dword offsets and one shared bounded selector,
  checking null/invalid descriptor normalization, candidate deduplication and dense key mapping.
- Mismatched count/address sources, strides, offsets, selectors, column widths and resource
  reuse must fail closed instead of combining unrelated descriptor words.
- SPIR-V and Vulkan readback cases for storage-image reads and writes through several table
  entries on native subgroup-32 wave64 execution, including repeated and all-null entries.
- Native Windows audit of the captured shader class and a bounded game run beyond its original
  `GetImageResource dword 0 is not a valid runtime value` failure.

## Scalar-buffer image tables with shifted wave-uniform selectors

Status: production fix and bounded game validation reached the next independent buffer boundary;
automated regression deferred.

Observed trigger: an eight-DWORD image descriptor was read from one scalar-buffer table with
the same `ReadFirstLane` selector multiplied by 32 using `ShiftLeftLogical32`. Resource
tracking recognized equivalent `selector * stride` and planned the existing inline image-table
materialization only for sampled images, so an `ImageRead` reached the generic runtime-value
validator with `ReadConstBuffer` roots.

Required tests:

- Four- and eight-DWORD scalar-buffer descriptors using `selector * stride` and
  `selector << shift`, proving equivalent keys produce the same inline table and GPU selector
  mapping.
- `ImageRead`, `ImageSampleRaw`, image-query and storage-image variants, including compact
  R128 descriptors, null/OOB table entries, repeated descriptors and the existing probe/dense
  image limits.
- Dominating selector limits, unknown wrapped-U32 selectors and scalar-buffer strides of zero,
  one, 16, 32 and non-power-of-two values, checking that valid in-range words are enumerated
  without inventing host descriptors.
- Writable/dirty table backing, unavailable specialization memory, alias overlap and
  transactionality cases, proving inline image materialization fails closed rather than
  snapshotting a GPU-written descriptor table.
- Exact `6cc64dee32dc7094` audit plus native game validation through guest PC `0x4d64`; the
  current run passes that image failure and stops next at a separate
  `GetBufferResource` scalar-buffer descriptor at PC `0x656c`.

## Guarded inline scalar-buffer descriptor tables

Status: synthetic RED/GREEN for the constant guarded-selector shape; exact Yōtei applicability
was disproved by a later captured variant.

Observed trigger: compute shader `6cc64dee32dc7094` reads four correlated descriptor DWORDs
with `S_BUFFER_LOAD_DWORDX4`, using one loop-carried `ReadFirstLane(Phi)` selector multiplied
by the 16-byte descriptor stride. A dominating unsigned guard bounds the selector, but resource
tracking previously supported this inline scalar-buffer pattern only for image and sampler
handles. The unchanged regression failed at guest PC `0x656c` with
`GetBufferResource dword 0 is not a valid runtime value (root=ReadConstBuffer)`.

The shared buffer path now reuses inline-descriptor recognition only when a dominating CFG guard
proves an exact nonzero selector limit. Coherent specialization reads all four descriptor words
for each candidate, produces one logical buffer table and leaves the live selector on the GPU.
Unguarded selectors and mismatched descriptor columns retain the prior closed failure. RED is
`_Build/logs/inline-buffer-table-red-20260908-v2.txt`; the unchanged selector and its two
negative boundaries are GREEN in `_Build/logs/inline-buffer-table-green-20260908-v2.txt`.

Validation correction:

- `_Build/runs/yotei-integrated-20260908-131508-5d7cde` reached frame 179 but did not compile the
  problematic `6cc64dee32dc7094` variant, so it cannot validate PC `0x656c`.
- `_Build/runs/yotei-integrated-20260908-132617-aaa93b` later reproduced PC `0x656c`; the exact
  variant uses a signed runtime Phi loop, stride 196 and an additional mask guard rather than the
  constant guarded-selector shape. This mechanism remains neighboring coverage, not the game fix.
- Add stride overflow/wrap, zero/OOB descriptors, unreadable/GPU-dirty backing, duplicate
  candidates, write-alias and 128-resource limit cases.
- Repair the pre-existing full `resource_tracking_tests` failure in the invariant indirect-image
  wrapped-immediate boundary; it occurs before the inline-table cases and is not counted GREEN.

## Dispatcher signed scalar-buffer descriptor loops

Status: synthetic RED/GREEN, exact captured-manifest audit and bounded native game retry pass;
the first nonzero frame remains pending.

Observed trigger: dispatcher compute shader `6cc64dee32dc7094` reads four correlated descriptor
DWORDs through an induction Phi starting at zero, unit increment, signed runtime count, an extra
bit-mask guard and 196-byte table rows. The existing `BoundedReadProof` already defined strict
signed-loop semantics, but the dispatcher path built only block indices and rejected the program
before canonical Phi proof.

The shared correction always builds the dispatcher graph and admits it to the same proof only
after the full CFG validates. RED `_Build/logs/dispatcher-signed-buffer-loop-red-20260908.txt`
failed at `GetBufferResource dword 0`; unchanged GREEN
`_Build/logs/dispatcher-signed-buffer-loop-green-20260908.txt` covers the positive two-row table,
zero and negative counts, bypassed count guard and non-unit increment. Exact audit
`_Build/logs/6cc64dee-dispatcher-signed-green-20260908.stdout.txt` completes resource tracking.

Bounded GPUAV run `_Build/runs/yotei-integrated-20260908-140320-cf909d` reaches frame 139, 124
shown frames and passes PC `0x656c`. The next fatal is `indirect image table at pc 0x000007c4 has
incompatible candidates`; it is a separate heterogeneous-image resource class and the next RED
target. No source readback was recorded in that quiet run.

The dimension-only heterogeneous-image subset is now covered by unchanged RED/GREEN selectors:
`_Build/logs/heterogeneous-indirect-images-red-20260908.txt`,
`_Build/logs/heterogeneous-indirect-images-green-20260908.txt` and
`_Build/logs/heterogeneous-indirect-images-spirv-green-20260908.txt`. Materialization accepts
sampled non-comparison candidates whose only differing layout property is 1D versus 2D and the
sample/read emitter derives coordinates per candidate. Numeric-class mismatch still rejects
transactionally; query/gather dimensions remain fail-closed.

Commit `22242aa` bounded run `_Build/runs/yotei-integrated-20260908-150356-e59dec` reaches frame
132 / 119 shown and passes PC `0x7c4`. The next fatal is PC `0x78b8`, where candidates differ in
both dimension (`3/1`) and shader swizzle (`0x24c/0`). The next RED must prove candidate-specific
swizzle semantics rather than weakening compatibility globally. Source frames 110–119 remain
RGB zero with alpha 3.

Remaining validation:

- Add signed-overflow/`INT32_MAX`, cyclic bound provenance, multiple latches and dirty-memory
  transactional boundaries without weakening the current fail-closed proof.
- Reproduce the exact PC `0x78b8` dimension-plus-swizzle candidates and extend only sample/read
  operations that can apply the swizzle per switch candidate; keep query/gather and incompatible
  numeric/conversion/comparison cases rejected.

## Signed integer storage images

Status: production fix, native build and bounded game validation complete; automated regression deferred.

Observed trigger: a compute shader writes one component through a regular eight-dword 2D image
descriptor whose guest format is `k16SInt`. Resource specialization rejected every signed
integer storage image even though the descriptor maps directly to a Vulkan signed integer
format and the shader does not use image atomics.

Required tests:

- Binding ABI coverage for signed storage images in each supported dimension, confirming that
  sampled signed, storage signed, storage unsigned and atomic unsigned arrays remain distinct.
- SPIR-V validation and Vulkan readback for `R8Sint`, `R16Sint`, `R8G8Sint`, `R16G16Sint`,
  `R8G8B8A8Sint`, `R16G16B16A16Sint`, `R32Sint`, and the wider signed 32-bit formats supported
  by the renderer.
- Store conversion cases for negative, zero and positive 32-bit shader values, including
  narrowing/clamping behavior of 8-bit and 16-bit signed image formats and component masks.
- Signed storage reads and read/write resources must return the original signed component bits
  to guest U32 registers; signed image atomics must remain rejected until their Vulkan contract
  is implemented explicitly.
- Mixed shaders using signed and unsigned storage images of the same dimension must allocate
  separate descriptor bindings and pass descriptor-budget checks for compute and pixel stages.
- Native Windows audit of `cs_0001dee4` and a bounded game run beyond shader
  `753c552fae650ec4` and its `storage image descriptor ... unsupported format 12` failure.

## GFX10 packed D16 formatted buffer loads

Status: production fix, native build, exact shader audit and bounded game validation complete;
automated regression deferred.

Observed trigger: compute shader `da7e70d9fcafe48c` repeatedly uses GFX10 MUBUF opcode
`0x83` (`BUFFER_LOAD_FORMAT_D16_XYZW`). The current decoder rejects the instruction before
CFG construction. GFX10 also defines the related `0x80` through `0x82` load forms, whose one
to four converted 16-bit components occupy one or two packed VGPRs.

Required tests:

- Decoder coverage for opcodes `0x80` through `0x83`, including exact destination DWORD and
  logical component counts, formatted metadata, addressing flags and raw diagnostic output.
- Vulkan readback for float, UNORM, SNORM, scaled, UINT and SINT descriptor formats, proving
  float-class results become IEEE half values while integer-class results retain their low
  sixteen result bits.
- Packing cases for X, XY, XYZ and XYZW, including preservation of the unused high half of
  the active destination VGPR for X and XYZ.
- EXEC-disabled and split-wave64 cases proving all destination bits remain unchanged for
  inactive guest lanes, including when VDATA overlaps VADDR.
- Descriptor swizzle and out-of-bounds cases covering memory, zero and one selectors and a
  format whose source record is wider than the packed destination.
- Native Windows audit of the captured shader class and a bounded game run beyond shader
  `da7e70d9fcafe48c` and its `unsupported family=MUBUF opcode=0x83` failure.

## Loop-indexed scalar-buffer descriptor tables

Status: production fix, native build, exact shader audit and bounded game validation complete;
automated regression deferred.

Observed trigger: a compute shader reads four correlated descriptor DWORDs from a scalar
buffer table. A loop-carried induction Phi starts at zero, advances by one, and contributes
`index * 196` to every table column. The loop is guarded by a signed comparison against a
runtime-uniform positive bound. Existing bounded-read proofs cover constant post-test bounds
and unsigned runtime pre-test bounds, so resource tracking leaves the descriptor words as
ordinary `ReadConstBuffer` values and later rejects them as host-unavailable runtime roots.

Required tests:

- Positive pre-test and post-test loops with `i = 0`, `next = i + 1`, a signed positive
  runtime bound and a 196-byte row stride, checking the exact materialized row count.
- Four correlated descriptor DWORD reads with consecutive memory offsets, checking that they
  share one coherent scalar-buffer snapshot and retain the live loop index for selection.
- Zero and negative signed bounds, signed overflow boundaries, `INT32_MAX`, unknown signs,
  cyclic bound provenance, non-unit updates, decrementing loops and multiple latches must be
  rejected or use explicitly defined zero-iteration/count semantics.
- Conditional or non-dominating scalar-buffer roots, mismatched column addresses, counts,
  strides, indices and offsets must fail closed instead of combining unrelated words.
- Materialization limits, address overflow, dirty-memory retry and aliasing cases must preserve
  transactional snapshots and never expose a partially read descriptor table.
- Native Windows audit and bounded game run beyond `da7e70d9fcafe48c` PC `0x000005d4`,
  followed by SPIR-V/Vulkan validation of the selected buffer resources.

## Misaligned tiled image descriptors

Status: production fix, native build and bounded game validation complete; automated regression
deferred.

Observed trigger: after 127 shaders compile and the loop-indexed descriptor shader creates a
valid Vulkan pipeline, image binding receives a 16x16 `k16_16_16_16Float` storage image using
`kStandard4KB`. Its SRD address is 256-byte aligned but has offset `0x800` within the 4-KB
allocation alignment returned by `TileGetTextureTotalSize`. That alignment describes a
standalone allocation; the descriptor's address describes a placed image view, and the detiler
already evaluates tiled offsets relative to that address while separately satisfying Vulkan
buffer-offset alignment.

Required tests:

- Diagnostic coverage proving every rejected image layout reports the guest address, computed
  size and alignment, format, type, tile mode, dimensions, levels and raw descriptor DWORDs.
- Linear and tiled image descriptors at exact, over-aligned and 256-byte placed-view addresses,
  including mipmapped, array, volume, multisample, sampled and storage resources. Allocation
  helpers must continue reporting the full tile-block alignment.
- Alias/view cases where a descriptor intentionally starts inside an existing backing image,
  checking whether a compatible subresource view is selected instead of creating a new image.
- Rejection cases for zero size/alignment, arithmetic overflow, unsupported tile modes and
  malformed descriptor address encodings. A placed view must retain its exact guest range;
  rounding the address down to the allocation boundary is forbidden.
- Vulkan validation and readback for any implemented rebase/copy path, followed by a bounded
  native game run beyond the current `descriptors.cpp` image-alignment failure.

Validation reached the original `053b2c82226fe5ed` storage write, created its Vulkan pipeline
and continued through four later compute pipelines without an image-layout fatal or Vulkan VUID.
The exact placed address was preserved rather than rounded to a tile boundary.

## Single-wave64 collectives on native subgroup32

Status: production split wave64 ballot is the 64-lane workgroup OR without
`OpGroupNonUniformBallot`; one complete guest wave stays on the split host
workgroup. Guest 32-bit LDS and collective scratch share one Workgroup `u32`
array (LDS prefix, scratch suffix) because SPIR-V Workgroup variables alias
without `WorkgroupMemoryExplicitLayoutKHR`. Sampled `OpImageFetch` no longer
takes ImageMemory control barriers; image writes still rendezvous. The compact
native-subgroup ballot and single-wave cooperative admission experiments were
reverted. Automated Vulkan/readback regression deferred.

Observed trigger: a complete 64-invocation guest wave uses subgroup operations on a
host whose native subgroup is 32. `OpGroupNonUniformBallot` mixed with workgroup
scratch aborts `nvgpucomp64.dll` on RTX 5060 Ti. Restoring the RTX 4060 64-lane
scratch OR lets the first 34 shader variants create pipelines and complete, including
the graphics pair `cc2b833287f423b9`/`2e17013fdae4d8ac`. The same compiler then
dies while compiling Screen Space Shadows `b90e2024732c6111` (`cs=0x8000399500`,
64×1×1, LDS, loops, ballot/image). RTX 4060 compiled that module (48 929 SPIR-V
words) and reached frame 125. Combining the Workgroup arrays (run
`yotei-integrated-20260907-163447-2da1c9`, SHA
`94eeb7449ba940752ea493c1b9efffbd256c38e7333fc7ae660a3220f03f8d1f`) and dropping
ImageMemory-after-fetch (`161953-de0599`) did not move that frontier.

Required tests:

- Native32 planner cases for 64-invocation wave64 collectives with and without
  LDS/barriers, proving one complete guest wave stays on the split 64-invocation host
  workgroup while multi-wave shared storage still selects the cooperative scheduler.
- SPIR-V validation proving split ballots use the 64-lane workgroup reconstruction,
  workgroup barriers and no `OpGroupNonUniformBallot`/`CapabilityGroupNonUniformBallot`.
  The reconstruction must not expand one ballot into a long per-invocation
  `OpLoad`/`OpBitwiseOr` chain: a bounded shared-word reduction may use
  `OpAtomicExchange`/`OpAtomicOr` (or an equally bounded workgroup primitive) and
  must preserve the two guest mask words.
- If the reduction uses the existing per-lane scratch slots, a leader-only
  aggregate write is a valid bounded alternative to per-invocation aggregate
  atomics. The regression must check that each logical wave publishes both
  aggregate words after the per-lane barrier, and must retain the same result
  for full, sparse and repeated predicates.
- Split+LDS modules declare exactly one Workgroup variable; guest LDS bounds stay
  the declared DWORD allocation; scratch AccessChains are `IAdd(lane, lds_dwords)`
  and must not be counted as competing LDS stores. Ballot aggregate words follow
  the complete per-lane scratch suffix with a separate two-word stride per
  cooperative wave; they must not alias `WavePublish` slots. 64-bit LDS atomics
  still emit a separate `u64` array.
- Vulkan readback for full and sparse/inactive EXEC lanes, repeated collectives,
  and looped LDS plus ballot, including a bounded analogue of the `b90e` loop/image
  pattern. Reusing the aggregate slots for a later ballot must not observe the
  previous mask, and cooperative multi-wave bases must remain disjoint.
- A shared `BitwiseOr32` used by multiple `IEqual32`/`INotEqual32` zero tests must
  lower each comparison through the equivalent De Morgan boolean form without
  emitting the integer OR; mixed nonzero consumers must retain the integer result.
- Host profiles with native subgroup64, native subgroup32 and unavailable subgroup-size
  control, proving the admission rule follows device capabilities rather than a title
  or shader hash.
- Native Windows game retry past `b90e2024732c6111` on RTX 50-series, comparing
  frame progress with the RTX 4060 result.

## Native32 wave64 barrier/image pipeline lowering

Status: b90e pipeline correction, native synthetic regression and the RTX 5060 Ti
game pipeline/dispatch path are GREEN; exact standalone probe compatibility and
nonzero surface output remain pending.

Observed trigger: the current `b90e2024732c6111` SPIR-V module passes
`vkCreateShaderModule` and `vkCreatePipelineLayout`, then crashes the NVIDIA compiler
inside `vkCreateComputePipelines` on the RTX 5060 Ti. The same lowering family must
remain valid when the guest wave64 contains LDS reads/writes, repeated barriers,
sampled image fetches, image publication and a 64-lane ballot. The test must not
identify the title, shader hash or guest address.

Required tests:

- A bounded synthetic one-wave64/native-subgroup32 module with LDS, a loop, ballot
  reconstruction, image fetch and image write, proving SPIR-V validation and the
  isolated `vkCreateComputePipelines` probe both complete without a driver fault.
- Lowering variants that remove each operation family independently, followed by a
  mechanically minimized public fixture, proving the failure is attributed to the
  shared wave64/barrier/LDS/image construction rather than module size or unrelated
  shader code.
- Split ballot output must use the defined 64-lane workgroup reconstruction, with
  exactly one aliased Workgroup `u32` allocation for LDS plus scratch where explicit
  Workgroup layout is unavailable; per-lane scratch starts after the LDS prefix and
  ballot aggregate words follow that scratch region without aliasing it.
  A 64-bit LDS atomic path must retain its separate typed storage.
- Collision-free DWORD stores whose byte address is exactly `(LaneId << 2)` or an
  EXEC-select of that address must use plain `OpStore` only for one complete
  64-invocation split host workgroup; arbitrary offsets, competing addresses,
  Function LDS, cooperative mode and non-64 host sizes must retain atomics.
- A split-ballot result consumed as
  `(lower & mask0) | (upper & mask1) != 0` must preserve the independent mask
  semantics while lowering the final zero-test without an integer OR over
  Workgroup-derived values. The regression must check the value-graph semantics
  (for example, De Morgan `!(lower_mask == 0 && upper_mask == 0)`), not only
  opcode counts. This is a narrow zero-test optimization; arbitrary guest
  integer OR operations must retain their exact 32-bit result.
- Sampled image fetches must not acquire `ImageMemory` publication semantics;
  image writes and image atomics must retain the required rendezvous and their
  publication scope must be `Workgroup`, not a device-wide scope invented by the
  lowering.
- Neighboring native subgroup64, wave32, no-LDS, read-only-image, inactive-EXEC and
  multi-wave/cooperative cases must keep their existing admission and SPIR-V
  contracts. Unsupported cyclic or cross-wave cases must still reject closed.
- Run the exact saved module through `vk_spv_probe.exe` before and after each
  candidate, record the exit/output, and only then retry the real 1280x720 game
  path with all owned processes confirmed stopped.

The current isolated minimization of fresh module
`_Build/runs/yotei-integrated-20260907-183157-411634/...b90e2024732c6111.spv`
produced a valid neutral-body module that passes both `spirv-val` and the isolated
probe. Independent family variants showed that removing barriers, atomics or image
operations alone does not clear the crash, while neutralizing loads, access chains or
all integer ORs does. Delta probing then isolated one downstream OR (ordinal 23,
`(lower & mask0) | (upper & mask1) != 0`) as the necessary compiler-sensitive
shape in that module. The shared emitter now rewrites only this semantically narrow
zero-test form to logical comparisons; arbitrary integer OR results remain unchanged.
The synthetic RED/GREEN artifact and probe results are retained under `_Build/analysis`.
The fresh game run `_Build/runs/yotei-integrated-20260907-211101-9da340` reached
frame 158 with `b90e2024732c6111` and `a7661ff4ea282325` completed. The exact
captured `b90e` artifact still crashes the minimal standalone probe during
pipeline creation, while the real game layout creates its pipeline successfully;
the difference is retained as an open probe-layout boundary. Eight source
readbacks remain RGB zero with alpha 3, so this result does not claim the first
nonzero frame.

## DWORD pattern fills and sampled HTILE clear materialization

Status: shared production fix and native synthetic GPU regression are GREEN; a
current-tree game readback remains pending behind the separate unfinished wave-ballot path.

Observed trigger: the compute-clear fast path recognized only the 16-byte `uint4`
fill shape. A full 4-byte DWORD pattern dispatch targeting registered HTILE was
therefore executed as an ordinary buffer shader without carrying its exact clear
encoding into depth metadata. The initial unchanged regression failed with
`complete dword-pattern HTile fill was not recognized`. Extending the fixture to
sample the resulting native D32 exposed a second failure: consuming only the logical
metadata state left stale raw guest metadata, and a repeated texture acquisition
materialized the old clear.

The shared correction now requires the exact descriptor, ten-user-SGPR pattern,
64x1x1 wave geometry, full group coverage, repeated DWORD value, write-only resource,
and absence of snapshots/images/samplers/DMA before replacing a dispatch. Rejected
partial, non-uniform and snapshot-dependent variants preserve the caller outputs.
An accepted metadata fill records its exact value and performs the same guest-memory
buffer fill; registered HTILE then maps only canonical `0` and `0xfffffff0` encodings
to native depth 0/1. Depth/stencil load clears use their respective HTILE bit fields,
and unsupported encodings still reject closed. No title, shader hash or guest address
is part of the mechanism.

Validation at `b1a894f-dirty`:

- RED log: `_Build/logs/pattern-htile-20260908/red.stderr.log`.
- GREEN log: `_Build/logs/pattern-htile-20260908/final-green.stdout.log`; executable
  SHA-256 `fc092c5ab176820803b25fa59b332e5f07879aa6c6ad4052c790f893e3198f97`.
- `--sampled-htile-clear-only`, `--sampled-htile-array-clear-only`,
  `--compute-meta-clear-only`, `--sampled-htile-admission-only`, and
  `--native-htile-subset-only` all pass. The clear selector also passes with the
  portable `VK_LAYER_KHRONOS_validation` loaded and GPU-assisted validation enabled.
- The modified current-tree production/test translation units compile with clang-cl.
  A normal full target remains blocked because neighboring dirty
  `spirvEmitterProgram.cpp` references missing
  `EmitterState::wave_ballot_word_variables`.
- Two bounded test-only game runs with executable SHA-256
  `57d425db107c4347fff58e1313da06b470d5879a9cab088ab9a41bc611bcaaeb`
  (`yotei-pattern-htile-current-20260908-092819-bb437d` and warm-cache
  `yotei-pattern-htile-current-warm-20260908-092936-fda3a1`) stopped at the prior
  `b90e2024732c6111` driver breakpoint before guest VideoOut/readback. The executable
  used only a temporary zero-initialized declaration to complete the unrelated dirty
  build; that declaration was removed immediately afterward. These runs do not prove
  the first frame or regress the HTILE synthetic result.

Remaining validation: restore a normal current-tree build by completing the ballot
change, repeat the bounded game run, and require at least one source readback with
nonzero RGB. Menu and gameplay remain separate pending stages.

## Partitioned graphics loop budget at the structured back-edge

Update 2026-10-03: a temporary Vertex budget of four iterations reached the
119856-index draw but produced invalid SPIR-V before GPU execution. Validator:
`OpPhi's number of incoming blocks (2) does not match block's predecessor
count (3)` in `_Build/runs/yotei-integrated-20261003-120114-presentfix-gpuav-sync`.
The budgeted back-edge adds a path to the loop merge without updating a merge
Phi. The new public `shader_cfg_tests
--partitioned-graphics-loop-merge-phi-only` produced the same validator RED
before the shared fix (`_Build/logs/loop-merge-phi-red2-20261003.txt.stderr`).
The emitter now applies the budget at an existing conditional exit when that
exit is the sole predecessor of the continue block. The unchanged merge Phi
retains the same predecessor set. If the merge has Phi values but no such
existing exit, the emitter does not add an invalid direct edge. The same test
is GREEN (`_Build/logs/partitioned-graphics-loop-merge-phi-green-20261003.txt`),
including a counter-update check; original partitioned loop, graphics wave64
collective and nested conditional-latch neighbors are GREEN. A temporary
Vertex cap still needs a bounded game retry as diagnosis, and is not a
semantic replacement for wave64.

Status: the synthetic validator regression and the captured game SPIR-V are GREEN;
the full native suite and nested-loop variants remain deferred.

The previous finite pixel-loop guard emitted a kill selection at the first
non-Phi instruction of the loop body. When that body was also the SPIR-V continue
target, the selection was not structurally post-dominated by the back-edge block.
A direct budget exit from the loop header avoided that error but introduced a new
path to the merge that bypassed body definitions. The unchanged captured shader
then failed dominance validation.

The shared emitter now records the structured loop header, merge and continue
blocks and applies the finite budget on the continue back-edge. The over-budget
edge reaches the existing merge only after the body has executed, preserving
dominance, while `OpLoopMerge` remains immediately adjacent to its branch.
`shader_cfg_tests --partitioned-graphics-loop-only` covers conditional while and
do-while back-edges; its original RED is retained at
`_Build/logs/partitioned-graphics-loop-red-20260908-v3.txt` and GREEN at
`_Build/logs/partitioned-graphics-loop-green-20260908-v2.txt`. The resulting game
module `f8927c09f4b928c7` also passes standalone `spirv-val`.

Remaining validation: run the complete native test suite and add explicit nested
and multiple-loop cases, including distinct continue targets and merge blocks.

## GFX10 VOPC V_CMPX_NE_U16

Status: production decoder/lowering, unchanged synthetic RED/GREEN, full CPU
corpus audit and runtime reproduction complete; bounded post-fix game retry pending.

Observed trigger: native run
`_Build/runs/yotei-integrated-20260908-171938-6e189d` on `672af1f` completed the
previous `6cc64dee32dc7094` and `34e090c623ad611c` frontiers, reached frame 222,
then exited itself with code 321 while building CFG for compute shader
`c8e8f554efbfadef`. The exact instruction at PC `0xf4` is compact VOPC raw
`0x7d7a40ff`, opcode `0xbd`. LLVM's GFX10 VOPC table identifies `0xbd` as
`V_CMPX_NE_U16`: compare the low unsigned halfwords for inequality, write the
lane mask and update EXEC. No title, shader hash, address or install-path branch
is involved.

The final regression uses the captured first DWORD with a controlled literal and
checks two-word decode, operands, EXEC destination, unsigned 16-bit inequality IR,
dynamic EXEC update, decoded text and SPIR-V validation. The unchanged test first
failed with `decoder rejected captured VOPC V_CMPX_NE_U16 fields`; after `5aaf3b4`
it prints `KYTY_VOPC_CMPX_NE_U16_PASS`.

Full corpus audit
`_Build/shader-audits/yotei-current-20260908-vopc-bd/report.json` passes 741 of
825 manifests. Both previous `0xbd` groups are absent: 19 compact manifests and
7 SDWA manifests now pass their captured audit depth. Remaining validation:

- Build and hash the full native `kyty_emulator` at the committed revision.
- Run the same bounded GPUAV-lite/source-readback profile using the preserved
  18 031 187-byte pipeline cache, prove `c8e8f554efbfadef` passes, and record the
  next first fatal or first nonzero RGB.
- Keep the 84 remaining corpus failures as independent debt. Do not claim them
  fixed from this opcode result, and do not prioritize them over an earlier
  runtime frontier without separate evidence.

## Local-region shared-tail CFG cloning and Yotei compile latency

Status: synthetic neighbors, exact manifests, full CPU corpus and bounded native
game timing are GREEN; first nonzero surface output remains pending.

The captured `6cc64dee32dc7094` graph has 346 blocks and one safe shared linear
tail in a two-block candidate region. The previous whole-graph cutoff rejected
that local rewrite, forced dispatcher fallback and produced a 359,238-word
SPIR-V module whose NVIDIA driver compile stalled for more than 15 minutes.
Removing the whole-graph cutoff without a local bound made neighboring
`ps_00051f2c.json` pathological because its candidate region contains 117/118
blocks.

Checkpoint `f1776f0` admits semantic cloning only while the local region has at
most 32 blocks and before synthetic `GotoVariable` routing exists. Existing
single-shared-block, straight-line, 16-instruction-tail and four-clone limits
remain. `shader_cfg_tests --shared-merge-cfg-only` covers seven neighboring CFG
shapes. Exact `6cc64dee…` is structured with 376 blocks and its full audit drops
from about 13.02 to 3.45 seconds. `ps_00051f2c.json` remains bounded through its
dispatcher path. Full corpus result is 743/825 in 37.851 seconds versus 741/825
in 57.019 seconds, with no new status regression.

Native GPUAV-lite run
`_Build/runs/yotei-integrated-20260908-200400-74722b` naturally reaches frame
133 in 145 seconds versus 1123 seconds for the prior comparable diagnostic run.
Warm run `_Build/runs/yotei-integrated-20260908-201027-d8c982` naturally reaches
frame 130 in 120 seconds. Its 118 source readbacks are still RGB zero/alpha 3.

Remaining validation:

- Keep pathological large-region and post-`GotoVariable` cases bounded while
  adding future structurization rewrites.
- Measure steady rendering FPS only after a nonzero frame exists; the current
  figures are startup/compiler progress, not gameplay performance.

## GPUAV shader instrumentation cache identity

Status: **shared cache-identity fix verified in native synthetic test**. Before
the fix, `DriverCacheFileName` and `FormatDriverCacheSignature` took only the
GPUAV enabled bit. A GPUAV run with `VK_LAYER_GPUAV_SHADER_INSTRUMENTATION=0`
and one with `=1` therefore selected the same `-gpuav.bin` blob and `v1`
signature, although the validation layer can hand different SPIR-V modules to
the driver.
The 3 October no-instrumentation run loaded the 306192646-byte GPUAV cache and
then hit `0x80000003` while creating compute pipeline `b90e2024732c6111`.
This was a collision in the cache identity, but the crash cause remains
unproved: an isolated no-layer probe also crashes on that module. The same
synthetic `shader_cfg_tests --pipeline-cache-instrumentation-only` failed before
the fix with `distinct validation instrumentation modes shared a driver cache
file` (`_Build/logs/cache-instrumentation-red-20261003.txt.stderr`) and passed
after the fix (`_Build/logs/pipeline-cache-instrumentation-green-20261003.txt`).
The native neighboring validation-mode, revision and build-identity checks also
passed. The three modes now use `-core.bin`/`v0`, `-gpuav-lite.bin`/`v2`, and
`-gpuav-instr.bin`/`v3`; the old mixed `v1` signature is rejected. Next: native
emulator build and a bounded game retry with the new isolated cache. Do not
interpret the identity test as proof that it fixes the `b90e` compiler crash.

## GPUAV-sensitive NVIDIA pipeline cache identity

Status: **shared identity fixed** (KytyPC3 + separate `{TITLE}-core.bin` /
`{TITLE}-gpuav.bin`). No-GPUAV game retry after cool-down still required to
confirm early graphics pipeline with a clean core cache.

Two bounded runs using a cache warmed under GPUAV-lite but launching without
GPUAV fail at frame 12/13 inside NVIDIA `nvgpucomp64.dll` 32.0.16.1664 with
exception `0x80000003`. Windows Event Log attributes both APPCRASH events to the
same module offset. The instrumented GPUAV-lite path continues to the later
emulator frontiers. Driver cache blobs from GPUAV-lite and non-GPUAV attempts are
therefore preserved under separate filenames and must not be mixed.

Tests:

- GREEN: `shader_cfg_tests --pipeline-cache-validation-mode-only` →
  `KYTY_PIPELINE_CACHE_VALIDATION_MODE_PASS` (distinct filenames; core vs GPUAV
  signatures incompatible; self-compatible). Neighbors
  `--pipeline-cache-revision-only` / `--pipeline-cache-identity-only`.
- Keep the native driver reproduction bounded and do not repeatedly crash the
  compiler merely to test cache loading.
- Next: after a long DeviceLost cool-down (GPUAV+instr recovers; core/noval and
  GPUAV-lite still abort in `vkCreateComputePipelines` for CS `0xb90e…` with
  `exit=-2147483645` for ≥12m), run no-GPUAV `ContinueAfterColored` with empty
  `_PipelineCache/PPSA26344-core.bin`. Do not load the legacy mixed
  `PPSA26344.bin`. Separately, consider tagging shader-instrumentation in the
  cache identity (lite vs instr currently share `-gpuav.bin`).

## GFX10 MUBUF opcode 0x87 runtime frontier

Status: exact native runtime RED captured; decoder/lowering regression and fix
pending.

Run `_Build/runs/yotei-integrated-20260908-200400-74722b` naturally exits while
building compute shader `c8e8f554efbfadef` at PC `0xc6c`. The decoded family is
MUBUF opcode `0x87`, raw words `[0xe21c6000 0x8002000e]`; CFG construction rejects
it as unimplemented. This is earlier and more actionable than speculative FPS
ports because execution cannot pass the instruction.

Required tests:

- Identify opcode `0x87` from the GFX10 ISA tables and add an exact raw-word RED
  covering operands, format/packing and destination footprint.
- Implement the shared decoder/IR/backend semantics without title, hash, address
  or install-path branches; keep unsupported neighboring formats fail-closed.
- Run unchanged GREEN, neighboring opcode cases, the 825-manifest corpus and a
  warm GPUAV-lite/source-readback game retry.

## First nonzero Yotei source frame

Status: **rendered-pixel milestone reached on current tip; menu and gameplay remain pending.**

Historical proof on `96611fe`: GPUAV-lite run
`_Build/runs/yotei-integrated-20260909-101008-669804` reached `shown=280`.
Source readback stayed black through frame 235; frame 236 had 10 nonzero RGB
pixels and frame 242 had 214. Screenshot
`_Build/analysis/yotei-first-nonzero-96611fe.png` shows the white loading
spinner.

**Re-proven 26 September 2026** on bring-up tip `fb1cc8b` / plan note `23e4fd74`:
warm GPUAV+instrumentation fullscreen run
`_Build/runs/yotei-integrated-20260926-081515-presentfix-gpuav` —
prepared **frame 250** `colored=10` (max RGB 45/45/45), then frames 251–276
grow to `colored=108` / max ~546/525/546; `shown=277`, auto-stop
`colored-proven` (~147 s). Present blit after Flip EOP is the readback site.

Remaining validation:

- Preserve the readback as the evidence boundary: do not promote the result to
  menu or gameplay until a recognizable full scene / menu frame is captured.
- Continue bounded runs past spinner (`ContinueAfterColored`); record the first
  post-spinner compile, resource, GPU execution, or presentation blocker.
  Evidence 26.09 `…-081840-presentfix-gpuav`: frame watchdog at **shown=159**
  while CS `54904fb419d79e49` did **4×** specialization recompile —
  `vkCreateComputePipelines` ≈289.8 / 295.5 / 286.8 s, fourth `begin` without
  `done` at Kill. ReadbackStart=240 never reached (`sawColored=false`).
- After `/STACK:16777216` + SPIR-V permutation reuse (`…-093517`): reached
  **shown=197**, then Fatal `MaterializeResources` (exit 321). Menu still pending.
- Dense buffer limit: raise shared `ShaderInfo::MaxBuffers` 64→128→512 (device
  DescriptorBudget remains the hard gate). `…-112601` cleared densify then hit
  RO SSBO host `adjustment=2` (`storage buffer offset adjustment is unsupported`).
- Shared RO realign: `NativeStorageBuffer` stages Upload when dword-indexed SSBO
  has byte host adj and `read && !written && !atomic` (non-GPU-dirty). RED/GREEN:
  `shader_recompiler_compute_tests --storage-buffer-ro-aligned-upload-only`
  (`_Build/logs/ro-align-{red,green}.out.txt`). Writable/atomic/GPU-dirty stay
  fail-closed. Long run after this tip: `…-120514-presentfix-gpuav` in progress.
- Debt for shared fix: print/use `LastResourceSpecializationError` on that Fatal;
  continue specialization-stable pipeline identity / smaller SPIR-V under GPUAV —
  not a title branch and not “merge #718 first”.
- Re-run the accumulated shader/GPU test debt before upstream submission; the
  rendered-pixel milestone does not waive neighboring regressions.

## Existing CFG literal-word assertion (2026-10-04; baseline confirmed)

The full native `shader_cfg_tests` suite fails in its legacy literal-word
assertion, before later tests. Reversing only the new SRT-bound and continue
bridge changes reproduces the same failure: logs
`_Build/logs/formatted-mask-cfg-baseline-suite-20261004.log{,.stderr,.run.json}`.
Do not describe the full suite as GREEN. The scalar MOV/store fixture needs
inspection of its valid resource/observable-store setup before changing an
expectation; the current emulator legitimately may eliminate unreachable
store operands. Relevant nested latch/shared merge/graphics-loop selectors
were checked separately and passed.
