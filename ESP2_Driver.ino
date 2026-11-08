// ============================================================
//  ESP-2  —  SEGMENT DRIVER UNIT  (rev-6)
// ============================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define ESPNOW_CHANNEL 1

// --- PINS ---
const int BUZZ_PIN    = 2;
const int latchPin    = 17;
const int clockPin    = 4;
const int dataPin     = 16;
const int LED_COLON_1 = 18;
const int LED_COLON_2 = 19;

// --- 7-SEG ---
const byte digitMap[] = {0xC0,0xF9,0xA4,0xB0,0x99,0x92,0x82,0xF8,0x80,0x90};
const byte allOff     = 0xFF;

// --- NOTES ---
#define NOTE_FS5 370
#define NOTE_G5  392
#define NOTE_A5  440
#define NOTE_B5  494
#define NOTE_C6  523
#define NOTE_E6  659
#define REST     0

const int crucifiedNotes[] = {
  NOTE_B5, NOTE_C6, NOTE_B5, NOTE_B5,
  NOTE_C6, NOTE_B5, NOTE_B5,
  NOTE_C6, NOTE_B5, NOTE_E6, NOTE_A5, REST,
  NOTE_A5, NOTE_B5, NOTE_A5, NOTE_E6, NOTE_G5,
  NOTE_G5, NOTE_A5, NOTE_G5, NOTE_FS5
};
const int crucifiedDur[] = {
  900,300,300,900, 300,300,900, 300,300,900,900,300,
  900,300,300,900,900, 900,300,300,900
};
const int MELODY_LEN = 21;

enum State {
  WELCOME, VIEW_CLOCK, VIEW_TIMER, VIEW_DUAL,
  SET_CLOCK, SET_TIMER, CONFIRM_SET, MADE_IN_HEAVEN
};

struct __attribute__((packed)) StatePacket {
  uint8_t  state;
  int16_t  clkH, clkM, clkS;
  int32_t  timerSeconds;
  bool     isTimerRunning;
  bool     alarmActive;
  bool     isBeeping;
  bool     heavenActive;
  bool     heavenConfirmed;
  bool     forceSeconds;
  bool     manualOverride;
  float    heavenSpeed;
  uint8_t  alertPattern;
  uint8_t  alertRepeats;
  char     keyPressed;
  uint8_t  configDigits[6];
  uint8_t  settingCursor;
  uint8_t  settingPhase;
  bool     blinkState;
  uint32_t seqNum;
};

StatePacket rx;
bool    newPacket      = false;
uint8_t pendingPattern = 0;
uint8_t pendingRepeats = 0;
bool    buzzerBusy     = false;
bool    isBeepingLocal = false;

unsigned long dualLastSwitch = 0;
bool          dualShowClock  = true;

unsigned long lastPktTime = 0;
#define WATCHDOG_MS 3000

uint32_t pktReceived = 0;
uint32_t lastSeq     = 0;

void onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len != sizeof(StatePacket)) return;

  StatePacket tmp;
  memcpy(&tmp, data, sizeof(StatePacket));

  if (!buzzerBusy && tmp.alertPattern != 0 && tmp.alertPattern != rx.alertPattern) {
    pendingPattern = tmp.alertPattern;
    pendingRepeats = tmp.alertRepeats;
  }

  memcpy(&rx, &tmp, sizeof(StatePacket));
  lastPktTime = millis();
  pktReceived++;
  lastSeq = tmp.seqNum;
  newPacket = true;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(BUZZ_PIN,    OUTPUT);
  pinMode(latchPin,    OUTPUT);
  pinMode(clockPin,    OUTPUT);
  pinMode(dataPin,     OUTPUT);
  pinMode(LED_COLON_1, OUTPUT);
  pinMode(LED_COLON_2, OUTPUT);

  memset(&rx, 0, sizeof(rx));
  rx.state = (uint8_t)WELCOME;

  write7Seg(allOff, allOff, allOff, allOff);
  digitalWrite(LED_COLON_1, LOW);
  digitalWrite(LED_COLON_2, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    while (true) {
      write7Seg(0x00, 0x00, 0x00, 0x00); delay(400);
      write7Seg(allOff, allOff, allOff, allOff); delay(400);
    }
  }

  esp_now_register_recv_cb(onDataRecv);
  lastPktTime = millis(); 

  xTaskCreatePinnedToCore(TaskBuzzer,  "TaskBuzzer",  4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskSegment, "TaskSegment", 4096, NULL, 1, NULL, 0);
}

void loop() {}

void write7Seg(byte d1, byte d2, byte d3, byte d4) {
  digitalWrite(latchPin, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, d4);
  shiftOut(dataPin, clockPin, MSBFIRST, d3);
  shiftOut(dataPin, clockPin, MSBFIRST, d2);
  shiftOut(dataPin, clockPin, MSBFIRST, d1);
  digitalWrite(latchPin, HIGH);
}

const char* decodePattern(uint8_t idx) {
  switch (idx) {
    case 1: return ". . . .  . . . .";
    case 2: return ". . .    . . .";
    case 3: return ". .  . .";
    case 4: return "- . . .  - . . .";
    case 5: return "- . .  - . .";
    case 6: return "- .  - . .";
    case 7: return ".-.-  .- .-";
    case 8: return "- -  - -";
    default: return NULL;
  }
}

void playPattern(const char* p) {
  if (!p) return;
  for (int i = 0; p[i] != '\0'; i++) {
    if (p[i] == '.' || p[i] == '-') {
      isBeepingLocal = true;
      tone(BUZZ_PIN, 2000);
      vTaskDelay(((p[i]=='.') ? 200 : 1000) / portTICK_PERIOD_MS);
      noTone(BUZZ_PIN);
      isBeepingLocal = false;
    } else if (p[i] == ' ') {
      if (p[i+1] == ' ') { vTaskDelay(1000/portTICK_PERIOD_MS); i++; }
      else                 vTaskDelay(500/portTICK_PERIOD_MS);
    }
    vTaskDelay(150/portTICK_PERIOD_MS);
  }
}

void TaskBuzzer(void* pvParameters) {
  static int mi = 0;
  for (;;) {
    if (rx.alarmActive) {
      buzzerBusy = true; playPattern("- -  - -"); buzzerBusy = false;
      vTaskDelay(200/portTICK_PERIOD_MS); continue;
    }
    if (rx.keyPressed != 0) { tone(BUZZ_PIN, 2000, 30); vTaskDelay(50/portTICK_PERIOD_MS); }
    if (pendingPattern != 0 && !buzzerBusy) {
      uint8_t pat=pendingPattern, reps=pendingRepeats; pendingPattern=0;
      buzzerBusy=true;
      const char* s=decodePattern(pat);
      for (int i=0;i<reps;i++) { playPattern(s); if(i<reps-1) vTaskDelay(500/portTICK_PERIOD_MS); }
      buzzerBusy=false; continue;
    }
    if (rx.heavenActive) {
      (crucifiedNotes[mi]==REST) ? noTone(BUZZ_PIN) : tone(BUZZ_PIN,crucifiedNotes[mi]);
      isBeepingLocal=true;
      vTaskDelay(crucifiedDur[mi]/portTICK_PERIOD_MS);
      noTone(BUZZ_PIN); isBeepingLocal=false;
      vTaskDelay(40/portTICK_PERIOD_MS);
      if(++mi>=MELODY_LEN) mi=0; continue;
    }
    mi=0; vTaskDelay(50/portTICK_PERIOD_MS);
  }
}

void displayClock7Seg() {
  if (rx.forceSeconds)
    write7Seg(digitMap[rx.clkM/10],digitMap[rx.clkM%10],
              digitMap[rx.clkS/10],digitMap[rx.clkS%10]);
  else
    write7Seg(digitMap[rx.clkH/10],digitMap[rx.clkH%10],
              digitMap[rx.clkM/10],digitMap[rx.clkM%10]);
}

void displayTimer7Seg() {
  long h=rx.timerSeconds/3600, m=(rx.timerSeconds%3600)/60, s=rx.timerSeconds%60;
  if (h>0 && !rx.forceSeconds)
    write7Seg(digitMap[h/10],digitMap[h%10],digitMap[m/10],digitMap[m%10]);
  else
    write7Seg(digitMap[m/10],digitMap[m%10],digitMap[s/10],digitMap[s%10]);
}

void displaySetting7Seg() {
  bool    blink = rx.blinkState;
  uint8_t phase = rx.settingPhase;
  uint8_t cur   = rx.settingCursor;
  uint8_t d[6];
  for (int i=0;i<6;i++) d[i]=rx.configDigits[i];

  int base = phase * 2;
  byte seg[4];
  for (int i=0;i<4;i++) {
    int src = base+i;
    if (src>5) { seg[i]=allOff; continue; }
    seg[i] = ((src==(int)cur) && !blink) ? allOff : digitMap[d[src]];
  }
  write7Seg(seg[0],seg[1],seg[2],seg[3]);
}

void TaskSegment(void* pvParameters) {
  for (;;) {
    bool blink = (millis() % 1000 < 500);
    bool linkLost = (millis() - lastPktTime > WATCHDOG_MS);

    // ── FIX: Auto-Channel Hunter ──────────────────────────────
    if (linkLost) {
      write7Seg(allOff, allOff, allOff, allOff);
      bool slowBlink = (millis() % 2000 < 1000);
      digitalWrite(LED_COLON_1, slowBlink);
      digitalWrite(LED_COLON_2, slowBlink);

      // If ESP-1 hopped channels to get WiFi, we must find it
      static unsigned long lastScanTime = 0;
      if (millis() - lastScanTime > 250) { 
        static uint8_t huntChannel = 1;
        huntChannel = (huntChannel % 11) + 1; // Cycle channels 1-11
        esp_wifi_set_channel(huntChannel, WIFI_SECOND_CHAN_NONE);
        lastScanTime = millis();
      }
      vTaskDelay(30/portTICK_PERIOD_MS);
      continue;
    }

    bool ledState = isBeepingLocal ? true
                  : ((rx.alertPattern || rx.alarmActive) ? false : blink);
    digitalWrite(LED_COLON_1, ledState);
    digitalWrite(LED_COLON_2, ledState);

    State st = (State)rx.state;

    if (st == WELCOME) {
      write7Seg(allOff, allOff, allOff, allOff);
    }
    else if (rx.alarmActive) {
      byte v = isBeepingLocal ? digitMap[0] : allOff;
      write7Seg(v,v,v,v);
    }
    else if (st == MADE_IN_HEAVEN) {
      if (!rx.heavenConfirmed)
        write7Seg(0x89,0x86,0x86,0xC7);
      else if (rx.heavenActive) {
        if (rx.heavenSpeed < 80.0f)
          write7Seg(digitMap[rx.clkH/10],digitMap[rx.clkH%10],
                    digitMap[rx.clkM/10],digitMap[rx.clkM%10]);
        else
          write7Seg(random(0,255),random(0,255),random(0,255),random(0,255));
      } else {
        write7Seg(allOff,allOff,allOff,allOff);
      }
    }
    else if (st == VIEW_CLOCK) {
      isBeepingLocal ? write7Seg(allOff,allOff,allOff,allOff) : displayClock7Seg();
    }
    else if (st == VIEW_TIMER) {
      isBeepingLocal ? write7Seg(allOff,allOff,allOff,allOff) : displayTimer7Seg();
    }
    else if (st == VIEW_DUAL) {
      if (millis()-dualLastSwitch>=5000) { dualShowClock=!dualShowClock; dualLastSwitch=millis(); }
      if      (isBeepingLocal)  write7Seg(allOff,allOff,allOff,allOff);
      else if (dualShowClock)   displayClock7Seg();
      else                      displayTimer7Seg();
    }
    else if (st == SET_CLOCK || st == SET_TIMER) {
      displaySetting7Seg();
    }
    else if (st == CONFIRM_SET) {
      write7Seg(digitMap[rx.clkH/10],digitMap[rx.clkH%10],
                digitMap[rx.clkM/10],digitMap[rx.clkM%10]);
    }

    vTaskDelay(30/portTICK_PERIOD_MS);
  }
}