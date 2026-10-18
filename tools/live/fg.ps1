Add-Type @'
using System; using System.Runtime.InteropServices;
public static class FgCheck { [StructLayout(LayoutKind.Sequential)] struct L { public uint cb; public uint t; }
[DllImport("user32.dll")] static extern bool GetLastInputInfo(ref L l);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow(); [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int p);
public static uint Ms() { L l = new L(); l.cb = 8; GetLastInputInfo(ref l); return (uint)Environment.TickCount - l.t; } }
'@
$q = 0; [void][FgCheck]::GetWindowThreadProcessId([FgCheck]::GetForegroundWindow(), [ref]$q)
"idle ms: $([FgCheck]::Ms()) fg: $((Get-Process -Id $q).ProcessName)"
