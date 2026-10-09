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
