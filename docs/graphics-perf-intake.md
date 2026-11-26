# Graphics and performance intake

`main` is the upstream comparison point. Work on `integration/graphics-perf`; each port below can be reverted independently. These changes do not enable RT, override AA, skip draws, or relax synchronization.

| Input | Branch | Revision reviewed |
|---|---|---|
| [KytyPS5/KytyPS5](https://github.com/KytyPS5/KytyPS5) | `main` | `93cce66564bdced2fc7054ca9794451dfeef4d9d` |
| [Z0mbieee547/AnyKyty-Yotei-RT](https://github.com/Z0mbieee547/AnyKyty-Yotei-RT) | `AnyKyty` | `c175b705ad333116eb8235d47b704b44f7d49619` |
| [usa-ratt/KytyPS5-GT7](https://github.com/usa-ratt/KytyPS5-GT7) | `gt7-eqaa` | `f9086362bf80358ec8420d531b4db7deeadc023b` |

## Landed on the integration branch

| Fork commit | Donor | What changed and what did not |
|---|---|---|
| `073643074b` | [GT7 `457202dd09`](https://github.com/usa-ratt/KytyPS5-GT7/commit/457202dd095b0c33f06ba03d0e6a4d3cf727660c) | A heap texture with out-of-range DCC metadata now takes the existing null-texture path. Upstream `IsMapped` already rejects bad heap texel ranges. The donor's literal `ValidOrEmpty()` would reject a valid metadata address with unknown size, so this uses the same one-byte minimum as the existing color-image validator. Non-heap textures retain strict validation. |
| `af41f3ecf5` | [AnyKyty `c175b705ad`](https://github.com/Z0mbieee547/AnyKyty-Yotei-RT/commit/c175b705ad333116eb8235d47b704b44f7d49619), credited there to simnJS/KytyPS5-GTA | Keep image-descriptor scratch slots alive between draws. `spirvEmitterModule.cpp` assigns contiguous per-kind mip ranges and counts samplers; `CommitBindings` overwrites those slots and submits counted writes, not the vector's size. This avoids reinitializing slots after smaller draws, not a GPU allocation or a measured FPS gain. No other hunk from the bundle was imported. |

## Next candidates, in review order (not applied)

| Donor commit | Why consider it | Gate before enabling |
|---|---|---|
| [AnyKyty `6a53916470`](https://github.com/Z0mbieee547/AnyKyty-Yotei-RT/commit/6a53916470c6639894e54bb864f5a60cda24fad2) | Identify the exact surface when tiled layout selection fails. Useful for separating a bad descriptor from an unsupported format. | Diagnostic only. Preserve upstream's extra mip-layout output argument and the existing failure behavior; exercise an invalid layout. |
| [GT7 `4d444955cb`](https://github.com/usa-ratt/KytyPS5-GT7/commit/4d444955cbf95556d7cf9d9eca466b75f98decfc) | Handle pixel shaders whose input-enable and input-address masks differ. The pinned base still stops on unequal masks. | Add shader-output tests for equal masks and for address-only inputs before enabled inputs. Check VGPR numbering and confirm ancillary inputs do not unexpectedly enable sample shading. |
| [GT7 `d7e7cd5fc9`](https://github.com/usa-ratt/KytyPS5-GT7/commit/d7e7cd5fc9e0ac5b22ba5b1822febcca1f7fd1fd) | Avoid sampling the opposite edge of a cube face in skies and reflections. | Trial **off by default**. Face-edge clamping is not seamless cross-face filtering. Compare cube-only, shared cube/2D, and bindless samplers in matched frames. |
| [AnyKyty `30221c5aca`](https://github.com/Z0mbieee547/AnyKyty-Yotei-RT/commit/30221c5aca9c80772b17483ac570afc3106fb2ad) | Compare completed VideoOut flips across pinned builds before calling a render change faster. | This extends an existing donor tool. Verify the base emits compatible logs and count completed flips, not repeated presents. |
| [GT7 `bf336fbf31`](https://github.com/usa-ratt/KytyPS5-GT7/commit/bf336fbf316c5f0e1526cf2d642b05b75dbba512) | Suppress redundant Vulkan state changes and pipeline binds. | Keep opt-in until state invalidation is tested across command buffer resets, raw handles, static/dynamic pipeline switches, and internal draws. |

**Hold back:** AnyKyty's bundled barrier changes and GT7's async buffer-GC downloads are not independent first ports. GT7 [restored conservative barrier defaults](https://github.com/usa-ratt/KytyPS5-GT7/commit/60ab069cf322409069adf07d4f0852d183e9048d); its async GC patch [needed a device-loss follow-up](https://github.com/usa-ratt/KytyPS5-GT7/commit/3aefac0996f8cbcb9b123032d1ed694ab14dea79). Neither donor's title-specific results transfer to this fork without a matched replay.

## Verification before promotion to main

- Build `kyty_emulator` and `gpu_blit_shader_tests` from the same Release configuration. Run the test executable directly (this base defines the target but does not register it with CTest). Its metadata case covers null, unknown size, last valid byte, boundary crossing, address gap, and overflow.
- Replay a descriptor-heavy scene against upstream and the integration branch on the same GPU/driver, game build, save, settings, and shader-cache state. Include both push descriptors and allocated sets where supported, and large/small/zero/large descriptor counts. Compare frames and Vulkan validation output. A CLI startup or a green unit test is not a replay.
- Measure frame times over repeated warmed runs. Keep screenshots, run order, median and p95 frame time, and completed flips. Do not promote on a donor benchmark alone.
