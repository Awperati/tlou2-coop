# A/B test of the remote body: for each setting, send the command, run the LAN sim and keep
# the log lines. Example:
#   .\tools\remote_ab.ps1 -Body npc-wat-bank-militia-1 -Toggle remotesnap -Values on,off
# Output: re\clips\ab_<toggle>.txt (one "== <value>" block per setting), plus a clip per
# setting when the game is in front. Summarise with: python tools/remote_ab.py re\clips\ab_<toggle>.txt
param(
    [string]$Body = "npc-wat-bank-militia-1",
    [string]$Toggle = "remotesnap",
    [string[]]$Values = @("on", "off"),
    [string]$Sim = "line -68.72 -6.8 -204.07 -0.87 8 1.5 127.0.0.1",
    [string[]]$Setup = @(),   # commands sent once before the runs, e.g. "remotesnap on"
    [switch]$Deploy
)
$g = "E:\Games\The Last of Us - Part II Remastered"
$c = "$g\tlou2-coop\cmd.txt"
$log = "$g\tlou2-coop.log"
$root = Split-Path $PSScriptRoot -Parent
if (-not ("U.Fg" -as [type])) {
    Add-Type -Name Fg -Namespace U -MemberDefinition '[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow(); [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint p);'
}
function Send-Coop($s, $w = 1.5) { Set-Content $c $s -Encoding ascii; Start-Sleep $w }
if ($Deploy) { & cmd /c "$root\deploy.cmd" | Select-Object -Last 1; Start-Sleep 4 }
$out = "$root\re\clips\ab_$Toggle.txt"
New-Item -ItemType Directory -Force "$root\re\clips" | Out-Null
"" | Set-Content $out -Encoding utf8
Send-Coop "puppet $Body"
Send-Coop "npcstate $Body"
foreach ($s in $Setup) { Send-Coop $s }
foreach ($v in $Values) {
    $fp = 0; [void][U.Fg]::GetWindowThreadProcessId([U.Fg]::GetForegroundWindow(), [ref]$fp)
    $front = (Get-Process -Id $fp -ErrorAction SilentlyContinue).ProcessName -eq "tlou-ii"
    $mark = (Get-Content $log).Count
    Send-Coop "$Toggle $v"
    $ff = $null
    if ($front) { $ff = Start-Process ffmpeg -ArgumentList "-loglevel error -y -f gdigrab -framerate 15 -offset_x 1095 -offset_y 38 -video_size 1440x810 -t 44 -i desktop -c:v libx264 -preset ultrafast re\clips\ab_${Toggle}_$v.mp4" -PassThru -WindowStyle Hidden -WorkingDirectory $root }
    $simProc = Start-Process -FilePath "$root\build\coop_lan_sim.exe" -ArgumentList $Sim -PassThru -WindowStyle Hidden
    $simProc.WaitForExit(60000) | Out-Null
    if ($ff) { $ff.WaitForExit(10000) | Out-Null }
    Start-Sleep 11
    "== $v" | Add-Content $out -Encoding utf8
    Get-Content $log | Select-Object -Skip $mark | Select-String "npcstate:|remote|teleport|taking|released|WARNING" | ForEach-Object { $_.Line } | Add-Content $out -Encoding utf8
}
"game: $((Get-Process tlou-ii -ErrorAction SilentlyContinue).Id)"
