/**
 * http_simple_ota.ino
 *
 * Minimal boot-time OTA client for ESP32.
 *
 * - Connects to Wi-Fi using compiled credentials.
 * - Calls the OTA firmware endpoint immediately after connecting.
 * - Downloads and installs the binary if the server returns HTTP 200.
 * - No web interface, registration flow, or OTA password handling.
 *
 * The OTA server is expected to:
 *   * Expose firmware at `${OTA_SERVER}/api/users/${OTA_USER}/repos/${OTA_REPO}/firmware`
 *   * Return HTTP 200 with the raw firmware binary when an update is available
 *   * Return a different status (e.g. 204/304/404) when no update should be performed
 *
 * TLS certificate validation is disabled (`setInsecure`). Only use trusted endpoints.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>

////////////////////////////////////////
const char* version   = "0.1.7";        //
const char* type      = "esp32s3-zero"; //
const char* device_id = "1234567890";   //
////////////////////////////////////////

static const char* WIFI_SSID     = "@@@WIFI_SSID@@@";
static const char* WIFI_PASSWORD = "@@@WIFI_PASSWORD@@@";
static const char* OTA_USER      = "@@@OTA_USER@@@";
static const char* OTA_REPO      = "@@@OTA_REPO@@@";
static const char* OTA_SERVER    = "@@@OTA_SERVER@@@";

static String repoApiBaseUrl() {
  if (!OTA_SERVER || !OTA_USER || !OTA_REPO) return String();
  if (!strlen(OTA_SERVER) || !strlen(OTA_USER) || !strlen(OTA_REPO)) return String();
  String url = String(OTA_SERVER);
  if (!url.endsWith("/")) url += "/";
  url += "api/users/";
  url += OTA_USER;
  url += "/repos/";
  url += OTA_REPO;
  return url;
}

static String firmwareEndpointUrl() {
  String url = repoApiBaseUrl();
  if (!url.length()) return url;
  url += "/firmware";
  return url;
}

static bool connectToWifi(uint32_t timeoutMs = 20000) {
  Serial.printf("[WiFi] Connecting to SSID '%s'\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(200);
  WiFi.begin(WIFI_SSID, strlen(WIFI_PASSWORD) ? WIFI_PASSWORD : nullptr);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected, IP address: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("[WiFi] Failed to connect.");
  return false;
}

static bool performHttpUpdate(const String& url) {
  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  WiFiClient* client = nullptr;

  if (!url.startsWith("http://") && !url.startsWith("https://")) {
    Serial.println("[OTA] Unsupported URL scheme.");
    return false;
  }

  if (url.startsWith("https://")) {
    secureClient.setInsecure();  // rely on trusted server/on-prem network
    client = &secureClient;
  } else {
    client = &plainClient;
  }

  http.setConnectTimeout(10000);

  Serial.printf("[OTA] Checking %s\n", url.c_str());
  if (!http.begin(*client, url)) {
    Serial.println("[OTA] HTTP begin failed.");
    return false;
  }

  http.addHeader("X-Device-Id", String(device_id));
  http.addHeader("X-Current-Version", String(version));
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[OTA] No update (HTTP %d)\n", httpCode);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  WiFiClient* stream = http.getStreamPtr();

  if (contentLength > 0) {
    Serial.printf("[OTA] Firmware size: %d bytes\n", contentLength);
  } else {
    Serial.println("[OTA] Firmware size unknown; streaming until connection closes.");
  }

  if (!Update.begin(contentLength > 0 ? (size_t)contentLength : UPDATE_SIZE_UNKNOWN)) {
    Serial.println("[OTA] Update.begin() failed.");
    http.end();
    return false;
  }

  uint8_t buffer[2048];
  size_t totalWritten = 0;

  while (true) {
    size_t available = stream->available();
    if (!available) {
      if (!stream->connected()) break;
      delay(10);
      continue;
    }
    if (available > sizeof(buffer)) available = sizeof(buffer);
    int bytesRead = stream->readBytes(buffer, available);
    if (bytesRead <= 0) break;
    if (Update.write(buffer, bytesRead) != (size_t)bytesRead) {
      Serial.println("[OTA] Write error, aborting.");
      Update.abort();
      http.end();
      return false;
    }
    totalWritten += bytesRead;
  }

  if (!Update.end()) {
    Serial.println("[OTA] Update.end() failed.");
    http.end();
    return false;
  }

  if (!Update.isFinished()) {
    Serial.println("[OTA] Update not finished (interrupted?).");
    http.end();
    return false;
  }

  Serial.printf("[OTA] Update successful (%u bytes). Rebooting...\n", (unsigned)totalWritten);
  http.end();
  delay(500);
  ESP.restart();
  return true;  // unreachable post-restart
}

static void checkForUpdate() {
  String url = firmwareEndpointUrl();
  if (!url.length()) {
    Serial.println("[OTA] OTA endpoint not configured.");
    return;
  }

  url += (url.indexOf('?') == -1) ? '?' : '&';
  url += "type=";
  url += type;
  url += "&version=";
  url += version;
  url += "&device_id=";
  url += device_id;

  performHttpUpdate(url);
}

void setup() {
  Serial.begin(115200);
  // Wait for USB CDC to be ready (important when using Hardware CDC)
  // ESP32-S3 USB CDC needs time to initialize, especially with build scripts
  unsigned long startWait = millis();
  while (!Serial && (millis() - startWait) < 3000) {
    delay(10);
  }
  delay(500);  // Additional delay to ensure USB CDC is fully initialized
  Serial.println();
  Serial.println("http_simple_ota starting...");
  Serial.printf("Current firmware version: %s\n", version);
  Serial.flush();  // Ensure output is sent

  if (!connectToWifi()) {
    Serial.println("[System] Wi-Fi connection failed; will retry in loop.");
    return;
  }

  checkForUpdate();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (connectToWifi()) {
      checkForUpdate();
    }
  }

  static unsigned long lastCheck = 0;
  const unsigned long intervalMs = 6UL * 60UL * 60UL * 1000UL; // every 6 hours
  unsigned long now = millis();
  if ((lastCheck == 0 || now - lastCheck >= intervalMs) && WiFi.status() == WL_CONNECTED) {
    checkForUpdate();
    lastCheck = now;
  }

  delay(1000);
}
