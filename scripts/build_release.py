"""Sestaví univerzální firmware pro webový nahrávač (docs/).

Spuštění z kořene projektu:  python scripts/build_release.py
Výstup: docs/firmware/wol-spinac.bin (celý obraz flash od adresy 0x0) a docs/manifest.json.
"""
import json
import re
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
pio = Path.home() / ".platformio" / "penv" / "Scripts" / "pio.exe"
pio_cmd = [str(pio)] if pio.exists() else ["pio"]

subprocess.run(pio_cmd + ["run", "-e", "release"], cwd=root, check=True)

build = root / ".pio" / "build" / "release"
boot_app0 = next((Path.home() / ".platformio" / "packages").glob("framework-arduinoespressif32*/tools/partitions/boot_app0.bin"))
out = root / "docs" / "firmware" / "wol-spinac.bin"
out.parent.mkdir(parents=True, exist_ok=True)

python = Path.home() / ".platformio" / "penv" / "Scripts" / "python.exe"
esptool = next((Path.home() / ".platformio" / "packages").glob("tool-esptoolpy*/esptool.py"))
subprocess.run([
    str(python) if python.exists() else sys.executable, str(esptool),
    "--chip", "esp32", "merge_bin", "-o", str(out),
    "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB",
    "0x1000", str(build / "bootloader.bin"),
    "0x8000", str(build / "partitions.bin"),
    "0xe000", str(boot_app0),
    "0x10000", str(build / "firmware.bin"),
], check=True)

version = re.search(r'FW_VERSION = "([^"]+)"', (root / "src" / "main.cpp").read_text(encoding="utf-8")).group(1)
manifest = {
    "name": "WoL spínač",
    "version": version,
    "new_install_prompt_erase": True,
    "builds": [{"chipFamily": "ESP32", "parts": [{"path": "firmware/wol-spinac.bin", "offset": 0}]}],
}
(root / "docs" / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(f"Hotovo: {out} ({out.stat().st_size} B), verze {version}")
