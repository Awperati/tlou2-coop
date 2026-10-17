# Run once on PC B, in an ADMIN PowerShell, with the game running (so its folder can be found):
#   powershell -ExecutionPolicy Bypass -File setup-pcb-ssh.ps1
# Lets PC A (Claude) log in to this PC over the LAN with a key (no password) to restart the game,
# swap mod files and read logs. Undo: Stop-Service sshd; Set-Service sshd -StartupType Disabled;
# Unregister-ScheduledTask TLOU2-Game,TLOU2-Launcher -Confirm:$false
param([string]$HostPc = "192.168.0.237")
$ErrorActionPreference = "Stop"

Write-Host "1/5 Installing OpenSSH Server (a few minutes the first time)..."
$cap = Get-WindowsCapability -Online | Where-Object Name -like "OpenSSH.Server*"
if ($cap.State -ne "Installed") { Add-WindowsCapability -Online -Name $cap.Name | Out-Null }
Set-Service sshd -StartupType Automatic
Start-Service sshd

Write-Host "2/5 Allowing PC A's key..."
# The file drop serves it as binary, so Content comes back as bytes, not text.
$raw = (Invoke-WebRequest -UseBasicParsing "http://${HostPc}:8765/pc-a.pub").Content
if ($raw -is [byte[]]) { $raw = [Text.Encoding]::ASCII.GetString($raw) }
$key = ([string]$raw).Trim()
if ($key -notlike "ssh-ed25519 *") { throw "Unexpected key from PC A: $key" }
# Admin accounts read this file; plain accounts read ~\.ssh\authorized_keys. Write both.
$adminKeys = "$env:ProgramData\ssh\administrators_authorized_keys"
Set-Content -Path $adminKeys -Value $key -Encoding ascii
icacls $adminKeys /inheritance:r /grant "Administrators:F" /grant "SYSTEM:F" | Out-Null
$userSsh = "$env:USERPROFILE\.ssh"
New-Item -ItemType Directory -Force $userSsh | Out-Null
Set-Content -Path "$userSsh\authorized_keys" -Value $key -Encoding ascii

Write-Host "3/5 Firewall: SSH from the home network only..."
if (-not (Get-NetFirewallRule -DisplayName "TLOU2 SSH (LAN)" -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName "TLOU2 SSH (LAN)" -Direction Inbound -Protocol TCP -LocalPort 22 -Action Allow -Profile Private | Out-Null
}

Write-Host "4/5 Tasks that start the game and the co-op launcher on this desktop..."
$game = (Get-Process tlou-ii -ErrorAction SilentlyContinue | Select-Object -First 1).Path
if (-not $game) { throw "Start the game first so its folder can be found, then run this again." }
$dir = Split-Path $game
$user = "$env:USERDOMAIN\$env:USERNAME"
foreach ($t in @(@{n="TLOU2-Game"; exe=$game; arg="-nolauncher"}, @{n="TLOU2-Launcher"; exe="$dir\tlou2-coop\coop_launcher.exe"; arg=""})) {
    $a = if ($t.arg) { New-ScheduledTaskAction -Execute $t.exe -Argument $t.arg -WorkingDirectory (Split-Path $t.exe) } else { New-ScheduledTaskAction -Execute $t.exe -WorkingDirectory (Split-Path $t.exe) }
    $p = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    Register-ScheduledTask -TaskName $t.n -Action $a -Principal $p -Force | Out-Null
}

Write-Host "5/5 Done. Tell Claude these:"
$ip = (Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like "192.168.*" } | Select-Object -First 1).IPAddress
Write-Host "  user: $env:USERNAME"
Write-Host "  address: $ip"
Write-Host "  game folder: $dir"
