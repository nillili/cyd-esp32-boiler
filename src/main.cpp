// CYD (ESP32-2432S028R) — 보일러 제어 화면
//
// [메인 화면]  2x2 + 오른쪽 위 기어(설정) + 오른쪽 아래 릴레이 On 표시등
//  1 왼쪽위   : sleep time (초)   위쪽 터치 +, 아래쪽 터치 -
//  2 오른쪽위 : running time (초) 위쪽 터치 +, 아래쪽 터치 -
//  3 왼쪽아래 : 조도센서 값 (IO35, 아날로그 0~4095)
//  4 오른쪽아래: 수동모드 - 누르는 동안 릴레이 On (IO27) + 누른 초(0.1) 표시, 떼면 5초 유지
//                Time/Sensor 모드 - "Disable" 표시
//  Time 모드: sleep 구간엔 1번 칸 오른쪽아래, running 구간엔 2번 칸 왼쪽아래에 남은 초 작게 표시
//
// [설정 화면]  기어 터치로 진입, 같은 자리(오른쪽 위) 뒤로가기로 복귀
//  1 Time mode   : sleep time 쉬고 → running time 작동, 반복
//  2 Sensor mode : 조도 >= 기준값 이면 On, 미만이면 Off  (기준값 +/- 100, 0~4095)
//  3 Manual mode : 누르는 동안 On (기본값)
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// ---- 터치 (XPT2046) ----
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// ---- 확장 IO ----
#define LIGHT_PIN 35   // 조도센서 아날로그 입력 (P3 커넥터)
#define RELAY_PIN 27   // 릴레이 출력 (CN1 커넥터)

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();

// ---- 색 ----
#define C_BG     TFT_WHITE
#define C_BOX    0xE71C          // 연한 회색
#define C_BORDER 0x7BEF          // 진한 회색
#define C_TEXT   TFT_BLACK
#define C_LABEL  0x4208          // 라벨 회색
#define C_DIS    0x9CD3          // Disable 글자색
#define C_ON     0x07E0          // 표시등 On (초록)

// ---- 모드 ----
enum Mode { MODE_TIME = 1, MODE_SENSOR = 2, MODE_MANUAL = 3 };
enum Screen { SCREEN_MAIN, SCREEN_SETTINGS };

// ---- 상태 ----
Mode   mode        = MODE_MANUAL;
Screen screen      = SCREEN_MAIN;
int    sleepTime   = 15;
int    runningTime = 7;
int    lightValue  = -1;
int    lightLevel  = 2000;          // Sensor mode 기준값
bool   relayOn     = false;

// 수동모드 초 표시
unsigned long relayStart = 0;
unsigned long relayEnd   = 0;
int  relayTenths = -1;              // -1 = 표시 없음
const unsigned long HOLD_MS = 5000;

// Time mode
bool          timePhaseRun  = false;   // false = sleep 구간, true = running 구간
int           timeShownSec  = -1;      // 화면에 표시된 남은 초
unsigned long timePhaseStart = 0;

// ---- 레이아웃 ----
struct Box { int x, y, w, h; };
// 메인
const Box BX_SLEEP = {  6,  26, 135, 88 };
const Box BX_RUN   = {147,  26, 135, 88 };
const Box BX_LIGHT = {  6, 124, 135, 88 };
const Box BX_RELAY = {147, 124, 135, 88 };
const Box BX_GEAR  = {284,   0,  36, 44 };   // 터치 영역 (아이콘은 그 안에)
const int  LAMP_X = 302, LAMP_Y = 200, LAMP_R = 12;   // 릴레이 On 표시등
// 설정
const Box BX_MODE1 = { 10,  44, 250, 32 };
const Box BX_MODE2 = { 10,  80, 250, 32 };
const Box BX_MODE3 = { 10, 116, 250, 32 };
const Box BX_LV_M  = {150, 156,  44, 36 };   // 기준값 -
const Box BX_LV_P  = {266, 156,  44, 36 };   // 기준값 +
const Box BX_BACK  = {284,   0,  36, 44 };   // 뒤로가기 (기어와 같은 자리)

bool inBox(const Box& b, int x, int y) {
  return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h;
}

void drawFrame(const Box& b) {
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 10, C_BOX);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 10, C_BORDER);
}

// ================= 메인 화면 =================
void drawNumber(const Box& b, int v) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_TEXT, C_BOX);
  tft.setTextPadding(100);
  tft.drawNumber(v, b.x + b.w / 2, b.y + b.h / 2, 6);
  tft.setTextPadding(0);
}

void drawPlusMinus(const Box& b, bool rightSide) {
  int x = rightSide ? b.x + b.w - 16 : b.x + 16;
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_LABEL, C_BOX);
  tft.drawString("+", x, b.y + 16, 4);
  tft.drawString("-", x, b.y + b.h - 16, 4);
}

void drawGear() {
  int cx = BX_GEAR.x + 18, cy = 20;
  tft.fillCircle(cx, cy, 11, C_LABEL);
  for (int i = 0; i < 8; i++) {                 // 톱니 8개
    float a = i * PI / 4;
    tft.fillCircle(cx + (int)(13 * cos(a)), cy + (int)(13 * sin(a)), 3, C_LABEL);
  }
  tft.fillCircle(cx, cy, 5, C_BG);
}

void drawLamp() {
  if (relayOn) tft.fillCircle(LAMP_X, LAMP_Y, LAMP_R, C_ON);
  else         tft.fillCircle(LAMP_X, LAMP_Y, LAMP_R, C_BG);
  tft.drawCircle(LAMP_X, LAMP_Y, LAMP_R, C_LABEL);
}

// Time mode 남은 초: sleep 구간 = 1번 칸 오른쪽아래, running 구간 = 2번 칸 왼쪽아래
void drawCountdown(int sec) {
  const int w = 44, h = 16;
  int sx = BX_SLEEP.x + BX_SLEEP.w - 6 - w, sy = BX_SLEEP.y + BX_SLEEP.h - 4 - h;   // sleep 칸 오른쪽아래
  int rx = BX_RUN.x + 6,                    ry = BX_RUN.y + BX_RUN.h - 4 - h;       // running 칸 왼쪽아래
  tft.fillRect(sx, sy, w, h, C_BOX);
  tft.fillRect(rx, ry, w, h, C_BOX);
  if (mode != MODE_TIME || sec < 0) return;
  tft.setTextColor(C_LABEL, C_BOX);
  if (timePhaseRun) { tft.setTextDatum(BL_DATUM); tft.drawNumber(sec, rx, ry + h, 2); }
  else              { tft.setTextDatum(BR_DATUM); tft.drawNumber(sec, sx + w, sy + h, 2); }
}

void drawBox1() { drawFrame(BX_SLEEP); drawPlusMinus(BX_SLEEP, false); drawNumber(BX_SLEEP, sleepTime); }
void drawBox2() { drawFrame(BX_RUN);   drawPlusMinus(BX_RUN, true);    drawNumber(BX_RUN, runningTime); }
void drawBox3() { drawFrame(BX_LIGHT); drawNumber(BX_LIGHT, lightValue < 0 ? 0 : lightValue); }

void drawBox4() {
  drawFrame(BX_RELAY);
  tft.setTextDatum(MC_DATUM);
  int cx = BX_RELAY.x + BX_RELAY.w / 2;
  int cy = BX_RELAY.y + BX_RELAY.h / 2;
  if (mode != MODE_MANUAL) {
    tft.setTextColor(C_DIS, C_BOX);
    tft.drawString("Disable", cx, cy, 4);
    return;
  }
  tft.setTextColor(C_TEXT, C_BOX);
  if (relayTenths < 0) {
    tft.drawString(relayOn ? "On" : "Off", cx, cy, 6);
  } else {
    tft.drawString(relayOn ? "On" : "Off", cx, BX_RELAY.y + 18, 4);
    tft.setTextPadding(130);
    tft.drawFloat(relayTenths / 10.0f, 1, cx, cy + 12, 6);
    tft.setTextPadding(0);
  }
}

void drawMain() {
  tft.fillScreen(C_BG);
  tft.setTextColor(C_LABEL, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("sleep time", BX_SLEEP.x + 8, 5, 2);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("running time", BX_RUN.x + BX_RUN.w - 8, 5, 2);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Light sensor", BX_LIGHT.x + 8, BX_LIGHT.y + BX_LIGHT.h + 4, 2);
  drawGear();
  drawLamp();
  drawBox1(); drawBox2(); drawBox3(); drawBox4();
  timeShownSec = -1;            // 카운트다운 다시 그리게
}

// ================= 설정 화면 =================
void drawModeRow(const Box& b, int n, const char* name) {
  bool sel = (mode == n);
  int cy = b.y + b.h / 2;
  tft.fillRect(b.x, b.y, b.w, b.h, C_BG);
  tft.drawCircle(b.x + 14, cy, 9, C_LABEL);
  if (sel) tft.fillCircle(b.x + 14, cy, 5, C_TEXT);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(sel ? C_TEXT : C_LABEL, C_BG);
  tft.drawString(name, b.x + 34, cy, 4);
}

void drawAllModeRows() {
  drawModeRow(BX_MODE1, MODE_TIME,   "Time mode");
  drawModeRow(BX_MODE2, MODE_SENSOR, "Sensor mode");
  drawModeRow(BX_MODE3, MODE_MANUAL, "Manual mode");
}

void drawLevelValue() {
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setTextPadding(70);
  tft.drawNumber(lightLevel, 230, BX_LV_M.y + BX_LV_M.h / 2, 4);
  tft.setTextPadding(0);
}

void drawSettings() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString("Settings", 10, 8, 4);

  drawModeRow(BX_MODE1, MODE_TIME,   "Time mode");
  drawModeRow(BX_MODE2, MODE_SENSOR, "Sensor mode");
  drawModeRow(BX_MODE3, MODE_MANUAL, "Manual mode");

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString("Light level", 10, BX_LV_M.y + BX_LV_M.h / 2, 2);
  drawFrame(BX_LV_M);
  drawFrame(BX_LV_P);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_TEXT, C_BOX);
  tft.drawString("-", BX_LV_M.x + BX_LV_M.w / 2, BX_LV_M.y + BX_LV_M.h / 2, 4);
  tft.drawString("+", BX_LV_P.x + BX_LV_P.w / 2, BX_LV_P.y + BX_LV_P.h / 2, 4);
  drawLevelValue();

  // 뒤로가기 아이콘 (왼쪽 화살표, 기어와 같은 자리)
  int cx = BX_BACK.x + 18, cy = 20;
  tft.fillTriangle(cx - 13, cy, cx - 2, cy - 10, cx - 2, cy + 10, C_LABEL);
  tft.fillRect(cx - 3, cy - 3, 16, 7, C_LABEL);
}

// ================= 릴레이 =================
void relayWrite(bool on) {
  if (relayOn == on) return;
  relayOn = on;
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  Serial.printf("relay %s\n", on ? "ON" : "OFF");
  if (screen == SCREEN_MAIN) drawLamp();
}

// 수동모드: 누르는 동안 On + 초 표시
void manualSetRelay(bool on) {
  if (relayOn == on) return;
  relayWrite(on);
  if (on) { relayStart = millis(); relayEnd = 0; relayTenths = 0; }
  else    { relayEnd = millis(); }
  if (screen == SCREEN_MAIN) drawBox4();
}

void manualUpdateDisplay() {
  if (relayOn) {
    int t = (millis() - relayStart) / 100;
    if (t != relayTenths) { relayTenths = t; if (screen == SCREEN_MAIN) drawBox4(); }
  } else if (relayTenths >= 0 && relayEnd && millis() - relayEnd >= HOLD_MS) {
    relayTenths = -1; relayEnd = 0;
    if (screen == SCREEN_MAIN) drawBox4();
  }
}

// Time mode: sleep → running 반복
void timeModeStart() {
  timePhaseRun = false;
  timePhaseStart = millis();
  timeShownSec = -1;
  relayWrite(false);
}

void timeModeUpdate() {
  if (sleepTime == 0 && runningTime == 0) { relayWrite(false); return; }
  unsigned long dur = (timePhaseRun ? runningTime : sleepTime) * 1000UL;
  unsigned long elapsed = millis() - timePhaseStart;
  if (elapsed >= dur) {
    timePhaseRun = !timePhaseRun;
    timePhaseStart = millis();
    elapsed = 0;
    dur = (timePhaseRun ? runningTime : sleepTime) * 1000UL;
    Serial.printf("time mode: %s\n", timePhaseRun ? "running" : "sleep");
  }
  relayWrite(timePhaseRun);
  int remain = (dur - elapsed + 999) / 1000;          // 남은 초 (올림)
  if (remain != timeShownSec) {
    timeShownSec = remain;
    if (screen == SCREEN_MAIN) drawCountdown(remain);
  }
}

// Sensor mode
void sensorModeUpdate() {
  if (lightValue < 0) return;
  relayWrite(lightValue >= lightLevel);
}

void setMode(Mode m) {
  if (mode == m) return;
  mode = m;
  Serial.printf("mode=%d\n", mode);
  relayTenths = -1; relayEnd = 0;
  timeShownSec = -1;
  if (mode == MODE_TIME) timeModeStart();
  else relayWrite(false);
}

// ================= setup / loop =================
void setup() {
  Serial.begin(115200);

  pinMode(LIGHT_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  tft.init();
  tft.setRotation(1);
  drawMain();
}

bool wasTouched = false;
unsigned long lastLightRead = 0;

void handleMainTouch(bool touched, bool pressed, int x, int y) {
  if (pressed) {
    if (inBox(BX_GEAR, x, y)) {
      screen = SCREEN_SETTINGS;
      drawSettings();
      return;
    }
    if (inBox(BX_SLEEP, x, y)) {
      sleepTime += (y < BX_SLEEP.y + BX_SLEEP.h / 2) ? 1 : -1;
      if (sleepTime < 0) sleepTime = 0;
      drawNumber(BX_SLEEP, sleepTime);
      Serial.printf("sleepTime=%d\n", sleepTime);
    } else if (inBox(BX_RUN, x, y)) {
      runningTime += (y < BX_RUN.y + BX_RUN.h / 2) ? 1 : -1;
      if (runningTime < 0) runningTime = 0;
      drawNumber(BX_RUN, runningTime);
      Serial.printf("runningTime=%d\n", runningTime);
    }
  }
  if (mode == MODE_MANUAL) manualSetRelay(touched && inBox(BX_RELAY, x, y));
}

void handleSettingsTouch(bool pressed, int x, int y) {
  if (!pressed) return;
  if (inBox(BX_BACK, x, y)) {
    screen = SCREEN_MAIN;
    drawMain();
  } else if (inBox(BX_MODE1, x, y)) {
    setMode(MODE_TIME);   drawAllModeRows();
  } else if (inBox(BX_MODE2, x, y)) {
    setMode(MODE_SENSOR); drawAllModeRows();
  } else if (inBox(BX_MODE3, x, y)) {
    setMode(MODE_MANUAL); drawAllModeRows();
  } else if (inBox(BX_LV_M, x, y)) {
    lightLevel = max(0, lightLevel - 100);    drawLevelValue(); Serial.printf("lightLevel=%d\n", lightLevel);
  } else if (inBox(BX_LV_P, x, y)) {
    lightLevel = min(4095, lightLevel + 100); drawLevelValue(); Serial.printf("lightLevel=%d\n", lightLevel);
  }
}

void loop() {
  // ---- 터치 ----
  bool touched = ts.tirqTouched() && ts.touched();
  bool pressed = touched && !wasTouched;      // 누르는 순간
  int x = -1, y = -1;
  if (touched) {
    TS_Point p = ts.getPoint();
    x = map(p.x, 200, 3700, 0, 320);
    y = map(p.y, 240, 3800, 0, 240);
  }
  if (screen == SCREEN_MAIN) handleMainTouch(touched, pressed, x, y);
  else                       handleSettingsTouch(pressed, x, y);
  wasTouched = touched;

  // ---- 조도센서 200ms 마다 ----
  if (millis() - lastLightRead > 200) {
    lastLightRead = millis();
    int v = analogRead(LIGHT_PIN);
    if (v != lightValue) {
      lightValue = v;
      if (screen == SCREEN_MAIN) drawNumber(BX_LIGHT, lightValue);
    }
  }

  // ---- 모드별 릴레이 제어 ----
  switch (mode) {
    case MODE_TIME:   timeModeUpdate();   break;
    case MODE_SENSOR: sensorModeUpdate(); break;
    case MODE_MANUAL: manualUpdateDisplay(); break;
  }

  delay(20);
}
