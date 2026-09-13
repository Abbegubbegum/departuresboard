#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <base64.h>
#include <ArduinoJson.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// HUB75 matrix setup is on hold while we get the Vasttrafik API working -
// see git history for the pin mapping / MatrixPanel_I2S_DMA init.

static const char *VASTTRAFIK_TOKEN_URL = "https://ext-api.vasttrafik.se/token";
static const char *VASTTRAFIK_API_BASE = "https://ext-api.vasttrafik.se/pr/v4";

String accessToken;
unsigned long tokenExpiresAtMs = 0;

unsigned long lastFetchMs = 0;
constexpr unsigned long FETCH_INTERVAL_MS = 30000;

#define PANEL_RES_X 64
#define PANEL_RES_Y 32
#define PANEL_CHAIN 1

constexpr unsigned int MAX_CHARS_PER_ROW = PANEL_RES_X / 6;

// Pin mapping chosen to avoid ESP32-S3 flash/PSRAM/strapping/USB pins.
// Wire the HUB75 connector to match these GPIOs (or tell me your board's
// fixed adapter pinout if you're using a breakout).
#define R1_PIN 4
#define G1_PIN 6
#define B1_PIN 5
#define R2_PIN 7
#define G2_PIN 16
#define B2_PIN 15
#define A_PIN 18
#define B_PIN 8
#define C_PIN 9
#define D_PIN 10
#define E_PIN 17 // needed if panel is 1/32 scan
#define LAT_PIN 12
#define OE_PIN 13
#define CLK_PIN 11

MatrixPanel_I2S_DMA *display = nullptr;

struct Departure
{
  String line;
  String destination;
  time_t departure_time;
};

Departure departure_array[4];

constexpr unsigned long RENDER_INTERVAL_MS = 1000;
unsigned long lastRenderMs = 0;

void connectWiFi()
{
  Serial.printf("Connecting to WiFi \"%s\"...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
}

bool fetchAccessToken()
{
  WiFiClientSecure client;
  client.setInsecure(); // dev only: skips TLS cert validation

  HTTPClient https;
  if (!https.begin(client, VASTTRAFIK_TOKEN_URL))
  {
    Serial.println("Failed to begin token request");
    return false;
  }

  String credentials = base64::encode(String(VASTTRAFIK_CLIENT_ID) + ":" + String(VASTTRAFIK_CLIENT_SECRET));
  https.addHeader("Authorization", "Basic " + credentials);
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int status = https.POST("grant_type=client_credentials");
  if (status != HTTP_CODE_OK)
  {
    Serial.printf("Token request failed, HTTP %d: %s\n", status, https.getString().c_str());
    https.end();
    return false;
  }

  String body = https.getString();
  https.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err)
  {
    Serial.printf("Failed to parse token response: %s\n", err.c_str());
    return false;
  }

  accessToken = doc["access_token"].as<String>();
  long expiresIn = doc["expires_in"] | 3600;
  tokenExpiresAtMs = millis() + (expiresIn * 1000UL) - 30000UL; // refresh 30s early

  Serial.println("Got Vasttrafik access token");
  return true;
}

bool ensureAccessToken()
{
  if (accessToken.length() > 0 && millis() < tokenExpiresAtMs)
  {
    return true;
  }
  return fetchAccessToken();
}

String urlEncode(const String &value)
{
  String encoded;
  for (size_t i = 0; i < value.length(); i++)
  {
    char c = value.charAt(i);
    if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~')
    {
      encoded += c;
    }
    else
    {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
      encoded += buf;
    }
  }
  return encoded;
}

// One-time helper: prints stop-area gids matching VASTTRAFIK_STOP_NAME so you
// can copy the right one into VASTTRAFIK_STOP_ID in .env.
void lookupStopId()
{
  if (!ensureAccessToken())
  {
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = String(VASTTRAFIK_API_BASE) + "/locations/by-text?q=" + urlEncode(VASTTRAFIK_STOP_NAME) + "&types=stoparea&limit=10";
  if (!https.begin(client, url))
  {
    Serial.println("Failed to begin stop lookup request");
    return;
  }
  https.addHeader("Authorization", "Bearer " + accessToken);

  int status = https.GET();
  if (status != HTTP_CODE_OK)
  {
    Serial.printf("Stop lookup failed, HTTP %d: %s\n", status, https.getString().c_str());
    https.end();
    return;
  }

  String body = https.getString();
  https.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err)
  {
    Serial.printf("Failed to parse stop lookup response: %s\n", err.c_str());
    return;
  }

  JsonArray results = doc["results"].as<JsonArray>();
  Serial.printf("---- %u matches for \"%s\" ----\n", results.size(), VASTTRAFIK_STOP_NAME);
  for (JsonObject result : results)
  {
    const char *gid = result["gid"] | "?";
    const char *name = result["name"] | "?";
    Serial.printf("gid=%s  name=%s\n", gid, name);
  }
  Serial.println("Copy the gid you want into VASTTRAFIK_STOP_ID in .env, then re-upload.");
}

time_t parseISOString(const char *time_string)
{
  tm time;

  int year, mon;

  int result = sscanf(time_string, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &mon, &time.tm_mday, &time.tm_hour, &time.tm_min, &time.tm_sec);

  if (result != 6)
  {
    Serial.println("Date String Parse FAILURE");
  }

  time.tm_year = year - 1900;
  time.tm_mon = mon - 1;

  return mktime(&time);
}

void fetchDepartures()
{
  if (!ensureAccessToken())
  {
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = String(VASTTRAFIK_API_BASE) + "/stop-areas/" + VASTTRAFIK_STOP_ID + "/departures?timeSpanInMinutes=60&maxDeparturesPerLine=3";
  if (!https.begin(client, url))
  {
    Serial.println("Failed to begin departures request");
    return;
  }
  https.addHeader("Authorization", "Bearer " + accessToken);

  int status = https.GET();
  if (status != HTTP_CODE_OK)
  {
    Serial.printf("Departures request failed, HTTP %d: %s\n", status, https.getString().c_str());
    https.end();
    return;
  }

  String body = https.getString();
  https.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err)
  {
    Serial.printf("Failed to parse departures response: %s\n", err.c_str());
    return;
  }

  JsonArray results = doc["results"].as<JsonArray>();
  Serial.printf("---- %u departures ----\n", results.size());

  size_t i = 0;
  for (JsonObject dep : results)
  {
    JsonObject serviceJourney = dep["serviceJourney"];
    const char *line = serviceJourney["line"]["name"] | "?";
    const char *direction = serviceJourney["direction"] | "?";
    const char *plannedTime = dep["plannedTime"] | "?";
    const char *estimatedTime = dep["estimatedTime"] | plannedTime;
    bool cancelled = dep["isCancelled"] | false;
    const char *shortDirection = serviceJourney["directionDetails"]["shortDirection"] | "?";

    Serial.printf("%d. Line %-4s -> %s planned %s estimated %s%s\n", i,
                  line, shortDirection, plannedTime, estimatedTime,
                  cancelled ? " [CANCELLED]" : "");

    if (i < 4)
    {
      Departure departure;

      departure.line = String(serviceJourney["line"]["shortName"] | "?");
      departure.destination = String(serviceJourney["directionDetails"]["shortDirection"] | "?");

      const char *departure_time = dep["estimatedTime"] | dep["plannedTime"] | "?";

      departure.departure_time = parseISOString(departure_time);

      departure_array[i] = departure;
    }

    i++;
  }
}

time_t getCurrentTimeEpoch()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("  Failed to obtain time");
    return 0;
  }

  return mktime(&timeinfo);
}

void renderDisplay()
{
  display->clearScreen();

  display->setCursor(0, 0);
  for (int i = 0; i < 4; i++)
  {
    Departure departure = departure_array[i];

    String print_line = String(departure.line + " " + departure.destination);

    display->println(print_line.substring(0, MAX_CHARS_PER_ROW).c_str());
  }

  display->flipDMABuffer();
}

void setup()
{
  Serial.begin(115200);

  connectWiFi();
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_11dBm); // lower peak TX current draw; try removing if flicker persists and check the panel's power supply instead

  if (String(VASTTRAFIK_STOP_ID).length() == 0)
  {
    lookupStopId();
  }

  HUB75_I2S_CFG::i2s_pins pins = {
      R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN,
      A_PIN, B_PIN, C_PIN, D_PIN, E_PIN,
      LAT_PIN, OE_PIN, CLK_PIN};

  HUB75_I2S_CFG mxconfig(
      PANEL_RES_X,
      PANEL_RES_Y,
      PANEL_CHAIN, pins);

  mxconfig.double_buff = true;
  mxconfig.clkphase = false;

  display = new MatrixPanel_I2S_DMA(mxconfig);
  display->begin();
  display->setBrightness8(20);
  display->clearScreen();
  display->setTextWrap(false);

  configTime(0, 0, "pool.ntp.org");
  // Timezone for Stockholm, Sweden
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("  Failed to obtain time");
    return;
  }
  Serial.printf("Got time %i:%i:%i\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    connectWiFi();
  }

  if (String(VASTTRAFIK_STOP_ID).length() == 0)
  {
    delay(5000);
    return;
  }

  if (lastFetchMs == 0 || millis() - lastFetchMs >= FETCH_INTERVAL_MS)
  {
    lastFetchMs = millis();
    fetchDepartures();
  }

  if (lastRenderMs == 0 || millis() - lastRenderMs >= RENDER_INTERVAL_MS)
  {
    lastRenderMs = millis();
    renderDisplay();
  }
}
