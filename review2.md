<!-- coderabbit-ai-generated:v1 -->

**Actionable comments posted: 3**

> [!CAUTION]
> Some comments are outside the diff and can’t be posted inline due to GitHub limitations.
> 
> **⚠️ Outside diff range comments (1)**
> 
> <details>
> <summary><strong><picture><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/amber-16.svg" width="16" height="16" align="absmiddle" alt=""></picture> Minor</strong> · Update mutex ownership across both condition-variable waits. · <code>threads.cpp:150</code></summary><blockquote>
> <dl>
> <dd>
> <br>
> 
> `src/common/threads.cpp:150`
> **<picture><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/amber-16.svg" width="16" height="16" align="absmiddle" alt=""></picture> Minor** · **Stability & Availability** · **Quick win**
> 
> **Update mutex ownership across both condition-variable waits.**
> 
> Both non-Windows waits pass `m_mutex` directly. `condition_variable_any` therefore releases and reacquires that object without calling `Mutex::Unlock` or `Mutex::Lock`. ([ece.uvic.ca](https://www.ece.uvic.ca/~frodo/cppbook/cppdraft/n4861/thread.condvarany.wait))
> 
> If thread B acquires and releases the mutex while thread A waits, B clears `m_owner`. After A resumes, A holds the mutex but `IsHeldByCurrentThread()` returns false. This also defeats the supplied `EmergencyShutdown` same-thread guard if invoked in that state.
> 
> Use a BasicLockable adapter that calls the ownership-tracking wrappers for both waits. Preserve notification and timeout behavior.
> 
> 
> 
> 
> 
> 
> Also applies to: 169-169
> 
> <!-- cr-comment:v1:a7d9c8b1745e268bfcbc4c81 -->
> 
> </dd>
> </dl>
> </blockquote></details>

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
- **Run ID**: `721c94b2-2ee2-4ea1-a1e3-56049d84aec3`

</dd>
</dl>
</details>

<details>
<summary><picture><source media="(prefers-color-scheme: dark)" srcset="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/git-commit-horizontal-dark.svg"><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/git-commit-horizontal.svg" width="16" height="16" align="absmiddle" alt=""></picture> Commits</summary>
<dl>
<dd>
<br>

Reviewing files that changed from the base of the PR and between 8958064dad8711894ed50e3f7438bf5674424def and 7d2b333b010751005297d315ac93a707cf6e1679.

</dd>
</dl>
</details>

<details>
<summary><picture><source media="(prefers-color-scheme: dark)" srcset="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/file-plus-corner-dark.svg"><img src="https://storage.googleapis.com/coderabbit_public_assets/review-comment-icons-v1/file-plus-corner.svg" width="16" height="16" align="absmiddle" alt=""></picture> Files selected for processing: 16</summary>
<dl>
<dd>
<br>

* `CMakeLists.txt`
* `src/common/threads.cpp`
* `src/common/threads.h`
* `src/graphics/guest_gpu/graphicsRun.cpp`
* `src/graphics/host_gpu/memoryTracker.h`
* `src/graphics/host_gpu/regionManager.h`
* `src/graphics/host_gpu/renderer/cache/bufferCache.cpp`
* `src/graphics/host_gpu/renderer/cache/bufferCache.h`
* `src/graphics/host_gpu/renderer/cache/textureCache.cpp`
* `src/graphics/host_gpu/renderer/pipeline/pipelineSerialization.cpp`
* `src/graphics/host_gpu/renderer/renderCompute.cpp`
* `src/graphics/host_gpu/renderer/renderDraw.cpp`
* `src/libs/agc.cpp`
* `src/libs/agc.h`
* `tests/MemoryTrackerTests.cpp`
* `tests/ShaderRecompilerComputeTests.cpp`

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

<!-- coderabbit-review-publication v1 publication=bb2328e1-b2db-49f1-b4d3-3313ec114717 attempt=a42839dd-a660-4764-a0ca-5bb2779d6f9f batch=1/1 -->