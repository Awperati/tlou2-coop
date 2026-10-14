# Focus the game and save one frame of its client area (ffmpeg gdigrab).
#   powershell -ExecutionPolicy Bypass -File tools\shot.ps1 [-Out re\shot.png] [-Cmd "press a 150"] [-Wait 1500]
param([string]$Out = "$PSScriptRoot\..\re\shot.png", [string]$Cmd = "", [int]$Wait = 1500,
      [string]$Game = "E:\Games\The Last of Us - Part II Remastered")
Add-Type @"
using System; using System.Runtime.InteropServices;
public class ShotWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public static uint FgPid() { uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
}
"@
[ShotWin]::SetProcessDPIAware() | Out-Null
$proc = Get-Process tlou-ii -ErrorAction Stop
$h = $proc.MainWindowHandle
for ($i = 0; $i -lt 5 -and [ShotWin]::FgPid() -ne $proc.Id; $i++) {
    [ShotWin]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [ShotWin]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
    [ShotWin]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 600
}
if ($Cmd) { $Cmd -split ';' | ForEach-Object { $_.Trim() } | Set-Content (Join-Path $Game "tlou2-coop\cmd.txt") }
Start-Sleep -Milliseconds $Wait
$r = New-Object ShotWin+RECT; [ShotWin]::GetClientRect($h, [ref]$r) | Out-Null
$pt = New-Object ShotWin+POINT; [ShotWin]::ClientToScreen($h, [ref]$pt) | Out-Null
$w = ($r.R - $r.L) -band -2; $hh = ($r.B - $r.T) -band -2
& ffmpeg -v error -y -f gdigrab -offset_x $pt.X -offset_y $pt.Y -video_size "${w}x${hh}" -i desktop -frames:v 1 -vf scale=960:-2 $Out
Write-Output "shot: $Out (${w}x${hh} at $($pt.X),$($pt.Y))"
