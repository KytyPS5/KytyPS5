# Shader capture fixtures

Tiny `--shader-replay` captures used by the `shader_*` ctests. Each directory has the layout the
emulator writes with `--shader-capture-dir` (see `src/graphics/shader/shaderCapture.h`) and is
assembled from the `.s` file next to it:

```
tools/shader/asm2capture.py tests/data/shader_capture/store_ok.s \
    tests/data/shader_capture/store_ok --user-data 0x10000000,0x0,0x100,0x30000000 --hash 0xa1
```

| fixture | expected replay |
|---|---|
| `store_ok` | `REPLAY OK` |
| `gpu_selected_store` | known failure: `GPU-selected access requires a raw DWORD x2/x3/x4 load` (exit 65) |

The tests copy these into the build directory first, because replaying writes `replay.log` and
`out.spv` next to the capture.
