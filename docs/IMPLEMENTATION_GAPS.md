# Auditoría de rutas stub o sin implementar

Fecha: 2026-08-24
Base revisada: `6eed46b`

## Actualización 2026-08-24

Estado actual de la rama `gods-will/runtime-compatibility`:

- **Imports opcionales:** los 61 imports observados de NP, Commerce, Entitlements, Manager,
  Signaling, Trophy, WebApi y JSON tienen resolución local. Las rutas online conservan estado
  determinista y permanecen desconectadas de la red oficial.
- **GPU:** la matriz cubre 509 opcodes; los 61 ALU pendientes ya tienen lowering y regresiones.
  No quedan pendientes en ALU, float, memory, image o graphics de esa matriz. `S_SETPC_B64`
  cubre destinos estáticos, dinámicos y tablas de salto; PM4 indirecto cubre CX, SH y UC.
- **Audio:** ACM incluye FFT/IFFT fp32/fp16, panner y convolution reverb por overlap-add;
  Audio3D mezcla beds/objetos y entrega PCM a SDL; NGS2 reproduce PCM y VAG/HE-VAG en racks
  sampler estándar y custom. AJM incluye LPCM (23) y HE-VAG (22) nativo.
- **Servicios offline:** RUDP implementa contextos, bind, pairing local, mensajes, polling y
  opciones sin PSN; PSML mantiene objetos, dispatch, progreso y solicitud de captura;
  TextToSpeech2 mantiene el ABI observado de estado/cancelación; CES mantiene sus dos contextos
  y la conversión UTF-8/CP1252. Privacy devuelve ahora `OPERATION_NOT_SUPPORTED` en vez de un
  éxito ficticio.
- **Loader:** `--unresolved-import-report <ruta.json>` genera automáticamente un inventario
  determinista, ordenado y agrupado por programa, NID, símbolo, tipo y binding. Puede combinarse
  con `--strict-unresolved-imports` para identificar el primer import realmente ejecutado.
- **UserService:** `--user-id` y `--user-name` configuran la identidad local; las rutas de usuario
  inicial, lista, evento, nombre y preferencias usan la misma identidad y devuelven errores ABI
  en argumentos inválidos.
- **Pruebas:** pasan `shader_recompiler_compute_tests.exe`, `shader_cfg_tests.exe` y
  `kernel_file_system_tests.exe`; `kyty_emulator.exe` compila y se instala.
- **Castlevania Dominus Collection 01.003:** llega al título y entra a Dawn of Sorrow a 60 FPS
  sin repetir el antiguo `0xc0000005`. La música del título usa
  VAGp HE-VAG estéreo a 44.1 kHz; `sceNgs2ParseWaveformData` devuelve ahora tipo `0x1c`, dos
  bloques y el bucle embebido, donde antes devolvía formato cero y ningún bloque. Evidencia:
  `_TempData/castlevania-hevag-guest.log`. La ruta PCM continua usada dentro del juego conserva
  ahora la voz al esperar datos y recicla los bloques ya consumidos; el usuario confirmó música
  audible en gameplay además del título y los efectos. Falta una partida prolongada.

Audio y servicios se contrastaron explícitamente con SDK 10, porque el índice local disponible
de SDK 12 no contiene esos contratos. Archivo consultado:
`F:\Downloads\SDK MANAGER 10.00\SDK MANAGER 10.00\InstallFiles\49f9e27e27d6e2f894c60f5acd3fd3f6\SDK-10_00_00_40-00_00_00_0_1.zip`.
Se revisaron `audio3d.h`, ACM FFT/panner/conv-reverb, NGS2, `ajm/lpcm_decoder.h`, `rudp.h`,
Share, PSML, TextToSpeech2 y CES.

## Estado de remediación

Implementado y verificado en la revisión posterior a la auditoría:

- **GPU / PM4:** AA sample locations y depth target ya aceptan paquetes independientes;
  `COPY_DATA` admite inmediatos de 64 bits sin perder el DWORD alto.
- **GPU / shaders (SDK 12):** `V_MOVRELS_B32` usa VGPR0 fuera de rango y acepta
  modificadores de fuente; `DS_SWIZZLE_B32` implementa FFT bit-reverse; FLAT acepta
  GLC/SLC/DLC como políticas de caché.
- **Loader:** `--strict-unresolved-imports` detiene la ejecución en el primer thunk llamado,
  mostrando símbolo y programa. `--unresolved-import-report` conserva además el inventario JSON
  completo por programa/NID, incluyendo el número de sitios de relocalización.
- **AudioIn:** captura real mediante SDL, escritura completa del buffer y silencio seguro cuando
  no existe dispositivo.
- **Sistema:** Sysmodule mantiene referencias de carga; las entradas exportadas de RUDP validan
  inicialización/hilo/callback; Share conserva configuración y devuelve error explícito para
  captura no disponible. UserService acepta una identidad local configurable y Privacy devuelve
  explícitamente que la operación no está soportada.
- **POSIX / red (SDK 10):** `rename`, `rmdir`, `fchmod`, `futimes`, `utimes`, `shutdown`,
  `getpeername`, `sendmsg` y `recvmsg` tienen implementación host. libSceNet enlaza además
  `connect`, send/recv, `getpeername`, `getsockopt` y el ciclo de vida/resolución Aton.
- **Loader / títulos reales (prueba histórica):** siete títulos encontrados en
  `F:\APPs_IA\SharpEMU\Game` superaron el arranque con `--strict-unresolved-imports`.
  En The Messenger, los 12 imports sin resolver del runtime IL2CPP ahora enlazan a funciones
  reales; el módulo opcional `libSceNpToolkit2.prx` conserva 61 imports de servicios online.
- **Pruebas:** regresiones GPU y de estado Sysmodule/RUDP añadidas. El ejecutable del emulador y
  las suites `kernel_file_system_tests` y `shader_recompiler_compute_tests` compilan y pasan. La
  suite completa valida también el reporte JSON, UserService, Privacy, Share, PSML, CES, TTS,
  Audio3D, NGS2 y AJM.

Fuentes SDK 12 usadas para GPU:

- `F:\Downloads\references\GPU Shader Core ISA Instruction Reference � SDK 12.000\0111.pdf`,
  páginas 2-3 (`V_MOVRELD_B32`/`V_MOVRELS_B32`).
- `F:\Downloads\references\GPU Shader Core ISA Specification � SDK 12.000\56.pdf`, página 8
  (FFT swizzle).
- `F:\Downloads\references\GPU Shader Core ISA Specification � SDK 12.000\71.pdf`, página 3
  (GLC/SLC/DLC).

El índice local de SDK 12 no contiene contratos de Audio3D, NGS2, ACM, Sysmodule, RUDP, Share,
PSML, TextToSpeech2, Privacy ni write throttling. Esas rutas no deben completarse inventando ABI
o efectos.

Fuentes SDK 10 usadas para POSIX y red:

- `sdk/target/include/libnet/resolver.h` para `sceNetResolverDestroy` y
  `sceNetResolverStartAton`.
- `sdk/target/include/sys/socket.h`, `sys/time.h` y `sys/stat.h` para sockets, `msghdr`,
  tiempos y permisos. Estas referencias son explícitamente SDK 10, no evidencia SDK 12.

## Veredicto

La afirmación queda **confirmada**, con un matiz: muchas apariciones de
`EXIT_NOT_IMPLEMENTED` son validaciones de argumentos o invariantes, no funciones vacías.
Los faltantes reales se concentran en GPU; loader, audio y varias librerías del sistema
tienen rutas funcionales, pero también fallbacks, simulaciones y no-ops verificables.

Como señal histórica de alcance, en la base auditada había 454 referencias a `EXIT_NOT_IMPLEMENTED` o
`KYTY_NOT_IMPLEMENTED` en `src/graphics`, y 469 en `src/libs`, `src/loader` y
`src/kernel`. Estas cifras no son un conteo de tareas: incluyen comprobaciones defensivas.
El decodificador de shaders contiene además 48 referencias a `SetUnsupported` o
`MarkMemoryUnsupported`.

La regresión actual cubre 509 opcodes del decodificador; quedan 0 pendientes en las categorías
ALU, float, memory, image o graphics de esa prueba.

## Evidencia confirmada

La tabla conserva el diagnóstico histórico de la base `6eed46b`; los puntos corregidos se indican
en **Estado de remediación**.

| Área | Ruta incompleta | Efecto |
|---|---|---|
| GPU / PM4 | Formatos no reconocidos de AA y depth target (`pm4Handlers.cpp:261`, `:718`, `:721`), registros/selectores no soportados (`:1355`, `:1678`, `:1900`, `:1916`, `:1941`, `:1950`, `:2449`, `:2514`, `:2868`) | Detiene la emulación al encontrar paquetes válidos aún no cubiertos. |
| GPU / ejecución | Operaciones `wait_reg_mem`, destinos de `writeData` y variantes DMA/GDS no soportadas (`graphicsRun.cpp:341`, `:366`, `:422`, `:428`, `:439`, `:442`) | Bloqueo durante la ejecución de command buffers. |
| GPU / tiling | Tamaños de elemento y layouts 3D no cubiertos (`tile.cpp:492`, `:561`, `:742`, `:1324`) | Texturas o render targets afectados no se pueden mapear. |
| GPU / shaders | Fallbacks explícitos para SMEM, MUBUF, MTBUF, FLAT y DS (`MemoryOps.cpp:236`, `:271`, `:309`, `:347`, `:351`, `:389`), MIMG (`ImageOps.cpp:346`) y familias escalares/vectoriales | El shader se rechaza o falla su recompilación. |
| GPU / control de shader | Export targets, instrucciones y CFG no soportados (`ShaderIR.cpp:921`, `:1102`; `ShaderCFG.cpp:1909`, `:1940`) | No hay lowering seguro para esos shaders. |
| Loader | Imports débiles/no resueltos se parchean a thunks (`runtimeLinker.cpp:119`, `:295`, `:996`, `:1000`) y, si no se resuelven tarde, retornan cero (`:300-341`) | Puede ocultar una API faltante y provocar fallos posteriores por estado o efectos secundarios ausentes. |
| Audio | `AudioInInput` solo espera y devuelve el número de muestras (`audio.cpp:590-607`) | No captura ni escribe audio en el buffer destino. |
| Audio3D | El hilo de reproducción solo simula el retardo (`audio.cpp:1095-1129`) | Consume la cola sin producir sonido. |
| NGS2 / AJM | Módulos custom NGS2 siguen stub (`audio.cpp:1929-1930`) y existe un fallback de voz (`:2614-2621`). AJM solo admite MP3, ATRAC9, AAC, Opus y el ABI desconocido mapeado (`ajm.cpp:496-535`) | Compatibilidad parcial con motores y codecs de audio. |
| Sistema | TextToSpeech2 es no-op (`libTextToSpeech2.cpp:12-21`); RUDP no implementa transporte ni hilo (`libRudp.cpp:22-54`); Sysmodule no mantiene estado (`libSysmodule.cpp:25-62`) | Devuelven éxito sin realizar la operación esperada. |
| Servicios | Share no soporta captura (`libShare.cpp:37-108`) y varios setters/callbacks son no-op; PSML valida pero no ejecuta dispatch/capture y reporta progreso fijo (`libPsml.cpp:262-310`) | La llamada parece aceptada, pero no hay comportamiento funcional. |
| Stubs aislados | UserService privacy (`libUserService.cpp:252-254`), dos NID de CES (`libNet.cpp:3131-3144`) y write throttling (`libKernel.cpp:1845-1850`) | Semántica ausente para esas entradas. |

La salida básica `AudioOut` **no es un stub**: abre un dispositivo SDL, convierte formatos
y encola audio (`audio.cpp:222-251`, `:327-391`). El loader tampoco está vacío; el problema
concreto es su fallback genérico para imports no resueltos.

## Pendientes priorizados actuales

1. **Castlevania:** completar una partida prolongada. El título, la selección y Dawn of Sorrow
   mantienen audio y 60 FPS; no reapareció el antiguo `0xc0000005` durante esta prueba.
2. **Partidas completas:** ejecutar recorridos jugables prolongados en los diez títulos de
   `F:\APPs_IA\SharpEMU\Game`; llegar al título no equivale a completar una partida.
3. **GPU no reproducida:** ampliar PM4, ISA, exports, layouts, mipmaps, arrays y texturas 3D solo
   cuando una traza válida revele un caso nuevo; la matriz actual no tiene opcodes pendientes.
4. **Audio avanzado:** validar Audio3D con un título real y ampliar módulos custom NGS2 solo al
   aparecer controles no cubiertos en trazas. Convolution reverb y HE-VAG ya están implementados.
5. **Codecs:** CELP8/CELP16 siguen pendientes: los perfiles MPE/RPE son propietarios y no existe
   un decodificador verificable en las dependencias actuales. No se añadirá audio ficticio.
6. **Servicios locales:** Share/PSML no tienen todavía una fuente de framebuffer/codificador para
   crear artefactos de captura; TextToSpeech2 no ha expuesto en trazas una ABI de síntesis, solo
   estado/cancelación. La semántica de write throttling tampoco está documentada en el corpus
   local. RUDP local ya está implementado. Nunca se conectarán a la red oficial.

No se considera completo un servicio que solo conserva estado si el juego necesita su efecto
externo. Los fallbacks offline permiten ejecución segura y revelan el siguiente bloqueo.
