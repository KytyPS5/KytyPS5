$ErrorActionPreference = "Stop"

$gameDir = "C:\Users\Recko\Downloads\PPSA21567\PPSA21567-app"
$backportDir = "C:\Users\Recko\Downloads\PPSA21567\backport (4.xx to 8.xx)"
$emuExe = "C:\Users\Recko\Documents\github\kyty\_Build\windows\kyty_emulator.exe"

Write-Host "=========================================" -ForegroundColor Cyan
Write-Host "   Iniciando ASTRO BOT en KytyPS5        " -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

# Comprobar si WinRAR sigue extrayendo
$winrar = Get-Process WinRAR -ErrorAction SilentlyContinue
if ($winrar) {
    Write-Warning "WinRAR todavía está extrayendo los archivos del juego. Espera a que finalice para evitar archivos incompletos o corruptos."
}

# Si falta eboot.bin en la carpeta principal de la app, copiarlo desde backport
if (-not (Test-Path "$gameDir\eboot.bin")) {
    if (Test-Path "$backportDir\eboot.bin") {
        Write-Host "Copiando eboot.bin del backport a la carpeta del juego..." -ForegroundColor Yellow
        Copy-Item "$backportDir\eboot.bin" -Destination "$gameDir\eboot.bin" -Force
        if (Test-Path "$backportDir\sce_module") {
            Copy-Item "$backportDir\sce_module\*" -Destination "$gameDir\sce_module\" -Recurse -Force -ErrorAction SilentlyContinue
        }
    } else {
        Write-Error "No se encontró eboot.bin en $gameDir ni en $backportDir."
        exit 1
    }
}

Write-Host "Ejecutable: $emuExe" -ForegroundColor Green
Write-Host "Carpeta de juego: $gameDir" -ForegroundColor Green
Write-Host "Parámetros activos: --shader-cache, --audio-compile-hold, --perf-overlay" -ForegroundColor Green
Write-Host ""

& $emuExe --game "$gameDir" --shader-cache --audio-compile-hold --perf-overlay
