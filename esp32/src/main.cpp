#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>
#include "config.h"

// --- Pin assignment ---
const uint8_t BTN_GREEN = 32;
const uint8_t BTN_YELLOW = 33;
const uint8_t BTN_RED = 25;
const uint8_t LED_GREEN = 26;
const uint8_t LED_YELLOW = 27;
const uint8_t LED_RED = 14;
const uint8_t BUZZER = 4;

const unsigned long DEBOUNCE_MS = 250;
const unsigned long PAUSE_MS = 150;

// Two ascending notes for the confirmation chime
const unsigned int NOTE_1 = 659; // E5
const unsigned int NOTE_2 = 784; // G5
const unsigned long NOTE_1_MS = 120;
const unsigned long NOTE_2_MS = 160;
const unsigned long NOTE_GAP_MS = 60;

// Lockout against repeated presses (device-wide, not per button).
// Every attempt during the lockout resets it to a constant BASE_LOCKOUT_MS
// starting from that exact moment.
const unsigned long BASE_LOCKOUT_MS = 2000;
unsigned long lockedUntil = 0;

const char *QUEUE_FILE = "/queue.jsonl";
const unsigned long NETWORK_POLL_MS = 500;    // how often the network task loop ticks (WiFi check etc.)
const unsigned long RETRY_BACKOFF_MS = 60000; // how long to wait before retrying after a failed send

uint8_t buttons[3] = {BTN_GREEN, BTN_YELLOW, BTN_RED};
uint8_t leds[3] = {LED_GREEN, LED_YELLOW, LED_RED};
const char *values[3] = {"green", "yellow", "red"};
unsigned long lastPress[3] = {0, 0, 0};

// Protects the queue file from concurrent access by loop() (Core 1,
// writes on every click) and the network task (Core 0, reads/sends).
SemaphoreHandle_t queueMutex;

// Tracked in RAM instead of calling LittleFS.exists()/open() every poll cycle:
// on this ESP32 core, opening a non-existent file for reading always logs a
// scary-looking "[E] open(): ... does not exist" line at the VFS level, even
// though the higher-level call correctly returns false/null. Since the queue
// is empty almost all the time, that would spam the console non-stop.
volatile bool queueMayHaveData = false;

// ---------- LED / buzzer ----------

void allLedsOn()
{
  for (int i = 0; i < 3; i++)
    digitalWrite(leds[i], HIGH);
}

void allLedsOff()
{
  for (int i = 0; i < 3; i++)
    digitalWrite(leds[i], LOW);
}

// Manual square wave instead of tone()/noTone().
void beep(unsigned int frequency, unsigned long durationMs)
{
  unsigned long periodUs = 1000000UL / frequency;
  unsigned long halfPeriodUs = periodUs / 2;
  unsigned long cycles = (durationMs * 1000UL) / periodUs;

  for (unsigned long i = 0; i < cycles; i++)
  {
    digitalWrite(BUZZER, HIGH);
    delayMicroseconds(halfPeriodUs);
    digitalWrite(BUZZER, LOW);
    delayMicroseconds(halfPeriodUs);
  }
}

void rejectFeedback()
{
  Serial.println("Too fast - input rejected");
  allLedsOff();
  for (int i = 0; i < 2; i++)
  {
    allLedsOn();
    beep(180, 90);
    allLedsOff();
    delay(70);
  }
  beep(110, 350);
  allLedsOn();
}

void confirmFeedback(uint8_t index)
{
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

String isoTimestamp()
{
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

void enqueue(const char *value, const String &timestamp)
{
  xSemaphoreTake(queueMutex, portMAX_DELAY);
  File f = LittleFS.open(QUEUE_FILE, "a");
  if (f)
  {
    f.print(value);
    f.print("|");
    f.println(timestamp);
    f.close();
    queueMayHaveData = true;
    Serial.printf("Enqueued: %s @ %s\n", value, timestamp.c_str());
  }
  else
  {
    Serial.println("ERROR: could not open queue file for writing");
  }
  xSemaphoreGive(queueMutex);
}

// ---------- HTTP: runs ONLY in the network task on Core 0 ----------

String buildPayload(const char *value, const String &timestamp, bool bootRecovered)
{
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

bool sendPayload(const String &payload)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("Send skipped: no WiFi connection");
    return false;
  }

  HTTPClient http;
  http.begin(API_ENDPOINT);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Api-Key", API_KEY);
  http.setConnectTimeout(800);
  http.setTimeout(1500);

  int code = http.POST(payload);
  bool ok = (code >= 200 && code < 300);

  if (ok)
  {
    Serial.printf("Send OK, HTTP %d: %s\n", code, payload.c_str());
  }
  else if (code > 0)
  {
    Serial.printf("Send FAILED, HTTP %d: %s\n", code, payload.c_str());
  }
  else
  {
    // negative code = connection-level error (timeout, DNS, refused, ...)
    Serial.printf("Send FAILED, connection error %d (%s): %s\n",
                  code, http.errorToString(code).c_str(), payload.c_str());
  }

  http.end();
  return ok;
}

// Works through the queue in order. As soon as one entry fails,
// it stops (order is preserved, no reordering).
// Returns true if the queue is now fully flushed (or was already empty),
// false if at least one entry failed and is still waiting.
bool flushQueue(bool bootRecovered)
{
  if (WiFi.status() != WL_CONNECTED)
    return true;
  if (!queueMayHaveData)
    return true; // avoids touching LittleFS when we already know it's empty

  xSemaphoreTake(queueMutex, portMAX_DELAY);

  if (!LittleFS.exists(QUEUE_FILE))
  {
    queueMayHaveData = false;
    xSemaphoreGive(queueMutex);
    return true;
  }

  File f = LittleFS.open(QUEUE_FILE, "r");
  if (!f)
  {
    queueMayHaveData = false;
    xSemaphoreGive(queueMutex);
    return true;
  }

  // Read all lines first, so we know up front whether there's anything to do
  // and can log it even before the first send attempt.
  int lineCount = 0;
  {
    File count = LittleFS.open(QUEUE_FILE, "r");
    while (count.available())
    {
      String l = count.readStringUntil('\n');
      l.trim();
      if (l.length() > 0)
        lineCount++;
    }
    count.close();
  }
  if (lineCount > 0)
  {
    Serial.printf("Flushing queue: %d entr%s pending\n", lineCount, lineCount == 1 ? "y" : "ies");
  }

  String remaining = "";
  bool stopSending = false;

  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0)
      continue;

    int sep = line.indexOf('|');
    if (sep == -1)
      continue;

    String value = line.substring(0, sep);
    String timestamp = line.substring(sep + 1);
    String payload = buildPayload(value.c_str(), timestamp, bootRecovered);

    if (!stopSending && sendPayload(payload))
    {
      // sendPayload() already logs success with its HTTP code
    }
    else
    {
      stopSending = true;
      remaining += line + "\n";
    }
  }
  f.close();

  LittleFS.remove(QUEUE_FILE);
  if (remaining.length() > 0)
  {
    File out = LittleFS.open(QUEUE_FILE, "w");
    out.print(remaining);
    out.close();
    Serial.printf("Queue not fully flushed, retrying in %lu s\n", RETRY_BACKOFF_MS / 1000);
  }
  queueMayHaveData = (remaining.length() > 0);

  xSemaphoreGive(queueMutex);
  return remaining.length() == 0;
}

// Runs permanently on Core 0 - completely separate from the button polling
// on Core 1. WiFi connection setup, NTP sync and HTTP sending only happen
// here; a hang has no effect whatsoever on button response time.
void networkTask(void *parameter)
{
  bool firstRun = true;
  unsigned long nextFlushAttempt = 0; // 0 = try immediately

  for (;;)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.printf("Connecting to WiFi \"%s\"...\n", WIFI_SSID);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 15000)
      {
        vTaskDelay(pdMS_TO_TICKS(300));
      }
      if (WiFi.status() == WL_CONNECTED)
      {
        Serial.print("WiFi connected, IP: ");
        Serial.println(WiFi.localIP());

        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        time_t now = time(nullptr);
        while (now < 100000)
        {
          vTaskDelay(pdMS_TO_TICKS(200));
          now = time(nullptr);
        }
        Serial.println("Time synchronized");
      }
      else
      {
        Serial.println("WiFi connection FAILED (timeout after 15s), will retry");
      }
    }

    if (millis() >= nextFlushAttempt)
    {
      bool fullyFlushed = flushQueue(firstRun);
      firstRun = false;
      nextFlushAttempt = fullyFlushed ? millis() : millis() + RETRY_BACKOFF_MS;
    }

    vTaskDelay(pdMS_TO_TICKS(NETWORK_POLL_MS));
  }
}

// ---------- Vote processing: runs in loop(), Core 1 ----------

void handleVote(uint8_t index)
{
  Serial.print("Selected: ");
  Serial.println(values[index]);

  // Local feedback + queue write - both with zero network dependency,
  // so loop() never blocks.
  confirmFeedback(index);
  enqueue(values[index], isoTimestamp());
}

void setup()
{
  Serial.begin(115200);
  delay(300);

  for (int i = 0; i < 3; i++)
  {
    pinMode(buttons[i], INPUT_PULLUP);
    pinMode(leds[i], OUTPUT);
  }
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  if (!LittleFS.begin(true))
  {
    Serial.println("LittleFS mount failed");
  }

  // One-time check at boot: a queue file might already exist from before a
  // reboot/power loss. This is the only place we call exists() unconditionally,
  // so the harmless "[E] open(): ... does not exist" log (if any) only ever
  // appears once here, not on every network poll cycle.
  queueMayHaveData = LittleFS.exists(QUEUE_FILE);

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
      0);

  allLedsOn();
  Serial.println("Ready. All LEDs on.");
}

void loop()
{
  unsigned long nowMs = millis();

  for (int i = 0; i < 3; i++)
  {
    if (digitalRead(buttons[i]) == LOW && nowMs - lastPress[i] > DEBOUNCE_MS)
    {
      lastPress[i] = nowMs;

      if (nowMs < lockedUntil)
      {
        lockedUntil = nowMs + BASE_LOCKOUT_MS;
        rejectFeedback();
      }
      else
      {
        handleVote(i);
        lockedUntil = nowMs + BASE_LOCKOUT_MS;
      }
    }
  }
}