<!-- coderabbit-ai-generated:v1 -->

**Actionable comments posted: 11**

> [!CAUTION]
> Some comments are outside the diff and can’t be posted inline due to GitHub limitations.
> 
> **⚠️ Outside diff range comments (4)**
> 
> <details>
> <summary><strong><picture><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/major-16.svg" width="16" height="16" align="absmiddle" alt=""></picture> Major</strong> · Preserve batched GPU unmaps when cache reuse is off. · <code>memory.cpp:2348</code></summary><blockquote>
> <dl>
> <dd>
> <br>
> 
> `src/kernel/memory.cpp:2348`
> **<picture><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/major-16.svg" width="16" height="16" align="absmiddle" alt=""></picture> Major** · **Functional Correctness** · **Quick win**
> 
> **Preserve batched GPU unmaps when cache reuse is off.**
> 
> Releasing two adjacent direct views now calls `SuspendOrUnmapGpuRange` once per view. With reuse off, that emits two unmap events. `TestDirectReleaseUnmapsGpuRuns` requires one event covering the combined span, so the full test run stops before the new remap cases. Use the existing contiguous-run unmap path when reuse is off. Keep per-view suspension when reuse is on so each alias retains its own remap identity. ([raw.githubusercontent.com](https://raw.githubusercontent.com/KytyPS5/KytyPS5/b56e9015c74b275912ab8237091f01d3ddcda726/src/kernel/memory.cpp))
> 
> <!-- cr-comment:v1:31cc3c3a5b6c0ef8a94d484e -->
> 
> </dd>
> </dl>
> </blockquote></details>
> <details>
> <summary><strong><picture><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/amber-16.svg" width="16" height="16" align="absmiddle" alt=""></picture> Minor</strong> · Check that synchronization uploads only the written page. · <code>ShaderRecompilerComputeTests.cpp:7404</code></summary><blockquote>
> <dl>
> <dd>
> <br>
> 
> `tests/ShaderRecompilerComputeTests.cpp:7404`
> **<picture><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/amber-16.svg" width="16" height="16" align="absmiddle" alt=""></picture> Minor** · **Functional Correctness** · **Quick win**
> 
> **Check that synchronization uploads only the written page.**
> 
> The dirty-mask assertion runs before synchronization. If synchronization uploads all four pages, the three unchanged pages still contain the expected words and `RequireNativePages` passes. Enable `BufferUploadBytes` for this probe and assert that the counter increases by `kSuspendPageSize` across the synchronization. Keep the native-word checks to verify the uploaded data. ([raw.githubusercontent.com](https://raw.githubusercontent.com/KytyPS5/KytyPS5/b56e9015c74b275912ab8237091f01d3ddcda726/src/graphics/host_gpu/renderer/cache/bufferCache.cpp))
> 
> <!-- cr-comment:v1:7e642efaff6757077392b304 -->
> 
> </dd>
> </dl>
> </blockquote></details>
> <details>
> <summary><strong><picture><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/amber-16.svg" width="16" height="16" align="absmiddle" alt=""></picture> Minor</strong> · Keep the GPU object installed during the stopping-state… · <code>ShaderRecompilerComputeTests.cpp:7870</code></summary><blockquote>
> <dl>
> <dd>
> <br>
> 
> `tests/ShaderRecompilerComputeTests.cpp:7870`
> **<picture><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/amber-16.svg" width="16" height="16" align="absmiddle" alt=""></picture> Minor** · **Functional Correctness** · **Quick win**
> 
> **Keep the GPU object installed during the stopping-state release.**
> 
> `ShutdownGpu()` resets the GPU pointer before `KeepRelease` runs. The test therefore exercises the absent-GPU fallback, not the stopping-GPU fallback. If `SuspendMemory` stops checking `IsStopping()` but still handles a null GPU pointer, this test passes. Call `rig.context.GetGpu().Shutdown()` before the release and remap, as the suspend fixture does, then call `ShutdownGpu()` after the checks. ([raw.githubusercontent.com](https://raw.githubusercontent.com/KytyPS5/KytyPS5/b56e9015c74b275912ab8237091f01d3ddcda726/src/graphics/host_gpu/renderer/renderContext.cpp))
> 
> <!-- cr-comment:v1:ec13e5007df7268629e37cd2 -->
> 
> </dd>
> </dl>
> </blockquote></details>
> <details>
> <summary><strong><picture><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/amber-16.svg" width="16" height="16" align="absmiddle" alt=""></picture> Minor</strong> · Make the baseline write order explicit instead of… · <code>ShaderRecompilerComputeTests.cpp:3806-3807</code></summary><blockquote>
> <dl>
> <dd>
> <br>
> 
> `tests/ShaderRecompilerComputeTests.cpp:3806-3807`
> **<picture><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/amber-16.svg" width="16" height="16" align="absmiddle" alt=""></picture> Minor** · **Functional Correctness** · **Quick win**
> 
> **Make the baseline write order explicit instead of relying on `std::fill_n`.**
> 
> `std::fill_n` specifies the values written, not the order of the stores. At lines 3806-3807, the baseline therefore depends on an unspecified order. The image starts at `slot + 1.5` pages, and the fill ends at `slot + 1.75` pages. The second protected page contains bytes before and inside the image. If a build stores to an in-image address first, the first fault on that page lands inside the image. The check at lines 3817-3820 then fails because the baseline image is CPU-dirty. That failure reflects store order, not the bulk invalidation under test.
> 
> The baseline must write each protected page in ascending address order, and the test should enforce that order. For example, use ascending `volatile` stores. Keep the existing fault-count and head-page assertions.
> 
> <!-- cr-comment:v1:cf0a2da0fb73e9c5c273480f -->
> 
> </dd>
> </dl>
> </blockquote></details>

<details>
<summary><picture><source media="(prefers-color-scheme: dark)" srcset="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/brush-cleaning-dark.svg"><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/brush-cleaning.svg" width="16" height="16" align="absmiddle" alt=""></picture> Nitpick comments: 1</summary><blockquote>
<dl>
<dd>
<br>

<details>
<summary>tests/MemoryTrackerTests.cpp: 1</summary><blockquote>
<dl>
<dd>
<br>

`472-472`: **<picture><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/neutral-16.svg" width="16" height="16" align="absmiddle" alt=""></picture> Trivial** · **Maintainability & Code Quality** · **Quick win**

**Make the fresh-tracker check distinguish tracker identity from generation.**

The check at Line 471 still passes if `ForEachUploadRange` stops comparing tracker IDs. The fresh tracker has no region, so its generation lookup returns zero. The previous tracker records a nonzero generation. Prepare the second tracker by uploading the neighboring page in the same region. Leave the target page dirty and match the memo's address, size, write mode, and generation before testing the upload. ([raw.githubusercontent.com](https://raw.githubusercontent.com/KytyPS5/KytyPS5/b56e9015c74b275912ab8237091f01d3ddcda726/src/graphics/host_gpu/memoryTracker.h))

<!-- cr-comment:v1:93d13d9e1e7eb0359391e651 -->

</dd>
</dl>
</blockquote></details>

</dd>
</dl>
</blockquote></details>

<h6>
<details>
<summary><picture><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/info.svg" width="12" height="12" align="absmiddle" alt=""></picture> Review info</summary>
<dl>
<dd>
<br>

<details>
<summary><picture><source media="(prefers-color-scheme: dark)" srcset="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/cog-dark.svg"><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/cog.svg" width="16" height="16" align="absmiddle" alt=""></picture> Run configuration</summary>
<dl>
<dd>
<br>

- **Source mode**: Auto
- **Repository**: [KytyPS5/KytyPS5](<https://github.com/KytyPS5/KytyPS5>)
- **Branch**: [main](<https://github.com/KytyPS5/KytyPS5/tree/main>)
- **File**: [.coderabbit.yaml](<https://github.com/KytyPS5/KytyPS5/blob/a1fc5ae438ea343480c9850f585d8c0afed06241/.coderabbit.yaml>)
- **Commit**: [a1fc5ae](<https://github.com/KytyPS5/KytyPS5/commit/a1fc5ae438ea343480c9850f585d8c0afed06241>)
- **Layers**: Repository YAML
- **Review profile**: CHILL
- **Plan**: Advanced
- **Run ID**: `a705a295-e41b-4323-8b89-144463633f2c`

</dd>
</dl>
</details>

<details>
<summary><picture><source media="(prefers-color-scheme: dark)" srcset="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/git-commit-horizontal-dark.svg"><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/git-commit-horizontal.svg" width="16" height="16" align="absmiddle" alt=""></picture> Commits</summary>
<dl>
<dd>
<br>

Reviewing files that changed from the base of the PR and between 7d2b333b010751005297d315ac93a707cf6e1679 and b56e9015c74b275912ab8237091f01d3ddcda726.

</dd>
</dl>
</details>

<details>
<summary><picture><source media="(prefers-color-scheme: dark)" srcset="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/file-plus-corner-dark.svg"><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/file-plus-corner.svg" width="16" height="16" align="absmiddle" alt=""></picture> Files selected for processing: 20</summary>
<dl>
<dd>
<br>

* `CMakeLists.txt`
* `src/common/emulatorConfig.cpp`
* `src/common/emulatorConfig.h`
* `src/common/threads.h`
* `src/graphics/host_gpu/pageManager.cpp`
* `src/graphics/host_gpu/pageManager.h`
* `src/graphics/host_gpu/renderer/cache/bufferCache.cpp`
* `src/graphics/host_gpu/renderer/cache/bufferCache.h`
* `src/graphics/host_gpu/renderer/cache/textureCache.cpp`
* `src/graphics/host_gpu/renderer/cache/textureCache.h`
* `src/graphics/host_gpu/renderer/renderContext.cpp`
* `src/graphics/host_gpu/renderer/renderContext.h`
* `src/graphics/presentation/window/vulkanWindow.cpp`
* `src/kernel/memory.cpp`
* `src/kernel/memory.h`
* `src/main.cpp`
* `tests/MemoryTrackerTests.cpp`
* `tests/PageManagerTests.cpp`
* `tests/ShaderRecompilerComputeTests.cpp`
* `tests/VirtualMemoryAllocationTests.cpp`

</dd>
</dl>
</details>

<details>
<summary><picture><source media="(prefers-color-scheme: dark)" srcset="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/file-too-large-dark.svg"><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/file-too-large.svg" width="16" height="16" align="absmiddle" alt=""></picture> Files not fully reviewed because they exceed a per-file size limit: 1</summary>
<dl>
<dd>
<br>

* tests/ShaderRecompilerComputeTests.cpp

</dd>
</dl>
</details>

**Included review availability:** This review used your included allowance. Your plan provides up to 10 included reviews per hour; 8 remain after this review.

</dd>
</dl>
</details>
</h6>

<!-- This is an auto-generated comment by CodeRabbit for review status -->

<!-- coderabbit-review-publication v1 publication=d94f04e2-2378-4cf4-aea9-245b59b45387 attempt=6564057a-ca35-417a-9bf3-834af4565f27 batch=1/1 -->