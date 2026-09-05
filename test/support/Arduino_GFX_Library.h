// 그래픽/버스 호출만 대체한다. 페이지 전환과 터치 판정은 제품 코드를 실행한다.
#pragma once
#include "Arduino.h"
struct GFXfont {};
constexpr int GFX_NOT_DEFINED = -1, FSPI = 0;
inline void (*testOnFlush)() = nullptr;
class Arduino_DataBus {};
class Arduino_ESP32SPI : public Arduino_DataBus {
public: template<typename... T> Arduino_ESP32SPI(T...) {}
};
class Arduino_GFX {
public:
    virtual ~Arduino_GFX() = default;
    bool begin(uint32_t) { return true; }
    void setFont(const GFXfont*) {}
    void setTextSize(uint8_t) {}
    void setTextColor(uint16_t) {}
    void setCursor(int16_t, int16_t) {}
    void print(const String&) {}
    void getTextBounds(const char* s, int, int, int16_t* x, int16_t* y, uint16_t* w, uint16_t* h) {
        *x = *y = 0; *w = std::strlen(s) * 6; *h = 8;
    }
    template<typename... T> void fillRoundRect(T...) {}
    template<typename... T> void drawRoundRect(T...) {}
    template<typename... T> void fillRect(T...) {}
    template<typename... T> void drawRect(T...) {}
    template<typename... T> void drawLine(T...) {}
    template<typename... T> void drawFastHLine(T...) {}
    template<typename... T> void drawFastVLine(T...) {}
    template<typename... T> void fillCircle(T...) {}
    template<typename... T> void drawCircle(T...) {}
    template<typename... T> void fillTriangle(T...) {}
    void fillScreen(uint16_t) {}
    void flush() { if (testOnFlush) testOnFlush(); }
};
class Arduino_ST7796 : public Arduino_GFX {
public: template<typename... T> Arduino_ST7796(T...) {}
};
class Arduino_Canvas : public Arduino_GFX {
public: template<typename... T> Arduino_Canvas(T...) {}
};
