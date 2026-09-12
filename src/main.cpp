#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <base64.h>
#include <ArduinoJson.h>

// HUB75 matrix setup is on hold while we get the Vasttrafik API working -
// see git history for the pin mapping / MatrixPanel_I2S_DMA init.

static const char *VASTTRAFIK_TOKEN_URL = "https://ext-api.vasttrafik.se/token";
static const char *VASTTRAFIK_API_BASE = "https://ext-api.vasttrafik.se/pr/v4";

String accessToken;
unsigned long tokenExpiresAtMs = 0;

unsigned long lastFetchMs = 0;
const unsigned long FETCH_INTERVAL_MS = 30000;

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
  for (JsonObject dep : results)
  {
    JsonObject serviceJourney = dep["serviceJourney"];
    const char *line = serviceJourney["line"]["name"] | "?";
    const char *direction = serviceJourney["direction"] | "?";
    const char *plannedTime = dep["plannedTime"] | "?";
    const char *estimatedTime = dep["estimatedTime"] | plannedTime;
    bool cancelled = dep["isCancelled"] | false;

    Serial.printf("Line %-4s -> %-25s planned %s estimated %s%s\n",
                  line, direction, plannedTime, estimatedTime,
                  cancelled ? " [CANCELLED]" : "");
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  connectWiFi();

  if (String(VASTTRAFIK_STOP_ID).length() == 0)
  {
    lookupStopId();
  }
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
}
