# Record the game client area for -Sec seconds, writing -Cmd lines to cmd.txt after -Delay ms.
# No focus changes: refuses unless the game is already in front.
param([string]$Out, [int]$Sec = 5, [string]$Cmd = "", [int]$Delay = 800,
      [string]$Game = "E:\Games\The Last of Us - Part II Remastered")
Add-Type @"
using System; using System.Runtime.InteropServices;
public class RecWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public static uint FgPid() { uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
}
"@
[RecWin]::SetProcessDPIAware() | Out-Null
$proc = Get-Process tlou-ii -ErrorAction Stop
if ([RecWin]::FgPid() -ne $proc.Id) { Write-Output "game not in front; not recording"; exit 2 }
$h = $proc.MainWindowHandle
$r = New-Object RecWin+RECT; [RecWin]::GetClientRect($h, [ref]$r) | Out-Null
$pt = New-Object RecWin+POINT; [RecWin]::ClientToScreen($h, [ref]$pt) | Out-Null
$w = ($r.R - $r.L) -band -2; $hh = ($r.B - $r.T) -band -2
$ff = Start-Process ffmpeg -ArgumentList "-v error -y -f gdigrab -framerate 20 -offset_x $($pt.X) -offset_y $($pt.Y) -video_size ${w}x${hh} -i desktop -t $Sec -vf scale=960:-2 -pix_fmt yuv420p `"$Out`"" -NoNewWindow -PassThru
Start-Sleep -Milliseconds $Delay
if ($Cmd) { Add-Content (Join-Path $Game "tlou2-coop\cmd.txt") ($Cmd -split ';' | ForEach-Object { $_.Trim() }) }
$ff.WaitForExit()
Write-Output "rec: $Out (${w}x${hh})"
