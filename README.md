# 🖥️ WoL spínač přes Telegram (ESP32)

Malá destička ESP32 položená u routeru, která na tvůj pokyn z Telegramu **zapne počítač na dálku**.
Pak se k PC připojíš přes **Moonlight / Artemis** (streamování přes Apollo) a hraješ nebo pracuješ, jako bys u něj seděl.

- Funguje odkudkoli – z mobilních dat, ze školy, od kamaráda.
- **Nemusíš nic nastavovat na routeru** (žádné otevírání portů). ESP32 se samo ptá Telegramu, jestli nemá něco udělat.
- Bot poslouchá jen tebe (a lidi, které povolíš). Ostatním vůbec neodpovídá.

### Co bot umí

| Příkaz | Co udělá |
|---|---|
| `/wake` | Probudí PC a do 90 sekund ti napíše, jestli naběhlo. |
| `/status` | Řekne, jestli je PC zapnuté, jak dlouho spínač běží a jak silnou má Wi-Fi. |
| `/help` | Seznam příkazů. |

Po každém zapnutí (třeba po výpadku proudu) ti bot napíše **„✅ WoL spínač online“**.
Příkazy poslané v době, kdy byl spínač vypnutý, se zahodí – PC se tedy po výpadku proudu samo nezapne.

---

> ### ⚡ Rychlá cesta: nech se provést Claude Code
> Máš na PC [Claude Code](https://claude.com/claude-code)? Otevři soubor **[CLAUDE_SETUP.md](CLAUDE_SETUP.md)**,
> zkopíruj z něj prompt do Claude Code (spuštěného ve složce tohoto projektu) a on tě celým nastavením provede
> krok po kroku – sám zjistí MAC a IP adresu, nastaví Windows a nahraje firmware.
> Kroky, které musíš udělat sám (BIOS, router, přihlášení), ti vysvětlí.
>
> Pokud Claude Code nemáš, pokračuj podle návodu níže.

---

## Přehled kroků

1. [Co koupit a kam ESP32 dát](#krok-1--co-koupit-a-kam-esp32-dát)
2. [Založení Telegram bota](#krok-2--založení-telegram-bota)
3. [MAC a IP adresa PC + rezervace v routeru](#krok-3--mac-a-ip-adresa-pc--rezervace-v-routeru)
4. [Zapnutí Wake-on-LAN v BIOSu a ve Windows](#krok-4--zapnutí-wake-on-lan-v-biosu-a-ve-windows)
5. [Nahrání firmwaru do ESP32 a připojení k Wi-Fi](#krok-5--nahrání-firmwaru-do-esp32-a-připojení-k-wi-fi)
6. [Tailscale (bezpečné připojení zvenku)](#krok-6--tailscale-bezpečné-připojení-zvenku)
7. [Apollo + Moonlight/Artemis (streamování)](#krok-7--apollo--moonlightartemis-streamování)
8. [Finální test](#krok-8--finální-test)
9. [Řešení problémů](#krok-9--řešení-problémů)

Počítej zhruba s hodinou času.

---

## Krok 1 – Co koupit a kam ESP32 dát

**Nákupní seznam** (dohromady cca 200–300 Kč):

1. **ESP32 DevKit** s čipem **ESP32-WROOM-32** (často prodávané jako „ESP32 DevKit V1“ nebo „ESP-32S NodeMCU“). Hledej na Laskarduino, GME, Botland, AliExpress…
2. **USB kabel, který přenáší data** – ne jen nabíjecí! Konektor podle desky: většinou **micro-USB**, u novějších **USB-C**.
   Tip: kabel od starého telefonu, přes který šlo kopírovat fotky do PC, je skoro jistě datový.
3. **USB nabíječka** do zásuvky (stačí jakákoli stará na telefon, 5 V / 1 A).

**Kam ESP32 umístit:**

- Do stejné domácí sítě jako PC, ideálně **kousek od routeru** (dobrý Wi-Fi signál).
- Do zásuvky, kterou nikdo nevypíná (ne do prodlužky s vypínačem u PC!).
- ESP32 umí jen **2,4 GHz Wi-Fi** (5 GHz nevidí). Většina routerů vysílá obě, takže to obvykle nevadí.
- **PC musí být připojené kabelem** (ethernet) – přes Wi-Fi se PC probudit nedá.

✅ **Jak poznám, že je hotovo:** Máš na stole ESP32, datový kabel a nabíječku. PC je k routeru připojené kabelem.

---

## Krok 2 – Založení Telegram bota

1. V Telegramu vyhledej **@BotFather** (s modrou fajfkou) a napiš mu `/newbot`.
2. Zadej jméno bota (cokoli, např. `Můj PC spínač`).
3. Zadej uživatelské jméno bota – musí končit na `bot`, např. `pepa_pc_spinac_bot`.
4. BotFather ti pošle **token** – dlouhý text jako `123456789:AAH4k...`. **Ulož si ho a nikomu ho neukazuj** – kdo má token, ovládá bota.
5. Teď vyhledej **@userinfobot** a napiš mu cokoli (např. `/start`). Odpoví ti tvým **Id** – číslo jako `987654321`. To je tvoje **chat ID**.
6. Najdi svého nového bota (podle uživatelského jména z bodu 3) a klikni na **Start**. (Bot ti zatím neodpoví – ožije až v kroku 5.)

Pokud má bota ovládat víc lidí, každý si zjistí své chat ID přes @userinfobot a klikne u bota na Start.

✅ **Jak poznám, že je hotovo:** Máš poznamenaný **token** a své **chat ID** (jen čísla).

---

## Krok 3 – MAC a IP adresa PC + rezervace v routeru

### 3a) Zjištění MAC a IP

1. Na PC stiskni **Win + R**, napiš `cmd` a dej Enter.
2. Napiš `ipconfig /all` a dej Enter.
3. Najdi blok **Ethernet adapter Ethernet** (adaptér pro kabel – **ne** Wi-Fi, **ne** Tailscale, **ne** „vEthernet“).
4. Opiš si:
   - **Fyzická adresa** (Physical Address) – např. `12-34-56-78-9A-BC` → to je **MAC**.
   - **Adresa IPv4** (IPv4 Address) – např. `192.168.1.50` → to je **IP**.

### 3b) Rezervace IP v routeru (aby se IP neměnila)

Router přiděluje IP adresy automaticky a po čase by PC mohl dostat jinou. Spínač by pak pingal špatnou adresu.

1. Otevři v prohlížeči administraci routeru – obvykle `http://192.168.1.1` nebo `http://192.168.0.1` (adresa bývá na štítku routeru, heslo taky).
2. Najdi sekci **DHCP** → **Rezervace adres** / **Static lease** / **Address Reservation** / **Pevná IP**.
3. Přidej rezervaci: MAC adresa PC + IP adresa PC (tu, kterou má teď).
4. Ulož.

✅ **Jak poznám, že je hotovo:** Máš poznamenanou MAC i IP a v routeru vidíš rezervaci pro PC. Po restartu PC ukazuje `ipconfig` stejnou IP.

---

## Krok 4 – Zapnutí Wake-on-LAN v BIOSu a ve Windows

Wake-on-LAN (WoL) = síťová karta PC i ve vypnutém stavu poslouchá a na speciální „magic packet“ počítač zapne.

### 4a) BIOS / UEFI

1. Restartuj PC a hned opakovaně mačkej **Delete** (někdy **F2**), dokud se neobjeví BIOS.
2. Přepni se do pokročilého režimu (obvykle klávesa **F7**).
3. Zapni volbu pro probouzení přes síť a **vypni ErP** (úsporný režim, který síťovku ve vypnutém PC úplně odpojí):

| Výrobce desky | Kde to najdeš |
|---|---|
| **ASUS** | Advanced → APM Configuration → **Power On By PCI-E** = Enabled, **ErP Ready** = Disabled |
| **MSI** | Settings → Advanced → Wake Up Event Setup → **Resume By PCI-E Device** = Enabled; Settings → Advanced → Power Management Setup → **ErP Ready** = Disabled |
| **Gigabyte** | Settings → Platform Power → **Wake on LAN** = Enabled, **ErP** = Disabled |
| **ASRock** | Advanced → ACPI Configuration → **PCIE Devices Power On** = Enabled; **Deep Sleep** = Disabled |
| Jiné / notebook | Hledej slova *Wake on LAN*, *Power On by PCI-E*, *Resume by LAN*, *PME*; vypni *ErP* / *Deep Sleep* / *EuP* |

4. Ulož a odejdi (**F10** → Yes).

### 4b) Windows – síťová karta

1. Klikni pravým na Start → **Správce zařízení**.
2. Rozbal **Síťové adaptéry** a dvakrát klikni na kabelovou síťovku (Realtek / Intel … *Ethernet* / *GbE*, ne Wi-Fi).
3. Záložka **Upřesnit** – nastav (pokud tam jsou):
   - **Wake on Magic Packet** = Povoleno / Enabled
   - **Shutdown Wake-On-Lan** (u Realteku) = Povoleno
   - **Energy Efficient Ethernet** / **Green Ethernet** = Zakázáno (někdy brání probuzení)
4. Záložka **Řízení spotřeby** – zaškrtni:
   - ☑ **Povolit zařízení probudit počítač**
   - ☑ **Probudit počítač pouze pomocí paketu Magic Packet** (aby PC nebudil každý šum v síti)
5. Dej **OK**.

### 4c) Windows – vypnout Rychlé spuštění

Rychlé spuštění dělá z vypnutí napůl hibernaci a WoL pak často nefunguje.

1. Win + R → napiš `control` → Enter.
2. **Možnosti napájení** → vlevo **Nastavení tlačítek napájení**.
3. Klikni na **Změnit nastavení, které nyní nejsou k dispozici**.
4. Odškrtni **Zapnout rychlé spuštění (doporučeno)** → **Uložit změny**.

### 4d) Windows – povolit ping z domácí sítě

Spínač zjišťuje, jestli PC běží, pomocí pingu. Windows ho ale často blokuje. Otevři **PowerShell jako správce** (pravým na Start → *Terminál (správce)*) a vlož:

```powershell
New-NetFirewallRule -DisplayName "WoL spinac - ping" -Direction Inbound -Protocol ICMPv4 -IcmpType 8 -RemoteAddress LocalSubnet -Action Allow -Profile Any
```

(Povolí ping jen ze tvé domácí sítě, ne z internetu.)

✅ **Jak poznám, že je hotovo:** Když PC vypneš (normálně přes Start → Vypnout), **světýlka u síťového konektoru vzadu na PC dál svítí/blikají**. Z jiného zařízení v síti (nebo `ping 192.168.1.50` z jiného PC) PC na ping odpovídá, když je zapnuté.

---

## Krok 5 – Nahrání firmwaru do ESP32 a připojení k Wi-Fi

### 5a) Nainstaluj nástroje

1. Nainstaluj **[Visual Studio Code](https://code.visualstudio.com/)**.
2. Ve VS Code klikni vlevo na ikonu kostiček (**Extensions**), vyhledej **PlatformIO IDE** a dej **Install**. Instalace chvíli trvá, pak VS Code restartuj.
3. Stáhni tento projekt: na GitHubu zelené tlačítko **Code → Download ZIP** a rozbal ho (nebo `git clone`).
4. Ve VS Code: **File → Open Folder…** a vyber složku projektu (tu, kde je `platformio.ini`).

### 5b) Vyplň své údaje

1. Ve složce `include` zkopíruj soubor `secrets.example.h` a kopii přejmenuj na **`secrets.h`**.
2. Otevři `secrets.h` a vyplň:
   - `BOT_TOKEN` – token z kroku 2,
   - `ALLOWED_CHAT_IDS` – tvoje chat ID z kroku 2 (v uvozovkách),
   - `PC_MAC` – MAC z kroku 3 (pomlčky klidně nech),
   - `PC_IP` – IP z kroku 3.
3. Ulož (Ctrl + S).

### 5c) Nahraj firmware

1. Připoj ESP32 kabelem k PC.
   - Pokud ho Windows nepozná (ve Správci zařízení není nic pod *Porty (COM a LPT)*), doinstaluj ovladač **CP210x** ([Silicon Labs](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)) nebo **CH340** – podle čipu napsaného u USB konektoru na desce.
2. Ve VS Code dole na modré liště klikni na **šipku →** (*PlatformIO: Upload*). Poprvé se stahuje spousta věcí, chvíli to trvá.
   - Když se to zasekne na `Connecting.....`, podrž na desce tlačítko **BOOT**, dokud nahrávání nezačne.
3. Až uvidíš `SUCCESS`, klikni dole na ikonu **zástrčky** (*Serial Monitor*).

### 5d) Připojení ESP32 k Wi-Fi (z mobilu)

1. ESP32 při prvním spuštění nezná tvou Wi-Fi, a tak vytvoří vlastní Wi-Fi síť **`WoL-Spinac`**. Modrá LED pomalu bliká.
2. Na mobilu se připoj k Wi-Fi **WoL-Spinac**, heslo **`wolspinac`**.
3. Měla by se sama otevřít stránka (pokud ne, otevři v prohlížeči `http://192.168.4.1`).
4. Klikni **Configure WiFi**, vyber svou domácí Wi-Fi (2,4 GHz), zadej heslo a **Save**.
5. ESP32 se připojí a LED zhasne. Mobil se vrátí na domácí Wi-Fi.

> Nastavovací Wi-Fi je otevřená 3 minuty, pak se ESP32 restartuje a zkusí to znovu.
> **Změna Wi-Fi později** (nový router, jiné heslo): odpoj a znovu zapoj napájení ESP32 a během prvních 3 sekund (LED rychle bliká) zmáčkni tlačítko **BOOT**. Uložená Wi-Fi se smaže a znovu se objeví síť WoL-Spinac.

Teď ESP32 odpoj od PC a zapoj ho do nabíječky na místě z kroku 1.

✅ **Jak poznám, že je hotovo:** Telegram bot ti napíše **„✅ WoL spínač online“**. Na `/status` odpoví „🟢 PC je zapnuté“.
(V sériovém monitoru je vidět `=== Připraveno, čekám na příkazy ===`.)

---

## Krok 6 – Tailscale (bezpečné připojení zvenku)

Tailscale vytvoří soukromou síť mezi tvým PC a telefonem/notebookem – kdekoli na světě, bez otevírání portů.

### Na PC

1. Stáhni a nainstaluj **[Tailscale pro Windows](https://tailscale.com/download/windows)**.
2. Přihlas se (Google / Microsoft / GitHub účet).
3. Klikni na ikonu Tailscale vedle hodin (možná schovanou pod šipkou ^) → **Preferences / Settings** → zaškrtni **Run unattended**.
   Díky tomu Tailscale běží, i když se do Windows nikdo nepřihlásí (PC po probuzení stojí na přihlašovací obrazovce).
4. Otevři **[admin konzoli Tailscale](https://login.tailscale.com/admin/machines)**, u svého PC klikni na **⋯** → **Disable key expiry**.
   Jinak by se PC za pár měsíců z Tailscale odhlásilo a zvenku by nebylo vidět.
5. V admin konzoli si poznamenej **IP adresu PC** – začíná na **100.** (např. `100.101.102.103`) – a jeho jméno (např. `herni-pc`).

### Na telefonu / notebooku, ze kterého se budeš připojovat

1. Nainstaluj Tailscale (Google Play / App Store / tailscale.com) a přihlas se **stejným účtem**.
2. Zapni ho.

✅ **Jak poznám, že je hotovo:** V aplikaci Tailscale na telefonu vidíš své PC jako **zelené / Connected**. Po restartu PC (bez přihlášení do Windows) je PC v Tailscale pořád online.

---

## Krok 7 – Apollo + Moonlight/Artemis (streamování)

**Apollo** běží na PC a streamuje obraz. **Moonlight** (nebo **Artemis** – vylepšený Moonlight pro Android) je aplikace, ve které obraz sleduješ a ovládáš PC.

### Na PC – Apollo

1. Stáhni nejnovější instalačku z **[GitHubu Apolla](https://github.com/ClassicOldSong/Apollo/releases)** (soubor `.exe`) a nainstaluj.
2. Otevři v prohlížeči `https://localhost:47990` (varování o certifikátu potvrď – *Pokračovat*).
3. Vytvoř si uživatelské jméno a heslo do Apolla.

Apollo umí streamovat i **přihlašovací obrazovku Windows**, takže se po probuzení přihlásíš rovnou přes Moonlight.

### Na telefonu / notebooku – Moonlight nebo Artemis

1. Nainstaluj **Moonlight** (všechny platformy, moonlight-stream.org) nebo **Artemis** (Android, [GitHub](https://github.com/ClassicOldSong/moonlight-android/releases)).
2. Zapni Tailscale.
3. V aplikaci klikni na **+** (Přidat PC ručně) a zadej **Tailscale IP** PC (`100.x.x.x`) nebo jeho jméno z Tailscale (MagicDNS, např. `herni-pc`).
4. Aplikace ukáže **čtyřmístný PIN**. Na PC v Apollu (`https://localhost:47990`) otevři záložku **PIN**, zadej ho a potvrď.
5. Klikni na PC v aplikaci → **Desktop** a mělo by se ukázat plocha PC.

✅ **Jak poznám, že je hotovo:** Na telefonu vidíš plochu PC a můžeš ho ovládat.

---

## Krok 8 – Finální test

1. Na telefonu **vypni Wi-Fi** a jeď přes **mobilní data** (tak to bude fungovat i venku).
2. PC normálně **vypni** (Start → Vypnout). Počkej, až úplně zhasne.
3. Pošli botovi **`/wake`**.
4. Bot napíše „📨 Magic packet odeslán…“ a do 90 s **„✅ PC naběhlo“**.
5. Otevři Moonlight/Artemis (Tailscale zapnutý) a připoj se k PC. Uvidíš přihlašovací obrazovku – přihlas se.

✅ **Jak poznám, že je hotovo:** Z mobilních dat jsi zapnul PC a vidíš ho v Moonlightu. Hotovo! 🎉

---

## Krok 9 – Řešení problémů

### 🔌 PC se neprobouzí

- Svítí po vypnutí PC světýlka u síťového konektoru? Pokud **ne**, je v BIOSu zapnuté **ErP / Deep Sleep** nebo vypnuté WoL – vrať se ke kroku 4a.
- Je vypnuté **Rychlé spuštění** (krok 4c)? Po Windows Update se občas zapne zpátky.
- Zkontroluj **MAC adresu** v `secrets.h` – musí být od **kabelové** síťovky, ne od Wi-Fi.
- Je PC opravdu na **kabelu** a ESP32 ve **stejné síti** (ne v síti pro hosty)?
- Po úplném odpojení PC ze zásuvky (nebo výpadku proudu) některé desky WoL neumí, dokud PC jednou nezapneš tlačítkem a zase nevypneš. Na to bohužel spínač nic nezmůže.
- Ovladač síťovky: stáhni nejnovější ze stránek výrobce desky (Realtek/Intel), ten od Windows někdy WoL ve vypnutém stavu nepodporuje.

### 🤖 Bot neodpovídá

- Svítí/bliká LED na ESP32? **Pomalé blikání** = nemůže se připojit k Wi-Fi (je router zapnutý? změnilo se heslo? → viz „Změna Wi-Fi“ v kroku 5d).
- Napsal bot po zapnutí „WoL spínač online“? Když ne, zkontroluj **token** v `secrets.h` a nahraj firmware znovu.
- Píšeš ze správného účtu? Bot odpovídá jen chat ID ze `secrets.h`. Ostatní ignoruje (v sériovém monitoru uvidíš `Zpráva od nepovoleného chatu …` i s ID, které tam stačí doplnit).
- Odpoj ESP32 z napájení a zase zapoj – za ~30 s by měl přijít „WoL spínač online“.

### 🟡 Bot říká „PC neodpovídá“, ale PC běží

- Windows blokuje ping – udělej krok 4d.
- PC má jinou IP, než je v `secrets.h` – zkontroluj `ipconfig` a rezervaci v routeru (krok 3b).

### 📺 Moonlight PC nevidí

- Je na telefonu **zapnutý Tailscale**? A je PC v Tailscale online (zelené)?
- Přidáváš PC přes **100.x.x.x** adresu (ne 192.168…)? Doma 192.168 funguje, venku ne.
- Běží Apollo? Na PC otevři `https://localhost:47990`. Případně ve **Službách** Windows zkontroluj, že služba **Apollo Service** běží a je nastavená na *Automaticky*.
- Brána firewall: při instalaci Apolla musí být povolené jeho porty – nejjednodušší je Apollo přeinstalovat a potvrdit dotaz na firewall.

### 🔒 Tailscale je offline na přihlašovací obrazovce

- Zkontroluj, že je zaškrtnuté **Run unattended** (krok 6 – bod 3). Bez toho Tailscale startuje až po přihlášení do Windows.
- V admin konzoli zkontroluj, že u PC nesvítí **Expired** – pak se přihlas znovu a zapni **Disable key expiry**.
- Ve **Službách** Windows musí služba **Tailscale** běžet a mít spuštění *Automaticky*.

---

## Pro zvídavé

- Kód je v [`src/main.cpp`](src/main.cpp), komentovaný česky. Verze knihoven jsou zafixované v [`platformio.ini`](platformio.ini).
- Spojení s Telegramem je šifrované a ESP32 ověřuje certifikát Telegramu (Go Daddy Root G2, platný do 2037).
- Odolnost: ESP32 se při výpadku Wi-Fi samo znovu připojuje, po 10 minutách bez Wi-Fi se restartuje; po 30 minutách bez spojení s Telegramem taky. Hardwarový watchdog restartuje ESP32, kdyby se program zasekl.
- Licence: [MIT](LICENSE).
