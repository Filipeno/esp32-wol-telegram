# WoL spinac - instalace pomocnika na PC (uspani a vypnuti z Telegramu)
# =====================================================================
# Spust v PowerShellu JAKO SPRAVCE (pravym na Start -> Terminal (spravce)):
#   cd <slozka projektu>\pc-helper
#   powershell -ExecutionPolicy Bypass -File .\install.ps1
#
# Co to udela:
#  1. zkopiruje wol-helper.ps1 do C:\ProgramData\WoLSpinac
#  2. ulozi tam heslo PC_HELPER_SECRET z ..\include\secrets.h (cist ho smi jen spravci)
#  3. vytvori naplanovanou ulohu "WoL spinac helper", ktera pomocnika spusti
#     pri kazdem startu PC (pod uctem SYSTEM, i bez prihlaseni)
#  4. povoli ve firewallu port 8766 - jen z domaci site
#  5. vyzkousi, ze pomocnik odpovida
#
# Odinstalace: uninstall.ps1

param(
    [string]$Secret = "",
    [int]$Port = 8766
)

$ErrorActionPreference = "Stop"
$TaskName = "WoL spinac helper"
$RuleName = "WoL spinac - helper"
$InstallDir = Join-Path $env:ProgramData "WoLSpinac"

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Spust tento skript jako spravce (pravym na Start -> Terminal (spravce))." -ForegroundColor Red
    exit 1
}

# --- Heslo ze secrets.h ---
if (-not $Secret) {
    $secretsPath = Join-Path $PSScriptRoot "..\include\secrets.h"
    if (Test-Path $secretsPath) {
        $m = Select-String -Path $secretsPath -Pattern '^\s*#define\s+PC_HELPER_SECRET\s+"([^"]*)"'
        if ($m) { $Secret = $m.Matches[0].Groups[1].Value }
    }
}
if (-not $Secret) {
    Write-Host "V include\secrets.h chybi PC_HELPER_SECRET (nebo je prazdne). Vypln ho a nahraj firmware do ESP32." -ForegroundColor Red
    exit 1
}

# --- Soubory ---
Write-Host "1/5 Kopiruji pomocnika do $InstallDir"
$task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if ($task) { Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue; Start-Sleep -Seconds 1 }
if (-not (Test-Path $InstallDir)) { New-Item -ItemType Directory -Path $InstallDir | Out-Null }
Copy-Item (Join-Path $PSScriptRoot "wol-helper.ps1") $InstallDir -Force

Write-Host "2/5 Ukladam heslo (pristup jen SYSTEM a spravci)"
$secretFile = Join-Path $InstallDir "secret.txt"
Set-Content -Path $secretFile -Value $Secret -Encoding ASCII
& icacls.exe $secretFile /inheritance:r /grant:r "*S-1-5-18:F" "*S-1-5-32-544:F" | Out-Null

# --- Naplanovana uloha ---
Write-Host "3/5 Vytvarim naplanovanou ulohu '$TaskName'"
$helperPath = Join-Path $InstallDir "wol-helper.ps1"
$action = New-ScheduledTaskAction -Execute "powershell.exe" `
    -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$helperPath`" -Port $Port"
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
    -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Principal $principal `
    -Settings $settings -Description "Uspani a vypnuti PC z Telegram bota (ESP32 WoL spinac)" -Force | Out-Null

# --- Firewall ---
Write-Host "4/5 Povoluji port $Port ve firewallu (jen z domaci site)"
Get-NetFirewallRule -DisplayName $RuleName -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName $RuleName -Direction Inbound -Protocol TCP -LocalPort $Port `
    -RemoteAddress LocalSubnet -Action Allow -Profile Any | Out-Null

# --- Spusteni a test ---
Write-Host "5/5 Spoustim a zkousim pomocnika"
Start-ScheduledTask -TaskName $TaskName

$hmac = New-Object System.Security.Cryptography.HMACSHA256
$hmac.Key = [Text.Encoding]::UTF8.GetBytes($Secret)
$ok = $false
for ($attempt = 0; $attempt -lt 10 -and -not $ok; $attempt++) {
    Start-Sleep -Seconds 1
    try {
        $time = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
        $sig = -join ($hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes("status:$time")) | ForEach-Object { $_.ToString("x2") })
        $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/status" -Method Post -UseBasicParsing -TimeoutSec 3 `
            -Headers @{ "X-Time" = "$time"; "X-Sig" = $sig }
        $ok = $r.StatusCode -eq 200
    } catch { }
}

if ($ok) {
    Write-Host ""
    Write-Host "Hotovo - pomocnik bezi. V Telegramu klikni na 'Stav': mel by ukazat 'Pomocnik na PC: bezi'." -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "Pomocnik neodpovida. Podivej se do $InstallDir\helper.log." -ForegroundColor Red
    exit 1
}
