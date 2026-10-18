# Write a minidump of a running process (thread stacks + memory info) without stopping it.
param([int]$ProcId, [string]$Out)
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Dbg { [DllImport("dbghelp.dll", SetLastError=true)] public static extern bool MiniDumpWriteDump(IntPtr h, uint pid, IntPtr f, uint type, IntPtr e, IntPtr u, IntPtr c); }
"@
$p = Get-Process -Id $ProcId
$fs = [System.IO.File]::Create($Out)
# MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory
$ok = [Dbg]::MiniDumpWriteDump($p.Handle, [uint32]$ProcId, $fs.SafeFileHandle.DangerousGetHandle(), 0x1041, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
$fs.Close(); "dump $ok $Out"
