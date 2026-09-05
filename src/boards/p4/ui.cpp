#include "ui.h"

#include <Arduino_GFX_Library.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include "dial_font.h"

#include "config.h"
#include "hw.h"
#include "log.h"
#include "stations.h"

namespace {

constexpr int16_t W = LCD_W;
constexpr int16_t H = LCD_H;

// ── 색 ────────────────────────────────────────────────────────────
// 강조는 바늘과 현재 채널에만. 화면이 커도 색이 많으면 산만하다.
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
}
constexpr uint16_t COL_BG       = rgb(16, 25, 29);
constexpr uint16_t COL_INK      = rgb(240, 235, 220);
constexpr uint16_t COL_DIM      = rgb(159, 178, 180);
constexpr uint16_t COL_RULE     = rgb(47, 66, 71);
constexpr uint16_t COL_NEEDLE   = rgb(174, 75, 48);
constexpr uint16_t COL_OK       = rgb(116, 189, 163);
constexpr uint16_t COL_WARN     = rgb(227, 181, 109);
constexpr uint16_t COL_BTN      = rgb(28, 42, 47);
constexpr uint16_t COL_BTN_DOWN = rgb(50, 68, 71);
constexpr uint16_t COL_ACCENT   = rgb(222, 177, 109);
constexpr uint16_t COL_PAPER    = rgb(235, 229, 212);
constexpr uint16_t COL_PAPER_DIM = rgb(100, 106, 99);
constexpr uint16_t COL_PAPER_RULE = rgb(195, 193, 177);

// ── 배치 (라디오 페이지) ──────────────────────────────────────────
constexpr float   kDialMin   = 88.0f;
constexpr float   kDialMax   = 108.0f;
constexpr int16_t DIAL_L     = 28;
constexpr int16_t DIAL_R     = W - 28;
constexpr int16_t DIAL_Y     = 184;
constexpr int16_t NEEDLE_TOP = 165;
constexpr int16_t VOL_Y      = 229;
constexpr int16_t VOL_H      = 4;
constexpr int16_t VOL_L      = 82;
constexpr int16_t VOL_R      = 414;
constexpr int16_t BTN_Y      = 258;
constexpr int16_t BTN_H      = 52;

struct Rect {
    int16_t x, y, w, h;
    bool contains(int16_t px, int16_t py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

constexpr Rect R_PREV{100, BTN_Y, 66, BTN_H};
constexpr Rect R_PLAY{184, BTN_Y, 104, BTN_H};
constexpr Rect R_NEXT{306, BTN_Y, 66, BTN_H};
constexpr Rect R_MENU{390, BTN_Y, 78, BTN_H};
constexpr Rect R_SOUND{12, BTN_Y, 78, BTN_H};
constexpr Rect R_SLEEP{384, 46, 72, 44};
// 다이얼과 슬라이더는 손가락이 굵으니 넉넉하게 잡는다.
constexpr Rect R_DIAL{12, 160, 456, 48};
constexpr Rect R_VOL{52, 213, 392, 36};

// ── 배치 (메뉴 페이지) ────────────────────────────────────────────
constexpr int16_t GRID_X = 14, GRID_Y = 62, CELL_W = 86, CELL_H = 54, GAP = 6, COLS = 5;
constexpr Rect R_BACK{400, 8, 68, 44};
constexpr Rect R_WIFI{12, 252, 144, 58};
constexpr Rect R_SCREENOFF{168, 252, 144, 58};
constexpr Rect R_POWER{324, 252, 144, 58};
constexpr Rect R_TONES[] = {{12, 56, 108, 46}, {128, 56, 108, 46},
                            {244, 56, 108, 46}, {360, 56, 108, 46}};
constexpr Rect R_SOUND_SLEEP{12, 254, 222, 56};
constexpr Rect R_SOUND_PLAY{246, 254, 222, 56};

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

enum class Page : uint8_t { RADIO, MENU, SOUND };
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

// 가변 길이 문구를 영역 안에 맞춘다.
void fitText(const String& value, int16_t x, int16_t y, int16_t maxWidth,
             uint16_t color, const GFXfont* font, Align align = AL_LEFT) {
    char out[128];
    snprintf(out, sizeof(out), "%s", value.c_str());
    gfx->setFont(font);
    gfx->setTextSize(1);
    int16_t bx, by;
    uint16_t bw, bh;
    size_t n = strlen(out);
    while (n) {
        gfx->getTextBounds(out, 0, 0, &bx, &by, &bw, &bh);
        if (bw <= maxWidth) break;
        out[--n] = '\0';
        if (n >= 3) out[n - 1] = out[n - 2] = out[n - 3] = '.';
    }
    text(out, x, y, color, font, 1, align);
}
void button(const Rect& r, const String& label, bool pressed, uint16_t ink = COL_INK) {
    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 12, pressed ? COL_BTN_DOWN : COL_BTN);
    text(label, r.x + r.w / 2, r.y + r.h / 2 - 7, ink, &FreeSans9pt7b, 1, AL_CENTER);
}
bool pressedIn(const Rect& r) { return touchWas && gest == Gest::BTN && gestBtn == &r; }
void playIcon(int16_t x, int16_t y, bool paused, uint16_t color) {
    if (paused) gfx->fillTriangle(x - 6, y - 10, x - 6, y + 10, x + 10, y, color);
    else {
        gfx->fillRoundRect(x - 9, y - 10, 6, 20, 2, color);
        gfx->fillRoundRect(x + 3, y - 10, 6, 20, 2, color);
    }
}
void skipButton(const Rect& r, bool next) {
    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 18, pressedIn(r) ? COL_BTN_DOWN : COL_BTN);
    const int16_t x = r.x + r.w / 2, y = r.y + r.h / 2, d = next ? 1 : -1;
    gfx->fillTriangle(x - d * 7, y - 8, x - d * 7, y + 8, x + d * 5, y, COL_INK);
    gfx->fillRoundRect(x + d * 9 - 1, y - 8, 3, 16, 1, COL_INK);
}
void drawHeader(const UiState& s) {
    text(s.hasTime ? two(s.hour) + ":" + two(s.minute) : String("--:--"),
         16, 12, COL_INK, &FreeSans9pt7b);
    gfx->fillCircle(106, 20, 3, stateColor(s.state));
    String status = stateText(s.state);
    if (s.state == ST_PLAYING && s.bitrate) status += " / " + String(s.bitrate / 1000) + "k";
    fitText(status, 116, 17, 216, stateColor(s.state), nullptr);
    for (int i = 0; i < 4; ++i) {
        const int h = 3 + i * 3;
        gfx->fillRect(342 + i * 6, 26 - h, 3, h, s.wifi && i < s.wifiBars ? COL_DIM : COL_RULE);
    }
    if (s.battery) {
        gfx->drawRoundRect(440, 14, 24, 12, 2, COL_DIM);
        gfx->fillRect(464, 18, 2, 4, COL_DIM);
        const int fill = 20 * (s.battPercent > 100 ? 100 : s.battPercent) / 100;
        if (fill) gfx->fillRect(442, 16, fill, 8, s.charging ? COL_OK : COL_ACCENT);
        text(String(s.battPercent) + "%", 432, 17, COL_DIM, nullptr, 1, AL_RIGHT);
    } else text(s.vbus ? "USB POWER" : "NO BATTERY", 464, 17, COL_DIM, nullptr, 1, AL_RIGHT);
}
void drawDial(const UiState& s) {
    gfx->drawFastHLine(DIAL_L, DIAL_Y, DIAL_R - DIAL_L, COL_PAPER_RULE);
    for (int half = 176; half <= 216; ++half) {
        const int x = freqToX(half / 2.0f);
        const bool major = half % 10 == 0 || half == 176 || half == 216;
        const int h = major ? 12 : (half % 2 ? 4 : 7);
        gfx->drawFastVLine(x, DIAL_Y - h, h, COL_PAPER_RULE);
        if (major) text(String(half / 2), x, DIAL_Y + 7, COL_PAPER_DIM, nullptr, 1, AL_CENTER);
    }
    for (size_t i = 0; i < kStationCount; ++i)
        gfx->fillCircle(freqToX(kStations[i].freq), 163, 1, COL_PAPER_DIM);
    const int nx = freqToX(dragging ? dragFreq : s.freq);
    gfx->fillRect(nx - 1, NEEDLE_TOP, 2, DIAL_Y - NEEDLE_TOP + 2, COL_NEEDLE);
    gfx->fillTriangle(nx - 4, NEEDLE_TOP, nx + 4, NEEDLE_TOP, nx, NEEDLE_TOP + 5, COL_NEEDLE);
}
void drawVolume(const UiState& s) {
    gfx->fillRect(24, 226, 5, 9, COL_DIM);
    gfx->fillTriangle(28, 226, 35, 220, 35, 241, COL_DIM);
    const uint8_t v = volDragging ? dragVol : s.volume;
    text(String(v), 462, 223, COL_INK, &FreeSans9pt7b, 1, AL_RIGHT);
    gfx->fillRoundRect(VOL_L, VOL_Y, VOL_R - VOL_L, VOL_H, 2, COL_RULE);
    const int kx = VOL_L + int32_t(VOL_R - VOL_L) * v / (s.volumeMax ? s.volumeMax : 1);
    if (kx > VOL_L) gfx->fillRoundRect(VOL_L, VOL_Y, kx - VOL_L, VOL_H, 2, COL_ACCENT);
    gfx->fillCircle(kx, VOL_Y + VOL_H / 2, volDragging ? 9 : 7, COL_ACCENT);
    gfx->fillCircle(kx, VOL_Y + VOL_H / 2, 2, COL_BG);
}
void renderRadio(const UiState& s) {
    drawHeader(s);
    gfx->fillRoundRect(12, 42, 456, 166, 16, COL_PAPER);
    text("F M  /  INTERNET RADIO", 30, 56, COL_PAPER_DIM, nullptr);
    gfx->fillRoundRect(R_SLEEP.x, R_SLEEP.y, R_SLEEP.w, R_SLEEP.h, 12,
                       pressedIn(R_SLEEP) ? COL_PAPER_RULE : COL_PAPER);
    text(s.sleepMinutes ? "SLEEP " + String((s.sleepSeconds + 59) / 60) + "m" : String("SLEEP OFF"),
         451, 58, s.sleepMinutes ? COL_NEEDLE : COL_PAPER_DIM, nullptr, 1, AL_RIGHT);
    const float frequency = dragging ? kStations[nearestStation(dragFreq)].freq : s.freq;
    text(String(frequency, 1), 226, 78, COL_BG, &DialDigits, 1, AL_CENTER);
    text("MHz", 354, 116, COL_PAPER_DIM, &FreeSans9pt7b);
    const String name = dragging ? String(kStations[nearestStation(dragFreq)].name) : s.name;
    const bool detail = !dragging && !s.detail.isEmpty() && s.state != ST_PLAYING && s.state != ST_PAUSED;
    fitText(detail ? s.detail : name, W / 2, 141, 408,
            detail ? COL_NEEDLE : COL_BG, &FreeSans9pt7b, AL_CENTER);
    drawDial(s);
    drawVolume(s);
    skipButton(R_PREV, false);
    skipButton(R_NEXT, true);
    gfx->fillRoundRect(R_PLAY.x, R_PLAY.y, R_PLAY.w, R_PLAY.h, 22,
                       pressedIn(R_PLAY) ? COL_PAPER : COL_ACCENT);
    playIcon(R_PLAY.x + R_PLAY.w / 2, R_PLAY.y + R_PLAY.h / 2, s.paused, COL_BG);
    for (int i = 0; i < 3; ++i) {
        const int x = 43 + i * 8;
        gfx->drawFastVLine(x, 267, 16, pressedIn(R_SOUND) ? COL_INK : COL_DIM);
        gfx->fillCircle(x, 270 + (i == 1 ? 8 : 2), 2, s.tone ? COL_ACCENT : COL_DIM);
    }
    text("SOUND", 51, 296, pressedIn(R_SOUND) ? COL_INK : COL_DIM, nullptr, 1, AL_CENTER);
    for (int i = 0; i < 3; ++i) {
        gfx->fillCircle(419, 270 + i * 5, 1, COL_DIM);
        gfx->drawFastHLine(425, 270 + i * 5, 17, pressedIn(R_MENU) ? COL_INK : COL_DIM);
    }
    text("STATIONS", 429, 296, pressedIn(R_MENU) ? COL_INK : COL_DIM, nullptr, 1, AL_CENTER);
}
void drawMeter(const char* label, int16_t y, uint8_t level, uint8_t peak) {
    text(label, 16, y, COL_DIM, nullptr);
    for (int i = 0; i < 44; ++i)
        gfx->fillRect(34 + i * 10, y, 7, 6, level > i * 255 / 44 ? (i > 37 ? COL_WARN : COL_OK) : COL_RULE);
    if (peak) gfx->fillRect(34 + int32_t(peak) * 437 / 255, y, 2, 6, COL_INK);
}
void renderSound(const UiState& s) {
    text("Sound", 16, 16, COL_INK, &FreeSans12pt7b);
    text(String(s.freq, 1) + " MHz", 154, 23, COL_DIM, nullptr);
    button(R_BACK, "Back", pressedIn(R_BACK));
    for (uint8_t i = 0; i < kTonePresetCount; ++i) {
        const Rect& r = R_TONES[i];
        const bool selected = s.tone == i;
        gfx->fillRoundRect(r.x, r.y, r.w, r.h, 12,
                           pressedIn(r) ? COL_BTN_DOWN : (selected ? COL_PAPER : COL_BTN));
        text(kTonePresets[i].name, r.x + r.w / 2, r.y + 16,
             selected && !pressedIn(r) ? COL_BG : COL_DIM, &FreeSans9pt7b, 1, AL_CENTER);
    }
    const auto& tone = kTonePresets[s.tone < kTonePresetCount ? s.tone : 0];
    text("BASS " + String(tone.bass) + "   MID " + String(tone.mid) + "   TREBLE " + String(tone.treble) + " dB",
         W / 2, 112, COL_DIM, nullptr, 1, AL_CENTER);
    drawMeter("L", 128, s.meters.left, s.meters.peakLeft);
    drawMeter("R", 140, s.meters.right, s.meters.peakRight);
    gfx->fillRoundRect(12, 158, 456, 82, 10, COL_BTN);
    for (int y = 171; y < 222; y += 17) gfx->drawFastHLine(24, y, 432, COL_RULE);
    for (uint8_t i = 0; i < kSpectrumBands; ++i) {
        const int x = 25 + i * 29, h = int32_t(s.meters.bands[i]) * 54 / 255;
        if (h) gfx->fillRoundRect(x, 220 - h, 23, h, 2, COL_ACCENT);
    }
    text("LOW", 24, 228, COL_DIM, nullptr);
    text("INPUT / BEFORE EQ + VOL", W / 2, 228, COL_DIM, nullptr, 1, AL_CENTER);
    text("HIGH", 456, 228, COL_DIM, nullptr, 1, AL_RIGHT);
    String sleep = "Sleep timer off";
    if (s.sleepMinutes) sleep = "Sleep  " + String(s.sleepSeconds / 60) + ":" + two(s.sleepSeconds % 60);
    button(R_SOUND_SLEEP, sleep, pressedIn(R_SOUND_SLEEP), s.sleepMinutes ? COL_ACCENT : COL_INK);
    button(R_SOUND_PLAY, s.paused ? "Resume" : "Pause", pressedIn(R_SOUND_PLAY), COL_ACCENT);
}
void renderMenu(const UiState& s) {
    text("Stations", 16, 16, COL_INK, &FreeSans12pt7b);
    text("15 LIVE CHANNELS", 154, 23, COL_DIM, nullptr);
    button(R_BACK, "Back", pressedIn(R_BACK));
    for (size_t i = 0; i < kStationCount; ++i) {
        const int x = GRID_X + (i % COLS) * (CELL_W + GAP);
        const int y = GRID_Y + (i / COLS) * (CELL_H + GAP);
        const bool cur = i == s.index;
        const bool down = touchWas && gest == Gest::BTN && gestCell == (int8_t)i;
        const uint16_t ink = cur && !down ? COL_BG : COL_INK;
        const uint16_t dim = cur && !down ? COL_PAPER_DIM : COL_DIM;
        gfx->fillRoundRect(x, y, CELL_W, CELL_H, 10, down ? COL_BTN_DOWN : (cur ? COL_PAPER : COL_BTN));
        text(String(kStations[i].freq, 1), x + CELL_W / 2, y + 9, ink, &FreeSansBold12pt7b, 1, AL_CENTER);
        char label[32];
        snprintf(label, sizeof(label), "%s", kStations[i].name);
        const size_t len = strlen(label);
        if (len >= 3 && strcmp(label + len - 3, " FM") == 0) label[len - 3] = '\0';
        fitText(label, x + CELL_W / 2, y + 37, CELL_W - 8, dim, nullptr, AL_CENTER);
        if (cur) gfx->fillCircle(x + CELL_W - 8, y + 8, 2, COL_NEEDLE);
    }
    button(R_WIFI, "Wi-Fi setup", pressedIn(R_WIFI));
    button(R_SCREENOFF, "Screen off", pressedIn(R_SCREENOFF));
    button(R_POWER, "Power off", pressedIn(R_POWER), COL_WARN);
}

}  // namespace

// ── 공개 ──────────────────────────────────────────────────────────
void uiLock()   { if (uiMutex) xSemaphoreTakeRecursive(uiMutex, portMAX_DELAY); }
void uiUnlock() { if (uiMutex) xSemaphoreGiveRecursive(uiMutex); }

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
    uiMutex = xSemaphoreCreateRecursiveMutex();
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
    if (!ok) { delete gfx; gfx = nullptr; return; }

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
    else if (page == Page::SOUND) renderSound(s);
    else                    renderRadio(s);
    const uint32_t t1 = millis();
    // 캔버스 쓰기는 loop 만 한다. 전송 중에는 터치 상태 잠금을 풀어
    // 47ms 가량의 SPI 전송 때문에 짧은 탭을 놓치지 않게 한다.
    uiUnlock();
    gfx->flush();
    const uint32_t t2 = millis();

    const uint32_t d = t1 - t0, f = t2 - t1;
    uiLock();
    tDrawSum += d;  if (d > tDrawMax) tDrawMax = d;
    tFlushSum += f; if (f > tFlushMax) tFlushMax = f;
    tFrames++;
    uiUnlock();
}

void uiRenderWifiSetup(const UiState& s) {
    if (!gfx) return;
    uiLock();
    uiWake();
    gfx->fillScreen(COL_BG);

    text("Connect your radio", 20, 20, COL_INK, &FreeSans12pt7b);
    text("WI-FI SETUP  /  2.4 GHz", 20, 54, COL_ACCENT, nullptr);
    gfx->fillRoundRect(12, 80, 456, 106, 14, COL_PAPER);
    text("01  JOIN THIS NETWORK", 28, 94, COL_PAPER_DIM, nullptr);
    fitText(s.apSsid, 28, 114, 420, COL_BG, &FreeSans12pt7b);
    fitText("Password: " + s.apPass, 28, 154, 420, COL_PAPER_DIM, &FreeSans9pt7b);
    text("02  OPEN IN YOUR BROWSER", 28, 202, COL_DIM, nullptr);
    fitText(s.apUrl, 28, 222, 420, COL_INK, &FreeSans12pt7b);
    text("03  ENTER YOUR HOME WI-FI DETAILS", 28, 266, COL_DIM, nullptr);
    fitText(s.detail.isEmpty() ? String("Setup closes after 5 minutes") : s.detail,
            28, 297, 420, COL_DIM, nullptr);

    gfx->flush();
    uiUnlock();
}

bool uiNeedsRedraw() { uiLock(); const bool value = dirty; uiUnlock(); return value; }
bool uiShowsMeters() { uiLock(); const bool value = page == Page::SOUND; uiUnlock(); return value; }

UiEvent uiHandleTouch(const TouchPoint* pts, uint8_t n, const UiState& s) {
    UiEvent ev;
    const bool down = n > 0;
    // 아무것도 안 눌려 있고 직전에도 아니었으면 할 일이 없다. 잠금도 잡지 않는다.
    if (!down && !touchWas) return ev;
    uiLock();

    if (s.controlsBlocked) {
        gest = Gest::SWALLOW;
        dragging = volDragging = false;
        touchWas = down;
        if (down) uiWake();
        uiUnlock();
        return ev;
    }

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
                    else if (R_SOUND.contains(lastX, lastY)) { gest = Gest::BTN; gestBtn = &R_SOUND; }
                    else if (R_SLEEP.contains(lastX, lastY)) { gest = Gest::BTN; gestBtn = &R_SLEEP; }
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
                } else if (page == Page::SOUND) {
                    if (R_BACK.contains(lastX, lastY)) { gest = Gest::BTN; gestBtn = &R_BACK; }
                    else if (R_SOUND_SLEEP.contains(lastX, lastY)) { gest = Gest::BTN; gestBtn = &R_SOUND_SLEEP; }
                    else if (R_SOUND_PLAY.contains(lastX, lastY)) { gest = Gest::BTN; gestBtn = &R_SOUND_PLAY; }
                    else for (const auto& r : R_TONES) {
                        if (r.contains(lastX, lastY)) { gest = Gest::BTN; gestBtn = &r; break; }
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
                    else if (gestBtn == &R_SOUND) page = Page::SOUND;
                    else if (gestBtn == &R_SLEEP) ev.action = UiAction::SLEEP;
                } else if (page == Page::SOUND && gestBtn && gestBtn->contains(lastX, lastY)) {
                    if (gestBtn == &R_BACK) page = Page::RADIO;
                    else if (gestBtn == &R_SOUND_SLEEP) ev.action = UiAction::SLEEP;
                    else if (gestBtn == &R_SOUND_PLAY) ev.action = UiAction::TOGGLE_PAUSE;
                    else for (uint8_t i = 0; i < kTonePresetCount; ++i) {
                        if (gestBtn == &R_TONES[i]) { ev.action = UiAction::TONE; ev.value = i; break; }
                    }
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
    uiLock();
    lastWakeMs = millis();
    screenOn = true;
    if (curBright != SCREEN_BRIGHT) {
        curBright = SCREEN_BRIGHT;
        hwBacklight(curBright);
    }
    uiUnlock();
}

void uiTickBacklight() {
    uiLock();
    if (!screenOn) { uiUnlock(); return; }
    const uint32_t idle = millis() - lastWakeMs;
    if (idle > SCREEN_OFF_MS) {
        uiScreenOff();
    } else if (idle > SCREEN_DIM_MS && curBright != SCREEN_DIM) {
        curBright = SCREEN_DIM;
        hwBacklight(curBright);
    }
    uiUnlock();
}

bool uiScreenIsOn() { uiLock(); const bool value = screenOn; uiUnlock(); return value; }

void uiScreenOff() {
    uiLock();
    curBright = 0;
    hwBacklight(0);
    screenOn = false;
    uiUnlock();
}
