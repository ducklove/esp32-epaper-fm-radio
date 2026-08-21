#include "ui.h"

#include <M5Unified.h>

#include "config.h"
#include "stations.h"

namespace {

// 눕혀서 쓴다. 240x135 는 고전적인 다이얼 비율에 가깝다.
constexpr int16_t W = 240;
constexpr int16_t H = 135;

constexpr float   kDialMin = 88.0f;
constexpr float   kDialMax = 108.0f;
constexpr int16_t kDialLeft = 10;
constexpr int16_t kDialRight = W - 10;
constexpr int16_t kDialY = 108;

// 전자종이는 흑백뿐이라 형태로만 구분했지만 여기서는 색을 쓸 수 있다.
// 강조는 바늘 하나로 제한한다 — 화면이 작아 색이 많으면 산만하다.
constexpr uint16_t COL_BG     = 0x0000;
constexpr uint16_t COL_INK    = 0xFFFF;
constexpr uint16_t COL_DIM    = 0x8410;
constexpr uint16_t COL_RULE   = 0x39E7;
constexpr uint16_t COL_NEEDLE = 0xFB00;
constexpr uint16_t COL_OK     = 0x2E68;
constexpr uint16_t COL_WARN   = 0xFBE0;

// 깜빡임 없이 그리려고 오프스크린에 그린 뒤 통째로 밀어 넣는다.
M5Canvas* canvas = nullptr;
uint32_t  lastWakeMs = 0;
bool      screenOn = true;

int16_t freqToX(float f) {
    if (f < kDialMin) f = kDialMin;
    if (f > kDialMax) f = kDialMax;
    const float t = (f - kDialMin) / (kDialMax - kDialMin);
    return (int16_t)lroundf(kDialLeft + t * (kDialRight - kDialLeft));
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

void drawStatusBar(const UiState& s) {
    canvas->setTextSize(1);
    canvas->setTextDatum(top_left);
    canvas->setTextColor(COL_INK, COL_BG);
    canvas->drawString(s.hasTime ? (two(s.hour) + ":" + two(s.minute)) : String("--:--"), 6, 6);

    // Wi-Fi 막대 — 실제 RSSI 를 반영한다
    const int16_t wx = 150;
    if (s.wifi) {
        for (int i = 0; i < 3; i++) {
            const int16_t h = 3 + i * 3;
            canvas->fillRect(wx + i * 5, 14 - h, 3, h, i < s.wifiBars ? COL_INK : COL_RULE);
        }
    } else {
        canvas->drawLine(wx, 4, wx + 12, 14, COL_NEEDLE);
        canvas->drawLine(wx + 12, 4, wx, 14, COL_NEEDLE);
    }

    // 배터리
    const int16_t bx = W - 40;
    canvas->drawRect(bx, 4, 26, 11, COL_DIM);
    canvas->fillRect(bx + 26, 7, 3, 5, COL_DIM);
    const int16_t fill = (int16_t)((int32_t)22 * s.battPercent / 100);
    const uint16_t bc = s.charging ? COL_OK
                        : (s.battPercent <= BATT_CUTOFF_PERCENT ? COL_NEEDLE : COL_INK);
    if (fill > 0) canvas->fillRect(bx + 2, 6, fill, 7, bc);

    canvas->setTextDatum(top_right);
    canvas->setTextColor(COL_DIM, COL_BG);
    canvas->drawString(String(s.battPercent) + "%", bx - 4, 6);
}

void drawDial(const UiState& s) {
    canvas->drawFastHLine(kDialLeft, kDialY, kDialRight - kDialLeft, COL_RULE);

    canvas->setTextSize(1);
    for (int f = 88; f <= 108; f++) {
        const int16_t x = freqToX((float)f);
        const bool major = (f % 5) == 0;
        canvas->drawFastVLine(x, kDialY - (major ? 7 : 4), major ? 7 : 4, COL_RULE);
        if (major) {
            canvas->setTextDatum(top_center);
            canvas->setTextColor(COL_DIM, COL_BG);
            canvas->drawString(String(f), x, kDialY + 4);
        }
    }

    for (size_t i = 0; i < kStationCount; i++) {
        canvas->fillRect(freqToX(kStations[i].freq) - 1, kDialY - 11, 2, 2, COL_DIM);
    }

    const int16_t nx = freqToX(s.freq);
    canvas->drawFastVLine(nx, kDialY - 24, 24, COL_NEEDLE);
    canvas->fillTriangle(nx - 4, kDialY - 24, nx + 4, kDialY - 24, nx, kDialY - 17, COL_NEEDLE);
}

}  // namespace

void uiBegin() {
    M5.Display.setRotation(1);  // 눕힌다 -> 240x135
    M5.Display.setBrightness(SCREEN_BRIGHT);
    M5.Display.fillScreen(COL_BG);

    canvas = new M5Canvas(&M5.Display);
    canvas->setPsram(true);
    canvas->createSprite(W, H);

    screenOn = true;
    lastWakeMs = millis();
}

void uiRender(const UiState& s) {
    if (!canvas) return;
    canvas->fillSprite(COL_BG);

    drawStatusBar(s);

    // 주파수 — 화면에서 가장 큰 요소
    char freq[8];
    snprintf(freq, sizeof(freq), "%.1f", s.freq);
    canvas->setTextDatum(middle_center);
    canvas->setTextColor(COL_INK, COL_BG);
    canvas->setFont(&fonts::Font7);
    canvas->drawString(freq, W / 2 - 14, 44);

    canvas->setFont(&fonts::Font0);
    canvas->setTextSize(1);
    canvas->setTextDatum(middle_left);
    canvas->setTextColor(COL_DIM, COL_BG);
    canvas->drawString("MHz", W / 2 + 54, 50);

    canvas->setTextDatum(top_center);
    canvas->setTextColor(COL_INK, COL_BG);
    canvas->drawString(s.name.isEmpty() ? String("---") : s.name, W / 2, 68);

    drawDial(s);

    canvas->setTextDatum(top_left);
    canvas->setTextColor(stateColor(s.state), COL_BG);
    String line = stateText(s.state);
    if (s.state == ST_PLAYING && s.bitrate > 0) {
        line += "  ";
        line += String(s.bitrate / 1000);
        line += "k";
    } else if (!s.detail.isEmpty()) {
        line += "  ";
        line += s.detail;
    }
    canvas->drawString(line, 6, H - 14);

    const int16_t vx = W - 74;
    canvas->drawRect(vx, H - 13, 62, 9, COL_RULE);
    if (s.volumeMax > 0 && s.volume > 0) {
        canvas->fillRect(vx + 2, H - 11, (int16_t)(58 * s.volume / s.volumeMax), 5, COL_INK);
    }

    canvas->pushSprite(0, 0);
}

void uiRenderWifiSetup(const UiState& s) {
    if (!canvas) return;
    uiWake();
    canvas->fillSprite(COL_BG);

    canvas->setFont(&fonts::Font0);
    canvas->setTextSize(1);
    canvas->setTextDatum(top_left);

    canvas->setTextColor(COL_NEEDLE, COL_BG);
    canvas->drawString("WI-FI SETUP", 6, 6);

    canvas->setTextColor(COL_DIM, COL_BG);
    canvas->drawString("1. Join this Wi-Fi", 6, 26);
    canvas->setTextColor(COL_INK, COL_BG);
    canvas->drawString(s.apSsid, 14, 40);
    canvas->setTextColor(COL_DIM, COL_BG);
    canvas->drawString("pw " + s.apPass, 14, 52);

    canvas->drawString("2. Open in browser", 6, 72);
    canvas->setTextColor(COL_INK, COL_BG);
    canvas->drawString(s.apUrl, 14, 86);

    canvas->setTextColor(COL_DIM, COL_BG);
    canvas->drawString("3. Enter SSID / password", 6, 106);
    if (s.detail.length()) canvas->drawString("last: " + s.detail, 6, H - 26);
    canvas->drawString("2.4GHz only - 5 min timeout", 6, H - 14);

    canvas->pushSprite(0, 0);
}

// ── 백라이트 ─────────────────────────────────────────────────────
// 전자종이와 달리 화면이 상시 전력을 먹는다. 조작이 없으면 어두워지고 그 뒤
// 꺼진다. 집어 들거나 버튼을 누르면 uiWake() 로 되살린다.
void uiWake() {
    lastWakeMs = millis();
    if (!screenOn) {
        M5.Display.wakeup();
        screenOn = true;
    }
    M5.Display.setBrightness(SCREEN_BRIGHT);
}

void uiTickBacklight() {
    if (!screenOn) return;
    const uint32_t idle = millis() - lastWakeMs;
    if (idle > SCREEN_OFF_MS) {
        uiScreenOff();
    } else if (idle > SCREEN_DIM_MS) {
        M5.Display.setBrightness(SCREEN_DIM);
    }
}

bool uiScreenIsOn() { return screenOn; }

void uiScreenOff() {
    M5.Display.setBrightness(0);
    M5.Display.sleep();
    screenOn = false;
}
