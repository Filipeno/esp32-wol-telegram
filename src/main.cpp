/*
 * Wake-on-LAN spínač ovládaný přes Telegram
 * ==========================================
 * Deska: ESP32 DevKit (ESP32-WROOM-32), framework Arduino.
 *
 * Jak to funguje:
 *  - ESP32 se samo pravidelně ptá Telegramu na nové zprávy (tzv. polling).
 *    Proto NENÍ potřeba otevírat žádné porty na routeru – veškerá komunikace
 *    jde z domácí sítě ven, stejně jako když si otevřeš web v prohlížeči.
 *  - Na příkaz /wake pošle do domácí sítě "magic packet", který probudí PC.
 *  - Pak PC pinguje a ohlásí, jestli naběhlo.
 *
 * Údaje (token, chat ID, MAC a IP PC) jsou v include/secrets.h.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <WakeOnLan.h>
#include <ESP32Ping.h>
#include <Ticker.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <time.h>

#include "telegram_cert.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Chybí include/secrets.h – zkopíruj include/secrets.example.h jako include/secrets.h a vyplň ho."
#endif

// Heslo k Wi-Fi síti "WoL-Spinac", kterou ESP32 vytvoří pro nastavení Wi-Fi.
// Starší secrets.h ho nemusí obsahovat, proto výchozí hodnota.
#ifndef PORTAL_PASSWORD
#define PORTAL_PASSWORD "wolspinac"
#endif

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

// Jak často se ptát Telegramu na nové zprávy.
static const uint32_t BOT_POLL_INTERVAL_MS = 2000;

// Wake-on-LAN: 3 pakety s rozestupem 100 ms, UDP port 9.
static const uint8_t WOL_REPEAT = 3;
static const uint32_t WOL_REPEAT_DELAY_MS = 100;
static const uint16_t WOL_PORT = 9;

// Po odeslání WoL čekáme až 90 s, jestli PC začne odpovídat na ping.
static const uint32_t WAKE_WAIT_MS = 90UL * 1000UL;
static const uint32_t WAKE_PING_INTERVAL_MS = 3000;

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
IPAddress pcIp;

static const size_t ALLOWED_CHAT_COUNT = sizeof(ALLOWED_CHAT_IDS) / sizeof(ALLOWED_CHAT_IDS[0]);

uint32_t lastBotPollMs = 0;
uint32_t lastTelegramCheckMs = 0;
uint8_t failedTelegramChecks = 0;
uint32_t wifiLostSinceMs = 0;   // 0 = Wi-Fi je v pořádku
uint32_t lastWifiRetryMs = 0;
time_t bootEpoch = 0;           // čas zapnutí ESP32 (unix čas), 0 = neznámý

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

// Pošle zprávu všem povoleným uživatelům.
void notifyAllowedUsers(const String &text) {
  for (size_t i = 0; i < ALLOWED_CHAT_COUNT; i++) {
    bot.sendMessage(ALLOWED_CHAT_IDS[i], text, "");
    watchdogFeed();
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
//  Čas a staré zprávy
// ---------------------------------------------------------------------

// Zjistí přesný čas z internetu (NTP) a spočítá, kdy bylo ESP32 zapnuto.
// Slouží jako pojistka proti zpracování zpráv odeslaných před zapnutím.
void syncClock() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  uint32_t start = millis();
  while (time(nullptr) < 1700000000 && millis() - start < 10000) {
    delay(200);
  }
  time_t now = time(nullptr);
  if (now >= 1700000000) {
    bootEpoch = now - (time_t)(esp_timer_get_time() / 1000000ULL);
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
bool isOldMessage(const telegramMessage &msg) {
  if (bootEpoch == 0) return false;
  return msg.date.toInt() < (long)bootEpoch;
}

// ---------------------------------------------------------------------
//  Příkazy bota
// ---------------------------------------------------------------------

void cmdHelp(const String &chatId) {
  String text =
      "🖥️ WoL spínač – příkazy:\n\n"
      "/wake – probudí PC a ohlásí, až naběhne\n"
      "/status – je PC zapnuté? + stav spínače\n"
      "/help – tento seznam";
  bot.sendMessage(chatId, text, "");
}

void cmdStatus(const String &chatId) {
  bool online = isPcOnline();
  watchdogFeed();
  String text = online ? "🟢 PC je zapnuté." : "🔴 PC je vypnuté (neodpovídá na ping).";
  text += "\n\n⏱️ Spínač běží: " + formatUptime();
  text += "\n📶 Wi-Fi signál: " + formatWifiSignal();
  bot.sendMessage(chatId, text, "");
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

void cmdWake(const String &chatId) {
  if (isPcOnline()) {
    bot.sendMessage(chatId, "🟢 PC už je zapnuté, není co budit.", "");
    return;
  }

  if (!sendWakePacket()) {
    bot.sendMessage(chatId, "⚠️ Magic packet se nepodařilo odeslat. Zkontroluj MAC adresu v secrets.h.", "");
    return;
  }
  bot.sendMessage(chatId, "📨 Magic packet odeslán, čekám až 90 s, jestli PC naběhne…", "");

  // Pingujeme PC, dokud neodpoví, nebo nevyprší 90 s.
  uint32_t start = millis();
  while (millis() - start < WAKE_WAIT_MS) {
    watchdogFeed();
    if (isPcOnline()) {
      uint32_t seconds = (millis() - start) / 1000;
      Serial.printf("[WoL] PC odpovídá po %lu s.\n", seconds);
      bot.sendMessage(chatId, "✅ PC naběhlo (za " + String(seconds) + " s). Můžeš se připojit přes Moonlight/Artemis.", "");
      return;
    }
    delay(WAKE_PING_INTERVAL_MS);
  }

  Serial.println(F("[WoL] PC neodpovědělo do 90 s."));
  bot.sendMessage(chatId,
                  "❌ PC ani po 90 s neodpovídá na ping.\n"
                  "Může ještě startovat – zkus za chvíli /status.\n"
                  "Pokud se nezapne vůbec, mrkni do README na „Řešení problémů“.",
                  "");
}

// Zpracuje jednu příchozí zprávu.
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

  // Ve skupinách přichází příkaz jako "/wake@JmenoBota" – část za @ zahodíme.
  String command = msg.text;
  command.trim();
  int at = command.indexOf('@');
  if (at > 0) command = command.substring(0, at);
  command.toLowerCase();

  Serial.printf("[Bot] %s (%s): %s\n", msg.from_name.c_str(), msg.chat_id.c_str(), command.c_str());

  if (command == "/wake") cmdWake(msg.chat_id);
  else if (command == "/status") cmdStatus(msg.chat_id);
  else if (command == "/help" || command == "/start") cmdHelp(msg.chat_id);
  else bot.sendMessage(msg.chat_id, "Tomuhle nerozumím 🙂 Napiš /help.", "");
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
  bot.setMyCommands(
      F("[{\"command\":\"wake\",\"description\":\"Probudit PC\"},"
        "{\"command\":\"status\",\"description\":\"Je PC zapnuté?\"},"
        "{\"command\":\"help\",\"description\":\"Seznam příkazů\"}]"));
}

// ---------------------------------------------------------------------
//  setup() a loop()
// ---------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=== WoL spínač startuje ==="));

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (!pcIp.fromString(PC_IP)) {
    Serial.printf("[Chyba] PC_IP \"%s\" v secrets.h není platná IP adresa!\n", PC_IP);
  }

  connectWifi();
  watchdogStart();

  secureClient.setCACert(TELEGRAM_ROOT_CA);
  syncClock();
  watchdogFeed();

  skipPendingMessages();
  registerBotCommands();
  watchdogFeed();

  notifyAllowedUsers("✅ WoL spínač online\nIP v síti: " + WiFi.localIP().toString() +
                     "\nWi-Fi signál: " + formatWifiSignal());
  Serial.println(F("=== Připraveno, čekám na příkazy ==="));
  lastTelegramCheckMs = millis();
}

void loop() {
  watchdogFeed();

  if (!maintainWifi()) {
    delay(200);
    return;
  }

  uint32_t now = millis();
  if (now - lastBotPollMs >= BOT_POLL_INTERVAL_MS) {
    lastBotPollMs = now;
    pollTelegram();
  }

  if (now - lastTelegramCheckMs >= TELEGRAM_CHECK_INTERVAL_MS) {
    lastTelegramCheckMs = now;
    checkTelegramConnection();
  }

  delay(10);
}
