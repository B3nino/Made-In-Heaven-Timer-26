// ============================================================
//  ESP-1  —  BRAIN UNIT  (rev-7 - 24/7 Stable)
// ============================================================

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "time.h"

#define ESPNOW_CHANNEL 1

uint8_t esp2Mac[] = {0x1C, 0x69, 0x20, 0x31, 0x06, 0x20};

const char* ssids[]     = { "TUPT_Electronics Project", "TUPT_Electronics Faculty", "HUAWEI-2.4G-4uJS" };
const char* passwords[] = { "1qAz2wSx", "TUPTElec", "dZZ3Ud3x" };
const int NUM_NETWORKS = 3;

// --- KEYPAD ---
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS]  = {26, 25, 33, 32};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- STATES ---
enum State {
  WELCOME, VIEW_CLOCK, VIEW_TIMER, VIEW_DUAL,
  SET_CLOCK, SET_TIMER, CONFIRM_SET, MADE_IN_HEAVEN
};
volatile State currentState = WELCOME;

// --- GLOBALS & SPINLOCKS ---
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED; // Protects shared state across cores

volatile int  clkH = 0, clkM = 0, clkS = 0;
volatile long timerSeconds      = 0;
long          lastBeepMilestone = -1;
volatile bool isTimerRunning    = false;
volatile bool alarmActive       = false;
volatile bool isConnected       = false;
volatile bool manualOverride    = false;
volatile bool isBeeping         = false;

int  configDigits[6] = {0,0,0,0,0,0};
int  cursor       = 0;
int  settingPhase = 0;   

bool forceSeconds   = false;
bool ntpSyncedToday = false;

bool  heavenActive    = false;
bool  heavenConfirmed = false;
float heavenSpeed     = 1.0f;
unsigned long heavenStartTime = 0;
const char* members[] = {
  "Baloloy","Bangcuyo","Baratbate",
  "Barcelona","Bendijo","Navarro","Rendon"
};

const char* activePattern = NULL;
int         activeRepeats = 0;

// ── Packet ────────────────────────────────────────────────────
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

QueueHandle_t sendQueue;
uint32_t seqNum = 0;

static uint8_t encodePattern(const char* p) {
  if (!p) return 0;
  if (strstr(p, ".-.-"))             return 7;
  if (strstr(p, "- .  - . ."))       return 6;
  if (strstr(p, "- . .  - . ."))     return 5;
  if (strstr(p, "- . . .  - . . .")) return 4;
  if (strstr(p, ". .  . ."))         return 3;
  if (strstr(p, ". . .    . . ."))   return 2;
  if (strstr(p, ". . . .  . . . .")) return 1;
  if (strstr(p, "- -  - -"))         return 8;
  return 0;
}

void triggerAlert(const char* pattern, int repeats = 1) {
  portENTER_CRITICAL(&stateMux);
  activePattern = pattern;
  activeRepeats = repeats;
  portEXIT_CRITICAL(&stateMux);
}

void checkTimerMilestones() {
  if (!isTimerRunning || timerSeconds == lastBeepMilestone) return;
  if      (timerSeconds == 3600) { triggerAlert(". . . .  . . . .", 1); lastBeepMilestone = 3600; }
  else if (timerSeconds == 2700) { triggerAlert(". . .    . . .", 1);   lastBeepMilestone = 2700; }
  else if (timerSeconds == 1800) { triggerAlert(". .  . .", 1);         lastBeepMilestone = 1800; }
  else if (timerSeconds == 900)  { triggerAlert("- . . .  - . . .", 1); lastBeepMilestone = 900;  }
  else if (timerSeconds == 600)  { triggerAlert("- . .  - . .", 1);     lastBeepMilestone = 600;  }
  else if (timerSeconds == 300)  { triggerAlert("- .  - . .", 1);       lastBeepMilestone = 300;  }
  else if (timerSeconds == 60)   { triggerAlert(".-.-  .- .-", 2);      lastBeepMilestone = 60;   }
}

void onDataSent(const uint8_t* mac, esp_now_send_status_t status) {}

void addPeer() {
  if (esp_now_is_peer_exist(esp2Mac)) esp_now_del_peer(esp2Mac);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, esp2Mac, 6);
  peer.channel = (WiFi.status() == WL_CONNECTED) ? WiFi.channel() : ESPNOW_CHANNEL;
  peer.encrypt = false;
  peer.ifidx   = WIFI_IF_STA;
  esp_now_add_peer(&peer);
}

void enqueueState(char key = 0) {
  StatePacket p;
  bool blink = (millis() % 1000 < 500);
  
  portENTER_CRITICAL(&stateMux);
  p.state           = (uint8_t)currentState;
  p.clkH            = clkH;  p.clkM = clkM;  p.clkS = clkS;
  p.timerSeconds    = timerSeconds;
  p.isTimerRunning  = isTimerRunning;
  p.alarmActive     = alarmActive;
  p.isBeeping       = isBeeping;
  p.heavenActive    = heavenActive;
  p.heavenConfirmed = heavenConfirmed;
  p.forceSeconds    = forceSeconds;
  p.manualOverride  = manualOverride;
  p.heavenSpeed     = heavenSpeed;
  p.alertPattern    = encodePattern(activePattern);
  p.alertRepeats    = (uint8_t)activeRepeats;
  p.keyPressed      = key;
  p.settingCursor   = (uint8_t)cursor;
  p.settingPhase    = (uint8_t)settingPhase;
  p.blinkState      = blink;
  p.seqNum          = ++seqNum;
  for (int i = 0; i < 6; i++) p.configDigits[i] = (uint8_t)configDigits[i];
  portEXIT_CRITICAL(&stateMux);
  
  xQueueSend(sendQueue, &p, 0);
}

void enterSettingMode(State s) {
  portENTER_CRITICAL(&stateMux);
  currentState  = s;
  cursor        = 0;
  settingPhase  = 0;
  for (int i = 0; i < 6; i++) configDigits[i] = 0;
  portEXIT_CRITICAL(&stateMux);
}

void saveManualTime() {
  portENTER_CRITICAL(&stateMux);
  clkH = configDigits[0]*10 + configDigits[1];
  clkM = configDigits[2]*10 + configDigits[3];
  clkS = configDigits[4]*10 + configDigits[5];
  portEXIT_CRITICAL(&stateMux);
}

// =============================================================
//  SETUP
// =============================================================
void setup() {
  Serial.begin(115200);
  Wire.begin(22, 23);
  lcd.init(); lcd.backlight();
  keypad.setDebounceTime(50);
  keypad.setHoldTime(660);

  sendQueue = xQueueCreate(10, sizeof(StatePacket));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() == ESP_OK) {
    esp_now_register_send_cb(onDataSent);
    addPeer();
  }

  // Initial Config Time (Prevents memory leaks from calling it in loop)
  configTime(28800, 0, "pool.ntp.org");
  
  xTaskCreatePinnedToCore(TaskLogic,   "TaskLogic",   8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskDisplay, "TaskDisplay", 8192, NULL, 1, NULL, 0);
}

void loop() { vTaskDelete(NULL); }

// =============================================================
//  TASK: LOGIC  (Core 1)
// =============================================================
void TaskLogic(void* pvParameters) {
  unsigned long lastTick = millis();
  for (;;) {
    char key = keypad.getKey();

    if (key) {
      portENTER_CRITICAL(&stateMux);
      if (alarmActive) {
        alarmActive   = false;
        activePattern = NULL;
        isBeeping     = false;
      }

      switch (currentState) {
        case VIEW_CLOCK:
          if      (key == 'D') currentState = VIEW_TIMER;
          else if (key == 'C') { portEXIT_CRITICAL(&stateMux); enterSettingMode(SET_CLOCK); portENTER_CRITICAL(&stateMux); }
          else if (key == 'B' && manualOverride) manualOverride = false;
          else if (key == '#') forceSeconds = !forceSeconds;
          break;

        case VIEW_TIMER:
          if      (key == 'D') currentState = VIEW_DUAL;
          else if (key == 'C') { isTimerRunning = false; portEXIT_CRITICAL(&stateMux); enterSettingMode(SET_TIMER); portENTER_CRITICAL(&stateMux); }
          else if (key == 'A') isTimerRunning = !isTimerRunning;
          else if (key == '#') forceSeconds = !forceSeconds;
          break;

        case VIEW_DUAL:
          if      (key == 'D') { currentState = MADE_IN_HEAVEN; heavenConfirmed = false; }
          else if (key == '#') forceSeconds = !forceSeconds;
          break;

        case MADE_IN_HEAVEN:
          if (key == 'B' || key == 'D') {
            heavenActive = false; heavenConfirmed = false; heavenSpeed = 1.0f;  
            currentState = (key == 'B') ? MADE_IN_HEAVEN : VIEW_CLOCK;
          } else if (!heavenConfirmed && key == 'A') {
            heavenConfirmed = true; heavenStartTime = millis();
          }
          break;

        case SET_CLOCK:
        case SET_TIMER: {
          int phaseStart = settingPhase * 2;
          int phaseEnd   = phaseStart + 1;
          if (key >= '0' && key <= '9') {
            configDigits[cursor] = key - '0';
            if (cursor < phaseEnd) cursor++;
          }
          else if (key == '*') { if (cursor > phaseStart) cursor--; }
          else if (key == '#') { if (cursor < phaseEnd)   cursor++; }
          else if (key == 'B') {
            for (int i = 0; i < 6; i++) configDigits[i] = 0;
            cursor = 0; settingPhase = 0;
          }
          else if (key == 'C') {
            if (settingPhase < 2) {
              settingPhase++;
              cursor = settingPhase * 2;
            } else {
              if (currentState == SET_CLOCK) {
                currentState = CONFIRM_SET;
              } else {
                long h = (long)configDigits[0]*10 + configDigits[1];
                long m = (long)configDigits[2]*10 + configDigits[3];
                long s = (long)configDigits[4]*10 + configDigits[5];
                timerSeconds = h*3600L + m*60L + s;
                currentState = VIEW_TIMER;
              }
            }
          }
          break;
        }

        case CONFIRM_SET:
          if      (key == 'A') { portEXIT_CRITICAL(&stateMux); saveManualTime(); portENTER_CRITICAL(&stateMux); manualOverride = true; currentState = VIEW_CLOCK; }
          else if (key == 'B')   currentState = VIEW_CLOCK;
          break;
      }
      portEXIT_CRITICAL(&stateMux);
      enqueueState(key);
    }

    // Clock tick
    unsigned long now = millis();
    unsigned long tickMs = heavenActive ? 50UL : 1000UL;
    
    if (now - lastTick >= tickMs) {
      lastTick = now;
      portENTER_CRITICAL(&stateMux);
      int inc = heavenActive ? (int)heavenSpeed : 1;
      clkS += inc;
      while (clkS >= 60) { clkS -= 60; clkM++; }
      while (clkM >= 60) { clkM -= 60; clkH++; }
      while (clkH >= 24)  clkH -= 24;

      if (isTimerRunning && timerSeconds > 0) {
        timerSeconds--;
        checkTimerMilestones();
        if (timerSeconds == 0) { alarmActive = true; isTimerRunning = false; }
      }
      if (heavenActive && heavenSpeed < 60.0f) heavenSpeed *= 1.05f;
      portEXIT_CRITICAL(&stateMux);
      enqueueState();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// =============================================================
//  TASK: DISPLAY + SEND  (Core 0)
// =============================================================
void TaskDisplay(void* pvParameters) {
  unsigned long lastWifiCheck  = 0;
  unsigned long lastHeartbeat  = 0;
  int wifiIdx = 0;

  for (;;) {
    StatePacket outPkt;
    while (xQueueReceive(sendQueue, &outPkt, 0) == pdTRUE) {
      esp_now_send(esp2Mac, (uint8_t*)&outPkt, sizeof(outPkt));
    }

    unsigned long now = millis();
    if (now - lastHeartbeat >= 50) {
      lastHeartbeat = now;
      enqueueState();
    }

    // Safe WiFi keep-alive (prevents memory leak crashes)
    if (now - lastWifiCheck > 15000) {
      lastWifiCheck = now;
      if (WiFi.status() != WL_CONNECTED) {
        isConnected = false;
        WiFi.disconnect();
        WiFi.begin(ssids[wifiIdx], passwords[wifiIdx]);
        wifiIdx = (wifiIdx + 1) % NUM_NETWORKS;
      } else if (!isConnected) {
        isConnected = true;
        esp_wifi_set_channel(WiFi.channel(), WIFI_SECOND_CHAN_NONE);
        addPeer();
      }
    }

    // NTP sync
    if (isConnected && !heavenActive && !manualOverride) {
      if (clkH == 0 && clkM == 0 && clkS == 0 && !ntpSyncedToday) {
         ntpSyncedToday = true; // Syncs natively via lwIP daily once configured
      } else if (clkH != 0) {
         ntpSyncedToday = false; 
      }
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 10)) {
        portENTER_CRITICAL(&stateMux);
        clkH = timeinfo.tm_hour; clkM = timeinfo.tm_min; clkS = timeinfo.tm_sec;
        portEXIT_CRITICAL(&stateMux);
      }
    }

    bool blink = (millis() % 1000 < 500);
    portENTER_CRITICAL(&stateMux);
    State localState = currentState;
    bool localAlarm = alarmActive;
    portEXIT_CRITICAL(&stateMux);

    if (localState == WELCOME) {
      lcd.setCursor(0,0); lcd.print("    WELCOME USER   ");
      lcd.setCursor(0,1); lcd.print(" SYSTEM STARTING");
      if (millis() > 3000) { 
        lcd.clear(); 
        portENTER_CRITICAL(&stateMux); currentState = VIEW_CLOCK; portEXIT_CRITICAL(&stateMux); 
      }
    }
    else if (localState == MADE_IN_HEAVEN) { handleHeavenDisplay(blink); }
    else if (localAlarm) {
      lcd.setCursor(0,0); lcd.print("    TIME'S UP!    ");
      lcd.setCursor(0,1); lcd.print(" PRESS ANY KEY  ");
    }
    else { updateLCD(blink); }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// =============================================================
//  LCD HELPERS (Optimized & Safe)
// =============================================================
void handleHeavenDisplay(bool blink) {
  if (!heavenConfirmed) {
    lcd.setCursor(0,0); lcd.print(" MADE IN HEAVEN ");
    lcd.setCursor(0,1); lcd.print(" [A:YES] [B:NO] ");
  } else {
    unsigned long elapsed = millis() - heavenStartTime;
    if (elapsed < 5000) {
      lcd.setCursor(0,0); lcd.print("Humans ought to ");
      lcd.setCursor(0,1); lcd.print("lead to heaven. ");
    } else if (elapsed < 10000) {
      lcd.setCursor(0,0); lcd.print("I am grateful...");
      lcd.setCursor(0,1); lcd.print(" - Enrico Pucci ");
    } else if (elapsed < 24000) {
      int idx = ((elapsed - 10000) / 2000) % 7;
      lcd.setCursor(0,0); lcd.print("Thank You:      ");
      char buf[17];
      snprintf(buf, sizeof(buf), "%-16s", members[idx]);
      lcd.setCursor(0,1); lcd.print(buf);
    } else if (elapsed < 29000) {
      lcd.setCursor(0,0); lcd.print("Engr. Jediah    ");
      lcd.setCursor(0,1); lcd.print("Puertollano     ");
    } else {
      portENTER_CRITICAL(&stateMux); heavenActive = true; portEXIT_CRITICAL(&stateMux);
      lcd.setCursor(0,0); lcd.print(" MADE IN HEAVEN ");
      char buf[17];
      snprintf(buf, sizeof(buf), "    %02d:%02d:%02d    ", clkH, clkM, clkS);
      lcd.setCursor(0,1); lcd.print(buf);
    }
  }
}

void updateLCD(bool blink) {
  char line1[17], line2[17];
  
  portENTER_CRITICAL(&stateMux); // Snapshot values safely
  State st = currentState;
  bool over = manualOverride;
  int h = clkH, m = clkM, s = clkS;
  long tmr = timerSeconds;
  bool tmrRun = isTimerRunning;
  int phase = settingPhase;
  int cur = cursor;
  int d[6]; for(int i=0;i<6;i++) d[i] = configDigits[i];
  portEXIT_CRITICAL(&stateMux);

  switch (st) {
    case VIEW_CLOCK:
      snprintf(line1, sizeof(line1), over ? "MANUAL [B:AUTO] " : "CLOCK: REGIONAL ");
      snprintf(line2, sizeof(line2), "    %02d:%02d:%02d    ", h, m, s);
      break;
    case VIEW_TIMER:
      snprintf(line1, sizeof(line1), "TIMER: %s  ", tmrRun ? "RUNNING " : "PAUSED  ");
      snprintf(line2, sizeof(line2), "    %02ld:%02ld:%02ld    ", tmr/3600, (tmr%3600)/60, tmr%60);
      break;
    case VIEW_DUAL:
      snprintf(line1, sizeof(line1), "CLK: %02d:%02d:%02d   ", h, m, s);
      snprintf(line2, sizeof(line2), "TMR: %02ld:%02ld:%02ld   ", tmr/3600, (tmr%3600)/60, tmr%60);
      break;
    case SET_CLOCK:
    case SET_TIMER: {
      const char* ph[] = {"HRS", "MIN", "SEC"};
      snprintf(line1, sizeof(line1), "%s %s  [B:RST]   ", st == SET_CLOCK ? "CLK" : "TMR", ph[phase]);
      
      // Buffer safe string replacement (fixes crash risk!)
      char disp[6];
      for (int i = 0; i < 6; i++) {
        disp[i] = (i == cur && !blink) ? '_' : ('0' + d[i]);
      }
      snprintf(line2, sizeof(line2), "    %c%c:%c%c:%c%c    ", disp[0], disp[1], disp[2], disp[3], disp[4], disp[5]);
      break;
    }
    case CONFIRM_SET:
      snprintf(line1, sizeof(line1), " APPLY CHANGES? ");
      snprintf(line2, sizeof(line2), " [A:YES] [B:NO] ");
      break;
    default: return;
  }
  lcd.setCursor(0,0); lcd.print(line1);
  lcd.setCursor(0,1); lcd.print(line2);
}// ============================================================
//  ESP-1  —  BRAIN UNIT  (rev-7 - 24/7 Stable)
// ============================================================

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "time.h"

#define ESPNOW_CHANNEL 1

uint8_t esp2Mac[] = {0x1C, 0x69, 0x20, 0x31, 0x06, 0x20};

const char* ssids[]     = { "TUPT_Electronics Project", "TUPT_Electronics Faculty", "HUAWEI-2.4G-4uJS" };
const char* passwords[] = { "1qAz2wSx", "TUPTElec", "dZZ3Ud3x" };
const int NUM_NETWORKS = 3;

// --- KEYPAD ---
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS]  = {26, 25, 33, 32};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- STATES ---
enum State {
  WELCOME, VIEW_CLOCK, VIEW_TIMER, VIEW_DUAL,
  SET_CLOCK, SET_TIMER, CONFIRM_SET, MADE_IN_HEAVEN
};
volatile State currentState = WELCOME;

// --- GLOBALS & SPINLOCKS ---
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED; // Protects shared state across cores

volatile int  clkH = 0, clkM = 0, clkS = 0;
volatile long timerSeconds      = 0;
long          lastBeepMilestone = -1;
volatile bool isTimerRunning    = false;
volatile bool alarmActive       = false;
volatile bool isConnected       = false;
volatile bool manualOverride    = false;
volatile bool isBeeping         = false;

int  configDigits[6] = {0,0,0,0,0,0};
int  cursor       = 0;
int  settingPhase = 0;   

bool forceSeconds   = false;
bool ntpSyncedToday = false;

bool  heavenActive    = false;
bool  heavenConfirmed = false;
float heavenSpeed     = 1.0f;
unsigned long heavenStartTime = 0;
const char* members[] = {
  "Baloloy","Bangcuyo","Baratbate",
  "Barcelona","Bendijo","Navarro","Rendon"
};

const char* activePattern = NULL;
int         activeRepeats = 0;

// ── Packet ────────────────────────────────────────────────────
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

QueueHandle_t sendQueue;
uint32_t seqNum = 0;

static uint8_t encodePattern(const char* p) {
  if (!p) return 0;
  if (strstr(p, ".-.-"))             return 7;
  if (strstr(p, "- .  - . ."))       return 6;
  if (strstr(p, "- . .  - . ."))     return 5;
  if (strstr(p, "- . . .  - . . .")) return 4;
  if (strstr(p, ". .  . ."))         return 3;
  if (strstr(p, ". . .    . . ."))   return 2;
  if (strstr(p, ". . . .  . . . .")) return 1;
  if (strstr(p, "- -  - -"))         return 8;
  return 0;
}

void triggerAlert(const char* pattern, int repeats = 1) {
  portENTER_CRITICAL(&stateMux);
  activePattern = pattern;
  activeRepeats = repeats;
  portEXIT_CRITICAL(&stateMux);
}

void checkTimerMilestones() {
  if (!isTimerRunning || timerSeconds == lastBeepMilestone) return;
  if      (timerSeconds == 3600) { triggerAlert(". . . .  . . . .", 1); lastBeepMilestone = 3600; }
  else if (timerSeconds == 2700) { triggerAlert(". . .    . . .", 1);   lastBeepMilestone = 2700; }
  else if (timerSeconds == 1800) { triggerAlert(". .  . .", 1);         lastBeepMilestone = 1800; }
  else if (timerSeconds == 900)  { triggerAlert("- . . .  - . . .", 1); lastBeepMilestone = 900;  }
  else if (timerSeconds == 600)  { triggerAlert("- . .  - . .", 1);     lastBeepMilestone = 600;  }
  else if (timerSeconds == 300)  { triggerAlert("- .  - . .", 1);       lastBeepMilestone = 300;  }
  else if (timerSeconds == 60)   { triggerAlert(".-.-  .- .-", 2);      lastBeepMilestone = 60;   }
}

void onDataSent(const uint8_t* mac, esp_now_send_status_t status) {}

void addPeer() {
  if (esp_now_is_peer_exist(esp2Mac)) esp_now_del_peer(esp2Mac);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, esp2Mac, 6);
  peer.channel = (WiFi.status() == WL_CONNECTED) ? WiFi.channel() : ESPNOW_CHANNEL;
  peer.encrypt = false;
  peer.ifidx   = WIFI_IF_STA;
  esp_now_add_peer(&peer);
}

void enqueueState(char key = 0) {
  StatePacket p;
  bool blink = (millis() % 1000 < 500);
  
  portENTER_CRITICAL(&stateMux);
  p.state           = (uint8_t)currentState;
  p.clkH            = clkH;  p.clkM = clkM;  p.clkS = clkS;
  p.timerSeconds    = timerSeconds;
  p.isTimerRunning  = isTimerRunning;
  p.alarmActive     = alarmActive;
  p.isBeeping       = isBeeping;
  p.heavenActive    = heavenActive;
  p.heavenConfirmed = heavenConfirmed;
  p.forceSeconds    = forceSeconds;
  p.manualOverride  = manualOverride;
  p.heavenSpeed     = heavenSpeed;
  p.alertPattern    = encodePattern(activePattern);
  p.alertRepeats    = (uint8_t)activeRepeats;
  p.keyPressed      = key;
  p.settingCursor   = (uint8_t)cursor;
  p.settingPhase    = (uint8_t)settingPhase;
  p.blinkState      = blink;
  p.seqNum          = ++seqNum;
  for (int i = 0; i < 6; i++) p.configDigits[i] = (uint8_t)configDigits[i];
  portEXIT_CRITICAL(&stateMux);
  
  xQueueSend(sendQueue, &p, 0);
}

void enterSettingMode(State s) {
  portENTER_CRITICAL(&stateMux);
  currentState  = s;
  cursor        = 0;
  settingPhase  = 0;
  for (int i = 0; i < 6; i++) configDigits[i] = 0;
  portEXIT_CRITICAL(&stateMux);
}

void saveManualTime() {
  portENTER_CRITICAL(&stateMux);
  clkH = configDigits[0]*10 + configDigits[1];
  clkM = configDigits[2]*10 + configDigits[3];
  clkS = configDigits[4]*10 + configDigits[5];
  portEXIT_CRITICAL(&stateMux);
}

// =============================================================
//  SETUP
// =============================================================
void setup() {
  Serial.begin(115200);
  Wire.begin(22, 23);
  lcd.init(); lcd.backlight();
  keypad.setDebounceTime(50);
  keypad.setHoldTime(660);

  sendQueue = xQueueCreate(10, sizeof(StatePacket));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() == ESP_OK) {
    esp_now_register_send_cb(onDataSent);
    addPeer();
  }

  // Initial Config Time (Prevents memory leaks from calling it in loop)
  configTime(28800, 0, "pool.ntp.org");
  
  xTaskCreatePinnedToCore(TaskLogic,   "TaskLogic",   8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskDisplay, "TaskDisplay", 8192, NULL, 1, NULL, 0);
}

void loop() { vTaskDelete(NULL); }

// =============================================================
//  TASK: LOGIC  (Core 1)
// =============================================================
void TaskLogic(void* pvParameters) {
  unsigned long lastTick = millis();
  for (;;) {
    char key = keypad.getKey();

    if (key) {
      portENTER_CRITICAL(&stateMux);
      if (alarmActive) {
        alarmActive   = false;
        activePattern = NULL;
        isBeeping     = false;
      }

      switch (currentState) {
        case VIEW_CLOCK:
          if      (key == 'D') currentState = VIEW_TIMER;
          else if (key == 'C') { portEXIT_CRITICAL(&stateMux); enterSettingMode(SET_CLOCK); portENTER_CRITICAL(&stateMux); }
          else if (key == 'B' && manualOverride) manualOverride = false;
          else if (key == '#') forceSeconds = !forceSeconds;
          break;

        case VIEW_TIMER:
          if      (key == 'D') currentState = VIEW_DUAL;
          else if (key == 'C') { isTimerRunning = false; portEXIT_CRITICAL(&stateMux); enterSettingMode(SET_TIMER); portENTER_CRITICAL(&stateMux); }
          else if (key == 'A') isTimerRunning = !isTimerRunning;
          else if (key == '#') forceSeconds = !forceSeconds;
          break;

        case VIEW_DUAL:
          if      (key == 'D') { currentState = MADE_IN_HEAVEN; heavenConfirmed = false; }
          else if (key == '#') forceSeconds = !forceSeconds;
          break;

        case MADE_IN_HEAVEN:
          if (key == 'B' || key == 'D') {
            heavenActive = false; heavenConfirmed = false; heavenSpeed = 1.0f;  
            currentState = (key == 'B') ? MADE_IN_HEAVEN : VIEW_CLOCK;
          } else if (!heavenConfirmed && key == 'A') {
            heavenConfirmed = true; heavenStartTime = millis();
          }
          break;

        case SET_CLOCK:
        case SET_TIMER: {
          int phaseStart = settingPhase * 2;
          int phaseEnd   = phaseStart + 1;
          if (key >= '0' && key <= '9') {
            configDigits[cursor] = key - '0';
            if (cursor < phaseEnd) cursor++;
          }
          else if (key == '*') { if (cursor > phaseStart) cursor--; }
          else if (key == '#') { if (cursor < phaseEnd)   cursor++; }
          else if (key == 'B') {
            for (int i = 0; i < 6; i++) configDigits[i] = 0;
            cursor = 0; settingPhase = 0;
          }
          else if (key == 'C') {
            if (settingPhase < 2) {
              settingPhase++;
              cursor = settingPhase * 2;
            } else {
              if (currentState == SET_CLOCK) {
                currentState = CONFIRM_SET;
              } else {
                long h = (long)configDigits[0]*10 + configDigits[1];
                long m = (long)configDigits[2]*10 + configDigits[3];
                long s = (long)configDigits[4]*10 + configDigits[5];
                timerSeconds = h*3600L + m*60L + s;
                currentState = VIEW_TIMER;
              }
            }
          }
          break;
        }

        case CONFIRM_SET:
          if      (key == 'A') { portEXIT_CRITICAL(&stateMux); saveManualTime(); portENTER_CRITICAL(&stateMux); manualOverride = true; currentState = VIEW_CLOCK; }
          else if (key == 'B')   currentState = VIEW_CLOCK;
          break;
      }
      portEXIT_CRITICAL(&stateMux);
      enqueueState(key);
    }

    // Clock tick
    unsigned long now = millis();
    unsigned long tickMs = heavenActive ? 50UL : 1000UL;
    
    if (now - lastTick >= tickMs) {
      lastTick = now;
      portENTER_CRITICAL(&stateMux);
      int inc = heavenActive ? (int)heavenSpeed : 1;
      clkS += inc;
      while (clkS >= 60) { clkS -= 60; clkM++; }
      while (clkM >= 60) { clkM -= 60; clkH++; }
      while (clkH >= 24)  clkH -= 24;

      if (isTimerRunning && timerSeconds > 0) {
        timerSeconds--;
        checkTimerMilestones();
        if (timerSeconds == 0) { alarmActive = true; isTimerRunning = false; }
      }
      if (heavenActive && heavenSpeed < 60.0f) heavenSpeed *= 1.05f;
      portEXIT_CRITICAL(&stateMux);
      enqueueState();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// =============================================================
//  TASK: DISPLAY + SEND  (Core 0)
// =============================================================
void TaskDisplay(void* pvParameters) {
  unsigned long lastWifiCheck  = 0;
  unsigned long lastHeartbeat  = 0;
  int wifiIdx = 0;

  for (;;) {
    StatePacket outPkt;
    while (xQueueReceive(sendQueue, &outPkt, 0) == pdTRUE) {
      esp_now_send(esp2Mac, (uint8_t*)&outPkt, sizeof(outPkt));
    }

    unsigned long now = millis();
    if (now - lastHeartbeat >= 50) {
      lastHeartbeat = now;
      enqueueState();
    }

    // Safe WiFi keep-alive (prevents memory leak crashes)
    if (now - lastWifiCheck > 15000) {
      lastWifiCheck = now;
      if (WiFi.status() != WL_CONNECTED) {
        isConnected = false;
        WiFi.disconnect();
        WiFi.begin(ssids[wifiIdx], passwords[wifiIdx]);
        wifiIdx = (wifiIdx + 1) % NUM_NETWORKS;
      } else if (!isConnected) {
        isConnected = true;
        esp_wifi_set_channel(WiFi.channel(), WIFI_SECOND_CHAN_NONE);
        addPeer();
      }
    }

    // NTP sync
    if (isConnected && !heavenActive && !manualOverride) {
      if (clkH == 0 && clkM == 0 && clkS == 0 && !ntpSyncedToday) {
         ntpSyncedToday = true; // Syncs natively via lwIP daily once configured
      } else if (clkH != 0) {
         ntpSyncedToday = false; 
      }
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 10)) {
        portENTER_CRITICAL(&stateMux);
        clkH = timeinfo.tm_hour; clkM = timeinfo.tm_min; clkS = timeinfo.tm_sec;
        portEXIT_CRITICAL(&stateMux);
      }
    }

    bool blink = (millis() % 1000 < 500);
    portENTER_CRITICAL(&stateMux);
    State localState = currentState;
    bool localAlarm = alarmActive;
    portEXIT_CRITICAL(&stateMux);

    if (localState == WELCOME) {
      lcd.setCursor(0,0); lcd.print("    WELCOME USER   ");
      lcd.setCursor(0,1); lcd.print(" SYSTEM STARTING");
      if (millis() > 3000) { 
        lcd.clear(); 
        portENTER_CRITICAL(&stateMux); currentState = VIEW_CLOCK; portEXIT_CRITICAL(&stateMux); 
      }
    }
    else if (localState == MADE_IN_HEAVEN) { handleHeavenDisplay(blink); }
    else if (localAlarm) {
      lcd.setCursor(0,0); lcd.print("    TIME'S UP!    ");
      lcd.setCursor(0,1); lcd.print(" PRESS ANY KEY  ");
    }
    else { updateLCD(blink); }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// =============================================================
//  LCD HELPERS (Optimized & Safe)
// =============================================================
void handleHeavenDisplay(bool blink) {
  if (!heavenConfirmed) {
    lcd.setCursor(0,0); lcd.print(" MADE IN HEAVEN ");
    lcd.setCursor(0,1); lcd.print(" [A:YES] [B:NO] ");
  } else {
    unsigned long elapsed = millis() - heavenStartTime;
    if (elapsed < 5000) {
      lcd.setCursor(0,0); lcd.print("Humans ought to ");
      lcd.setCursor(0,1); lcd.print("lead to heaven. ");
    } else if (elapsed < 10000) {
      lcd.setCursor(0,0); lcd.print("I am grateful...");
      lcd.setCursor(0,1); lcd.print(" - Enrico Pucci ");
    } else if (elapsed < 24000) {
      int idx = ((elapsed - 10000) / 2000) % 7;
      lcd.setCursor(0,0); lcd.print("Thank You:      ");
      char buf[17];
      snprintf(buf, sizeof(buf), "%-16s", members[idx]);
      lcd.setCursor(0,1); lcd.print(buf);
    } else if (elapsed < 29000) {
      lcd.setCursor(0,0); lcd.print("Engr. Jediah    ");
      lcd.setCursor(0,1); lcd.print("Puertollano     ");
    } else {
      portENTER_CRITICAL(&stateMux); heavenActive = true; portEXIT_CRITICAL(&stateMux);
      lcd.setCursor(0,0); lcd.print(" MADE IN HEAVEN ");
      char buf[17];
      snprintf(buf, sizeof(buf), "    %02d:%02d:%02d    ", clkH, clkM, clkS);
      lcd.setCursor(0,1); lcd.print(buf);
    }
  }
}

void updateLCD(bool blink) {
  char line1[17], line2[17];
  
  portENTER_CRITICAL(&stateMux); // Snapshot values safely
  State st = currentState;
  bool over = manualOverride;
  int h = clkH, m = clkM, s = clkS;
  long tmr = timerSeconds;
  bool tmrRun = isTimerRunning;
  int phase = settingPhase;
  int cur = cursor;
  int d[6]; for(int i=0;i<6;i++) d[i] = configDigits[i];
  portEXIT_CRITICAL(&stateMux);

  switch (st) {
    case VIEW_CLOCK:
      snprintf(line1, sizeof(line1), over ? "MANUAL [B:AUTO] " : "CLOCK: REGIONAL ");
      snprintf(line2, sizeof(line2), "    %02d:%02d:%02d    ", h, m, s);
      break;
    case VIEW_TIMER:
      snprintf(line1, sizeof(line1), "TIMER: %s  ", tmrRun ? "RUNNING " : "PAUSED  ");
      snprintf(line2, sizeof(line2), "    %02ld:%02ld:%02ld    ", tmr/3600, (tmr%3600)/60, tmr%60);
      break;
    case VIEW_DUAL:
      snprintf(line1, sizeof(line1), "CLK: %02d:%02d:%02d   ", h, m, s);
      snprintf(line2, sizeof(line2), "TMR: %02ld:%02ld:%02ld   ", tmr/3600, (tmr%3600)/60, tmr%60);
      break;
    case SET_CLOCK:
    case SET_TIMER: {
      const char* ph[] = {"HRS", "MIN", "SEC"};
      snprintf(line1, sizeof(line1), "%s %s  [B:RST]   ", st == SET_CLOCK ? "CLK" : "TMR", ph[phase]);
      
      // Buffer safe string replacement (fixes crash risk!)
      char disp[6];
      for (int i = 0; i < 6; i++) {
        disp[i] = (i == cur && !blink) ? '_' : ('0' + d[i]);
      }
      snprintf(line2, sizeof(line2), "    %c%c:%c%c:%c%c    ", disp[0], disp[1], disp[2], disp[3], disp[4], disp[5]);
      break;
    }
    case CONFIRM_SET:
      snprintf(line1, sizeof(line1), " APPLY CHANGES? ");
      snprintf(line2, sizeof(line2), " [A:YES] [B:NO] ");
      break;
    default: return;
  }
  lcd.setCursor(0,0); lcd.print(line1);
  lcd.setCursor(0,1); lcd.print(line2);
}