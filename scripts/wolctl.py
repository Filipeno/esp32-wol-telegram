"""Jednoduchá konzole pro WoL spínač: změna nastavení přes USB, bez nastavovací Wi-Fi.

Spuštění:   python scripts/wolctl.py            (port se najde sám)
            python scripts/wolctl.py COM11      (nebo zadej port)
Potřebuje:  pip install pyserial

Spínač musí být připojený USB kabelem k tomuto PC a mít firmware 1.4.0 nebo novější.
Nic se neukládá, dokud v nabídce nezvolíš „Uložit“ – pak se spínač restartuje.
"""
import getpass
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("Chybí modul pyserial. Nainstaluj ho příkazem:  pip install pyserial")

# Položky nastavení: klíč ve firmwaru, popis, jde o tajnou hodnotu?
SETTINGS = [
    ("token", "Token bota z @BotFather", True),
    ("chats", "Chat ID (víc lidí odděl čárkou)", False),
    ("mac", "MAC adresa PC", False),
    ("ip", "IP adresa PC (prázdné = najde se podle MAC)", False),
    ("hsec", "Heslo pomocníka na PC (uspání/vypnutí/aktualizace)", True),
    ("ota", "Heslo pro nahrávání firmwaru přes Wi-Fi", True),
]
# USB převodníky na běžných deskách ESP32 (CP210x, CH340, FTDI).
USB_IDS = {(0x10C4, 0xEA60), (0x1A86, 0x7523), (0x1A86, 0x55D4), (0x0403, 0x6001)}


def find_port():
    ports = [p.device for p in list_ports.comports() if (p.vid, p.pid) in USB_IDS]
    return ports[0] if len(ports) == 1 else (None if not ports else choose_port(ports))


def choose_port(ports):
    print("Našlo se víc portů:")
    for i, name in enumerate(ports, 1):
        print(f"  {i}) {name}")
    pick = input("Který? ").strip()
    return ports[int(pick) - 1] if pick.isdigit() and 1 <= int(pick) <= len(ports) else None


def open_port(name):
    # DTR/RTS musí být vypnuté dřív, než se port otevře, jinak se ESP32 při otevření restartuje.
    conn = serial.Serial()
    conn.port = name
    conn.baudrate = 115200
    conn.timeout = 0.3
    conn.dtr = False
    conn.rts = False
    conn.open()
    return conn


def send(conn, line, until=None, timeout=3.0):
    """Pošle příkaz a vrátí odpovědi (řádky začínající @). Čeká na @ok/@err, nebo na řádek `until`."""
    conn.reset_input_buffer()
    conn.write((line + "\n").encode("utf-8"))
    replies = []
    end = time.time() + timeout
    while time.time() < end:
        raw = conn.readline().decode("utf-8", "replace").strip()
        if not raw.startswith("@"):
            continue
        replies.append(raw)
        if raw.startswith(("@ok", "@err")) or raw == until:
            break
    return replies


def check_firmware(conn):
    for _ in range(3):
        for reply in send(conn, "@ping", timeout=1.5):
            if reply.startswith("@pong"):
                return reply.split(" ", 1)[1]
    return None


def show(conn):
    lines = send(conn, "@get", until="@end")
    print()
    for line in lines:
        if line.startswith("@cfg "):
            key, value = line[5:].split(" ", 1)
            label = next((d for k, d, _ in SETTINGS if k == key), key)
            print(f"  {label}: {value}")
        elif line.startswith("@info "):
            key, value = line[6:].split(" ", 1)
            print(f"  [{key}] {value}")
    print()


def change(conn):
    print()
    for i, (_, desc, _) in enumerate(SETTINGS, 1):
        print(f"  {i}) {desc}")
    pick = input("Kterou položku změnit (Enter = zpět)? ").strip()
    if not pick.isdigit() or not 1 <= int(pick) <= len(SETTINGS):
        return
    key, desc, secret = SETTINGS[int(pick) - 1]
    prompt = f"Nová hodnota pro „{desc}“: "
    value = getpass.getpass(prompt) if secret else input(prompt)
    reply = send(conn, f"@set {key} {value}")
    print("  Připraveno (zatím neuloženo)." if "@ok" in reply else f"  Chyba: {' '.join(reply) or 'spínač neodpověděl'}")


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else find_port()
    if not name:
        sys.exit("Nenašel jsem ESP32. Připoj ho USB kabelem (datovým) nebo zadej port: python wolctl.py COM11")
    try:
        conn = open_port(name)
    except serial.SerialException as err:
        sys.exit(f"Port {name} nejde otevřít ({err}). Je zavřený sériový monitor a nahrávání?")
    version = check_firmware(conn)
    if not version:
        sys.exit(f"Spínač na {name} neodpovídá. Má firmware 1.4.0 nebo novější? Zkus ho restartovat (tlačítko EN).")
    print(f"Připojeno k WoL spínači (firmware {version}) na {name}.")

    while True:
        print("Co chceš udělat?")
        print("  1) Ukázat nastavení")
        print("  2) Změnit položku")
        print("  3) Uložit změny a restartovat spínač")
        print("  4) Zahodit neuložené změny")
        print("  5) Otevřít nastavovací Wi-Fi (z mobilu, včetně změny domácí Wi-Fi)")
        print("  6) Smazat uloženou Wi-Fi a restartovat do nastavení")
        print("  7) Restartovat spínač")
        print("  0) Konec")
        choice = input("> ").strip()
        if choice == "1":
            show(conn)
        elif choice == "2":
            change(conn)
        elif choice == "3":
            print(" ".join(send(conn, "@save", timeout=5)) or "Bez odpovědi.")
        elif choice == "4":
            print(" ".join(send(conn, "@discard")))
        elif choice == "5":
            send(conn, "@portal")
            print("Připoj se z mobilu k Wi-Fi „WoL-Spinac“ (heslo wolspinac). Otevře se stránka s nastavením.")
        elif choice == "6":
            if input("Opravdu smazat uloženou Wi-Fi? (ano/ne) ").strip().lower() == "ano":
                print(" ".join(send(conn, "@wifireset")))
        elif choice == "7":
            print(" ".join(send(conn, "@reboot")))
        elif choice == "0":
            break
    conn.close()


if __name__ == "__main__":
    main()
