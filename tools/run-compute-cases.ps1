param(
    [string]$Executable,
    [string]$OutputDirectory,
    [string]$CasePattern,
    [ValidateRange(1, 3600)][int]$TimeoutSeconds = 30
)
$ErrorActionPreference = 'Stop'

function ConvertTo-WindowsArgument {
    param([AllowEmptyString()][string]$Value)
    # CommandLineToArgvW/CRT quoting: backslashes before quotes, including the
    # closing quote, must be doubled. No command shell interprets these arguments.
    $quoted = New-Object Text.StringBuilder
    $null = $quoted.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { $slashes++; continue }
        if ($character -eq '"') {
            $null = $quoted.Append(('\' * (2 * $slashes + 1)))
        } elseif ($slashes -gt 0) {
            $null = $quoted.Append(('\' * $slashes))
        }
        $null = $quoted.Append($character)
        $slashes = 0
    }
    if ($slashes -gt 0) { $null = $quoted.Append(('\' * (2 * $slashes))) }
    $null = $quoted.Append('"')
    return $quoted.ToString()
}

function Stop-ComputeWorker {
    param([Diagnostics.Process]$Process)
    try {
        if (-not $Process.HasExited) {
            try { $Process.Kill() }
            catch {
                # The worker may have exited between the check and Kill.
                $Process.Refresh()
                if (-not $Process.HasExited) { throw }
            }
        }
        return $Process.WaitForExit(5000)
    } catch {
        Write-Warning ('Could not confirm compute worker termination: {0}' -f $_.Exception.Message)
        return $false
    }
}

function Invoke-ComputeWorker {
    param([string[]]$WorkerArguments, [string]$Directory)
    New-Item -ItemType Directory -Path $Directory | Out-Null
    $stdoutPath = Join-Path $Directory 'stdout.txt'
    $stderrPath = Join-Path $Directory 'stderr.txt'
    $process = New-Object Diagnostics.Process
    $stdoutStream = $null
    $stderrStream = $null
    $stdoutTask = $null
    $stderrTask = $null
    $startedProcess = $false
    $terminated = $true
    $timedOut = $false
    $exitCode = $null
    $workerId = $null
    $errorText = ''
    $started = [DateTime]::UtcNow
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try {
        $stdoutStream = [IO.File]::Open($stdoutPath, [IO.FileMode]::CreateNew,
                                      [IO.FileAccess]::Write, [IO.FileShare]::Read)
        $stderrStream = [IO.File]::Open($stderrPath, [IO.FileMode]::CreateNew,
                                      [IO.FileAccess]::Write, [IO.FileShare]::Read)
        $startInfo = New-Object Diagnostics.ProcessStartInfo
        $startInfo.FileName = $Executable
        $startInfo.Arguments = (@($WorkerArguments | ForEach-Object {
            ConvertTo-WindowsArgument $_
        }) -join ' ')
        $startInfo.WorkingDirectory = $Directory
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $process.StartInfo = $startInfo
        if (-not $process.Start()) { throw 'Could not start compute test worker.' }
        $startedProcess = $true
        $terminated = $false
        $workerId = $process.Id
        # Keep the process handle, including when it exits before WaitForExit.
        $null = $process.Handle
        # Drain both pipes immediately into files; retain partial output on a
        # timeout without keeping every compiler diagnostic in runner memory.
        $stdoutTask = $process.StandardOutput.BaseStream.CopyToAsync($stdoutStream)
        $stderrTask = $process.StandardError.BaseStream.CopyToAsync($stderrStream)
        if ($process.WaitForExit($TimeoutSeconds * 1000)) {
            $terminated = $true
        } else {
            $timedOut = $true
            $errorText = 'worker timeout'
            $terminated = Stop-ComputeWorker $process
        }
        if ($terminated) {
            $process.Refresh()
            $exitCode = $process.ExitCode
        } else {
            $errorText += '; worker termination could not be confirmed; further launches are disabled'
        }
    } catch {
        $errorText = $_.Exception.Message
    } finally {
        if ($startedProcess -and -not $terminated) {
            $terminated = Stop-ComputeWorker $process
        }
        $tasks = [Threading.Tasks.Task[]]@(@($stdoutTask, $stderrTask) | Where-Object { $null -ne $_ })
        if ($tasks.Count -gt 0) {
            try {
                if (-not [Threading.Tasks.Task]::WaitAll($tasks, 5000)) {
                    throw 'Timed out draining worker output streams.'
                }
            } catch {
                $errorText = ($errorText + '; ' + $_.Exception.Message).TrimStart([char[]]'; ')
            }
        }
        if ($startedProcess) {
            # Also close our pipe handles if a stream-copy task did not finish.
            foreach ($reader in @($process.StandardOutput, $process.StandardError)) {
                try { $reader.Dispose() }
                catch { $errorText = ($errorText + '; ' + $_.Exception.Message).TrimStart([char[]]'; ') }
            }
        }
        foreach ($stream in @($stdoutStream, $stderrStream)) {
            if ($null -ne $stream) {
                try { $stream.Dispose() }
                catch { $errorText = ($errorText + '; ' + $_.Exception.Message).TrimStart([char[]]'; ') }
            }
        }
        try { $process.Dispose() }
        catch { $errorText = ($errorText + '; ' + $_.Exception.Message).TrimStart([char[]]'; ') }
        $watch.Stop()
    }
    if (-not $terminated -and -not $errorText) { $errorText = 'worker termination could not be confirmed' }
    if ($null -ne $exitCode -and $exitCode -ne 0 -and -not $errorText) {
        $errorText = 'worker exited with code ' + $exitCode
    }
    $status = if ($timedOut) { 'timeout' }
              elseif ($terminated -and $exitCode -eq 0 -and -not $errorText) { 'passed' }
              else { 'failed' }
    return [pscustomobject]@{
        status = $status; exit_code = $exitCode; timed_out = $timedOut
        termination_confirmed = $terminated; pid = $workerId
        started_utc = $started.ToString('o'); elapsed_seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
        arguments = $WorkerArguments; error = $errorText; stdout = $stdoutPath; stderr = $stderrPath
    }
}

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $repo '_Build\windows\shader_recompiler_compute_tests.exe' }
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw 'Compute test executable does not exist.' }
$Executable = (Get-Item -LiteralPath $Executable).FullName
$executableHash = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
$matcher = if ($CasePattern) {
    [regex]::new($CasePattern, [Text.RegularExpressions.RegexOptions]::CultureInvariant,
                 [TimeSpan]::FromSeconds(1))
} else { $null }
if (-not $OutputDirectory) {
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $OutputDirectory = Join-Path $repo ('_Build\compute-tests\' + $stamp)
}
if (Test-Path -LiteralPath $OutputDirectory) {
    if (-not (Test-Path -LiteralPath $OutputDirectory -PathType Container) -or
        @(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count -gt 0) {
        throw 'OutputDirectory must be new or empty; previous compute test results are preserved.'
    }
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Get-Item -LiteralPath $OutputDirectory).FullName
$utf8 = New-Object Text.UTF8Encoding($false)
$resultsPath = Join-Path $OutputDirectory 'results.jsonl'
[IO.File]::WriteAllText($resultsPath, '', $utf8)
$results = New-Object 'System.Collections.Generic.List[object]'
$cases = New-Object 'System.Collections.Generic.List[string]'
$selected = @()
$listing = $null
$runnerError = ''
$started = [DateTime]::UtcNow
$watch = [Diagnostics.Stopwatch]::StartNew()
try {
    $listing = Invoke-ComputeWorker @('--list-compute-cases') (Join-Path $OutputDirectory 'listing')
    if ($listing.status -ne 'passed') { throw ('Case listing failed: {0}' -f $listing.error) }
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($line in [IO.File]::ReadLines($listing.stdout)) {
        if (-not $line.StartsWith('KYTY_COMPUTE_CASE ', [StringComparison]::Ordinal)) { continue }
        $name = $line.Substring('KYTY_COMPUTE_CASE '.Length)
        if ([string]::IsNullOrWhiteSpace($name) -or -not $seen.Add($name)) {
            throw 'Case listing contains an empty or duplicate case name.'
        }
        $cases.Add($name)
    }
    if ($cases.Count -eq 0) { throw 'Case listing produced no KYTY_COMPUTE_CASE markers.' }
    $selected = @($cases | Where-Object { $null -eq $matcher -or $matcher.IsMatch($_) })
    if ($selected.Count -eq 0) { throw 'CasePattern matched no compute fixtures.' }
    for ($index = 0; $index -lt $selected.Count; $index++) {
        $name = $selected[$index]
        Write-Output ('Compute {0}/{1}: {2}' -f ($index + 1), $selected.Count, $name)
        $row = Invoke-ComputeWorker @('--compute-case', $name) (Join-Path $OutputDirectory ('{0:D5}' -f $index))
        $row | Add-Member -NotePropertyName name -NotePropertyValue $name
        $row | Add-Member -NotePropertyName index -NotePropertyValue $index
        $results.Add($row)
        [IO.File]::AppendAllText($resultsPath, ($row | ConvertTo-Json -Depth 8 -Compress) + "`n", $utf8)
        Write-Output ('  {0} ({1}s)' -f $row.status, $row.elapsed_seconds)
        # Never overlap GPU workers when a timed-out process could still be alive.
        if (-not $row.termination_confirmed) { throw ('Worker for {0} may still be running; batch stopped.' -f $name) }
    }
} catch {
    $runnerError = $_.Exception.Message
    Write-Warning $runnerError
} finally {
    $watch.Stop()
    $failed = @($results | Where-Object status -ne 'passed').Count
    $report = [ordered]@{
        schema_version = 1; kind = 'compute-fixtures'; executable = $Executable
        executable_sha256 = $executableHash
        case_pattern = $CasePattern; timeout_seconds = $TimeoutSeconds
        started_utc = $started.ToString('o'); elapsed_seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
        listed = $cases.Count; selected = $selected.Count; completed = $results.Count
        not_run = $selected.Count - $results.Count; passed = $results.Count - $failed; failed = $failed
        timed_out = @($results | Where-Object timed_out -eq $true).Count
        runner_error = $runnerError; listing = $listing; results = $results.ToArray()
    }
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'report.json'),
                          ($report | ConvertTo-Json -Depth 12), $utf8)
}
Write-Output ('Compute fixtures: {0} passed, {1} failed, {2} not run. Report: {3}' -f
              $report.passed, $report.failed, $report.not_run, (Join-Path $OutputDirectory 'report.json'))
if ($runnerError -or $report.failed -gt 0 -or $report.completed -eq 0) { exit 1 }
exit 0
