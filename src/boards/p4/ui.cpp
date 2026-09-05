#include "ui.h"

#include <Arduino_GFX_Library.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include "config.h"
#include "hw.h"
#include "log.h"
#include "stations.h"

namespace {

constexpr int16_t W = LCD_W;
constexpr int16_t H = LCD_H;

// ── 색 ────────────────────────────────────────────────────────────
// 강조는 바늘과 현재 채널에만. 화면이 커도 색이 많으면 산만하다.
constexpr uint16_t COL_BG       = 0x0000;
constexpr uint16_t COL_INK      = 0xFFFF;
constexpr uint16_t COL_DIM      = 0x8410;
constexpr uint16_t COL_RULE     = 0x39E7;
constexpr uint16_t COL_NEEDLE   = 0xFB00;
constexpr uint16_t COL_OK       = 0x2E68;
constexpr uint16_t COL_WARN     = 0xFBE0;
constexpr uint16_t COL_BTN      = 0x2124;
constexpr uint16_t COL_BTN_DOWN = 0x4A69;
constexpr uint16_t COL_ACCENT   = 0x04DF;

// ── 배치 (라디오 페이지) ──────────────────────────────────────────
constexpr int16_t HDR_H      = 28;
constexpr int16_t FREQ_Y     = 86;    // 큰 숫자 베이스라인
constexpr int16_t NAME_Y     = 118;
constexpr float   kDialMin   = 88.0f;
constexpr float   kDialMax   = 108.0f;
constexpr int16_t DIAL_L     = 28;
constexpr int16_t DIAL_R     = W - 28;
constexpr int16_t DIAL_Y     = 178;   // 눈금 기준선
constexpr int16_t NEEDLE_TOP = 134;
constexpr int16_t VOL_Y      = 214;   // 슬라이더 트랙 상단
constexpr int16_t VOL_H      = 12;
constexpr int16_t VOL_L      = 72;
constexpr int16_t VOL_R      = W - 24;
constexpr int16_t BTN_Y      = 246;
constexpr int16_t BTN_H      = 62;

struct Rect {
    int16_t x, y, w, h;
    bool contains(int16_t px, int16_t py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

constexpr Rect R_PREV{12, BTN_Y, 96, BTN_H};
constexpr Rect R_PLAY{120, BTN_Y, 96, BTN_H};
constexpr Rect R_NEXT{228, BTN_Y, 96, BTN_H};
constexpr Rect R_MENU{372, BTN_Y, 96, BTN_H};
// 다이얼과 슬라이더는 손가락이 굵으니 넉넉하게 잡는다.
constexpr Rect R_DIAL{0, NEEDLE_TOP - 10, W, (DIAL_Y + 26) - (NEEDLE_TOP - 10)};
constexpr Rect R_VOL{VOL_L - 30, VOL_Y - 12, VOL_R - VOL_L + 60, VOL_H + 24};

// ── 배치 (메뉴 페이지) ────────────────────────────────────────────
constexpr int16_t GRID_X = 4, GRID_Y = 34, CELL_W = 92, CELL_H = 62, GAP = 3, COLS = 5;
constexpr Rect R_BACK{W - 96, 2, 92, 28};
constexpr Rect R_WIFI{8, 244, 148, 62};
constexpr Rect R_SCREENOFF{166, 244, 148, 62};
constexpr Rect R_POWER{324, 244, 148, 62};

// ── 화면 객체 ─────────────────────────────────────────────────────
// 깜빡임 없이 그리려고 PSRAM 캔버스(480x320x2 = 300KB)에 그린 뒤 통째로
// 밀어 넣는다.
//
// 버스는 Arduino_ESP32SPI(레지스터 직접 제어)다. 셋을 다 실물에서 겪었다.
//
//   Arduino_HWSPI      벤더 예제. Arduino SPI 객체 경유. 한 장 2,500ms — 그리기
//                      7ms 에 전송 2,500ms. 청크를 32픽셀에서 4096픽셀로 키워도
//                      그대로라 Arduino SPI HAL 자체가 P4 에서 느리다.
//   Arduino_ESP32SPIDMA IDF spi_master + DMA. 전송 37ms 로 찍히지만 패널에는
//                      아무것도 그려지지 않는다(백라이트만 켜진 검은 화면).
//                      초기화 명령이 패널에 닿지 않는 것으로 보이는데 원인은
//                      못 잡았다. spi_num 규칙도 다르다(호스트 번호 + 1).
//   Arduino_ESP32SPI   한 장 47ms 이고 실제로 그려진다. P4 분기가 명시돼 있다.
//                      spi_num 은 Arduino 식 FSPI(=0)가 SPI2 다.
//
// 값은 상태 로그(화면 N장 그리기/전송)에 찍힌다. config.h 의 LCD_BUS 로 바꾼다.
Arduino_DataBus* bus = nullptr;
Arduino_GFX*     panel = nullptr;
Arduino_Canvas*  gfx = nullptr;

enum class Page : uint8_t { RADIO, MENU };
Page page = Page::RADIO;

// ── 터치 제스처 ───────────────────────────────────────────────────
enum class Gest : uint8_t { IDLE, DIAL, VOL, BTN, SWALLOW };
Gest        gest = Gest::IDLE;
const Rect* gestBtn = nullptr;   // 누르기 시작한 버튼. 같은 버튼에서 떼야 동작
int8_t      gestCell = -1;       // 메뉴 격자에서 누른 칸
bool        touchWas = false;
int16_t     lastX = 0, lastY = 0;

bool    dragging = false;        // 다이얼을 끌고 있는가
float   dragFreq = 0.0f;
bool    volDragging = false;
uint8_t dragVol = 0;
bool    dirty = false;

// ── 백라이트 ─────────────────────────────────────────────────────
uint32_t lastWakeMs = 0;
bool     screenOn = true;
uint8_t  curBright = SCREEN_BRIGHT;

// ── 잠금 / 시간 통계 ──────────────────────────────────────────────
SemaphoreHandle_t uiMutex = nullptr;
uint32_t tDrawSum = 0, tDrawMax = 0, tFlushSum = 0, tFlushMax = 0, tFrames = 0;

// ── 도우미 ────────────────────────────────────────────────────────
int16_t freqToX(float f) {
    if (f < kDialMin) f = kDialMin;
    if (f > kDialMax) f = kDialMax;
    const float t = (f - kDialMin) / (kDialMax - kDialMin);
    return (int16_t)lroundf(DIAL_L + t * (DIAL_R - DIAL_L));
}

float xToFreq(int16_t x) {
    if (x < DIAL_L) x = DIAL_L;
    if (x > DIAL_R) x = DIAL_R;
    return kDialMin + (kDialMax - kDialMin) * (float)(x - DIAL_L) / (float)(DIAL_R - DIAL_L);
}

uint8_t nearestStation(float f) {
    uint8_t best = 0;
    float bestD = 1e9f;
    for (size_t i = 0; i < kStationCount; i++) {
        const float d = fabsf(kStations[i].freq - f);
        if (d < bestD) { bestD = d; best = (uint8_t)i; }
    }
    return best;
}

uint8_t xToVol(int16_t x, uint8_t vmax) {
    if (x < VOL_L) x = VOL_L;
    if (x > VOL_R) x = VOL_R;
    return (uint8_t)lroundf((float)vmax * (float)(x - VOL_L) / (float)(VOL_R - VOL_L));
}

const char* stateText(PlayState s) {
    switch (s) {
        case ST_BOOT:      return "STARTING";
        case ST_WIFI:      return "WIFI...";
        case ST_TUNING:    return "TUNING";
        case ST_BUFFERING: return "BUFFERING";
        case ST_PLAYING:   return "ON AIR";
        case ST_PAUSED:    return "PAUSED";
        case ST_ERROR:     return "ERROR";
        case ST_UPDATING:  return "UPDATING";
        case ST_LOWBATT:   return "LOW BATTERY";
        case ST_WIFISETUP: return "WIFI SETUP";
    }
    return "";
}

uint16_t stateColor(PlayState s) {
    switch (s) {
        case ST_PLAYING:   return COL_OK;
        case ST_BUFFERING:
        case ST_TUNING:
        case ST_WIFI:      return COL_WARN;
        case ST_ERROR:
        case ST_LOWBATT:   return COL_NEEDLE;
        default:           return COL_DIM;
    }
}

String two(uint8_t v) { return (v < 10 ? String("0") : String("")) + String(v); }

// 텍스트 정렬. Arduino_GFX 는 커서 기준 왼쪽·베이스라인 그리기뿐이라
// 폭을 재서 맞춘다. font 가 nullptr 이면 내장 5x7 을 size 배로.
enum Align : uint8_t { AL_LEFT, AL_CENTER, AL_RIGHT };

void text(const String& s, int16_t x, int16_t y, uint16_t color, const GFXfont* font,
          uint8_t size = 1, Align al = AL_LEFT) {
    gfx->setFont(font);
    gfx->setTextSize(size);
    gfx->setTextColor(color);
    int16_t bx, by;
    uint16_t bw, bh;
    gfx->getTextBounds(s.c_str(), 0, 0, &bx, &by, &bw, &bh);
    int16_t cx = x;
    if (al == AL_CENTER) cx = x - (int16_t)bw / 2 - bx;
    else if (al == AL_RIGHT) cx = x - (int16_t)bw - bx;
    else cx = x - bx;
    // 내장 폰트는 커서가 글자 위쪽, GFX 폰트는 베이스라인이다. 위쪽 기준으로 통일.
    const int16_t cy = font ? (y - by) : y;
    gfx->setCursor(cx, cy);
    gfx->print(s);
}

void button(const Rect& r, const String& label, bool pressed, uint16_t ink = COL_INK) {
    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 8, pressed ? COL_BTN_DOWN : COL_BTN);
    gfx->drawRoundRect(r.x, r.y, r.w, r.h, 8, COL_RULE);
    text(label, r.x + r.w / 2, r.y + r.h / 2 - 8, ink, nullptr, 2, AL_CENTER);
}

bool pressedIn(const Rect& r) { return touchWas && gest == Gest::BTN && gestBtn == &r; }

// ── 라디오 페이지 ─────────────────────────────────────────────────
void drawHeader(const UiState& s) {
    text(s.hasTime ? (two(s.hour) + ":" + two(s.minute)) : String("--:--"), 8, 6, COL_INK,
         nullptr, 2);

    String line = stateText(s.state);
    if (s.state == ST_PLAYING && s.bitrate > 0) {
        line += "  " + String(s.bitrate / 1000) + "k";
    } else if (!s.detail.isEmpty()) {
        line += "  " + s.detail;
    }
    text(line, W / 2, 10, stateColor(s.state), nullptr, 1, AL_CENTER);

    // Wi-Fi 막대 — 실제 RSSI
    const int16_t wx = W - 128;
    if (s.wifi) {
        for (int i = 0; i < 4; i++) {
            const int16_t h = 4 + i * 4;
            gfx->fillRect(wx + i * 6, 22 - h, 4, h, i < s.wifiBars ? COL_INK : COL_RULE);
        }
    } else {
        gfx->drawLine(wx, 6, wx + 18, 22, COL_NEEDLE);
        gfx->drawLine(wx + 18, 6, wx, 22, COL_NEEDLE);
    }

    // 배터리. 셀이 없으면 USB 라고만 적는다.
    const int16_t bx = W - 40;
    if (s.battery) {
        gfx->drawRect(bx, 7, 30, 14, COL_DIM);
        gfx->fillRect(bx + 30, 11, 3, 6, COL_DIM);
        const int16_t fill = (int16_t)((int32_t)26 * s.battPercent / 100);
        const uint16_t bc = s.charging ? COL_OK
                            : (s.battPercent <= BATT_CUTOFF_PERCENT ? COL_NEEDLE : COL_INK);
        if (fill > 0) gfx->fillRect(bx + 2, 9, fill, 10, bc);
        text(String(s.battPercent) + "%", bx - 6, 6, COL_DIM, nullptr, 2, AL_RIGHT);
    } else {
        text(s.vbus ? "USB" : "NO BAT", W - 8, 6, COL_DIM, nullptr, 2, AL_RIGHT);
    }
}

void drawDial(const UiState& s) {
    gfx->drawFastHLine(DIAL_L, DIAL_Y, DIAL_R - DIAL_L, COL_RULE);

    for (int f = 88; f <= 108; f++) {
        const int16_t x = freqToX((float)f);
        const bool major = (f % 5) == 0 || f == 88 || f == 108;
        gfx->drawFastVLine(x, DIAL_Y - (major ? 12 : 6), major ? 12 : 6, COL_RULE);
        if (major) text(String(f), x, DIAL_Y + 8, COL_DIM, nullptr, 1, AL_CENTER);
    }

    // 채널 점. 현재 채널은 강조.
    for (size_t i = 0; i < kStationCount; i++) {
        const int16_t x = freqToX(kStations[i].freq);
        gfx->fillCircle(x, DIAL_Y - 20, 3, i == s.index ? COL_ACCENT : COL_DIM);
    }

    // 바늘. 끌고 있으면 손가락을 따라간다.
    const float f = dragging ? dragFreq : s.freq;
    const int16_t nx = freqToX(f);
    gfx->fillRect(nx - 1, NEEDLE_TOP, 3, DIAL_Y - NEEDLE_TOP, COL_NEEDLE);
    gfx->fillTriangle(nx - 7, NEEDLE_TOP, nx + 7, NEEDLE_TOP, nx, NEEDLE_TOP + 12, COL_NEEDLE);

    if (dragging) {
        // 놓으면 어디로 갈지 미리 보여 준다.
        const uint8_t n = nearestStation(dragFreq);
        text(String(kStations[n].freq, 1) + "  " + kStations[n].name, W / 2, NAME_Y,
             COL_ACCENT, nullptr, 2, AL_CENTER);
    }
}

void drawVolume(const UiState& s) {
    text("VOL", 24, VOL_Y - 2, COL_DIM, nullptr, 2);
    gfx->fillRoundRect(VOL_L, VOL_Y, VOL_R - VOL_L, VOL_H, 6, COL_BTN);
    const uint8_t v = volDragging ? dragVol : s.volume;
    const int16_t kx = VOL_L + (int16_t)((int32_t)(VOL_R - VOL_L) * v / (s.volumeMax ? s.volumeMax : 1));
    if (kx > VOL_L) gfx->fillRoundRect(VOL_L, VOL_Y, kx - VOL_L, VOL_H, 6, v ? COL_INK : COL_BTN);
    gfx->fillCircle(kx, VOL_Y + VOL_H / 2, 11, volDragging ? COL_ACCENT : COL_INK);
    gfx->drawCircle(kx, VOL_Y + VOL_H / 2, 11, COL_BG);
}

void renderRadio(const UiState& s) {
    drawHeader(s);

    if (!dragging) {
        char freq[8];
        snprintf(freq, sizeof(freq), "%.1f", s.freq);
        text(freq, W / 2 - 24, FREQ_Y - 34, COL_INK, &FreeSansBold24pt7b, 1, AL_CENTER);
        text("MHz", W / 2 + 60, FREQ_Y - 16, COL_DIM, &FreeSans12pt7b, 1);
        text(s.name.isEmpty() ? String("---") : s.name, W / 2, NAME_Y, COL_INK, nullptr, 2,
             AL_CENTER);
    }

    drawDial(s);
    drawVolume(s);

    button(R_PREV, "<<", pressedIn(R_PREV));
    button(R_PLAY, s.paused ? ">" : "||", pressedIn(R_PLAY), s.paused ? COL_OK : COL_INK);
    button(R_NEXT, ">>", pressedIn(R_NEXT));
    button(R_MENU, "MENU", pressedIn(R_MENU));
}

// ── 메뉴 페이지 ───────────────────────────────────────────────────
void renderMenu(const UiState& s) {
    text("STATIONS", 8, 8, COL_INK, nullptr, 2);
    button(R_BACK, "BACK", pressedIn(R_BACK));

    for (size_t i = 0; i < kStationCount; i++) {
        const int16_t col = (int16_t)(i % COLS), row = (int16_t)(i / COLS);
        const int16_t x = GRID_X + col * (CELL_W + GAP);
        const int16_t y = GRID_Y + row * (CELL_H + GAP);
        const bool cur = (i == s.index);
        const bool down = touchWas && gest == Gest::BTN && gestCell == (int8_t)i;
        gfx->fillRoundRect(x, y, CELL_W, CELL_H, 6, down ? COL_BTN_DOWN : COL_BTN);
        gfx->drawRoundRect(x, y, CELL_W, CELL_H, 6, cur ? COL_ACCENT : COL_RULE);
        text(String(kStations[i].freq, 1), x + CELL_W / 2, y + 10, cur ? COL_ACCENT : COL_INK,
             nullptr, 2, AL_CENTER);
        text(kStations[i].name, x + CELL_W / 2, y + 40, COL_DIM, nullptr, 1, AL_CENTER);
    }

    button(R_WIFI, "WI-FI", pressedIn(R_WIFI));
    button(R_SCREENOFF, "SCREEN OFF", pressedIn(R_SCREENOFF));
    button(R_POWER, "POWER OFF", pressedIn(R_POWER), COL_NEEDLE);
}

}  // namespace

// ── 공개 ──────────────────────────────────────────────────────────
void uiLock()   { if (uiMutex) xSemaphoreTake(uiMutex, portMAX_DELAY); }
void uiUnlock() { if (uiMutex) xSemaphoreGive(uiMutex); }

UiTiming uiTakeTiming() {
    UiTiming t;
    uiLock();
    t.frames = (uint16_t)tFrames;
    if (tFrames) {
        t.drawAvg = (uint16_t)(tDrawSum / tFrames);
        t.flushAvg = (uint16_t)(tFlushSum / tFrames);
    }
    t.drawMax = (uint16_t)tDrawMax;
    t.flushMax = (uint16_t)tFlushMax;
    tDrawSum = tDrawMax = tFlushSum = tFlushMax = tFrames = 0;
    uiUnlock();
    return t;
}

void uiBegin() {
    uiMutex = xSemaphoreCreateMutex();
    hwBacklight(0);
#if LCD_BUS == 1
    bus = new Arduino_ESP32SPIDMA(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_MOSI,
                                  GFX_NOT_DEFINED, 2 /* SPI2_HOST + 1 */, false);
    const char* busName = "ESP32SPIDMA";
#elif LCD_BUS == 2
    // 이쪽은 버스 번호 규칙이 다르다. Arduino 식 FSPI(=0)가 P4 의 SPI2 다.
    bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_MOSI,
                               GFX_NOT_DEFINED, FSPI, false);
    const char* busName = "ESP32SPI";
#else
    bus = new Arduino_HWSPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_MOSI, GFX_NOT_DEFINED,
                            &SPI, false);
    const char* busName = "HWSPI";
#endif
    panel = new Arduino_ST7796(bus, PIN_LCD_RST, LCD_ROTATION, true /* IPS */, 320, 480);
    gfx = new Arduino_Canvas(W, H, panel);
    const bool ok = gfx->begin(LCD_SPI_HZ);
    RLOGI("LCD 초기화 %s (버스 %s, %lu Hz, 회전 %u)", ok ? "OK" : "실패", busName,
          (unsigned long)LCD_SPI_HZ, (unsigned)LCD_ROTATION);

    screenOn = true;
    curBright = SCREEN_BRIGHT;
    lastWakeMs = millis();
    hwBacklight(SCREEN_BRIGHT);

    // 자가 진단. 버스가 패널을 실제로 구동하는지는 읽어 볼 길이 없다(MISO 없음).
    // 빨강·초록·파랑을 차례로 채워서 눈으로 판정한다 — 셋이 보이면 버스와
    // 백라이트가 다 산 것이고, 검으면 버스, 희면 데이터 경로가 문제다.
    const uint16_t bars[] = {0xF800, 0x07E0, 0x001F};
    for (uint16_t c : bars) {
        const uint32_t t0 = millis();
        gfx->fillScreen(c);
        gfx->flush();
        RLOGI("자가 진단 색 0x%04X (%lu ms)", c, (unsigned long)(millis() - t0));
        delay(350);
    }
    gfx->fillScreen(COL_BG);
    gfx->flush();
}

void uiRender(const UiState& s) {
    if (!gfx) return;
    uiLock();
    dirty = false;
    const uint32_t t0 = millis();
    gfx->fillScreen(COL_BG);
    if (page == Page::MENU) renderMenu(s);
    else                    renderRadio(s);
    const uint32_t t1 = millis();
    gfx->flush();
    const uint32_t t2 = millis();

    const uint32_t d = t1 - t0, f = t2 - t1;
    tDrawSum += d;  if (d > tDrawMax) tDrawMax = d;
    tFlushSum += f; if (f > tFlushMax) tFlushMax = f;
    tFrames++;
    uiUnlock();
}

void uiRenderWifiSetup(const UiState& s) {
    if (!gfx) return;
    uiWake();
    gfx->fillScreen(COL_BG);

    text("WI-FI SETUP", 12, 12, COL_NEEDLE, nullptr, 3);
    text("1. Join this Wi-Fi", 12, 60, COL_DIM, nullptr, 2);
    text(s.apSsid, 36, 88, COL_INK, nullptr, 3);
    text("password  " + s.apPass, 36, 120, COL_DIM, nullptr, 2);
    text("2. Open in a browser", 12, 160, COL_DIM, nullptr, 2);
    text(s.apUrl, 36, 188, COL_INK, nullptr, 3);
    text("3. Enter your SSID / password", 12, 228, COL_DIM, nullptr, 2);
    if (s.detail.length()) text("last: " + s.detail, 12, 268, COL_DIM, nullptr, 2);
    text("2.4GHz only - 5 min timeout", 12, 296, COL_DIM, nullptr, 2);

    gfx->flush();
}

bool uiNeedsRedraw() { return dirty; }

UiEvent uiHandleTouch(const TouchPoint* pts, uint8_t n, const UiState& s) {
    UiEvent ev;
    const bool down = n > 0;
    // 아무것도 안 눌려 있고 직전에도 아니었으면 할 일이 없다. 잠금도 잡지 않는다.
    if (!down && !touchWas) return ev;
    uiLock();

    if (down) {
        lastX = pts[0].x;
        lastY = pts[0].y;

        if (!touchWas) {
            // ── 누르기 시작 ──
            if (!screenOn) {
                // 꺼진 화면을 건드린 것은 "켜라"일 뿐이다. 동작으로 치지 않는다.
                uiWake();
                gest = Gest::SWALLOW;
            } else {
                uiWake();
                gest = Gest::IDLE;
                gestBtn = nullptr;
                gestCell = -1;
                if (page == Page::RADIO) {
                    if (R_PREV.contains(lastX, lastY))      { gest = Gest::BTN; gestBtn = &R_PREV; }
                    else if (R_PLAY.contains(lastX, lastY)) { gest = Gest::BTN; gestBtn = &R_PLAY; }
                    else if (R_NEXT.contains(lastX, lastY)) { gest = Gest::BTN; gestBtn = &R_NEXT; }
                    else if (R_MENU.contains(lastX, lastY)) { gest = Gest::BTN; gestBtn = &R_MENU; }
                    else if (R_VOL.contains(lastX, lastY)) {
                        gest = Gest::VOL;
                        volDragging = true;
                        dragVol = xToVol(lastX, s.volumeMax);
                        ev.action = UiAction::VOLUME;
                        ev.value = dragVol;
                    } else if (R_DIAL.contains(lastX, lastY)) {
                        gest = Gest::DIAL;
                        dragging = true;
                        dragFreq = xToFreq(lastX);
                    }
                } else {
                    if (R_BACK.contains(lastX, lastY))           { gest = Gest::BTN; gestBtn = &R_BACK; }
                    else if (R_WIFI.contains(lastX, lastY))      { gest = Gest::BTN; gestBtn = &R_WIFI; }
                    else if (R_SCREENOFF.contains(lastX, lastY)) { gest = Gest::BTN; gestBtn = &R_SCREENOFF; }
                    else if (R_POWER.contains(lastX, lastY))     { gest = Gest::BTN; gestBtn = &R_POWER; }
                    else {
                        for (size_t i = 0; i < kStationCount; i++) {
                            const Rect c{(int16_t)(GRID_X + (i % COLS) * (CELL_W + GAP)),
                                         (int16_t)(GRID_Y + (i / COLS) * (CELL_H + GAP)), CELL_W, CELL_H};
                            if (c.contains(lastX, lastY)) { gest = Gest::BTN; gestCell = (int8_t)i; break; }
                        }
                    }
                }
                dirty = true;
            }
        } else {
            // ── 끌기 ──
            lastWakeMs = millis();
            if (gest == Gest::DIAL) {
                dragFreq = xToFreq(lastX);
                dirty = true;
            } else if (gest == Gest::VOL) {
                const uint8_t v = xToVol(lastX, s.volumeMax);
                if (v != dragVol) {
                    dragVol = v;
                    ev.action = UiAction::VOLUME;   // 끄는 동안 바로바로 반영
                    ev.value = v;
                    dirty = true;
                }
            }
        }
    } else if (touchWas) {
        // ── 뗌 ──
        switch (gest) {
            case Gest::DIAL:
                dragging = false;
                ev.action = UiAction::TUNE;
                ev.value = nearestStation(dragFreq);
                break;
            case Gest::VOL:
                volDragging = false;
                ev.action = UiAction::VOLUME;
                ev.value = dragVol;
                break;
            case Gest::BTN:
                if (page == Page::RADIO && gestBtn && gestBtn->contains(lastX, lastY)) {
                    if (gestBtn == &R_PREV)      ev.action = UiAction::PREV;
                    else if (gestBtn == &R_PLAY) ev.action = UiAction::TOGGLE_PAUSE;
                    else if (gestBtn == &R_NEXT) ev.action = UiAction::NEXT;
                    else if (gestBtn == &R_MENU) page = Page::MENU;
                } else if (page == Page::MENU) {
                    if (gestBtn && gestBtn->contains(lastX, lastY)) {
                        if (gestBtn == &R_BACK)           page = Page::RADIO;
                        else if (gestBtn == &R_WIFI)      { page = Page::RADIO; ev.action = UiAction::WIFI_SETUP; }
                        else if (gestBtn == &R_SCREENOFF) { page = Page::RADIO; ev.action = UiAction::SCREEN_OFF; }
                        else if (gestBtn == &R_POWER)     ev.action = UiAction::POWER_OFF;
                    } else if (gestCell >= 0) {
                        const Rect c{(int16_t)(GRID_X + (gestCell % COLS) * (CELL_W + GAP)),
                                     (int16_t)(GRID_Y + (gestCell / COLS) * (CELL_H + GAP)), CELL_W, CELL_H};
                        if (c.contains(lastX, lastY)) {
                            page = Page::RADIO;
                            ev.action = UiAction::TUNE;
                            ev.value = (uint8_t)gestCell;
                        }
                    }
                }
                break;
            default:
                break;
        }
        gest = Gest::IDLE;
        gestBtn = nullptr;
        gestCell = -1;
        dirty = true;
    }

    touchWas = down;
    uiUnlock();
    return ev;
}

// ── 백라이트 ─────────────────────────────────────────────────────
void uiWake() {
    lastWakeMs = millis();
    screenOn = true;
    if (curBright != SCREEN_BRIGHT) {
        curBright = SCREEN_BRIGHT;
        hwBacklight(curBright);
    }
}

void uiTickBacklight() {
    if (!screenOn) return;
    const uint32_t idle = millis() - lastWakeMs;
    if (idle > SCREEN_OFF_MS) {
        uiScreenOff();
    } else if (idle > SCREEN_DIM_MS && curBright != SCREEN_DIM) {
        curBright = SCREEN_DIM;
        hwBacklight(curBright);
    }
}

bool uiScreenIsOn() { return screenOn; }

void uiScreenOff() {
    curBright = 0;
    hwBacklight(0);
    screenOn = false;
}
