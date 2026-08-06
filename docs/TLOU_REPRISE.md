# TLOU Part I — Guide de reprise propre sur base Kyty

Guide pour rejouer le travail **Pouare514** sur une base [`upstream/main`](https://github.com/KytyPS5/KytyPS5) à jour, sans cherry-pick massif ni soft-HLE hardcodé actif par défaut.

**Jeu cible :** The Last of Us Part I (`PPSA03396`)

---

## 1. État des lieux

| Réf. | Hash / valeur |
|------|----------------|
| Branche travail | `Pouare` |
| Tip perso | `6164fef` (`enhance x64InstructionEmulator logging`) |
| Merge-base avec upstream | `8587638` (gpuTiler, ~22 juil. 2026) |
| `upstream/main` (au moment du guide) | `869329e` |
| Divergence | **~131 derrière / ~43 devant** |

Remotes :

- `origin` → `Pouare514/KytyPS5`
- `upstream` → `KytyPS5/KytyPS5` (auteur principal : nmzik)

### Conflits attendus (fichiers chauds)

| Fichier | Pourquoi |
|---------|----------|
| `src/graphics/presentation/videoOut.cpp` | +3k / +6k L sur `65cc9d2` / `b1d07e8` ; phases P44–P71 |
| `src/kernel/memory.cpp` | chaîne ProcParam / flexible ; upstream a rework mémoire |
| `src/libs/agc.cpp` | soft-HLE submit / EQ |
| `src/kernel/pthread.cpp` / `semaphore.cpp` | waits menu, FlipStats, soft-HLE |
| `src/graphics/host_gpu/renderer/textureCache.cpp` | `MarkGpuWritten` VideoOut |
| `src/loader/runtimeLinker.cpp` | boot mémoire + null-page |

**Règle :** ne pas cherry-pick `videoOut.cpp` en bloc. Reposer des patchs minimaux couche par couche.

---

## 2. Inventaire commits perso (par thème)

~35 commits non-merge `Pouare514` sur `Pouare` (hors merges upstream `52ed31b` / `902110a` / `493ba9a`).

### 2.1 Infra Windows / Vulkan / CI — générique, faible risque

| Hash | Intention |
|------|-----------|
| `8966ea4` | Windows CI, compat-db, stubs / roadmap |
| `e036119` | Chemins / build config |
| `50d86bb` / `4913731` | Cleanup PR md, Vulkan SDK 1.4.350, README |
| `c2a97cf` / `561c067` | Workaround layers Vulkan |
| `4974469` | SDK CI / README |
| `062b3fb` / `9b3abc0` | Shader recompiler, push constants, tests |

**Fichiers :** `.github/workflows/*`, `README.md`, `scripts/build-windows.cmd`, `vulkanWindow.cpp`, shader recompiler.

**Rejouer :** uniquement ce qui manque encore côté upstream (layers README, CI Windows). Skip si déjà absorbé.

**Ne pas copier tel quel :** bumps Vulkan SDK / chemins machine-spécifiques.

### 2.2 Crash diagnostics / exceptions — générique

| Hash | Intention |
|------|-----------|
| `d1bfef6` | Logging exceptions Windows fatales |
| `e84f4db` | Crash diag + sélection GPU |
| `65cc9d2` (partie diag) | `crashDiagnostics`, `hostException`, sync |
| `82d19c4` / `445a371` / `3690c73` | Exceptions + ffmpeg-core + diag |
| `e18bdf3` | Crash diag + pthread + x64 emu |
| `2c1df81` (partie diag) | Soft-idle exit codes, null-page AV |

**Fichiers :** `crashDiagnostics.*`, `hostException.cpp`, `main.cpp`.

**Rejouer :** diag Windows / dump utile pour smoke TLOU.

**Ne pas copier tel quel :** hooks phase-spécifiques mélangés dans les mêmes commits que le diag.

### 2.3 Mémoire (flexible, ProcParam, modules) — générique, revalider vs upstream

| Hash | Intention |
|------|-----------|
| `2a3afaf` | Flexible memory + synthèse ProcParam |
| `9ce4b7f` | `ApplyMemoryRegionsFromProcParam` |
| `2375ece` | Registration mémoire modules système |
| `794b9ae` | Flexible + direct memory init |
| `5308083` | Data ranges + memory trace |
| `65cc9d2` / `2c1df81` (parties memory) | Ranges, soft-idle, null-page |

**Fichiers :** `memory.cpp` / `.h`, `runtimeLinker.cpp`, `memoryAddressSpace.inc`, `libKernel.cpp`.

**Rejouer :** comportement boot (ProcParam → régions, modules système) **après** lecture du rework mémoire upstream.

**Ne pas copier tel quel :** gros diff `memory.cpp` sans diff vs tip upstream.

### 2.4 VideoOut / Flip / Phase 44 — mixte, gros diff

| Hash | Intention |
|------|-----------|
| `65cc9d2` | Socle VideoOut (+~3.3k L), AGC, pthread, sync |
| `08e4f98` | Phase 44 ABI re-Register VideoOut |
| `b1d07e8` | Sync / flip, VideoOut (+~6.6k L), harnais `_validate_phase.ps1` |
| `2c1df81` | Upload / register VideoOut, soft-idle |
| `fe20fd6` | FlipStats CPU/GPU/presented |

**Fichiers :** `videoOut.cpp` / `.h`, `window.cpp`, `swapchain.cpp`, `eventQueue.cpp`, `agc.cpp`.

**Rejouer :** Register / Unreg / Flip / FlipStats en patchs **petits** et testés.

**Ne pas copier tel quel :** monolithe VideoOut des commits `65cc9d2` / `b1d07e8`.

### 2.5 Soft-HLE / phases 66–71 — TLOU-spécifique

| Hash | Intention |
|------|-----------|
| `caa8b11` | Phases 68–71 (submit-state, NdJob, CTX_FIELD) |
| `fe20fd6` / `ee76c55` | Soft-HLE submit + semaphores / pthread / x64 |
| docs `TLOU_AGC_P60`…`P71` | Causes figées + gates env |

**Gates env (exemples) :** `KYTY_PHASE66_MENU_RECYCLE`, `KYTY_PHASE68_SHARPEMU_WAIT`, `KYTY_PHASE69_NDJOB_READY`, `KYTY_PHASE70_NDJOB_FIELD`, `KYTY_PHASE71_CTX_FIELD`.

**Fichiers :** `videoOut.cpp`, `graphicsRun.cpp`, `agc.cpp`, `semaphore.cpp`, `pthread.cpp`, `_validate_phase.ps1`.

**Rejouer :** **en dernier**, toujours derrière flags env, jamais ON par défaut.

**Ne pas copier tel quel :** VA guest hardcodées (`0x905f254c0`, `0x905f25cd0`, NdJob `0x903420b00` / `0x90386ed80`, etc.) sans revalidation run.

### 2.6 GPU (depth bias, scratch, textures) — générique utile

| Hash | Intention |
|------|-----------|
| `4499f38` | Depth bias pipeline |
| `38e4002` | Pipeline Vulkan + textures / SPIR-V image ops |
| `4dc2749` | Scratch ring buffer + shader resources |
| `b1df52d` (partie GPU) | `TextureCache::MarkGpuWritten` pour images VideoOut |

**Fichiers :** `pm4Handlers.cpp`, `pipelineCache.*`, `scratchRingBuffer.*`, `textureCache.cpp`, spirvEmitter*.

**Rejouer :** depth bias, scratch ring, `MarkGpuWritten` VideoOut (évite exit intempestif).

**Ne pas copier tel quel :** parties audio/x64 du même commit `b1df52d` sans besoin.

### 2.7 Audio NGS2 — générique (upstream a déjà pacing)

| Hash | Intention |
|------|-----------|
| `b1df52d` (partie audio) | +~856 L `audio.cpp` (waveform / decode) |
| `0550c43` | `Ngs2VoiceParamHeader` + micro-fix VideoOut CommandBuffer |

**Rejouer :** header NGS2 + fixes minimaux si boot audio casse.

**Ne pas copier tel quel :** gros bloc audio si upstream `fix AudioOut pacing` couvre déjà le besoin.

### 2.8 Threading / x64 emulator logging — générique / debug

| Hash | Intention |
|------|-----------|
| `4f18e81` | Font layout + UserService errors |
| `123fffd` | Mutex / threading + x64 emu |
| `6164fef` | Logging x64InstructionEmulator |

**Rejouer :** si utile au debug boot ; UserService/font si crash early.

**Ne pas copier tel quel :** spam log x64 en prod.

### 2.9 Hors thème / merges à ignorer au port

- Merges : `52ed31b`, `902110a`, `493ba9a`, merges locaux `main`.
- `ffmpeg-core` submodule bumps (`445a371`, `3690c73`, `b1d07e8`) : aligner sur tip upstream, ne pas forcer un SHA perso.

---

## 3. Campagne phases TLOU (P60 → P71)

Annexes détaillées : [`TLOU_AGC_P60.md`](TLOU_AGC_P60.md) … [`TLOU_AGC_P71.md`](TLOU_AGC_P71.md).  
Harnais : [`_validate_phase.ps1`](../_validate_phase.ps1), [`_validate_p59.cmd`](../_validate_p59.cmd).  
Scratch ABI : [`_p69_jb_abi_scratch.txt`](_p69_jb_abi_scratch.txt).

| Phase | Cause figée | Gate / note |
|-------|-------------|-------------|
| P60 | `still_need_external_docs` | Modèle AGC conceptuel (SharpEmu vs Kyty) |
| P61 | `still_opaque_ring` | Probe ring PM4 post-Unreg |
| P62 | `producer_pre_unreg_only` | Silence producteur post-Unreg |
| P63 | `producer_never_armed` | Forensics Unreg ; `guest_real=0` |
| P64 | `still_opaque_wait` | Waiters post-Unreg (cond / Flip / NdJob) |
| P65 | `waits_non_main_dominant` | Mixed/Compute dominent ; Reg2 + guest Flip OK |
| P66 | `menu_recycle_active` | `KYTY_PHASE66_MENU_RECYCLE=1` — recycle Flip L0 (écran noir) |
| P67 | `sharpemu_submit_wait_delta` | Doc delta SharpEmu submit-state / WAIT |
| P68 | `submit_state_only` | `KYTY_PHASE68_SHARPEMU_WAIT=1` — state OK, pas de guest Submit |
| P69 | `ndjob_hle_no_submit` (cible) | `KYTY_PHASE69_NDJOB_READY=1` |
| P70 | `field_still_static` / suite | `KYTY_PHASE70_NDJOB_FIELD=1` |
| P71 | `field_hle_no_submit` | `KYTY_PHASE71_CTX_FIELD=1` — `*(u32*)(0x905f254c0+0x24)` one-shot ≠ gate producteur |

**État net :** Flip path (Reg2 + SubmitFlip) marche ; producteur AGC guest post-Unreg **jamais armé**. Soft-HLE Mixed/CTX_FIELD n’a pas débloqué `guest_real`.

### Ancres VA — ne pas porter aveuglément

| VA | Rôle (campagne) |
|----|-----------------|
| `0x905f25cd0` | Ban LIST (`kPhase56BannedBase`) — **interdit** |
| `0x905f254c0` | Base CTX Mixed ; champ HLE `+0x24` (P71) |
| `0x903420b00` | NdJob ctrl |
| `0x90386ed80` | NdJob status |
| `0x1000000000` | User ring (hypothèse P59/P60) |
| `0x904bb6de8` | Slot predicate Mixed (P70) |

Revalider ces adresses sur **chaque** build eboot / run avant de réactiver un HLE.

---

## 4. Ordre de re-port recommandé

```text
upstream/main propre
  → 1. Diag crash + Vulkan Windows
  → 2. Mémoire ProcParam / flexible (après lecture upstream)
  → 3. GPU depth bias / scratch / MarkGpuWritten
  → 4. Audio NGS2 minimal
  → 5. VideoOut Register / Flip / FlipStats (patchs petits)
  → 6. Soft-HLE phase-gated (dernier)
  → 7. Harnais _validate_phase.ps1 + smoke menu / Unreg
```

### Checklist

- [ ] Checkout / reset sur `upstream/main` propre (nouvelle branche `tlou-reprise` ou équivalent)
- [ ] Couche 1 : crash diag + layers Vulkan — smoke boot
- [ ] Couche 2 : mémoire — comparer avec rework upstream avant port
- [ ] Couche 3 : GPU générique (depth bias, scratch, MarkGpuWritten)
- [ ] Couche 4 : audio NGS2 si nécessaire (sinon skip)
- [ ] Couche 5 : VideoOut minimal (Register / Unreg / Flip / FlipStats) — **pas** le monolithe
- [ ] Couche 6 : soft-HLE derrière `KYTY_PHASE*` uniquement
- [ ] Couche 7 : `.\ _validate_phase.ps1 -Phase N` ; confirmer menu / Unreg / `guest_real`

### Règles figées

1. **Pas de cherry-pick massif** de `videoOut.cpp`.
2. Soft-HLE **toujours** derrière env / `KYTY_PHASE*`, OFF par défaut.
3. VA guest = constantes documentées à **revalider**, pas copier-coller aveugle.
4. Après **chaque** couche : smoke boot TLOU (au moins jusqu’à Unreg / menu) avant d’empiler la suivante.
5. Préférer patchs nouveaux sur tip upstream plutôt que replay commit-à-commit des messages verbeux.

---

## 5. Fichiers / outils à conserver

| Élément | Chemin | Note |
|---------|--------|------|
| Guide master | `docs/TLOU_REPRISE.md` | ce fichier (versionné) |
| Journaux phases | `docs/TLOU_AGC_P60.md` … `P71.md` | versionnés (plus dans `.gitignore`) |
| Scratch ABI P69 | `docs/_p69_jb_abi_scratch.txt` | versionné |
| Harnais | `_validate_phase.ps1`, `_validate_p59.cmd` | versionnés |
| Logs runs | `_kyty_tlou_*`, verdicts JSON | **locaux** (gitignore `/_kyty*`) |
| Scripts build phase | `_build_p*` | locaux (gitignore `/_build_p*`) |

---

## 6. Hors scope

- Pas de rebase/cherry-pick automatique décrit ici.
- Pas de fiche `compat-db` TLOU (DB vide côté perso).
- Les commits « boot astrobot » (`2c1df81`) restent une note de stabilité soft-idle, pas un port AstroBot.
- P72+ non démarré : callee / autre offset Mixed (pas retry P70/P71).
