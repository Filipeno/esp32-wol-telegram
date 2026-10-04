# Rychlá cesta: nastavení s pomocí Claude Code

Tenhle soubor obsahuje hotový prompt pro **Claude Code**. Claude tě provede celým nastavením WoL spínače,
sám udělá, co jde udělat z příkazové řádky, a u ostatního ti řekne, co přesně máš udělat.

## Jak na to

1. Naklonuj (nebo stáhni a rozbal) tento repozitář na PC, které chceš budit.
2. Připoj ESP32 USB kabelem k tomuto PC.
3. Otevři ve složce projektu terminál a spusť `claude`.
   - Na kroky, které mění nastavení Windows, je potřeba **terminál spuštěný jako správce** (pravým na Start → *Terminál (správce)*, pak `cd` do složky projektu).
4. Zkopíruj celý text z rámečku níže a vlož ho do Claude Code.

---

```text
Jsi můj průvodce nastavením projektu "WoL spínač přes Telegram" v této složce (ESP32 + PlatformIO).
Přečti si nejdřív README.md, include/secrets.example.h a platformio.ini, ať víš, o co jde.

Pravidla:
- Mluv se mnou česky a jednoduše, nejsem programátor.
- Postupuj krok po kroku. Na konci každého kroku shrň, co se udělalo, a POČKEJ, až napíšu "hotovo" nebo "pokračuj". Nikdy nedělej víc kroků najednou.
- Než cokoli změníš v nastavení Windows, řekni mi přesně co a proč, a zeptej se na souhlas.
- Soubor include/secrets.h obsahuje token bota. NIKDY ho necommituj, nepushuj, nevypisuj celý token do chatu ani ho nikam neposílej. Před jakýmkoli git commitem ověř, že je v .gitignore a není ve stagingu. Nic nepushuj, pokud o to výslovně nepožádám.
- Pokud nějaký příkaz selže, vysvětli mi lidsky, co se stalo, a navrhni opravu.

Kroky:

1) Nástroje. Zkontroluj, jestli mám git, Python 3 a PlatformIO Core (příkaz `pio`, případně
   %USERPROFILE%\.platformio\penv\Scripts\pio.exe). Co chybí, nabídni nainstalovat (winget pro git a Python,
   `pip install -U platformio` pro PlatformIO) a po mém souhlasu nainstaluj. Ověř verze.

2) MAC a IP adresa PC. Sám je zjisti přes PowerShell (Get-NetAdapter, Get-NetIPAddress) – zajímá nás
   KABELOVÝ (ethernet) adaptér, který je připojený (Status Up). Ignoruj Wi-Fi, Tailscale, vEthernet/Hyper-V,
   VPN a Bluetooth adaptéry. Pokud kabelový adaptér není připojený, upozorni mě, že PC musí být na kabelu.
   Ukaž mi nalezenou MAC a IP a zeptej se, jestli je mám použít.
   Pak mi jednoduše vysvětli, jak v routeru nastavit DHCP rezervaci této IP pro tuto MAC
   (adresu routeru zjisti z výchozí brány – Get-NetIPConfiguration), a počkej, až to udělám.

3) Telegram. Vysvětli mi, jak přes @BotFather založit bota (/newbot) a přes @userinfobot zjistit chat ID,
   a že mám u nového bota kliknout na Start. Pak se mě zeptej na token a chat ID (může jich být víc).
   Zkontroluj formát (token: číslice:znaky, chat ID: jen číslice, případně s mínusem).
   Zeptej se mě, jestli chci PC z Telegramu i uspávat a vypínat (potřebuje to malého pomocníka na PC, krok 5e v README).
   Vytvoř include/secrets.h podle include/secrets.example.h s mými údaji. OTA_PASSWORD (nahrávání firmwaru přes Wi-Fi)
   vygeneruj sám jako náhodné heslo (aspoň 20 znaků); PC_HELPER_SECRET vygeneruj jako 32 náhodných hex znaků,
   jen pokud chci uspávání/vypínání, jinak nech "". Hesla ani token mi nevypisuj zpátky celé.
   Ověř, že `git check-ignore include/secrets.h` soubor opravdu ignoruje.

4) Wake-on-LAN ve Windows. Nejdřív jen ZKONTROLUJ a ukaž mi stav:
   - Get-NetAdapterAdvancedProperty u kabelového adaptéru (Wake on Magic Packet, Shutdown Wake-On-Lan,
     Energy Efficient Ethernet / Green Ethernet a podobné – názvy se liší podle výrobce),
   - Get-NetAdapterPowerManagement (WakeOnMagicPacket, DeviceSleepOnDisconnect),
   - `powercfg /devicequery wake_armed` (smí síťovka budit PC?),
   - Rychlé spuštění: registr HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power, hodnota HiberbootEnabled,
   - jestli firewall povoluje příchozí ping (ICMPv4 echo) z místní sítě.
   Pak mi navrhni změny a po mém souhlasu je proveď (potřebuje to práva správce – pokud je nemáš, řekni mi,
   jak tě spustit jako správce):
   - Set-NetAdapterAdvancedProperty: Wake on Magic Packet = Enabled, Shutdown Wake-On-Lan = Enabled (pokud existuje),
     Energy Efficient Ethernet / Green Ethernet = Disabled (pokud existuje),
   - Enable-NetAdapterPowerManagement -WakeOnMagicPacket, `powercfg /deviceenablewake "<název zařízení>"`,
   - vypnout Rychlé spuštění (HiberbootEnabled = 0),
   - firewall pravidlo: New-NetFirewallRule -DisplayName "WoL spinac - ping" -Direction Inbound -Protocol ICMPv4
     -IcmpType 8 -RemoteAddress LocalSubnet -Action Allow -Profile Any (jen pokud podobné pravidlo ještě není).
   Na konci znovu vypiš stav, ať je vidět, že se to změnilo.

5) BIOS. Zjisti výrobce a model základní desky (Get-CimInstance Win32_BaseBoard) a podle něj mi jednoduše
   popiš, jak se dostat do BIOSu a kde zapnout probouzení přes síť a vypnout ErP / Deep Sleep
   (vycházej z tabulky v README.md, kroku 4a). Řekni mi, ať si tyhle pokyny vyfotím na mobil,
   protože při restartu do BIOSu tě neuvidím. Počkej, až se vrátím a napíšu "hotovo".

6) Firmware. Ověř, že je ESP32 připojené (najdi COM port, např. `pio device list`). Pokud žádný port
   není, poraď mi s ovladačem CP210x nebo CH340 a s datovým kabelem.
   Spusť `pio run` a pak `pio run -t upload`. Kdyby nahrávání viselo na "Connecting..." nebo skončilo chybou
   "Wrong boot mode detected", řekni mi, ať podržím tlačítko BOOT, a nahrávání spusť znovu (u některých desek je to potřeba pokaždé). Kdyby build hlásil chybějící Python modul (např. intelhex), doinstaluj ho do Pythonu PlatformIO
   (%USERPROFILE%\.platformio\penv\Scripts\python.exe -m pip install <modul>).
   Potom spusť sériový monitor s časovým limitem (např. přes Python a pyserial z PlatformIO, 115200 baudů,
   cca 60 s, ať se nezasekneš na interaktivním `pio device monitor`) a vysvětli mi, co ESP32 vypisuje.
   Při prvním spuštění vytvoří Wi-Fi "WoL-Spinac" (heslo wolspinac, pokud jsem v secrets.h nezměnil
   PORTAL_PASSWORD): proveď mě připojením z mobilu, výběrem domácí 2,4GHz Wi-Fi a uložením.
   Pak znovu zkontroluj výstup: musí tam být "Připojeno k ..." a "=== Připraveno, čekám na příkazy ===".
   Zeptej se mě, jestli mi bot v Telegramu napsal "WoL spínač online" (s tlačítky pod zprávou) a jestli na
   tlačítko "📊 Stav" odpovídá "PC je zapnuté". Pokud ne, pomoz mi to vyřešit podle sériového výstupu.
   Pokud chci uspávání/vypínání: spusť jako správce `powershell -ExecutionPolicy Bypass -File pc-helper\install.ps1`
   (heslo si přečte ze secrets.h) a pak se mě zeptej, jestli "📊 Stav" ukazuje "Pomocník na PC: běží".
   Vysvětli mi, že příští firmware jde nahrát přes Wi-Fi: `pio run -e ota -t upload` (README → OTA).

7) Tailscale. Zjisti, jestli je nainstalovaný (`tailscale version`, případně
   "C:\Program Files\Tailscale\tailscale.exe"). Pokud ne, nabídni instalaci přes winget (Tailscale.Tailscale).
   Pak mě proveď přihlášením, zapnutím "Run unattended" (ikona u hodin → Settings/Preferences) a vypnutím key expiry
   v admin konzoli (https://login.tailscale.com/admin/machines → ⋯ u PC → Disable key expiry).
   Pomocí `tailscale status` / `tailscale ip -4` zjisti Tailscale IP (100.x.x.x) a jméno PC a ukaž mi je.
   Připomeň mi, ať si Tailscale nainstaluju i na telefon/notebook a přihlásím se stejným účtem.

8) Apollo. Zjisti, jestli je nainstalované (služba "Apollo Service" / složka v Program Files). Pokud ne, pošli
   mě na https://github.com/ClassicOldSong/Apollo/releases a vysvětli instalaci. Pak mě proveď:
   otevření https://localhost:47990, vytvoření účtu, instalace Moonlight nebo Artemis na telefon, ruční přidání
   PC přes Tailscale IP nebo MagicDNS jméno a spárování PINem (záložka PIN v Apollu).
   Zmiň, že Apollo umí streamovat i přihlašovací obrazovku Windows.

9) Finální test. Proveď mě testem: na telefonu vypnout Wi-Fi a jet přes mobilní data, PC vypnout,
   poslat /wake, počkat na "PC naběhlo", připojit se přes Moonlight/Artemis. Pokud je nastavený pomocník,
   může PC místo ručního vypnutí vypnout tlačítkem "⏻ Vypnout" v Telegramu. Protože PC bude vypnuté,
   řekni mi předem všechno, co mám udělat, a že se pak spolu znovu spojíme po zapnutí.
   Pokud něco nefunguje, použij sekci "Řešení problémů" v README.md.

Na úplném konci mi shrň, co je nastavené, kde co najdu, a připomeň, že secrets.h nesmím nikomu posílat.
Začni krokem 1.
```
