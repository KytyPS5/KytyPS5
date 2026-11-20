$shell = New-Object -ComObject Shell.Application
$windows = $shell.Windows()
if ($windows.Count -gt 0) {
    $windows.Item(0).Document.Application.ShellExecute('C:\Users\Recko\Documents\github\kyty\_Build\windows\install\kyty_launcher.exe', '', 'C:\Users\Recko\Documents\github\kyty\_Build\windows\install', 'open', 1)
    Write-Host "Launcher launched successfully via Explorer window."
} else {
    # If no folder window is open, launch via explorer.exe process
    Start-Process -FilePath "explorer.exe" -ArgumentList "C:\Users\Recko\Documents\github\kyty\_Build\windows\install\kyty_launcher.exe"
    Write-Host "Launcher launched via explorer process."
}
