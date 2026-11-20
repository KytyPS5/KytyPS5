$prs = Get-Content "open_prs.json" | ConvertFrom-Json
$mainRev = (git rev-parse HEAD).Trim()

$results = @()

foreach ($p in $prs) {
    $num = $p.number
    $refName = "pr-$num"
    
    # Check if local ref exists
    $rev = (git rev-parse --verify --quiet "$refName")
    if (-not $rev) {
        continue
    }
    $rev = $rev.Trim()

    # Check if already merged
    $mb = (git merge-base HEAD $rev).Trim()
    if ($mb -eq $rev) {
        $results += [PSCustomObject]@{
            Number = $num
            Title = $p.title
            User = $p.user
            Status = "ALREADY_MERGED"
        }
        continue
    }

    # Test merge-tree
    $mergeTree = (git merge-tree HEAD $rev 2>&1)
    $hasConflict = ($LASTEXITCODE -ne 0) -or ($mergeTree -match "<<<<<<<")

    if ($hasConflict) {
        $results += [PSCustomObject]@{
            Number = $num
            Title = $p.title
            User = $p.user
            Status = "CONFLICT"
        }
    } else {
        $results += [PSCustomObject]@{
            Number = $num
            Title = $p.title
            User = $p.user
            Status = "CLEAN"
        }
    }
}

$results | Format-Table -AutoSize
$clean = $results | Where-Object { $_.Status -eq "CLEAN" }
Write-Host "Found $($clean.Count) cleanly mergeable PRs out of fetched!"
$clean | ForEach-Object { "$($_.Number) | $($_.Title)" }
