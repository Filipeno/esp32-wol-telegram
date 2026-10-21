# WoL spinac - priprava Windows (Wake-on-LAN) a vypis MAC/IP pro nastaveni ESP32
# =====================================================================
# Spust v PowerShellu JAKO SPRAVCE (pravym na Start -> Terminal (spravce)):
#   irm https://raw.githubusercontent.com/Filipeno/esp32-wol-telegram/main/scripts/windows-setup.ps1 | iex
# nebo ze stazeneho repozitare:
#   powershell -ExecutionPolicy Bypass -File scripts\windows-setup.ps1
#
# Co to udela (u kazde zmeny se zepta):
#  1. najde kabelovou sitovku a vypise jeji MAC a IP
#  2. zapne probouzeni po siti (Magic Packet) a vypne usporu u sitovky
#  3. vypne Rychle spusteni Windows
#  4. povoli prichozi ping z mistni site (ESP32 podle nej pozna, jestli PC bezi)
#  5. volitelne nainstaluje pomocnika pro uspani/vypnuti z Telegramu a vypise jeho heslo
# BIOS a router ti skript neudela - na konci vypise, co zbyva.

$ErrorActionPreference = "Stop"
$Repo = "https://raw.githubusercontent.com/Filipeno/esp32-wol-telegram/main"

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Spust tento skript jako spravce (pravym na Start -> Terminal (spravce))." -ForegroundColor Red
    return
}

function Ask([string]$question) {
    $a = Read-Host "$question [A/n]"
    return ($a -eq "" -or $a -match '^[aAyY]')
}

# --- 1. Kabelova sitovka ---
$adapter = Get-NetAdapter -Physical | Where-Object {
    $_.Status -eq "Up" -and $_.MediaType -eq "802.3" -and $_.InterfaceDescription -notmatch "Wi-?Fi|Wireless|Bluetooth|Virtual|VPN|Tailscale|Hyper-V"
} | Select-Object -First 1
if (-not $adapter) {
    Write-Host "Nenasel jsem pripojenou kabelovou sitovku. PC musi byt pripojene kabelem (Wi-Fi budit nejde)." -ForegroundColor Red
    return
}
$ip = (Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 | Select-Object -First 1).IPAddress
$mac = $adapter.MacAddress.Replace("-", ":")
Write-Host ""
Write-Host "Kabelova sitovka: $($adapter.InterfaceDescription)"
Write-Host "  MAC: $mac"
Write-Host "  IP:  $ip"
Write-Host ""

# --- 2. Probouzeni po siti ---
if (Ask "Zapnout probouzeni po siti u teto sitovky (Magic Packet) a vypnout usporu?") {
    foreach ($kw in @(@("Wake on Magic Packet", "Enabled"), @("Shutdown Wake-On-Lan", "Enabled"),
                      @("Energy Efficient Ethernet", "Disabled"), @("Green Ethernet", "Disabled"))) {
        $prop = Get-NetAdapterAdvancedProperty -Name $adapter.Name -ErrorAction SilentlyContinue |
            Where-Object { $_.DisplayName -like "*$($kw[0])*" } | Select-Object -First 1
        if ($prop) {
            try { Set-NetAdapterAdvancedProperty -Name $adapter.Name -DisplayName $prop.DisplayName -DisplayValue $kw[1]; Write-Host "  $($prop.DisplayName) = $($kw[1])" }
            catch { Write-Host "  $($prop.DisplayName): nepodarilo se nastavit na $($kw[1]) (nevadi)" -ForegroundColor Yellow }
        }
    }
    try { Enable-NetAdapterPowerManagement -Name $adapter.Name -WakeOnMagicPacket Enabled -ErrorAction Stop } catch { }
    & powercfg.exe /deviceenablewake "$($adapter.InterfaceDescription)" | Out-Null
}

# --- 3. Rychle spusteni ---
if (Ask "Vypnout Rychle spusteni Windows (jinak se PC po vypnuti nemusi probudit)?") {
    Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power" -Name HiberbootEnabled -Value 0
    Write-Host "  Rychle spusteni vypnuto."
}

# --- 4. Ping ---
if (-not (Get-NetFirewallRule -DisplayName "WoL spinac - ping" -ErrorAction SilentlyContinue)) {
    if (Ask "Povolit prichozi ping z mistni site (ESP32 podle nej pozna, jestli PC bezi)?") {
        New-NetFirewallRule -DisplayName "WoL spinac - ping" -Direction Inbound -Protocol ICMPv4 -IcmpType 8 `
            -RemoteAddress LocalSubnet -Action Allow -Profile Any | Out-Null
        Write-Host "  Ping povolen."
    }
}

# --- 5. Pomocnik (uspani/vypnuti) ---
$secret = ""
if (Ask "Chces PC z Telegramu take USPAT a VYPNOUT? (nainstaluje malou sluzbu na PC)") {
    $secret = -join ((1..32) | ForEach-Object { '{0:x}' -f (Get-Random -Maximum 16) })
    $dir = Join-Path ([IO.Path]::GetTempPath()) ("wol-helper-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $dir | Out-Null
    $local = if ($PSScriptRoot) { Join-Path $PSScriptRoot "..\pc-helper" } else { "" }
    foreach ($f in "install.ps1", "wol-helper.ps1") {
        if ($local -and (Test-Path (Join-Path $local $f))) { Copy-Item (Join-Path $local $f) $dir }
        else { Invoke-WebRequest "$Repo/pc-helper/$f" -OutFile (Join-Path $dir $f) -UseBasicParsing }
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $dir "install.ps1") -Secret $secret
    if ($LASTEXITCODE -ne 0) { $secret = ""; Write-Host "Pomocnika se nepodarilo nainstalovat, uspani/vypnuti nebude fungovat." -ForegroundColor Red }
}

# --- Shrnuti ---
Write-Host ""
Write-Host "================ Udaje pro nastavovaci stranku ESP32 ================" -ForegroundColor Green
Write-Host "  MAC adresa PC:  $mac"
Write-Host "  IP adresa PC:   $ip"
if ($secret) { Write-Host "  Heslo pomocnika: $secret" }
Write-Host "====================================================================="
Write-Host ""
Write-Host "Co zbyva udelat rucne:"
Write-Host "  - V routeru nastav DHCP rezervaci IP $ip pro MAC $mac (aby se IP nemenila)."
Write-Host "  - V BIOSu zapni probouzeni po siti (Wake on LAN / PCI-E wake) a vypni ErP / Deep Sleep."
Write-Host "  - Nahraj firmware: https://filipeno.github.io/esp32-wol-telegram/"
