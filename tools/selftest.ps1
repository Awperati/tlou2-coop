# Hands-free in-game test: focus the game, record its window, play a pad script,
# then save the clip, a contact sheet and the matching log lines.
#   powershell -ExecutionPolicy Bypass -File tools\selftest.ps1 [-Scenario follow] [-Seconds 20]
# Scenarios live in tools\scenarios\<name>.txt (cmd.txt lines: stick/press/wait/...).
# Needs: the game running with the mod loaded, ffmpeg on PATH. The game pauses when it
# loses focus, so this takes the foreground for the length of the run.
param(
    [string]$Scenario = "follow",
    [int]$Seconds = 20,
    [string]$Game = "E:\Games\The Last of Us - Part II Remastered",
    [string]$Out = "$PSScriptRoot\..\re\selftest"
)
$ErrorActionPreference = "Stop"
Add-Type @"
using System; using System.Runtime.InteropServices;
public class SelfTestWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public static uint FgPid() { uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
}
"@
[SelfTestWin]::SetProcessDPIAware() | Out-Null

$proc = Get-Process tlou-ii -ErrorAction SilentlyContinue
if (-not $proc) { throw "tlou-ii.exe is not running" }
$h = $proc.MainWindowHandle
$scenFile = Join-Path $PSScriptRoot "scenarios\$Scenario.txt"
if (-not (Test-Path $scenFile)) { throw "no scenario $scenFile" }

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$dir = Join-Path $Out "$stamp-$Scenario"
New-Item -ItemType Directory -Force $dir | Out-Null

# Focus (Alt tap lets SetForegroundWindow through the foreground lock).
for ($i = 0; $i -lt 5 -and [SelfTestWin]::FgPid() -ne $proc.Id; $i++) {
    [SelfTestWin]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [SelfTestWin]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
    [SelfTestWin]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 700
}
if ([SelfTestWin]::FgPid() -ne $proc.Id) { throw "could not focus the game window (foreground pid $([SelfTestWin]::FgPid()))" }
Start-Sleep -Milliseconds 800   # let the game unpause

# Client area in screen pixels, even sizes for the encoder.
$r = New-Object SelfTestWin+RECT; [SelfTestWin]::GetClientRect($h, [ref]$r) | Out-Null
$pt = New-Object SelfTestWin+POINT; [SelfTestWin]::ClientToScreen($h, [ref]$pt) | Out-Null
$w = ($r.R - $r.L) -band -2; $hh = ($r.B - $r.T) -band -2
$clip = Join-Path $dir "clip.mp4"
$ffArgs = "-y -v error -f gdigrab -framerate 15 -offset_x $($pt.X) -offset_y $($pt.Y) -video_size ${w}x${hh} -i desktop -t $Seconds -vf scale=960:-2 -c:v libx264 -preset ultrafast -crf 30 `"$clip`""
$logPath = Join-Path $Game "tlou2-coop.log"
$logStart = (Get-Content $logPath).Count
$ff = Start-Process ffmpeg -ArgumentList $ffArgs -PassThru -WindowStyle Hidden

Start-Sleep -Milliseconds 500
Get-Content $scenFile | Where-Object { $_ -and -not $_.StartsWith("#") } | Set-Content (Join-Path $Game "tlou2-coop\cmd.txt")
$ff.WaitForExit()

# Log lines written during the run, and a 2 fps contact sheet.
Get-Content $logPath | Select-Object -Skip $logStart | Set-Content (Join-Path $dir "log.txt")
& ffmpeg -v error -i $clip -vf "fps=2,scale=480:-1,tile=4x4" -q:v 4 (Join-Path $dir "sheet%02d.jpg")
Write-Output "selftest: $dir"
