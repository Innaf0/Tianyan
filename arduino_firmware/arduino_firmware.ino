#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <string.h>

// ILI9341 and XPT2046 shared bus. The display keeps the software-SPI setup
// from the working graphics test.
constexpr uint8_t TFT_DC = 5;
constexpr uint8_t TFT_CS = 13;
constexpr uint8_t TFT_MOSI = 11;
constexpr uint8_t TFT_CLK = 14;
constexpr uint8_t TFT_RST = 15;
constexpr uint8_t TFT_MISO = 12;
constexpr uint8_t TOUCH_CS = 9;
constexpr uint8_t TOUCH_IRQ = 8;

// SD card on SPI0.
constexpr uint8_t SD_MISO = 4;
constexpr uint8_t SD_CLK = 6;
constexpr uint8_t SD_MOSI = 7;
constexpr uint8_t SD_CS = 21;

constexpr uint32_t MATCH_RETRY_MS = 30000;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint8_t DISPLAY_ROTATION = 1;  // Portrait in the device's mounted orientation.
constexpr int16_t LAYOUT_X = -10;

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_CLK, TFT_RST, TFT_MISO);

struct Config {
  String wifiSsid = "haz1";
  String wifiPassword = "84915801";
  String apiKey = "HDEV-4456951f-37f3-4f81-9f30-86d690763655";
  String playerRegion = "eu";
  String playerName = "Innaf";
  String playerTag = "Fanni";
};

Config config;
uint32_t lastFetchAttempt = 0;
bool firstFetch = true;
bool matchLoaded = false;

void drawStatus(const char *title, const String &detail = "") {
  tft.fillScreen(ILI9341_BLACK);
  tft.setCursor(LAYOUT_X, 12);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.println(title);
  if (detail.length() > 0) {
    tft.setCursor(LAYOUT_X, 42);
    tft.setTextColor(ILI9341_DARKGREY);
    tft.setTextSize(1);
    tft.println(detail);
  }
}

bool loadConfig() {
  SPI.setRX(SD_MISO);
  SPI.setSCK(SD_CLK);
  SPI.setTX(SD_MOSI);
  SPI.begin();

  if (!SD.begin(SD_CS, SPI)) {
    Serial.println("SD card initialization failed; using defaults");
    return false;
  }

  File file = SD.open("/CONFIG.TXT", FILE_READ);
  if (!file) {
    Serial.println("CONFIG.TXT not found; using defaults");
    return false;
  }

  JsonDocument document;
  DeserializationError error = deserializeJson(document, file);
  file.close();
  if (error) {
    Serial.print("Invalid CONFIG.TXT: ");
    Serial.println(error.c_str());
    return false;
  }

  if (document["wifi_ssid"].is<const char *>()) {
    config.wifiSsid = document["wifi_ssid"].as<const char *>();
  }
  if (document["wifi_password"].is<const char *>()) {
    config.wifiPassword = document["wifi_password"].as<const char *>();
  }
  if (document["api_key"].is<const char *>()) {
    config.apiKey = document["api_key"].as<const char *>();
  }
  if (document["player_region"].is<const char *>()) {
    config.playerRegion = document["player_region"].as<const char *>();
  }
  if (document["player_name"].is<const char *>()) {
    config.playerName = document["player_name"].as<const char *>();
  }
  if (document["player_tag"].is<const char *>()) {
    config.playerTag = document["player_tag"].as<const char *>();
  }
  Serial.println("Loaded configuration from CONFIG.TXT");
  return true;
}

bool connectWifi() {
  drawStatus("Connecting", config.wifiSsid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.wifiSsid.c_str(), config.wifiPassword.c_str());

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print('.');
  }

  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    const int status = WiFi.status();
    Serial.print("Wi-Fi connection failed, status: ");
    Serial.println(status);

    if (status == WL_NO_SSID_AVAIL) {
      drawStatus("Wi-Fi failed", "SSID not found; use 2.4 GHz");
    } else if (status == WL_CONNECT_FAILED) {
      drawStatus("Wi-Fi failed", "Check the password");
    } else {
      drawStatus("Wi-Fi timeout", "Check 2.4 GHz, SSID, password");
    }
    return false;
  }

  Serial.print("Wi-Fi connected, IP: ");
  Serial.println(WiFi.localIP());
  drawStatus("Connected", WiFi.localIP().toString());
  return true;
}

String urlEncode(const String &value) {
  const char hex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0f];
    }
  }
  return encoded;
}

void drawMatch(JsonObject match) {
  JsonObject metadata = match["metadata"];
  JsonObject teams = match["teams"];
  JsonArray players = match["players"]["all_players"];

  const uint16_t red = tft.color565(214, 45, 61);
  const uint16_t blue = tft.color565(48, 113, 247);

  tft.fillScreen(ILI9341_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);

  int16_t y = 10;
  tft.setCursor(LAYOUT_X, y);
  tft.print(metadata["map"].as<const char *>());
  tft.print(" | ");
  tft.println(metadata["mode"].as<const char *>());
  y += 14;

  const uint16_t redRounds = teams["red"]["rounds_won"] | 0;
  const uint16_t blueRounds = teams["blue"]["rounds_won"] | 0;
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(LAYOUT_X, y);
  tft.print("Red ");
  tft.print(redRounds);
  tft.print(" - ");
  tft.print(blueRounds);
  tft.println(" Blue");
  y += 14;

  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(LAYOUT_X, y);
  tft.print("Winner: ");
  tft.println(teams["red"]["has_won"].as<bool>() ? "Red" : "Blue");
  y += 10;
  tft.drawFastHLine(LAYOUT_X, y, tft.width() - 20, ILI9341_WHITE);
  y += 8;

  tft.setTextColor(ILI9341_DARKGREY);
  for (JsonObject player : players) {
    if (y > tft.height() - 15) {
      break;
    }

    const char *team = player["team"].as<const char *>();
    const uint16_t teamColor = team != nullptr && strcmp(team, "Red") == 0 ? red : blue;
    tft.setCursor(LAYOUT_X, y);
    tft.setTextColor(teamColor);
    tft.print(player["name"].as<const char *>());
    tft.setTextColor(ILI9341_DARKGREY);
    tft.print(" (");
    tft.print(player["character"].as<const char *>());
    tft.print(") ");
    tft.print(player["stats"]["kills"].as<unsigned int>());
    tft.print('/');
    tft.println(player["stats"]["deaths"].as<unsigned int>());
    y += 12;
  }
}

void logMatch(JsonObject match) {
  JsonObject metadata = match["metadata"];
  JsonObject teams = match["teams"];
  Serial.println("=== Match ===");
  Serial.print("Map: ");
  Serial.println(metadata["map"].as<const char *>());
  Serial.print("Mode: ");
  Serial.println(metadata["mode"].as<const char *>());
  Serial.print("Region: ");
  Serial.println(metadata["region"].as<const char *>());
  Serial.print("Started: ");
  Serial.println(metadata["game_start_patched"].as<const char *>());
  Serial.print("Rounds: ");
  Serial.println(metadata["rounds_played"].as<unsigned int>());
  Serial.print("Score: Red ");
  Serial.print(teams["red"]["rounds_won"].as<unsigned int>());
  Serial.print(" - ");
  Serial.print(teams["blue"]["rounds_won"].as<unsigned int>());
  Serial.println(" Blue");

  Serial.println("=== Players ===");
  for (JsonObject player : match["players"]["all_players"].as<JsonArray>()) {
    Serial.print('[');
    Serial.print(player["team"].as<const char *>());
    Serial.print("] ");
    Serial.print(player["name"].as<const char *>());
    Serial.print(" #");
    Serial.print(player["tag"].as<const char *>());
    Serial.print(" | ");
    Serial.print(player["character"].as<const char *>());
    Serial.print(" | ");
    Serial.print(player["currenttier_patched"].as<const char *>());
    Serial.print(" | K/D/A: ");
    Serial.print(player["stats"]["kills"].as<unsigned int>());
    Serial.print('/');
    Serial.print(player["stats"]["deaths"].as<unsigned int>());
    Serial.print('/');
    Serial.print(player["stats"]["assists"].as<unsigned int>());
    Serial.print(" | HS: ");
    Serial.println(player["stats"]["headshots"].as<unsigned int>());
  }
}

bool fetchAndDrawMatch() {
  if (WiFi.status() != WL_CONNECTED && !connectWifi()) {
    return false;
  }

  String url = "https://api.henrikdev.xyz/valorant/v3/matches/";
  url += urlEncode(config.playerRegion) + "/";
  url += urlEncode(config.playerName) + "/";
  url += urlEncode(config.playerTag) + "?size=1";

  Serial.print("Fetching: ");
  Serial.println(url);
  drawStatus("Loading match");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) {
    Serial.println("Unable to start HTTPS request");
    drawStatus("Request failed", "Could not open HTTPS connection");
    return false;
  }

  http.addHeader("Authorization", config.apiKey);
  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    Serial.print("API request failed, HTTP ");
    Serial.println(statusCode);
    drawStatus("Request failed", "HTTP " + String(statusCode));
    http.end();
    return false;
  }

  Serial.print("Response size: ");
  Serial.println(http.getSize());

  JsonDocument filter;
  filter["status"] = true;
  filter["data"][0]["metadata"]["map"] = true;
  filter["data"][0]["metadata"]["mode"] = true;
  filter["data"][0]["metadata"]["region"] = true;
  filter["data"][0]["metadata"]["game_start_patched"] = true;
  filter["data"][0]["metadata"]["rounds_played"] = true;
  filter["data"][0]["teams"]["red"]["rounds_won"] = true;
  filter["data"][0]["teams"]["red"]["has_won"] = true;
  filter["data"][0]["teams"]["blue"]["rounds_won"] = true;
  filter["data"][0]["players"]["all_players"][0]["team"] = true;
  filter["data"][0]["players"]["all_players"][0]["name"] = true;
  filter["data"][0]["players"]["all_players"][0]["tag"] = true;
  filter["data"][0]["players"]["all_players"][0]["character"] = true;
  filter["data"][0]["players"]["all_players"][0]["currenttier_patched"] = true;
  filter["data"][0]["players"]["all_players"][0]["stats"] = true;

  JsonDocument document;
  DeserializationError error = deserializeJson(
      document, http.getStream(), DeserializationOption::Filter(filter),
      DeserializationOption::NestingLimit(32));
  http.end();
  if (error) {
    Serial.print("API JSON parse failed: ");
    Serial.println(error.c_str());
    drawStatus("Invalid response", error.c_str());
    return false;
  }

  if ((document["status"] | 0) != 200 || document["data"].size() == 0) {
    Serial.println("API returned no match data");
    drawStatus("No match found");
    return false;
  }

  JsonObject match = document["data"][0];
  logMatch(match);
  drawMatch(match);
  Serial.println("Match display updated");
  return true;
}

uint16_t touchTransfer(uint8_t command) {
  for (int8_t bit = 7; bit >= 0; --bit) {
    digitalWrite(TFT_CLK, LOW);
    digitalWrite(TFT_MOSI, (command >> bit) & 1);
    delayMicroseconds(1);
    digitalWrite(TFT_CLK, HIGH);
    delayMicroseconds(1);
  }

  uint16_t value = 0;
  for (uint8_t bit = 0; bit < 16; ++bit) {
    digitalWrite(TFT_CLK, LOW);
    delayMicroseconds(1);
    digitalWrite(TFT_CLK, HIGH);
    value = (value << 1) | digitalRead(TFT_MISO);
    delayMicroseconds(1);
  }
  return value >> 3;
}

bool readTouch(int16_t &x, int16_t &y) {
  if (digitalRead(TOUCH_IRQ) != LOW) {
    return false;
  }

  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TOUCH_CS, LOW);
  const int32_t rawX = touchTransfer(0x90);
  const int32_t rawY = touchTransfer(0xd0);
  digitalWrite(TOUCH_CS, HIGH);

  x = constrain((rawX - 3880) * tft.width() / (340 - 3880), 0,
                tft.width() - 1);
  y = constrain((rawY - 262) * tft.height() / (3850 - 262), 0,
                tft.height() - 1);
  return !(x == 0 && y == 0);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Valorant Flex starting");

  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT_PULLUP);

  tft.begin();
  // Clear through every address orientation to remove pixels left by old firmware.
  for (uint8_t rotation = 0; rotation < 4; ++rotation) {
    tft.setRotation(rotation);
    tft.fillScreen(ILI9341_BLACK);
  }
  tft.setRotation(DISPLAY_ROTATION);
  drawStatus("Valorant Flex", "Starting...");

  loadConfig();
}

void loop() {
  const uint32_t now = millis();
  if (!matchLoaded && (firstFetch || now - lastFetchAttempt >= MATCH_RETRY_MS)) {
    firstFetch = false;
    lastFetchAttempt = now;
    matchLoaded = fetchAndDrawMatch();
    if (matchLoaded) {
      Serial.println("Match loaded; automatic reload disabled");
    }
  }

  int16_t x;
  int16_t y;
  if (readTouch(x, y)) {
    Serial.print("Touch at (");
    Serial.print(x);
    Serial.print(", ");
    Serial.print(y);
    Serial.println(')');
    delay(100);
  }
  delay(20);
}
