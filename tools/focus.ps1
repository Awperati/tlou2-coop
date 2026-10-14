# Bring the game window to the front (it pauses itself when unfocused), optionally
# write mod commands, keep it in front for -Seconds, then print the matching log lines.
#   powershell -ExecutionPolicy Bypass -File tools\focus.ps1 -Cmd "pkgloaded x" -Seconds 8 -Grep "loadpkg"
param([string]$Cmd = "", [int]$Seconds = 5, [string]$Grep = "", [string]$Game = "E:\Games\The Last of Us - Part II Remastered")
Add-Type @"
using System; using System.Runtime.InteropServices;
public class FocusWin {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, UIntPtr e);
  public static uint FgPid() { uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
}
"@
$proc = Get-Process tlou-ii -ErrorAction Stop
$log = Join-Path $Game "tlou2-coop.log"
$start = (Get-Content $log).Count
for ($i = 0; $i -lt 5 -and [FocusWin]::FgPid() -ne $proc.Id; $i++) {
    [FocusWin]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [FocusWin]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
    [FocusWin]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 600
}
if ([FocusWin]::FgPid() -ne $proc.Id) { Write-Output "focus: failed" } else { Write-Output "focus: ok" }
Start-Sleep -Milliseconds 800
if ($Cmd) { $Cmd -split ';' | ForEach-Object { $_.Trim() } | Set-Content (Join-Path $Game "tlou2-coop\cmd.txt") }
Start-Sleep -Seconds $Seconds
$lines = Get-Content $log | Select-Object -Skip $start
if ($Grep) { $lines = $lines | Select-String -Pattern $Grep | ForEach-Object Line }
$lines
