param(
    [Parameter(Mandatory = $true)][string]$CorpusDirectory,
    [string]$Auditor,
    [string]$OutputDirectory,
    [string]$ComputeHostProfile,
    [string[]]$AuditorArgumentPrefix = @(),
    [switch]$EmitSpirv,
    [switch]$Resume,
    [switch]$RetryFailed,
    [ValidateRange(1, 1024)][int]$MaxOutputMegabytes = 64,
    [ValidateRange(1, 16)][int]$Jobs = 4,
    [ValidateRange(1, 300)][int]$TimeoutSeconds = 30
)
$ErrorActionPreference = 'Stop'

function Get-TextHash {
    param([string]$Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function ConvertTo-CanonicalInput {
    param($Value)
    if ($null -eq $Value) { return $null }
    if ($Value -is [System.Collections.IDictionary] -or $Value -is [pscustomobject]) {
        $names = [string[]]$(if ($Value -is [System.Collections.IDictionary]) { @($Value.Keys) } else { @($Value.PSObject.Properties.Name) })
        [Array]::Sort($names, [StringComparer]::Ordinal)
        $ordered = [ordered]@{}
        foreach ($name in $names) {
            # Only observation metadata is excluded. Unknown compiler fields remain inputs.
            if ($name -eq 'capture_attempt') { continue }
            $ordered[$name] = ConvertTo-CanonicalInput $Value.$name
        }
        return $ordered
    }
    if ($Value -is [System.Collections.IEnumerable] -and $Value -isnot [string]) {
        $items = New-Object 'System.Collections.Generic.List[object]'
        foreach ($item in $Value) { $items.Add((ConvertTo-CanonicalInput $item)) }
        return ,$items.ToArray()
    }
    return $Value
}

function Get-CompileInput {
    param([IO.FileInfo]$Manifest)
    $raw = [IO.File]::ReadAllText($Manifest.FullName)
    $metadata = $raw | ConvertFrom-Json
    if (-not $metadata.schema_version -or -not $metadata.stage) { throw 'Shader manifest requires schema_version and stage.' }
    $canonical = ConvertTo-CanonicalInput $metadata
    $canonical.Remove('kind')
    $references = New-Object 'System.Collections.Generic.List[object]'
    foreach ($field in @('code_file', 'back_code_file')) {
        $filename = $metadata.$field
        if ($field -eq 'code_file' -and -not $filename) { $filename = [IO.Path]::ChangeExtension($Manifest.Name, '.bin') }
        if (-not $filename) { continue }
        if ([IO.Path]::GetFileName([string]$filename) -ne [string]$filename -or [IO.Path]::IsPathRooted([string]$filename)) {
            throw ($field + ' must be a sibling filename.')
        }
        $path = Join-Path $Manifest.DirectoryName $filename
        $file = Get-Item -LiteralPath $path
        if ($file.FullName -eq $Manifest.FullName) { throw ($field + ' cannot refer to its own manifest.') }
        if ($file.PSIsContainer -or $file.Length -eq 0 -or $file.Length -gt 64MB) { throw ('Invalid or oversized ' + $field) }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        $canonical[$field] = [ordered]@{ sha256 = $hash; bytes = $file.Length }
        $references.Add([pscustomobject]@{ name = $filename; path = $file.FullName; sha256 = $hash })
    }
    $identity = $canonical | ConvertTo-Json -Depth 100 -Compress
    return [pscustomobject]@{ id = Get-TextHash $identity; canonical = $identity; raw = $raw
        references = $references.ToArray(); capture_attempt = $metadata.capture_attempt }
}

function Quote-NativeArgument {
    param([string]$Argument)
    # Windows CommandLineToArgvW escaping; shell execution is disabled.
    return '"' + [regex]::Replace([regex]::Replace($Argument, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

function Quote-PowerShellArgument {
    param([string]$Argument)
    return "'" + $Argument.Replace("'", "''") + "'"
}

function Get-FailureCategory {
    param([string]$Phase, [string]$Status, [string]$ErrorText)
    if ($Status -eq 'input_error') { return 'input_error' }
    # Explicit backend/replay phases are stronger evidence than diagnostic wording.
    switch -Regex ($Phase) {
        '^materialize' { return 'materialize' }
        '^emit|^optimize|^compile_emit' { return 'emit' }
        '^valid|^spirv_valid' { return 'validation' }
        '^driver|^pipeline|^host_shader_module' { return 'driver' }
    }
    if ($ErrorText -match 'shader resource tracking|resource plan|descriptor source') { return 'resource_plan' }
    switch -Regex ($Phase) {
        '^decode$' { return 'decode' }
        '^cfg$|^translate' { return 'translate' }
        '^resource|^compute_execution_precheck$' { return 'resource_plan' }
        default { return 'worker_error' }
    }
}

function Write-AtomicText {
    param([string]$Path, [string]$Text)
    $temporary = $Path + '.tmp-' + [Guid]::NewGuid().ToString('N')
    [IO.File]::WriteAllText($temporary, $Text, (New-Object Text.UTF8Encoding($false)))
    if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temporary, $Path, [System.Management.Automation.Language.NullString]::Value) }
    else { [IO.File]::Move($temporary, $Path) }
}

function Stop-AuditProcess {
    param([Diagnostics.Process]$Process)
    $workerId = 'unknown'
    try { $workerId = $Process.Id } catch { }
    try {
        if (-not $Process.HasExited) {
            if ($Process.GetType().GetMethod('Kill', [type[]]@([bool]))) {
                try { $Process.Kill($true) }
                catch {
                    # The worker can exit between HasExited and Kill.
                    $Process.Refresh()
                    if (-not $Process.HasExited) { throw }
                }
            } else {
                # .NET Framework lacks Kill(entireProcessTree). Use only this
                # retained worker PID, with bounded, hidden and drained cleanup.
                $killInfo = New-Object Diagnostics.ProcessStartInfo
                $killInfo.FileName = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::System)) 'taskkill.exe'
                $killInfo.Arguments = '/PID ' + $Process.Id + ' /T /F'
                $killInfo.UseShellExecute = $false; $killInfo.CreateNoWindow = $true
                $killInfo.RedirectStandardOutput = $true; $killInfo.RedirectStandardError = $true
                $killer = New-Object Diagnostics.Process
                $killer.StartInfo = $killInfo
                try {
                    if (-not $killer.Start()) { throw 'Could not start owned-tree cleanup.' }
                    $killStdout = $killer.StandardOutput.ReadToEndAsync()
                    $killStderr = $killer.StandardError.ReadToEndAsync()
                    if (-not $killer.WaitForExit(5000)) {
                        $killer.Kill()
                        if (-not $killer.WaitForExit(5000)) { throw 'Could not stop owned-tree cleanup helper.' }
                        throw 'Owned-tree cleanup helper exceeded its deadline.'
                    }
                    if (-not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($killStdout, $killStderr), 5000)) {
                        throw 'Owned-tree cleanup output did not close.'
                    }
                    if ($killer.ExitCode -ne 0) {
                        throw ('Owned-tree cleanup failed: ' + $killStdout.Result.Trim() + ' ' + $killStderr.Result.Trim())
                    }
                } finally { $killer.Dispose() }
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
        $Job.StdoutStream.Flush()
        $Job.StderrStream.Flush()
    } catch {
        $readError = $_.Exception.Message
    }
    finally { $Job.StdoutStream.Dispose(); $Job.StderrStream.Dispose() }
    # Streams are drained directly to files, rather than retained unbounded in RAM.
    $output = [IO.File]::ReadAllText($Job.Stdout)
    $errorOutput = [IO.File]::ReadAllText($Job.Stderr)
    return [pscustomobject]@{ Output = $output; ErrorOutput = $errorOutput; ReadError = $readError }
}

$repo = Split-Path -Parent $PSScriptRoot
if ($Resume -and -not $OutputDirectory) { throw '-Resume requires an explicit OutputDirectory.' }
if ($RetryFailed -and -not $Resume) { throw '-RetryFailed requires -Resume.' }
if (-not $Auditor) { $Auditor = Join-Path $repo '_Build\windows\shader_cfg_tests.exe' }
if (-not $OutputDirectory) {
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 6)
    $OutputDirectory = Join-Path $repo ('_Build\shader-audits\' + $stamp)
}
$Auditor = (Get-Item -LiteralPath $Auditor).FullName
$CorpusDirectory = (Get-Item -LiteralPath $CorpusDirectory).FullName
$OutputDirectory = [IO.Path]::GetFullPath(
    $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory))
$corpusPrefix = $CorpusDirectory.TrimEnd([char[]]@('\', '/')) + [IO.Path]::DirectorySeparatorChar
if ($OutputDirectory.Equals($CorpusDirectory, [StringComparison]::OrdinalIgnoreCase) -or
    $OutputDirectory.StartsWith($corpusPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputDirectory must be outside CorpusDirectory; audit artifacts are not shader manifests.'
}
# Freeze the input inventory before writing any audit artifact.
$jsonFiles = @(Get-ChildItem -LiteralPath $CorpusDirectory -Filter '*.json' -File -Recurse | Sort-Object FullName)
$ignoredJson = New-Object 'System.Collections.Generic.List[string]'
$manifests = @($jsonFiles | Where-Object {
    $candidate = $false
    try {
        $metadata = [IO.File]::ReadAllText($_.FullName) | ConvertFrom-Json
        $candidate = $metadata.kind -in @('registered', 'dispatched') -or $metadata.code_file -or $metadata.back_code_file -or
            ($metadata.stage -and ($metadata.shader_hash -or $metadata.code_size_bytes))
    } catch { }
    # Malformed captures with their sibling binary remain explicit input errors.
    if (-not $candidate) { $candidate = Test-Path -LiteralPath ([IO.Path]::ChangeExtension($_.FullName, '.bin')) -PathType Leaf }
    if (-not $candidate) { $ignoredJson.Add($_.FullName) }
    $candidate
})
if ($manifests.Count -eq 0) { throw 'No shader manifests found.' }
if (Test-Path -LiteralPath $OutputDirectory) {
    if (-not (Test-Path -LiteralPath $OutputDirectory -PathType Container) -or
        (-not $Resume -and @(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count -gt 0)) {
        throw 'OutputDirectory must be a new or empty directory; previous audit results are preserved.'
    }
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Get-Item -LiteralPath $OutputDirectory).FullName
$profileSnapshot = $null
$profileHash = $null
if ($ComputeHostProfile) {
    $ComputeHostProfile = (Get-Item -LiteralPath $ComputeHostProfile).FullName
    # Every worker consumes the same preserved host input, never a changing file.
    $profileHash = (Get-FileHash -LiteralPath $ComputeHostProfile -Algorithm SHA256).Hash.ToLowerInvariant()
    $profileSnapshot = Join-Path $OutputDirectory ('compute-host-profile-' + $profileHash + '.json')
    if (-not [IO.File]::Exists($profileSnapshot)) { [IO.File]::Copy($ComputeHostProfile, $profileSnapshot, $false) }
    if ((Get-FileHash -LiteralPath $profileSnapshot -Algorithm SHA256).Hash.ToLowerInvariant() -ne $profileHash) {
        throw 'Preserved compute host profile changed.'
    }
    $profileHash = (Get-FileHash -LiteralPath $profileSnapshot -Algorithm SHA256).Hash.ToLowerInvariant()
}
$pending = New-Object 'System.Collections.Generic.List[object]'
$results = New-Object 'System.Collections.Generic.List[object]'
$auditorHash = (Get-FileHash -LiteralPath $Auditor -Algorithm SHA256).Hash.ToLowerInvariant()
# Preserve identity, not values, for compiler-affecting environment. No secrets are written.
$environmentInputs = [ordered]@{}
foreach ($variable in @(Get-ChildItem Env: | Where-Object Name -Match '^(KYTY_|VK_|SPIRV_)' | Sort-Object Name)) {
    $environmentInputs[$variable.Name] = Get-TextHash $variable.Value
}
$prefixInputs = @($AuditorArgumentPrefix | ForEach-Object {
    if (Test-Path -LiteralPath $_ -PathType Leaf) {
        [ordered]@{ argument = $_; sha256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant() }
    } else { [ordered]@{ argument = $_ } }
})
$auditIdentity = [ordered]@{ schema = 2; auditor_sha256 = $auditorHash; prefix = $prefixInputs
    runner_sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
    host_profile_sha256 = $profileHash; emit_spirv = [bool]$EmitSpirv
    timeout_seconds = $TimeoutSeconds; max_output_megabytes = $MaxOutputMegabytes; environment = $environmentInputs }
$auditIdentityHash = Get-TextHash ($auditIdentity | ConvertTo-Json -Depth 100 -Compress)
$cached = @{}
$journal = Join-Path $OutputDirectory 'results.jsonl'
if ($Resume -and [IO.File]::Exists($journal)) {
    $lines = [IO.File]::ReadAllLines($journal)
    for ($line = 0; $line -lt $lines.Length; $line++) {
        if (-not $lines[$line].Trim()) { continue }
        try { $previous = $lines[$line] | ConvertFrom-Json }
        catch {
            if ($line -ne $lines.Length - 1) { throw 'Audit journal contains a corrupt completed line.' }
            # Preserve a truncated final line separately and repair only this task-owned journal.
            [IO.File]::WriteAllText((Join-Path $OutputDirectory ('journal-tail-' + [Guid]::NewGuid().ToString('N') + '.txt')), $lines[$line])
            $completeLines = [string[]]@()
            if ($line -gt 0) { $completeLines = $lines[0..($line - 1)] }
            [IO.File]::WriteAllLines($journal, $completeLines, (New-Object Text.UTF8Encoding($false)))
            Write-Warning 'Preserved and removed an interrupted final journal line; its input will be retried.'
            continue
        }
        if ($previous.audit_key) { $cached[$previous.audit_key] = $previous }
    }
    # Append must start on a fresh line even after interruption immediately after JSON.
    if ([IO.File]::Exists($journal)) {
        $journalText = [IO.File]::ReadAllText($journal)
        if ($journalText.Length -gt 0 -and -not $journalText.EndsWith("`n")) { [IO.File]::AppendAllText($journal, "`n") }
    }
}
$inputIndex = @{}
$inventory = New-Object 'System.Collections.Generic.List[object]'
foreach ($manifestFile in $manifests) {
    try { $input = Get-CompileInput $manifestFile; $inputError = '' }
    catch { $inputError = $_.Exception.Message; $input = [pscustomobject]@{ id = Get-TextHash ([IO.File]::ReadAllText($manifestFile.FullName)); canonical = $null } }
    $key = Get-TextHash ($input.id + ':' + $auditIdentityHash + ':' + $inputError)
    if ($inputIndex.ContainsKey($key)) {
        $inputIndex[$key].Manifests.Add($manifestFile.FullName)
        $inputIndex[$key].Attempts.Add([pscustomobject]@{ manifest = $manifestFile.FullName; capture_attempt = $input.capture_attempt })
        continue
    }
    $aliases = New-Object 'System.Collections.Generic.List[string]'
    $aliases.Add($manifestFile.FullName)
    $attempts = New-Object 'System.Collections.Generic.List[object]'
    $attempts.Add([pscustomobject]@{ manifest = $manifestFile.FullName; capture_attempt = $input.capture_attempt })
    $entry = [pscustomobject]@{ FullName = $manifestFile.FullName; Original = $manifestFile.FullName
        Manifests = $aliases; Attempts = $attempts; Input = $input; Key = $key; InputError = $inputError }
    $inputIndex[$key] = $entry
    $inventory.Add($entry)
}
$queue = New-Object 'System.Collections.Generic.List[object]'
foreach ($entry in $inventory) {
    $previous = $cached[$entry.Key]
    if ($previous -and (-not $RetryFailed -or $previous.status -eq 'passed') -and $previous.status -ne 'input_error') {
        $previous.manifest = $entry.Original
        $previous.manifests = $entry.Manifests.ToArray()
        $previous | Add-Member -NotePropertyName capture_attempts -NotePropertyValue $entry.Attempts.ToArray() -Force
        $previous | Add-Member -NotePropertyName reused -NotePropertyValue $true -Force
        $results.Add($previous)
    } elseif ($entry.InputError) {
        $row = [pscustomobject]@{ compile_input_id = $entry.Input.id; audit_key = $entry.Key
            manifest = $entry.Original; manifests = $entry.Manifests.ToArray(); status = 'input_error'
            phase = 'input'; category = 'input_error'; error = $entry.InputError; result = $null
            reused = $false; timed_out = $false; exit_code = $null; elapsed_seconds = 0; stdout = $null; stderr = $null; replay_command = $null }
        $results.Add($row)
        $row | ConvertTo-Json -Depth 100 -Compress | Add-Content -LiteralPath $journal -Encoding UTF8
    } else { $queue.Add($entry) }
}
$manifests = $queue.ToArray()
$sessionId = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 6)
$totalInputs = $inventory.Count
$totalManifests = @($inventory | ForEach-Object { $_.Manifests.Count } | Measure-Object -Sum).Sum
Write-Output ('Inventory: {0} manifests, {1} distinct inputs, {2} reused, {3} queued.' -f $totalManifests, $totalInputs, @($results | Where-Object reused).Count, $queue.Count)
$next = 0
$started = [DateTime]::UtcNow
$watch = [Diagnostics.Stopwatch]::StartNew()
try {
    while ($next -lt $manifests.Count -or $pending.Count -gt 0) {
        while ($next -lt $manifests.Count -and $pending.Count -lt $Jobs) {
            $manifest = $manifests[$next]
            $jobDirectory = Join-Path $OutputDirectory ('{0}-{1:D5}' -f $sessionId, $next)
            New-Item -ItemType Directory -Path $jobDirectory | Out-Null
            $inputDirectory = Join-Path $jobDirectory 'input'
            New-Item -ItemType Directory -Path $inputDirectory | Out-Null
            $frozenManifest = Join-Path $inputDirectory ([IO.Path]::GetFileName($manifest.Original))
            [IO.File]::WriteAllText($frozenManifest, $manifest.Input.raw, (New-Object Text.UTF8Encoding($false)))
            foreach ($reference in $manifest.Input.references) {
                $destination = Join-Path $inputDirectory $reference.name
                if (-not [IO.File]::Exists($destination)) { [IO.File]::Copy($reference.path, $destination, $false) }
                if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $reference.sha256) {
                    throw ('Capture binary changed while inventory was frozen: ' + $reference.path)
                }
            }
            # An implicit sibling binary uses the manifest's original basename.
            $frozenMetadata = $manifest.Input.raw | ConvertFrom-Json
            if (-not $frozenMetadata.code_file) {
                $frozenMetadata | Add-Member -NotePropertyName code_file -NotePropertyValue $manifest.Input.references[0].name
                $frozenMetadata | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $frozenManifest -Encoding UTF8
            }
            [IO.File]::WriteAllText((Join-Path $inputDirectory 'identity.txt'), $manifest.Input.canonical)
            $stdout = Join-Path $jobDirectory 'stdout.txt'
            $stderr = Join-Path $jobDirectory 'stderr.txt'
            $startInfo = New-Object Diagnostics.ProcessStartInfo
            $startInfo.FileName = $Auditor
            $arguments = @($AuditorArgumentPrefix) + @('--audit-shader', $frozenManifest)
            if ($EmitSpirv) { $arguments += '--emit-spirv' }
            if ($profileSnapshot) {
                $arguments += @('--compute-host-profile', $profileSnapshot)
            }
            $startInfo.Arguments = ($arguments | ForEach-Object { Quote-NativeArgument $_ }) -join ' '
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
                $stdoutStream = [IO.File]::Open($stdout, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
                $stderrStream = [IO.File]::Open($stderr, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
                $stdoutTask = $process.StandardOutput.BaseStream.CopyToAsync($stdoutStream)
                $stderrTask = $process.StandardError.BaseStream.CopyToAsync($stderrStream)
            } catch {
                $null = Stop-AuditProcess $process
                if ($stdoutStream) { $stdoutStream.Dispose() }
                if ($stderrStream) { $stderrStream.Dispose() }
                $process.Dispose()
                throw
            }
            $pending.Add([pscustomobject]@{
                Process = $process; Manifest = $manifest.Original; Entry = $manifest; Directory = $jobDirectory
                Arguments = $arguments; FrozenManifest = $frozenManifest
                Stdout = $stdout; Stderr = $stderr; Started = [DateTime]::UtcNow
                StdoutStream = $stdoutStream; StderrStream = $stderrStream
                StdoutTask = $stdoutTask; StderrTask = $stderrTask
            })
            $next++
        }
        foreach ($job in @($pending.ToArray())) {
            $elapsed = ([DateTime]::UtcNow - $job.Started).TotalSeconds
            $timedOut = -not $job.Process.HasExited -and $elapsed -ge $TimeoutSeconds
            $outputLimited = $job.StdoutStream.Length + $job.StderrStream.Length -gt $MaxOutputMegabytes * 1MB
            if (($timedOut -or $outputLimited) -and -not (Stop-AuditProcess $job.Process)) {
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
            $phase = if ($payload -and $payload.phase) { $payload.phase } elseif ($phaseMatches.Count) { $phaseMatches[$phaseMatches.Count - 1].Groups[1].Value } else { 'input' }
            $status = if ($timedOut) { 'timeout' } elseif ($outputLimited) { 'output_limit' } elseif ($capturedOutput.ReadError) { 'failed' } elseif ($null -ne $payload) { $payload.status } else { 'failed' }
            if ($status -notin @('passed', 'failed', 'input_error', 'timeout', 'output_limit')) { $status = 'failed' }
            if ($job.Process.ExitCode -ne 0 -and $status -eq 'passed') { $status = 'failed' }
            $errorText = ''
            if ($status -ne 'passed') {
                if ($timedOut) { $errorText = 'worker timeout' }
                elseif ($outputLimited) { $errorText = 'worker output limit exceeded' }
                elseif ($capturedOutput.ReadError) { $errorText = $capturedOutput.ReadError }
                elseif ($payload -and $payload.error) { $errorText = $payload.error }
                elseif ($payload -and $payload.materialization_error) { $errorText = $payload.materialization_error }
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
            $replayCommand = '& ' + (Quote-PowerShellArgument $PSCommandPath) +
                ' -CorpusDirectory ' + (Quote-PowerShellArgument (Split-Path -Parent $job.FrozenManifest)) +
                ' -Auditor ' + (Quote-PowerShellArgument $Auditor) +
                ' -Jobs 1 -TimeoutSeconds ' + $TimeoutSeconds + ' -MaxOutputMegabytes ' + $MaxOutputMegabytes
            if ($EmitSpirv) { $replayCommand += ' -EmitSpirv' }
            if ($profileSnapshot) { $replayCommand += ' -ComputeHostProfile ' + (Quote-PowerShellArgument $profileSnapshot) }
            if ($AuditorArgumentPrefix.Count) {
                $replayCommand += ' -AuditorArgumentPrefix @(' + (($AuditorArgumentPrefix | ForEach-Object { Quote-PowerShellArgument $_ }) -join ',') + ')'
            }
            $row = [pscustomobject]@{
                compile_input_id = $job.Entry.Input.id; audit_key = $job.Entry.Key
                manifest = $job.Manifest; status = $status; phase = $phase
                manifests = $job.Entry.Manifests.ToArray(); reused = $false
                capture_attempts = $job.Entry.Attempts.ToArray()
                category = $(if ($status -ne 'passed') { Get-FailureCategory $phase $status $errorText } else { $null })
                exit_code = $job.Process.ExitCode; timed_out = $timedOut; output_limited = $outputLimited
                elapsed_seconds = [Math]::Round($elapsed, 3); error = $errorText
                result = $payload; stdout = $job.Stdout; stderr = $job.Stderr
                frozen_manifest = $job.FrozenManifest
                replay_command = $replayCommand
                auditor_command = '& ' + ((@($Auditor) + $job.Arguments | ForEach-Object { Quote-PowerShellArgument $_ }) -join ' ')
            }
            $results.Add($row)
            $row | ConvertTo-Json -Depth 100 -Compress | Add-Content -LiteralPath $journal -Encoding UTF8
            $job.Process.Dispose()
            $null = $pending.Remove($job)
            if ($results.Count % 25 -eq 0 -or $results.Count -eq $totalInputs) {
                Write-AtomicText (Join-Path $OutputDirectory 'checkpoint.json') ($results.ToArray() | ConvertTo-Json -Depth 100)
                Write-Output ('Audited {0}/{1} distinct inputs' -f $results.Count, $totalInputs)
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
# Preserve independent planner failures as independent groups. Conditional
# possible_errors are retained in each profile, never counted as proven rejects.
$precheckReasons = @($failed | ForEach-Object {
    $row = $_
    foreach ($reason in @($row.result.compute_execution_precheck.errors)) {
        if ($null -ne $reason) {
            $groupReason = [string]$reason -replace '\bat pc\s+[0-9]+\b', 'at pc <instruction>'
            [pscustomobject]@{ reason = $groupReason; manifest = $row.manifest }
        }
    }
})
$groups += @($precheckReasons | Group-Object -Property reason | ForEach-Object {
    $shaders = @($_.Group.manifest | Select-Object -Unique)
    [pscustomobject]@{
        count = $shaders.Count; distinct_shaders = $shaders.Count
        reason = 'resource_plan: compute_execution_precheck: ' + $_.Name; category = 'resource_plan'; manifests = $shaders
    }
})
$groups += @($failed | Where-Object {
    -not $_.result.unsupported_instructions.Count -and
    -not $_.result.compute_execution_precheck.errors.Count
} | Group-Object -Property {
    $text = $_.error -replace 'hash=0x[0-9a-fA-F]+', 'hash=<shader>'
    $text = $text -replace '\bpc(?:\s*=\s*|\s+)0x[0-9a-fA-F]+', 'pc=<instruction>'
    $text = $text -replace '0x[0-9a-fA-F]+(?=: unsupported)', '<instruction>'
    $text = $text -replace ' in [^\r\n]+:\d+', ''
    $_.category + ': ' + $text
} | ForEach-Object {
    $shaders = @($_.Group.manifest | Select-Object -Unique)
    [pscustomobject]@{ count = $shaders.Count; distinct_shaders = $shaders.Count; reason = $_.Name; manifests = $shaders }
})
$groups = @($groups | Sort-Object Count -Descending)
foreach ($group in $groups) {
    $affected = @($results | Where-Object { $_.manifest -in $group.manifests })
    $aliases = @($affected | ForEach-Object { $_.manifests } | Select-Object -Unique)
    $group | Add-Member -NotePropertyName compile_input_ids -NotePropertyValue @($affected.compile_input_id | Select-Object -Unique) -Force
    $group | Add-Member -NotePropertyName distinct_compile_inputs -NotePropertyValue $affected.Count -Force
    $group | Add-Member -NotePropertyName captured_manifest_count -NotePropertyValue $aliases.Count -Force
    $group | Add-Member -NotePropertyName replay_commands -NotePropertyValue @($affected.replay_command | Where-Object { $_ } | Select-Object -Unique) -Force
}
$coverage = @($results | Where-Object status -eq 'passed' | Group-Object -Property {
    if ($_.result.checked_through -eq 'cfg') {
        if ($_.result.dispatcher_fallback_required) { 'cfg_dispatcher_fallback_required' }
        else { 'cfg_structured' }
    } else { $_.result.checked_through }
} | ForEach-Object {
    [pscustomobject]@{ checked_through = $_.Name; count = $_.Count }
})
$precheckCoverage = @()
if ($profileSnapshot) {
    $precheckCoverage = @($results | Group-Object -Property {
        if ($null -ne $_.result.compute_execution_precheck) {
            $_.result.compute_execution_precheck.status
        } else { 'not_checked' }
    } | ForEach-Object { [pscustomobject]@{ status = $_.Name; count = $_.Count } })
}
$report = [ordered]@{
    schema_version = 2; audit_identity = $auditIdentity; audit_identity_sha256 = $auditIdentityHash
    corpus = $CorpusDirectory; auditor = $Auditor
    auditor_sha256 = $auditorHash
    started_utc = $started.ToString('o'); elapsed_seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
    total = $results.Count; passed = $results.Count - $failed.Count; failed = $failed.Count
    captured_manifests = $totalManifests; distinct_compile_inputs = $totalInputs
    ignored_non_shader_json = $ignoredJson.ToArray()
    distinct_code_binaries = @($inventory | Where-Object { -not $_.InputError } | ForEach-Object { $_.Input.references[0].sha256 } | Select-Object -Unique).Count
    reused = @($results | Where-Object reused).Count
    executed = @($results | Where-Object { -not $_.reused -and $_.status -ne 'input_error' }).Count
    input_errors = @($results | Where-Object status -eq 'input_error').Count
    status_counts = @($results | Group-Object status | ForEach-Object { [pscustomobject]@{ status = $_.Name; count = $_.Count } })
    failure_categories = @($failed | Group-Object category | ForEach-Object { [pscustomobject]@{ category = $_.Name; count = $_.Count } })
    error_group_memberships = [int](@($groups | Measure-Object count -Sum).Sum)
    error_group_counts_overlap = $true
    coverage = $coverage; error_groups = $groups
    compute_host_profile = $profileSnapshot; compute_host_profile_sha256 = $profileHash
    compute_execution_precheck_coverage = $precheckCoverage
    limitations = @('Coverage is recorded per result; decode/CFG/resource planning is not SPIR-V or GPU proof.', 'Missing replay inputs are input_error, not an emulator semantic failure.', 'EmitSpirv uses captured inputs for isolated-stage unoptimized emission and production SPIR-V validation; no driver pipeline or GPU work is run.', 'Optional compute_execution_precheck is before specialization; possible_errors are conditional and are not proven rejects.', 'Error groups overlap; do not sum group counts to obtain the number of failed distinct inputs.', 'Counts distinguish capture manifests, distinct inputs and distinct code binaries; shader hash alone is never a dedup key. Distinct inputs identify captured variants and do not imply complete runtime captures.', 'A captured pending phase means the call did not return; it is not a proven failure cause. Offline results supply the classification.')
    results = $results.ToArray()
}
Write-AtomicText (Join-Path $OutputDirectory 'report.json') ($report | ConvertTo-Json -Depth 100)
$markdown = New-Object 'System.Collections.Generic.List[string]'
$markdown.Add('# Shader batch audit')
$markdown.Add('')
$markdown.Add(('Checked {0}; passed {1}; failed {2}; elapsed {3}s.' -f $report.total, $report.passed, $report.failed, $report.elapsed_seconds))
$markdown.Add(('Captures {0}; distinct inputs {1}; reused {2}; executed {3}; input errors {4}.' -f $totalManifests, $totalInputs, $report.reused, $report.executed, $report.input_errors))
$markdown.Add('')
$markdown.Add('Coverage is explicit per result. Missing capture inputs are input_error. No driver pipeline, GPU execution or visible game outcome is checked. Cause groups overlap and their counts must not be summed.')
$markdown.Add('')
foreach ($item in $coverage) { $markdown.Add(('- {0}: {1}' -f $item.checked_through, $item.count)) }
foreach ($item in $precheckCoverage) { $markdown.Add(('- compute_execution_precheck {0}: {1}' -f $item.status, $item.count)) }
if ($profileSnapshot) {
    $markdown.Add('Compute precheck is before resource specialization; not_rejected is conditional, and not_checked entries did not reach this phase. SPIR-V, runtime descriptors and GPU execution are not verified.')
}
$markdown.Add('')
foreach ($group in $groups) {
    $markdown.Add(('## {0} shader(s)' -f $group.count))
    $markdown.Add('')
    $markdown.Add('```text')
    $markdown.Add($group.reason)
    $markdown.Add('```')
    $markdown.Add('')
    $markdown.Add(('Example: {0}' -f $group.manifests[0]))
    $markdown.Add(('Distinct inputs: {0}; capture manifests: {1}.' -f $group.distinct_compile_inputs, $group.captured_manifest_count))
    foreach ($command in $group.replay_commands) { $markdown.Add('```powershell'); $markdown.Add($command); $markdown.Add('```') }
    foreach ($example in @($group.pc_examples)) {
        if ($null -ne $example) { $markdown.Add(('- {0}: {1} {2}' -f $example.manifest, $example.pc, $example.instruction)) }
    }
    $markdown.Add('')
}
Write-AtomicText (Join-Path $OutputDirectory 'report.md') ($markdown -join "`n")
$replays = @($failed | Where-Object replay_command | ForEach-Object { $_.replay_command })
$replayScript = New-Object 'System.Collections.Generic.List[string]'
$replayScript.Add('# Run one failed input at a time; retained inputs, bounds and host profile are used.')
$replayScript.Add('$auditReplayFailures = 0')
foreach ($command in $replays) {
    $replayScript.Add($command)
    $replayScript.Add('if ($LASTEXITCODE -ne 0) { $auditReplayFailures++ }')
}
$replayScript.Add('if ($auditReplayFailures -gt 0) { exit 1 }')
Write-AtomicText (Join-Path $OutputDirectory 'replay-failed.ps1') ($replayScript -join "`n")
Write-Output ('Report: ' + (Join-Path $OutputDirectory 'report.json'))
Write-Output ('Passed {0}, failed {1}' -f $report.passed, $report.failed)
if ($failed.Count -gt 0) { exit 1 }
