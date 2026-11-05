// Borewell Controller - RECEIVER (ESP32-C3 SuperMini + SX1278 RA-02 433 MHz)
// FreeRTOS: radioTask owns LoRa/SPI; safetyTask owns relay timing (preset + link watchdog).
#include <SPI.h>
#include <LoRa.h>
#include <esp_task_wdt.h>
#include <esp_arduino_version.h>
#include <driver/gpio.h>

#define LORA_SCK   4
#define LORA_MISO  5
#define LORA_MOSI  6
#define LORA_SS    7
#define LORA_RST  10
#define LORA_DIO0  3
#define LORA_FREQ 433E6

// GPIO0: not a strapping pin on the C3 and high-impedance during reset.
#define RELAY_PIN 0
#define RELAY_ACTIVE   LOW    // active-LOW relay module
#define RELAY_INACTIVE HIGH

const uint32_t LINK_TIMEOUT_MS   = 10000;
const uint32_t RADIO_RECOVERY_MS = 30000;
const uint32_t STATUS_INTERVAL_MS = 10000;
const uint32_t WDT_TIMEOUT_S     = 10;

struct MotorState {
  bool on;
  char mode[12];
  uint32_t startMs;
  uint32_t presetMs;
  uint32_t lastTxPacketMs;
};

static MotorState m;
static SemaphoreHandle_t mMutex;

static void lockM()   { xSemaphoreTake(mMutex, portMAX_DELAY); }
static void unlockM() { xSemaphoreGive(mMutex); }

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

// Caller must hold mMutex.
static void setMotorLocked(bool on, const char *mode, uint32_t presetMs) {
  m.on = on;
  strlcpy(m.mode, on ? mode : "OFF", sizeof(m.mode));
  m.startMs = millis();
  m.presetMs = on ? presetMs : 0;
  digitalWrite(RELAY_PIN, on ? RELAY_ACTIVE : RELAY_INACTIVE);
  Serial.printf("Motor: %s | Mode: %s\n", on ? "ON" : "OFF", m.mode);
}

static bool radioInit() {
  // LoRa.end() releases the SPI bus, so always re-attach it to our pins.
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) return false;
  LoRa.setSyncWord(0x42);
  LoRa.setSpreadingFactor(10);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(8);
  LoRa.setPreambleLength(12);
  LoRa.setTxPower(17);
  LoRa.enableCrc();
  return true;
}

// ACK:id:state:mode:elapsed
static void sendAck(unsigned long id) {
  char pkt[64];
  lockM();
  unsigned long elapsed = m.on ? (millis() - m.startMs) / 1000UL : 0;
  snprintf(pkt, sizeof(pkt), "ACK:%lu:%s:%s:%lu", id, m.on ? "ON" : "OFF", m.mode, elapsed);
  unlockM();

  LoRa.beginPacket();
  LoRa.print(pkt);
  LoRa.endPacket();
  Serial.printf("ACK sent: %s\n", pkt);
}

static void processPacket(const char *pkt) {
  static unsigned long lastCmdId = 0;
  static bool haveLastCmd = false;
  unsigned long id;
  char cmd[8] = {0};

  if (sscanf(pkt, "PING:%lu", &id) == 1) {
    lockM();
    m.lastTxPacketMs = millis();
    unlockM();
    sendAck(id);
    return;
  }

  if (sscanf(pkt, "CMD:%lu:%7s", &id, cmd) != 2) {
    Serial.println("Unknown/invalid packet ignored");
    return;
  }

  lockM();
  m.lastTxPacketMs = millis();
  if (!(haveLastCmd && id == lastCmdId)) {  // retries are ACKed but not re-executed
    lastCmdId = id;
    haveLastCmd = true;
    if (strcmp(cmd, "ON") == 0)       setMotorLocked(true, "MANUAL", 0);
    else if (strcmp(cmd, "OFF") == 0) setMotorLocked(false, "OFF", 0);
    else if (strcmp(cmd, "T10") == 0) setMotorLocked(true, "PRESET10", 10UL * 60UL * 1000UL);
    else if (strcmp(cmd, "T15") == 0) setMotorLocked(true, "PRESET15", 15UL * 60UL * 1000UL);
    else Serial.printf("Unsupported command: %s\n", cmd);
  } else {
    Serial.printf("Duplicate command ID %lu (retry)\n", id);
  }
  unlockM();

  sendAck(id);
}

static void radioTask(void *) {
  esp_task_wdt_add(NULL);
  char pkt[64];
  uint32_t lastAnyRx = millis();

  for (;;) {
    esp_task_wdt_reset();

    int size = LoRa.parsePacket();
    if (size > 0) {
      int n = 0;
      while (LoRa.available()) {
        int c = LoRa.read();
        if (n < (int)sizeof(pkt) - 1) pkt[n++] = (char)c;
      }
      pkt[n] = '\0';
      lastAnyRx = millis();
      Serial.printf("Received: %s (RSSI %d, SNR %.1f)\n", pkt, LoRa.packetRssi(), LoRa.packetSnr());
      processPacket(pkt);
    }

    if (millis() - lastAnyRx > RADIO_RECOVERY_MS) {
      Serial.println("RADIO: silent too long; re-initialising LoRa");
      LoRa.end();
      vTaskDelay(pdMS_TO_TICKS(50));
      if (!radioInit()) Serial.println("RADIO ERROR: re-init failed");
      lastAnyRx = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void safetyTask(void *) {
  esp_task_wdt_add(NULL);
  TickType_t wake = xTaskGetTickCount();
  uint32_t lastStatus = 0;

  for (;;) {
    esp_task_wdt_reset();

    lockM();
    // Read time under the lock so timestamps written by radioTask are never newer than t.
    uint32_t t = millis();
    if (m.on && m.presetMs > 0 && t - m.startMs >= m.presetMs) {
      Serial.println("Preset expired: motor OFF");
      setMotorLocked(false, "OFF", 0);
    }
    // Presets end on their own timer; only unbounded MANUAL runs need a live transmitter.
    if (m.on && m.presetMs == 0 && t - m.lastTxPacketMs > LINK_TIMEOUT_MS) {
      Serial.println("WATCHDOG: transmitter lost during MANUAL run; motor OFF");
      setMotorLocked(false, "OFF", 0);
    }
    // Re-assert relay output every cycle against glitches.
    digitalWrite(RELAY_PIN, m.on ? RELAY_ACTIVE : RELAY_INACTIVE);
    bool on = m.on;
    unlockM();

    if (t - lastStatus >= STATUS_INTERVAL_MS) {
      lastStatus = t;
      Serial.printf("[HEALTH] motor=%s heap=%u minHeap=%u\n", on ? "ON" : "OFF",
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    }

    vTaskDelayUntil(&wake, pdMS_TO_TICKS(50));
  }
}

void setup() {
  // Preload HIGH before enabling the driver; pinMode alone would briefly drive LOW (relay ON).
  gpio_set_level((gpio_num_t)RELAY_PIN, RELAY_INACTIVE);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_INACTIVE);

  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);  // never block when no USB host is attached
#endif
  delay(500);

  mMutex = xSemaphoreCreateMutex();
  strlcpy(m.mode, "OFF", sizeof(m.mode));
  m.lastTxPacketMs = millis();

  if (!radioInit()) {
    Serial.println("LoRa init failed - restarting in 5 s");
    delay(5000);
    ESP.restart();
  }

  wdtInit();

  xTaskCreate(safetyTask, "safety", 3072, NULL, 5, NULL);
  xTaskCreate(radioTask, "radio", 6144, NULL, 4, NULL);

  Serial.println("Receiver ready (FreeRTOS)");
}

void loop() { vTaskDelete(NULL); }
