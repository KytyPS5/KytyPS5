# Shader capture and batch audit

The corpus tools inspect shader code and compiler failures without executing shaders on a GPU. The compute fixture runner below executes the existing synthetic GPU tests. Run the Windows commands in PowerShell from the repository root, after following the [Windows build setup](../README.md#build-requirements-windows).

## Build the auditor

```powershell
cmake --build _Build/windows --target shader_cfg_tests
```

The examples use the Ninja build directory `_Build/windows`. Pass `-Auditor` to the runner if `shader_cfg_tests.exe` is elsewhere.

## Prepare a corpus

Choose either capture during an emulator run or extraction from a supported container. A corpus consists of JSON manifests and their referenced `.bin` files. Keep unrelated JSON files outside the corpus: the runner discovers manifests recursively by extension.

### Capture registered and dispatched shaders

With an installed emulator build:

```powershell
.\_Build\windows\install\kyty_emulator.exe --game "C:\path\to\game" `
  --graphics-debug-dump true --shader-log-direction File `
  --shader-log-folder ".\_Build\shader-capture\example"
```

`registered` captures code and AGC metadata when shaders are registered, before a later compilation failure can stop the run. It includes only registrations reached during that run. `dispatched` captures shaders reaching pipeline compilation, including the compute parameters needed for deeper auditing. Captures contain code and plain metadata; runtime resource contents and usable host pointers are not included. Matching code with different compilation metadata can produce separate manifests.

Audit either subdirectory. Auditing their parent includes both sets and may count the same code with multiple metadata records.

### Extract a KCAP container

The extractor requires Python 3 and uses only its standard library:

```powershell
python .\tools\extract-shader-corpus.py "C:\path\to\shaders.xpps" `
  .\_Build\shader-corpus\example
```

The destination must be new or empty. The extractor validates the supported KCAP layout, AGC headers and bounded code references before writing files. It rejects unsupported or ambiguous layouts; it is not a general extractor for every shader container. Each header gets a manifest, while identical code ranges share a binary. The summary is printed to the console.

An optional profile derives compute settings from AGC headers and SH registers:

```powershell
python .\tools\extract-shader-corpus.py "C:\path\to\shaders.xpps" `
  .\_Build\shader-corpus\example-profile --compute-header-profile
```

This enables compute translation and resource tracking with explicitly recorded assumptions. Actual dispatch state may override header values. The manifests retain `metadata_complete: false` and `runtime_context_complete: false`; the auditor tests both LDS barrier settings for this profile. Extraction without the option checks decoding and control flow only.

## Run the batch audit

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\audit-shaders.ps1 `
  -CorpusDirectory .\_Build\shader-corpus\example-profile `
  -OutputDirectory .\_Build\shader-audits\example-profile `
  -Jobs 4 -TimeoutSeconds 30
```

For live captures, replace `-CorpusDirectory` with the `registered` or `dispatched` directory. `-OutputDirectory` must be new or empty and outside the corpus; existing results are preserved and audit artifacts cannot become shader inputs. Omit it to create a unique directory under `_Build/shader-audits`.

Each manifest runs in a separate worker process, with up to `-Jobs` workers active. Workers create no console windows and both output streams are drained asynchronously. A failure or timeout is recorded and the batch continues. Timeout handling kills the affected worker and waits for termination with a bound; normal completion and script cleanup dispose of worker processes. Forcibly terminating the PowerShell host can bypass script cleanup.

The runner writes:

- `report.md`: coverage, status totals and grouped failures.
- `report.json`: the structured report and individual results.
- `results.jsonl`: one completed result per line.
- Numbered job directories: each worker's `stdout.txt` and `stderr.txt`.

The runner exits with code 1 if any manifest fails or times out, and 0 if all pass. Input or runner errors also produce a nonzero exit code. Counts refer to manifests, not unique binaries. Unsupported instructions are grouped by family, opcode and reason, with distinct shader counts and PC examples; one shader can appear in several groups.

### Check compute execution planning for a host

Add `-ComputeHostProfile path\to\host.json` to run the shared CPU compute planner
after resource tracking. Supply measured device limits rather than inferred defaults.
The following profile describes the tested RTX 4060; other hosts need their own values:

```json
{
  "schema_version": 1,
  "max_size": [1024, 1024, 64],
  "max_invocations": 1024,
  "native_subgroup_size": 32,
  "can_require_subgroup_size_64": false,
  "max_shared_memory_bytes": 49152
}
```

Keep this JSON outside the corpus. The runner copies and hashes it in the output
directory so all workers use the same input. The worker can also be invoked directly:

```powershell
.\_Build\windows\shader_cfg_tests.exe --audit-shader path\to\shader.json `
  --compute-host-profile path\to\host.json
```

`compute_execution_precheck` reports `rejected`, `not_rejected` or `not_checked`.
It runs before resource specialization, with no runtime descriptor values or guest
memory reads. Unknown buffer ADD_TID is checked both ways: rejection under both
assumptions is counted as an early failure; a conditional rejection remains in
`possible_errors`. The individual attempts and successfully translated LDS-barrier
profiles are retained. A fatal translation still ends that worker; later profiles
of that manifest remain unchecked, while the batch proceeds to other manifests.

The report groups independent planner reasons separately and records precheck
coverage for all manifests. Missing stage metadata remains `not_checked`.
`not_rejected` does not establish post-specialization admission, valid SPIR-V or GPU
execution. Compare counts only between audits using the same stages and host profile.

## Interpret coverage

| Coverage | What was checked |
| --- | --- |
| `cfg_structured` | Instructions decoded and control flow structured. |
| `cfg_dispatcher_fallback_required` | Instructions decoded and a control-flow graph was built; structured control flow requires the dispatcher fallback. Fallback translation was not validated. |
| `resource_tracking` | Compute IR translation and resource-plan extraction completed, using captured dispatch parameters or the explicitly assumed header profile. |
| `compute_execution_precheck` | The CPU compute planner checked the available profiles before specialization; individual outcomes and unknown descriptor requirements are recorded. |

A passing result applies only to the reported coverage. Non-compute stages and compute captures without sufficient metadata stop at control-flow analysis. The audit does not materialize runtime descriptors, emit or validate SPIR-V, execute GPU work, or prove that a game renders correctly. Use targeted compiler tests, GPU regression tests and runtime checks to validate fixes beyond the audit's scope.

## Run compute fixtures and retain every failure

```powershell
cmake --build _Build/windows --target shader_recompiler_compute_tests
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\run-compute-cases.ps1 `
  -CasePattern 'Scalar.*Mask|Waterfall' -TimeoutSeconds 30
```

Omit `-CasePattern` to run every registered compute fixture; the pattern is a case-sensitive regular expression. Pass `-Executable` for a different native test executable. The runner obtains names using `--list-compute-cases`, then invokes each selected name with `--compute-case` in a separate process. Workers run strictly sequentially, create no console windows, and drain both streams asynchronously into their own log files. Do not run another GPU test or emulator concurrently.

The listing and every case have the specified timeout. A failure or timeout is recorded and the next case runs after the previous process has terminated. If termination cannot be confirmed, the batch stops and reports the unrun count. A timeout bounds the worker process; it does not establish that a shader loop is safe for the driver, so potentially hanging regressions still require a bounded fixture or compiler-level test first.

`-OutputDirectory` must be new or empty; by default a unique directory is created under `_Build/compute-tests`. It contains `report.json`, incrementally written `results.jsonl`, listing logs, and numbered per-case directories with `stdout.txt` and `stderr.txt`. Exit code is 1 for any failed/timed-out case, listing error, or empty selection, and 0 when all selected cases pass. Results apply to these fixtures and the current GPU; they do not prove that a game renders correctly.

## Finite FP64 arithmetic

The recompiler represents binary64 values as exact pairs of 32-bit words. Signed
and unsigned 32-bit integer conversions require no native Float64 feature.

Native multiplication, fused multiply-add, reciprocal and narrowing to FP32 are
admitted only when the compiler proves finite operands and results that are zero
or normal. It uses known constants and typed integer-derived dataflow; unknown
runtime bit pairs, unproved pair provenance, subnormal/overflow ranges and possible
zero reciprocal denominators are rejected. Reciprocal currently requires a proved
nonzero integer with magnitude below 2^32. Guest FP state must be known, relevant
rounding modes must be nearest-even, and shaders that write MODE are rejected for
this arithmetic path.

The logical Vulkan device must have the required Float64, FMA and float-control
features enabled. Multiplication and FMA use guaranteed `OpFmaKHR`; reciprocal
uses a fused Newton correction to meet the guest ISA error bound. Native
capabilities cover every retained arithmetic instruction after dead-code
elimination. Signed zero is preserved
explicitly. Supported arithmetic sources are SGPR/VGPR pairs. MUL/FMA accept source
absolute-value and negation modifiers. Literal/inline FP64 sources, DPP/SDWA
forms, output modifiers and e64 forms of reciprocal/narrowing remain outside
this supported contract.

Run the focused compiler checks from PowerShell:

```powershell
cmake --build _Build/windows --target shader_cfg_tests shader_recompiler_compute_tests
.\_Build\windows\shader_cfg_tests.exe --fma-khr-validator-only
.\_Build\windows\shader_cfg_tests.exe --f64-certificate-only
.\_Build\windows\shader_cfg_tests.exe --unused-f64-emission-only
.\_Build\windows\shader_recompiler_compute_tests.exe --f64-admission-only
```

Run the synthetic GPU cases sequentially, retaining every result:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\run-compute-cases.ps1 `
  -CasePattern '^(CvtF64|MulF64|FmaF64|RcpF64|CvtF32F64|F64IntegerDerived)' `
  -TimeoutSeconds 30
```

These cases check exact conversion/product/FMA words, a fused-versus-unfused
residual, rounding boundaries, signed zeros, register aliases and reciprocal
intervals allowed by the ISA. Other buffer words and sentinels compare exactly.
The bounded arithmetic class and results on the tested GPU do not establish
support for arbitrary FP64 shaders or successful game rendering.

## Localize GPU execution faults

For a diagnostic run, set `KYTY_GPU_SYNC_DIAGNOSTICS=1` in the emulator process environment.
The command processor submits and waits immediately before and after each nonempty guest
compute dispatch, logging `GpuDispatchSync` phases and the guest shader address. A failed
`before-wait` points to earlier queued work; a failed `after-wait` identifies a candidate
interval containing the dispatch and its resource preparation. Presentation can also
submit work to the shared queue during that interval. A matching `after-complete`
confirms that batch completed. This does not identify an individual failing GPU instruction.

`KYTY_GPU_SYNC_MIN_WORKGROUPS` and `KYTY_GPU_SYNC_GROUPS` filter compute dispatches.
Non-empty indexed and auto draws remain traceable while diagnostics are enabled because
draws do not have a guest workgroup-count tuple to compare with those dispatch filters.

The setting changes scheduling and can hide timing-dependent faults. Keep the normal
asynchronous run as a separate check. Unset the variable after diagnosis; no extra waits
are added by default. The waits do not replace Vulkan or shader validation.
