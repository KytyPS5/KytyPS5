$headers = @{ "User-Agent" = "KytyAgent" }
$p1 = Invoke-RestMethod -Uri "https://api.github.com/repos/KytyPS5/KytyPS5/pulls?state=open&per_page=100&page=1" -Headers $headers
$p2 = Invoke-RestMethod -Uri "https://api.github.com/repos/KytyPS5/KytyPS5/pulls?state=open&per_page=100&page=2" -Headers $headers
$p3 = Invoke-RestMethod -Uri "https://api.github.com/repos/KytyPS5/KytyPS5/pulls?state=open&per_page=100&page=3" -Headers $headers
$all = @($p1) + @($p2) + @($p3)
Write-Host "Total fetched: $($all.Count)"
$res = $all | Select-Object number, title, @{Name="head";Expression={$_.head.ref}}, @{Name="user";Expression={$_.user.login}}
$res | ConvertTo-Json -Depth 3 | Out-File -FilePath "open_prs.json" -Encoding utf8
Write-Host "Saved open_prs.json"
