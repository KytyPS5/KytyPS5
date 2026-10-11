param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repo ('_Build\checks\shader-audit-runner-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 6))
}
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Fixture output must be new; existing evidence is preserved.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Get-Item -LiteralPath $OutputDirectory).FullName
$corpus = Join-Path $OutputDirectory 'corpus with spaces'
$audit = Join-Path $OutputDirectory 'audit'
New-Item -ItemType Directory -Path $corpus | Out-Null
$worker = Join-Path $OutputDirectory "worker's fixture.ps1"
@'
$manifest = Get-Content -LiteralPath $args[1] -Raw | ConvertFrom-Json
[Console]::Out.WriteLine('KYTY_SHADER_AUDIT_PHASE decode')
switch ($manifest.fixture) {
    'timeout' {
        # A wrapper's owned child must be stopped with the timed-out wrapper.
        $childInfo = New-Object Diagnostics.ProcessStartInfo
        $childInfo.FileName = (Get-Process -Id $PID).Path
        $childInfo.Arguments = '-NoProfile -EncodedCommand ' + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes('Start-Sleep -Seconds 20'))
        $childInfo.UseShellExecute = $false; $childInfo.CreateNoWindow = $true
        $childInfo.RedirectStandardOutput = $true; $childInfo.RedirectStandardError = $true
        $child = New-Object Diagnostics.Process
        $child.StartInfo = $childInfo
        $null = $child.Start()
        $null = $child.StandardOutput.ReadToEndAsync(); $null = $child.StandardError.ReadToEndAsync()
        [IO.File]::WriteAllText((Join-Path (Split-Path -Parent $args[1]) 'owned-child.pid'), [string]$child.Id)
        [IO.File]::WriteAllText((Join-Path (Split-Path -Parent $args[1]) 'owned-parent.pid'), [string]$PID)
        Start-Sleep -Seconds 20
    }
    'flood' {
        $block = 'x' * 32768
        for ($index = 0; $index -lt 1000; $index++) {
            [Console]::Out.Write($block); [Console]::Error.Write($block)
            Start-Sleep -Milliseconds 3
        }
    }
    'decode' {
        $result = @{ status = 'failed'; phase = 'decode'; unsupported_instructions = @(
            @{ family = 1; opcode_id = 2; reason = 'missing A'; pc = 4; instruction = 'A' },
            @{ family = 1; opcode_id = 3; reason = 'missing B'; pc = 8; instruction = 'B' }) }
        [Console]::Out.WriteLine('KYTY_SHADER_AUDIT_RESULT ' + ($result | ConvertTo-Json -Depth 8 -Compress))
        exit 1
    }
    { $_ -in @('materialize_plan', 'materialize_descriptor') } {
        $reason = if ($manifest.fixture -eq 'materialize_plan') { 'resource plan is incomplete' }
            else { 'runtime descriptor source count does not match resources (unused=1)' }
        [Console]::Out.WriteLine('KYTY_SHADER_AUDIT_PHASE materialize_resources')
        $result = @{ status = 'failed'; phase = 'materialize_resources'; checked_through = 'compute_materialize_resources'; materialization_error = $reason }
        [Console]::Out.WriteLine('KYTY_SHADER_AUDIT_RESULT ' + ($result | ConvertTo-Json -Depth 8 -Compress))
        exit 1
    }
}
[Console]::Out.WriteLine('KYTY_SHADER_AUDIT_RESULT {"status":"passed","phase":"resource_plan","checked_through":"resource_tracking"}')
exit 0
'@ | Set-Content -LiteralPath $worker -Encoding UTF8
[IO.File]::WriteAllBytes((Join-Path $corpus 'same.bin'), [byte[]]@(0, 0, 0, 0))
[IO.File]::WriteAllBytes((Join-Path $corpus 'renamed.bin'), [byte[]]@(0, 0, 0, 0))
[IO.File]::WriteAllBytes((Join-Path $corpus 'back.bin'), [byte[]]@(1, 0, 0, 0))

function New-Manifest {
    param([string]$Name, [string]$Fixture = 'pass', [int]$UserData = 1)
    $value = [ordered]@{ schema_version = 2; kind = 'dispatched'; stage = 'compute'
        code_file = 'same.bin'; code_size_bytes = 4; shader_hash = 'constant hash'
        user_data = @($UserData); host_profile = @{ known = $true }; fixture = $Fixture
        capture_attempt = @{ phase = 'compile_emit'; phase_status = 'pending'; input_id = $Name } }
    $value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $corpus ($Name + '.json')) -Encoding UTF8
    return $value
}
$base = New-Manifest 'base'
$alias = New-Manifest 'alias'
$alias.code_file = 'renamed.bin'
$alias.capture_attempt.phase_status = 'completed'
$alias | ConvertTo-Json -Depth 12 -Compress | Set-Content -LiteralPath (Join-Path $corpus 'alias.json') -Encoding UTF8
$null = New-Manifest 'userdata' -UserData 2
$policy = New-Manifest 'policy'; $policy['captured_compile_policy'] = @{ shader_optimization_type = 2 }
$policy | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $corpus 'policy.json') -Encoding UTF8
$runtime = New-Manifest 'runtime'; $runtime['runtime'] = @{ complete = $true; events = @(@{ words = @(1, $null, 2) }) }
$runtime | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $corpus 'runtime.json') -Encoding UTF8
$back = New-Manifest 'back'; $back['back_code_file'] = 'back.bin'; $back['back_code_size_bytes'] = 4
$back | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $corpus 'back.json') -Encoding UTF8
$null = New-Manifest 'decode' 'decode'
$null = New-Manifest 'materialize-plan' 'materialize_plan'
$null = New-Manifest 'materialize-descriptor' 'materialize_descriptor'
$null = New-Manifest 'timeout' 'timeout'
$null = New-Manifest 'flood' 'flood'
$bad = New-Manifest 'missing'; $bad.code_file = 'missing.bin'
$bad | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $corpus 'missing.json') -Encoding UTF8
'{"schema_version":2,"total":123,"results":[]}' | Set-Content -LiteralPath (Join-Path $corpus 'report.json') -Encoding UTF8

function Assert-True {
    param([bool]$Condition, [string]$Reason)
    if (-not $Condition) { throw $Reason }
}

function Invoke-FixtureAudit {
    param([string]$Name, [string]$Extra = '')
    $shell = (Get-Process -Id $PID).Path
    $runner = Join-Path $repo 'tools\audit-shaders.ps1'
    function Literal([string]$Value) { return "'" + $Value.Replace("'", "''") + "'" }
    $command = '& ' + (Literal $runner) + ' -CorpusDirectory ' + (Literal $corpus) +
        ' -OutputDirectory ' + (Literal $audit) + ' -Auditor ' + (Literal $shell) +
        ' -AuditorArgumentPrefix @(' + (Literal '-NoProfile') + ',' + (Literal '-File') + ',' + (Literal $worker) + ')' +
        ' -Jobs 3 -TimeoutSeconds 3 -MaxOutputMegabytes 1 ' + $Extra + '; if (-not $?) { if ($LASTEXITCODE -eq 1) { exit 1 }; exit 2 }; exit $LASTEXITCODE'
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $shell
    $start.Arguments = '-NoProfile -EncodedCommand ' + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    try {
        $null = $process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $orphanSince = $null
        $orphanFailure = $false
        while (-not $process.HasExited -and $watch.Elapsed.TotalSeconds -lt 30) {
            foreach ($record in @(Get-ChildItem -LiteralPath $audit -Filter 'owned-child.pid' -Recurse -ErrorAction SilentlyContinue)) {
                $parentRecord = Join-Path $record.DirectoryName 'owned-parent.pid'
                if (-not (Test-Path -LiteralPath $parentRecord)) { continue }
                $parentId = [int][IO.File]::ReadAllText($parentRecord)
                $ownedChildId = [int][IO.File]::ReadAllText($record.FullName)
                $parentAlive = Get-Process -Id $parentId -ErrorAction SilentlyContinue
                $childAlive = Get-Process -Id $ownedChildId -ErrorAction SilentlyContinue
                if (-not $parentAlive -and $childAlive) {
                    if ($null -eq $orphanSince) { $orphanSince = [DateTime]::UtcNow }
                    if (([DateTime]::UtcNow - $orphanSince).TotalSeconds -gt 5) {
                        try { $childAlive.Kill(); $null = $childAlive.WaitForExit(5000) } finally { $childAlive.Dispose() }
                        $orphanFailure = $true
                    }
                }
            }
            if ($orphanFailure) { break }
            Start-Sleep -Milliseconds 50
        }
        if (-not $process.WaitForExit(5000)) { $process.Kill($true); $null = $process.WaitForExit(5000); throw 'Fixture runner exceeded its deadline.' }
        $null = [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($stdout, $stderr), 5000)
        $stdout.Result | Set-Content -LiteralPath (Join-Path $OutputDirectory ($Name + '-stdout.txt')) -Encoding UTF8
        $stderr.Result | Set-Content -LiteralPath (Join-Path $OutputDirectory ($Name + '-stderr.txt')) -Encoding UTF8
        Assert-True (-not $orphanFailure) 'Timed-out wrapper left its owned child alive beyond the cleanup deadline.'
        Assert-True ($process.ExitCode -eq 1) ('Expected failed fixture batch, got ' + $process.ExitCode + ': ' + $stderr.Result)
    } finally { if (-not $process.HasExited) { $process.Kill($true); $null = $process.WaitForExit(5000) }; $process.Dispose() }
    return Get-Content -LiteralPath (Join-Path $audit 'report.json') -Raw | ConvertFrom-Json
}

$first = Invoke-FixtureAudit 'first'
Assert-True ($first.captured_manifests -eq 12 -and $first.distinct_compile_inputs -eq 11) 'Hash-only dedup merged distinct inputs or failed to merge a renamed observation-only alias.'
Assert-True ($first.passed -eq 5 -and $first.failed -eq 6 -and $first.input_errors -eq 1) 'Worker/input statuses were mixed.'
Assert-True (@($first.results | Where-Object { $_.phase -eq 'materialize_resources' -and $_.category -eq 'materialize' }).Count -eq 2) 'Materialization error wording overrode its explicit phase.'
Assert-True (@($first.error_groups | Where-Object { $_.reason -like 'materialize:*' }).Count -eq 2) 'Materialization causes were mixed with resource planning.'
Assert-True ($first.ignored_non_shader_json.Count -eq 1) 'Non-shader JSON entered the compiler inventory.'
Assert-True ($first.error_group_memberships -gt $first.failed) 'Overlapping decode causes were lost.'
Assert-True (@($first.results | Where-Object status -eq 'timeout').Count -eq 1) 'Bounded worker timeout was not recorded.'
$timeout = $first.results | Where-Object status -eq 'timeout'
$childPidPath = Join-Path (Split-Path -Parent $timeout.frozen_manifest) 'owned-child.pid'
Assert-True (Test-Path -LiteralPath $childPidPath) 'Timeout fixture did not start its owned child.'
$childId = [int][IO.File]::ReadAllText($childPidPath)
$remaining = Get-Process -Id $childId -ErrorAction SilentlyContinue
if ($remaining) {
    # This PID comes only from the task-owned wrapper's retained artifact.
    try { $remaining.Kill(); $null = $remaining.WaitForExit(5000) } finally { $remaining.Dispose() }
    throw 'Timed-out wrapper left its owned child alive.'
}
Assert-True (@($first.results | Where-Object status -eq 'output_limit').Count -eq 1) 'Bounded output was not recorded.'
$flood = $first.results | Where-Object status -eq 'output_limit'
Assert-True ((Get-Item -LiteralPath $flood.stdout).Length -gt 0 -and (Get-Item -LiteralPath $flood.stderr).Length -gt 0) 'Both streams were not drained.'
$second = Invoke-FixtureAudit 'resume' '-Resume'
Assert-True ($second.reused -eq 10 -and $second.executed -eq 0) 'Resume relaunched completed inputs.'
$third = Invoke-FixtureAudit 'retry' '-Resume -RetryFailed'
Assert-True ($third.reused -eq 5 -and $third.executed -eq 5) 'RetryFailed did not preserve passing inputs and retry worker failures.'
$changed = Get-Content -LiteralPath (Join-Path $corpus 'userdata.json') -Raw | ConvertFrom-Json
$changed.user_data = @(3)
$changed | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $corpus 'userdata.json') -Encoding UTF8
$fourth = Invoke-FixtureAudit 'changed' '-Resume'
Assert-True ($fourth.executed -eq 1 -and $fourth.reused -eq 9) 'Changed userdata reused a stale result or unchanged inputs were relaunched.'
[IO.File]::AppendAllText((Join-Path $audit 'results.jsonl'), '{"audit_key":')
$fifth = Invoke-FixtureAudit 'interrupted-journal' '-Resume'
Assert-True ($fifth.executed -eq 0 -and @(Get-ChildItem -LiteralPath $audit -Filter 'journal-tail-*.txt').Count -eq 1) 'Interrupted journal tail was not preserved and recovered.'
Write-Output ('PASS: dedup of distinct inputs, policy/runtime/back inputs, resume/retry, timeout/output cleanup, non-shader inventory, explicit materialization phase, overlapping causes, journal recovery. Evidence: ' + $OutputDirectory)
