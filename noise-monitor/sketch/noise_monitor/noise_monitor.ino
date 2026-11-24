#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

/*
 * Arduino-native Wi-Fi SoftAP + WebServer for the Noise Monitor dashboard.
 * This replaces the ESP-IDF httpd when building under Arduino,
 * guaranteeing phone/laptop Wi-Fi access to http://192.168.4.1
 */

extern "C" {
  void app_init(void);
  void arduino_serial_write(const char *buf, size_t len);

  /* Pull in the shared C helpers from web_dashboard.c */
  #include "src/web_dashboard.h"
  #include "src/app_config.h"
  #include "src/index_html.h"
}

static WebServer server(80);

/* ---- Shared state (populated by web_dashboard_publish from freertos.c) ---- */
extern "C" SemaphoreHandle_t arduino_web_mutex;
extern "C" noise_measurement_t arduino_latest_meas;
extern "C" bool arduino_has_meas;
extern "C" web_history_buffer_t arduino_history;

SemaphoreHandle_t arduino_web_mutex = NULL;
noise_measurement_t arduino_latest_meas;
bool arduino_has_meas = false;
web_history_buffer_t arduino_history;

void arduino_serial_write(const char *buf, size_t len) {
  if (buf && len > 0) {
    Serial.write((const uint8_t*)buf, len);
  }
}

/* ---- HTTP Handlers ---- */
static void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "text/html; charset=utf-8", INDEX_HTML_STR);
}

static void handleLive() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");

  noise_measurement_t snap;
  bool valid = false;

  if (arduino_web_mutex && xSemaphoreTake(arduino_web_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    snap = arduino_latest_meas;
    valid = arduino_has_meas;
    xSemaphoreGive(arduino_web_mutex);
  }

  if (!valid) {
    memset(&snap, 0, sizeof(snap));
    snap.threshold_trigger = APP_THRESHOLD_TRIGGER_DEFAULT;
    snap.threshold_reset   = APP_THRESHOLD_RESET_DEFAULT;
    snap.is_calibrated     = APP_CALIBRATED_MODE_DEFAULT;
    snap.is_millivolts     = true;
    snap.status            = NOISE_STATUS_NORMAL;
    snap.preset_name       = "DEFAULT";
  }

  char json[512];
  int len = web_format_live_json(json, sizeof(json), &snap, "192.168.4.1", "ESP32-S3 Wi-Fi AP");
  server.send(200, "application/json", String(json));
}

static void handleHistory() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");

  char *json = (char *)malloc(4608);
  if (!json) {
    server.send(500, "application/json", "{\"error\":\"OOM\"}");
    return;
  }

  int len = 0;
  if (arduino_web_mutex && xSemaphoreTake(arduino_web_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    len = web_format_history_json(json, 4608, &arduino_history, arduino_has_meas ? &arduino_latest_meas : NULL);
    xSemaphoreGive(arduino_web_mutex);
  }

  server.send(200, "application/json", String(json));
  free(json);
}

static void handleControlPost() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String body = server.arg("plain");
  if (body.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty\"}");
    return;
  }
  /* Just acknowledge; full control parsing can be added later */
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleControlOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204, "", "");
}

/* ---- Web server task (runs on Core 0) ---- */
static void webServerTask(void *param) {
  (void)param;
  Serial.println("[WEB] HTTP server task started on Core " + String(xPortGetCoreID()));
  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

/* ================================================================ */
void setup() {
  Serial.begin(115200);
  delay(1500);  /* Wait for USB CDC enumeration */

  Serial.println();
  Serial.println("====================================================");
  Serial.println(" Real-Time Noise Pollution Monitoring System");
  Serial.println(" ESP32-S3 + FreeRTOS + Wi-Fi Dashboard");
  Serial.println("====================================================");

  /* Initialize the shared web mutex and history buffer */
  arduino_web_mutex = xSemaphoreCreateMutex();
  web_history_init(&arduino_history);
  arduino_has_meas = false;

  /* Start Wi-Fi Access Point */
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  delay(100);

  bool ap_ok = WiFi.softAP("NoiseMonitor-ESP32S3", "noisemonitor", 1, 0, 4);
  delay(500);  /* Let DHCP settle */

  if (ap_ok) {
    IPAddress ip = WiFi.softAPIP();
    Serial.println();
    Serial.println("========================================");
    Serial.println("  Wi-Fi Access Point STARTED!");
    Serial.println("  SSID     : NoiseMonitor-ESP32S3");
    Serial.println("  Password : noisemonitor");
    Serial.print(  "  Dashboard: http://");
    Serial.println(ip);
    Serial.println("========================================");
    Serial.println();
    Serial.println(">>> On your PHONE or LAPTOP:");
    Serial.println(">>> 1. Open Wi-Fi settings");
    Serial.println(">>> 2. Connect to 'NoiseMonitor-ESP32S3'");
    Serial.println(">>> 3. Open http://192.168.4.1 in browser");
    Serial.println();
  } else {
    Serial.println("[ERROR] Failed to start Wi-Fi SoftAP!");
  }

  /* Register HTTP routes */
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/live", HTTP_GET, handleLive);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/control", HTTP_POST, handleControlPost);
  server.on("/api/control", HTTP_OPTIONS, handleControlOptions);
  server.begin();
  Serial.println("[WEB] HTTP server listening on port 80");

  /* Start web server handler on Core 0 as a background task */
  xTaskCreatePinnedToCore(webServerTask, "web_srv", 4096, NULL, 2, NULL, 0);

  /* Initialize the noise monitor (ADC, processing, display, alert, UART tasks) */
  Serial.println("[INIT] Starting noise monitor core...");
  app_init();

  Serial.println("[INIT] System fully running!");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
