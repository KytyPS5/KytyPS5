$prs = Get-Content "open_prs.json" | ConvertFrom-Json

$trulyMissing = @()

foreach ($p in $prs) {
    $num = $p.number
    $title = $p.title.Trim()
    
    # Strip issue number references like (#123)
    $cleanTitle = $title -replace '\s*\([#\d\w\s,]+\)$', ''
    
    # Search for first 25 chars of title in git log
    $prefix = $cleanTitle.Substring(0, [Math]::Min(25, $cleanTitle.Length))
    $found = git log --grep="$prefix" -n 1 --oneline
    if (-not $found) {
        $trulyMissing += $p
    }
}

Write-Host "Truly missing PRs from git log: $($trulyMissing.Count)"
$trulyMissing | ForEach-Object { "$($_.number) | $($_.title) | by $($_.user)" } | Out-File -FilePath "truly_missing_prs.txt" -Encoding utf8
Write-Host "Saved truly_missing_prs.txt"
