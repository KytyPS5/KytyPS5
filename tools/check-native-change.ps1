param(
    [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Target,
    [string]$TestRegex,
    [string]$HarnessArguments,
    [ValidateSet('red','green')][string]$Phase = 'green',
    [string]$ExpectedFailurePattern,
    [ValidateRange(1,3600)][int]$BuildTimeoutSeconds = 900,
    [ValidateRange(1,600)][int]$TestTimeoutSeconds = 60,
    [switch]$GpuValidation,
    [switch]$BuildOnly,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $repo '_Build\windows'
$wrapper = Join-Path $repo '_Build\windows-local.cmd'
if (-not (Test-Path -LiteralPath $wrapper)) { throw 'Native windows-local.cmd is missing.' }
if (-not $BuildOnly -and ([bool]$TestRegex -eq [bool]$HarnessArguments)) {
    throw 'Select exactly one explicit CTest regex or direct harness mode.'
}
if ($Phase -eq 'red' -and (-not $ExpectedFailurePattern -or $BuildOnly)) {
    throw 'RED requires a test and its intended failure pattern.'
}
if ($TestRegex -match '["\r\n]' -or $wrapper -match '["\r\n]') {
    throw 'Invalid native command argument.'
}
$ctest = Join-Path $env:ProgramFiles 'CMake\bin\ctest.exe'
if ($TestRegex -and -not (Test-Path -LiteralPath $ctest)) {
    $ctest = (Get-Command ctest.exe -ErrorAction Stop).Source
}
$testExe = if ($TestRegex) { $ctest } else { Join-Path $build ($Target + '.exe') }
$testArguments = if ($TestRegex) {
    '--test-dir "' + $build + '" --output-on-failure --no-tests=error --timeout ' +
        $TestTimeoutSeconds + ' -R "' + $TestRegex + '"'
} else { $HarnessArguments }
$buildArguments = '/d /c ""' + $wrapper + '" build-target ' + $Target + '"'
if ($DryRun) {
    [ordered]@{target=$Target;buildCommand=$buildArguments;testExecutable=$testExe;
        testArguments=$testArguments;phase=$Phase;buildOnly=[bool]$BuildOnly;
        gpuValidation=[bool]$GpuValidation;executes=$false} | ConvertTo-Json
    exit 0
}
$conflicts = Get-Process -Name kyty_emulator,shader_cfg_tests,shader_recompiler_compute_tests,
    ninja,MSBuild,clang-cl,cl,link -ErrorAction SilentlyContinue
if ($conflicts) { throw 'An active native game/build/test owns the execution slot.' }
$label = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff') + '-' + $Target
$output = Join-Path $repo ('_Build\checks\' + $label)
$null = New-Item -ItemType Directory -Path $output
$lock = $null
$changedEnvironment = @{}
function Set-RunEnvironment([string]$Name, [string]$Value) {
    if (-not $changedEnvironment.ContainsKey($Name)) {
        $changedEnvironment[$Name] = [Environment]::GetEnvironmentVariable($Name, 'Process')
    }
    [Environment]::SetEnvironmentVariable($Name, $Value, 'Process')
}
function Run-Owned([string]$Executable, [string]$Arguments, [string]$Name, [int]$Timeout) {
    $si = [Diagnostics.ProcessStartInfo]::new()
    $si.FileName=$Executable; $si.Arguments=$Arguments; $si.WorkingDirectory=$repo
    $si.UseShellExecute=$false; $si.CreateNoWindow=$true
    $si.RedirectStandardOutput=$true; $si.RedirectStandardError=$true
    $p=[Diagnostics.Process]::new(); $p.StartInfo=$si
    $r=[ordered]@{executable=$Executable;arguments=$Arguments;
        sha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash;
        startedUtc=[DateTime]::UtcNow.ToString('o');timeoutSeconds=$Timeout;timedOut=$false}
    $stdout=$null; $stderr=$null
    try {
        [void]$p.Start(); $r.pid=$p.Id
        $stdout=$p.StandardOutput.ReadToEndAsync(); $stderr=$p.StandardError.ReadToEndAsync()
        $r.timedOut=-not $p.WaitForExit($Timeout * 1000)
    } finally {
        if ($stdout) {
            if (-not $p.HasExited) {
                # Kill only this owned command tree, including Ninja/CTest children.
                $killInfo=[Diagnostics.ProcessStartInfo]::new()
                $killInfo.FileName=Join-Path $env:SystemRoot 'System32\taskkill.exe'
                $killInfo.Arguments='/PID ' + $p.Id + ' /T /F'
                $killInfo.UseShellExecute=$false; $killInfo.CreateNoWindow=$true
                $killInfo.RedirectStandardOutput=$true; $killInfo.RedirectStandardError=$true
                $killer=[Diagnostics.Process]::Start($killInfo)
                try {
                    $ko=$killer.StandardOutput.ReadToEndAsync(); $ke=$killer.StandardError.ReadToEndAsync()
                    if (-not $killer.WaitForExit(10000)) { $killer.Kill(); [void]$killer.WaitForExit(5000) }
                    if (-not $p.WaitForExit(5000)) { throw 'Owned command did not terminate.' }
                    [void][Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($ko,$ke),5000)
                } finally { $killer.Dispose() }
            }
            $r.exitCode=$p.ExitCode
            if (-not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($stdout,$stderr),5000)) {
                throw 'Owned command streams did not drain.'
            }
            [IO.File]::WriteAllText((Join-Path $output ($Name+'.stdout.txt')),$stdout.Result)
            [IO.File]::WriteAllText((Join-Path $output ($Name+'.stderr.txt')),$stderr.Result)
        }
        $p.Dispose(); $r.finishedUtc=[DateTime]::UtcNow.ToString('o')
        $r | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $output ($Name+'.run.json'))
    }
    return $r
}
try {
    $lock=[IO.File]::Open((Join-Path $repo '_Build\native-check.lock'),
        [IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $revision=(& git -C $repo rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot record source revision.' }
    $built=Run-Owned (Join-Path $env:SystemRoot 'System32\cmd.exe') $buildArguments 'build' $BuildTimeoutSeconds
    if ($built.timedOut -or $built.exitCode -ne 0) { throw 'Build failed; this is not a RED reproduction.' }
    if (-not $BuildOnly) {
        if ($GpuValidation) {
            $validation=Join-Path $repo '_Build\tools\vulkan-validation-ad4ed518'
            if (-not (Test-Path -LiteralPath $validation)) { throw 'Validation layer directory is missing.' }
            $implicit=Join-Path $output 'empty-implicit-layers'
            $null=New-Item -ItemType Directory -Path $implicit
            Set-RunEnvironment 'VK_IMPLICIT_LAYER_PATH' $implicit
            Set-RunEnvironment 'KYTY_TEST_GPU_ASSISTED_VALIDATION' '1'
            Set-RunEnvironment 'VK_LAYER_PATH' $validation
            Set-RunEnvironment 'VK_INSTANCE_LAYERS' 'VK_LAYER_KHRONOS_validation'
            Set-RunEnvironment 'VK_LOADER_DEBUG' 'error,warn'
            Set-RunEnvironment 'VK_LAYER_GPUAV_FORCE_ON_ROBUSTNESS' '0'
            foreach ($name in @('VK_LAYER_ENABLES','VK_LAYER_DISABLES',
                'VK_LOADER_LAYERS_ENABLE','VK_LOADER_LAYERS_DISABLE')) {
                Set-RunEnvironment $name $null
            }
            foreach ($key in @('GPUAV_ENABLE','VALIDATE_CORE','GPUAV_SAFE_MODE',
                'GPUAV_SHADER_INSTRUMENTATION','GPUAV_DESCRIPTOR_CHECKS',
                'GPUAV_BUFFER_ADDRESS_OOB','GPUAV_POST_PROCESS_DESCRIPTOR_INDEXING')) {
                Set-RunEnvironment ('VK_LAYER_'+$key) '1'
            }
        }
        $tested=Run-Owned $testExe $testArguments 'test' $TestTimeoutSeconds
        if ($tested.timedOut) { throw 'Timed out; this is not semantic RED/GREEN evidence.' }
        $text=(Get-Content -Raw -LiteralPath (Join-Path $output 'test.stdout.txt')) +
              (Get-Content -Raw -LiteralPath (Join-Path $output 'test.stderr.txt'))
        $passed=if ($Phase -eq 'red') {
            $tested.exitCode -ne 0 -and $text -match $ExpectedFailurePattern
        } else { $tested.exitCode -eq 0 }
        if (-not $passed) { throw 'Selected checks did not meet the requested RED/GREEN outcome.' }
    }
    [ordered]@{revision=$revision;target=$Target;phase=$Phase;output=$output;
        buildOnly=[bool]$BuildOnly;gpuValidation=[bool]$GpuValidation;
        builtExecutableSha256=$(if (Test-Path -LiteralPath (Join-Path $build ($Target+'.exe'))) {
            (Get-FileHash -LiteralPath (Join-Path $build ($Target+'.exe')) -Algorithm SHA256).Hash
        });result='PASS'} |
        ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $output 'result.json')
    Write-Output ('PASS ' + $Phase + ': ' + $output)
} finally {
    foreach ($name in $changedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name,$changedEnvironment[$name],'Process')
    }
    if ($lock) { $lock.Dispose() }
}
