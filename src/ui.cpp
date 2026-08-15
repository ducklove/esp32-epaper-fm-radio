#include "ui.h"

#include <GxEPD2_BW.h>
#include <SPI.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include "config.h"
#include "stations.h"

// Waveshare 1.54inch e-Paper V2 = SSD1681 200x200.
// (예제의 EPD_Init 이 0x12 SWRESET / 0x01 0xC7,0x00,0x01 / 0x11 / 0x3C / 0x18 / 0x22-0x20 를
//  쓰는 SSD1681 시퀀스 그대로여서 GxEPD2_154_D67 과 일치한다.)
static GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(
    GxEPD2_154_D67(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

namespace {

constexpr int16_t W = 200;
constexpr int16_t H = 200;

// 다이얼 눈금 범위
constexpr float   kDialMin = 88.0f;
constexpr float   kDialMax = 108.0f;
constexpr int16_t kDialLeft = 8;
constexpr int16_t kDialRight = W - 8;
constexpr int16_t kDialY = 143;   // 눈금 기준선
constexpr int16_t kDialTop = 120; // 바늘 꼭대기

uint16_t sinceFullRefresh = 0;

int16_t freqToX(float f) {
    if (f < kDialMin) f = kDialMin;
    if (f > kDialMax) f = kDialMax;
    const float t = (f - kDialMin) / (kDialMax - kDialMin);
    return (int16_t)lroundf(kDialLeft + t * (kDialRight - kDialLeft));
}

void drawCentered(const char* text, int16_t cx, int16_t baselineY) {
    int16_t  x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, baselineY, &x1, &y1, &w, &h);
    display.setCursor(cx - w / 2 - (x1 - 0), baselineY);
    display.print(text);
}

const char* stateText(const UiState& s) {
    switch (s.state) {
        case ST_BOOT:      return "STARTING";
        case ST_WIFI:      return "WIFI...";
        case ST_TUNING:    return "TUNING";
        case ST_BUFFERING: return "BUFFERING";
        case ST_PLAYING:   return "ON AIR";
        case ST_MUTED:     return "MUTED";
        case ST_ERROR:     return "ERROR";
    }
    return "";
}

void drawHeader(const UiState& s) {
    display.fillRect(0, 0, W, 20, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(6, 15);
    display.print("FM RADIO");

    // 오른쪽: Wi-Fi 안테나 아이콘 (막대 3개) — 끊기면 X 로
    const int16_t ax = W - 22;
    if (s.wifi) {
        for (int i = 0; i < 3; i++) {
            const int16_t h = 4 + i * 4;
            display.fillRect(ax + i * 5, 15 - h, 3, h, GxEPD_WHITE);
        }
    } else {
        display.drawLine(ax + 2, 5, ax + 12, 15, GxEPD_WHITE);
        display.drawLine(ax + 12, 5, ax + 2, 15, GxEPD_WHITE);
    }
    display.setTextColor(GxEPD_BLACK);
}

void drawFrequency(const UiState& s) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%.1f", s.freq);

    display.setFont(&FreeSansBold24pt7b);
    int16_t  x1, y1;
    uint16_t w, h;
    display.getTextBounds(buf, 0, 68, &x1, &y1, &w, &h);

    // 숫자 + "MHz" 를 한 덩어리로 보고 가운데 맞춤
    constexpr int16_t kMhzW = 32;
    const int16_t total = (int16_t)w + 4 + kMhzW;
    const int16_t x = (W - total) / 2;

    display.setCursor(x - x1, 68);
    display.print(buf);

    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(x + w + 6, 68);
    display.print("MHz");
}

void drawStationName(const UiState& s) {
    display.setFont(&FreeSansBold9pt7b);
    drawCentered(s.name.isEmpty() ? "---" : s.name.c_str(), W / 2, 92);
}

void drawDial(const UiState& s) {
    display.drawFastHLine(kDialLeft, kDialY, kDialRight - kDialLeft, GxEPD_BLACK);

    // 1MHz 눈금, 5MHz 마다 길게 + 숫자
    display.setFont(nullptr);  // 내장 5x7 폰트
    display.setTextSize(1);
    for (int f = 88; f <= 108; f++) {
        const int16_t x = freqToX((float)f);
        const bool major = (f % 5) == 0;
        display.drawFastVLine(x, kDialY - (major ? 7 : 4), major ? 7 : 4, GxEPD_BLACK);
        if (major) {
            char lab[4];
            snprintf(lab, sizeof(lab), "%d", f);
            const int16_t lw = (int16_t)strlen(lab) * 6;
            int16_t lx = x - lw / 2;
            if (lx < 0) lx = 0;
            if (lx + lw > W) lx = W - lw;
            display.setCursor(lx, kDialY + 4);
            display.print(lab);
        }
    }

    // 프리셋 위치 표시 (눈금선 위 작은 점)
    for (size_t i = 0; i < kStationCount; i++) {
        display.fillRect(freqToX(kStations[i].freq) - 1, kDialY - 11, 2, 2, GxEPD_BLACK);
    }

    // 현재 주파수 바늘
    const int16_t nx = freqToX(s.freq);
    display.drawFastVLine(nx, kDialTop, kDialY - kDialTop, GxEPD_BLACK);
    display.drawFastVLine(nx - 1, kDialTop + 6, kDialY - kDialTop - 6, GxEPD_BLACK);
    display.fillTriangle(nx - 5, kDialTop, nx + 5, kDialTop, nx, kDialTop + 8, GxEPD_BLACK);
}

void drawStatus(const UiState& s) {
    display.setFont(&FreeSans9pt7b);

    String line = stateText(s);
    if (s.state == ST_PLAYING && s.bitrate > 0) {
        line += "  ";
        line += s.bitrate / 1000;
        line += "k";
    } else if (!s.detail.isEmpty()) {
        line += "  ";
        line += s.detail;
    }
    drawCentered(line.c_str(), W / 2, 175);
}

void drawVolume(const UiState& s) {
    constexpr int16_t y = 184;
    constexpr int16_t h = 10;
    constexpr int16_t x0 = 34;
    const int16_t barW = W - x0 - 8;

    display.setFont(nullptr);
    display.setTextSize(1);
    display.setCursor(6, y + 2);
    display.print("VOL");

    display.drawRect(x0, y, barW, h, GxEPD_BLACK);
    if (s.volumeMax > 0 && s.volume > 0) {
        const int16_t fill = (int16_t)((barW - 4) * s.volume / s.volumeMax);
        display.fillRect(x0 + 2, y + 2, fill, h - 4, GxEPD_BLACK);
    }
}

void drawAll(const UiState& s) {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    drawHeader(s);
    drawFrequency(s);
    drawStationName(s);
    drawDial(s);
    drawStatus(s);
    drawVolume(s);
}

}  // namespace

void uiBegin() {
    // GxEPD2 는 전역 SPI 인스턴스를 쓴다. MISO 는 안 쓰므로 -1.
    SPI.end();
    SPI.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, PIN_EPD_CS);

    display.init(115200, true, 2, false);
    display.setRotation(0);
    display.setTextWrap(false);
    sinceFullRefresh = 0xFFFF;  // 첫 렌더는 무조건 전체 갱신
}

void uiRender(const UiState& s, bool forceFull) {
    // 부분 갱신은 빠른 대신 잔상이 쌓인다 — 가끔 전체 갱신으로 털어낸다.
    const bool full = forceFull || sinceFullRefresh >= 12;

    if (full) {
        display.setFullWindow();
        sinceFullRefresh = 0;
    } else {
        display.setPartialWindow(0, 0, W, H);
        sinceFullRefresh++;
    }

    display.firstPage();
    do {
        drawAll(s);
    } while (display.nextPage());
}

void uiSleep() {
    display.hibernate();
}
