# WoL spinac - pomocnik na PC
# ===========================
# Posloucha v domaci siti na prikazy od ESP32 a umi PC uspat nebo vypnout.
# Instaluje se pres install.ps1 (bezi pak jako naplanovana uloha pod SYSTEM).
#
# Kazdy prikaz je podepsany heslem (HMAC-SHA256) a obsahuje cas. Pomocnik
# prijme jen prikaz s platnym podpisem, ne starsi nez 2 minuty a jen jednou,
# takze zachyceny prikaz nejde pouzit znovu. Heslo samotne po siti nejde.
#
# Spusteni na zkousku (nic nevypne ani neuspi, jen zapisuje do logu):
#   powershell -ExecutionPolicy Bypass -File wol-helper.ps1 -Secret <heslo> -DryRun

param(
    [int]$Port = 8766,
    [string]$Secret = "",
    [string]$DataDir = (Join-Path $env:ProgramData "WoLSpinac"),
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$MaxClockSkewSeconds = 120

if (-not (Test-Path $DataDir)) { New-Item -ItemType Directory -Path $DataDir | Out-Null }
$LogPath = Join-Path $DataDir "helper.log"

function Write-Log([string]$Text) {
    $line = "{0}  {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Text
    try {
        if ((Test-Path $LogPath) -and (Get-Item $LogPath).Length -gt 1MB) {
            Move-Item $LogPath "$LogPath.old" -Force
        }
        Add-Content -Path $LogPath -Value $line -Encoding ASCII
    } catch { }
    Write-Host $line
}

if (-not $Secret) {
    $secretFile = Join-Path $DataDir "secret.txt"
    if (Test-Path $secretFile) { $Secret = (Get-Content $secretFile -TotalCount 1).Trim() }
}
if (-not $Secret) {
    Write-Log "CHYBA: chybi heslo (secret.txt nebo -Secret). Koncim."
    exit 1
}

$hmac = New-Object System.Security.Cryptography.HMACSHA256
$hmac.Key = [Text.Encoding]::UTF8.GetBytes($Secret)

function Get-Signature([string]$Message) {
    $bytes = $hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes($Message))
    return -join ($bytes | ForEach-Object { $_.ToString("x2") })
}

# Porovnani bez predcasneho ukonceni (aby z doby odpovedi neslo hadat podpis).
function Test-SameText([string]$A, [string]$B) {
    if ($A.Length -ne $B.Length) { return $false }
    $diff = 0
    for ($i = 0; $i -lt $A.Length; $i++) { $diff = $diff -bor ([int][char]$A[$i] -bxor [int][char]$B[$i]) }
    return $diff -eq 0
}

# Podpisy, ktere uz byly pouzite (podpis -> unix cas), aby nesly poslat znovu.
$usedSignatures = @{}

function Send-Response($Stream, [int]$Code, [string]$Body) {
    $reason = @{ 200 = "OK"; 400 = "Bad Request"; 401 = "Unauthorized"; 404 = "Not Found" }[$Code]
    $text = "HTTP/1.0 $Code $reason`r`nContent-Type: text/plain`r`nContent-Length: $($Body.Length)`r`nConnection: close`r`n`r`n$Body"
    $bytes = [Text.Encoding]::ASCII.GetBytes($text)
    $Stream.Write($bytes, 0, $bytes.Length)
    $Stream.Flush()
}

function Invoke-PowerAction([string]$Action) {
    if ($DryRun) {
        Write-Log "DryRun: ted bych provedl '$Action'."
        return
    }
    if ($Action -eq "shutdown") {
        # /f = zavrit i programy, ktere by vypnuti blokovaly (u PC nikdo nesedi)
        & shutdown.exe /s /f /t 0 /c "WoL spinac: vypnuti na dalku"
    } elseif ($Action -eq "sleep") {
        Add-Type -AssemblyName System.Windows.Forms
        [void][System.Windows.Forms.Application]::SetSuspendState([System.Windows.Forms.PowerState]::Suspend, $false, $false)
    }
}

# Zpracuje jedno spojeni. Vraci akci, ktera se ma provest po odeslani odpovedi.
function Read-Request($Client) {
    $Client.ReceiveTimeout = 3000
    $Client.SendTimeout = 3000
    $remote = $Client.Client.RemoteEndPoint.Address.ToString()
    $stream = $Client.GetStream()
    $reader = New-Object System.IO.StreamReader($stream, [Text.Encoding]::ASCII)

    $requestLine = $reader.ReadLine()
    $headers = @{}
    for ($i = 0; $i -lt 30; $i++) {
        $line = $reader.ReadLine()
        if ([string]::IsNullOrEmpty($line)) { break }
        $colon = $line.IndexOf(":")
        if ($colon -gt 0) { $headers[$line.Substring(0, $colon).Trim()] = $line.Substring($colon + 1).Trim() }
    }

    if ($requestLine -notmatch '^POST /(status|sleep|shutdown) HTTP/1\.[01]$') {
        Send-Response $stream 404 "unknown"
        Write-Log "Odmitnuto ($remote): neznamy pozadavek '$requestLine'"
        return $null
    }
    $action = $Matches[1]
    $time = $headers["X-Time"]
    $sig = $headers["X-Sig"]

    $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    [long]$sent = 0
    if (-not $time -or -not $sig -or -not [long]::TryParse($time, [ref]$sent)) {
        Send-Response $stream 400 "missing"
        Write-Log "Odmitnuto ($remote): chybi cas nebo podpis"
        return $null
    }
    if ([Math]::Abs($now - $sent) -gt $MaxClockSkewSeconds) {
        Send-Response $stream 401 "time"
        Write-Log "Odmitnuto ($remote): cas $sent se lisi od casu PC $now o vic nez $MaxClockSkewSeconds s"
        return $null
    }
    if (-not (Test-SameText (Get-Signature "${action}:$time") $sig.ToLower())) {
        Send-Response $stream 401 "signature"
        Write-Log "Odmitnuto ($remote): spatny podpis (nesedi heslo?)"
        return $null
    }
    if ($usedSignatures.ContainsKey($sig)) {
        Send-Response $stream 401 "replay"
        Write-Log "Odmitnuto ($remote): prikaz uz byl jednou pouzity"
        return $null
    }

    # Zapamatovat podpis a zapomenout ty, ktere uz stejne propadly.
    $usedSignatures[$sig] = $sent
    foreach ($old in @($usedSignatures.Keys)) {
        if ($now - $usedSignatures[$old] -gt 2 * $MaxClockSkewSeconds) { $usedSignatures.Remove($old) }
    }

    Send-Response $stream 200 "ok"
    if ($action -ne "status") { Write-Log "Prikaz '$action' od $remote" }
    return $action
}

$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Any, $Port)
$listener.Start()
Write-Log ("Pomocnik bezi na portu {0}{1}." -f $Port, $(if ($DryRun) { " (DryRun - nic nevypne)" } else { "" }))

while ($true) {
    $client = $listener.AcceptTcpClient()
    $action = $null
    try {
        $action = Read-Request $client
    } catch {
        Write-Log "Chyba pri cteni pozadavku: $($_.Exception.Message)"
    } finally {
        $client.Close()
    }
    if ($action -and $action -ne "status") {
        try { Invoke-PowerAction $action } catch { Write-Log "Chyba pri '$action': $($_.Exception.Message)" }
    }
}
