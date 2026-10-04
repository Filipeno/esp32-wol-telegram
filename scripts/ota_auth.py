# Předá heslo pro nahrávání přes Wi-Fi (OTA) z include/secrets.h do PlatformIO,
# aby nemuselo být zapsané ještě jednou v platformio.ini (ten jde na GitHub).
import os
import re

Import("env")  # noqa: F821 – dodává PlatformIO

secrets_path = os.path.join(env.subst("$PROJECT_INCLUDE_DIR"), "secrets.h")  # noqa: F821
password = ""
if os.path.isfile(secrets_path):
    with open(secrets_path, encoding="utf-8") as f:
        match = re.search(r'^\s*#define\s+OTA_PASSWORD\s+"([^"]*)"', f.read(), re.MULTILINE)
    if match:
        password = match.group(1)

if password:
    env.Append(UPLOAD_FLAGS=["--auth=" + password])  # noqa: F821
else:
    print("*** OTA_PASSWORD v include/secrets.h je prázdné – nahrávání přes Wi-Fi nepůjde. ***")
