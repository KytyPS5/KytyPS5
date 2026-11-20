$prs = Get-Content "open_prs.json" | ConvertFrom-Json
$merged = @(2395, 1405, 1569, 2431, 1366, 1802, 1491, 1184, 2139, 2287, 2284, 2110, 2143, 2260, 2259, 1872, 2141, 1933, 2409, 716, 1856, 1865, 2026, 2094, 2119, 506, 2106)
$remaining = $prs | Where-Object { $merged -notcontains $_.number }
Write-Host "Remaining open PRs: $($remaining.Count)"
$remaining | ForEach-Object { "$($_.number) | $($_.title) | $($_.user)" } | Out-File -FilePath "remaining_prs.txt" -Encoding utf8
Write-Host "Saved remaining_prs.txt"
