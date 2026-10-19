# Logs the game's GPU and process memory once a minute, read-only, so a graphics glitch can be matched to a number
# (ROADMAP: PC B graphics corruption, 2026-10-20). Stops by itself after -Hours, or when the game exits.
#   powershell -NoProfile -File vramlog.ps1 [-Out <file>] [-Hours 3] [-Every 60]
param([string]$Out = "$env:TEMP\tlou2-vram.log", [double]$Hours = 3, [int]$Every = 60)
$until = (Get-Date).AddHours($Hours)
"{0} vramlog start, every {1} s until {2:HH:mm}" -f (Get-Date -Format 'HH:mm:ss'), $Every, $until | Add-Content -LiteralPath $Out
while ((Get-Date) -lt $until) {
    $p = Get-Process tlou-ii -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { "{0} game not running, stopping" -f (Get-Date -Format 'HH:mm:ss') | Add-Content -LiteralPath $Out; break }
    $ded = 0; $sh = 0
    $c = (Get-Counter '\GPU Process Memory(*)\Dedicated Usage', '\GPU Process Memory(*)\Shared Usage' -ErrorAction SilentlyContinue).CounterSamples |
        Where-Object { $_.InstanceName -match ('pid_' + $p.Id + '_') }
    foreach ($s in $c) { if ($s.Path -match 'dedicated') { $ded += $s.CookedValue } else { $sh += $s.CookedValue } }
    "{0} pid {1} vram {2:N2} GB shared {3:N2} GB ws {4:N2} GB private {5:N2} GB" -f (Get-Date -Format 'HH:mm:ss'), $p.Id,
        ($ded / 1GB), ($sh / 1GB), ($p.WorkingSet64 / 1GB), ($p.PrivateMemorySize64 / 1GB) | Add-Content -LiteralPath $Out
    Start-Sleep -Seconds $Every
}
