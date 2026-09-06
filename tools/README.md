# Shader capture and batch audit

These tools inspect shader code and compiler failures without executing shaders on a GPU. Run the Windows commands below in PowerShell from the repository root, after following the [Windows build setup](../README.md#build-requirements-windows).

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

For live captures, replace `-CorpusDirectory` with the `registered` or `dispatched` directory. `-OutputDirectory` must be new or empty; existing results are preserved and a nonempty destination is rejected. Omit it to create a unique directory under `_Build/shader-audits`.

Each manifest runs in a separate worker process, with up to `-Jobs` workers active. Workers create no console windows and both output streams are drained asynchronously. A failure or timeout is recorded and the batch continues. Timeout handling kills the affected worker and waits for termination with a bound; normal completion and script cleanup dispose of worker processes. Forcibly terminating the PowerShell host can bypass script cleanup.

The runner writes:

- `report.md`: coverage, status totals and grouped failures.
- `report.json`: the structured report and individual results.
- `results.jsonl`: one completed result per line.
- Numbered job directories: each worker's `stdout.txt` and `stderr.txt`.

The runner exits with code 1 if any manifest fails or times out, and 0 if all pass. Input or runner errors also produce a nonzero exit code. Counts refer to manifests, not unique binaries. Unsupported instructions are grouped by family, opcode and reason, with distinct shader counts and PC examples; one shader can appear in several groups.

## Interpret coverage

| Coverage | What was checked |
| --- | --- |
| `cfg_structured` | Instructions decoded and control flow structured. |
| `cfg_dispatcher_fallback_required` | Instructions decoded and a control-flow graph was built; structured control flow requires the dispatcher fallback. Fallback translation was not validated. |
| `resource_tracking` | Compute IR translation and resource-plan extraction completed, using captured dispatch parameters or the explicitly assumed header profile. |

A passing result applies only to the reported coverage. Non-compute stages and compute captures without sufficient metadata stop at control-flow analysis. The audit does not materialize runtime descriptors, emit or validate SPIR-V, execute GPU work, or prove that a game renders correctly. Use targeted compiler tests, GPU regression tests and runtime checks to validate fixes beyond the audit's scope.
