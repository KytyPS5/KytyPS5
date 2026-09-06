param(
    [Parameter(Mandatory = $true)][string]$CorpusDirectory,
    [string]$Auditor,
    [string]$OutputDirectory,
    [ValidateRange(1, 16)][int]$Jobs = 4,
    [ValidateRange(1, 300)][int]$TimeoutSeconds = 30
)
$ErrorActionPreference = 'Stop'

function Stop-AuditProcess {
    param([Diagnostics.Process]$Process)
    $workerId = 'unknown'
    try { $workerId = $Process.Id } catch { }
    try {
        if (-not $Process.HasExited) {
            try { $Process.Kill() }
            catch {
                # The worker can exit between HasExited and Kill.
                $Process.Refresh()
                if (-not $Process.HasExited) { throw }
            }
        }
        return $Process.WaitForExit(5000)
    } catch {
        Write-Warning ('Could not confirm worker termination: PID {0}: {1}' -f $workerId, $_.Exception.Message)
        return $false
    }
}

function Get-UnsupportedReason {
    param($Instruction)
    return ('family={0} opcode=0x{1:x}: {2}' -f $Instruction.family, [uint32]$Instruction.opcode_id, $Instruction.reason)
}

function Read-AuditOutput {
    param($Job)
    $output = ''
    $errorOutput = ''
    $readError = ''
    try {
        $tasks = [Threading.Tasks.Task[]]@($Job.StdoutTask, $Job.StderrTask)
        if (-not [Threading.Tasks.Task]::WaitAll($tasks, 5000)) {
            throw 'Timed out waiting for worker output streams to close.'
        }
        $output = $Job.StdoutTask.Result
        $errorOutput = $Job.StderrTask.Result
    } catch {
        $readError = $_.Exception.Message
    }
    [IO.File]::WriteAllText($Job.Stdout, $output)
    [IO.File]::WriteAllText($Job.Stderr, $errorOutput)
    return [pscustomobject]@{ Output = $output; ErrorOutput = $errorOutput; ReadError = $readError }
}

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Auditor) { $Auditor = Join-Path $repo '_Build\windows\shader_cfg_tests.exe' }
if (-not $OutputDirectory) {
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 6)
    $OutputDirectory = Join-Path $repo ('_Build\shader-audits\' + $stamp)
}
$Auditor = (Get-Item -LiteralPath $Auditor).FullName
$CorpusDirectory = (Get-Item -LiteralPath $CorpusDirectory).FullName
if (Test-Path -LiteralPath $OutputDirectory) {
    if (-not (Test-Path -LiteralPath $OutputDirectory -PathType Container) -or
        @(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count -gt 0) {
        throw 'OutputDirectory must be a new or empty directory; previous audit results are preserved.'
    }
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Get-Item -LiteralPath $OutputDirectory).FullName
$manifests = @(Get-ChildItem -LiteralPath $CorpusDirectory -Filter '*.json' -File -Recurse | Sort-Object FullName)
if ($manifests.Count -eq 0) { throw 'No shader manifests found.' }
$pending = New-Object 'System.Collections.Generic.List[object]'
$results = New-Object 'System.Collections.Generic.List[object]'
$next = 0
$started = [DateTime]::UtcNow
$watch = [Diagnostics.Stopwatch]::StartNew()
try {
    while ($next -lt $manifests.Count -or $pending.Count -gt 0) {
        while ($next -lt $manifests.Count -and $pending.Count -lt $Jobs) {
            $manifest = $manifests[$next]
            $jobDirectory = Join-Path $OutputDirectory ('{0:D5}' -f $next)
            New-Item -ItemType Directory -Path $jobDirectory | Out-Null
            $stdout = Join-Path $jobDirectory 'stdout.txt'
            $stderr = Join-Path $jobDirectory 'stderr.txt'
            $startInfo = New-Object Diagnostics.ProcessStartInfo
            $startInfo.FileName = $Auditor
            $startInfo.Arguments = '--audit-shader "' + $manifest.FullName + '"'
            $startInfo.WorkingDirectory = $jobDirectory
            $startInfo.UseShellExecute = $false
            $startInfo.CreateNoWindow = $true
            $startInfo.RedirectStandardOutput = $true
            $startInfo.RedirectStandardError = $true
            $process = New-Object Diagnostics.Process
            $process.StartInfo = $startInfo
            try {
                if (-not $process.Start()) { throw 'Could not start shader auditor.' }
                # Drain both pipes immediately: waiting first can deadlock on a full pipe.
                $stdoutTask = $process.StandardOutput.ReadToEndAsync()
                $stderrTask = $process.StandardError.ReadToEndAsync()
            } catch {
                $null = Stop-AuditProcess $process
                $process.Dispose()
                throw
            }
            $pending.Add([pscustomobject]@{
                Process = $process; Manifest = $manifest.FullName; Directory = $jobDirectory
                Stdout = $stdout; Stderr = $stderr; Started = [DateTime]::UtcNow
                StdoutTask = $stdoutTask; StderrTask = $stderrTask
            })
            $next++
        }
        foreach ($job in @($pending.ToArray())) {
            $elapsed = ([DateTime]::UtcNow - $job.Started).TotalSeconds
            $timedOut = -not $job.Process.HasExited -and $elapsed -ge $TimeoutSeconds
            if ($timedOut -and -not (Stop-AuditProcess $job.Process)) {
                throw ('Unable to stop timed-out worker PID {0}; aborting further launches.' -f $job.Process.Id)
            }
            if (-not $job.Process.HasExited) { continue }
            if (-not $job.Process.WaitForExit(5000)) {
                throw ('Unable to confirm worker exit for PID {0}.' -f $job.Process.Id)
            }
            $job.Process.Refresh()
            $capturedOutput = Read-AuditOutput $job
            $output = $capturedOutput.Output
            $errorOutput = $capturedOutput.ErrorOutput
            $payload = $null
            $marker = [regex]::Match($output, '(?m)^KYTY_SHADER_AUDIT_RESULT (.+)\r?$')
            if ($marker.Success) {
                try { $payload = $marker.Groups[1].Value | ConvertFrom-Json } catch { }
            }
            $phaseMatches = [regex]::Matches($output, '(?m)^KYTY_SHADER_AUDIT_PHASE (\w+)')
            $phase = if ($phaseMatches.Count) { $phaseMatches[$phaseMatches.Count - 1].Groups[1].Value } else { 'input' }
            $status = if ($timedOut) { 'timeout' } elseif ($capturedOutput.ReadError) { 'failed' } elseif ($null -ne $payload) { $payload.status } else { 'failed' }
            if ($job.Process.ExitCode -ne 0 -and $status -eq 'passed') { $status = 'failed' }
            $errorText = ''
            if ($status -ne 'passed') {
                if ($timedOut) { $errorText = 'worker timeout' }
                elseif ($capturedOutput.ReadError) { $errorText = $capturedOutput.ReadError }
                elseif ($payload -and $payload.error) { $errorText = $payload.error }
                elseif ($payload -and $payload.unsupported_instructions.Count) {
                    $errorText = (@($payload.unsupported_instructions | ForEach-Object { Get-UnsupportedReason $_ } | Select-Object -Unique) -join "`n")
                }
                elseif ($payload -and $payload.unknown_instruction_pcs.Count) { $errorText = 'unsupported instruction encodings' }
                else {
                    $fatal = [regex]::Match($output, '(?s)--- Error ---\s*(.*)')
                    $errorText = if ($fatal.Success) { $fatal.Groups[1].Value.Trim() } else { ($output + "`n" + $errorOutput).Trim() }
                    if ($errorText.Length -gt 3000) { $errorText = $errorText.Substring($errorText.Length - 3000) }
                }
            }
            $row = [pscustomobject]@{
                manifest = $job.Manifest; status = $status; phase = $phase
                exit_code = $job.Process.ExitCode; timed_out = $timedOut
                elapsed_seconds = [Math]::Round($elapsed, 3); error = $errorText
                result = $payload; stdout = $job.Stdout; stderr = $job.Stderr
            }
            $results.Add($row)
            $row | ConvertTo-Json -Depth 12 -Compress | Add-Content -LiteralPath (Join-Path $OutputDirectory 'results.jsonl') -Encoding UTF8
            $job.Process.Dispose()
            $null = $pending.Remove($job)
            if ($results.Count % 25 -eq 0 -or $results.Count -eq $manifests.Count) {
                Write-Output ('Audited {0}/{1} shaders' -f $results.Count, $manifests.Count)
            }
        }
        if ($pending.Count -gt 0) { Start-Sleep -Milliseconds 25 }
    }
} finally {
    foreach ($job in $pending.ToArray()) {
        try {
            if (-not (Stop-AuditProcess $job.Process)) {
                Write-Warning ('Worker PID {0} may still be running.' -f $job.Process.Id)
            }
            $null = Read-AuditOutput $job
        } catch {
            Write-Warning ('Worker cleanup failed for {0}: {1}' -f $job.Manifest, $_.Exception.Message)
        } finally {
            try { $job.Process.Dispose() } catch {
                Write-Warning ('Could not dispose worker for {0}: {1}' -f $job.Manifest, $_.Exception.Message)
            }
        }
    }
}
$watch.Stop()
$failed = @($results | Where-Object status -ne 'passed')
$unsupported = @($failed | ForEach-Object {
    $row = $_
    foreach ($instruction in @($row.result.unsupported_instructions)) {
        if ($null -eq $instruction) { continue }
        [pscustomobject]@{
            key = Get-UnsupportedReason $instruction
            family = $instruction.family; opcode_id = $instruction.opcode_id
            reason = $instruction.reason; manifest = $row.manifest
            pc = $instruction.pc; instruction = $instruction.instruction
        }
    }
})
$groups = @($unsupported | Group-Object -Property key | ForEach-Object {
    $group = $_
    $first = $group.Group[0]
    $shaders = @($group.Group.manifest | Select-Object -Unique)
    [pscustomobject]@{
        count = $shaders.Count; distinct_shaders = $shaders.Count
        instruction_count = $group.Count; reason = 'decode: ' + $group.Name
        family = $first.family; opcode_id = $first.opcode_id
        unsupported_reason = $first.reason; manifests = $shaders
        pc_examples = @($group.Group | Select-Object -First 8 | ForEach-Object {
            [pscustomobject]@{ manifest = $_.manifest; pc = ('0x{0:x8}' -f [uint32]$_.pc); instruction = $_.instruction }
        })
    }
})
$groups += @($failed | Where-Object { -not $_.result.unsupported_instructions.Count } | Group-Object -Property {
    $text = $_.error -replace 'hash=0x[0-9a-fA-F]+', 'hash=<shader>'
    $text = $text -replace '\bpc(?:\s*=\s*|\s+)0x[0-9a-fA-F]+', 'pc=<instruction>'
    $text = $text -replace '0x[0-9a-fA-F]+(?=: unsupported)', '<instruction>'
    $text = $text -replace ' in [^\r\n]+:\d+', ''
    $_.phase + ': ' + $text
} | ForEach-Object {
    $shaders = @($_.Group.manifest | Select-Object -Unique)
    [pscustomobject]@{ count = $shaders.Count; distinct_shaders = $shaders.Count; reason = $_.Name; manifests = $shaders }
})
$groups = @($groups | Sort-Object Count -Descending)
$coverage = @($results | Where-Object status -eq 'passed' | Group-Object -Property {
    if ($_.result.checked_through -eq 'cfg') {
        if ($_.result.dispatcher_fallback_required) { 'cfg_dispatcher_fallback_required' }
        else { 'cfg_structured' }
    } else { $_.result.checked_through }
} | ForEach-Object {
    [pscustomobject]@{ checked_through = $_.Name; count = $_.Count }
})
$report = [ordered]@{
    corpus = $CorpusDirectory; auditor = $Auditor
    auditor_sha256 = (Get-FileHash -LiteralPath $Auditor -Algorithm SHA256).Hash.ToLowerInvariant()
    started_utc = $started.ToString('o'); elapsed_seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
    total = $results.Count; passed = $results.Count - $failed.Count; failed = $failed.Count
    coverage = $coverage; error_groups = $groups
    limitations = @('A passed CFG check does not prove translation or GPU compatibility.', 'CFG dispatcher fallback requirements are reported separately; the fallback IR is not checked by CFG-only jobs.', 'Resource tracking checks do not materialize guest memory or execute SPIR-V.', 'Each shader runs in a separate bounded process; failure does not stop remaining shaders.', 'Error groups overlap: a shader may have several unsupported instructions, so group counts must not be added to obtain the failed-shader count.')
    results = $results.ToArray()
}
$report | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'report.json') -Encoding UTF8
$markdown = New-Object 'System.Collections.Generic.List[string]'
$markdown.Add('# Shader batch audit')
$markdown.Add('')
$markdown.Add(('Checked {0}; passed {1}; failed {2}; elapsed {3}s.' -f $report.total, $report.passed, $report.failed, $report.elapsed_seconds))
$markdown.Add('')
$markdown.Add('Passing this audit is not proof of a working game: captured metadata determines the checked phases, and guest memory/GPU execution are not checked.')
$markdown.Add('')
foreach ($item in $coverage) { $markdown.Add(('- {0}: {1}' -f $item.checked_through, $item.count)) }
$markdown.Add('')
foreach ($group in $groups) {
    $markdown.Add(('## {0} shader(s)' -f $group.count))
    $markdown.Add('')
    $markdown.Add('```text')
    $markdown.Add($group.reason)
    $markdown.Add('```')
    $markdown.Add('')
    $markdown.Add(('Example: {0}' -f $group.manifests[0]))
    foreach ($example in @($group.pc_examples)) {
        if ($null -ne $example) { $markdown.Add(('- {0}: {1} {2}' -f $example.manifest, $example.pc, $example.instruction)) }
    }
    $markdown.Add('')
}
$markdown | Set-Content -LiteralPath (Join-Path $OutputDirectory 'report.md') -Encoding UTF8
Write-Output ('Report: ' + (Join-Path $OutputDirectory 'report.json'))
Write-Output ('Passed {0}, failed {1}' -f $report.passed, $report.failed)
if ($failed.Count -gt 0) { exit 1 }
