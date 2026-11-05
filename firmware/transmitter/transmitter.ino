// Borewell Controller - TRANSMITTER (ESP32 Dev Module + SX1278 RA-02 433 MHz)
// FreeRTOS: radioTask (core 0, owns LoRa/SPI), touchTask + uiTask (core 1, UI owns I2C).
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_task_wdt.h>
#include <esp_arduino_version.h>

// ------------------------------ Pins ------------------------------
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define LORA_SS     5
#define LORA_RST   14
#define LORA_DIO0  26
#define LORA_FREQ 433E6

#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_ADDR 0x3C

#define LED_BLUE       2
#define LED_MOTOR_OFF 32
#define LED_MOTOR_ON   4
#define TOUCH_PIN     34   // input-only, needs module's push-pull output
#define BUZZER_PIN    33
#define TOUCH_ACTIVE HIGH

// ----------------------------- Timing -----------------------------
const uint32_t LINK_TIMEOUT_MS        = 8000;
const uint32_t HEARTBEAT_INTERVAL_MS  = 2500;
const uint32_t RADIO_RECOVERY_MS      = 30000;  // re-init radio if silent this long
const uint32_t COMMAND_ACK_TIMEOUT_MS = 1500;
const uint8_t  COMMAND_MAX_ATTEMPTS   = 3;

const uint32_t DEBOUNCE_MS          = 30;
const uint32_t MIN_PRESS_MS         = 50;
const uint32_t NORMAL_HOLD_MS       = 1000;
const uint32_t PRESET15_HOLD_MS     = 4000;
const uint32_t TRIPLE_TAP_WINDOW_MS = 700;

const uint32_t DISPLAY_INTERVAL_MS = 500;
const uint32_t DEBUG_INTERVAL_MS   = 5000;
const uint32_t NO_LINK_BLINK_MS    = 150;
const uint32_t LINK_LED_ON_MS      = 100;
const uint32_t LINK_LED_OFF_MS     = 2000;
const uint32_t BEEP_ON_MS          = 100;
const uint32_t BEEP_GAP_MS         = 120;

const uint32_t WDT_TIMEOUT_S = 10;

// -------------------------- Shared state --------------------------
enum Cmd : uint8_t { CMD_ON, CMD_OFF, CMD_T10, CMD_T15, CMD_TOGGLE };
static const char *CMD_NAMES[] = {"ON", "OFF", "T10", "T15"};

struct LinkState {
  bool connected;
  bool haveFirstAck;
  bool motorOn;
  bool commandPending;
  uint8_t pendingCmd;
  uint8_t attempts;
  char mode[16];
  uint32_t elapsedBase;   // seconds reported by receiver
  uint32_t elapsedSyncMs;
  uint32_t lastRxMs;
};

static LinkState g;
static SemaphoreHandle_t gMutex;
static QueueHandle_t cmdQueue;    // touchTask -> radioTask (Cmd)
static QueueHandle_t beepQueue;   // radioTask -> uiTask (beep count)
static TaskHandle_t uiTaskHandle;

Adafruit_SSD1306 display(128, 64, &Wire, -1);

static inline uint32_t nowMs() { return millis(); }

static void lockState()   { xSemaphoreTake(gMutex, portMAX_DELAY); }
static void unlockState() { xSemaphoreGive(gMutex); }

static LinkState snapshot() {
  lockState();
  LinkState s = g;
  unlockState();
  return s;
}

static void requestRedraw() { if (uiTaskHandle) xTaskNotifyGive(uiTaskHandle); }

static void beep(uint8_t count) { xQueueSend(beepQueue, &count, 0); }

static void wdtInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {.timeout_ms = WDT_TIMEOUT_S * 1000,
                               .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
                               .trigger_panic = true};
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
}

// ------------------------------ Radio -----------------------------
static bool radioInit() {
  // LoRa.end() releases the SPI bus, so always re-attach it to our pins.
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) return false;
  // Must match receiver.
  LoRa.setSyncWord(0x42);
  LoRa.setSpreadingFactor(10);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(8);
  LoRa.setPreambleLength(12);
  LoRa.setTxPower(17);
  LoRa.enableCrc();
  return true;
}

static void sendPacket(const char *packet) {
  if (LoRa.beginPacket() == 0) {
    Serial.println("[LORA ERROR] beginPacket failed");
    return;
  }
  LoRa.print(packet);
  if (LoRa.endPacket() == 0) Serial.println("[LORA ERROR] endPacket failed");
  Serial.printf("[TX] %s\n", packet);
}

// ACK:id:state:mode:elapsed
static void handleAck(const char *pkt, uint32_t pendingId, bool &pending) {
  unsigned long id = 0, elapsed = 0;
  char state[4] = {0}, mode[16] = {0};
  if (sscanf(pkt, "ACK:%lu:%3[^:]:%15[^:]:%lu", &id, state, mode, &elapsed) != 4) {
    Serial.println("[RX ERROR] Malformed packet ignored");
    return;
  }
  bool on;
  if (strcmp(state, "ON") == 0) on = true;
  else if (strcmp(state, "OFF") == 0) on = false;
  else {
    Serial.println("[RX ERROR] Invalid motor state");
    return;
  }

  uint32_t t = nowMs();
  bool wasConnected, prevMotor, confirmed = false;
  uint8_t cmd;

  lockState();
  wasConnected = g.connected;
  prevMotor = g.motorOn;
  g.motorOn = on;
  strlcpy(g.mode, mode, sizeof(g.mode));
  g.elapsedBase = elapsed;
  g.elapsedSyncMs = t;
  g.connected = true;
  g.haveFirstAck = true;
  g.lastRxMs = t;
  cmd = g.pendingCmd;
  if (pending && id == pendingId) {
    pending = false;
    g.commandPending = false;
    confirmed = true;
  }
  unlockState();

  if (confirmed) Serial.printf("[COMMAND CONFIRMED] %s ID=%lu\n", CMD_NAMES[cmd], id);
  if (!wasConnected) {
    Serial.println("[LINK OK] ACK received; link restored");
    beep(2);
  } else if (prevMotor != on) {
    Serial.printf("[MOTOR CONFIRMED] %s\n", on ? "ON" : "OFF");
    beep(1);
  }
  requestRedraw();
}

static void radioTask(void *) {
  esp_task_wdt_add(NULL);

  // Random session base so receiver dedupe never matches IDs from a previous boot.
  uint32_t seq = random(1, 100000);
  bool pending = false;
  uint32_t pendingId = 0, pendingSince = 0, lastHeartbeat = 0;
  uint32_t lastAnyRx = nowMs();
  uint8_t attempts = 0, pendingCmd = 0;
  char pkt[64];

  for (;;) {
    esp_task_wdt_reset();
    uint32_t t = nowMs();

    // ---- Receive ----
    int size = LoRa.parsePacket();
    if (size > 0) {
      int n = 0;
      while (LoRa.available()) {
        int c = LoRa.read();
        if (n < (int)sizeof(pkt) - 1) pkt[n++] = (char)c;
      }
      pkt[n] = '\0';
      lastAnyRx = t;
      Serial.printf("[RX] %s (RSSI %d, SNR %.1f)\n", pkt, LoRa.packetRssi(), LoRa.packetSnr());
      handleAck(pkt, pendingId, pending);
    }

    // ---- New command from touch ----
    uint8_t cmd;
    if (!pending && xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
      LinkState s = snapshot();
      if (!s.connected) {
        Serial.println("[COMMAND REJECTED] Link down");
      } else {
        if (cmd == CMD_TOGGLE) cmd = s.motorOn ? CMD_OFF : CMD_ON;
        pendingId = ++seq;
        pendingCmd = cmd;
        attempts = 0;
        pending = true;
        lockState();
        g.commandPending = true;
        g.pendingCmd = cmd;
        g.attempts = 0;
        unlockState();
        pendingSince = t - COMMAND_ACK_TIMEOUT_MS;  // transmit immediately below
        Serial.printf("[COMMAND] %s ID=%lu\n", CMD_NAMES[cmd], (unsigned long)pendingId);
        requestRedraw();
      }
    } else if (pending) {
      xQueueReset(cmdQueue);  // drop presses while busy
    }

    // ---- Command retry ----
    if (pending && t - pendingSince >= COMMAND_ACK_TIMEOUT_MS) {
      if (attempts < COMMAND_MAX_ATTEMPTS) {
        if (attempts > 0) Serial.printf("[RETRY] %s attempt %u\n", CMD_NAMES[pendingCmd], attempts + 1);
        snprintf(pkt, sizeof(pkt), "CMD:%lu:%s", (unsigned long)pendingId, CMD_NAMES[pendingCmd]);
        sendPacket(pkt);
        attempts++;
        pendingSince = nowMs();
        lockState();
        g.attempts = attempts;
        unlockState();
      } else {
        Serial.printf("[COMMAND FAILED] No ACK for %s ID=%lu\n", CMD_NAMES[pendingCmd],
                      (unsigned long)pendingId);
        pending = false;
        lockState();
        g.commandPending = false;
        unlockState();
        requestRedraw();
      }
    }

    // ---- Heartbeat (skipped while waiting for a command ACK) ----
    if (!pending && t - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
      lastHeartbeat = t;
      snprintf(pkt, sizeof(pkt), "PING:%lu", (unsigned long)++seq);
      sendPacket(pkt);
    }

    // ---- Link timeout ----
    lockState();
    bool lost = g.connected && (nowMs() - g.lastRxMs > LINK_TIMEOUT_MS);
    if (lost) {
      g.connected = false;
      strlcpy(g.mode, "LINK LOST", sizeof(g.mode));
    }
    unlockState();
    if (lost) {
      Serial.println("[LINK LOST] No valid ACK; PING polling continues");
      requestRedraw();
    }

    // ---- Radio self-recovery (handles SX1278 lockups / brownouts) ----
    if (nowMs() - lastAnyRx > RADIO_RECOVERY_MS) {
      Serial.println("[RADIO] Silent too long; re-initialising LoRa");
      LoRa.end();
      vTaskDelay(pdMS_TO_TICKS(50));
      if (!radioInit()) Serial.println("[RADIO ERROR] Re-init failed");
      lastAnyRx = nowMs();
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ------------------------------ Touch -----------------------------
static void sendCmd(uint8_t c) { xQueueSend(cmdQueue, &c, 0); }

static void touchTask(void *) {
  esp_task_wdt_add(NULL);
  bool raw = digitalRead(TOUCH_PIN) == TOUCH_ACTIVE;
  bool stable = raw, timing = false, preset15Sent = false;
  uint32_t rawChangedAt = nowMs(), touchStart = 0, lastTap = 0;
  uint8_t taps = 0;
  TickType_t wake = xTaskGetTickCount();

  for (;;) {
    esp_task_wdt_reset();
    uint32_t t = nowMs();
    bool reading = digitalRead(TOUCH_PIN) == TOUCH_ACTIVE;
    if (reading != raw) {
      raw = reading;
      rawChangedAt = t;
    }

    if (raw != stable && t - rawChangedAt >= DEBOUNCE_MS) {
      stable = raw;
      if (stable) {
        touchStart = t;
        timing = true;
        preset15Sent = false;
      } else if (timing) {
        uint32_t d = t - touchStart;
        timing = false;
        Serial.printf("[TOUCH] Release after %lu ms\n", (unsigned long)d);
        if (d >= MIN_PRESS_MS && !preset15Sent) {
          if (d >= NORMAL_HOLD_MS) {
            taps = 0;
            sendCmd(CMD_TOGGLE);
          } else {
            if (t - lastTap > TRIPLE_TAP_WINDOW_MS) taps = 0;
            taps++;
            lastTap = t;
            if (taps >= 3) {
              taps = 0;
              sendCmd(CMD_T10);
            }
          }
        }
        preset15Sent = false;
      }
    }

    if (stable && timing && !preset15Sent && t - touchStart >= PRESET15_HOLD_MS) {
      preset15Sent = true;
      taps = 0;
      Serial.println("[TOUCH] Requesting 15-minute preset");
      sendCmd(CMD_T15);
    }

    vTaskDelayUntil(&wake, pdMS_TO_TICKS(10));
  }
}

// ------------------------- UI: LEDs/Buzzer/OLED -------------------
static void drawOLED(const LinkState &s) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("BOREWELL ");
  display.print(s.connected ? "LINK OK" : "NO LINK");
  display.drawLine(0, 11, 127, 11, SSD1306_WHITE);

  display.setCursor(0, 15);
  display.print("MOTOR: ");
  display.print(!s.connected ? "UNKNOWN" : (s.motorOn ? "ON" : "OFF"));

  display.setCursor(0, 25);
  display.print("MODE: ");
  display.print(s.mode);
  if (s.commandPending) {
    display.setCursor(82, 15);
    display.print("WAIT ACK");
  }

  uint32_t sec = s.elapsedBase;
  if (s.connected && s.motorOn) sec += (nowMs() - s.elapsedSyncMs) / 1000UL;
  char buf[12];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(sec / 60UL), (unsigned long)(sec % 60UL));
  display.setTextSize(3);
  display.setCursor(8, 38);
  display.print(buf);
  display.display();
}

static void uiTask(void *) {
  esp_task_wdt_add(NULL);
  bool blue = false, buzzerOn = false;
  uint8_t beepsLeft = 0;
  uint32_t lastBlue = 0, buzzerAt = 0, lastDisplay = 0, lastDebug = 0;

  for (;;) {
    esp_task_wdt_reset();
    // Wake every 10 ms or immediately on redraw request.
    bool redraw = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)) > 0;
    LinkState s = snapshot();
    uint32_t t = nowMs();

    // Blue LED
    uint32_t iv = s.connected ? (blue ? LINK_LED_ON_MS : LINK_LED_OFF_MS) : NO_LINK_BLINK_MS;
    if (t - lastBlue >= iv) {
      lastBlue = t;
      blue = !blue;
      digitalWrite(LED_BLUE, blue);
    }

    // Motor LEDs (both off = unknown)
    digitalWrite(LED_MOTOR_ON, s.connected && s.motorOn);
    digitalWrite(LED_MOTOR_OFF, s.connected && !s.motorOn);

    // Buzzer
    uint8_t req;
    if (xQueueReceive(beepQueue, &req, 0) == pdTRUE) {
      beepsLeft = req;
      buzzerOn = false;
      buzzerAt = t - BEEP_GAP_MS;
      digitalWrite(BUZZER_PIN, LOW);
    }
    if (beepsLeft) {
      if (!buzzerOn && t - buzzerAt >= BEEP_GAP_MS) {
        buzzerOn = true;
        buzzerAt = t;
        digitalWrite(BUZZER_PIN, HIGH);
      } else if (buzzerOn && t - buzzerAt >= BEEP_ON_MS) {
        buzzerOn = false;
        buzzerAt = t;
        digitalWrite(BUZZER_PIN, LOW);
        beepsLeft--;
      }
    }

    // OLED
    if (redraw || t - lastDisplay >= DISPLAY_INTERVAL_MS) {
      lastDisplay = t;
      drawOLED(s);
    }

    if (t - lastDebug >= DEBUG_INTERVAL_MS) {
      lastDebug = t;
      char age[16] = "NO-ACK-YET";
      if (s.haveFirstAck) snprintf(age, sizeof(age), "%lu", (unsigned long)(t - s.lastRxMs));
      Serial.printf("[HEALTH] link=%s ACK-age-ms=%s motor=%s pending=%s attempts=%u heap=%u minHeap=%u\n",
                    s.connected ? "OK" : "DOWN", age,
                    !s.connected ? "UNKNOWN" : (s.motorOn ? "ON" : "OFF"),
                    s.commandPending ? CMD_NAMES[s.pendingCmd] : "NONE",
                    s.commandPending ? (unsigned)s.attempts : 0u,
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    }
  }
}

// ------------------------------ Setup -----------------------------
static void fatal(const char *msg) {
  Serial.printf("[FATAL] %s - restarting in 5 s\n", msg);
  delay(5000);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(LED_BLUE, OUTPUT);
  pinMode(LED_MOTOR_OFF, OUTPUT);
  pinMode(LED_MOTOR_ON, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TOUCH_PIN, INPUT);
  digitalWrite(LED_BLUE, LOW);
  digitalWrite(LED_MOTOR_OFF, LOW);
  digitalWrite(LED_MOTOR_ON, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  gMutex = xSemaphoreCreateMutex();
  cmdQueue = xQueueCreate(4, sizeof(uint8_t));
  beepQueue = xQueueCreate(4, sizeof(uint8_t));
  strlcpy(g.mode, "OFF", sizeof(g.mode));

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(50);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) fatal("OLED init failed");
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("BOREWELL CONTROLLER");
  display.println("Starting LoRa...");
  display.display();

  if (!radioInit()) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("LORA INIT FAILED");
    display.println("Restarting...");
    display.display();
    fatal("LoRa init failed; check wiring and 3.3V power");
  }

  wdtInit();

  xTaskCreatePinnedToCore(uiTask, "ui", 4096, NULL, 1, &uiTaskHandle, 1);
  xTaskCreatePinnedToCore(touchTask, "touch", 3072, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(radioTask, "radio", 6144, NULL, 4, NULL, 0);

  Serial.println("\n================================");
  Serial.println("Transmitter ready (FreeRTOS)");
  Serial.println("Hold 1-4 s: ON/OFF | 3 taps: T10 | Hold 4 s: T15");
  Serial.println("================================");
}

void loop() { vTaskDelete(NULL); }
