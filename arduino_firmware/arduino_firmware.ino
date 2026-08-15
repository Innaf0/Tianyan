#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <ArduinoJson.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/Org_01.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <string.h>

// ILI9341 and XPT2046 share the hardware SPI1 bus at device-specific speeds.
constexpr uint8_t TFT_DC = 5;
constexpr uint8_t TFT_CS = 13;
constexpr uint8_t TFT_MOSI = 11;
constexpr uint8_t TFT_CLK = 14;
constexpr uint8_t TFT_RST = 15;
constexpr uint8_t TFT_MISO = 12;
constexpr uint8_t TOUCH_CS = 9;
constexpr uint8_t TOUCH_IRQ = 8;
constexpr uint8_t BUTTON_NEWER = 20;  // Upper side button, SW2.
constexpr uint8_t BUTTON_OLDER = 22;  // Lower side button, SW3.
constexpr uint8_t BUTTON_PAGE = 16;   // Front button, SW1.

// SD card on SPI0.
constexpr uint8_t SD_MISO = 4;
constexpr uint8_t SD_CLK = 6;
constexpr uint8_t SD_MOSI = 7;
constexpr uint8_t SD_CS = 21;

constexpr uint32_t MATCH_RETRY_MS = 30000;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 40;
constexpr uint32_t TFT_SPI_HZ = 24000000;
constexpr uint32_t TOUCH_SPI_HZ = 2000000;
constexpr uint8_t MATCH_HISTORY_SIZE = 10;
constexpr uint8_t DISPLAY_ROTATION = 1;  // Portrait in the device's mounted orientation.
constexpr uint8_t DISPLAY_MADCTL_RGB = 0x20;  // Rotation 1 without BGR color order.
constexpr int16_t LAYOUT_X = 3;
constexpr int16_t LAYOUT_RIGHT_PAD = 3;
constexpr uint32_t ARTWORK_STREAM_TIMEOUT_MS = 15000;
constexpr int32_t MAX_ARTWORK_BYTES = 192 * 1024;

Adafruit_ILI9341 tft(&SPI1, TFT_DC, TFT_CS, TFT_RST);
PNG png;

struct Config {
  String wifiSsid = "haz1";
  String wifiPassword = "84915801";
  String apiKey = "HDEV-4456951f-37f3-4f81-9f30-86d690763655";
  String playerRegion = "eu";
  String playerName = "Innaf";
  String playerTag = "Fanni";
};

Config config;
JsonDocument matchDocument;
uint32_t lastFetchAttempt = 0;
bool firstFetch = true;
bool matchLoaded = false;
bool sdReady = false;
size_t currentMatchIndex = 0;
size_t matchCount = 0;

enum class DisplayPage : uint8_t {
  PlayerCard,
  Agent,
  MatchStats,
};

DisplayPage currentPage = DisplayPage::MatchStats;

uint16_t *pngLineBuffer = nullptr;
uint16_t *pngScaledLineBuffer = nullptr;
int16_t pngImageWidth = 0;
int16_t pngImageHeight = 0;
int16_t pngDestinationX = 0;
int16_t pngDestinationY = 0;
int16_t pngDrawWidth = 0;
int16_t pngDrawHeight = 0;
int16_t pngLastOutputY = -1;

struct ButtonState {
  uint8_t pin;
  bool rawState = HIGH;
  bool stableState = HIGH;
  uint32_t changedAt = 0;
};

ButtonState newerButton{BUTTON_NEWER};
ButtonState olderButton{BUTTON_OLDER};
ButtonState pageButton{BUTTON_PAGE};

void drawStatus(const char *title, const String &detail = "") {
  tft.fillScreen(ILI9341_BLACK);
  tft.setFont(&FreeSansBold12pt7b);
  tft.setCursor(8, 30);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  tft.println(title);
  if (detail.length() > 0) {
    tft.setFont(&FreeSans9pt7b);
    tft.setCursor(8, 58);
    tft.setTextColor(ILI9341_DARKGREY);
    tft.println(detail);
  }
  tft.setFont(nullptr);
  tft.setTextSize(1);
}

bool initSdCard() {
  if (sdReady) {
    return true;
  }

  SPI.setRX(SD_MISO);
  SPI.setSCK(SD_CLK);
  SPI.setTX(SD_MOSI);
  SPI.begin();
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  for (uint8_t attempt = 1; attempt <= 3; ++attempt) {
    if (SD.begin(SD_CS, SD_SCK_MHZ(1), SPI)) {
      sdReady = true;
      Serial.println("SD card mounted");
      return true;
    }
    SD.end(false);
    delay(100);
  }

  Serial.println("SD card not found; using built-in configuration");
  return false;
}

bool loadConfig() {
  if (!initSdCard()) {
    Serial.println("Using built-in configuration");
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

void printLimited(const char *text, uint8_t maxChars) {
  if (text == nullptr || maxChars == 0) {
    return;
  }

  uint8_t length = 0;
  while (text[length] != '\0' && length < 255) {
    length++;
  }

  const bool clipped = length > maxChars;
  const uint8_t limit = clipped && maxChars > 2 ? maxChars - 2 : maxChars;
  for (uint8_t i = 0; i < limit && text[i] != '\0'; ++i) {
    tft.print(text[i]);
  }
  if (clipped && maxChars > 2) {
    tft.print("..");
  }
}

JsonObject findConfiguredPlayer(JsonObject match) {
  for (JsonObject player : match["players"]["all_players"].as<JsonArray>()) {
    const char *name = player["name"] | "";
    const char *tag = player["tag"] | "";
    if (config.playerName.equalsIgnoreCase(name) &&
        config.playerTag.equalsIgnoreCase(tag)) {
      return player;
    }
  }
  return JsonObject();
}

int drawPngLine(PNGDRAW *line) {
  if (pngLineBuffer == nullptr || pngScaledLineBuffer == nullptr) {
    return 1;
  }

  const int16_t outputY = static_cast<int32_t>(line->y) * pngDrawHeight /
                          pngImageHeight;
  if (outputY == pngLastOutputY || outputY >= pngDrawHeight) {
    return 1;
  }
  pngLastOutputY = outputY;

  png.getLineAsRGB565(line, pngLineBuffer, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);
  for (int16_t x = 0; x < pngDrawWidth; ++x) {
    const int16_t sourceX = static_cast<int32_t>(x) * pngImageWidth /
                            pngDrawWidth;
    pngScaledLineBuffer[x] = pngLineBuffer[sourceX];
  }

  tft.setAddrWindow(pngDestinationX, pngDestinationY + outputY,
                    pngDrawWidth, 1);
  tft.writePixels(pngScaledLineBuffer, pngDrawWidth, true);
  return 1;
}

bool drawPngFromUrl(const char *url, int16_t imageTop) {
  if (url == nullptr || url[0] == '\0') {
    return false;
  }
  if (WiFi.status() != WL_CONNECTED && !connectWifi()) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(ARTWORK_STREAM_TIMEOUT_MS);
  HTTPClient http;
  http.setTimeout(ARTWORK_STREAM_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    return false;
  }

  const int statusCode = http.GET();
  const int32_t artworkSize = http.getSize();
  if (statusCode != HTTP_CODE_OK || artworkSize <= 0 ||
      artworkSize > MAX_ARTWORK_BYTES) {
    Serial.print("Artwork download failed, HTTP/size: ");
    Serial.print(statusCode);
    Serial.print('/');
    Serial.println(artworkSize);
    http.end();
    return false;
  }

  uint8_t *artworkData = new (std::nothrow) uint8_t[artworkSize];
  if (artworkData == nullptr) {
    Serial.println("Artwork download failed: out of memory");
    http.end();
    return false;
  }

  WiFiClient &stream = http.getStream();
  int32_t received = 0;
  uint32_t lastProgress = millis();
  while (received < artworkSize &&
         millis() - lastProgress < ARTWORK_STREAM_TIMEOUT_MS) {
    const int n = stream.read(artworkData + received, artworkSize - received);
    if (n > 0) {
      received += n;
      lastProgress = millis();
    } else if (!stream.connected()) {
      break;
    } else {
      delay(1);
    }
  }
  http.end();
  if (received != artworkSize) {
    Serial.print("Artwork download incomplete: ");
    Serial.print(received);
    Serial.print('/');
    Serial.println(artworkSize);
    delete[] artworkData;
    return false;
  }

  const int result = png.openRAM(artworkData, artworkSize, drawPngLine);
  if (result != PNG_SUCCESS) {
    Serial.print("PNG open failed: ");
    Serial.println(result);
    delete[] artworkData;
    return false;
  }

  pngImageWidth = png.getWidth();
  pngImageHeight = png.getHeight();
  const int16_t availableHeight = tft.height() - imageTop - 4;
  if (pngImageWidth <= tft.width() && pngImageHeight <= availableHeight) {
    pngDrawWidth = pngImageWidth;
    pngDrawHeight = pngImageHeight;
  } else if (static_cast<int32_t>(pngImageWidth) * availableHeight >
             static_cast<int32_t>(pngImageHeight) * tft.width()) {
    pngDrawWidth = tft.width();
    pngDrawHeight = static_cast<int32_t>(pngImageHeight) * tft.width() /
                    pngImageWidth;
  } else {
    pngDrawHeight = availableHeight;
    pngDrawWidth = static_cast<int32_t>(pngImageWidth) * availableHeight /
                   pngImageHeight;
  }
  pngDestinationX = tft.width() > pngDrawWidth ? (tft.width() - pngDrawWidth) / 2 : 0;
  pngDestinationY = imageTop +
                    (availableHeight > pngDrawHeight
                         ? (availableHeight - pngDrawHeight) / 2
                         : 0);

  pngLineBuffer = new (std::nothrow) uint16_t[pngImageWidth];
  pngScaledLineBuffer = new (std::nothrow) uint16_t[pngDrawWidth];
  if (pngLineBuffer == nullptr || pngScaledLineBuffer == nullptr) {
    delete[] pngLineBuffer;
    delete[] pngScaledLineBuffer;
    pngLineBuffer = nullptr;
    pngScaledLineBuffer = nullptr;
    png.close();
    delete[] artworkData;
    return false;
  }

  pngLastOutputY = -1;
  tft.startWrite();
  const int decodeResult = png.decode(nullptr, 0);
  tft.endWrite();
  delete[] pngLineBuffer;
  delete[] pngScaledLineBuffer;
  pngLineBuffer = nullptr;
  pngScaledLineBuffer = nullptr;
  png.close();
  delete[] artworkData;
  if (decodeResult != PNG_SUCCESS) {
    Serial.print("PNG decode failed: ");
    Serial.println(decodeResult);
    return false;
  }
  Serial.print("PNG decoded: ");
  Serial.print(pngDrawWidth);
  Serial.print('x');
  Serial.print(pngDrawHeight);
  Serial.print(" from ");
  Serial.print(pngImageWidth);
  Serial.print('x');
  Serial.print(pngImageHeight);
  Serial.print(" bytes=");
  Serial.println(artworkSize);
  return true;
}

void drawArtworkPage(JsonObject match, bool playerCard) {
  JsonObject player = findConfiguredPlayer(match);
  if (player.isNull()) {
    drawStatus("Player not found", config.playerName + " #" + config.playerTag);
    return;
  }

  const char *title = playerCard ? "PLAYER CARD" : "AGENT";
  const char *sourceUrl = playerCard
                              ? player["assets"]["card"]["large"] | ""
                              : player["assets"]["agent"]["small"] | "";
  const int16_t imageSize = min(tft.width(), tft.height() - 42 - 4);
  String resizedUrl = "https://wsrv.nl/?url=" + urlEncode(sourceUrl) +
                      "&h=" + String(imageSize);
  if (!playerCard) {
    resizedUrl += "&w=" + String(imageSize) + "&fit=contain";
  }
  resizedUrl += "&bg=black&output=png";
  drawStatus(title, "Loading image...");
  if (!drawPngFromUrl(resizedUrl.c_str(), 42)) {
    drawStatus(title, "Image unavailable");
    return;
  }

  tft.fillRect(0, 0, tft.width(), 42, ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(LAYOUT_X, 6);
  tft.print(title);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setTextSize(1);
  tft.setCursor(LAYOUT_X, 27);
  if (playerCard) {
    printLimited(player["name"].as<const char *>(), 24);
    tft.print(" #");
    printLimited(player["tag"].as<const char *>(), 12);
  } else {
    printLimited(player["character"].as<const char *>(), 30);
  }
}

void drawMatch(JsonObject match) {
  JsonObject metadata = match["metadata"];
  JsonObject teams = match["teams"];
  JsonArray players = match["players"]["all_players"];

  const uint16_t red = tft.color565(214, 45, 61);
  const uint16_t blue = tft.color565(48, 113, 247);
  const uint16_t muted = tft.color565(145, 145, 145);
  const uint16_t divider = tft.color565(70, 70, 70);
  const int16_t matchWidth = min(tft.width(), tft.height());
  const int16_t contentWidth = matchWidth - LAYOUT_X - LAYOUT_RIGHT_PAD;
  const int16_t playerNameX = 12;
  const int16_t playerAgentX = 82;
  const int16_t playerRankX = 140;
  const int16_t playerStatsRight = matchWidth - 1;
  const int16_t playerRowHeight = 14;

  tft.fillScreen(ILI9341_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);

  int16_t y = 10;
  tft.setTextSize(1);
  tft.setCursor(LAYOUT_X, y);
  printLimited(metadata["map"].as<const char *>(), 22);
  tft.setTextColor(muted);
  tft.print("  ");
  printLimited(metadata["mode"].as<const char *>(), 20);
  y += 13;

  const uint16_t redRounds = teams["red"]["rounds_won"] | 0;
  const uint16_t blueRounds = teams["blue"]["rounds_won"] | 0;
  tft.setTextSize(2);
  tft.setCursor(LAYOUT_X, y);
  tft.setTextColor(red);
  tft.print("RED ");
  tft.setTextColor(ILI9341_WHITE);
  tft.print(redRounds);
  tft.print(" - ");
  tft.print(blueRounds);
  tft.setTextColor(blue);
  tft.print(" BLUE");
  y += 18;

  tft.setTextSize(1);
  tft.setCursor(LAYOUT_X, y);
  tft.setTextColor(muted);
  tft.print("Winner: ");
  const bool redWon = teams["red"]["has_won"].as<bool>();
  tft.setTextColor(redWon ? red : blue);
  tft.print(redWon ? "Red" : "Blue");
  y += 10;
  tft.drawFastHLine(LAYOUT_X, y, contentWidth, divider);
  y += 5;

  tft.setFont(&Org_01);
  tft.setTextSize(1);
  tft.setTextColor(muted);
  tft.setCursor(playerNameX, y + 4);
  tft.print("PLAYER");
  tft.setCursor(playerAgentX, y + 4);
  tft.print("AGENT");
  tft.setCursor(playerRankX, y + 4);
  tft.print("RANK");
  int16_t textX;
  int16_t textY;
  uint16_t textWidth;
  uint16_t textHeight;
  tft.getTextBounds("K/D", 0, 0, &textX, &textY, &textWidth, &textHeight);
  tft.setCursor(playerStatsRight - textWidth - textX, y + 4);
  tft.print("K/D");
  y += 9;

  for (JsonObject player : players) {
    if (y > tft.height() - playerRowHeight) {
      break;
    }

    const char *team = player["team"].as<const char *>();
    const bool isRed = team != nullptr && strcmp(team, "Red") == 0;
    const bool isBlue = team != nullptr && strcmp(team, "Blue") == 0;
    const uint16_t teamColor = isRed ? red : (isBlue ? blue : muted);
    tft.drawFastVLine(LAYOUT_X, y, 9, teamColor);
    tft.setTextSize(1);
    tft.setCursor(playerNameX, y + 4);
    tft.setTextColor(ILI9341_WHITE);
    printLimited(player["name"].as<const char *>(), 11);
    tft.setCursor(playerAgentX, y + 4);
    tft.setTextColor(muted);
    printLimited(player["character"].as<const char *>(), 9);
    tft.setCursor(playerRankX, y + 4);
    printLimited(player["currenttier_patched"].as<const char *>(), 11);
    String stats = String(player["stats"]["kills"].as<unsigned int>()) + "/" +
                   String(player["stats"]["deaths"].as<unsigned int>());
    tft.getTextBounds(stats, 0, 0, &textX, &textY, &textWidth, &textHeight);
    tft.setCursor(playerStatsRight - textWidth - textX, y + 4);
    tft.setTextColor(teamColor);
    tft.print(stats);
    y += playerRowHeight;
  }
  tft.setFont(nullptr);
}

void drawCurrentMatch() {
  if (currentMatchIndex >= matchCount) {
    return;
  }

  JsonObject match = matchDocument["data"][currentMatchIndex];
  if (currentPage == DisplayPage::PlayerCard) {
    drawArtworkPage(match, true);
  } else if (currentPage == DisplayPage::Agent) {
    drawArtworkPage(match, false);
  } else {
    logMatch(match);
    drawMatch(match);
  }
  Serial.print("Showing match ");
  Serial.print(currentMatchIndex + 1);
  Serial.print(" of ");
  Serial.println(matchCount);
}

bool wasPressed(ButtonState &button) {
  const bool rawState = digitalRead(button.pin);
  const uint32_t now = millis();

  if (rawState != button.rawState) {
    button.rawState = rawState;
    button.changedAt = now;
  }

  if (rawState != button.stableState &&
      now - button.changedAt >= BUTTON_DEBOUNCE_MS) {
    button.stableState = rawState;
    return button.stableState == LOW;
  }
  return false;
}

void navigateMatches(int8_t direction) {
  if (!matchLoaded || matchCount == 0) {
    return;
  }

  const int32_t nextIndex = static_cast<int32_t>(currentMatchIndex) + direction;
  if (nextIndex < 0 || nextIndex >= static_cast<int32_t>(matchCount)) {
    return;
  }

  currentMatchIndex = static_cast<size_t>(nextIndex);
  drawCurrentMatch();
}

void cyclePage() {
  if (!matchLoaded || matchCount == 0) {
    return;
  }

  const uint8_t nextPage = (static_cast<uint8_t>(currentPage) + 1) % 3;
  currentPage = static_cast<DisplayPage>(nextPage);
  drawCurrentMatch();
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
  url += urlEncode(config.playerTag) + "?size=" + String(MATCH_HISTORY_SIZE);

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
  filter["data"][0]["players"]["all_players"][0]["assets"]["card"]["large"] = true;
  filter["data"][0]["players"]["all_players"][0]["assets"]["agent"]["small"] = true;
  filter["data"][0]["players"]["all_players"][0]["stats"] = true;

  matchDocument.clear();
  DeserializationError error = deserializeJson(
      matchDocument, http.getStream(), DeserializationOption::Filter(filter),
      DeserializationOption::NestingLimit(32));
  http.end();
  if (error) {
    Serial.print("API JSON parse failed: ");
    Serial.println(error.c_str());
    drawStatus("Invalid response", error.c_str());
    return false;
  }

  if ((matchDocument["status"] | 0) != 200 || matchDocument["data"].size() == 0) {
    Serial.println("API returned no match data");
    drawStatus("No match found");
    return false;
  }

  matchCount = matchDocument["data"].size();
  currentMatchIndex = 0;
  drawCurrentMatch();
  Serial.println("Match history loaded");
  return true;
}

uint16_t touchTransfer(uint8_t command) {
  SPI1.transfer(command);
  return SPI1.transfer16(0) >> 3;
}

bool readTouch(int16_t &x, int16_t &y) {
  if (digitalRead(TOUCH_IRQ) != LOW) {
    return false;
  }

  digitalWrite(TFT_CS, HIGH);
  SPI1.beginTransaction(SPISettings(TOUCH_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  const int32_t rawX = touchTransfer(0x90);
  const int32_t rawY = touchTransfer(0xd0);
  digitalWrite(TOUCH_CS, HIGH);
  SPI1.endTransaction();

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
  pinMode(BUTTON_NEWER, INPUT_PULLUP);
  pinMode(BUTTON_OLDER, INPUT_PULLUP);
  pinMode(BUTTON_PAGE, INPUT_PULLUP);

  SPI1.setRX(TFT_MISO);
  SPI1.setSCK(TFT_CLK);
  SPI1.setTX(TFT_MOSI);
  tft.begin(TFT_SPI_HZ);
  // Clear through every address orientation to remove pixels left by old firmware.
  for (uint8_t rotation = 0; rotation < 4; ++rotation) {
    tft.setRotation(rotation);
    tft.fillScreen(ILI9341_BLACK);
  }
  tft.setRotation(DISPLAY_ROTATION);
  tft.sendCommand(ILI9341_MADCTL, &DISPLAY_MADCTL_RGB, 1);
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

  if (wasPressed(newerButton)) {
    navigateMatches(-1);
  }
  if (wasPressed(olderButton)) {
    navigateMatches(1);
  }
  if (wasPressed(pageButton)) {
    cyclePage();
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
