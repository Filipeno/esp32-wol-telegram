#pragma once

// =====================================================================
//  ŠABLONA – zkopíruj tento soubor jako "secrets.h" (do stejné složky)
//  a vyplň vlastní údaje. Soubor secrets.h se NIKDY nenahrává na GitHub
//  (je v .gitignore), protože obsahuje token bota.
// =====================================================================

// Token Telegram bota od @BotFather (vypadá jako 123456789:AAH...)
#define BOT_TOKEN "123456789:ZDE_VLOZ_TOKEN_OD_BOTFATHER"

// Chat ID uživatelů, kteří smějí bota ovládat (zjistíš přes @userinfobot).
// Více lidí odděl čárkou, např. { "111111111", "222222222" }
static const char *const ALLOWED_CHAT_IDS[] = { "111111111" };

// MAC adresa síťové karty PC (ipconfig /all -> "Fyzická adresa").
// Pomlčky i dvojtečky fungují: "AA-BB-CC-DD-EE-FF" nebo "AA:BB:CC:DD:EE:FF"
#define PC_MAC "AA:BB:CC:DD:EE:FF"

// IP adresa PC v domácí síti (ideálně rezervovaná v routeru přes DHCP).
#define PC_IP "192.168.1.50"

// Heslo k Wi-Fi "WoL-Spinac", kterou ESP32 vytvoří pro nastavení domácí Wi-Fi
// (captive portal). Musí mít aspoň 8 znaků.
#define PORTAL_PASSWORD "wolspinac"
