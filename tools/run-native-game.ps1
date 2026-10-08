param(
 [Parameter(Mandatory=$true)][string]$GameDirectory,
 [ValidateRange(128,16384)][int]$ScreenWidth=2560,
 [ValidateRange(128,16384)][int]$ScreenHeight=1440,
 [ValidateRange(1,3600)][int]$TimeoutSeconds=3600,
 [ValidateRange(1,128)][int]$MaxWorkingSetGiB=28,
 [switch]$Background,
 [switch]$DryRun
)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$install=Join-Path $root '_Build\windows\install'
$exe=Join-Path $install 'kyty_emulator.exe'
$hash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLower()
$game=(Resolve-Path -LiteralPath $GameDirectory).Path.TrimEnd('\')
if($game.Contains('"')){throw 'Invalid game directory'}
$gameExe=Join-Path $game 'eboot.bin'
$gameHash=(Get-FileHash -LiteralPath $gameExe -Algorithm SHA256).Hash.ToLower()
$arguments='--game "'+$game+'" --redzone --fullscreen --screen-width '+$ScreenWidth+' --screen-height '+$ScreenHeight+' --present-mode Fifo --printf-direction Silent --shader-log-direction Silent --graphics-debug-dump false --vulkan-validation false --shader-validation false --gpu-assisted-validation false'
if($DryRun){[ordered]@{executable=$exe;sha256=$hash;gameExecutable=$gameExe;gameSha256=$gameHash;arguments=$arguments;timeoutSeconds=$TimeoutSeconds;maxWorkingSetGiB=$MaxWorkingSetGiB;executes=$false}|ConvertTo-Json;exit 0}
if(Get-Process -Name kyty_emulator,ninja,MSBuild,clang-cl,cl,link,shader_recompiler_compute_tests,shader_cfg_tests,resource_tracking_tests,virtual_memory_allocation_tests -ErrorAction SilentlyContinue){throw 'Native execution slot busy'}
if($Background){
 $stamp=[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,6)
 $wrapper=Join-Path $root ('_Build\normal-supervisor-'+$stamp+'.ps1')
 $log=Join-Path $root ('_Build\normal-supervisor-'+$stamp+'.log')
 $scriptLiteral=$PSCommandPath.Replace("'","''");$gameLiteral=$game.Replace("'","''");$logLiteral=$log.Replace("'","''")
 $body="& '$scriptLiteral' -GameDirectory '$gameLiteral' -ScreenWidth $ScreenWidth -ScreenHeight $ScreenHeight -TimeoutSeconds $TimeoutSeconds -MaxWorkingSetGiB $MaxWorkingSetGiB *> '$logLiteral'"
 [IO.File]::WriteAllText($wrapper,$body)
 $worker=[Diagnostics.ProcessStartInfo]::new();$worker.FileName=Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
 $worker.Arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$wrapper+'"'
 $worker.UseShellExecute=$true;$worker.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
 $supervisor=[Diagnostics.Process]::Start($worker)
 try{[ordered]@{supervisorPid=$supervisor.Id;startedUtc=[DateTime]::UtcNow.ToString('o');wrapper=$wrapper;log=$log;timeoutSeconds=$TimeoutSeconds}|ConvertTo-Json}finally{$supervisor.Dispose()}
 exit 0
}
$runDir=Join-Path $root ('_Build\runs\game-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-normal-native-game')
$null=New-Item -ItemType Directory -Path $runDir
$si=[Diagnostics.ProcessStartInfo]::new();$si.FileName=$exe;$si.WorkingDirectory=$install
$si.Arguments=$arguments
$si.UseShellExecute=$false;$si.CreateNoWindow=$true;$si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true
foreach($name in @($si.EnvironmentVariables.Keys)){
 if(($name -like 'KYTY_*' -and $name -match 'DEBUG|DUMP|TRACE|READBACK|CAPTURE|DIAGNOSTIC|PROFILE') -or
    $name -match '^VK_LAYER_|^VK_INSTANCE_LAYERS$|^VK_LAYER_PATH$|^VK_IMPLICIT_LAYER_PATH$|^VK_LOADER_DEBUG$'){
  $si.EnvironmentVariables.Remove($name)
 }
}
if(-not ('NativeGameLogs' -as [type])){
 Add-Type @'
using System.IO;
using System.Text;
using System.Threading.Tasks;
public static class NativeGameLogs {
 public static async Task CopyLines(StreamReader input,string path) {
  using(var output=new StreamWriter(new FileStream(path,FileMode.CreateNew,FileAccess.Write,FileShare.Read),new UTF8Encoding(false))) {
   output.AutoFlush=true;
   string line;
   while((line=await input.ReadLineAsync())!=null)await output.WriteLineAsync(line);
  }
 }
}
'@
}
$p=[Diagnostics.Process]::new();$p.StartInfo=$si;$o=$null;$e=$null;$started=$false
$lock=$null
$r=[ordered]@{executable=$exe;sha256=$hash;runDirectory=$runDir;arguments=$si.Arguments;startedUtc=[DateTime]::UtcNow.ToString('o');timeoutSeconds=$TimeoutSeconds;maxWorkingSetGiB=$MaxWorkingSetGiB;diagnostics='disabled';redZoneProtection=$true;gameExecutable=$gameExe;gameSha256=$gameHash;timedOut=$false;memoryGuard=$false}
try{
 $lock=[IO.File]::Open((Join-Path $root '_Build\native-check.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
 if(Get-Process -Name kyty_emulator,ninja,MSBuild,clang-cl,shader_recompiler_compute_tests -ErrorAction SilentlyContinue){throw 'Native execution slot became busy'}
 [void]$p.Start();$started=$true;$r.pid=$p.Id;$o=[NativeGameLogs]::CopyLines($p.StandardOutput,(Join-Path $runDir 'stdout.txt'));$e=[NativeGameLogs]::CopyLines($p.StandardError,(Join-Path $runDir 'stderr.txt'))
 $r | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $runDir 'run.json')
 Write-Output ('NORMAL_GAME_STARTED pid='+$p.Id+' directory='+$runDir)
 $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
 while(-not $p.WaitForExit(1000)){
  $p.Refresh()
  if($p.WorkingSet64 -ge ($MaxWorkingSetGiB * 1GB)){$r.memoryGuard=$true;break}
  if([DateTime]::UtcNow -ge $deadline){$r.timedOut=$true;break}
 }
 }finally{
 try{
  if($started){
   if(-not $p.HasExited){$null=$p.CloseMainWindow();if(-not $p.WaitForExit(15000)){$p.Kill();[void]$p.WaitForExit(5000);$r.forcedCleanup=$true}}
   $pending=@($o,$e)|Where-Object {$null -ne $_}
   if($pending.Count -and -not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]$pending,5000)){throw 'Owned streams not drained'}
   $r.exitCode=$p.ExitCode
  }
 }finally{
  $p.Dispose();if($lock){$lock.Dispose()}
  $r.finishedUtc=[DateTime]::UtcNow.ToString('o');$r | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $runDir 'run.json')
 }
}
Write-Output ('NORMAL_GAME_FINISHED exit='+$r.exitCode)
