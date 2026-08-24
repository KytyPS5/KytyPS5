# Auditoría de rutas stub o sin implementar

Fecha: 2026-08-23
Base revisada: `6eed46b`

## Estado de remediación

Implementado y verificado en la revisión posterior a la auditoría:

- **GPU / PM4:** AA sample locations y depth target ya aceptan paquetes independientes;
  `COPY_DATA` admite inmediatos de 64 bits sin perder el DWORD alto.
- **GPU / shaders (SDK 12):** `V_MOVRELS_B32` usa VGPR0 fuera de rango y acepta
  modificadores de fuente; `DS_SWIZZLE_B32` implementa FFT bit-reverse; FLAT acepta
  GLC/SLC/DLC como políticas de caché.
- **Loader:** `--strict-unresolved-imports` detiene la ejecución en el primer thunk llamado,
  mostrando símbolo y programa.
- **AudioIn:** captura real mediante SDL, escritura completa del buffer y silencio seguro cuando
  no existe dispositivo.
- **Sistema:** Sysmodule mantiene referencias de carga; las entradas exportadas de RUDP validan
  inicialización/hilo/callback; Share conserva configuración y devuelve error explícito para
  captura no disponible.
- **POSIX / red (SDK 10):** `rename`, `rmdir`, `fchmod`, `futimes`, `utimes`, `shutdown`,
  `getpeername`, `sendmsg` y `recvmsg` tienen implementación host. libSceNet enlaza además
  `connect`, send/recv, `getpeername`, `getsockopt` y el ciclo de vida/resolución Aton.
- **Loader / títulos reales:** los siete títulos encontrados en
  `F:\APPs\_IA\SharpEMU\Game` superaron el arranque con `--strict-unresolved-imports`.
  En The Messenger, los 12 imports sin resolver del runtime IL2CPP ahora enlazan a funciones
  reales; el módulo opcional `libSceNpToolkit2.prx` conserva 61 imports de servicios online.
- **Pruebas:** regresiones GPU y de estado Sysmodule/RUDP añadidas. El ejecutable del emulador y
  las suites `kernel_file_system_tests` y `shader_recompiler_compute_tests` compilan y pasan.

Fuentes SDK 12 usadas para GPU:

- `F:\Downloads\references\GPU Shader Core ISA Instruction Reference � SDK 12.000\0111.pdf`,
  páginas 2-3 (`V_MOVRELD_B32`/`V_MOVRELS_B32`).
- `F:\Downloads\references\GPU Shader Core ISA Specification � SDK 12.000\56.pdf`, página 8
  (FFT swizzle).
- `F:\Downloads\references\GPU Shader Core ISA Specification � SDK 12.000\71.pdf`, página 3
  (GLC/SLC/DLC).

El índice local de SDK 12 no contiene contratos de Audio3D, NGS2, ACM, Sysmodule, RUDP, Share ni
PSML. Esas rutas no deben completarse inventando ABI o efectos.

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

La regresión actual cubre 448 opcodes del decodificador; quedan 61 opcodes ALU marcados como
pendientes y ninguno pendiente en las categorías float, memory, image o graphics de esa prueba.

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

## Pendientes priorizados

1. **P0 — títulos reales:** ampliar las pruebas más allá del arranque y convertir cada import
   realmente llamado en un caso reproducible. Los 61 imports opcionales observados pertenecen
   principalmente a NP Utility/Commerce/Entitlements/Manager/Signaling/Trophy/WebApi y JSON.
2. **P0 — GPU:** implementar y probar los 61 opcodes ALU aún pendientes; además permanecen
   selectores PM4, `S_SETPC_B64` dinámico y casos sin contrato/captura reproducible.
3. **P0 — audio avanzado:** aportar contratos y cargas reales para implementar Audio3D, DSP de
   ACM/NGS2, módulos custom y codecs adicionales.
4. **P1 — servicios:** definir backend y ABI para RUDP de transporte, Share/PSML de captura,
   TextToSpeech2, privacy, CES y write throttling.
5. **P1 — configuración:** sustituir el usuario único de UserService por perfiles configurables.
6. **P1 — pruebas de integración:** añadir un título o traza redistribuible que cubra loader,
   AudioIn/Audio3D y servicios en runtime.

Prioridad propuesta: primero obtener trazas de títulos reales y convertir cada bloqueo en un
caso reproducible; después implementar por frecuencia e impacto, no por cantidad de macros.
