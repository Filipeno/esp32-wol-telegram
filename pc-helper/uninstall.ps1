# WoL spinac - odinstalace pomocnika na PC
# Spust v PowerShellu JAKO SPRAVCE:
#   powershell -ExecutionPolicy Bypass -File .\uninstall.ps1

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

if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "Uloha '$TaskName' odstranena."
}
Get-NetFirewallRule -DisplayName $RuleName -ErrorAction SilentlyContinue | Remove-NetFirewallRule
Write-Host "Pravidlo firewallu odstraneno."
Start-Sleep -Seconds 1
if (Test-Path $InstallDir) {
    Remove-Item $InstallDir -Recurse -Force
    Write-Host "Slozka $InstallDir odstranena."
}
Write-Host "Hotovo - pomocnik je odinstalovany." -ForegroundColor Green
