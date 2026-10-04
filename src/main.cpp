/*
 * Wake-on-LAN spínač ovládaný přes Telegram
 * ==========================================
 * Deska: ESP32 DevKit (ESP32-WROOM-32), framework Arduino.
 *
 * Jak to funguje:
 *  - ESP32 se samo pravidelně ptá Telegramu na nové zprávy (tzv. polling).
 *    Proto NENÍ potřeba otevírat žádné porty na routeru – veškerá komunikace
 *    jde z domácí sítě ven, stejně jako když si otevřeš web v prohlížeči.
 *  - Na příkaz /wake (nebo tlačítko) pošle do domácí sítě "magic packet",
 *    který probudí PC, a pak hlídá, jestli PC naběhlo.
 *  - Průběžně PC pinguje a dá vědět, když se zapne nebo vypne.
 *  - Uspat/vypnout PC umí přes malého pomocníka na PC (složka pc-helper).
 *  - Nový firmware jde nahrát přes Wi-Fi (OTA), bez kabelu.
 *
 * Údaje (token, chat ID, MAC a IP PC, hesla) jsou v include/secrets.h.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <WakeOnLan.h>
#include <ESP32Ping.h>
#include <Ticker.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <mbedtls/md.h>
#include <time.h>

#include "telegram_cert.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Chybí include/secrets.h – zkopíruj include/secrets.example.h jako include/secrets.h a vyplň ho."
#endif

// Starší secrets.h nemusí obsahovat novější položky, proto výchozí hodnoty.
#ifndef PORTAL_PASSWORD
#define PORTAL_PASSWORD "wolspinac"
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif
#ifndef PC_HELPER_SECRET
#define PC_HELPER_SECRET ""
#endif
#ifndef PC_HELPER_PORT
#define PC_HELPER_PORT 8766
#endif

static const char *FW_VERSION = "1.1.0";

// ---------------------------------------------------------------------
//  Nastavení (běžně není třeba měnit)
// ---------------------------------------------------------------------

// Zabudovaná modrá LED na ESP32 DevKitu je na GPIO2 (svítí při HIGH).
static const int LED_PIN = 2;
// Tlačítko BOOT na desce (GPIO0, při stisku je LOW).
static const int BOOT_BUTTON_PIN = 0;

// Název Wi-Fi sítě, kterou ESP32 vytvoří, když se nemůže připojit.
static const char *PORTAL_SSID = "WoL-Spinac";
// Jak dlouho je nastavovací Wi-Fi otevřená, než se ESP32 restartuje a zkusí to znovu.
static const uint32_t PORTAL_TIMEOUT_S = 180;
// Jak dlouho zkoušet uloženou Wi-Fi, než se otevře nastavovací Wi-Fi.
static const uint32_t WIFI_CONNECT_TIMEOUT_S = 30;

// Jméno ESP32 v síti – pro nahrávání přes Wi-Fi: wol-spinac.local
static const char *OTA_HOSTNAME = "wol-spinac";

// Jak často se ptát Telegramu na nové zprávy.
static const uint32_t BOT_POLL_INTERVAL_MS = 2000;

// Wake-on-LAN: 3 pakety s rozestupem 100 ms, UDP port 9.
static const uint8_t WOL_REPEAT = 3;
static const uint32_t WOL_REPEAT_DELAY_MS = 100;
static const uint16_t WOL_PORT = 9;

// Hlídání PC pingem: normálně každých 30 s; po /wake nebo vypnutí každé 3 s.
static const uint32_t PC_MONITOR_INTERVAL_MS = 30UL * 1000UL;
static const uint32_t PC_FAST_PING_INTERVAL_MS = 3000;
// PC bereme jako vypnuté až po 3 neúspěšných pingách za sebou (jeden ztracený
// ping při slabší Wi-Fi tak nespustí falešné "PC se vypnulo").
static const uint8_t PC_OFF_AFTER_FAILS = 3;
// Po /wake čekáme až 90 s, po uspání/vypnutí až 2 minuty.
static const uint32_t WAKE_WAIT_MS = 90UL * 1000UL;
static const uint32_t OFF_WAIT_MS = 120UL * 1000UL;

// Potvrzovací tlačítko u uspání/vypnutí platí 2 minuty.
static const uint32_t CONFIRM_VALID_MS = 120UL * 1000UL;

// Pomocník na PC: jak dlouho čekat na spojení a na odpověď.
static const uint32_t HELPER_CONNECT_TIMEOUT_MS = 2000;
static const uint32_t HELPER_RESPONSE_TIMEOUT_MS = 3000;

// Výpadek Wi-Fi: každých 15 s zkusit znovu připojit, po 10 minutách restart.
static const uint32_t WIFI_RETRY_INTERVAL_MS = 15UL * 1000UL;
static const uint32_t WIFI_MAX_OUTAGE_MS = 10UL * 60UL * 1000UL;

// Kontrola spojení s Telegramem: každých 10 minut, po 3 neúspěších za sebou restart.
static const uint32_t TELEGRAM_CHECK_INTERVAL_MS = 10UL * 60UL * 1000UL;
static const uint8_t TELEGRAM_MAX_FAILED_CHECKS = 3;

// Hardwarový watchdog: když se program na 60 s zasekne, ESP32 se samo restartuje.
static const uint32_t WATCHDOG_TIMEOUT_S = 60;

// ---------------------------------------------------------------------
//  Globální objekty a stav
// ---------------------------------------------------------------------

WiFiClientSecure secureClient;
UniversalTelegramBot bot(BOT_TOKEN, secureClient);
WiFiUDP udp;
WakeOnLan wol(udp);
Ticker ledBlinker;
Preferences prefs;
IPAddress pcIp;

static const size_t ALLOWED_CHAT_COUNT = sizeof(ALLOWED_CHAT_IDS) / sizeof(ALLOWED_CHAT_IDS[0]);

uint32_t lastBotPollMs = 0;
uint32_t lastTelegramCheckMs = 0;
uint8_t failedTelegramChecks = 0;
uint32_t wifiLostSinceMs = 0;   // 0 = Wi-Fi je v pořádku
uint32_t lastWifiRetryMs = 0;
time_t bootEpoch = 0;           // čas zapnutí ESP32 (unix čas), 0 = neznámý

// Oznámení o zapnutí/vypnutí PC (uloženo v paměti ESP32, přežije restart).
bool notifyEnabled = true;

// Co víme o PC.
enum PcState : uint8_t { PC_UNKNOWN, PC_ON, PC_OFF };
PcState pcState = PC_UNKNOWN;
uint8_t pcFailedPings = 0;
uint32_t lastPcPingMs = 0;

// Na co právě čekáme: až PC naběhne (po /wake), nebo až zhasne (po uspání/vypnutí).
enum PendingAction : uint8_t { PENDING_NONE, PENDING_WAKE, PENDING_OFF };
PendingAction pending = PENDING_NONE;
String pendingChatId;
uint32_t pendingStartMs = 0;
bool pendingIsSleep = false;    // u PENDING_OFF: uspání (true), nebo vypnutí (false)

// Otevřené potvrzení uspání/vypnutí (tlačítko "Ano").
String confirmAction;           // "sleep" / "shutdown" / "" = nic
uint32_t confirmNonce = 0;
uint32_t confirmStartMs = 0;

// ---------------------------------------------------------------------
//  LED
// ---------------------------------------------------------------------

void ledToggle() {
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));
}

// Začne blikat LED (např. během připojování k Wi-Fi).
void ledStartBlinking(uint32_t intervalMs) {
  ledBlinker.detach();
  ledBlinker.attach_ms(intervalMs, ledToggle);
}

// Přestane blikat a LED zhasne.
void ledStopBlinking() {
  ledBlinker.detach();
  digitalWrite(LED_PIN, LOW);
}

// Krátké bliknutí (při odeslání WoL).
void ledFlash(uint8_t times) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(60);
    digitalWrite(LED_PIN, LOW);
    delay(60);
  }
}

// ---------------------------------------------------------------------
//  Watchdog
// ---------------------------------------------------------------------

// Zapne hardwarový watchdog pro hlavní smyčku. Pokud se loop() na déle než
// WATCHDOG_TIMEOUT_S zasekne (a nezavolá watchdogFeed), ESP32 se restartuje.
void watchdogStart() {
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t config = {
      .timeout_ms = WATCHDOG_TIMEOUT_S * 1000,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  if (esp_task_wdt_reconfigure(&config) != ESP_OK) {
    esp_task_wdt_init(&config);
  }
#else
  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);  // hlídej aktuální úlohu (loop)
}

// "Nakrmí" watchdog = dá mu vědět, že program žije.
void watchdogFeed() {
  esp_task_wdt_reset();
}

// ---------------------------------------------------------------------
//  Pomocné funkce
// ---------------------------------------------------------------------

bool helperConfigured() {
  return strlen(PC_HELPER_SECRET) > 0;
}

bool otaConfigured() {
  return strlen(OTA_PASSWORD) > 0;
}

// Je chat ID na seznamu povolených?
bool isAllowedChat(const String &chatId) {
  for (size_t i = 0; i < ALLOWED_CHAT_COUNT; i++) {
    if (chatId == ALLOWED_CHAT_IDS[i]) return true;
  }
  return false;
}

// Uptime ESP32 jako čitelný text, např. "2 d 3 h 15 min".
// esp_timer počítá v mikrosekundách v 64 bitech, takže nepřeteče jako millis().
String formatUptime() {
  uint64_t totalMin = esp_timer_get_time() / 1000000ULL / 60ULL;
  uint32_t days = totalMin / (60 * 24);
  uint32_t hours = (totalMin / 60) % 24;
  uint32_t minutes = totalMin % 60;
  String s;
  if (days > 0) s += String(days) + " d ";
  if (days > 0 || hours > 0) s += String(hours) + " h ";
  s += String(minutes) + " min";
  return s;
}

// Síla Wi-Fi signálu jako text, např. "-58 dBm (dobrý)".
String formatWifiSignal() {
  int rssi = WiFi.RSSI();
  const char *quality;
  if (rssi >= -55) quality = "výborný";
  else if (rssi >= -67) quality = "dobrý";
  else if (rssi >= -75) quality = "slabší";
  else quality = "slabý";
  return String(rssi) + " dBm (" + quality + ")";
}

// Odpovídá PC na ping? (1 pokus, cca 1 s)
bool isPcOnline() {
  return Ping.ping(pcIp, 1);
}

// ---------------------------------------------------------------------
//  Telegram – zprávy a tlačítka
// ---------------------------------------------------------------------

// Hlavní nabídka tlačítek pod zprávou (JSON pro Telegram "inline keyboard").
String mainKeyboard() {
  String kb = F("[[{\"text\":\"⚡ Probudit\",\"callback_data\":\"wake\"},"
                "{\"text\":\"📊 Stav\",\"callback_data\":\"status\"}],");
  if (helperConfigured()) {
    kb += F("[{\"text\":\"😴 Uspat\",\"callback_data\":\"sleep\"},"
            "{\"text\":\"⏻ Vypnout\",\"callback_data\":\"shutdown\"}],");
  }
  kb += notifyEnabled ? F("[{\"text\":\"🔕 Vypnout oznámení\",\"callback_data\":\"notify\"}]]")
                      : F("[{\"text\":\"🔔 Zapnout oznámení\",\"callback_data\":\"notify\"}]]");
  return kb;
}

// Pošle zprávu s hlavní nabídkou tlačítek.
void sendWithMenu(const String &chatId, const String &text) {
  bot.sendMessageWithInlineKeyboard(chatId, text, "", mainKeyboard());
  watchdogFeed();
}

// Pošle zprávu všem povoleným uživatelům (kromě exceptChatId, pokud je zadané).
void notifyAllowedUsers(const String &text, const String &exceptChatId = "") {
  for (size_t i = 0; i < ALLOWED_CHAT_COUNT; i++) {
    if (exceptChatId == ALLOWED_CHAT_IDS[i]) continue;
    sendWithMenu(ALLOWED_CHAT_IDS[i], text);
  }
}

// ---------------------------------------------------------------------
//  Wi-Fi
// ---------------------------------------------------------------------

// Když uživatel do 3 s po zapnutí podrží tlačítko BOOT, smaže se uložená
// Wi-Fi a otevře se nastavovací Wi-Fi (např. po změně routeru či hesla).
bool bootButtonHeldAtStart() {
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  ledStartBlinking(50);  // rychlé blikání = "teď můžeš držet BOOT"
  bool held = false;
  uint32_t start = millis();
  while (millis() - start < 3000) {
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      held = true;
      break;
    }
    delay(20);
  }
  ledStopBlinking();
  return held;
}

// Připojení k Wi-Fi přes WiFiManager. Pokud uložená Wi-Fi nefunguje,
// ESP32 vytvoří vlastní Wi-Fi "WoL-Spinac" s nastavovací stránkou (captive portal).
void connectWifi() {
  WiFiManager wm;
  WiFi.mode(WIFI_STA);

  if (bootButtonHeldAtStart()) {
    Serial.println(F("[WiFi] Tlačítko BOOT drženo – mažu uloženou Wi-Fi."));
    wm.resetSettings();
  }

  ledStartBlinking(250);  // pomalé blikání = připojuji se
  wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_S);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setTitle("WoL spínač");

  Serial.printf("[WiFi] Připojuji se (pokud to nepůjde, otevře se Wi-Fi \"%s\")...\n", PORTAL_SSID);
  if (!wm.autoConnect(PORTAL_SSID, PORTAL_PASSWORD)) {
    // Nikdo Wi-Fi nenastavil, nebo router ještě nenaběhl (např. po výpadku proudu).
    Serial.println(F("[WiFi] Nepřipojeno, restartuji a zkusím to znovu."));
    delay(1000);
    ESP.restart();
  }

  ledStopBlinking();
  WiFi.setAutoReconnect(true);
  Serial.printf("[WiFi] Připojeno k \"%s\", IP %s, signál %s\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str(), formatWifiSignal().c_str());
}

// Hlídá Wi-Fi v hlavní smyčce. Vrací true, když je Wi-Fi připojená.
// Při výpadku zkouší znovu připojit, po dlouhém výpadku restartuje ESP32.
bool maintainWifi() {
  uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (wifiLostSinceMs != 0) {
      Serial.printf("[WiFi] Znovu připojeno po %lu s.\n", (now - wifiLostSinceMs) / 1000);
      wifiLostSinceMs = 0;
      ledStopBlinking();
    }
    return true;
  }

  if (wifiLostSinceMs == 0) {
    Serial.println(F("[WiFi] Spojení ztraceno, zkouším znovu připojit."));
    wifiLostSinceMs = now == 0 ? 1 : now;
    lastWifiRetryMs = now;
    ledStartBlinking(250);
  }

  if (now - wifiLostSinceMs > WIFI_MAX_OUTAGE_MS) {
    Serial.println(F("[WiFi] Výpadek trvá přes 10 minut, restartuji ESP32."));
    delay(500);
    ESP.restart();
  }

  if (now - lastWifiRetryMs > WIFI_RETRY_INTERVAL_MS) {
    lastWifiRetryMs = now;
    Serial.println(F("[WiFi] Nový pokus o připojení..."));
    WiFi.reconnect();
  }
  return false;
}

// ---------------------------------------------------------------------
//  Nahrávání firmwaru přes Wi-Fi (OTA)
// ---------------------------------------------------------------------

// Zapne OTA: z PlatformIO pak jde nahrát firmware přes Wi-Fi (prostředí "ota").
// Bez hesla v secrets.h zůstane OTA vypnuté – do ESP32 by jinak mohl nahrát
// cokoli kdokoli v domácí síti.
void setupOta() {
  if (!otaConfigured()) {
    Serial.println(F("[OTA] Vypnuto (v secrets.h chybí OTA_PASSWORD)."));
    return;
  }
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println(F("[OTA] Nahrávám nový firmware..."));
    ledStartBlinking(100);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    watchdogFeed();  // nahrávání trvá déle než jedno kolo loop()
  });
  ArduinoOTA.onEnd([]() {
    Serial.println(F("[OTA] Hotovo, restartuji."));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Chyba %u.\n", error);
    ledStopBlinking();
  });
  ArduinoOTA.begin();
  Serial.printf("[OTA] Zapnuto, adresa %s.local\n", OTA_HOSTNAME);
}

// ---------------------------------------------------------------------
//  Čas a staré zprávy
// ---------------------------------------------------------------------

// Je hodinový čas platný (synchronizovaný z internetu)?
bool clockValid() {
  return time(nullptr) >= 1700000000;
}

// Zjistí přesný čas z internetu (NTP) a spočítá, kdy bylo ESP32 zapnuto.
// Slouží jako pojistka proti zpracování zpráv odeslaných před zapnutím
// a k podepisování příkazů pro pomocníka na PC.
void syncClock() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  uint32_t start = millis();
  while (!clockValid() && millis() - start < 10000) {
    delay(200);
  }
  if (clockValid()) {
    bootEpoch = time(nullptr) - (time_t)(esp_timer_get_time() / 1000000ULL);
    Serial.printf("[Čas] Synchronizováno, ESP32 zapnuto v unix čase %ld.\n", (long)bootEpoch);
  } else {
    Serial.println(F("[Čas] NTP nedostupné, kontrola stáří zpráv vypnuta."));
  }
}

// Zahodí všechny zprávy, které čekají na Telegramu z doby, kdy bylo ESP32
// vypnuté. Jinak by se třeba po výpadku proudu PC samo zaplo kvůli
// starému /wake. Zkouší to, dokud se to nepovede (Telegram musí odpovědět).
void skipPendingMessages() {
  String command = String("bot") + BOT_TOKEN + "/getUpdates?offset=-1&limit=1";
  for (uint8_t attempt = 1; attempt <= 10; attempt++) {
    watchdogFeed();
    String response = bot.sendGetToTelegram(command);
    JsonDocument doc;
    if (response.length() > 0 && !deserializeJson(doc, response) && doc["ok"] == true) {
      JsonArray result = doc["result"].as<JsonArray>();
      if (result.size() > 0) {
        // Další getUpdates s offsetem o 1 vyšším potvrdí (smaže) všechny starší zprávy.
        bot.last_message_received = result[0]["update_id"].as<long>();
        Serial.printf("[Bot] Staré zprávy přeskočeny (poslední update_id %ld).\n", bot.last_message_received);
      } else {
        Serial.println(F("[Bot] Žádné staré zprávy."));
      }
      return;
    }
    Serial.printf("[Bot] Telegram neodpověděl (pokus %u/10), zkusím znovu.\n", attempt);
    delay(3000);
  }
  // Nepovedlo se – zprávy pak odfiltruje aspoň kontrola času v isOldMessage().
  Serial.println(F("[Bot] Staré zprávy se nepodařilo přeskočit."));
}

// Byla zpráva odeslaná ještě před zapnutím ESP32?
// Kliknutí na tlačítko (callback_query) Telegram datum nedává – ta se
// kontrolují jinak (potvrzení uspání/vypnutí platí jen 2 minuty).
bool isOldMessage(const telegramMessage &msg) {
  if (bootEpoch == 0 || msg.type == "callback_query") return false;
  return msg.date.toInt() < (long)bootEpoch;
}

// ---------------------------------------------------------------------
//  Pomocník na PC (uspání / vypnutí)
// ---------------------------------------------------------------------

// HMAC-SHA256 podpis zprávy tajným heslem PC_HELPER_SECRET (hex text).
// Heslo samotné po síti nikdy nejde – jen podpis, který platí pro
// jeden příkaz a krátkou dobu, takže ho nejde zachytit a použít znovu.
String hmacSha256Hex(const String &message) {
  uint8_t out[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                  (const uint8_t *)PC_HELPER_SECRET, strlen(PC_HELPER_SECRET),
                  (const uint8_t *)message.c_str(), message.length(), out);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", out[i]);
  return String(hex);
}

// Výsledky volání pomocníka.
enum HelperResult : uint8_t { HELPER_OK, HELPER_NO_CLOCK, HELPER_UNREACHABLE, HELPER_BAD_SECRET, HELPER_ERROR };

// Pošle pomocníkovi na PC příkaz ("status", "sleep" nebo "shutdown").
HelperResult callPcHelper(const char *action) {
  if (!clockValid()) return HELPER_NO_CLOCK;
  String timestamp = String((long)time(nullptr));
  String signature = hmacSha256Hex(String(action) + ":" + timestamp);

  WiFiClient client;
  if (!client.connect(pcIp, PC_HELPER_PORT, HELPER_CONNECT_TIMEOUT_MS)) {
    Serial.printf("[Pomocník] %s:%d neodpovídá.\n", pcIp.toString().c_str(), PC_HELPER_PORT);
    return HELPER_UNREACHABLE;
  }
  client.printf("POST /%s HTTP/1.0\r\nHost: %s\r\nX-Time: %s\r\nX-Sig: %s\r\nContent-Length: 0\r\n\r\n",
                action, pcIp.toString().c_str(), timestamp.c_str(), signature.c_str());

  // Stačí nám první řádek odpovědi, např. "HTTP/1.0 200 OK".
  String statusLine;
  uint32_t start = millis();
  while (millis() - start < HELPER_RESPONSE_TIMEOUT_MS) {
    if (client.available()) {
      char c = client.read();
      if (c == '\n') break;
      statusLine += c;
    } else if (!client.connected()) {
      break;
    } else {
      delay(10);
    }
  }
  client.stop();
  watchdogFeed();

  int code = statusLine.startsWith("HTTP/") ? statusLine.substring(9, 12).toInt() : 0;
  Serial.printf("[Pomocník] %s -> %d\n", action, code);
  if (code == 200) return HELPER_OK;
  if (code == 401) return HELPER_BAD_SECRET;
  if (code == 0) return HELPER_UNREACHABLE;
  return HELPER_ERROR;
}

// Lidsky čitelná chyba pomocníka.
String helperErrorText(HelperResult r) {
  switch (r) {
    case HELPER_NO_CLOCK:
      return "⚠️ ESP32 ještě nezná přesný čas z internetu, zkus to za minutu.";
    case HELPER_UNREACHABLE:
      return "⚠️ Pomocník na PC neodpovídá. Běží na PC úloha „WoL spinac helper“? (README → Uspání a vypnutí)";
    case HELPER_BAD_SECRET:
      return "⚠️ Pomocník na PC odmítl příkaz – nesedí heslo PC_HELPER_SECRET, nebo má PC/ESP32 špatný čas.";
    default:
      return "⚠️ Pomocník na PC vrátil chybu. Podrobnosti jsou v C:\\ProgramData\\WoLSpinac\\helper.log.";
  }
}

// ---------------------------------------------------------------------
//  Hlídání PC (ping) a oznámení
// ---------------------------------------------------------------------

// Změní uložený stav PC a případně pošle oznámení. Komu jsme už napsali
// konkrétnější zprávu (např. "PC naběhlo"), toho vynecháme.
void setPcState(PcState newState, const String &alreadyToldChatId = "") {
  if (newState == pcState) return;
  PcState oldState = pcState;
  pcState = newState;
  Serial.printf("[PC] %s\n", newState == PC_ON ? "zapnuto" : "vypnuto");
  if (oldState == PC_UNKNOWN || !notifyEnabled) return;  // první měření po startu neoznamujeme
  notifyAllowedUsers(newState == PC_ON ? "🟢 PC se zapnulo." : "⚫ PC se vypnulo (nebo uspalo).",
                     alreadyToldChatId);
}

// Vyhodnotí jeden ping: dokončí čekání po /wake nebo vypnutí a aktualizuje stav.
void handlePingResult(bool online) {
  if (online) pcFailedPings = 0;
  else if (pcFailedPings < 255) pcFailedPings++;

  String toldChatId;
  if (pending == PENDING_WAKE && online) {
    uint32_t seconds = (millis() - pendingStartMs) / 1000;
    sendWithMenu(pendingChatId, "✅ PC naběhlo (za " + String(seconds) +
                                    " s). Můžeš se připojit přes Moonlight/Artemis.");
    toldChatId = pendingChatId;
    pending = PENDING_NONE;
  } else if (pending == PENDING_OFF && pcFailedPings >= 2) {
    sendWithMenu(pendingChatId, pendingIsSleep ? "😴 PC je uspané." : "⚫ PC je vypnuté.");
    toldChatId = pendingChatId;
    pending = PENDING_NONE;
    setPcState(PC_OFF, toldChatId);
    return;
  }

  if (online) setPcState(PC_ON, toldChatId);
  else if (pcFailedPings >= PC_OFF_AFTER_FAILS) setPcState(PC_OFF, toldChatId);
}

// Volá se z loop(): pinguje PC v pravidelných intervalech a hlídá časové limity.
void monitorPc() {
  uint32_t now = millis();

  if (pending == PENDING_WAKE && now - pendingStartMs > WAKE_WAIT_MS) {
    Serial.println(F("[WoL] PC neodpovědělo do 90 s."));
    sendWithMenu(pendingChatId,
                 "❌ PC ani po 90 s neodpovídá na ping.\n"
                 "Může ještě startovat – zkus za chvíli 📊 Stav.\n"
                 "Pokud se nezapne vůbec, mrkni do README na „Řešení problémů“.");
    pending = PENDING_NONE;
  } else if (pending == PENDING_OFF && now - pendingStartMs > OFF_WAIT_MS) {
    sendWithMenu(pendingChatId, "⚠️ PC ani po 2 minutách nezhaslo (pořád odpovídá na ping).\n"
                                "Možná ho drží otevřený program – zkus to znovu, nebo se podívej přes Moonlight.");
    pending = PENDING_NONE;
  }

  uint32_t interval = pending != PENDING_NONE ? PC_FAST_PING_INTERVAL_MS : PC_MONITOR_INTERVAL_MS;
  if (lastPcPingMs != 0 && now - lastPcPingMs < interval) return;
  lastPcPingMs = now == 0 ? 1 : now;
  bool online = isPcOnline();
  watchdogFeed();
  handlePingResult(online);
}

// ---------------------------------------------------------------------
//  Příkazy bota
// ---------------------------------------------------------------------

void cmdHelp(const String &chatId) {
  String text =
      "🖥️ WoL spínač – stačí klikat na tlačítka, nebo psát příkazy:\n\n"
      "/wake – probudí PC a ohlásí, až naběhne\n"
      "/status – je PC zapnuté? + stav spínače\n";
  if (helperConfigured()) {
    text += "/sleep – uspí PC (s potvrzením)\n"
            "/shutdown – vypne PC (s potvrzením)\n";
  }
  text += "/notify – zapne/vypne oznámení, když se PC zapne nebo vypne\n"
          "/help – tento seznam";
  sendWithMenu(chatId, text);
}

void cmdStatus(const String &chatId) {
  bool online = isPcOnline();
  watchdogFeed();
  if (online) {
    pcFailedPings = 0;
    setPcState(PC_ON, chatId);
  }
  String text = online ? "🟢 PC je zapnuté." : "🔴 PC je vypnuté (neodpovídá na ping).";

  if (helperConfigured() && online) {
    HelperResult r = callPcHelper("status");
    text += r == HELPER_OK ? "\n🧩 Pomocník na PC: běží (uspání/vypnutí funguje)"
                           : "\n" + helperErrorText(r);
  }
  text += "\n\n🔔 Oznámení: " + String(notifyEnabled ? "zapnutá" : "vypnutá");
  text += "\n⏱️ Spínač běží: " + formatUptime();
  text += "\n📶 Wi-Fi signál: " + formatWifiSignal();
  text += "\nℹ️ Firmware " + String(FW_VERSION) + ", IP " + WiFi.localIP().toString();
  sendWithMenu(chatId, text);
}

// Odešle magic packet na broadcast adresu domácí podsítě.
bool sendWakePacket() {
  IPAddress broadcast = wol.calculateBroadcastAddress(WiFi.localIP(), WiFi.subnetMask());
  wol.setBroadcastAddress(broadcast);
  wol.setRepeat(WOL_REPEAT, WOL_REPEAT_DELAY_MS);
  Serial.printf("[WoL] Posílám magic packet na %s pro MAC %s\n", broadcast.toString().c_str(), PC_MAC);
  bool ok = wol.sendMagicPacket(PC_MAC, WOL_PORT);
  ledFlash(3);
  return ok;
}

// Probudí PC. Na výsledek se nečeká tady – hlídá ho monitorPc(), takže bot
// mezitím dál odpovídá na další zprávy.
void cmdWake(const String &chatId) {
  if (pending == PENDING_WAKE) {
    bot.sendMessage(chatId, "⏳ Už čekám, až PC naběhne.", "");
    return;
  }
  if (isPcOnline()) {
    pcFailedPings = 0;
    setPcState(PC_ON, chatId);
    sendWithMenu(chatId, "🟢 PC už je zapnuté, není co budit.");
    return;
  }

  if (!sendWakePacket()) {
    bot.sendMessage(chatId, "⚠️ Magic packet se nepodařilo odeslat. Zkontroluj MAC adresu v secrets.h.", "");
    return;
  }
  bot.sendMessage(chatId, "📨 Magic packet odeslán, čekám až 90 s, jestli PC naběhne…", "");

  // PC teď určitě neběží – zapíšeme to potichu, ať se jeho naběhnutí
  // ohlásí jako změna ostatním uživatelům (pokud mají oznámení).
  pcState = PC_OFF;
  pending = PENDING_WAKE;
  pendingChatId = chatId;
  pendingStartMs = millis();
  lastPcPingMs = pendingStartMs;  // první kontrola za 3 s
}

// Uspání/vypnutí – první krok: zeptá se, jestli to uživatel myslí vážně.
void askConfirm(const String &chatId, const String &action) {
  if (!helperConfigured()) {
    bot.sendMessage(chatId, "Uspání a vypnutí není nastavené (chybí PC_HELPER_SECRET v secrets.h a pomocník na PC).", "");
    return;
  }
  bool sleep = action == "sleep";
  confirmAction = action;
  confirmNonce = esp_random();
  confirmStartMs = millis();
  String yes = action + "!" + String(confirmNonce);
  String keyboard = String("[[{\"text\":\"") + (sleep ? "✅ Ano, uspat" : "✅ Ano, vypnout") +
                    "\",\"callback_data\":\"" + yes + "\"},"
                    "{\"text\":\"❌ Zrušit\",\"callback_data\":\"cancel\"}]]";
  bot.sendMessageWithInlineKeyboard(chatId,
                                    sleep ? "😴 Opravdu uspat PC?"
                                          : "⏻ Opravdu vypnout PC?\nNeuložená práce na PC se ztratí.",
                                    "", keyboard);
}

// Uspání/vypnutí – druhý krok: uživatel klikl na "Ano".
// messageId = zpráva s potvrzením, kterou přepíšeme (zmizí tlačítka).
void doPowerAction(const String &chatId, const String &action, int messageId) {
  bool sleep = action == "sleep";
  confirmAction = "";

  if (!isPcOnline()) {
    bot.sendMessage(chatId, "⚫ PC už je vypnuté.", "", messageId);
    return;
  }
  bot.sendMessage(chatId, sleep ? "😴 Uspávám PC…" : "⏻ Vypínám PC…", "", messageId);

  HelperResult r = callPcHelper(sleep ? "sleep" : "shutdown");
  if (r != HELPER_OK) {
    sendWithMenu(chatId, helperErrorText(r));
    return;
  }
  pending = PENDING_OFF;
  pendingIsSleep = sleep;
  pendingChatId = chatId;
  pendingStartMs = millis();
  pcFailedPings = 0;
  lastPcPingMs = pendingStartMs;
}

void cmdNotify(const String &chatId) {
  notifyEnabled = !notifyEnabled;
  prefs.putBool("notify", notifyEnabled);
  sendWithMenu(chatId, notifyEnabled ? "🔔 Oznámení zapnutá – napíšu, když se PC zapne nebo vypne."
                                     : "🔕 Oznámení vypnutá.");
}

// Kliknutí na tlačítko pod zprávou.
void handleButton(const telegramMessage &msg) {
  bot.answerCallbackQuery(msg.query_id);  // jinak by se na tlačítku točilo kolečko
  String data = msg.text;
  Serial.printf("[Bot] %s (%s) klikl: %s\n", msg.from_name.c_str(), msg.chat_id.c_str(), data.c_str());

  if (data == "wake") cmdWake(msg.chat_id);
  else if (data == "status") cmdStatus(msg.chat_id);
  else if (data == "notify") cmdNotify(msg.chat_id);
  else if (data == "sleep" || data == "shutdown") askConfirm(msg.chat_id, data);
  else if (data == "cancel") {
    confirmAction = "";
    bot.sendMessage(msg.chat_id, "Zrušeno.", "", msg.message_id);
  } else {
    // Potvrzení "sleep!<číslo>" / "shutdown!<číslo>" – platí jen poslední a jen 2 minuty.
    int bang = data.indexOf('!');
    String action = bang > 0 ? data.substring(0, bang) : "";
    bool valid = action.length() > 0 && action == confirmAction &&
                 data.substring(bang + 1) == String(confirmNonce) &&
                 millis() - confirmStartMs < CONFIRM_VALID_MS;
    if (valid) {
      doPowerAction(msg.chat_id, action, msg.message_id);
    } else {
      bot.sendMessage(msg.chat_id, "⌛ Tohle potvrzení už neplatí, zkus to znovu.", "", msg.message_id);
    }
  }
}

// Zpracuje jednu příchozí zprávu nebo kliknutí na tlačítko.
void handleMessage(const telegramMessage &msg) {
  if (!isAllowedChat(msg.chat_id)) {
    // Cizím neodpovídáme vůbec, jen to zapíšeme do sériové linky.
    Serial.printf("[Bot] Zpráva od nepovoleného chatu %s (%s): %s\n", msg.chat_id.c_str(),
                  msg.from_name.c_str(), msg.text.c_str());
    return;
  }
  if (isOldMessage(msg)) {
    Serial.printf("[Bot] Ignoruji starou zprávu z doby před zapnutím: %s\n", msg.text.c_str());
    return;
  }

  if (msg.type == "callback_query") {
    handleButton(msg);
    return;
  }
  if (msg.type != "message") return;  // upravené zprávy apod. ignorujeme

  // Ve skupinách přichází příkaz jako "/wake@JmenoBota" – část za @ zahodíme.
  String command = msg.text;
  command.trim();
  int at = command.indexOf('@');
  if (at > 0) command = command.substring(0, at);
  command.toLowerCase();

  Serial.printf("[Bot] %s (%s): %s\n", msg.from_name.c_str(), msg.chat_id.c_str(), command.c_str());

  if (command == "/wake") cmdWake(msg.chat_id);
  else if (command == "/status") cmdStatus(msg.chat_id);
  else if (command == "/sleep") askConfirm(msg.chat_id, "sleep");
  else if (command == "/shutdown") askConfirm(msg.chat_id, "shutdown");
  else if (command == "/notify") cmdNotify(msg.chat_id);
  else if (command == "/help" || command == "/start") cmdHelp(msg.chat_id);
  else sendWithMenu(msg.chat_id, "Tomuhle nerozumím 🙂 Klikni na tlačítko, nebo napiš /help.");
}

// Stáhne nové zprávy z Telegramu a zpracuje je.
void pollTelegram() {
  int count = bot.getUpdates(bot.last_message_received + 1);
  while (count > 0) {
    for (int i = 0; i < count; i++) {
      handleMessage(bot.messages[i]);
      watchdogFeed();
    }
    count = bot.getUpdates(bot.last_message_received + 1);
  }
}

// Občas ověří, že Telegram opravdu odpovídá. Když 3× za sebou ne
// (např. zaseknuté spojení), ESP32 se restartuje.
void checkTelegramConnection() {
  if (bot.getMe()) {
    failedTelegramChecks = 0;
    return;
  }
  failedTelegramChecks++;
  Serial.printf("[Bot] Telegram neodpovídá (%u/%u).\n", failedTelegramChecks, TELEGRAM_MAX_FAILED_CHECKS);
  if (failedTelegramChecks >= TELEGRAM_MAX_FAILED_CHECKS) {
    Serial.println(F("[Bot] Telegram dlouho nedostupný, restartuji ESP32."));
    delay(500);
    ESP.restart();
  }
}

// Zaregistruje příkazy, aby je Telegram nabízel v menu.
void registerBotCommands() {
  String commands = F("[{\"command\":\"wake\",\"description\":\"Probudit PC\"},"
                      "{\"command\":\"status\",\"description\":\"Je PC zapnuté?\"},");
  if (helperConfigured()) {
    commands += F("{\"command\":\"sleep\",\"description\":\"Uspat PC\"},"
                  "{\"command\":\"shutdown\",\"description\":\"Vypnout PC\"},");
  }
  commands += F("{\"command\":\"notify\",\"description\":\"Oznámení zap/vyp\"},"
                "{\"command\":\"help\",\"description\":\"Seznam příkazů\"}]");
  bot.setMyCommands(commands);
}

// ---------------------------------------------------------------------
//  setup() a loop()
// ---------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.printf("=== WoL spínač %s startuje ===\n", FW_VERSION);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (!pcIp.fromString(PC_IP)) {
    Serial.printf("[Chyba] PC_IP \"%s\" v secrets.h není platná IP adresa!\n", PC_IP);
  }

  prefs.begin("wol", false);
  notifyEnabled = prefs.getBool("notify", true);

  connectWifi();
  watchdogStart();
  setupOta();

  secureClient.setCACert(TELEGRAM_ROOT_CA);
  syncClock();
  watchdogFeed();

  skipPendingMessages();
  registerBotCommands();
  watchdogFeed();

  // Zjistíme, jestli PC běží (první měření se neoznamuje jako změna).
  monitorPc();

  String pcText = pcState == PC_ON ? "zapnuté 🟢" : "vypnuté 🔴";
  notifyAllowedUsers("✅ WoL spínač online\nPC je " + pcText + "\nWi-Fi signál: " + formatWifiSignal());
  Serial.println(F("=== Připraveno, čekám na příkazy ==="));
  lastTelegramCheckMs = millis();
}

void loop() {
  watchdogFeed();

  if (!maintainWifi()) {
    delay(200);
    return;
  }

  if (otaConfigured()) ArduinoOTA.handle();

  uint32_t now = millis();
  if (now - lastBotPollMs >= BOT_POLL_INTERVAL_MS) {
    lastBotPollMs = now;
    pollTelegram();
  }

  monitorPc();

  now = millis();
  if (now - lastTelegramCheckMs >= TELEGRAM_CHECK_INTERVAL_MS) {
    lastTelegramCheckMs = now;
    checkTelegramConnection();
  }

  delay(10);
}
