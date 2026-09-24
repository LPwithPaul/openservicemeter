#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>
#include "config.h"

// --- Pin assignment ---
const uint8_t BTN_GREEN  = 32;
const uint8_t BTN_YELLOW = 33;
const uint8_t BTN_RED    = 25;
const uint8_t LED_GREEN  = 26;
const uint8_t LED_YELLOW = 27;
const uint8_t LED_RED    = 14;
const uint8_t BUZZER     = 4;

const unsigned long DEBOUNCE_MS = 250;
const unsigned long PAUSE_MS    = 150;

// Two ascending notes for the confirmation chime
const unsigned int  NOTE_1      = 659;  // E5
const unsigned int  NOTE_2      = 784;  // G5
const unsigned long NOTE_1_MS   = 120;
const unsigned long NOTE_2_MS   = 160;
const unsigned long NOTE_GAP_MS = 60;

// Lockout against repeated presses (device-wide, not per button).
// Every attempt during the lockout resets it to a constant BASE_LOCKOUT_MS
// starting from that exact moment.
const unsigned long BASE_LOCKOUT_MS = 2000;
unsigned long lockedUntil = 0;

const char* QUEUE_FILE = "/queue.jsonl";
const unsigned long NETWORK_POLL_MS = 500; // how often the network task checks for new entries

uint8_t buttons[3]        = {BTN_GREEN, BTN_YELLOW, BTN_RED};
uint8_t leds[3]           = {LED_GREEN, LED_YELLOW, LED_RED};
const char* values[3]     = {"green", "yellow", "red"};
unsigned long lastPress[3] = {0, 0, 0};

// Protects the queue file from concurrent access by loop() (Core 1,
// writes on every click) and the network task (Core 0, reads/sends).
SemaphoreHandle_t queueMutex;

// ---------- LED / buzzer ----------

void allLedsOn() {
  for (int i = 0; i < 3; i++) digitalWrite(leds[i], HIGH);
}

void allLedsOff() {
  for (int i = 0; i < 3; i++) digitalWrite(leds[i], LOW);
}

// Manual square wave instead of tone()/noTone().
void beep(unsigned int frequency, unsigned long durationMs) {
  unsigned long periodUs = 1000000UL / frequency;
  unsigned long halfPeriodUs = periodUs / 2;
  unsigned long cycles = (durationMs * 1000UL) / periodUs;

  for (unsigned long i = 0; i < cycles; i++) {
    digitalWrite(BUZZER, HIGH);
    delayMicroseconds(halfPeriodUs);
    digitalWrite(BUZZER, LOW);
    delayMicroseconds(halfPeriodUs);
  }
}

void rejectFeedback() {
  Serial.println("Too fast - input rejected");
  allLedsOff();
  for (int i = 0; i < 2; i++) {
    allLedsOn();
    beep(180, 90);
    allLedsOff();
    delay(70);
  }
  beep(110, 350);
  allLedsOn();
}

void confirmFeedback(uint8_t index) {
  allLedsOff();

  digitalWrite(leds[index], HIGH);
  beep(NOTE_1, NOTE_1_MS);
  digitalWrite(leds[index], LOW);
  delay(NOTE_GAP_MS);

  digitalWrite(leds[index], HIGH);
  beep(NOTE_2, NOTE_2_MS);
  digitalWrite(leds[index], LOW);
  delay(PAUSE_MS);

  allLedsOn();
}

// ---------- Time ----------

String isoTimestamp() {
  time_t now;
  time(&now);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

// ---------- Queue: plain file access, NO networking ----------
// enqueue() runs in the context of loop() (Core 1) and must therefore
// always be fast - so it deliberately makes no HTTP call anymore.

void enqueue(const char* value, const String& timestamp) {
  xSemaphoreTake(queueMutex, portMAX_DELAY);
  File f = LittleFS.open(QUEUE_FILE, "a");
  if (f) {
    f.print(value);
    f.print("|");
    f.println(timestamp);
    f.close();
  } else {
    Serial.println("Could not open queue file");
  }
  xSemaphoreGive(queueMutex);
}

// ---------- HTTP: runs ONLY in the network task on Core 0 ----------

String buildPayload(const char* value, const String& timestamp, bool bootRecovered) {
  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["location"] = LOCATION;
  doc["value"] = value;
  doc["timestamp"] = timestamp;
  // true = survived a reboot/power outage in the queue,
  // false = processed normally during the running session
  doc["queued"] = bootRecovered;
  String out;
  serializeJson(doc, out);
  return out;
}

bool sendPayload(const String& payload) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(API_ENDPOINT);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Api-Key", API_KEY);
  http.setConnectTimeout(800);
  http.setTimeout(1500);

  int code = http.POST(payload);
  http.end();

  return (code >= 200 && code < 300);
}

// Works through the queue in order. As soon as one entry fails,
// it stops (order is preserved, no reordering).
void flushQueue(bool bootRecovered) {
  if (WiFi.status() != WL_CONNECTED) return;

  xSemaphoreTake(queueMutex, portMAX_DELAY);

  if (!LittleFS.exists(QUEUE_FILE)) {
    xSemaphoreGive(queueMutex);
    return;
  }

  File f = LittleFS.open(QUEUE_FILE, "r");
  if (!f) {
    xSemaphoreGive(queueMutex);
    return;
  }

  String remaining = "";
  bool stopSending = false;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int sep = line.indexOf('|');
    if (sep == -1) continue;

    String value = line.substring(0, sep);
    String timestamp = line.substring(sep + 1);
    String payload = buildPayload(value.c_str(), timestamp, bootRecovered);

    if (!stopSending && sendPayload(payload)) {
      Serial.println("Queue entry sent");
    } else {
      stopSending = true;
      remaining += line + "\n";
    }
  }
  f.close();

  LittleFS.remove(QUEUE_FILE);
  if (remaining.length() > 0) {
    File out = LittleFS.open(QUEUE_FILE, "w");
    out.print(remaining);
    out.close();
  }

  xSemaphoreGive(queueMutex);
}

// Runs permanently on Core 0 - completely separate from the button polling
// on Core 1. WiFi connection setup, NTP sync and HTTP sending only happen
// here; a hang has no effect whatsoever on button response time.
void networkTask(void* parameter) {
  bool firstRun = true;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        vTaskDelay(pdMS_TO_TICKS(300));
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi connected, IP: ");
        Serial.println(WiFi.localIP());

        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        time_t now = time(nullptr);
        while (now < 100000) {
          vTaskDelay(pdMS_TO_TICKS(200));
          now = time(nullptr);
        }
        Serial.println("Time synchronized");
      }
    }

    flushQueue(firstRun);
    firstRun = false;

    vTaskDelay(pdMS_TO_TICKS(NETWORK_POLL_MS));
  }
}

// ---------- Vote processing: runs in loop(), Core 1 ----------

void handleVote(uint8_t index) {
  Serial.print("Selected: ");
  Serial.println(values[index]);

  // Local feedback + queue write - both with zero network dependency,
  // so loop() never blocks.
  confirmFeedback(index);
  enqueue(values[index], isoTimestamp());
}

void setup() {
  Serial.begin(115200);
  delay(300);

  for (int i = 0; i < 3; i++) {
    pinMode(buttons[i], INPUT_PULLUP);
    pinMode(leds[i], OUTPUT);
  }
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  queueMutex = xSemaphoreCreateMutex();

  // Start the network task on Core 0. loop() runs on Core 1 by default
  // on ESP32 Arduino, keeping it completely independent.
  xTaskCreatePinnedToCore(
      networkTask,
      "NetworkTask",
      8192,
      NULL,
      1,
      NULL,
      0
  );

  allLedsOn();
  Serial.println("Ready. All LEDs on.");
}

void loop() {
  unsigned long nowMs = millis();

  for (int i = 0; i < 3; i++) {
    if (digitalRead(buttons[i]) == LOW && nowMs - lastPress[i] > DEBOUNCE_MS) {
      lastPress[i] = nowMs;

      if (nowMs < lockedUntil) {
        lockedUntil = nowMs + BASE_LOCKOUT_MS;
        rejectFeedback();
      } else {
        handleVote(i);
        lockedUntil = nowMs + BASE_LOCKOUT_MS;
      }
    }
  }
}
