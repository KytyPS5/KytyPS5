$prs = Get-Content "open_prs.json" | ConvertFrom-Json
Write-Host "Scanning all $($prs.Count) open PRs against git history..."

$unmerged = @()

foreach ($p in $prs) {
    $num = $p.number
    # check if PR number is mentioned in any commit message in git log
    $found = git log --grep="PR #$num\b" --grep="#$num\b" -n 1 --oneline
    if (-not $found) {
        $unmerged += $p
    }
}

Write-Host "Actually unmerged PRs count: $($unmerged.Count)"
$unmerged | ForEach-Object { "$($_.number) | $($_.title) | by $($_.user)" } | Out-File -FilePath "truly_unmerged_prs.txt" -Encoding utf8
Write-Host "Saved truly_unmerged_prs.txt"
